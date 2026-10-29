#include "revision.h"
#include "level_store.h"
#include "object_store.h"
#include "repository_owner.h"
#include "file_content.h"
#include "replay_format.h"
#include "replay_validation.h"
#include "repository_budget.h"
#include "gc_checkpoint.h"
#include "gc_mark.h"
#include "gc_physical_index.h"
#include "metadata_view.h"
#ifndef __APPLE__
#include <sys/syscall.h>
#include <linux/fs.h>
#endif
#include <cstdio>
#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/env.h>
#include <chrono>
#include <algorithm>
#include <leveldb/filter_policy.h>
#include <leveldb/write_batch.h>
#include <cstring>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef FORKFS_TEST_FAULTS
#include <cstdlib>
#endif

namespace forkfs {
#ifdef FORKFS_TEST_LOCKS
// Linked only by the dedicated concurrency test; absent from production builds.
void test_prepared();
void test_verify_snapshot();
void test_compact();
#endif
#ifdef FORKFS_TEST_GC
void test_gc_foreground_demand();
void test_gc_work_boundary(uint64_t);
#endif
struct GcCycle {
    enum Phase {Roots,Leases,Mark,Objects,References,Chunks,Complete};
    leveldb::DB* db;GcMark mark;const leveldb::Snapshot* baseline;const leveldb::Snapshot* references=nullptr;
    uint64_t sequence,deleted=0;Phase phase=Roots;
    std::string root_cursor,object_cursor,reference_cursor,chunk_cursor;
    std::array<unsigned char,32> lease_cursor{};bool lease_started=false;
    GcCycle(leveldb::DB* source,const std::string& path,uint64_t seq,std::shared_ptr<GcMark::MutexCounters> counters):db(source),mark(path,std::move(counters)),baseline(source->GetSnapshot()),sequence(seq){}
    ~GcCycle(){if(references)db->ReleaseSnapshot(references);db->ReleaseSnapshot(baseline);}
    const char* phase_name() const {switch(phase){case Roots:return "roots";case Leases:return "leases";case Mark:return "mark";case Objects:return "objects";case References:return "references";case Chunks:return "chunks";default:return "complete";}}
};
// Restart-discardable cursors, owned by gc_mutex_. No payload/location pins
// survive a call. A sealed source cannot gain new C references.
struct GcPhysical {
    enum Phase {BuildIndex,RefreshActive,ReapMarker,ReapC,Inventory,CandidateC,SourceC,WaitPins,Cleanup};
    Phase phase=BuildIndex;
    leveldb::DB* db;GcPhysicalIndex index;const leveldb::Snapshot* snapshot=nullptr;
    GcPhysical(leveldb::DB* source,const std::string& path):db(source),index(path){}
    ~GcPhysical(){if(snapshot)db->ReleaseSnapshot(snapshot);}
    std::filesystem::directory_iterator directory,end;
    bool directory_started=false,changed=false,reap_ready=false,eligible=false,tail_refreshed=false;
    uint64_t high_water=0,sequence=0,candidate=0,length=0,indexed=0,reap_sid=0,reaped_high_water=0;
    std::string cursor,reap_key;
    const char* phase_name() const {switch(phase){case BuildIndex:return "index";case RefreshActive:return "refresh_active";case ReapMarker:return "reap_marker";case ReapC:return "reap_c";case Inventory:return "inventory";case CandidateC:return "candidate_c";case WaitPins:return "wait_pins";case Cleanup:return "cleanup";default:return "source_c";}}
};
struct StoreIoMetrics {
    struct LockTiming {
        std::atomic<uint64_t> wait_calls=0,wait_ns=0,wait_max_ns=0;
        std::atomic<uint64_t> hold_calls=0,hold_ns=0,hold_max_ns=0;
        std::array<std::atomic<uint64_t>,7> hold_buckets{};
    };
    struct OperationTiming {std::atomic<uint64_t> calls=0,ns=0,max_ns=0;};
    std::shared_ptr<GcMark::MutexCounters> gc_mutex=std::make_shared<GcMark::MutexCounters>();
    LockTiming foreground_writer,gc_writer_sweep,gc_writer_relocate,gc_writer_retire;
    OperationTiming gc_sweep_dbwrite,gc_relocation_chunk_read,gc_relocation_sync,gc_relocation_dbwrite,gc_retirement_dbwrite;
    std::atomic<uint64_t> root_get_calls=0,root_get_ns=0,root_misses=0,root_cache_hits=0,object_get_calls=0,object_get_ns=0,object_misses=0,meta_get_calls=0,meta_get_ns=0;
    std::atomic<uint64_t> transactions=0,prepare_ns=0,write_ns=0,wal_sync_calls=0,wal_sync_ns=0,other_sync_calls=0,other_sync_ns=0,wal_append_bytes=0,wal_append_ns=0,wal_flush_ns=0;
    OperationTiming data_prepare,data_sync,metadata_publish,gc_barrier;
};
namespace {
using Clock=std::chrono::steady_clock;
uint64_t elapsed(Clock::time_point start){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();}
class ForegroundDemand {
    std::atomic<uint64_t>& active_;
public:
    explicit ForegroundDemand(std::atomic<uint64_t>& active):active_(active){active_.fetch_add(1,std::memory_order_relaxed);}
    ~ForegroundDemand(){active_.fetch_sub(1,std::memory_order_relaxed);}
    ForegroundDemand(const ForegroundDemand&)=delete;ForegroundDemand& operator=(const ForegroundDemand&)=delete;
};
void update_max(std::atomic<uint64_t>& target,uint64_t value){auto current=target.load(std::memory_order_relaxed);while(current<value&&!target.compare_exchange_weak(current,value,std::memory_order_relaxed)) {}}
void record_operation(StoreIoMetrics::OperationTiming& metrics,uint64_t duration){++metrics.calls;metrics.ns+=duration;update_max(metrics.max_ns,duration);}
class TimedOperation {
    StoreIoMetrics::OperationTiming& metrics_;Clock::time_point start_=Clock::now();
public:
    explicit TimedOperation(StoreIoMetrics::OperationTiming& metrics):metrics_(metrics){}
    ~TimedOperation(){record_operation(metrics_,elapsed(start_));}
};
class TimedMutexLock {
    std::unique_lock<std::mutex> lock_;StoreIoMetrics::LockTiming& metrics_;Clock::time_point acquired_;
public:
    TimedMutexLock(std::mutex& mutex,StoreIoMetrics::LockTiming& metrics):lock_(mutex,std::defer_lock),metrics_(metrics){
        auto waiting=Clock::now();lock_.lock();acquired_=Clock::now();auto wait_ns=elapsed(waiting);
        ++metrics_.wait_calls;metrics_.wait_ns+=wait_ns;update_max(metrics_.wait_max_ns,wait_ns);
    }
    ~TimedMutexLock(){
        auto hold_ns=elapsed(acquired_);++metrics_.hold_calls;metrics_.hold_ns+=hold_ns;update_max(metrics_.hold_max_ns,hold_ns);
        // Non-cumulative duration bands, then an open-ended tail.
        constexpr std::array<uint64_t,6> limits={1000000,2000000,5000000,10000000,20000000,50000000};
        size_t bucket=0;while(bucket<limits.size()&&hold_ns>limits[bucket])++bucket;
        ++metrics_.hold_buckets[bucket];
        if(lock_.owns_lock())lock_.unlock();
    }
    TimedMutexLock(const TimedMutexLock&)=delete;TimedMutexLock& operator=(const TimedMutexLock&)=delete;
};
std::string timing_json(const StoreIoMetrics::LockTiming& m){
    std::string out="{\"wait_calls\":"+std::to_string(m.wait_calls.load())+",\"wait_ns\":"+std::to_string(m.wait_ns.load())+",\"wait_max_ns\":"+std::to_string(m.wait_max_ns.load())+
        ",\"hold_calls\":"+std::to_string(m.hold_calls.load())+",\"hold_ns\":"+std::to_string(m.hold_ns.load())+",\"hold_max_ns\":"+std::to_string(m.hold_max_ns.load())+
        ",\"hold_buckets\":{\"le_1ms\":"+std::to_string(m.hold_buckets[0].load())+",\"gt_1_le_2ms\":"+std::to_string(m.hold_buckets[1].load())+
        ",\"gt_2_le_5ms\":"+std::to_string(m.hold_buckets[2].load())+",\"gt_5_le_10ms\":"+std::to_string(m.hold_buckets[3].load())+
        ",\"gt_10_le_20ms\":"+std::to_string(m.hold_buckets[4].load())+",\"gt_20_le_50ms\":"+std::to_string(m.hold_buckets[5].load())+
        ",\"gt_50ms\":"+std::to_string(m.hold_buckets[6].load())+"}}";return out;
}
std::string timing_json(const StoreIoMetrics::OperationTiming& m){return "{\"calls\":"+std::to_string(m.calls.load())+",\"ns\":"+std::to_string(m.ns.load())+",\"max_ns\":"+std::to_string(m.max_ns.load())+"}";}
class TimedFile:public leveldb::WritableFile {
    std::unique_ptr<leveldb::WritableFile> file_;StoreIoMetrics& m_;bool wal_;
public:
    TimedFile(std::unique_ptr<leveldb::WritableFile> file,StoreIoMetrics& m,bool wal):file_(std::move(file)),m_(m),wal_(wal){}
    leveldb::Status Append(const leveldb::Slice& data) override {
        auto start=Clock::now();auto s=file_->Append(data);if(wal_){m_.wal_append_ns+=elapsed(start);m_.wal_append_bytes+=data.size();}return s;
    }
    leveldb::Status Close() override{return file_->Close();}
    leveldb::Status Flush() override{auto start=Clock::now();auto s=file_->Flush();if(wal_)m_.wal_flush_ns+=elapsed(start);return s;}
    leveldb::Status Sync() override {
        auto start=Clock::now();auto s=file_->Sync();if(wal_){++m_.wal_sync_calls;m_.wal_sync_ns+=elapsed(start);}
        else {++m_.other_sync_calls;m_.other_sync_ns+=elapsed(start);}return s;
    }
};
class TimedEnv:public leveldb::EnvWrapper {
    StoreIoMetrics& m_;
    leveldb::Status wrap(const std::string& name,leveldb::WritableFile** out,bool append){
        leveldb::WritableFile* raw=nullptr;auto s=append?target()->NewAppendableFile(name,&raw):target()->NewWritableFile(name,&raw);
        if(!s.ok())return s;std::unique_ptr<leveldb::WritableFile> owned(raw);
        *out=new TimedFile(std::move(owned),m_,name.ends_with(".log"));return s;
    }
public:
    explicit TimedEnv(StoreIoMetrics& m):EnvWrapper(leveldb::Env::Default()),m_(m){}
    leveldb::Status NewWritableFile(const std::string& name,leveldb::WritableFile** out) override{return wrap(name,out,false);}
    leveldb::Status NewAppendableFile(const std::string& name,leveldb::WritableFile** out) override{return wrap(name,out,true);}
};
using Bytes=std::vector<unsigned char>;
using Id=std::array<unsigned char,32>;
void need(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
void status(const leveldb::Status& s,const char* operation) {
    if(!s.ok())throw std::runtime_error(std::string(operation)+": "+s.ToString());
}
std::string integer(uint64_t n) {std::string b(8,0);for(unsigned i=0;i<8;++i)b[i]=static_cast<char>(n>>(8*i));return b;}
uint64_t number(const std::string& b) {
    need(b.size()==8,"invalid LevelDB counter");uint64_t n=0;
    for(unsigned i=0;i<8;++i)n|=uint64_t(static_cast<unsigned char>(b[i]))<<(8*i);return n;
}
std::string bytes(const Id& id) {return std::string(reinterpret_cast<const char*>(id.data()),id.size());}
Id id(const std::string& value) {need(value.size()==32,"invalid object reference");Id result{};memcpy(result.data(),value.data(),32);return result;}
void fault(const char* stage) {
#ifdef FORKFS_TEST_FAULTS
    const char* requested=std::getenv("FORKFS_TEST_CRASH");if(requested && !strcmp(requested,stage))_exit(86);
#else
    (void)stage;
#endif
}
}
void LevelStore::create(const std::string& path) {
    if(mkdir(path.c_str(),0700)<0)throw std::system_error(errno,std::generic_category(),"create LevelDB repository");
    {LevelStore store(path,true,default_object_cache_bytes);}
    // Persist the new repository name as well as LevelDB's own file namespace.
    auto parent=std::filesystem::path(path).parent_path();if(parent.empty())parent=".";
    for(const auto& dir:{std::filesystem::path(path),parent}) {
        int fd=open(dir.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if(fd<0)throw std::system_error(errno,std::generic_category(),"open repository directory");
        int rc=fsync(fd),saved=errno;close(fd);if(rc<0)throw std::system_error(saved,std::generic_category(),"sync repository directory");
    }
}
LevelStore::LevelStore(const std::string& path,size_t object_cache_bytes):LevelStore(path,false,object_cache_bytes) {}
LevelStore::LevelStore(const std::string& path,bool initialize,size_t object_cache_bytes) {
    owner_=std::make_shared<RepositoryOwner>(path);path_=path;
    // Scratch is never a recovery authority. A new owner starts a fresh mark;
    // remove links themselves, never traverse their targets.
    for(const auto& entry:std::filesystem::directory_iterator(path))if(entry.path().filename().string().starts_with(".gc-mark-") || entry.path().filename().string().starts_with(".gc-physical-")){
        std::error_code ec;std::filesystem::remove_all(entry.path(),ec);if(ec)throw std::system_error(ec,"discard abandoned GC mark");}

    need(!gc_checkpoint::exists_nofollow(path+"/GC-PENDING"),"GC checkpoint publication is pending; retry gc with its original --checkpoint target");
    leveldb::Options options;options.create_if_missing=initialize;options.error_if_exists=initialize;
    metrics_=std::make_unique<StoreIoMetrics>();env_=std::make_unique<TimedEnv>(*metrics_);options.env=env_.get();
    options.paranoid_checks=true;options.compression=leveldb::kNoCompression;
    cache_.reset(leveldb::NewLRUCache(16*1024*1024));filter_.reset(leveldb::NewBloomFilterPolicy(10));
    object_store_=std::make_unique<ObjectStore>(path,object_cache_bytes);
    options.block_cache=cache_.get();options.filter_policy=filter_.get();
    leveldb::DB* raw=nullptr;status(leveldb::DB::Open(options,path,&raw),"open LevelDB");db_.reset(raw);
    if(!initialize){std::string format;status(db_->Get({},"M/format",&format),"read physical format");
        need(format.size()==8 && format.starts_with("FFLDB00") && format[7]>='1' && format[7]<='8',"unsupported repository format");physical_format_=format[7]-'0';}
    if(initialize) {
        std::string repository(16,0);int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
        if(fd<0)throw std::system_error(errno,std::generic_category(),"repository entropy");
        size_t done=0;
        while(done<repository.size()) {
            auto n=::read(fd,repository.data()+done,repository.size()-done);
            if(n<0 && errno==EINTR)continue;
            if(n<=0){int saved=n<0?errno:EIO;close(fd);throw std::system_error(saved,std::generic_category(),"repository entropy");}done+=n;
        }
        close(fd);replay::Batch batch;batch.Put("M/format","FFLDB001");batch.Put("M/repository",repository);
        batch.Put("M/sequence",integer(0));batch.Put("M/objects",integer(0));batch.Put("M/roots",integer(0));
        publish(batch);
    }
    std::string value;need(read("M/format",value) && (value=="FFLDB001" || value=="FFLDB002"),"unsupported LevelDB repository format");
    const bool replay_format=value=="FFLDB002";
    need(read("M/repository",value) && value.size()==16,"invalid repository identity");
    const char* hex="0123456789abcdef";for(unsigned char c:value){repository_+=hex[c>>4];repository_+=hex[c&15];}
    need(read("M/sequence",value),"missing sequence");sequence_=number(value);
    need(read("M/objects",value),"missing object count");objects_=number(value);
    need(read("M/roots",value),"missing root count");roots_=number(value);
    if(read("M/replay/start",value)) {
        need(replay_format,"replay requires format FFLDB002");
        replay_enabled_=true;replay_start_=number(value);need(replay_start_<=sequence_,"invalid replay start");
        need(read("M/replay/base",replay_base_) && replay_base_.size()==32,"invalid replay base");
        need(read("M/replay/head",replay_head_) && replay_head_.size()==32,"invalid replay head");
        need(replay_hash(sequence_)==replay_head_,"replay head mismatch");
    } else {
        need(!replay_format,"missing replay metadata");
        need(!read("M/replay/base",value) && !read("M/replay/head",value),"incomplete replay metadata");
    }
}
LevelStore::~LevelStore(){gc_physical_.reset();gc_cycle_.reset();}
std::shared_ptr<void> LevelStore::pin_objects() const {
    auto state=pin_state_;{std::lock_guard lock(state->mutex);++state->logical;}
    return std::shared_ptr<void>(state.get(),[state](void*){std::lock_guard lock(state->mutex);--state->logical;});
}
std::shared_ptr<void> LevelStore::pin_manifest(const Id& object_id) const {
    auto temporary=pin_objects();auto raw=bytes(object_id);std::string payload;
    need(read("O/"+raw,payload),"manifest object is not indexed");
    auto state=pin_state_;auto active=std::make_shared<bool>(false);
    auto token=std::shared_ptr<void>(state.get(),[state,object_id,active](void*){
        if(!*active)return;std::lock_guard lock(state->mutex);auto found=state->manifests.find(object_id);
        if(found==state->manifests.end())return;if(--found->second==0)state->manifests.erase(found);
    });
    std::lock_guard writer(writer_mutex_);healthy();
    {std::lock_guard lock(state->mutex);auto found=state->manifests.find(object_id);
        if(found==state->manifests.end()){
            need(state->manifest_epoch!=UINT64_MAX,"manifest pin epoch exhausted");state->manifests.emplace(object_id,1);++state->manifest_epoch;
        }else {need(found->second!=SIZE_MAX,"manifest pin count exhausted");++found->second;}
        *active=true;
    }
    if(gc_cycle_)try{gc_cycle_->mark.seed_manifest(object_id);}catch(const std::exception& error){gc_error_=std::string("GC manifest barrier failed: ")+error.what();gc_cycle_.reset();}
    return token;
}
void LevelStore::LocationRelease::operator()(PinState* state) const noexcept {
    std::lock_guard lock(state->mutex);--state->locations;
}
LevelStore::LocationPin LevelStore::pin_locations() const {
    auto* state=pin_state_.get();{std::lock_guard lock(state->mutex);++state->locations;}
    return LocationPin(state);
}
void LevelStore::cancel_collection(){std::lock_guard gc(gc_mutex_);std::lock_guard writer(writer_mutex_);gc_cycle_.reset();gc_physical_.reset();}
void LevelStore::barrier() const {std::lock_guard lock(writer_mutex_);healthy();}
// The coordinator is the only component that knows both object bytes and DB
// indexes. LevelDB itself is unwrapped and receives metadata-only WriteBatches.
std::string LevelStore::object_payload(const leveldb::ReadOptions& options,const std::string& id) const {
    std::string location;status(db_->Get(options,"O/"+id,&location),"read recovery object");
    return logical_value(options,"O/"+id,location);
}
std::string LevelStore::logical_value(const leveldb::ReadOptions& options,const std::string& key,const std::string& value) const {
    if(key=="M/format"){
        need(value.size()==8 && value.starts_with("FFLDB00") && value[7]>='1' && value[7]<='8',"invalid physical format");
        return ((value[7]-'0')%2)?"FFLDB001":"FFLDB002";
    }
    if(key.starts_with("O/")){
        need(key.size()==34,"invalid object key");std::string result;
        if(physical_format_<=2)result=value;
        else if(physical_format_<=4 && value.starts_with("I")){need(value.size()<=257,"oversized legacy inline object");result=value.substr(1);}
        else {
            if(physical_format_>=5)need(value.starts_with("FFOBJ002"),"object payload is forbidden in metadata index");
            result=object_store_->read(key.substr(2),value,[&](const std::string& hash)->std::optional<std::string>{
                std::string location;auto found=db_->Get(options,"C/"+hash,&location);if(found.IsNotFound())return std::nullopt;status(found,"read chunk location");return location;
            });
        }
        need(replay::hash(result)==key.substr(2),"object checksum mismatch");return result;
    }
    if(key.starts_with("L/") && physical_format_>=3){
        replay::Input in{value};need(in.take(8)=="FFLOGR01","invalid reference log");auto seq=in.integer(8);auto previous=in.take(32);auto count=in.integer(4);need(count>0 && count<=8195,"invalid reference log operation count");
        std::vector<replay::Op> ops;
        for(uint64_t i=0;i<count;++i){auto kind=in.integer(1),ks=in.integer(4),vs=in.integer(4);need(kind<=1 && ks>0 && ks<=1026 && vs<=256*1024,"invalid reference log lengths");replay::Op op{in.take(ks),in.take(vs),kind==0};
            if(op.key.starts_with("O/")){need(!op.erase && op.value.empty(),"reference log contains object payload");op.value=object_payload(options,op.key.substr(2));}ops.push_back(std::move(op));}
        in.end();auto result="T"+integer(seq)+previous+replay::encode(ops);replay::transaction(result);need(key==replay::logkey(seq),"reference log sequence mismatch");return result;
    }
    return value;
}
void LevelStore::publish(const replay::Batch& input){
    // Caller owns the writer lock, or the unpublished recovery destination.
    // Admission failures leave the current owner healthy: no append has begun.
    auto data_prepare_begin=Clock::now();
    if(object_store_->has_capacity_budget()){
        std::vector<size_t> new_chunk_sizes;std::set<std::string> chunks;
        for(const auto& op:input.ops)if(!op.erase && op.key.starts_with("O/")){
            auto count=std::max<size_t>(1,(op.value.size()+65535)/65536);
            for(size_t i=0;i<count;++i){auto payload=op.value.substr(i*65536,65536),hash=replay::hash(payload);std::string existing;
                if(chunks.insert(hash).second){leveldb::ReadOptions read;read.verify_checksums=true;auto found=db_->Get(read,"C/"+hash,&existing);if(found.IsNotFound())new_chunk_sizes.push_back(payload.size());else status(found,"read capacity chunk index");}
            }
        }
        object_store_->admit(object_store_->append_cost(new_chunk_sizes));
    }
    try{
        leveldb::WriteBatch metadata;ObjectStore::Locations pending;auto next_format=physical_format_.load();
        leveldb::ReadOptions read;read.verify_checksums=true;
        auto lookup=[&](const std::string& hash)->std::optional<std::string>{std::string location;auto found=db_->Get(read,"C/"+hash,&location);if(found.IsNotFound())return std::nullopt;status(found,"read chunk index");return location;};
        for(const auto& op:input.ops){
            need(!op.key.starts_with("C/"),"chunk index is coordinator-owned");
            if(op.erase){need(!op.key.starts_with("O/") && !op.key.starts_with("L/"),"immutable data deletion unsupported");metadata.Delete(op.key);continue;}
            if(op.key.starts_with("O/")){
                need(physical_format_>=5,"legacy object layout is read/export-only; migrate with backup/replay");
                auto location=object_store_->prepare(op.key.substr(2),op.value,lookup,pending);metadata.Put(op.key,location);
            }else if(op.key.starts_with("L/")){
                need(physical_format_>=5,"legacy replay writes are disabled");
                auto tx=replay::transaction(op.value);need(op.key==replay::logkey(tx.sequence),"invalid log key");for(auto& item:tx.ops)if(item.key.starts_with("O/"))item.value.clear();
                metadata.Put(op.key,"FFLOGR01"+integer(tx.sequence)+tx.previous+replay::encode(tx.ops));
            }else if(op.key=="M/format"){
                need(op.value=="FFLDB001" || op.value=="FFLDB002","invalid logical format");
                next_format=((physical_format_.load()-1)/2)*2+(op.value=="FFLDB001"?1:2);
                metadata.Put(op.key,"FFLDB00"+std::to_string(next_format));
            }else metadata.Put(op.key,op.value);
        }
        for(const auto& [hash,location]:pending)metadata.Put("C/"+hash,location);
        record_operation(metrics_->data_prepare,elapsed(data_prepare_begin));
        auto data_sync_begin=Clock::now();object_store_->sync();fault("segment_before_metadata");
        record_operation(metrics_->data_sync,elapsed(data_sync_begin));
#ifdef FORKFS_TEST_FAULTS
        const char* injected=std::getenv("FORKFS_TEST_SEGMENT_ERROR");if(injected && !strcmp(injected,"metadata"))throw std::system_error(EIO,std::generic_category(),"injected metadata publication failure");
#endif
        auto metadata_publish_begin=Clock::now();leveldb::WriteOptions options;options.sync=true;status(db_->Write(options,&metadata),"publish metadata indexes");physical_format_=next_format;fault("segment_after_metadata");
        record_operation(metrics_->metadata_publish,elapsed(metadata_publish_begin));
        if(gc_cycle_){
            fault("gc_mark_before_barrier");
            auto gc_barrier_begin=Clock::now();
            try {
#ifdef FORKFS_TEST_FAULTS
                const char* gc_failure=std::getenv("FORKFS_TEST_GC_ERROR");if(gc_failure && !strcmp(gc_failure,"barrier"))throw std::runtime_error("injected GC scratch barrier failure");
#endif
                auto roots=[&](const std::string& key)->std::optional<Id>{std::string ref;auto found=db_->Get(read,key,&ref);if(found.IsNotFound())return std::nullopt;status(found,"GC publication root lookup");return id(ref);};
                // Descriptors contain at most four chunks; bound the per-batch
                // collection to the maximum foreground mutation count times four.
                std::vector<std::string> chunk_hashes;
                for(const auto& op:input.ops)if(!op.erase){
                    if(op.key.starts_with("R/"))gc_cycle_->mark.seed_published_root(op.key,id(op.value),roots);
                    else if(op.key.starts_with("O/")){
                        std::string descriptor;status(db_->Get(read,op.key,&descriptor),"GC published object lookup");
                        replay::Input in{descriptor};need(in.take(8)=="FFOBJ002","invalid GC publication descriptor");(void)in.integer(8);auto count=in.integer(4);need(count>0&&count<=4,"invalid GC publication chunks");
                        need(count<=GcMark::max_protected_chunks-chunk_hashes.size(),"too many GC publication chunks");for(uint64_t i=0;i<count;++i)chunk_hashes.push_back(in.take(32));in.end();
                    }
                }
                gc_cycle_->mark.protect_chunks(chunk_hashes);
            }catch(const std::exception& error){gc_error_=std::string("GC publication barrier failed: ")+error.what();gc_cycle_.reset();}
            record_operation(metrics_->gc_barrier,elapsed(gc_barrier_begin));
            fault("gc_mark_after_barrier");
        }
    }catch(...){object_store_->abandon();poisoned_=true;throw;}
}
void LevelStore::healthy() const {need(!poisoned_,"LevelDB store requires reopen after write failure");}
bool LevelStore::read(const std::string& key,std::string& value) const {
    auto pin=pin_locations();healthy();uint64_t epoch=0;
    if(key.starts_with("R/")) {
        std::lock_guard lock(root_cache_mutex_);epoch=sequence_.load();
        if(root_cache_sequence_!=epoch){root_cache_.clear();root_cache_sequence_=epoch;}
        auto it=root_cache_.find(key);
        if(it!=root_cache_.end()){++metrics_->root_cache_hits;value=it->second.value;return it->second.found;}
    }
    leveldb::ReadOptions options;options.verify_checksums=true;
    auto begin=Clock::now();auto s=db_->Get(options,key,&value);if(s.ok())value=logical_value(options,key,value);auto ns=elapsed(begin);
    if(key.starts_with("R/")){++metrics_->root_get_calls;metrics_->root_get_ns+=ns;if(s.IsNotFound())++metrics_->root_misses;}
    else if(key.starts_with("O/")){++metrics_->object_get_calls;metrics_->object_get_ns+=ns;if(s.IsNotFound())++metrics_->object_misses;}
    else {++metrics_->meta_get_calls;metrics_->meta_get_ns+=ns;}
    if(!s.ok() && !s.IsNotFound())status(s,"read LevelDB");
    if(key.starts_with("R/") && key.size()<=1026 && (!s.ok() || value.size()==32)) {
        std::lock_guard lock(root_cache_mutex_);
        // A read crossing a successful commit may return its snapshot, but must
        // never install it as a fact in the next commit's epoch.
        if(sequence_.load()==epoch && root_cache_sequence_==epoch) {
            if(root_cache_.size()>=4096)root_cache_.clear();
            root_cache_[key]={s.ok(),s.ok()?value:std::string{}};
        }
    }
    return s.ok();
}
Id LevelStore::root(const std::string& key) const {std::string value;need(read("R/"+key,value),"object root not found");return id(value);}
bool LevelStore::contains(const std::string& key) const {std::string value;return read("R/"+key,value);}
Bytes LevelStore::object(const Id& object_id) const {
    auto pin=pin_locations();healthy();auto key=bytes(object_id);
    auto value=object_store_->cached(key,[&]{std::string result;need(read("O/"+key,result),"object not indexed");return result;});
    healthy();return Bytes(value.begin(),value.end());
}
LevelStore::CacheStats LevelStore::cache_stats() const {
    auto stats=object_store_->cache_stats();return {stats.hits,stats.misses,stats.charge};
}
std::vector<std::string> LevelStore::keys(const std::string& prefix) const {
    auto pin=pin_locations();healthy();std::vector<std::string> result;auto search="R/"+prefix;
    leveldb::ReadOptions options;options.verify_checksums=true;
    std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(options));
    for(it->Seek(search);it->Valid();it->Next()) {
        auto key=it->key().ToString();if(!key.starts_with(search))break;result.push_back(key.substr(2));
    }
    status(it->status(),"scan LevelDB roots");return result;
}
void LevelStore::transact(uint64_t expected,const std::vector<Mutation>& changes) {
    // Pin admission may itself wait behind GC: advertise demand before it.
    ForegroundDemand demand(foreground_demand_);auto pin=pin_locations();
#ifdef FORKFS_TEST_GC
    test_gc_foreground_demand();
#endif
    auto begin=Clock::now();healthy();need(physical_format_>=5,"legacy object layout is read/export-only; migrate with backup/replay");need(expected==sequence_,"transaction conflict: sequence changed");
    need(!changes.empty() && changes.size()<=4096,"LevelDB batch requires 1..4096 mutations");
    // Validate and hash caller-owned immutable input before taking the writer
    // lock. No mutable roots/counters are read and no database mutation occurs here.
    std::set<std::string> seen;std::map<Id,const Bytes*> values;
    std::map<std::string,std::optional<Id>> updates;size_t total=0;
    for(const auto& m:changes) {
        need(!m.key.empty() && m.key.size()<=1024 && seen.insert(m.key).second,"invalid or duplicate root key");
        need(!(m.value && m.reference),"mutation has both value and reference");
        if(m.reference){object(*m.reference);updates[m.key]=*m.reference;}
        else if(m.value) {
            need(m.value->size()<=256*1024,"object exceeds 256 KiB");auto hash=Journal::object_id(*m.value);
            if(!values.contains(hash)){total+=m.value->size();values.emplace(hash,&*m.value);}
            need(total<=16*1024*1024,"LevelDB batch exceeds 16 MiB");updates[m.key]=hash;
        } else updates[m.key]=std::nullopt;
    }
#ifdef FORKFS_TEST_LOCKS
    test_prepared();
#endif
    auto prepared_ns=elapsed(begin);
    // Another writer may have committed during preparation. Recheck under the
    // lock before root existence checks, collision validation and publication.
    TimedMutexLock lock(writer_mutex_,metrics_->foreground_writer);healthy();need(expected==sequence_,"transaction conflict: sequence changed");
    need(sequence_!=UINT64_MAX,"transaction sequence exhausted");auto locked_begin=Clock::now();
    uint64_t roots=roots_,objects=objects_;
    for(const auto& [key,hash]:updates) {
        bool exists=contains(key);
        if(hash){if(!exists)++roots;}
        else {need(exists,"cannot delete absent root");--roots;}
    }
    replay::Batch batch;
    for(const auto& [hash,value]:values) {
        std::string previous;
        if(read("O/"+bytes(hash),previous))need(previous.size()==value->size() && (previous.empty() || !memcmp(previous.data(),value->data(),value->size())),"immutable object collision/corruption");
        else {batch.Put("O/"+bytes(hash),std::string(value->begin(),value->end()));++objects;}
    }
    for(const auto& [key,hash]:updates){if(hash)batch.Put("R/"+key,bytes(*hash));else batch.Delete("R/"+key);}
    batch.Put("M/sequence",integer(sequence_+1));batch.Put("M/objects",integer(objects));batch.Put("M/roots",integer(roots));
    std::string next_replay_head;
    if(replay_enabled_) {
        auto record="T"+integer(sequence_+1)+replay_head_+replay::encode(batch.ops);
        next_replay_head=replay::hash(record);
        batch.Put(replay::logkey(sequence_+1),record);batch.Put("M/replay/head",next_replay_head);
    }
    fault("leveldb_before_write");
    metrics_->prepare_ns+=prepared_ns+elapsed(locked_begin);auto write_begin=Clock::now();
    publish(batch);metrics_->write_ns+=elapsed(write_begin);
    if(replay_enabled_)replay_head_=std::move(next_replay_head);
    ++metrics_->transactions;sequence_=sequence_+1;objects_=objects;roots_=roots;fault("leveldb_after_write");
}
std::string LevelStore::metrics_json(bool detailed) const {
    auto& m=*metrics_;std::string result="{\"transactions\":"+std::to_string(m.transactions.load())+
        ",\"prepare_ns\":"+std::to_string(m.prepare_ns.load())+",\"write_ns\":"+std::to_string(m.write_ns.load())+
        ",\"wal_sync_calls\":"+std::to_string(m.wal_sync_calls.load())+",\"wal_sync_ns\":"+std::to_string(m.wal_sync_ns.load())+
        ",\"other_sync_calls\":"+std::to_string(m.other_sync_calls.load())+",\"other_sync_ns\":"+std::to_string(m.other_sync_ns.load())+
        ",\"wal_append_bytes\":"+std::to_string(m.wal_append_bytes.load())+",\"wal_append_ns\":"+std::to_string(m.wal_append_ns.load())+
        ",\"wal_flush_ns\":"+std::to_string(m.wal_flush_ns.load())+
        ",\"data_prepare_calls\":"+std::to_string(m.data_prepare.calls.load())+",\"data_prepare_ns\":"+std::to_string(m.data_prepare.ns.load())+",\"data_prepare_max_ns\":"+std::to_string(m.data_prepare.max_ns.load())+
        ",\"data_sync_calls\":"+std::to_string(m.data_sync.calls.load())+",\"data_sync_ns\":"+std::to_string(m.data_sync.ns.load())+",\"data_sync_max_ns\":"+std::to_string(m.data_sync.max_ns.load())+
        ",\"metadata_publish_calls\":"+std::to_string(m.metadata_publish.calls.load())+",\"metadata_publish_ns\":"+std::to_string(m.metadata_publish.ns.load())+",\"metadata_publish_max_ns\":"+std::to_string(m.metadata_publish.max_ns.load())+
        ",\"gc_barrier_calls\":"+std::to_string(m.gc_barrier.calls.load())+",\"gc_barrier_ns\":"+std::to_string(m.gc_barrier.ns.load())+",\"gc_barrier_max_ns\":"+std::to_string(m.gc_barrier.max_ns.load());
    result+=",\"gc_foreground_wait_ns\":"+std::to_string(m.gc_mutex->foreground_wait_ns.load())+",\"gc_foreground_hold_ns\":"+std::to_string(m.gc_mutex->foreground_hold_ns.load())+
        ",\"gc_advance_wait_ns\":"+std::to_string(m.gc_mutex->advance_wait_ns.load())+",\"gc_advance_hold_ns\":"+std::to_string(m.gc_mutex->advance_hold_ns.load());
    result+=",\"foreground_writer\":"+timing_json(m.foreground_writer)+
        ",\"gc_writer_sweep\":"+timing_json(m.gc_writer_sweep)+
        ",\"gc_writer_relocate\":"+timing_json(m.gc_writer_relocate)+
        ",\"gc_writer_retire\":"+timing_json(m.gc_writer_retire)+
        ",\"gc_sweep_dbwrite\":"+timing_json(m.gc_sweep_dbwrite)+
        ",\"gc_relocation_chunk_read\":"+timing_json(m.gc_relocation_chunk_read)+
        ",\"gc_relocation_sync\":"+timing_json(m.gc_relocation_sync)+
        ",\"gc_relocation_dbwrite\":"+timing_json(m.gc_relocation_dbwrite)+
        ",\"gc_retirement_dbwrite\":"+timing_json(m.gc_retirement_dbwrite);
    if(detailed){auto c=cache_stats();result+=
        ",\"root_get_calls\":"+std::to_string(m.root_get_calls.load())+",\"root_get_ns\":"+std::to_string(m.root_get_ns.load())+
        ",\"root_misses\":"+std::to_string(m.root_misses.load())+",\"root_cache_hits\":"+std::to_string(m.root_cache_hits.load())+",\"object_get_calls\":"+std::to_string(m.object_get_calls.load())+
        ",\"object_get_ns\":"+std::to_string(m.object_get_ns.load())+",\"object_misses\":"+std::to_string(m.object_misses.load())+
        ",\"meta_get_calls\":"+std::to_string(m.meta_get_calls.load())+",\"meta_get_ns\":"+std::to_string(m.meta_get_ns.load())+
        ",\"object_cache_hits\":"+std::to_string(c.hits)+",\"object_cache_misses\":"+std::to_string(c.misses)+",\"object_cache_charge\":"+std::to_string(c.charge);}
    return result+"}";
}
void LevelStore::verify() const {
    auto pin=pin_locations();healthy();
    // The snapshot pins the index and durable counters to one commit. Holding
    // the writer lock for this full scan is unnecessary while objects are
    // retained. RAII releases the snapshot on every error path.
    auto release=[this](const leveldb::Snapshot* snapshot){db_->ReleaseSnapshot(snapshot);};
    std::unique_ptr<const leveldb::Snapshot,decltype(release)> snapshot(db_->GetSnapshot(),release);
    leveldb::ReadOptions options;options.verify_checksums=true;options.snapshot=snapshot.get();
#ifdef FORKFS_TEST_LOCKS
    test_verify_snapshot();
#endif
    auto counter=[&](const char* key){std::string value;status(db_->Get(options,key,&value),"read verification counter");return number(value);};
    const auto expected_roots=counter("M/roots"),expected_objects=counter("M/objects");
    std::string start_value,chain,expected_head;
    auto replay_status=db_->Get(options,"M/replay/start",&start_value);
    bool logged=replay_status.ok();if(!logged && !replay_status.IsNotFound())status(replay_status,"verify replay start");
    uint64_t replay_seq=0;
    if(logged){replay_seq=number(start_value);status(db_->Get(options,"M/replay/base",&chain),"verify replay base");status(db_->Get(options,"M/replay/head",&expected_head),"verify replay head");need(chain.size()==32 && expected_head.size()==32,"invalid replay hashes");}
    std::string format,identity;
    status(db_->Get(options,"M/format",&format),"verify format");status(db_->Get(options,"M/repository",&identity),"verify identity");
    auto raw_format=format;format=logical_value(options,"M/format",format);const unsigned snapshot_physical_format=raw_format[7]-'0';
    need(format==(logged?"FFLDB002":"FFLDB001") && identity.size()==16,"invalid repository format/identity");
    uint64_t roots=0,objects=0;
    std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(options));
    for(it->SeekToFirst();it->Valid();it->Next()) {
        auto key=it->key().ToString(),value=it->value().ToString();
        if(key.starts_with("C/")){need(physical_format_>=3 && key.size()==34,"invalid chunk index key");(void)object_store_->read_chunk(key.substr(2),value);continue;}
        if(key.starts_with("G/")){need(snapshot_physical_format>=7 && key.size()==10 && value=="FFGCRET1","invalid retired segment marker");auto sid=number(key.substr(2));need(sid>0 && integer(sid)==key.substr(2),"invalid retired segment ID");continue;}
        value=logical_value(options,key,value);
        if(key.starts_with("L/")) {
            need(logged && replay_seq!=UINT64_MAX && key==replay::logkey(replay_seq+1),"replay log gap or unexpected record");
            auto tx=replay::transaction(value);need(tx.sequence==++replay_seq && tx.previous==chain,"replay log chain mismatch");chain=replay::hash(value);
        } else if(key.starts_with("O/")){need(key.size()==34,"invalid object key");need(Journal::object_id(Bytes(value.begin(),value.end()))==id(key.substr(2)),"object checksum mismatch");++objects;}
        else if(key.starts_with("R/")) {
            auto reference=id(value);std::string object;
            try{object=object_payload(options,bytes(reference));}catch(const std::exception& e){throw std::runtime_error(std::string("verify root reference: ")+e.what());}
            need(Journal::object_id(Bytes(object.begin(),object.end()))==reference,"root object checksum mismatch");++roots;
        } else need(key=="M/format" || key=="M/repository" || key=="M/sequence" || key=="M/objects" || key=="M/roots" || (logged && (key=="M/replay/start" || key=="M/replay/base" || key=="M/replay/head")),"unknown metadata key");
    }
    status(it->status(),"verify LevelDB");
    need(roots==expected_roots && objects==expected_objects,"LevelDB count mismatch");
    if(logged)need(replay_seq==counter("M/sequence") && chain==expected_head,"incomplete replay log");healthy();
}
void LevelStore::compact() {
    healthy();
#ifdef FORKFS_TEST_LOCKS
    test_compact();
#endif
    // LevelDB synchronizes manual compaction internally; it need not own the
    // metadata writer lock. Post-compaction verification uses its own snapshot.
    db_->CompactRange(nullptr,nullptr);verify();
}
}

