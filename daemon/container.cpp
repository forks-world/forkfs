#include "container.h"
#include "level_store.h"
#include "replay_format.h"
#include "namespace.h"
#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace forkfs {
namespace {
void backup_fault(const char* stage) {
#ifdef FORKFS_TEST_FAULTS
    const char* selected=std::getenv("FORKFS_TEST_CRASH");
    if(selected && !strcmp(selected,stage))_exit(86);
#else
    (void)stage;
#endif
}
using Anchor = std::array<unsigned char, 4096>;
struct Fd {
    int n;
    ~Fd() { if (n >= 0) close(n); }
};
[[noreturn]] void fail(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
uint64_t decode_le(const unsigned char* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= uint64_t(p[i]) << (8*i);
    return v;
}
void encode_le(unsigned char* p, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<unsigned char>(v >> (8*i));
}
uint32_t crc(const unsigned char* p, size_t n) {
    uint32_t v = 0xffffffff;
    for (size_t i = 0; i < n; ++i) {
        v ^= p[i];
        for (int b = 0; b < 8; ++b) v = (v >> 1) ^ ((v & 1) ? 0x82f63b78 : 0);
    }
    return v ^ 0xffffffff;
}
void read_at(int fd, void* data, size_t n, off_t offset) {
    auto* p = static_cast<unsigned char*>(data);
    while (n) {
        auto r = pread(fd, p, n, offset);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) fail("pread");
        require(r != 0, "truncated container");
        p += r; n -= r; offset += r;
    }
}
void write_at(int fd, const void* data, size_t n, off_t offset) {
    auto* p = static_cast<const unsigned char*>(data);
    while (n) {
        auto r = pwrite(fd, p, n, offset);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) fail("pwrite");
        require(r != 0, "zero length write");
        p += r; n -= r; offset += r;
    }
}
void durable(int fd) {
    if (fsync(fd) < 0) fail("fsync");
#ifdef __APPLE__
    if (fcntl(fd, F_FULLFSYNC) < 0) fail("F_FULLFSYNC");
#endif
}
std::string json_string(const std::string& value) {
    std::string result="\"";const char* hex="0123456789abcdef";
    for(unsigned char c:value) {
        if(c=='"' || c=='\\'){result+='\\';result+=static_cast<char>(c);}
        else if(c<32){result+="\\u00";result+=hex[c>>4];result+=hex[c&15];}
        else result+=static_cast<char>(c);
    }
    return result+'"';
}
bool valid(const Anchor& a) {
    return !memcmp(a.data(), "FORKFS00", 8) &&
        decode_le(a.data()+4092, 4) == crc(a.data(), 4092);
}
void supported(const Anchor& a) {
    require(decode_le(a.data()+8, 2) == 0 && decode_le(a.data()+10, 2) == 0,
            "unsupported container version");
    require(decode_le(a.data()+12, 4) == 4096, "unsupported anchor size");
    require(decode_le(a.data()+40, 8) == 0 && decode_le(a.data()+48, 8) == 0,
            "unsupported container features");
    require(decode_le(a.data()+56, 8) == 16*1024*1024, "unsupported segment span");
    if (decode_le(a.data()+64,8)==0) {
        for (size_t i=64;i<120;++i) require(a[i]==0,"invalid empty checkpoint reference");
        require(decode_le(a.data()+120,8)==65536,"invalid initial replay offset");
    }
    for (size_t i = 136; i < 4092; ++i)
        require(a[i] == 0, "nonzero reserved anchor field");
    bool nonzero = false;
    for (size_t i = 16; i < 32; ++i) nonzero |= a[i] != 0;
    require(nonzero, "invalid repository identity");
}
}

