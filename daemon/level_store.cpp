#include "revision.h"
#include "level_store.h"
#include "replay_format.h"
#include "replay_validation.h"
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
struct StoreIoMetrics {
    std::atomic<uint64_t> root_get_calls=0,root_get_ns=0,root_misses=0,root_cache_hits=0,object_get_calls=0,object_get_ns=0,object_misses=0,meta_get_calls=0,meta_get_ns=0;
    std::atomic<uint64_t> transactions=0,prepare_ns=0,write_ns=0,wal_sync_calls=0,wal_sync_ns=0,other_sync_calls=0,other_sync_ns=0,wal_append_bytes=0,wal_append_ns=0,wal_flush_ns=0;
};
namespace {
using Clock=std::chrono::steady_clock;
uint64_t elapsed(Clock::time_point start){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();}
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
LevelStore::LevelStore(const std::string& path,bool initialize,size_t object_cache_bytes):object_cache_bytes_(object_cache_bytes) {
    leveldb::Options options;options.create_if_missing=initialize;options.error_if_exists=initialize;
    metrics_=std::make_unique<StoreIoMetrics>();env_=std::make_unique<TimedEnv>(*metrics_);options.env=env_.get();
    options.paranoid_checks=true;options.compression=leveldb::kNoCompression;
    cache_.reset(leveldb::NewLRUCache(16*1024*1024));filter_.reset(leveldb::NewBloomFilterPolicy(10));
    object_cache_.reset(leveldb::NewLRUCache(object_cache_bytes));
    options.block_cache=cache_.get();options.filter_policy=filter_.get();
    leveldb::DB* raw=nullptr;status(leveldb::DB::Open(options,path,&raw),"open LevelDB");db_.reset(raw);
    if(initialize) {
        std::string repository(16,0);int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
        if(fd<0)throw std::system_error(errno,std::generic_category(),"repository entropy");
        size_t done=0;
        while(done<repository.size()) {
            auto n=::read(fd,repository.data()+done,repository.size()-done);
            if(n<0 && errno==EINTR)continue;
            if(n<=0){int saved=n<0?errno:EIO;close(fd);throw std::system_error(saved,std::generic_category(),"repository entropy");}done+=n;
        }
        close(fd);leveldb::WriteBatch batch;batch.Put("M/format","FFLDB001");batch.Put("M/repository",repository);
        batch.Put("M/sequence",integer(0));batch.Put("M/objects",integer(0));batch.Put("M/roots",integer(0));
        leveldb::WriteOptions write;write.sync=true;status(db_->Write(write,&batch),"initialize LevelDB repository");
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
LevelStore::~LevelStore()=default;
void LevelStore::healthy() const {need(!poisoned_,"LevelDB store requires reopen after write failure");}
bool LevelStore::read(const std::string& key,std::string& value) const {
    healthy();uint64_t epoch=0;
    if(key.starts_with("R/")) {
        std::lock_guard lock(root_cache_mutex_);epoch=sequence_.load();
        if(root_cache_sequence_!=epoch){root_cache_.clear();root_cache_sequence_=epoch;}
        auto it=root_cache_.find(key);
        if(it!=root_cache_.end()){++metrics_->root_cache_hits;value=it->second.value;return it->second.found;}
    }
    leveldb::ReadOptions options;options.verify_checksums=true;
    auto begin=Clock::now();auto s=db_->Get(options,key,&value);auto ns=elapsed(begin);
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
    healthy();auto key=bytes(object_id);
    // Only verified immutable payloads enter this repository-local cache.
    // RAII releases the pin even if copying the payload throws.
    auto release=[this](leveldb::Cache::Handle* h){object_cache_->Release(h);};
    using Pin=std::unique_ptr<leveldb::Cache::Handle,decltype(release)>;
    if(auto* handle=object_cache_->Lookup(key)) {
        Pin pin(handle,release);++cache_hits_;
        return *static_cast<const Bytes*>(object_cache_->Value(handle));
    }
    ++cache_misses_;
    std::string value;need(read("O/"+bytes(object_id),value),"object not indexed");
    Bytes result(value.begin(),value.end());need(Journal::object_id(result)==object_id,"LevelDB object hash mismatch");
    size_t charge=result.size()+sizeof(Bytes)+32;
    // LevelDB shards its LRU into 16 parts. Do not admit objects larger than
    // one shard; a large file must not consume an entire shard by itself.
    if(charge<=object_cache_bytes_/16) {
        auto copy=std::make_unique<Bytes>(result);
        Pin pin(object_cache_->Insert(key,copy.get(),charge,
            [](const leveldb::Slice&,void* p){delete static_cast<Bytes*>(p);}),release);
        copy.release();
    }
    return result;
}
LevelStore::CacheStats LevelStore::cache_stats() const {
    return {cache_hits_.load(),cache_misses_.load(),object_cache_->TotalCharge()};
}
std::vector<std::string> LevelStore::keys(const std::string& prefix) const {
    healthy();std::vector<std::string> result;auto search="R/"+prefix;
    leveldb::ReadOptions options;options.verify_checksums=true;
    std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(options));
    for(it->Seek(search);it->Valid();it->Next()) {
        auto key=it->key().ToString();if(!key.starts_with(search))break;result.push_back(key.substr(2));
    }
    status(it->status(),"scan LevelDB roots");return result;
}
void LevelStore::transact(uint64_t expected,const std::vector<Mutation>& changes) {
    auto begin=Clock::now();healthy();need(expected==sequence_,"transaction conflict: sequence changed");
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
    std::lock_guard lock(writer_mutex_);healthy();need(expected==sequence_,"transaction conflict: sequence changed");
    need(sequence_!=UINT64_MAX,"transaction sequence exhausted");auto locked_begin=Clock::now();
    uint64_t roots=roots_,objects=objects_;
    for(const auto& [key,hash]:updates) {
        bool exists=contains(key);
        if(hash){if(!exists)++roots;}
        else {need(exists,"cannot delete absent root");--roots;}
    }
    leveldb::WriteBatch batch;
    for(const auto& [hash,value]:values) {
        std::string previous;
        if(read("O/"+bytes(hash),previous))need(previous.size()==value->size() && (previous.empty() || !memcmp(previous.data(),value->data(),value->size())),"immutable object collision/corruption");
        else {batch.Put("O/"+bytes(hash),leveldb::Slice(reinterpret_cast<const char*>(value->data()),value->size()));++objects;}
    }
    for(const auto& [key,hash]:updates){if(hash)batch.Put("R/"+key,bytes(*hash));else batch.Delete("R/"+key);}
    batch.Put("M/sequence",integer(sequence_+1));batch.Put("M/objects",integer(objects));batch.Put("M/roots",integer(roots));
    std::string next_replay_head;
    if(replay_enabled_) {
        replay::Capture capture;status(batch.Iterate(&capture),"capture replay transaction");
        auto record="T"+integer(sequence_+1)+replay_head_+replay::encode(capture.ops);
        next_replay_head=replay::hash(record);
        batch.Put(replay::logkey(sequence_+1),record);batch.Put("M/replay/head",next_replay_head);
    }
    fault("leveldb_before_write");leveldb::WriteOptions options;options.sync=true;
    metrics_->prepare_ns+=prepared_ns+elapsed(locked_begin);auto write_begin=Clock::now();
    auto s=db_->Write(options,&batch);metrics_->write_ns+=elapsed(write_begin);if(!s.ok()){poisoned_=true;status(s,"commit LevelDB batch");}
    if(replay_enabled_)replay_head_=std::move(next_replay_head);
    ++metrics_->transactions;sequence_=sequence_+1;objects_=objects;roots_=roots;fault("leveldb_after_write");
}
std::string LevelStore::metrics_json(bool detailed) const {
    auto& m=*metrics_;std::string result="{\"transactions\":"+std::to_string(m.transactions.load())+
        ",\"prepare_ns\":"+std::to_string(m.prepare_ns.load())+",\"write_ns\":"+std::to_string(m.write_ns.load())+
        ",\"wal_sync_calls\":"+std::to_string(m.wal_sync_calls.load())+",\"wal_sync_ns\":"+std::to_string(m.wal_sync_ns.load())+
        ",\"other_sync_calls\":"+std::to_string(m.other_sync_calls.load())+",\"other_sync_ns\":"+std::to_string(m.other_sync_ns.load())+
        ",\"wal_append_bytes\":"+std::to_string(m.wal_append_bytes.load())+",\"wal_append_ns\":"+std::to_string(m.wal_append_ns.load())+
        ",\"wal_flush_ns\":"+std::to_string(m.wal_flush_ns.load());
    if(detailed){auto c=cache_stats();result+=
        ",\"root_get_calls\":"+std::to_string(m.root_get_calls.load())+",\"root_get_ns\":"+std::to_string(m.root_get_ns.load())+
        ",\"root_misses\":"+std::to_string(m.root_misses.load())+",\"root_cache_hits\":"+std::to_string(m.root_cache_hits.load())+",\"object_get_calls\":"+std::to_string(m.object_get_calls.load())+
        ",\"object_get_ns\":"+std::to_string(m.object_get_ns.load())+",\"object_misses\":"+std::to_string(m.object_misses.load())+
        ",\"meta_get_calls\":"+std::to_string(m.meta_get_calls.load())+",\"meta_get_ns\":"+std::to_string(m.meta_get_ns.load())+
        ",\"object_cache_hits\":"+std::to_string(c.hits)+",\"object_cache_misses\":"+std::to_string(c.misses)+",\"object_cache_charge\":"+std::to_string(c.charge);}
    return result+"}";
}
void LevelStore::verify() const {
    healthy();
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
    need(format==(logged?"FFLDB002":"FFLDB001") && identity.size()==16,"invalid repository format/identity");
    uint64_t roots=0,objects=0;
    std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(options));
    for(it->SeekToFirst();it->Valid();it->Next()) {
        auto key=it->key().ToString(),value=it->value().ToString();
        if(key.starts_with("L/")) {
            need(logged && replay_seq!=UINT64_MAX && key==replay::logkey(replay_seq+1),"replay log gap or unexpected record");
            auto tx=replay::transaction(value);need(tx.sequence==++replay_seq && tx.previous==chain,"replay log chain mismatch");chain=replay::hash(value);
        } else if(key.starts_with("O/")){need(key.size()==34,"invalid object key");need(Journal::object_id(Bytes(value.begin(),value.end()))==id(key.substr(2)),"object checksum mismatch");++objects;}
        else if(key.starts_with("R/")) {
            auto reference=id(value);std::string object;
            status(db_->Get(options,"O/"+bytes(reference),&object),"verify root reference");
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
void verify_recovery_graph(leveldb::DB& db,const leveldb::ReadOptions& options,uint64_t sequence){
    replay::Graph graph([&](const Id& object_id){
        std::string value;status(db.Get(options,"O/"+bytes(object_id),&value),"read recovery object");
        Bytes b(value.begin(),value.end());need(Journal::object_id(b)==object_id,"recovery object checksum mismatch");return b;
    },sequence);
    std::unique_ptr<leveldb::Iterator> it(db.NewIterator(options));
    for(it->Seek("R/");it->Valid() && it->key().starts_with("R/");it->Next())graph.root(it->key().ToString(),it->value().ToString());
    status(it->status(),"scan recovery roots");
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
std::string LevelStore::replay_hash(uint64_t seq) const {
    need(replay_enabled_ && seq>=replay_start_ && seq<=sequence_,"sequence outside retained replay range");
    if(seq==replay_start_)return replay_base_;
    std::string record;need(read(replay::logkey(seq),record),"missing retained replay transaction");
    auto tx=replay::transaction(record);need(tx.sequence==seq,"invalid retained replay sequence");return replay::hash(record);
}
uint64_t LevelStore::backup(const std::string& archive,std::string* chain_hash){
    std::unique_lock lock(writer_mutex_);healthy();
    if(!replay_enabled_){
        std::string entropy(32,0);int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);if(fd<0)throw std::system_error(errno,std::generic_category(),"replay anchor entropy");
        size_t done=0;while(done<entropy.size()){auto n=::read(fd,entropy.data()+done,entropy.size()-done);if(n<0 && errno==EINTR)continue;if(n<=0){close(fd);throw std::runtime_error("replay anchor entropy failed");}done+=n;}close(fd);
        auto anchor=replay::hash("forkfs.replay.anchor.v1"+repository_+integer(sequence_)+entropy);
        leveldb::WriteBatch batch;batch.Put("M/format","FFLDB002");batch.Put("M/replay/start",integer(sequence_));batch.Put("M/replay/base",anchor);batch.Put("M/replay/head",anchor);
        leveldb::WriteOptions write;write.sync=true;fault("replay_enable_before_write");auto s=db_->Write(write,&batch);if(!s.ok()){poisoned_=true;status(s,"enable replay");}fault("replay_enable_after_write");
        replay_enabled_=true;replay_start_=sequence_;replay_base_=anchor;replay_head_=anchor;
    }
    auto release=[this](const leveldb::Snapshot* s){db_->ReleaseSnapshot(s);};
    std::unique_ptr<const leveldb::Snapshot,decltype(release)> snapshot(db_->GetSnapshot(),release);
    leveldb::ReadOptions options;options.verify_checksums=true;options.snapshot=snapshot.get();
    std::string identity;status(db_->Get(options,"M/repository",&identity),"backup identity");
    replay::Header header{'B',identity,sequence_,sequence_,replay_head_,replay_head_};lock.unlock();
    verify_recovery_graph(*db_,options,header.to);
#ifdef FORKFS_TEST_BACKUP
    extern void test_backup_snapshot();test_backup_snapshot();
#endif
    replay::Writer out(archive,header);std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(options));
    for(it->SeekToFirst();it->Valid();it->Next()){
        auto key=it->key().ToString(),value=it->value().ToString();
        if(key.starts_with("L/") || key.starts_with("M/replay/"))continue;
        auto row="B"+replay::le(key.size(),4)+replay::le(value.size(),4)+key+value;
        replay::base_row(row);out.frame(row);
    }
    status(it->status(),"export backup");out.finish();sync_parent(archive);if(chain_hash)*chain_hash=header.head;return header.to;
}
uint64_t LevelStore::export_replay(uint64_t after,const std::string& archive,std::string* chain_hash) const {
    std::unique_lock lock(writer_mutex_);healthy();auto previous=replay_hash(after);
    auto release=[this](const leveldb::Snapshot* s){db_->ReleaseSnapshot(s);};
    std::unique_ptr<const leveldb::Snapshot,decltype(release)> snapshot(db_->GetSnapshot(),release);
    leveldb::ReadOptions options;options.verify_checksums=true;options.snapshot=snapshot.get();
    std::string identity;status(db_->Get(options,"M/repository",&identity),"export replay identity");
    replay::Header header{'I',identity,after,sequence_,previous,replay_head_};lock.unlock();verify_recovery_graph(*db_,options,header.to);replay::Writer out(archive,header);
    uint64_t seq=after;auto head=previous;
    while(seq<header.to){std::string record;status(db_->Get(options,replay::logkey(++seq),&record),"export replay record");
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
        verify_recovery_graph(*recovered.db_,options,recovered.sequence_);
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
            existing.verify();leveldb::ReadOptions options;options.verify_checksums=true;verify_recovery_graph(*existing.db_,options,existing.sequence_);return existing.sequence_;
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
            leveldb::WriteOptions write;write.sync=true;leveldb::WriteBatch batch;size_t pending=0;
            auto flush=[&]{if(pending){status(target.db_->Write(write,&batch),"restore backup rows");batch.Clear();pending=0;fault("replay_base_rows");}};
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
            status(target.db_->Write(write,&batch),"restore backup replay anchor");target.verify();leveldb::ReadOptions options;options.verify_checksums=true;verify_recovery_graph(*target.db_,options,header.to);fault("replay_base_before_publish");
        }
        sync_parent(staging+"/CURRENT");publish_directory(staging,destination);fault("replay_base_after_publish");return header.to;
    }
    struct stat st{};need(lstat(destination.c_str(),&st)==0 && S_ISDIR(st.st_mode),"incremental destination must be an existing repository directory");
    LevelStore target(destination);std::lock_guard lock(target.writer_mutex_);target.healthy();
    if(until)need(target.sequence_<=stop,"destination is ahead of recovery point");
    target.verify();
    leveldb::ReadOptions graph_options;graph_options.verify_checksums=true;verify_recovery_graph(*target.db_,graph_options,target.sequence_);
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
        uint64_t objects=target.objects_,roots=target.roots_;std::set<std::string> new_objects;leveldb::WriteBatch batch;
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
        leveldb::WriteOptions write;write.sync=true;fault("replay_before_write");status(target.db_->Write(write,&batch),"apply replay transaction");fault("replay_after_write");
        target.sequence_=tx.sequence;target.objects_=objects;target.roots_=roots;target.replay_head_=next;
    }
    need(seq==header.to && head==header.head,"incomplete replay range during apply");
    if(until)need(target.sequence_==stop && target.replay_head_==endpoint->second,"recovery point not reached");
    return target.sequence_;
}
}

namespace forkfs {
std::string LevelStore::revision_point(const std::string& name) const {
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
    status(db_->Get(options,"O/"+revision,&descriptor),"read revision recovery descriptor");
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
            auto record=it->value().ToString();auto tx=replay::transaction(record);
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