namespace forkfs {
namespace {
std::string recovery_unhex(const std::string& value,size_t size){
        need(value.size()==size*2,"invalid recovery point ID length");std::string raw;
        auto digit=[](char c)->unsigned {if(c>='0' && c<='9')return c-'0';if(c>='a' && c<='f')return c-'a'+10;
            if(c>='A' && c<='F')return c-'A'+10;throw std::runtime_error("invalid recovery point hex ID");};
        for(size_t i=0;i<value.size();i+=2)raw.push_back(static_cast<char>((digit(value[i])<<4)|digit(value[i+1])));return raw;
}
std::string recovery_hex(const std::string& raw){
    constexpr char digits[]="0123456789abcdef";std::string out;
    for(unsigned char c:raw){out+=digits[c>>4];out+=digits[c&15];}return out;
}
void sync_parent(const std::string& path){
    auto parent=std::filesystem::path(path).parent_path();if(parent.empty())parent=".";
    int fd=open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)throw std::system_error(errno,std::generic_category(),"open replay parent");
    int rc=fsync(fd),saved=errno;close(fd);if(rc<0)throw std::system_error(saved,std::generic_category(),"sync replay parent");
}
void publish_directory(const std::string& from,const std::string& to){
#ifdef __APPLE__
    int rc=renamex_np(from.c_str(),to.c_str(),RENAME_EXCL);
#else
    int rc=static_cast<int>(syscall(SYS_renameat2,AT_FDCWD,from.c_str(),AT_FDCWD,to.c_str(),RENAME_NOREPLACE));
#endif
    if(rc<0)throw std::system_error(errno,std::generic_category(),"publish recovered repository (destination must not exist)");
    sync_parent(to);
}
}
void LevelStore::verify_recovery_graph(const leveldb::ReadOptions& options,uint64_t sequence) const {
    replay::Graph graph([&](const Id& object_id){
        auto value=object_payload(options,bytes(object_id));
        Bytes b(value.begin(),value.end());need(Journal::object_id(b)==object_id,"recovery object checksum mismatch");return b;
    },sequence);
    std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(options));
    for(it->Seek("R/");it->Valid() && it->key().starts_with("R/");it->Next())graph.root(it->key().ToString(),it->value().ToString());
    status(it->status(),"scan recovery roots");
}
std::string LevelStore::replay_hash(uint64_t seq) const {
    need(replay_enabled_ && seq>=replay_start_ && seq<=sequence_,"sequence outside retained replay range");
    if(seq==replay_start_)return replay_base_;
    std::string record;need(read(replay::logkey(seq),record),"missing retained replay transaction");
    auto tx=replay::transaction(record);need(tx.sequence==seq,"invalid retained replay sequence");return replay::hash(record);
}
uint64_t LevelStore::backup(const std::string& archive,std::string* chain_hash){
    auto pin=pin_locations();
    std::unique_lock lock(writer_mutex_);healthy();
    if(!replay_enabled_){
        std::string entropy(32,0);int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);if(fd<0)throw std::system_error(errno,std::generic_category(),"replay anchor entropy");
        size_t done=0;while(done<entropy.size()){auto n=::read(fd,entropy.data()+done,entropy.size()-done);if(n<0 && errno==EINTR)continue;if(n<=0){close(fd);throw std::runtime_error("replay anchor entropy failed");}done+=n;}close(fd);
        auto anchor=replay::hash("forkfs.replay.anchor.v1"+repository_+integer(sequence_)+entropy);
        replay::Batch batch;batch.Put("M/format","FFLDB002");batch.Put("M/replay/start",integer(sequence_));batch.Put("M/replay/base",anchor);batch.Put("M/replay/head",anchor);
        fault("replay_enable_before_write");publish(batch);fault("replay_enable_after_write");
        replay_enabled_=true;replay_start_=sequence_;replay_base_=anchor;replay_head_=anchor;
    }
    auto release=[this](const leveldb::Snapshot* s){db_->ReleaseSnapshot(s);};
    std::unique_ptr<const leveldb::Snapshot,decltype(release)> snapshot(db_->GetSnapshot(),release);
    leveldb::ReadOptions options;options.verify_checksums=true;options.snapshot=snapshot.get();
    std::string identity;status(db_->Get(options,"M/repository",&identity),"backup identity");
    replay::Header header{'B',identity,sequence_,sequence_,replay_head_,replay_head_};lock.unlock();
    verify_recovery_graph(options,header.to);
