#pragma once
#include "journal.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <set>
#include <stdexcept>
#include <system_error>

namespace forkfs {
// Immutable sparse radix tree: six 8-bit levels address 64 KiB blocks.
// Payloads and index nodes use the same ObjectStore publication transaction.
class FileContent {
public:
    using Bytes=std::vector<unsigned char>;
    using Id=std::array<unsigned char,32>;
    using Reader=std::function<Bytes(const Id&)>;
    static constexpr uint64_t block_size=65536, inline_limit=256*1024;
    static constexpr uint64_t max_size=INT64_MAX, max_io=8*1024*1024;
    FileContent(Reader reader,Bytes value,uint64_t size,bool indexed):read_(std::move(reader)),size_(size),indexed_(indexed) {
        limit(size);
        if(indexed){require(value.size()==48 && !memcmp(value.data(),"FFFILE01",8) && number(value.data()+8,8)==size,"invalid file descriptor");std::copy_n(value.data()+16,32,root_.begin());require(size || root_==Id{},"nonempty zero-size extent tree");}
        else {require(value.size()==size && size<=inline_limit,"inode content size mismatch");raw_=std::move(value);}
    }
    static void limit(uint64_t size){if(size>max_size)throw std::system_error(EFBIG,std::generic_category(),"file size exceeds signed 64-bit range");}
    static void io_limit(uint64_t size){if(size>max_io)throw std::system_error(E2BIG,std::generic_category(),"file I/O exceeds 8 MiB; use range operations");}
    uint64_t size() const{return size_;}
    bool indexed() const{return indexed_;}
    Bytes read(uint64_t offset,uint64_t count) const {
        if(offset>=size_ || !count)return {};count=std::min(count,size_-offset);io_limit(count);
        if(!indexed_)return Bytes(raw_.begin()+offset,raw_.begin()+offset+count);
        Bytes result(count,0);uint64_t at=0;
        while(at<count){auto pos=offset+at,index=pos/block_size,within=pos%block_size,n=std::min(block_size-within,count-at);auto b=block(index);
            std::copy_n(b.begin()+within,n,result.begin()+at);at+=n;}return result;
    }
    void write(uint64_t offset,const Bytes& value){
        io_limit(value.size());if(value.empty())return;
        if(offset>max_size || value.size()>max_size-offset)limit(UINT64_MAX);
        auto end=offset+value.size();if(!indexed_ && end<=inline_limit){if(end>raw_.size())raw_.resize(end,0);std::copy(value.begin(),value.end(),raw_.begin()+offset);size_=std::max(size_,end);return;}
        promote();uint64_t at=0;
        while(at<value.size()){auto pos=offset+at,index=pos/block_size,within=pos%block_size,n=std::min<uint64_t>(block_size-within,value.size()-at);auto b=block(index);
            std::copy_n(value.begin()+at,n,b.begin()+within);put_block(index,std::move(b));at+=n;}size_=std::max(size_,end);
    }
    void resize(uint64_t size){
        limit(size);if(size==size_)return;
        if(!indexed_ && size<=inline_limit){raw_.resize(size,0);size_=size;return;}
        promote();if(size<size_){auto blocks=(size+block_size-1)/block_size;root_=trim(root_,5,0,blocks);
            if(size%block_size){auto b=block(size/block_size);std::fill(b.begin()+size%block_size,b.end(),0);put_block(size/block_size,std::move(b));}}
        size_=size;
    }
    // Publish only final reachable pending objects, never intermediate radix paths.
    Bytes encode(std::vector<Bytes>& objects) const {
        if(!indexed_)return raw_;
        std::set<Id> seen;collect(root_,5,objects,seen);
        Bytes result(48,0);memcpy(result.data(),"FFFILE01",8);integer(result.data()+8,size_,8);std::copy(root_.begin(),root_.end(),result.begin()+16);return result;
    }
    void verify() const {if(indexed_){std::set<std::pair<Id,unsigned>> verified;verify_node(root_,5,0,verified);}}
private:
    using Node=std::map<unsigned,Id>;
    Reader read_;uint64_t size_;bool indexed_;Bytes raw_;Id root_{};
    std::map<Id,Bytes> pending_;
    static void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
    static uint64_t number(const unsigned char* p,unsigned width){uint64_t n=0;for(unsigned i=0;i<width;++i)n|=uint64_t(p[i])<<(8*i);return n;}
    static void integer(unsigned char* p,uint64_t n,unsigned width){for(unsigned i=0;i<width;++i)p[i]=static_cast<unsigned char>(n>>(8*i));}
    Bytes payload(const Id& id) const{auto it=pending_.find(id);return it==pending_.end()?read_(id):it->second;}
    Id save(Bytes bytes){auto id=Journal::object_id(bytes);pending_.emplace(id,std::move(bytes));return id;}
    Node node(const Id& id,unsigned level) const {
        if(id==Id{})return {};auto b=payload(id);require(b.size()>=11 && !memcmp(b.data(),"FFEXT001",8) && b[8]==level,"invalid extent node");
        auto count=number(b.data()+9,2);require(count && count<=256 && b.size()==11+33*count,"invalid extent node length");Node result;unsigned previous=0;
        for(unsigned i=0;i<count;++i){auto at=11+33*i;unsigned slot=b[at];Id child{};std::copy_n(b.data()+at+1,32,child.begin());require(child!=Id{} && (!i || slot>previous),"invalid extent child order");previous=slot;result.emplace(slot,child);}return result;
    }
    Id save_node(const Node& entries,unsigned level){
        if(entries.empty())return {};Bytes b(11+33*entries.size());memcpy(b.data(),"FFEXT001",8);b[8]=level;integer(b.data()+9,entries.size(),2);size_t at=11;
        for(const auto& [slot,id]:entries){b[at]=slot;std::copy(id.begin(),id.end(),b.begin()+at+1);at+=33;}return save(std::move(b));
    }
    Bytes block(uint64_t index) const {
        Id id=root_;for(int level=5;level>=0;--level){auto entries=node(id,level);auto it=entries.find((index>>(8*level))&255);if(it==entries.end())return Bytes(block_size,0);id=it->second;}
        auto b=payload(id);require(b.size()==block_size,"invalid extent data length");return b;
    }
    Id set(Id id,unsigned level,uint64_t index,const Id& value){
        auto entries=node(id,level);unsigned slot=(index>>(8*level))&255;
        auto child=level?set(entries.contains(slot)?entries.at(slot):Id{},level-1,index,value):value;
        if(child==Id{})entries.erase(slot);else entries[slot]=child;return save_node(entries,level);
    }
    void put_block(uint64_t index,Bytes b){auto id=std::all_of(b.begin(),b.end(),[](auto c){return c==0;})?Id{}:save(std::move(b));root_=set(root_,5,index,id);}
    void promote(){if(indexed_)return;indexed_=true;for(uint64_t at=0;at<raw_.size();at+=block_size){Bytes b(block_size,0);std::copy_n(raw_.begin()+at,std::min<uint64_t>(block_size,raw_.size()-at),b.begin());put_block(at/block_size,std::move(b));}raw_.clear();}
    Id trim(Id id,unsigned level,uint64_t first,uint64_t count){
        if(id==Id{} || first>=count)return {};uint64_t span=uint64_t(1)<<(8*(level+1));if(span<=count-first)return id;
        auto entries=node(id,level);for(auto it=entries.begin();it!=entries.end();){auto start=first+(uint64_t(it->first)<<(8*level));
            auto child=level?trim(it->second,level-1,start,count):(start<count?it->second:Id{});
            if(child==Id{})it=entries.erase(it);else {it->second=child;++it;}}return save_node(entries,level);
    }
    void collect(const Id& id,unsigned level,std::vector<Bytes>& out,std::set<Id>& seen) const {
        if(id==Id{} || !pending_.contains(id) || !seen.insert(id).second)return;
        for(const auto& [slot,child]:node(id,level)){(void)slot;if(level)collect(child,level-1,out,seen);else if(pending_.contains(child) && seen.insert(child).second)out.push_back(pending_.at(child));}out.push_back(pending_.at(id));
    }
    void verify_node(const Id& id,unsigned level,uint64_t first,std::set<std::pair<Id,unsigned>>& verified) const {
        auto span=uint64_t(1)<<(8*(level+1));auto blocks=size_/block_size;bool full=first<=blocks && span<=blocks-first;
        if(full && verified.contains({id,level}))return;
        for(const auto& [slot,child]:node(id,level)){auto start=first+(uint64_t(slot)<<(8*level));require(start<=(size_?((size_-1)/block_size):0) && size_,"extent beyond EOF");
            if(level)verify_node(child,level-1,start,verified);else {auto b=payload(child);require(b.size()==block_size && !std::all_of(b.begin(),b.end(),[](auto c){return c==0;}),"invalid extent data");
                if(start==size_/block_size && size_%block_size)require(std::all_of(b.begin()+size_%block_size,b.end(),[](auto c){return c==0;}),"nonzero extent tail");}}
        if(full)verified.emplace(id,level);
    }
};
}
