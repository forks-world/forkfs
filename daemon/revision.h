#pragma once
#include "journal.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <set>
#include <stdexcept>
namespace forkfs {
struct RevisionRecord {
    using Id=std::array<unsigned char,32>;using Bytes=std::vector<unsigned char>;
    Id manifest{};uint64_t source_sequence=0;std::vector<Id> parents;
    static constexpr size_t max_parents=64;
    static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
    static uint64_t number(const Bytes& b,size_t at,size_t n){uint64_t v=0;for(size_t i=0;i<n;++i)v|=uint64_t(b.at(at+i))<<(8*i);return v;}
    void validate(const Id& self={}) const {
        require(manifest!=Id{},"zero revision/World manifest");require(parents.size()<=max_parents,"too many revision parents");std::set<Id> seen;
        for(const auto& p:parents){require(p!=Id{} && p!=self,"zero or self revision parent");require(seen.insert(p).second,"duplicate revision parent");}
    }
    static RevisionRecord decode(const Bytes& b,const Id& self={}){
        require(b.size()>=8,"invalid revision descriptor");RevisionRecord r;
        if(!memcmp(b.data(),"FFREV001",8)){
            require(b.size()==80,"invalid revision descriptor length");std::copy_n(b.begin()+8,32,r.manifest.begin());r.source_sequence=number(b,72,8);
            Id parent{};std::copy_n(b.begin()+40,32,parent.begin());if(parent!=Id{})r.parents.push_back(parent);
        }else{
            require(!memcmp(b.data(),"FFREV002",8) && b.size()>=52,"unsupported revision descriptor");
            auto count=number(b,48,4);require(count<=max_parents && b.size()==52+count*32,"invalid revision parent count/length");
            std::copy_n(b.begin()+8,32,r.manifest.begin());r.source_sequence=number(b,40,8);
            for(size_t i=0;i<count;++i){Id parent{};std::copy_n(b.begin()+52+32*i,32,parent.begin());r.parents.push_back(parent);}
        }
        r.validate(self);return r;
    }
    Bytes encode() const {
        validate();Bytes b(52+32*parents.size());memcpy(b.data(),"FFREV002",8);std::copy(manifest.begin(),manifest.end(),b.begin()+8);
        for(unsigned i=0;i<8;++i)b[40+i]=source_sequence>>(8*i);for(unsigned i=0;i<4;++i)b[48+i]=parents.size()>>(8*i);
        for(size_t i=0;i<parents.size();++i)std::copy(parents[i].begin(),parents[i].end(),b.begin()+52+32*i);return b;
    }
    static RevisionRecord read(const std::function<Bytes(const Id&)>& reader,const Id& id){
        auto b=reader(id);require(Journal::object_id(b)==id,"revision object checksum mismatch");try{return decode(b,id);}catch(const std::exception& e){throw std::runtime_error(std::string("invalid replay revision descriptor: ")+e.what());}
    }
};
inline std::vector<std::string> revision_parent_names(const std::string& text){
    if(text=="-")return {};std::vector<std::string> names;size_t at=0;
    for(;;){auto end=text.find(',',at);names.push_back(text.substr(at,end==std::string::npos?end:end-at));
        RevisionRecord::require(names.size()<=RevisionRecord::max_parents,"too many revision parents");if(end==std::string::npos)break;at=end+1;}
    return names;
}
// Reader returns validated descriptors; keeping traversal separate also permits
// synthetic cycle tests without requiring a cryptographic hash fixed point.
class RevisionDag {
public:
    using Id=RevisionRecord::Id;
    struct Limit:std::runtime_error{Limit():std::runtime_error("revision DAG validation incomplete: resource limit") {}};
    RevisionDag(std::function<RevisionRecord(const Id&)> read,std::function<void(const Id&)> manifest,
                size_t max_nodes=100000,size_t max_bytes=64*1024*1024):read_(std::move(read)),manifest_(std::move(manifest)),max_nodes_(max_nodes),max_bytes_(max_bytes){}
    void validate(const Id& root){
        try{
            RevisionRecord::require(root!=Id{},"zero revision reference");
            struct Frame{Id id;RevisionRecord record;size_t next=0;};std::vector<Frame> stack;
            auto enter=[&](const Id& id){
                if(colors_.size()>=max_nodes_)throw Limit();auto record=read_(id);record.validate(id);
                const auto charge=sizeof(Frame)+32*record.parents.size()+64;
                if(charge>max_bytes_-std::min(bytes_,max_bytes_))throw Limit();bytes_+=charge;
                colors_[id]=1;manifest_(record.manifest);stack.push_back({id,std::move(record),0});
            };
            if(colors_.contains(root))return;enter(root);
            while(!stack.empty()){
                auto& frame=stack.back();if(frame.next==frame.record.parents.size()){colors_[frame.id]=2;stack.pop_back();continue;}
                auto parent=frame.record.parents[frame.next++];auto found=colors_.find(parent);
                if(found==colors_.end())enter(parent);else RevisionRecord::require(found->second==2,"revision DAG cycle");
            }
        }catch(...){colors_.clear();bytes_=0;throw;}
    }
private:
    std::function<RevisionRecord(const Id&)> read_;std::function<void(const Id&)> manifest_;
    std::map<Id,unsigned char> colors_;size_t bytes_=0,max_nodes_,max_bytes_;
};
}