#ifdef FORKFS_TEST_BACKUP
    extern void test_backup_snapshot();test_backup_snapshot();
#endif
    replay::Writer out(archive,header);std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(options));
    for(it->SeekToFirst();it->Valid();it->Next()){
        auto key=it->key().ToString();
        if(key.starts_with("C/") || key.starts_with("G/") || key.starts_with("L/") || key.starts_with("M/replay/"))continue;
        auto value=logical_value(options,key,it->value().ToString());
        auto row="B"+replay::le(key.size(),4)+replay::le(value.size(),4)+key+value;
        replay::base_row(row);out.frame(row);
    }
    status(it->status(),"export backup");out.finish();sync_parent(archive);if(chain_hash)*chain_hash=header.head;return header.to;
}
uint64_t LevelStore::export_replay(uint64_t after,const std::string& archive,std::string* chain_hash) const {
    auto pin=pin_locations();
    std::unique_lock lock(writer_mutex_);healthy();auto previous=replay_hash(after);
    auto release=[this](const leveldb::Snapshot* s){db_->ReleaseSnapshot(s);};
    std::unique_ptr<const leveldb::Snapshot,decltype(release)> snapshot(db_->GetSnapshot(),release);
    leveldb::ReadOptions options;options.verify_checksums=true;options.snapshot=snapshot.get();
    std::string identity;status(db_->Get(options,"M/repository",&identity),"export replay identity");
    replay::Header header{'I',identity,after,sequence_,previous,replay_head_};lock.unlock();verify_recovery_graph(options,header.to);replay::Writer out(archive,header);
    uint64_t seq=after;auto head=previous;
    while(seq<header.to){std::string record;status(db_->Get(options,replay::logkey(++seq),&record),"export replay record");record=logical_value(options,replay::logkey(seq),record);
        auto tx=replay::transaction(record);need(tx.sequence==seq && tx.previous==head,"retained replay chain mismatch");out.frame(record);head=replay::hash(record);
    }
    need(head==header.head,"retained replay end mismatch");out.finish();sync_parent(archive);if(chain_hash)*chain_hash=header.head;return header.to;
}
std::vector<LevelStore::RestoreStep> LevelStore::plan_restore(const std::vector<std::string>& archives,const RecoveryPoint& target){
    need(!archives.empty() && archives.size()<=4096,"restore requires 1 to 4096 archives");
    const auto repository=recovery_unhex(target.repository,16),target_hash=recovery_unhex(target.chain_hash,32);
    using Point=std::pair<uint64_t,std::string>;
    struct Candidate {std::string path;replay::Header header;Point end;};
    std::vector<Candidate> bases,deltas;std::set<std::string> paths;
    for(const auto& input:archives){
        auto path=std::filesystem::absolute(input).lexically_normal().string();if(!paths.insert(path).second)continue;
        replay::File file(path);auto header=replay::validate(file);
        need(header.repository==repository,"restore archive repository mismatch");
        if(header.kind=='B'){
            if(header.to<=target.sequence)bases.push_back({path,header,{header.to,header.head}});
        } else if(header.from<target.sequence && header.from<header.to){
            Point end{header.to,header.head};
            if(header.to>target.sequence){
                replay::Reader reader(file);
                while(!reader.ended){auto payload=reader.next();if(reader.ended)break;
                    auto tx=replay::transaction(payload);
                    if(tx.sequence==target.sequence){end={tx.sequence,replay::hash(payload)};break;}
                }
            }
            deltas.push_back({path,header,end});
        }
    }
    // Strictly increasing sequence numbers form a DAG. Work backwards from the
    // requested hash, retaining only edges that can actually reach it.
    std::sort(deltas.begin(),deltas.end(),[](const auto& a,const auto& b){
        if(a.header.from!=b.header.from)return a.header.from>b.header.from;
        if(a.end.first!=b.end.first)return a.end.first>b.end.first;return a.path<b.path;
    });
    const Point goal{target.sequence,target_hash};std::map<Point,size_t> next;next.emplace(goal,deltas.size());
    for(size_t i=0;i<deltas.size();++i){const auto& d=deltas[i];
        if(next.contains(d.end))next.emplace(Point{d.header.from,d.header.previous},i);
    }
    std::sort(bases.begin(),bases.end(),[](const auto& a,const auto& b){
        if(a.end.first!=b.end.first)return a.end.first>b.end.first;return a.path<b.path;
    });
    auto base=std::find_if(bases.begin(),bases.end(),[&](const auto& b){return next.contains(b.end);});
    need(base!=bases.end(),"no continuous archive chain reaches recovery point");
    auto receipt=[&](const Point& p){return RecoveryPoint{p.first,recovery_hex(repository),recovery_hex(p.second)};};
    std::vector<RestoreStep> result{{base->path,receipt(base->end)}};auto current=base->end;
    while(current!=goal){const auto& delta=deltas.at(next.at(current));result.push_back({delta.path,receipt(delta.end)});current=delta.end;}
    return result;
}
uint64_t LevelStore::restore(const std::vector<std::string>& archives,const std::string& destination,const RecoveryPoint& target,const std::function<void(const std::string&)>& verify_namespace){
    need(bool(verify_namespace),"restore requires namespace validation");
    auto plan=plan_restore(archives,target);struct stat st{};
    need(lstat(destination.c_str(),&st)!=0 && errno==ENOENT,"restore destination must not exist");
    auto staging=destination+".restore-job-XXXXXX";
    if(!mkdtemp(staging.data()))throw std::system_error(errno,std::generic_category(),"create restore job staging");
    struct Cleanup {std::string path;~Cleanup(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}} cleanup{staging};
    const auto repository=staging+"/repository";
    for(const auto& step:plan){replay_archive(step.archive,repository,step.endpoint);fault("restore_after_step");}
    {
        LevelStore recovered(repository);recovered.verify();leveldb::ReadOptions options;options.verify_checksums=true;
        recovered.verify_recovery_graph(options,recovered.sequence_);
        need(recovered.sequence_==target.sequence && recovered.repository_==recovery_hex(recovery_unhex(target.repository,16)) &&
             recovered.replay_head_==recovery_unhex(target.chain_hash,32),"restore did not reach requested checkpoint");
    }
    verify_namespace(repository);
    sync_parent(repository+"/CURRENT");fault("restore_before_publish");publish_directory(repository,destination);
    fault("restore_after_publish");return target.sequence;
}
uint64_t LevelStore::replay_archive(const std::string& archive,const std::string& destination,std::optional<RecoveryPoint> until){
    std::optional<std::pair<uint64_t,std::string>> endpoint;std::string repository;
    if(until){repository=recovery_unhex(until->repository,16);endpoint={{until->sequence,recovery_unhex(until->chain_hash,32)}};}
    replay::File file(archive);auto header=replay::validate(file,endpoint);
    if(until)need(repository==header.repository,"recovery point repository mismatch");
    const auto stop=until?until->sequence:header.to;
    if(header.kind=='B'){
        // A crashed base restore leaves only a private staging directory.
        // Ordinary failures clean up this attempt after LevelDB has closed.
        // Publication cannot replace an existing target, even under a rename race.
        struct stat st{};
        if(lstat(destination.c_str(),&st)==0){
            need(S_ISDIR(st.st_mode),"backup destination is not a repository directory");LevelStore existing(destination);
            if(until)need(existing.sequence_<=stop,"destination is ahead of recovery point");
            std::string identity;need(existing.read("M/repository",identity) && identity==header.repository && existing.replay_hash(header.to)==header.head,"backup destination is not this recovery lineage");
            existing.verify();leveldb::ReadOptions options;options.verify_checksums=true;existing.verify_recovery_graph(options,existing.sequence_);return existing.sequence_;
        }
        need(errno==ENOENT,"backup destination is inaccessible");
        auto staging=destination+".restore-XXXXXX";if(!mkdtemp(staging.data()))throw std::system_error(errno,std::generic_category(),"create restore staging directory");
        struct StagingCleanup {
            const std::string& path;
            ~StagingCleanup() noexcept {
                // Preserve the original restore exception if cleanup itself fails.
                std::error_code ignored;std::filesystem::remove_all(path,ignored);
            }
        } cleanup{staging};
        {
            LevelStore target(staging,true,default_object_cache_bytes);
            replay::Reader in(file);need(in.header.encode()==header.encode(),"archive changed during replay");
            replay::Batch batch;size_t pending=0;
            auto flush=[&]{if(pending){target.publish(batch);batch.Clear();pending=0;fault("replay_base_rows");}};
            std::string previous;std::set<std::string> metadata;
            while(!in.ended){auto payload=in.next();if(in.ended)break;auto op=replay::base_row(payload);
                need(previous.empty() || previous<op.key,"unordered backup rows");previous=op.key;
                if(op.key.starts_with("M/"))metadata.insert(op.key);
                batch.Put(op.key,op.value);pending+=op.key.size()+op.value.size();if(pending>=1024*1024)flush();
            }
            flush();need(metadata==std::set<std::string>{"M/format","M/repository","M/sequence","M/objects","M/roots"},"incomplete backup metadata");
            std::string value;need(target.read("M/repository",value) && value==header.repository,"backup repository mismatch");
            need(target.read("M/sequence",value) && number(value)==header.to,"backup sequence mismatch");
            batch.Put("M/replay/start",integer(header.to));batch.Put("M/replay/base",header.head);batch.Put("M/replay/head",header.head);
            target.publish(batch);target.verify();leveldb::ReadOptions options;options.verify_checksums=true;target.verify_recovery_graph(options,header.to);fault("replay_base_before_publish");
        }
        sync_parent(staging+"/CURRENT");publish_directory(staging,destination);fault("replay_base_after_publish");return header.to;
    }
    struct stat st{};need(lstat(destination.c_str(),&st)==0 && S_ISDIR(st.st_mode),"incremental destination must be an existing repository directory");
    LevelStore target(destination);std::lock_guard lock(target.writer_mutex_);target.healthy();
    if(until)need(target.sequence_<=stop,"destination is ahead of recovery point");
    target.verify();
    leveldb::ReadOptions graph_options;graph_options.verify_checksums=true;target.verify_recovery_graph(graph_options,target.sequence_);
    std::string identity;need(target.read("M/repository",identity) && identity==header.repository,"replay repository identity mismatch");
    need(target.replay_hash(header.from)==header.previous,"replay base differs from destination");
    need(target.sequence_>=header.from,"replay gap at destination");
    replay::Reader in(file);need(in.header.encode()==header.encode(),"archive changed during replay");
    uint64_t seq=header.from;auto head=header.previous;
    while(!in.ended){auto payload=in.next();if(in.ended)break;auto tx=replay::transaction(payload);auto next=replay::hash(payload);
        need(seq!=UINT64_MAX && tx.sequence==seq+1 && tx.sequence<=header.to && tx.previous==head,"invalid replay chain during apply");seq=tx.sequence;head=next;
        if(tx.sequence>stop)continue;
        if(tx.sequence<=target.sequence_){need(target.replay_hash(tx.sequence)==next,"replay diverges from already applied transaction");continue;}
        need(tx.sequence==target.sequence_+1 && tx.previous==target.replay_head_,"replay gap or divergent destination");
        uint64_t objects=target.objects_,roots=target.roots_;std::set<std::string> new_objects;replay::Batch batch;
        for(const auto& op:tx.ops)if(op.key.starts_with("O/")){
            std::string old;need(!target.read(op.key,old),"replay object already exists");new_objects.insert(op.key);++objects;
        }
        for(const auto& op:tx.ops){
            if(op.key.starts_with("R/")){
                std::string old;bool exists=target.read(op.key,old);
                if(op.erase){need(exists,"replay deletes missing root");--roots;}
                else {std::string value;auto key="O/"+op.value;need(new_objects.contains(key) || target.read(key,value),"replay root references missing object");if(!exists)++roots;}
            }
            if(op.key=="M/objects")need(number(op.value)==objects,"replay object count mismatch");
            if(op.erase)batch.Delete(op.key);else batch.Put(op.key,op.value);
        }
        for(const auto& op:tx.ops)if(op.key=="M/roots")need(number(op.value)==roots,"replay root count mismatch");
        // Validate new roots against this transaction's objects before publishing.
        std::map<std::string,const std::string*> overlay;
        for(const auto& op:tx.ops)if(op.key.starts_with("O/"))overlay.emplace(op.key,&op.value);
        replay::Graph graph([&](const Id& object_id){
            auto key="O/"+bytes(object_id);auto found=overlay.find(key);std::string value;
            if(found!=overlay.end())value=*found->second;else need(target.read(key,value),"missing replay graph object");
            Bytes b(value.begin(),value.end());need(Journal::object_id(b)==object_id,"replay graph object checksum mismatch");return b;
        },tx.sequence);
        for(const auto& op:tx.ops)if(!op.erase)graph.root(op.key,op.value);
        batch.Put(replay::logkey(tx.sequence),payload);batch.Put("M/replay/head",next);
        fault("replay_before_write");target.publish(batch);fault("replay_after_write");
        target.sequence_=tx.sequence;target.objects_=objects;target.roots_=roots;target.replay_head_=next;
    }
    need(seq==header.to && head==header.head,"incomplete replay range during apply");
    if(until)need(target.sequence_==stop && target.replay_head_==endpoint->second,"recovery point not reached");
    return target.sequence_;
}
}