void Container::create(const std::string& path) {
#ifdef FORKFS_LEGACY_DEFAULT
    create_legacy(path);
#else
    LevelStore::create(path);
#endif
}
void Container::create_legacy(const std::string& path) {
    // O_EXCL prevents destructive reinitialization, including through symlinks.
    Fd fd{open(path.c_str(), O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW, 0600)};
    if (fd.n < 0) fail("create container");
    if (flock(fd.n, LOCK_EX|LOCK_NB) < 0) fail("lock container");
    std::array<unsigned char, 65536> header{};
    Anchor a{};
    memcpy(a.data(), "FORKFS00", 8);
    encode_le(a.data()+12, 4096, 4);
    Fd random{open("/dev/urandom", O_RDONLY|O_CLOEXEC)};
    if (random.n < 0) fail("open entropy source");
    size_t done = 0;
    while (done != 16) {
        auto n = read(random.n, a.data()+16+done, 16-done);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) fail("read entropy");
        require(n != 0, "entropy source ended");
        done += n;
    }
    encode_le(a.data()+32, 1, 8);
    encode_le(a.data()+56, 16*1024*1024, 8);
    encode_le(a.data()+120, 65536, 8);
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    encode_le(a.data()+128, static_cast<uint64_t>(now), 8);
    encode_le(a.data()+4092, crc(a.data(), 4092), 4);
    memcpy(header.data(), a.data(), a.size());
    memcpy(header.data()+4096, a.data(), a.size());
    write_at(fd.n, header.data(), header.size(), 0);
    durable(fd.n);
    auto parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) parent = ".";
    Fd dir{open(parent.c_str(), O_RDONLY|O_DIRECTORY|O_CLOEXEC)};
    if (dir.n < 0) fail("open parent directory");
    if (fsync(dir.n) < 0) fail("sync parent directory");
    // On error the incomplete file remains for inspection; never remove a path
    // that another process might have replaced while initialization was running.
}

