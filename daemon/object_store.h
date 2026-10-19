#pragma once
#include "replay_format.h"
#include "repository_budget.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <list>
#include <mutex>
#include <limits>

namespace forkfs {
// Owns object bytes, segment layout, integrity checks and verified payload cache.
// No LevelDB dependency. The coordinator supplies location lookup and publishes
// returned indexes only after sync() succeeds. Writers are externally serialized.
class ObjectStore {
    std::string path_;
    int active_=-1;uint64_t segment_=0,end_=0;bool dirty_=false;
    static constexpr uint64_t segment_limit=64*1024*1024,header_size=48;
    static constexpr size_t chunk_size=64*1024;
    size_t budget_;
    RepositoryBudget capacity_;std::atomic<uint64_t> data_usage_=0;
#ifdef FORKFS_TEST_FAULTS
    size_t test_record_=0;
#endif
    struct Entry {std::string bytes;size_t charge;std::list<std::string>::iterator position;};
    mutable std::mutex cache_mutex_;
    mutable std::list<std::string> lru_;
    mutable std::map<std::string,Entry> cache_;
    mutable uint64_t hits_=0,misses_=0;mutable size_t charge_=0;
    static void need(bool ok,const char* message){replay::require(ok,message);}
    static void fault(const char* stage){
#ifdef FORKFS_TEST_FAULTS
        const char* p=getenv("FORKFS_TEST_CRASH");if(p && !strcmp(p,stage))_exit(86);
#else
        (void)stage;
#endif
    }
    static bool injected(const char* stage){
#ifdef FORKFS_TEST_FAULTS
        const char* p=getenv("FORKFS_TEST_SEGMENT_ERROR");return p && !strcmp(p,stage);
#else
        (void)stage;return false;
#endif
    }
    static std::string number(uint64_t n){return replay::le(n);}
    static uint64_t num(const std::string& s){return replay::num(s);}
    static void sync(int fd){if(fsync(fd)<0)throw std::system_error(errno,std::generic_category(),"sync data segment");}
    static void write_at(int fd,const std::string& s,uint64_t at){
        size_t done=0;while(done<s.size()){auto n=pwrite(fd,s.data()+done,s.size()-done,at+done);if(n<0 && errno==EINTR)continue;if(n<=0)throw std::system_error(n<0?errno:EIO,std::generic_category(),"append data segment");done+=n;}
    }
    static std::string read_at(int fd,size_t n,uint64_t at){
        need(at<=uint64_t(INT64_MAX) && n<=uint64_t(INT64_MAX)-at,"invalid data segment offset");
        std::string s(n,0);size_t done=0;while(done<n){auto rc=pread(fd,s.data()+done,n-done,at+done);if(rc<0 && errno==EINTR)continue;need(rc>0,"truncated or unreadable data segment");done+=rc;}return s;
    }
    struct Fd{int fd;~Fd(){if(fd>=0)close(fd);}};
    std::string filename(uint64_t id) const {return path_+"/DATA-"+std::to_string(id)+".seg";}
    static std::string header(uint64_t id){auto h=std::string("FFSEG001")+number(id);return h+replay::hash(h);}
    struct Tail {uint64_t id=0,end=0;};
    Tail discover_tail() const {
        uint64_t largest=segment_;
        for(const auto& entry:std::filesystem::directory_iterator(path_)){
            auto name=entry.path().filename().string();if(!name.starts_with("DATA-") || !name.ends_with(".seg"))continue;
            auto digits=name.substr(5,name.size()-9);need(!digits.empty() && digits.find_first_not_of("0123456789")==std::string::npos,"invalid data segment name");
            auto id=std::stoull(digits);need(std::to_string(id)==digits,"noncanonical data segment name");largest=std::max(largest,uint64_t(id));
        }
        if(!largest)return {};
        Fd prior{open(filename(largest).c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};
        if(prior.fd<0)throw std::system_error(errno,std::generic_category(),"reopen data segment");
        struct stat st{};need(fstat(prior.fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size>=int64_t(header_size) && uint64_t(st.st_size)<=segment_limit,"invalid data segment size/type");
        need(read_at(prior.fd,header_size,0)==header(largest),"data segment header mismatch");
        return {largest,uint64_t(st.st_size)};
    }
    static uint64_t append_growth(size_t size,uint64_t& end,bool& has_tail){
        need(size<=chunk_size,"invalid chunk size");const uint64_t record=size+44;
        const bool rollover=!has_tail || end>segment_limit || record>segment_limit-end;
        uint64_t cost=record;
        if(rollover){need(cost<=UINT64_MAX-header_size,"segment growth overflow");cost+=header_size;end=header_size;has_tail=true;}
        need(record<=UINT64_MAX-end,"segment growth overflow");end+=record;return cost;
    }
    static uint64_t plan_append_cost(const std::vector<size_t>& payload_sizes,uint64_t end,bool has_tail){
        uint64_t cost=0;
        for(auto size:payload_sizes){auto growth=append_growth(size,end,has_tail);need(growth<=UINT64_MAX-cost,"segment growth overflow");cost+=growth;}
        return cost;
    }
    void open_segment(size_t required){
        if(active_>=0)return;
        // Indexed extents are immutable. An unindexed crash tail can remain as
        // garbage; new records start after EOF, without rescanning file contents.
        auto tail=discover_tail();const auto largest=tail.id;
        if(largest>0 && tail.end<=segment_limit && required<=segment_limit-tail.end){
            Fd prior{open(filename(largest).c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};
            if(prior.fd<0)throw std::system_error(errno,std::generic_category(),"reopen data segment");
            segment_=largest;end_=tail.end;active_=prior.fd;prior.fd=-1;return;
        }
        need(largest<UINT64_MAX,"data segment IDs exhausted");segment_=largest+1;
        active_=open(filename(segment_).c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(active_<0)throw std::system_error(errno,std::generic_category(),"create data segment");
        try{write_at(active_,header(segment_),0);sync(active_);Fd dir{open(path_.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC)};if(dir.fd<0)throw std::system_error(errno,std::generic_category(),"open segment directory");sync(dir.fd);end_=header_size;data_usage_+=header_size;}
        catch(...){close(active_);active_=-1;throw;}
    }
    std::string append(const std::string& hash,const std::string& data){
        need(data.size()<=chunk_size,"invalid chunk size");
        auto record=std::string("FFCHNK01")+hash+replay::le(data.size(),4)+data;
        if(capacity_.enabled())capacity_.admit(path_,data_usage_,append_cost(data.size()));
        if(active_>=0 && (end_>segment_limit || record.size()>segment_limit-end_)){sync(active_);close(active_);active_=-1;}
        open_segment(record.size());auto at=end_;
#ifdef FORKFS_TEST_FAULTS
        // Crash mid-record, without exception unwinding or a metadata write.
        const char* crash=getenv("FORKFS_TEST_CRASH");
        if(crash && !strcmp(crash,"segment_partial_record")){
            const char* ordinal=getenv("FORKFS_TEST_SEGMENT_RECORD");
            if(++test_record_==(ordinal?std::stoull(ordinal):1)){
                const char* count=getenv("FORKFS_TEST_SEGMENT_CUT");auto cut=count?std::stoull(count):record.size()/2;
                need(cut>0 && cut<record.size(),"invalid test record cut");write_at(active_,record.substr(0,cut),at);_exit(86);
            }
        }
#endif
        if(injected("partial_write")){write_at(active_,record.substr(0,record.size()/2),at);throw std::system_error(ENOSPC,std::generic_category(),"injected segment write failure");}
        write_at(active_,record,at);end_+=record.size();data_usage_+=record.size();dirty_=true;fault("segment_after_append");
        return number(segment_)+number(at)+replay::le(data.size(),4);
    }
public:
    using Lookup=std::function<std::optional<std::string>(const std::string&)>;
    using Locations=std::map<std::string,std::string>;
    struct CacheStats{uint64_t hits,misses;size_t charge;};
    ObjectStore(std::string path,size_t cache_bytes):path_(std::move(path)),budget_(cache_bytes),capacity_(RepositoryBudget::load(path_)){if(capacity_.enabled())data_usage_=RepositoryBudget::data_bytes(path_);}
    std::pair<uint64_t,uint64_t> space() const{
        auto host=std::filesystem::space(path_);uint64_t available=host.available,capacity=host.capacity;
        if(capacity_.min_free){auto reserve=capacity_.min_free;available=available>reserve?available-reserve:0;available=available>4*1024*1024?available-4*1024*1024:0;}
        if(capacity_.max_data){auto used=data_usage_.load();capacity=std::min(capacity,capacity_.max_data);available=std::min(available,used<capacity_.max_data?capacity_.max_data-used:0);}
        return {capacity,available};
    }
    bool has_capacity_budget() const{return capacity_.enabled();}
    void admit(uint64_t bytes) const{if(capacity_.enabled())capacity_.admit(path_,data_usage_,bytes);}
    uint64_t append_cost(const std::vector<size_t>& payload_sizes) const {
        if(payload_sizes.empty())return 0;
        if(active_>=0)return plan_append_cost(payload_sizes,end_,true);
        auto tail=discover_tail();return plan_append_cost(payload_sizes,tail.end,tail.id>0);
    }
    uint64_t append_cost(size_t payload_size) const {
        uint64_t end=end_;bool has_tail=active_>=0;
        if(!has_tail){auto tail=discover_tail();end=tail.end;has_tail=tail.id>0;}
        return append_growth(payload_size,end,has_tail);
    }
    static uint64_t new_store_cost(const std::vector<size_t>& payload_sizes){return plan_append_cost(payload_sizes,0,false);}
    ~ObjectStore(){if(active_>=0)close(active_);}
    ObjectStore(const ObjectStore&)=delete;
    std::string read_chunk(const std::string& hash,const std::string& location) const {
        need(hash.size()==32,"invalid chunk identity");replay::Input in{location};auto segment=in.integer(8),offset=in.integer(8),length=in.integer(4);in.end();
        need(segment>0 && offset>=header_size && offset<segment_limit && length<=chunk_size && length+44<=segment_limit-offset,"invalid chunk location");
        Fd fd{open(filename(segment).c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};
        if(fd.fd<0)throw std::system_error(errno,std::generic_category(),"open data segment");
        struct stat st{};need(fstat(fd.fd,&st)==0 && S_ISREG(st.st_mode),"data segment is not a regular file");
        need(read_at(fd.fd,header_size,0)==header(segment),"data segment header mismatch");
        auto record=read_at(fd.fd,44+length,offset);need(record.substr(0,8)=="FFCHNK01" && record.substr(8,32)==hash,"chunk header mismatch");
        auto bytes=record.substr(44);need(record.substr(40,4)==replay::le(length,4) && replay::hash(bytes)==hash,"chunk checksum mismatch");return bytes;
    }
    std::string read(const std::string& id,const std::string& descriptor,const Lookup& lookup) const {
        need(id.size()==32,"invalid object identity");replay::Input in{descriptor};auto magic=in.take(8);need(magic=="FFOBJ001" || magic=="FFOBJ002","invalid object location descriptor");
        auto size=in.integer(8),count=in.integer(4);
        // Empty objects have an actual packed record too, rather than a special
        // inline-payload case in the metadata engine.
        auto expected=std::max<uint64_t>(1,(size+chunk_size-1)/chunk_size);
        need(size<=256*1024 && count==expected && (magic!="FFOBJ001" || size>256),"invalid object descriptor size");
        std::string result;
        for(uint64_t i=0;i<count;++i){auto hash=in.take(32);auto location=lookup(hash);need(location.has_value(),"missing chunk location");
            auto data=read_chunk(hash,*location);need(data.size()==std::min<uint64_t>(chunk_size,size-i*chunk_size),"invalid extent length");result+=data;}
        in.end();need(replay::hash(result)==id,"object checksum mismatch");return result;
    }
    std::string prepare(const std::string& id,const std::string& value,const Lookup& lookup,Locations& pending){
        need(id.size()==32 && value.size()<=256*1024 && replay::hash(value)==id,"invalid object for ObjectStore");
        auto count=std::max<size_t>(1,(value.size()+chunk_size-1)/chunk_size);
        std::string descriptor="FFOBJ002"+number(value.size())+replay::le(count,4);
        for(size_t i=0;i<count;++i){auto data=value.substr(i*chunk_size,chunk_size),hash=replay::hash(data);descriptor+=hash;
            if(pending.contains(hash))continue;auto location=lookup(hash);
            if(location){need(read_chunk(hash,*location)==data,"chunk collision/corruption");continue;}
            pending.emplace(hash,append(hash,data));
        }
        return descriptor;
    }
    void sync(){
        if(!dirty_)return;
        if(injected("sync"))throw std::system_error(EIO,std::generic_category(),"injected segment sync failure");
        sync(active_);dirty_=false;fault("segment_after_sync");
    }
    void abandon() noexcept {if(active_>=0)close(active_);active_=-1;dirty_=false;}
    std::string cached(const std::string& id,const std::function<std::string()>& loader) const {
        {std::lock_guard lock(cache_mutex_);auto found=cache_.find(id);if(found!=cache_.end()){++hits_;lru_.splice(lru_.begin(),lru_,found->second.position);return found->second.bytes;}++misses_;}
        auto value=loader();need(replay::hash(value)==id,"object checksum mismatch");
        auto charge=value.size()+sizeof(Entry)+id.size();
        if(charge<=budget_){std::lock_guard lock(cache_mutex_);if(!cache_.contains(id)){
            while(charge_>budget_-charge){auto last=lru_.back();charge_-=cache_.at(last).charge;cache_.erase(last);lru_.pop_back();}
            lru_.push_front(id);cache_.emplace(id,Entry{value,charge,lru_.begin()});charge_+=charge;
        }}return value;
    }
    CacheStats cache_stats() const {std::lock_guard lock(cache_mutex_);return {hits_,misses_,charge_};}
};
}
