#include "level_store.h"
#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/env.h>
#include <chrono>
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
    std::string value;need(read("M/format",value) && value=="FFLDB001","unsupported LevelDB repository format");
    need(read("M/repository",value) && value.size()==16,"invalid repository identity");
    const char* hex="0123456789abcdef";for(unsigned char c:value){repository_+=hex[c>>4];repository_+=hex[c&15];}
    need(read("M/sequence",value),"missing sequence");sequence_=number(value);
    need(read("M/objects",value),"missing object count");objects_=number(value);
    need(read("M/roots",value),"missing root count");roots_=number(value);
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
    std::lock_guard lock(mutex_);auto begin=Clock::now();healthy();need(expected==sequence_,"transaction conflict: sequence changed");
    need(sequence_!=UINT64_MAX,"transaction sequence exhausted");need(!changes.empty() && changes.size()<=4096,"LevelDB batch requires 1..4096 mutations");
    // transact is synchronous: payload references remain valid until DB::Write
    // has copied the batch. Async queues must own their mutation buffers.
    std::set<std::string> seen;std::map<Id,const Bytes*> values;std::map<std::string,std::optional<Id>> updates;
    size_t total=0;uint64_t roots=roots_,objects=objects_;
    for(const auto& m:changes) {
        need(!m.key.empty() && m.key.size()<=1024 && seen.insert(m.key).second,"invalid or duplicate root key");
        need(!(m.value && m.reference),"mutation has both value and reference");
        bool exists=contains(m.key);
        if(m.reference){object(*m.reference);updates[m.key]=*m.reference;if(!exists)++roots;}
        else if(m.value) {
            need(m.value->size()<=256*1024,"object exceeds 256 KiB");auto hash=Journal::object_id(*m.value);
            if(!values.contains(hash)){total+=m.value->size();values.emplace(hash,&*m.value);}
            need(total<=16*1024*1024,"LevelDB batch exceeds 16 MiB");updates[m.key]=hash;if(!exists)++roots;
        } else {need(exists,"cannot delete absent root");updates[m.key]=std::nullopt;--roots;}
    }
    leveldb::WriteBatch batch;
    for(const auto& [hash,value]:values) {
        std::string previous;
        if(read("O/"+bytes(hash),previous))need(previous.size()==value->size() && (previous.empty() || !memcmp(previous.data(),value->data(),value->size())),"immutable object collision/corruption");
        else {batch.Put("O/"+bytes(hash),leveldb::Slice(reinterpret_cast<const char*>(value->data()),value->size()));++objects;}
    }
    for(const auto& [key,hash]:updates){if(hash)batch.Put("R/"+key,bytes(*hash));else batch.Delete("R/"+key);}
    batch.Put("M/sequence",integer(sequence_+1));batch.Put("M/objects",integer(objects));batch.Put("M/roots",integer(roots));
    fault("leveldb_before_write");leveldb::WriteOptions options;options.sync=true;
    metrics_->prepare_ns+=elapsed(begin);auto write_begin=Clock::now();
    auto s=db_->Write(options,&batch);metrics_->write_ns+=elapsed(write_begin);if(!s.ok()){poisoned_=true;status(s,"commit LevelDB batch");}
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
    std::lock_guard lock(mutex_);healthy();uint64_t roots=0,objects=0;
    leveldb::ReadOptions options;options.verify_checksums=true;std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(options));
    for(it->SeekToFirst();it->Valid();it->Next()) {
        auto key=it->key().ToString(),value=it->value().ToString();
        if(key.starts_with("O/")){need(key.size()==34,"invalid object key");need(Journal::object_id(Bytes(value.begin(),value.end()))==id(key.substr(2)),"object checksum mismatch");++objects;}
        else if(key.starts_with("R/")){object(id(value));++roots;}
        else need(key=="M/format" || key=="M/repository" || key=="M/sequence" || key=="M/objects" || key=="M/roots","unknown metadata key");
    }
    status(it->status(),"verify LevelDB");need(roots==roots_ && objects==objects_,"LevelDB count mismatch");
}
void LevelStore::compact() {{std::lock_guard lock(mutex_);healthy();db_->CompactRange(nullptr,nullptr);}verify();}
}