Container::Container(const std::string& path,OpenMode mode):mode_(mode) {
    struct stat backing{};
    if(lstat(path.c_str(),&backing)==0 && S_ISDIR(backing.st_mode)) {
        journal_=std::make_unique<Journal>(path,mode==OpenMode::RevisionPreview);id_=journal_->repository();
        backup_directory_=std::filesystem::absolute(path).string();
        if(mode==OpenMode::ReadWrite)Namespace::recover_orphans(*this);return;
    }
    require(mode==OpenMode::ReadWrite,"revision preview requires LevelDB");
    Fd fd{open(path.c_str(), O_RDWR|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};
    if (fd.n < 0) fail("open container");
    struct stat st{};
    if (fstat(fd.n, &st) < 0) fail("stat container");
    require(S_ISREG(st.st_mode), "container must be a regular file");
    if (flock(fd.n, LOCK_EX|LOCK_NB) < 0) fail("container already owned");
    require(st.st_size >= 65536, "truncated container header");
    Anchor a{}, b{};
    read_at(fd.n, a.data(), a.size(), 0);
    read_at(fd.n, b.data(), b.size(), 4096);
    bool va = valid(a), vb = valid(b);
    require(va || vb, "no valid container anchor");
    // A checksum-valid unsupported header is never silently downgraded.
    if (va) supported(a);
    if (vb) supported(b);
    if (va && vb) {
        require(!memcmp(a.data()+16, b.data()+16, 16), "anchor identity mismatch");
        if (decode_le(a.data()+32,8) == decode_le(b.data()+32,8))
            require(a == b, "conflicting anchors at the same generation");
    }
    const auto& chosen = !vb || (va && decode_le(a.data()+32,8) >= decode_le(b.data()+32,8)) ? a : b;
    std::array<unsigned char, 57344> reserved{};
    read_at(fd.n, reserved.data(), reserved.size(), 8192);
    for (auto c : reserved) require(c == 0, "nonzero reserved container region");
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 16; i < 32; ++i) {
        id_ += hex[chosen[i] >> 4]; id_ += hex[chosen[i] & 15];
    }
    degraded_ = !va || !vb;
    std::array<unsigned char,16> repository{};
    std::copy_n(chosen.data()+16,16,repository.begin());
    Checkpoint checkpoint;
    checkpoint.offset=decode_le(chosen.data()+64,8);
    checkpoint.length=decode_le(chosen.data()+72,8);
    checkpoint.sequence=decode_le(chosen.data()+80,8);
    std::copy_n(chosen.data()+88,32,checkpoint.hash.begin());
    checkpoint.replay_start=decode_le(chosen.data()+120,8);
    journal_ = std::make_unique<Journal>(fd.n,repository,checkpoint);
    anchor_=chosen;
    anchor_offset_=(&chosen==&a)?0:4096;
    fd_ = fd.n; fd.n = -1;
}
Container::~Container() { stop_backup(); stop_compaction(); if (fd_ >= 0) close(fd_); }
void Container::put(const std::string& key,const std::vector<unsigned char>& value) {
    require(!key.starts_with(std::string("\0forkfs/",8)) && !key.starts_with(std::string("\0forkfs-history/",16)),"reserved filesystem key");
    std::lock_guard lock(mutex_);
    journal_->put(key,value);
}
std::vector<unsigned char> Container::get(const std::string& key) const {
    std::lock_guard lock(mutex_); return journal_->get(key);
}
void Container::transact(uint64_t expected,const std::vector<Mutation>& mutations) {
    for(const auto& m:mutations)
        require(!m.key.starts_with(std::string("\0forkfs/",8)) && !m.key.starts_with(std::string("\0forkfs-history/",16)),"reserved filesystem key");
    std::lock_guard lock(mutex_); journal_->transact(expected,mutations);
}
std::vector<std::vector<unsigned char>> Container::get_many(const std::vector<std::string>& keys) const {
    std::lock_guard lock(mutex_);
    std::vector<std::vector<unsigned char>> result;
    for(const auto& key:keys) result.push_back(journal_->get(key));
    return result;
}
void Container::verify() const { std::lock_guard lock(mutex_); journal_->verify(); }
void Container::checkpoint() {
    std::lock_guard lock(mutex_);
    if(journal_->is_leveldb()){journal_->compact();return;}
    const auto generation=decode_le(anchor_.data()+32,8);
    require(generation!=UINT64_MAX,"anchor generation exhausted");
    auto c=journal_->checkpoint();
    auto next=anchor_;
    encode_le(next.data()+32,generation+1,8);
    encode_le(next.data()+64,c.offset,8); encode_le(next.data()+72,c.length,8);
    encode_le(next.data()+80,c.sequence,8); memcpy(next.data()+88,c.hash.data(),32);
    encode_le(next.data()+120,c.replay_start,8);
    encode_le(next.data()+4092,crc(next.data(),4092),4);
    auto target=anchor_offset_^4096;
    try {
        write_at(fd_,next.data(),next.size(),target);
#ifdef FORKFS_TEST_FAULTS
        const char* crash=std::getenv("FORKFS_TEST_CRASH");
        if(crash && !strcmp(crash,"anchor_written")) _exit(86);
#endif
        durable(fd_);
#ifdef FORKFS_TEST_FAULTS
        if(crash && !strcmp(crash,"anchor_synced")) _exit(86);
#endif
        anchor_=next; anchor_offset_=target; degraded_=false;
    } catch(...) { journal_->poison(); throw; }
}
std::string Container::storage_engine() const {
    std::lock_guard lock(mutex_);return journal_->is_leveldb()?"leveldb":"legacy";
}
std::string Container::storage_profile() const {
    std::lock_guard lock(mutex_);return "{\"metadata_prepare_ns\":"+std::to_string(metadata_prepare_ns_)+",\"storage_metrics\":"+journal_->metrics_json(true)+"}\n";
}
std::string Container::status() const {
    std::lock_guard lock(mutex_);
    return "{\"service\":\"forkfsd\",\"state\":\"" +
        (journal_->is_leveldb()?std::string("leveldb"):std::string("object-journal")) + "\",\"storage_engine\":\"" +
        (journal_->is_leveldb()?std::string("leveldb"):std::string("legacy")) + "\",\"repository_id\":\"" + id_ +
        "\",\"degraded_anchor\":" + (degraded_ ? "true" : "false") +
        ",\"mounted\":false,\"transactions_supported\":true,\"last_committed_seq\":" +
        std::to_string(journal_->sequence()) + ",\"checkpoint_seq\":" +
        std::to_string(decode_le(anchor_.data()+80,8)) + ",\"replayed_transactions\":" +
        std::to_string(journal_->replayed()) + ",\"indexed_objects\":" +
        std::to_string(journal_->objects()) + ",\"object_roots\":" + std::to_string(journal_->count()) +
        ",\"compaction_running\":" + (compaction_running_ ? "true" : "false") +
        ",\"compaction_queued\":" + std::to_string(compaction_queue_.size()) +
        ",\"compaction_active\":" + (compaction_active_ ? "true" : "false") +
        ",\"compaction_completed\":" + std::to_string(compaction_completed_) +
        ",\"compaction_races\":" + std::to_string(compaction_races_) +
        ",\"compaction_failures\":" + std::to_string(compaction_failures_) +
        ",\"compaction_last_error\":" + json_string(compaction_error_) +
        ",\"metadata_prepare_ns\":" + std::to_string(metadata_prepare_ns_) +
        ",\"storage_metrics\":" + journal_->metrics_json() +
        ",\"uncommitted_tail\":" + (journal_->pending_tail() ? "true" : "false") + "}\n";
}
}

