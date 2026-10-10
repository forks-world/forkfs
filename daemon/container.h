#pragma once
#include <string>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <set>
#include <thread>
#include <chrono>
#include "journal.h"

namespace forkfs {
// Exclusive container owner; named-object journal is an experimental transaction profile.
class Container {
public:
    static void create(const std::string& path);
    static void create_legacy(const std::string& path);
    enum class OpenMode {ReadWrite,RevisionPreview};
    explicit Container(const std::string& path,OpenMode mode=OpenMode::ReadWrite);
    ~Container();
    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;
    std::string status() const;
    std::string storage_profile() const;
    std::string storage_engine() const;
    void checkpoint();
    void start_compaction();
    void stop_compaction();
    bool wait_compaction(std::chrono::milliseconds timeout=std::chrono::seconds(10));
    std::string compaction_error() const;
    void verify() const;
    void put(const std::string& key, const std::vector<unsigned char>& value);
    void transact(uint64_t expected_sequence, const std::vector<Mutation>& mutations);
    std::vector<std::vector<unsigned char>> get_many(const std::vector<std::string>& keys) const;
    std::vector<unsigned char> get(const std::string& key) const;
private:
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
};
}
