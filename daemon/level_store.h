#pragma once
#include "journal.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <functional>
namespace leveldb {class DB;class Cache;class FilterPolicy;class Env;struct ReadOptions;}
namespace forkfs {
// Repository coordinator: independent ObjectStore plus an unwrapped LevelDB
// metadata index. Sync object data before atomically publishing index/head changes.
struct StoreIoMetrics;
class ObjectStore;
struct RepositoryOwner;
namespace replay {struct Batch;}
struct GcCycle;
class LevelStore {
public:
    static void create(const std::string& path);
    static constexpr size_t default_object_cache_bytes=8*1024*1024;
    explicit LevelStore(const std::string& path,size_t object_cache_bytes=default_object_cache_bytes);
    ~LevelStore();
    void transact(uint64_t expected,const std::vector<Mutation>& changes);
    // Opt-in retained transaction stream. Backup enables it atomically at a base.
    uint64_t backup(const std::string& archive,std::string* chain_hash=nullptr);
    uint64_t export_replay(uint64_t after,const std::string& archive,std::string* chain_hash=nullptr) const;
    struct RecoveryPoint {uint64_t sequence;std::string repository,chain_hash;}; // Hex IDs, as returned by revision_point.
    static uint64_t replay_archive(const std::string& archive,const std::string& destination,
                                   std::optional<RecoveryPoint> until=std::nullopt);

    struct RestoreStep {std::string archive;RecoveryPoint endpoint;};
    static std::vector<RestoreStep> plan_restore(const std::vector<std::string>& archives,const RecoveryPoint& target);
    static uint64_t restore(const std::vector<std::string>& archives,const std::string& destination,const RecoveryPoint& target,
                            const std::function<void(const std::string&)>& verify_namespace);

    std::vector<unsigned char> object(const std::array<unsigned char,32>& id) const;
    std::array<unsigned char,32> root(const std::string& key) const;
    bool contains(const std::string& key) const;
    std::vector<std::string> keys(const std::string& prefix) const;
    void verify() const;
    void compact();
    std::pair<uint64_t,uint64_t> space() const;
    // Offline, exclusive repository generation rewrite. A checkpoint explicitly
    // starts a new replay lineage; default GC retains all replay dependencies.
    static std::string collect(const std::string& path,const std::string& checkpoint="",bool dry_run=false);
    std::shared_ptr<void> pin_objects() const;
    std::shared_ptr<void> pin_manifest(const std::array<unsigned char,32>& id) const;
    std::string collect_step(size_t max_copy_bytes=1024*1024,size_t max_objects=128,
                             size_t max_metadata_bytes=64*1024*1024,size_t mark_work=128);
    void cancel_collection();
    void barrier() const;
    struct CacheStats {uint64_t hits,misses;size_t charge;};
    CacheStats cache_stats() const;
    std::string metrics_json(bool detailed=false) const;
    uint64_t sequence() const {return sequence_;}
    size_t objects() const {return objects_;}
    size_t roots() const {return roots_;}
    std::string revision_point(const std::string& name) const;
    std::string repository() const {return repository_;}
    void poison() {poisoned_=true;}
private:
    LevelStore(const std::string& path,bool initialize,size_t object_cache_bytes);
    bool read(const std::string& key,std::string& value) const;
    void healthy() const;
    std::string logical_value(const leveldb::ReadOptions&,const std::string& key,const std::string& physical) const;
    std::string object_payload(const leveldb::ReadOptions&,const std::string& id) const;
    void publish(const replay::Batch& batch);
    void verify_recovery_graph(const leveldb::ReadOptions&,uint64_t sequence) const;
    std::atomic<unsigned> physical_format_=5;
    std::shared_ptr<RepositoryOwner> owner_;
    std::unique_ptr<ObjectStore> object_store_;
    // Serializes publication, including root validation and durable DB::Write.
    mutable std::mutex writer_mutex_;
    struct RevisionIndex {
        uint64_t start=0,through=0,event=0;
        std::string anchor,head,event_head;
        unsigned matches=0;
    };
    mutable std::mutex revision_index_mutex_;
    mutable std::map<std::pair<std::string,std::string>,RevisionIndex> revision_index_;
    struct RootFact {bool found;std::string value;};
    mutable std::mutex root_cache_mutex_;
    mutable uint64_t root_cache_sequence_=UINT64_MAX;
    mutable std::map<std::string,RootFact> root_cache_;
    std::atomic<uint64_t> sequence_=0,objects_=0,roots_=0;
    std::atomic<bool> poisoned_=false;
    struct PinState {
        mutable std::mutex mutex; size_t logical=0,locations=0;
        std::map<std::array<unsigned char,32>,size_t> manifests;
        uint64_t manifest_epoch=0;
    };
    struct LocationRelease { void operator()(PinState* state) const noexcept; };
    using LocationPin=std::unique_ptr<PinState,LocationRelease>;
    std::shared_ptr<PinState> pin_state_=std::make_shared<PinState>();
    LocationPin pin_locations() const;
    mutable std::mutex gc_mutex_;
    mutable std::shared_ptr<GcCycle> gc_cycle_; // publication and lifecycle guarded by writer_mutex_
    mutable std::string gc_error_;
    std::string path_;
    std::string repository_;
    bool replay_enabled_=false;
    uint64_t replay_start_=0;
    std::string replay_base_,replay_head_;
    std::string replay_hash(uint64_t sequence) const;
    std::unique_ptr<leveldb::Cache> cache_;
    std::unique_ptr<const leveldb::FilterPolicy> filter_;
    std::unique_ptr<StoreIoMetrics> metrics_;
    std::unique_ptr<leveldb::Env> env_;
    std::unique_ptr<leveldb::DB> db_;
};
}