namespace forkfs {
std::string Container::backup_status_locked(const std::string& request_id) const {
    auto i=backup_tasks_.find(request_id);
    require(i!=backup_tasks_.end(),"unknown backup request");
    const auto& task=i->second;
    std::string chain;constexpr char digits[]="0123456789abcdef";
    for(unsigned char c:task.chain_hash){chain+=digits[c>>4];chain+=digits[c&15];}
    std::string checkpoint=task.state=="durable"?
        "{\"repository\":"+json_string(id_)+",\"sequence\":"+std::to_string(task.sequence)+",\"chain_hash\":"+json_string(chain)+"}":"null";
    return "{\"request_id\":"+json_string(request_id)+",\"state\":"+json_string(task.state)+
        ",\"archive\":"+json_string(task.archive)+",\"kind\":"+json_string(task.after?"incremental":"base")+
        ",\"sequence\":"+(task.state=="durable"?std::to_string(task.sequence):"null")+
        ",\"checkpoint\":"+checkpoint+",\"error\":"+json_string(task.error)+"}\n";
}
std::string Container::backup_status(const std::string& request_id) {
    std::lock_guard lock(backup_mutex_);load_backup_tasks_locked();return backup_status_locked(request_id);
}
std::string Container::start_backup(const std::string& request_id,const std::string& archive,
                                    std::optional<uint64_t> after) {
    require(mode_==OpenMode::ReadWrite && journal_->is_leveldb(),"backup requires writable LevelDB owner");
    require(!request_id.empty() && request_id.size()<=128,"invalid backup request ID");
    require(request_id.find('\0')==std::string::npos && archive.find('\0')==std::string::npos,"NUL in backup argument");
    require(archive.size()<=4096,"backup archive path too long");
    require(std::filesystem::path(archive).is_absolute(),"backup archive path must be absolute");
    std::lock_guard lifecycle(backup_lifecycle_mutex_);
    {
        std::lock_guard lock(backup_mutex_);
        load_backup_tasks_locked();
        require(backup_catalog_error_.empty(),"backup catalog persistence failed; restart and inspect task state");
        auto i=backup_tasks_.find(request_id);
        if(i!=backup_tasks_.end()) {
            require(i->second.archive==archive && i->second.after==after,"backup request ID reused with different arguments");
            return backup_status_locked(request_id);
        }
        require(!backup_active_,"another backup export is running");
        require(backup_tasks_.size()<128,"backup task history full; forget a terminal task before starting another");
    }
    // A finished worker is joined without holding the mutex it uses to publish.
    if(backup_thread_.joinable())backup_thread_.join();
    std::lock_guard lock(backup_mutex_);
    backup_tasks_.emplace(request_id,BackupTask{archive,"running",{},{},after,0});
    try {save_backup_tasks_locked();}
    catch(const std::exception& e){backup_catalog_error_=e.what();auto& task=backup_tasks_.at(request_id);task.state="interrupted";task.error="task intent persistence failed";throw;}
    backup_fault("backup_task_after_intent");
    backup_active_=true;
    try {
        backup_thread_=std::thread([this,request_id,archive,after]{
            uint64_t sequence=0;std::string error,chain_hash;
            try {sequence=after?journal_->export_replay(*after,archive,&chain_hash):journal_->backup(archive,&chain_hash);backup_fault("backup_task_after_archive");}
            catch(const std::exception& e){error=e.what();}
            catch(...){error="unknown backup export failure";}
            std::lock_guard done(backup_mutex_);
            auto& task=backup_tasks_.at(request_id);task.sequence=sequence;
            task.state=error.empty()?"durable":"failed";task.error=std::move(error);task.chain_hash=std::move(chain_hash);
            try {save_backup_tasks_locked();backup_fault("backup_task_after_completion");}
            catch(const std::exception& e){backup_catalog_error_=e.what();task.state="interrupted";task.error="completion persistence failed: "+backup_catalog_error_;}
            backup_active_=false;
        });
    } catch(...) {
        backup_active_=false;auto& task=backup_tasks_.at(request_id);task.state="failed";task.error="cannot start backup worker";
        try {save_backup_tasks_locked();}catch(const std::exception& e){backup_catalog_error_=e.what();}
        throw;
    }
    return backup_status_locked(request_id);
}
void Container::stop_backup() {
    std::lock_guard lifecycle(backup_lifecycle_mutex_);
    if(backup_thread_.joinable())backup_thread_.join();
}
}