namespace forkfs {
std::string LevelStore::revision_point(const std::string& name) const {
    auto pin=pin_locations();
    NamespaceView::valid_name(name);
    auto hex=[](const std::string& bytes){
        std::string result;constexpr char digits[]="0123456789abcdef";
        for(unsigned char c:bytes){result+=digits[c>>4];result+=digits[c&15];}return result;
    };
    std::unique_lock writer(writer_mutex_);healthy();
    const auto end=sequence_.load(),start=replay_start_;
    const auto anchor=replay_base_,end_head=replay_head_;const bool enabled=replay_enabled_;
    auto release=[this](const leveldb::Snapshot* s){db_->ReleaseSnapshot(s);};
    std::unique_ptr<const leveldb::Snapshot,decltype(release)> snapshot(db_->GetSnapshot(),release);
    leveldb::ReadOptions options;options.verify_checksums=true;options.snapshot=snapshot.get();
    writer.unlock();
    const auto key="R/"+std::string("\0forkfs-history/",16)+"revision/"+name;
    std::string revision,descriptor;
    status(db_->Get(options,key,&revision),"read revision recovery root");
    need(revision.size()==32,"invalid revision recovery root");
    descriptor=object_payload(options,revision);
    need(replay::hash(descriptor)==revision,"revision recovery descriptor checksum mismatch");
    auto record=RevisionRecord::decode(Bytes(descriptor.begin(),descriptor.end()));
    auto source=record.source_sequence;need(source<end,"invalid revision source sequence");
    auto prefix="{\"repository\":\""+repository_+"\",\"revision_id\":\""+hex(revision)+"\",\"source_sequence\":"+std::to_string(source);
    auto unavailable=[&](const char* reason){return prefix+",\"available\":false,\"reason\":\""+reason+"\",\"checkpoint\":null}\n";};
    if(!enabled)return unavailable("retention_disabled");
    // An older descriptor cannot establish when its name was originally bound.
    // A later re-PUT of the same root is not evidence of its creation transaction.
    if(source<start)return unavailable("creation_not_retained");
    std::lock_guard index_lock(revision_index_mutex_);
    const auto cache_key=std::make_pair(key,revision);RevisionIndex index;
    auto cached=revision_index_.find(cache_key);
    if(cached!=revision_index_.end() && cached->second.start==start && cached->second.anchor==anchor && cached->second.through<=end)
        index=cached->second;
    else {index.start=start;index.through=start;index.anchor=anchor;index.head=anchor;}
    if(index.through<end){
        std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(options));
        it->Seek(replay::logkey(index.through+1));
        while(index.through<end){
            need(it->Valid() && it->key().ToString()==replay::logkey(index.through+1),"missing retained revision event log");
            auto record=logical_value(options,it->key().ToString(),it->value().ToString());auto tx=replay::transaction(record);
            need(tx.sequence==index.through+1 && tx.previous==index.head,"invalid revision event log chain");
            auto head=replay::hash(record);
            for(const auto& op:tx.ops)if(op.key==key && !op.erase && op.value==revision){
                need(tx.sequence>source,"revision event precedes source sequence");
                if(index.matches<2)++index.matches;
                index.event=tx.sequence;index.event_head=head;
            }
            index.through=tx.sequence;index.head=std::move(head);it->Next();
        }
        status(it->status(),"scan revision event log");
    }
    need(index.head==end_head,"revision event log head mismatch");
    if(!revision_index_.contains(cache_key) && revision_index_.size()>=128)revision_index_.erase(revision_index_.begin());
    revision_index_[cache_key]=index;
    if(!index.matches)return unavailable("creation_not_retained");
    if(index.matches!=1)return unavailable("ambiguous_binding_events");
    return prefix+",\"available\":true,\"checkpoint\":{\"repository\":\""+repository_+"\",\"sequence\":"+
        std::to_string(index.event)+",\"chain_hash\":\""+hex(index.event_head)+"\"}}\n";
}
}

