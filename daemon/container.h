#pragma once
#include <string>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <set>
#include <thread>
#include <chrono>
#include <atomic>
#include "journal.h"

namespace forkfs {
class RevisionDag;
struct MountRegistration {std::string view,generation,token;bool read_only=false,revision=false;};
// Exclusive container owner; named-object journal is an experimental transaction profile.
class Container {
public:
    static void create(const std::string& path);
    static void create_legacy(const std::string& path);
    enum class OpenMode {ReadWrite,RevisionPreview};
    explicit Container(const std::string& path,OpenMode mode=OpenMode::ReadWrite);
    ~Container();
    OpenMode open_mode() const noexcept {return mode_;}
    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;
    std::string status() const;
    std::string storage_profile() const;
    std::string storage_engine() const;
    void checkpoint();
    std::pair<uint64_t,uint64_t> storage_space() const;
    // Persistent request IDs. One background export; interrupted work is not retried.
    std::string start_backup(const std::string& request_id,const std::string& archive,
                             std::optional<uint64_t> after=std::nullopt,const std::string& catalog="",bool retry_catalog=false);
    std::string retry_backup_catalog(const std::string& request_id);
    std::string backup_status(const std::string& request_id);
    void forget_backup(const std::string& request_id);
    void start_compaction();
    void stop_compaction();
    bool wait_compaction(std::chrono::milliseconds timeout=std::chrono::seconds(10));
    std::string compaction_error() const;
    std::string collect_step(size_t max_copy_bytes=1024*1024,size_t max_objects=128,size_t max_metadata_bytes=64*1024*1024);
    std::string start_collection(size_t max_copy_bytes=1024*1024,size_t max_objects=128,size_t max_steps=64,size_t max_metadata_bytes=64*1024*1024);
    std::string collection_status() const;
    void stop_collection();
    void sync() const;
    void verify() const;
    void put(const std::string& key, const std::vector<unsigned char>& value);
    void transact(uint64_t expected_sequence, const std::vector<Mutation>& mutations);
    std::vector<std::vector<unsigned char>> get_many(const std::vector<std::string>& keys) const;
    std::vector<unsigned char> get(const std::string& key) const;
    void mount_register(const std::string& view,const std::string& generation,const std::string& token,bool read_only,bool revision=false);
    MountRegistration mount_check(const std::string& token,const std::string& generation) const;
    std::vector<MountRegistration> mount_list() const;
    void mount_retire(const std::string& token,const std::string& old_generation);
private:
    struct BackupTask {
        std::string archive,state="running",error;
        std::string chain_hash;
        std::optional<uint64_t> after;
        uint64_t sequence=0;
        std::string catalog;
        bool archive_durable=false;
    };
    mutable std::mutex collection_mutex_;
    std::mutex collection_lifecycle_mutex_;
    std::thread collection_thread_;
    std::atomic<bool> collection_stop_=false;
    bool collection_active_=false;
    uint64_t collection_steps_=0;
    std::string collection_state_="idle",collection_last_="null",collection_error_;
    std::string backup_status_locked(const std::string& request_id) const;
    void stop_backup();
    void load_backup_tasks_locked();
    void save_backup_tasks_locked();
    bool backup_tasks_loaded_=false;
    std::string backup_directory_,backup_catalog_error_;
    mutable std::mutex backup_mutex_;
    std::mutex backup_lifecycle_mutex_;
    std::map<std::string,BackupTask> backup_tasks_;
    std::thread backup_thread_;
    bool backup_active_=false;
    const OpenMode mode_;
    uint64_t metadata_prepare_ns_=0;
    friend class Namespace;
    std::shared_ptr<const int> lifetime_=std::make_shared<const int>(0);
    friend class Compaction;
    struct LockRange {uint64_t start,end;unsigned char mode;};
    struct Cursor {uint64_t position=0;std::vector<LockRange> locks;};
    struct OpenHandle {std::string view,inode;bool revision,readable,writable,append;std::shared_ptr<Cursor> cursor;};
    std::map<std::string,OpenHandle> handles_;
    std::map<std::pair<std::string,std::string>,size_t> open_counts_;
    void schedule_compaction_locked(const std::string& world);
    void compaction_loop();
    std::mutex compaction_lifecycle_mutex_;
    std::condition_variable compaction_cv_;
    std::thread compaction_thread_;
    bool compaction_running_=false,compaction_stop_=false,compaction_active_=false;
    std::deque<std::string> compaction_queue_;
    std::set<std::string> compaction_pending_;
    uint64_t compaction_completed_=0,compaction_races_=0,compaction_failures_=0;
    std::string compaction_error_;
    std::map<std::string,uint64_t> compaction_world_failures_;
    mutable std::mutex mutex_;
    int fd_ = -1;
    std::string id_;
    bool degraded_ = false;
    std::array<unsigned char,4096> anchor_{};
    uint64_t anchor_offset_ = 0;
    std::unique_ptr<Journal> journal_;
    // Immutable ancestry cache, accessed under mutex_; discarded on reopen.
    std::unique_ptr<RevisionDag> revision_dag_;
};
}