namespace forkfs {
void Container::load_backup_tasks_locked() {
    if(backup_tasks_loaded_)return;
    require(!backup_directory_.empty(),"backup tasks require LevelDB");
    Fd fd{open((backup_directory_+"/BACKUP-TASKS").c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};
    if(fd.n<0){if(errno==ENOENT){backup_tasks_loaded_=true;return;}fail("open backup task catalog");}
    struct stat st{};if(fstat(fd.n,&st)<0)fail("stat backup task catalog");
    require(S_ISREG(st.st_mode) && st.st_size>=76 && st.st_size<=2*1024*1024,"invalid backup task catalog size/type");
    std::string bytes(static_cast<size_t>(st.st_size),0);read_at(fd.n,bytes.data(),bytes.size(),0);
    auto content=bytes.substr(0,bytes.size()-32);
    require(replay::hash(content)==bytes.substr(bytes.size()-32),"backup task catalog checksum mismatch");
    replay::Input input{content};require(input.take(8)=="FFBKT001","unsupported backup task catalog");
    require(input.take(32)==id_,"backup task catalog repository mismatch");
    auto count=input.integer(4);require(count<=128,"too many backup tasks");
    auto text=[&](size_t limit){auto size=input.integer(4);require(size<=limit,"oversized backup task field");return input.take(size);};
    std::map<std::string,BackupTask> tasks;
    for(uint64_t n=0;n<count;++n){
        auto id=text(128);BackupTask task;
        task.archive=text(4096);task.state=text(16);task.error=text(8192);task.chain_hash=text(32);
        auto incremental=input.integer(1),after=input.integer(8);task.sequence=input.integer(8);
        require(!id.empty() && id.find('\0')==std::string::npos,"invalid persisted backup ID");
        require(std::filesystem::path(task.archive).is_absolute() && task.archive.find('\0')==std::string::npos,"invalid persisted backup path");
        require(incremental<=1 && (incremental || after==0),"invalid persisted backup range");
        if(incremental)task.after=after;
        require(task.state=="running" || task.state=="durable" || task.state=="failed" || task.state=="interrupted","invalid persisted backup state");
        require(task.state!="durable" || (task.chain_hash.size()==32 && (!task.after || task.sequence>=*task.after)),"invalid persisted checkpoint");
        if(task.state=="running"){task.state="interrupted";task.error="daemon stopped before completion was durably recorded; inspect archive, do not overwrite";}
        require(tasks.emplace(id,std::move(task)).second,"duplicate persisted backup ID");
    }
    input.end();backup_tasks_=std::move(tasks);backup_tasks_loaded_=true;
}
void Container::save_backup_tasks_locked() {
    require(mode_==OpenMode::ReadWrite,"cannot persist backup tasks in preview mode");
    std::string content="FFBKT001"+id_+replay::le(backup_tasks_.size(),4);
    auto text=[&](const std::string& value){content+=replay::le(value.size(),4);content+=value;};
    for(const auto& [id,task]:backup_tasks_){
        text(id);text(task.archive);text(task.state);text(task.error.substr(0,8192));text(task.chain_hash);
        content+=char(task.after.has_value());content+=replay::le(task.after.value_or(0));content+=replay::le(task.sequence);
    }
    content+=replay::hash(content);
    require(content.size()<=2*1024*1024,"backup task catalog too large");
    auto temporary=backup_directory_+"/.backup-tasks-XXXXXX";
    Fd fd{mkstemp(temporary.data())};if(fd.n<0)fail("create backup task catalog");
    struct Cleanup {const std::string& path;~Cleanup(){unlink(path.c_str());}} cleanup{temporary};
    if(fcntl(fd.n,F_SETFD,FD_CLOEXEC)<0)fail("protect backup catalog fd");
    write_at(fd.n,content.data(),content.size(),0);durable(fd.n);
    if(rename(temporary.c_str(),(backup_directory_+"/BACKUP-TASKS").c_str())<0)fail("publish backup task catalog");
    Fd dir{open(backup_directory_.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC)};
    if(dir.n<0)fail("open backup task directory");
    if(fsync(dir.n)<0)fail("sync backup task directory");
}
void Container::forget_backup(const std::string& request_id) {
    require(mode_==OpenMode::ReadWrite,"cannot forget backup task in preview mode");
    std::lock_guard lifecycle(backup_lifecycle_mutex_);
    std::lock_guard lock(backup_mutex_);load_backup_tasks_locked();
    require(backup_catalog_error_.empty(),"backup catalog persistence failed; restart and inspect task state");
    auto i=backup_tasks_.find(request_id);require(i!=backup_tasks_.end(),"unknown backup request");
    require(i->second.state!="running","cannot forget running backup task");
    auto saved=backup_tasks_.extract(i);
    try {save_backup_tasks_locked();}
    catch(const std::exception& e){backup_tasks_.insert(std::move(saved));backup_catalog_error_=e.what();throw;}
}
}