namespace forkfs {
std::string LevelStore::collect_step(size_t max_copy_bytes,size_t max_objects,size_t max_metadata_bytes,size_t mark_work) {
    need(max_copy_bytes>=65580 && max_copy_bytes<=8*1024*1024,"GC copy budget must be 64 KiB..8 MiB");
    need(max_objects>0 && max_objects<=4096,"invalid GC object budget");
    need(max_metadata_bytes>=64*1024 && max_metadata_bytes<=256*1024*1024,"invalid GC metadata memory budget");
    need(mark_work>0 && mark_work<=65536,"invalid GC mark work budget");
    // Conservative allocation charges, not RSS: shared DB/cache buffers and a
    // bounded payload/iterator working set are separate from GC-owned metadata.
    size_t metadata_bytes=0;
    auto charge=[&](size_t n){need(n<=max_metadata_bytes-metadata_bytes,"GC metadata memory budget exceeded");metadata_bytes+=n;};
    std::unique_lock gc(gc_mutex_,std::try_to_lock);need(gc.owns_lock(),"online GC is already running");
    auto pin=pin_objects();healthy();
    uint64_t segments_deleted=0,pending=0;
    charge(32*1024); // bounded marker/task working metadata; disk marks are not resident sets
    uint64_t seq=0,deleted=0,chunks_deleted=0,work_done=0,mark_processed=0;
    bool foreground_pressure=false;
    size_t work_limit=mark_work;
    auto refresh_pressure=[&]{
        if(foreground_demand_.load(std::memory_order_relaxed)!=0){
            foreground_pressure=true;
            work_limit=std::min<size_t>(work_limit,8);
        }
    };
    bool sweep_skipped=false,mark_pending=false;std::string mark_phase="complete";
    std::shared_ptr<GcCycle> cycle;
    try {
        {std::lock_guard writer(writer_mutex_);healthy();
            if(!gc_error_.empty()){auto message=std::move(gc_error_);gc_error_.clear();throw std::runtime_error(message);}
            if(replay_enabled_){gc_cycle_.reset();sweep_skipped=true;seq=sequence_;}
            else {if(!gc_cycle_)gc_cycle_=std::make_shared<GcCycle>(db_.get(),path_,sequence_,metrics_->gc_mutex);cycle=gc_cycle_;seq=cycle->sequence;}}
        fault("gc_mark_after_begin");
        auto read_object=[&](const Id& ref)->Bytes{
            auto location=pin_locations();healthy();leveldb::ReadOptions ro;ro.verify_checksums=true;ro.fill_cache=false;
            auto payload=object_payload(ro,bytes(ref));healthy();return Bytes(payload.begin(),payload.end());};
        bool hook_called=false;
        while(cycle){
#ifdef FORKFS_TEST_GC
            test_gc_work_boundary(work_done);
#endif
            refresh_pressure();
            if(work_done>=work_limit || (cycle->phase==GcCycle::Complete && !cycle->mark.has_work()))break;
            if(cycle->phase==GcCycle::Roots){
                leveldb::ReadOptions ro;ro.verify_checksums=true;ro.fill_cache=false;ro.snapshot=cycle->baseline;
                std::unique_ptr<leveldb::Iterator> roots(db_->NewIterator(ro));
                roots->Seek(cycle->root_cursor.empty()?"R/":cycle->root_cursor);if(!cycle->root_cursor.empty()&&roots->Valid()&&roots->key().ToString()==cycle->root_cursor)roots->Next();
                if(!roots->Valid()||!roots->key().starts_with("R/")){status(roots->status(),"incremental root scan");cycle->phase=GcCycle::Leases;continue;}
                auto key=roots->key().ToString(),value=roots->value().ToString();status(roots->status(),"incremental root scan");
                auto lookup=[&](const std::string& name)->std::optional<Id>{std::string value;auto found=db_->Get(ro,name,&value);if(found.IsNotFound())return std::nullopt;status(found,"incremental root pair");return id(value);};
                cycle->mark.seed_root(key,id(value),lookup);cycle->root_cursor=key;++work_done;continue;
            }
            if(cycle->phase==GcCycle::Leases){
                std::unique_lock writer(writer_mutex_);healthy();if(gc_cycle_!=cycle)throw std::runtime_error("GC mark invalidated");
                std::optional<Id> next;
                {std::lock_guard pins(pin_state_->mutex);auto it=cycle->lease_started?pin_state_->manifests.upper_bound(cycle->lease_cursor):pin_state_->manifests.begin();if(it!=pin_state_->manifests.end())next=it->first;}
                if(next){cycle->mark.seed_manifest(*next);cycle->lease_cursor=*next;cycle->lease_started=true;++work_done;}
                else cycle->phase=GcCycle::Mark;
                continue;
            }
            if(cycle->mark.has_work()) {work_done+=cycle->mark.advance(1,read_object);fault("gc_mark_after_task");continue;}
            if(cycle->phase==GcCycle::Mark){cycle->phase=GcCycle::Objects;continue;}
#ifdef FORKFS_TEST_GC
            if(!hook_called){extern void test_gc_marked();test_gc_marked();hook_called=true;}
#else
            (void)hook_called;
#endif
            TimedMutexLock writer(writer_mutex_,metrics_->gc_writer_sweep);healthy();if(gc_cycle_!=cycle)throw std::runtime_error("GC mark invalidated");
            if(replay_enabled_){gc_cycle_.reset();cycle.reset();sweep_skipped=true;break;}
            if(cycle->mark.has_work()){sweep_skipped=true;break;}
            if(cycle->phase==GcCycle::Objects || cycle->phase==GcCycle::Chunks){
                std::lock_guard pins(pin_state_->mutex);
                if(pin_state_->logical!=1 || pin_state_->locations!=0){sweep_skipped=true;break;}
                const bool objects_phase=cycle->phase==GcCycle::Objects;
                auto& cursor=objects_phase?cycle->object_cursor:cycle->chunk_cursor;const std::string prefix=objects_phase?"O/":"C/";
                leveldb::ReadOptions ro;ro.verify_checksums=true;ro.fill_cache=false;ro.snapshot=objects_phase?cycle->baseline:nullptr;
                std::unique_ptr<leveldb::Iterator> scan(db_->NewIterator(ro));scan->Seek(cursor.empty()?prefix:cursor);if(!cursor.empty()&&scan->Valid()&&scan->key().ToString()==cursor)scan->Next();
                leveldb::WriteBatch batch;std::vector<std::string> forgotten;uint64_t root_removals=0,object_removals=0,chunk_removals=0;
                const std::string alias("R/\0forkfs-history/object/",25);
                size_t visits=0;bool exhausted=false;
                for(;visits<128;scan->Next()){
                    refresh_pressure();
                    if(work_done+visits>=work_limit)break;
                    if(!scan->Valid()||!scan->key().starts_with(prefix)){exhausted=true;break;}
                    auto key=scan->key().ToString();need(key.size()==34,"invalid incremental GC object/chunk key");++visits;cursor=key;
                    const bool live=objects_phase?cycle->mark.live(id(key.substr(2))):cycle->mark.chunk_live(key.substr(2));if(live)continue;
                    std::string existing;auto present=db_->Get({},key,&existing);if(present.IsNotFound())continue;status(present,"incremental GC candidate");
                    charge(256);batch.Delete(key);
                    if(objects_phase){++object_removals;forgotten.push_back(key.substr(2));auto alias_key=alias+recovery_hex(key.substr(2));std::string ref;auto found=db_->Get({},alias_key,&ref);if(found.ok()){need(ref==key.substr(2),"invalid incremental staging alias");charge(256);batch.Delete(alias_key);++root_removals;}else if(!found.IsNotFound())status(found,"incremental staging alias");}
                    else ++chunk_removals;
                    const auto phase_deleted=objects_phase?deleted:chunks_deleted;
                    const auto batch_deleted=objects_phase?object_removals:chunk_removals;
                    if(phase_deleted+batch_deleted>=max_objects){scan->Next();if(!scan->Valid()||!scan->key().starts_with(prefix))exhausted=true;break;}
                }
                status(scan->status(),"incremental sweep scan");work_done+=visits;
                if(object_removals || chunk_removals){
                    need(object_removals<=objects_ && root_removals<=roots_,"incremental GC counter underflow");
                    if(object_removals)batch.Put("M/objects",integer(objects_-object_removals));if(root_removals)batch.Put("M/roots",integer(roots_-root_removals));
                    try{fault("gc_mark_before_sweep");leveldb::WriteOptions wo;wo.sync=true;{TimedOperation timing(metrics_->gc_sweep_dbwrite);status(db_->Write(wo,&batch),"incremental GC metadata prune");}fault("gc_mark_after_sweep");}catch(...){object_store_->abandon();poisoned_=true;throw;}
                    objects_=objects_-object_removals;roots_=roots_-root_removals;deleted+=object_removals;chunks_deleted+=chunk_removals;cycle->deleted+=object_removals+chunk_removals;
                    for(const auto& raw:forgotten)object_store_->forget(raw);
                    if(root_removals){std::lock_guard cache(root_cache_mutex_);root_cache_.clear();root_cache_sequence_=UINT64_MAX;}
                }
                if(exhausted){if(objects_phase){cycle->references=db_->GetSnapshot();cycle->phase=GcCycle::References;}else cycle->phase=GcCycle::Complete;}
                // Deletion limits apply to the whole call, not each batch.
                if(deleted>=max_objects || chunks_deleted>=max_objects)break;
                continue;
            }
            if(cycle->phase==GcCycle::References){
                leveldb::ReadOptions ro;ro.verify_checksums=true;ro.fill_cache=false;ro.snapshot=cycle->references;
                std::unique_ptr<leveldb::Iterator> refs(db_->NewIterator(ro));refs->Seek(cycle->reference_cursor.empty()?"O/":cycle->reference_cursor);if(!cycle->reference_cursor.empty()&&refs->Valid()&&refs->key().ToString()==cycle->reference_cursor)refs->Next();
                if(!refs->Valid()||!refs->key().starts_with("O/")){status(refs->status(),"incremental chunk references");cycle->phase=GcCycle::Chunks;continue;}
                auto descriptor=refs->value().ToString();replay::Input in{descriptor};need(in.take(8)=="FFOBJ002","invalid incremental object descriptor");(void)in.integer(8);auto count=in.integer(4);need(count>0&&count<=4,"invalid incremental chunk count");for(uint64_t i=0;i<count;++i)cycle->mark.protect_chunk(in.take(32));in.end();cycle->reference_cursor=refs->key().ToString();status(refs->status(),"incremental chunk references");++work_done;continue;
            }
        }
        if(cycle){mark_processed=cycle->mark.processed();mark_pending=cycle->phase!=GcCycle::Complete;mark_phase=cycle->phase_name();
            if(!mark_pending){std::lock_guard writer(writer_mutex_);healthy();if(gc_cycle_!=cycle)throw std::runtime_error("GC mark invalidated");if(cycle->mark.has_work()){mark_pending=true;mark_phase="mark";}else {mark_pending=cycle->deleted!=0 || cycle->sequence!=sequence_;if(mark_pending){mark_phase="cycle_complete";gc_cycle_.reset();}}}}
    }catch(...){std::lock_guard writer(writer_mutex_);if(gc_cycle_==cycle)gc_cycle_.reset();throw;}
    uint64_t moved=0,copied=0,physical_work=0,index_work=0,reference_work=0,candidate_work=0;
    bool physical_pending=true;
    std::string physical_phase="complete";
    if(!gc_physical_){
        gc_physical_=std::make_unique<GcPhysical>(db_.get(),path_);
        std::lock_guard writer(writer_mutex_);gc_physical_->high_water=object_store_->tail().first;gc_physical_->sequence=sequence_;gc_physical_->snapshot=db_->GetSnapshot();
    }
    auto& ph=*gc_physical_;
    ph.changed=ph.changed || chunks_deleted!=0;
    ph.eligible=ph.eligible || !mark_pending || sweep_skipped || mark_phase=="cycle_complete";
    leveldb::ReadOptions ro;ro.verify_checksums=true;ro.fill_cache=false;
    auto chunk_location=[&](const std::string& loc){
        replay::Input d{loc};auto sid=d.integer(8);auto offset=d.integer(8);auto n=d.integer(4);d.end();
        need(sid>0 && offset>=48 && n<=65536,"invalid physical GC chunk location");
        return std::pair<uint64_t,uint64_t>{sid,n};
    };
    try {
        // EOF proof remains valid for this sealed retired segment. A reader
        // delays unlink only; let inventory/relocation finish in the meantime.
        refresh_pressure();
        if(ph.reap_ready){
            pending=1;
            TimedMutexLock writer(writer_mutex_,metrics_->gc_writer_retire);healthy();std::lock_guard pins(pin_state_->mutex);
            if(pin_state_->locations==0){
                object_store_->retire_segment(ph.reap_sid);fault("gc_online_after_unlink");
                leveldb::WriteBatch del;del.Delete(ph.reap_key);leveldb::WriteOptions wo;wo.sync=true;
                try{TimedOperation timing(metrics_->gc_retirement_dbwrite);status(db_->Write(wo,&del),"delete retired segment marker");}catch(...){object_store_->abandon();poisoned_=true;throw;}
                fault("gc_online_after_delete_sync");++segments_deleted;++physical_work;pending=0;ph.reaped_high_water=std::max(ph.reaped_high_water,ph.reap_sid);
                ph.reap_ready=false;ph.changed=true;
                if(ph.phase==GcPhysical::WaitPins){ph.directory_started=false;ph.phase=GcPhysical::ReapMarker;}
            }
        }
        while(true){
            refresh_pressure();
            if(physical_work>=work_limit)break;
            healthy();
            if(ph.phase==GcPhysical::Cleanup){
                ++physical_work;
                if(ph.index.cleanup_one()){
                    const bool again=ph.changed || ph.sequence!=sequence_;
                    physical_pending=again;physical_phase=again?"round_complete":"complete";gc_physical_.reset();break;
                }
                continue;
            }
            if(ph.phase==GcPhysical::BuildIndex || ph.phase==GcPhysical::RefreshActive){
                auto scan_options=ro;scan_options.snapshot=ph.snapshot;
                auto it=std::unique_ptr<leveldb::Iterator>(db_->NewIterator(scan_options));
                it->Seek(ph.cursor.empty()?"C/":ph.cursor);
                if(!ph.cursor.empty() && it->Valid() && it->key().ToString()==ph.cursor)it->Next();
                status(it->status(),"build physical GC reverse index");++physical_work;
                if(it->Valid() && it->key().starts_with("C/")){
                    auto key=it->key().ToString(),loc=it->value().ToString();auto [sid,n]=chunk_location(loc);
                    if(ph.phase==GcPhysical::BuildIndex || sid==ph.candidate)ph.index.add(sid,key,loc,n);
                    ph.cursor=key;++index_work;fault("gc_physical_after_index_row");continue;
                }
                it.reset();db_->ReleaseSnapshot(ph.snapshot);ph.snapshot=nullptr;
                const bool refresh=ph.phase==GcPhysical::RefreshActive;
                if(refresh)ph.tail_refreshed=true;
                ph.cursor.clear();ph.phase=refresh?GcPhysical::SourceC:GcPhysical::ReapMarker;
                if(refresh){
                    // Earlier relocations may have packed the initial tail.
                    // Refresh proves coverage, not that a fresh copy is useful.
                    auto file=object_store_->inspect_segment(std::filesystem::directory_entry(object_store_->segment_path(ph.candidate)));
                    ph.length=file.second;
                    if(ph.index.bytes(ph.candidate)+48>=ph.length)ph.phase=GcPhysical::Inventory;
                }
                fault("gc_physical_after_index_complete");continue;
            }
            if(ph.phase==GcPhysical::WaitPins)break;
            if(ph.phase==GcPhysical::ReapMarker && ph.reap_ready){ph.phase=GcPhysical::Inventory;continue;}
            if(ph.phase==GcPhysical::ReapMarker){
                ++physical_work;
                auto it=std::unique_ptr<leveldb::Iterator>(db_->NewIterator(ro));it->Seek("G/");
                status(it->status(),"physical GC retired marker");
                if(!it->Valid() || !it->key().starts_with("G/")){ph.phase=GcPhysical::Inventory;continue;}
                ph.reap_key=it->key().ToString();
                need(ph.reap_key.size()==10 && it->value().ToString()=="FFGCRET1","invalid retired segment marker");
                ph.reap_sid=number(ph.reap_key.substr(2));need(ph.reap_sid>0,"invalid retired segment ID");
                need(ph.reap_sid<ph.high_water || (ph.reap_sid==ph.high_water && ph.tail_refreshed),"retired segment outside indexed generation");
                {std::lock_guard writer(writer_mutex_);healthy();
                    need(physical_format_>=7 && ph.reap_sid<object_store_->tail().first,"retired segment lacks a durable higher segment");}
                pending=1;ph.cursor.clear();ph.phase=GcPhysical::ReapC;continue;
            }
            if(ph.phase==GcPhysical::ReapC){
                auto row=ph.index.next(ph.reap_sid,ph.cursor);++physical_work;pending=1;
                if(row){
                    ++reference_work;std::string current;auto st=db_->Get(ro,row->first,&current);
                    if(!st.IsNotFound()){status(st,"validate retired reverse reference");
                        need(chunk_location(current).first!=ph.reap_sid,"retired segment still has a current chunk reference");}
                    ph.cursor=row->first;continue;
                }
                TimedMutexLock writer(writer_mutex_,metrics_->gc_writer_retire);healthy();std::lock_guard pins(pin_state_->mutex);
                if(pin_state_->locations!=0 || segments_deleted!=0){
                    ph.reap_ready=true;ph.cursor.clear();ph.phase=GcPhysical::Inventory;
                    continue;
                }
                object_store_->retire_segment(ph.reap_sid);fault("gc_online_after_unlink");
                leveldb::WriteBatch del;del.Delete(ph.reap_key);leveldb::WriteOptions wo;wo.sync=true;
                try{TimedOperation timing(metrics_->gc_retirement_dbwrite);status(db_->Write(wo,&del),"delete retired segment marker");}catch(...){object_store_->abandon();poisoned_=true;throw;}
                fault("gc_online_after_delete_sync");++segments_deleted;pending=0;ph.changed=true;ph.reaped_high_water=std::max(ph.reaped_high_water,ph.reap_sid);
                ph.cursor.clear();ph.phase=GcPhysical::ReapMarker;continue;
            }
            if(ph.phase==GcPhysical::Inventory){
                if(!ph.eligible)break; // do not copy garbage before logical pruning finishes
                if(!ph.directory_started){ph.directory=std::filesystem::directory_iterator(path_);ph.directory_started=true;}
                if(ph.directory==ph.end){
                    // Inventory was finite. New writes/deletions require another round,
                    // but never reset a cursor while an existing round is progressing.
                    if(ph.reap_ready){ph.phase=GcPhysical::WaitPins;pending=1;break;}
                    ph.phase=GcPhysical::Cleanup;continue;
                }
                auto entry=*ph.directory;++ph.directory;++physical_work;
                const auto name=entry.path().filename().string();
                if(!name.starts_with("DATA-") || !name.ends_with(".seg"))continue;
                // readdir may retain an entry unlinked by this same GC round.
                // Only the exclusive owner mutates DATA names; bound tolerance
                // to IDs at/below a retirement already proved by this round.
                if(ph.reaped_high_water){
                    struct stat st{};
                    if(lstat(entry.path().c_str(),&st)<0 && errno==ENOENT){
                        auto digits=name.substr(5,name.size()-9);
                        need(!digits.empty() && digits.find_first_not_of("0123456789")==std::string::npos,"invalid data segment name");
                        auto sid=std::stoull(digits);need(sid>0 && std::to_string(sid)==digits && sid<=ph.reaped_high_water,"unexpected missing GC segment");
                        continue;
                    }
                }
                auto file=object_store_->inspect_segment(entry);
                if(file.first>ph.high_water)continue;
                std::string marker;auto st=db_->Get(ro,"G/"+integer(file.first),&marker);
                if(st.ok()){need(marker=="FFGCRET1","invalid retired segment marker");continue;}
                if(!st.IsNotFound())status(st,"physical GC candidate marker");
                ph.candidate=file.first;ph.length=file.second;ph.indexed=0;ph.cursor.clear();ph.phase=GcPhysical::CandidateC;continue;
            }
            if(ph.phase==GcPhysical::CandidateC){
                ++physical_work;++candidate_work;ph.indexed=ph.index.bytes(ph.candidate);
                if(ph.indexed+48>=ph.length){ph.phase=GcPhysical::Inventory;ph.cursor.clear();continue;}
                TimedMutexLock writer(writer_mutex_,metrics_->gc_writer_retire);healthy();
                if(ph.candidate==object_store_->tail().first){
                    object_store_->admit_segment_header();
                    try{object_store_->seal_and_rotate();}catch(...){object_store_->abandon();poisoned_=true;throw;}
                }
                need(ph.candidate<object_store_->tail().first,"physical GC source is not sealed");
                ph.cursor.clear();
                if(ph.candidate==ph.high_water){
                    // The initial tail could gain new locations after the first
                    // snapshot, even if a foreground write already rotated it.
                    ph.index.reset_bytes(ph.candidate);ph.snapshot=db_->GetSnapshot();ph.phase=GcPhysical::RefreshActive;
                }else ph.phase=GcPhysical::SourceC;
                continue;
            }
            // C rows cannot start pointing to a sealed source: publications either
            // reuse a current row or append to the higher active segment. A fresh
            // iterator plus exclusive cursor therefore suffices without a long pin.
            auto location_pin=pin_locations();
            auto row=ph.index.next(ph.candidate,ph.cursor);
            if(!row){
                ++physical_work;location_pin.reset();
                TimedMutexLock writer(writer_mutex_,metrics_->gc_writer_retire);healthy();
                leveldb::WriteBatch wb;wb.Put("G/"+integer(ph.candidate),"FFGCRET1");
                const auto fmt=physical_format_.load()%2?7:8;wb.Put("M/format","FFLDB00"+std::to_string(fmt));
                try{fault("gc_online_before_publish");leveldb::WriteOptions wo;wo.sync=true;
                    {TimedOperation timing(metrics_->gc_retirement_dbwrite);status(db_->Write(wo,&wb),"retire online GC source");}
#ifdef FORKFS_TEST_GC
                    extern void test_gc_indexes_published();test_gc_indexes_published();
#endif
                    physical_format_=fmt;fault("gc_online_after_publish");
                }catch(...){object_store_->abandon();poisoned_=true;throw;}
                ph.changed=true;ph.cursor.clear();ph.phase=GcPhysical::ReapMarker;pending=1;continue;
            }
            // Collect a small batch outside the writer lock. Every visited row
            // consumes work; at most eight matching chunks enter a publication.
            std::vector<std::pair<std::string,std::string>> refs;
            uint64_t batch_bytes=0,visits=0;std::string next_cursor=ph.cursor;
            while(row){
                refresh_pressure();
                if(physical_work+visits>=work_limit)break;
                auto key=row->first,loc=row->second;
                need(key.size()==34,"invalid physical GC chunk key");auto [sid,n]=chunk_location(loc);
                if(sid==ph.candidate){
                    if(n>max_copy_bytes-copied-batch_bytes || refs.size()==8)break;
                    charge(1024);refs.emplace_back(key,loc);batch_bytes+=n;
                }
                next_cursor=key;++visits;++reference_work;row=ph.index.next(ph.candidate,next_cursor);
            }
            if(visits==0)break; // copy allowance exhausted; retry same key next call
            TimedMutexLock writer(writer_mutex_,metrics_->gc_writer_relocate);healthy();
            std::vector<std::pair<std::string,std::string>> current_refs;std::vector<size_t> sizes;
            for(const auto& [key,oldloc]:refs){
                std::string current;auto st=db_->Get(ro,key,&current);
                if(!st.ok() && !st.IsNotFound())status(st,"recheck physical GC location");
                if(st.ok() && current==oldloc){current_refs.emplace_back(key,oldloc);sizes.push_back(size_t(chunk_location(oldloc).second));}
            }
            if(!sizes.empty())object_store_->admit(object_store_->append_cost(sizes));
            bool attempted=false;uint64_t batch_moved=0,batch_copied=0;
            try{
                leveldb::WriteBatch wb;
                for(const auto& [key,oldloc]:current_refs){
                    auto hash=key.substr(2);std::string payload;{TimedOperation timing(metrics_->gc_relocation_chunk_read);payload=object_store_->read_chunk(hash,oldloc);}
                    attempted=true;auto newloc=object_store_->relocate(hash,payload);fault("gc_online_after_copy");
                    wb.Put(key,newloc);++batch_moved;batch_copied+=payload.size();
                }
                if(batch_moved){
                    {TimedOperation timing(metrics_->gc_relocation_sync);object_store_->sync();}fault("gc_online_after_sync");leveldb::WriteOptions wo;wo.sync=true;
                    fault("gc_online_before_publish");{TimedOperation timing(metrics_->gc_relocation_dbwrite);status(db_->Write(wo,&wb),"publish online GC relocation");}fault("gc_online_after_chunk_publish");
                }
            }catch(...){if(attempted){object_store_->abandon();poisoned_=true;}throw;}
            moved+=batch_moved;copied+=batch_copied;ph.changed=ph.changed || batch_moved!=0;
            physical_work+=visits;ph.cursor=next_cursor;

        }
        if(gc_physical_)physical_phase=gc_physical_->phase_name();
    }catch(...){gc_physical_.reset();throw;}
    if(!mark_pending && !physical_pending){std::lock_guard writer(writer_mutex_);gc_cycle_.reset();}
    mark_pending=mark_pending || physical_pending;
    refresh_pressure();
    return "{\"metadata_budget_bytes\":"+std::to_string(max_metadata_bytes)+",\"metadata_bytes_charged\":"+std::to_string(metadata_bytes)+",\"mark_work_done\":"+std::to_string(work_done)+",\"mark_processed\":"+std::to_string(mark_processed)+",\"mark_pending\":"+(mark_pending?"true":"false")+",\"mark_phase\":\""+mark_phase+"\",\"physical_work_done\":"+std::to_string(physical_work)+",\"physical_index_work\":"+std::to_string(index_work)+",\"physical_reference_work\":"+std::to_string(reference_work)+",\"physical_candidate_work\":"+std::to_string(candidate_work)+",\"physical_pending\":"+(physical_pending?"true":"false")+",\"physical_phase\":\""+physical_phase+"\",\"objects_deleted\":"+std::to_string(deleted)+",\"chunks_deleted\":"+std::to_string(chunks_deleted)+",\"chunks_moved\":"+std::to_string(moved)+",\"bytes_copied\":"+std::to_string(copied)+",\"segments_deleted\":"+std::to_string(segments_deleted)+",\"pending_segments\":"+std::to_string(pending)+",\"logical_sweep_skipped\":"+(sweep_skipped?"true":"false")+",\"foreground_pressure\":"+(foreground_pressure?"true":"false")+",\"sequence\":"+std::to_string(seq)+"}";

}

std::string LevelStore::collect(const std::string& input,const std::string& checkpoint,bool dry_run){
    namespace fs=std::filesystem;
    struct stat entry{};need(lstat(input.c_str(),&entry)==0 && S_ISDIR(entry.st_mode),"GC requires a real repository directory");
    const auto path=fs::canonical(input).string();
    struct stat source_directory{};need(lstat(path.c_str(),&source_directory)==0 && S_ISDIR(source_directory.st_mode),"GC requires a real repository directory");
    need(source_directory.st_uid==geteuid(),"GC requires invoking as the repository owner; cross-identity GC is unsupported");
    const auto repository_mode=source_directory.st_mode&07777;const auto repository_gid=source_directory.st_gid;
    const auto canonical_checkpoint=[&](){
        if(checkpoint.empty())return std::string{};auto requested=fs::absolute(checkpoint);
        need(!requested.filename().empty() && requested.filename()!="." && requested.filename()!="..","invalid GC checkpoint destination");
        return (fs::canonical(requested.parent_path())/requested.filename()).string();
    };
    const auto requested_checkpoint=canonical_checkpoint();
    // A post-exchange checkpoint is completed under the stable owner lease
    // before any ordinary LevelStore can open the repository.
    {
        RepositoryOwner recovery_owner(path);
        const auto pending_bytes=gc_checkpoint::read_file(fs::path(path)/"GC-PENDING",32*1024);
        const auto done_bytes=gc_checkpoint::read_file(fs::path(path)/"GC-DONE",32*1024);
        auto verify_receipt=[&](const gc_checkpoint::Receipt& receipt,bool allow_advanced){
            need(receipt.repository==path,"GC receipt belongs to another repository path");
            leveldb::Options options;options.create_if_missing=false;leveldb::DB* raw=nullptr;
            status(leveldb::DB::Open(options,path,&raw),"open repository for GC receipt recovery");std::unique_ptr<leveldb::DB> db(raw);
            leveldb::ReadOptions read;read.verify_checksums=true;
            std::string identity,sequence,start,anchor;
            status(db->Get(read,"M/repository",&identity),"read GC receipt repository identity");
            status(db->Get(read,"M/sequence",&sequence),"read GC receipt sequence");
            status(db->Get(read,"M/replay/start",&start),"read GC receipt generation");
            status(db->Get(read,"M/replay/base",&anchor),"read GC receipt anchor");
            need(identity==receipt.identity && number(sequence)>=receipt.sequence && (allow_advanced || number(sequence)==receipt.sequence) &&
                 number(start)==receipt.sequence && anchor==receipt.anchor,
                 "GC receipt does not match the active repository generation");
        };
        auto cleanup_retired=[&](const gc_checkpoint::Receipt& receipt){
            struct stat active_owner{};need(fstat(recovery_owner.fd,&active_owner)==0,"GC recovery owner stat failed");
            const auto parent=fs::path(path).parent_path();const auto prefix=fs::path(path).filename().string()+".gc-";
            std::string repository_hex;const char* digits="0123456789abcdef";for(unsigned char c:receipt.identity){repository_hex+=digits[c>>4];repository_hex+=digits[c&15];}
            const auto ready_marker="FFGC0001"+repository_hex+recovery_hex(replay::hash(path));
            const auto prepared_marker="FFGCP001"+repository_hex+recovery_hex(replay::hash(path));
            for(const auto& item:fs::directory_iterator(parent))if(item.path().filename().string().starts_with(prefix)){
                struct stat dir{},owner{};if(lstat(item.path().c_str(),&dir)<0 || !S_ISDIR(dir.st_mode))continue;
                const auto bound=(item.path()/"GC-SOURCE-OWNERSHIP").string();
                const bool same_source=lstat(bound.c_str(),&owner)==0 && S_ISREG(owner.st_mode) && owner.st_dev==active_owner.st_dev && owner.st_ino==active_owner.st_ino;
                bool safe=false;
                if(same_source && gc_checkpoint::read_file(item.path()/"GC-PREPARED",256)==prepared_marker)safe=true;
                if(!safe){const auto owned=(item.path()/"OWNERSHIP").string();const bool same_generation=lstat(owned.c_str(),&owner)==0 && S_ISREG(owner.st_mode) && owner.st_dev==active_owner.st_dev && owner.st_ino==active_owner.st_ino;
                    if(same_generation && gc_checkpoint::read_file(item.path()/"GC-READY",256)==ready_marker)safe=true;}
                if(safe)fs::remove_all(item.path());
            }
            gc_checkpoint::sync_directory(parent);
        };
        if(!pending_bytes.empty()){
            need(!dry_run,"GC checkpoint publication is pending; gc-plan will not recover it");
            auto receipt=gc_checkpoint::decode(pending_bytes);verify_receipt(receipt,false);
            need(!requested_checkpoint.empty() && requested_checkpoint==receipt.archive,
                 "GC checkpoint recovery requires the original --checkpoint target");
            auto capacity=RepositoryBudget::load(path);
            gc_checkpoint::publish_archive(fs::path(path)/"GC-BASELINE.pack",receipt,capacity.min_free);fault("gc_after_checkpoint_publish");
            gc_checkpoint::write_atomic(fs::path(path)/"GC-DONE",gc_checkpoint::encode(receipt));fault("gc_after_checkpoint_complete");
            if(unlink((path+"/GC-PENDING").c_str())<0)throw std::system_error(errno,std::generic_category(),"remove completed GC receipt");
            gc_checkpoint::sync_directory(path);
            if(unlink((path+"/GC-BASELINE.pack").c_str())<0 && errno!=ENOENT)throw std::system_error(errno,std::generic_category(),"remove GC checkpoint baseline");
            gc_checkpoint::sync_directory(path);
            cleanup_retired(receipt);return receipt.report;
        }
        if(!requested_checkpoint.empty() && !done_bytes.empty()){
            auto receipt=gc_checkpoint::decode(done_bytes);
            if(receipt.repository==path && receipt.archive==requested_checkpoint){
                need(!dry_run,"GC checkpoint already completed; gc-plan will not replay checkpoint state");
                verify_receipt(receipt,true);uint64_t size=0;
                need(gc_checkpoint::fingerprint(requested_checkpoint,&size)==receipt.fingerprint && size==receipt.size,
                     "completed GC checkpoint archive no longer matches its receipt");
                if(unlink((path+"/GC-BASELINE.pack").c_str())<0 && errno!=ENOENT)throw std::system_error(errno,std::generic_category(),"remove GC checkpoint baseline");
                gc_checkpoint::sync_directory(path);
                cleanup_retired(receipt);return receipt.report;
            }
        }
    }
    auto source=std::make_unique<LevelStore>(path);source->healthy();source->verify();
    need(source->physical_format_>=5,"GC requires the segmented layout");
    const bool reset=!checkpoint.empty(),pinned=source->replay_enabled_ && !reset;
    // Read and validate under the source lease that remains held through copying;
    // an earlier recovery lease may have observed a different replay generation.
    std::optional<std::string> validated_done_receipt;
    if(!reset){
        auto bytes=gc_checkpoint::read_file(fs::path(path)/"GC-DONE",32*1024);
        if(!bytes.empty()){
            auto receipt=gc_checkpoint::decode(bytes);
            need(receipt.repository==path && recovery_hex(receipt.identity)==source->repository_ &&
                 source->sequence_.load()>=receipt.sequence && source->replay_start_==receipt.sequence && source->replay_base_==receipt.anchor,
                 "GC receipt does not match the active repository generation");
            validated_done_receipt=std::move(bytes);
        }
    }
    if(reset){auto parent=fs::canonical(fs::absolute(checkpoint).parent_path());
        auto relative=parent.lexically_relative(path);need(parent!=path && (relative.empty() || *relative.begin()==".."),"GC checkpoint must be outside the repository");
        need(!gc_checkpoint::exists_nofollow(requested_checkpoint),"GC checkpoint destination must not exist");}
    leveldb::ReadOptions options;options.verify_checksums=true;
    std::map<std::string,std::string> roots;std::set<Id> live;std::set<Id> manifests;
    std::unique_ptr<leveldb::Iterator> it(source->db_->NewIterator(options));
    for(it->Seek("R/");it->Valid() && it->key().starts_with("R/");it->Next())roots.emplace(it->key().ToString(),it->value().ToString());
    status(it->status(),"GC root scan");
    const std::string history("R/\0forkfs-history/",18),ns("\0forkfs/",8),alias=history+"object/";
    auto read_object=[&](const Id& reference){auto value=source->object(reference);live.insert(reference);return value;};
    auto known_inode=[&](const Bytes& value){
        need(value.size()>=48 && value.size()<=128*1024,"GC invalid inode size");
        std::string version(value.begin(),value.begin()+8);
        need(version=="FFINOD01" || version=="FFINOD02" || version=="FFINOD03" || version=="FFINOD04" || version=="FFINOD05","GC unsupported inode codec");
        if(version=="FFINOD03" || version=="FFINOD04" || version=="FFINOD05"){
            need(value.size()>=80,"GC truncated inode");auto flags=static_cast<unsigned>(value[48])|(unsigned(value[49])<<8)|(unsigned(value[50])<<16)|(unsigned(value[51])<<24);
            need(version=="FFINOD05"?flags==2:flags<=1,"GC unknown inode flags");
        }
    };
    auto manifest=[&](const Id& reference){if(!manifests.insert(reference).second)return;
        NamespaceView::verify_manifest(read_object,reference,[&](const std::string& key,const Id& value){if(key.starts_with(ns+"inode/"))known_inode(read_object(value));});};
    RevisionDag dag([&](const Id& reference){return RevisionRecord::read(read_object,reference);},manifest);
    for(const auto& [key,value]:roots){
        if(key.starts_with(alias)){need(key.substr(alias.size())==recovery_hex(value),"GC noncanonical staging alias");continue;}
        auto reference=id(value);auto payload=read_object(reference);
        if(key.starts_with(history+"world/"))manifest(reference);
        else if(key.starts_with(history+"revision/"))dag.validate(reference);
        else if(key.starts_with(history+"base/")){need(payload.size()==32,"GC invalid World base");Id base{};std::copy(payload.begin(),payload.end(),base.begin());dag.validate(base);}
        else if((key.starts_with(history+"orphan/") && key.ends_with("/content")) || key.starts_with("R/"+ns+"content/")){
            auto inode_key=key.starts_with(history+"orphan/")?key.substr(0,key.size()-7)+"inode":"R/"+ns+"inode/"+key.substr(("R/"+ns+"content/").size());
            auto found=roots.find(inode_key);need(found!=roots.end(),"GC content has no inode");auto inode=read_object(id(found->second));known_inode(inode);
            if(std::string(inode.begin(),inode.begin()+8)=="FFINOD05"){
                uint64_t size=0;for(unsigned i=0;i<8;++i)size|=uint64_t(inode[24+i])<<(8*i);FileContent(read_object,payload,size,true).verify();
            }
        }
    }
    if(pinned)for(it->Seek("O/");it->Valid() && it->key().starts_with("O/");it->Next())live.insert(id(it->key().ToString().substr(2)));
    status(it->status(),"GC object scan");
    uint64_t kept_roots=0;for(const auto& [key,value]:roots)if(!key.starts_with(alias) || live.contains(id(value)))++kept_roots;
    uint64_t before_bytes=0;for(const auto& item:fs::directory_iterator(path))if(item.is_regular_file() && item.path().filename().string().starts_with("DATA-") && item.path().extension()==".seg")before_bytes+=item.file_size();
    const auto sequence=source->sequence_.load(),before_objects=source->objects_.load();
    // Estimate the exact unique chunks the copied generation will append.
    // Host-space reservation accounts for the temporary second generation;
    // max_data is enforced against staged DATA bytes by ObjectStore itself.
    std::vector<size_t> staged_chunk_sizes;std::set<std::string> staged_chunks;
    for(const auto& reference:live){auto payload=source->object(reference);auto count=std::max<size_t>(1,(payload.size()+65535)/65536);
        for(size_t i=0;i<count;++i){auto offset=i*65536,length=std::min<size_t>(65536,payload.size()-offset);
            std::string chunk=length?std::string(reinterpret_cast<const char*>(payload.data()+offset),length):std::string{};auto hash=replay::hash(chunk);
            if(staged_chunks.insert(hash).second)staged_chunk_sizes.push_back(chunk.size());}
    }
    const uint64_t staging_reserve=ObjectStore::new_store_cost(staged_chunk_sizes);
    // Planning uses precisely the same live-object/chunk order as the copy.
    // Include record and rollover headers, not merely logical payload sizes.
    // A dry run must not create a staging directory or clean old generations.
    auto report=[&](uint64_t after_bytes,bool applied){
        return "{\"applied\":"+std::string(applied?"true":"false")+
            ",\"sequence\":"+std::to_string(sequence)+
            ",\"objects_before\":"+std::to_string(before_objects)+
            ",\"objects_after\":"+std::to_string(live.size())+
            ",\"roots_before\":"+std::to_string(roots.size())+
            ",\"roots_after\":"+std::to_string(kept_roots)+
            ",\"data_bytes_before\":"+std::to_string(before_bytes)+
            ",\"data_bytes_after\":"+(applied?std::to_string(after_bytes):"null")+
            ",\"data_bytes_estimated_after\":"+std::to_string(staging_reserve)+
            ",\"reclaimable_data_bytes\":"+std::to_string(before_bytes>staging_reserve?before_bytes-staging_reserve:0)+
            ",\"temporary_data_bytes_required\":"+std::to_string(staging_reserve)+
            ",\"replay_pins_all_objects\":"+(pinned?"true":"false")+
            ",\"new_replay_generation\":"+(reset?"true":"false")+"}";
    };
    if(dry_run)return report(0,false);
    RepositoryBudget capacity=RepositoryBudget::load(path);
    uint64_t host_reserve=staging_reserve;
    if(reset){
        uint64_t metadata=0;
        for(const auto& item:fs::directory_iterator(path))if(item.is_regular_file() && !item.path().filename().string().starts_with("DATA-")){
            auto size=item.file_size();need(size<=UINT64_MAX-metadata,"GC metadata size overflow");metadata+=size;
        }
        need(staging_reserve<=UINT64_MAX-host_reserve,"GC checkpoint reserve overflow");host_reserve+=staging_reserve;
        need(metadata<=UINT64_MAX-host_reserve,"GC checkpoint reserve overflow");host_reserve+=metadata;
        constexpr uint64_t archive_overhead=1024*1024;need(archive_overhead<=UINT64_MAX-host_reserve,"GC checkpoint reserve overflow");host_reserve+=archive_overhead;
    }
    // The owner lease excludes a live collector; every matching sibling is a
    // leftover staging or exchanged-out generation from an interrupted run.
    const auto marker=std::string("FFGC0001")+source->repository_+recovery_hex(replay::hash(path));
    auto read_control=[](const fs::path& file,size_t limit)->std::optional<std::string>{
        int fd=open(file.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0){if(errno==ENOENT)return std::nullopt;throw std::system_error(errno,std::generic_category(),"open GC control file");}
        struct Close{int fd;~Close(){close(fd);}} close_fd{fd};struct stat st{};
        need(fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size>=0 && uint64_t(st.st_size)<=limit,"GC invalid control file");
        std::string value(st.st_size,0);size_t at=0;while(at<value.size()){auto n=::read(fd,value.data()+at,value.size()-at);if(n<0 && errno==EINTR)continue;need(n>0,"GC truncated control file");at+=n;}return value;
    };
    auto write_control=[](const fs::path& file,const std::string& value){
        auto temporary=file.string()+".tmp-XXXXXX";int fd=mkstemp(temporary.data());if(fd<0)throw std::system_error(errno,std::generic_category(),"create GC control file");
        try{size_t at=0;while(at<value.size()){auto n=::write(fd,value.data()+at,value.size()-at);if(n<0 && errno==EINTR)continue;if(n<=0)throw std::system_error(n<0?errno:EIO,std::generic_category(),"write GC control file");at+=n;}if(fsync(fd)<0)throw std::system_error(errno,std::generic_category(),"sync GC control file");close(fd);fd=-1;
            if(rename(temporary.c_str(),file.c_str())<0)throw std::system_error(errno,std::generic_category(),"publish GC control file");sync_parent(file.string());}
        catch(...){if(fd>=0)close(fd);unlink(temporary.c_str());throw;}
    };
    struct stat owner_stat{};need(fstat(source->owner_->fd,&owner_stat)==0,"GC owner stat failed");
    const auto parent=fs::path(path).parent_path();const auto prefix=fs::path(path).filename().string()+".gc-";
    const auto prepared_marker=std::string("FFGCP001")+source->repository_+recovery_hex(replay::hash(path));
    for(const auto& item:fs::directory_iterator(parent))if(item.path().filename().string().starts_with(prefix)){
        struct stat dir{},owner{};if(lstat(item.path().c_str(),&dir)<0 || !S_ISDIR(dir.st_mode))continue;
        const auto bound=(item.path()/"GC-SOURCE-OWNERSHIP").string();
        const bool same_source=lstat(bound.c_str(),&owner)==0 && S_ISREG(owner.st_mode) && owner.st_dev==owner_stat.st_dev && owner.st_ino==owner_stat.st_ino;
        bool safe=false;
        if(same_source && read_control(item.path()/"GC-PREPARED",256)==prepared_marker)safe=true;
        if(!safe){const auto owned=(item.path()/"OWNERSHIP").string();const bool same_generation=lstat(owned.c_str(),&owner)==0 && S_ISREG(owner.st_mode) && owner.st_dev==owner_stat.st_dev && owner.st_ino==owner_stat.st_ino;
            if(same_generation && read_control(item.path()/"GC-READY",256)==marker)safe=true;}
        if(safe)fs::remove_all(item.path());
    }
    gc_checkpoint::sync_directory(parent);
    RepositoryBudget{0,capacity.min_free}.admit(path,0,host_reserve);
    auto staging=path+".gc-XXXXXX";need(mkdtemp(staging.data())!=nullptr,"create GC staging directory failed");
    struct Cleanup{const std::string& path;bool swapped=false;~Cleanup(){if(!swapped){std::error_code ignored;fs::remove_all(path,ignored);}}} cleanup{staging};
    write_control(fs::path(staging)/"GC-PREPARED",prepared_marker);sync_parent(staging);
    need(link((path+"/OWNERSHIP").c_str(),(staging+"/GC-SOURCE-OWNERSHIP").c_str())==0,"bind GC staging directory to source lease");
    gc_checkpoint::sync_directory(staging);sync_parent(staging);
    if(capacity.enabled())capacity.save(staging);
    uint64_t archive_estimate=105+69;
    {
        LevelStore target(staging,true,default_object_cache_bytes);replay::Batch batch;size_t pending=0;
        auto flush=[&]{if(pending){target.publish(batch);batch.Clear();pending=0;fault("gc_copy_batch");}};
        for(it->SeekToFirst();it->Valid();it->Next()){
            auto key=it->key().ToString();if(key.starts_with("C/") || key.starts_with("G/"))continue;
            if(key.starts_with("O/") && !live.contains(id(key.substr(2))))continue;
            if(key.starts_with(alias) && !live.contains(id(it->value().ToString())))continue;
            if(reset && (key.starts_with("L/") || key.starts_with("M/replay/")))continue;
            auto value=source->logical_value(options,key,it->value().ToString());
            if(key=="M/objects")value=integer(live.size());if(key=="M/roots")value=integer(kept_roots);
            if(reset && key=="M/format")value="FFLDB001";
            if(reset){const auto row=uint64_t(45)+key.size()+value.size();need(row<=UINT64_MAX-archive_estimate,"GC checkpoint size overflow");archive_estimate+=row;}
            batch.Put(key,value);pending+=key.size()+value.size();if(pending>=1024*1024)flush();
        }
        status(it->status(),"copy live GC records");flush();
    }
    it.reset();
    std::optional<gc_checkpoint::Receipt> checkpoint_receipt;
    {LevelStore target(staging);target.verify();target.verify_recovery_graph(options,sequence);
        if(reset){
            RepositoryBudget{0,capacity.min_free}.admit(staging,0,archive_estimate);
            target.backup(staging+"/GC-BASELINE.pack");
            uint64_t after_bytes=0;for(const auto& item:fs::directory_iterator(staging))if(item.is_regular_file() && item.path().filename().string().starts_with("DATA-") && item.path().extension()==".seg")after_bytes+=item.file_size();
            std::string identity;status(target.db_->Get(options,"M/repository",&identity),"read GC checkpoint identity");
            gc_checkpoint::Receipt receipt;receipt.repository=path;receipt.identity=identity;receipt.archive=requested_checkpoint;
            receipt.report=report(after_bytes,true);receipt.sequence=target.replay_start_;receipt.anchor=target.replay_base_;
            receipt.fingerprint=gc_checkpoint::fingerprint(staging+"/GC-BASELINE.pack",&receipt.size);checkpoint_receipt=std::move(receipt);
        }}
    fault("gc_after_verified");
    for(const auto& name:{"BACKUP-TASKS",".forkfs-fskit-world",".forkfs-fskit-revision"}){
        if(auto value=read_control(fs::path(path)/name,std::string(name)=="BACKUP-TASKS"?2*1024*1024:4096))write_control(fs::path(staging)/name,*value);
    }
    if(!reset && validated_done_receipt)write_control(fs::path(staging)/"GC-DONE",*validated_done_receipt);
    if(checkpoint_receipt)gc_checkpoint::write_atomic(fs::path(staging)/"GC-PENDING",gc_checkpoint::encode(*checkpoint_receipt));
    write_control(fs::path(staging)/"GC-READY",marker);
    need(unlink((staging+"/OWNERSHIP").c_str())==0 && link((path+"/OWNERSHIP").c_str(),(staging+"/OWNERSHIP").c_str())==0,"GC ownership handoff failed");
    struct stat staged_directory{};need(lstat(staging.c_str(),&staged_directory)==0 && S_ISDIR(staged_directory.st_mode),"GC staging directory disappeared");
    if(staged_directory.st_gid!=repository_gid && chown(staging.c_str(),static_cast<uid_t>(-1),repository_gid)<0)
        throw std::system_error(errno,std::generic_category(),"preserve GC repository group");
    if(chmod(staging.c_str(),repository_mode)<0)throw std::system_error(errno,std::generic_category(),"preserve GC repository mode");
    need(lstat(staging.c_str(),&staged_directory)==0 && S_ISDIR(staged_directory.st_mode) && staged_directory.st_uid==source_directory.st_uid &&
         staged_directory.st_gid==repository_gid && (staged_directory.st_mode&07777)==repository_mode,"GC staging directory identity mismatch");
    write_control(fs::path(path)/"GC-READY",marker);
    sync_parent(staging+"/CURRENT");
    if(unlink((staging+"/GC-SOURCE-OWNERSHIP").c_str())<0)throw std::system_error(errno,std::generic_category(),"remove GC staging source lease binding");
    gc_checkpoint::sync_directory(staging);sync_parent(staging);
    auto lease=source->owner_;source.reset();fault("gc_before_swap");
#ifdef __APPLE__
    int exchanged=renamex_np(path.c_str(),staging.c_str(),RENAME_SWAP);
#else
    int exchanged=static_cast<int>(syscall(SYS_renameat2,AT_FDCWD,path.c_str(),AT_FDCWD,staging.c_str(),RENAME_EXCHANGE));
#endif
    if(exchanged<0)throw std::system_error(errno,std::generic_category(),"atomic GC generation exchange");
    cleanup.swapped=true;fault("gc_after_swap");sync_parent(path);fault("gc_after_parent_sync");
    if(checkpoint_receipt){
        fault("gc_before_checkpoint_publish");gc_checkpoint::publish_archive(fs::path(path)/"GC-BASELINE.pack",*checkpoint_receipt,capacity.min_free);
        fault("gc_after_checkpoint_publish");gc_checkpoint::write_atomic(fs::path(path)/"GC-DONE",gc_checkpoint::encode(*checkpoint_receipt));
        fault("gc_after_checkpoint_complete");
        if(unlink((path+"/GC-PENDING").c_str())<0)throw std::system_error(errno,std::generic_category(),"remove completed GC receipt");gc_checkpoint::sync_directory(path);
    }
    uint64_t after_bytes=0;for(const auto& item:fs::directory_iterator(path))if(item.is_regular_file() && item.path().filename().string().starts_with("DATA-") && item.path().extension()==".seg")after_bytes+=item.file_size();
    const auto result=checkpoint_receipt?checkpoint_receipt->report:report(after_bytes,true);
    if(checkpoint_receipt && unlink((path+"/GC-BASELINE.pack").c_str())<0 && errno!=ENOENT)throw std::system_error(errno,std::generic_category(),"remove GC checkpoint baseline");
    fs::remove_all(staging);sync_parent(path);if(checkpoint_receipt)gc_checkpoint::sync_directory(path);return result;
}
}

namespace forkfs {std::pair<uint64_t,uint64_t> LevelStore::space() const {healthy();return object_store_->space();}}
