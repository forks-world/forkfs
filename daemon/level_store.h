#pragma once
#include "journal.h"
#include <atomic>
#include <memory>
#include <mutex>
namespace leveldb {class DB;class Cache;class FilterPolicy;class Env;}
namespace forkfs {
// One upstream LevelDB per repository. Objects are immutable; heads and the
// sequence/counts are changed atomically with the same synchronous WriteBatch.
struct StoreIoMetrics;
class LevelStore {
public:
    static void create(const std::string& path);
    static constexpr size_t default_object_cache_bytes=8*1024*1024;
    explicit LevelStore(const std::string& path,size_t object_cache_bytes=default_object_cache_bytes);
    ~LevelStore();
    void transact(uint64_t expected,const std::vector<Mutation>& changes);
    std::vector<unsigned char> object(const std::array<unsigned char,32>& id) const;
    std::array<unsigned char,32> root(const std::string& key) const;
    bool contains(const std::string& key) const;
    std::vector<std::string> keys(const std::string& prefix) const;
    void verify() const;
    void compact();
    struct CacheStats {uint64_t hits,misses;size_t charge;};
    CacheStats cache_stats() const;
    std::string metrics_json(bool detailed=false) const;
    uint64_t sequence() const {return sequence_;}
    size_t objects() const {return objects_;}
    size_t roots() const {return roots_;}
    std::string repository() const {return repository_;}
    void poison() {poisoned_=true;}
private:
    LevelStore(const std::string& path,bool initialize,size_t object_cache_bytes);
    bool read(const std::string& key,std::string& value) const;
    void healthy() const;
    // Serializes publication, including root validation and durable DB::Write.
    mutable std::mutex writer_mutex_;
    struct RootFact {bool found;std::string value;};
    mutable std::mutex root_cache_mutex_;
    mutable uint64_t root_cache_sequence_=UINT64_MAX;
    mutable std::map<std::string,RootFact> root_cache_;
    std::atomic<uint64_t> sequence_=0,objects_=0,roots_=0;
    std::atomic<bool> poisoned_=false;
    std::string repository_;
    std::unique_ptr<leveldb::Cache> cache_;
    std::unique_ptr<leveldb::Cache> object_cache_;
    size_t object_cache_bytes_;
    mutable std::atomic<uint64_t> cache_hits_=0,cache_misses_=0;
    std::unique_ptr<const leveldb::FilterPolicy> filter_;
    std::unique_ptr<StoreIoMetrics> metrics_;
    std::unique_ptr<leveldb::Env> env_;
    std::unique_ptr<leveldb::DB> db_;
};
}
