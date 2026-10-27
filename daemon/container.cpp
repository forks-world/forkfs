#include "container.h"
#include "revision.h"
#include "level_store.h"
#include "replay_format.h"
#include "namespace.h"
#include "backup_catalog.h"
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
Container::~Container() { stop_collection(); stop_backup(); stop_compaction(); if (fd_ >= 0) close(fd_); }
void Container::put(const std::string& key,const std::vector<unsigned char>& value) {
    require(!key.starts_with(std::string("\0forkfs/",8)) && !key.starts_with(std::string("\0forkfs-history/",16)),"reserved filesystem key");
    std::lock_guard lock(mutex_);
    journal_->put(key,value);
}
std::vector<unsigned char> Container::get(const std::string& key) const {
    std::lock_guard lock(mutex_); return journal_->get(key);
}
namespace {
const std::string mount_root("\0forkfs-history/mount/world/",28);
const std::string revision_mount_root("\0forkfs-history/mount/revision/",31);
bool mount_id(const std::string& s){return s.size()==32&&std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
void mount_name(const std::string& s){require(!s.empty()&&s.size()<=64&&std::all_of(s.begin(),s.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';})&&s!="."&&s!="..","invalid mount view name");}
void check_mount_record(const std::vector<unsigned char>& v){require(v.size()==10&&!memcmp(v.data(),"FFMOUNT01",9)&&v[9]<=1,"invalid mount registration; refusing operation");}
std::vector<MountRegistration> mount_records(const Journal& journal){
    auto keys=journal.keys(mount_root);auto revisions=journal.keys(revision_mount_root);keys.insert(keys.end(),revisions.begin(),revisions.end());require(keys.size()<=128,"mount registration limit exceeded");std::vector<MountRegistration> out;out.reserve(keys.size());
    for(const auto& key:keys){bool revision=key.starts_with(revision_mount_root);auto root=revision?revision_mount_root:mount_root;require(key.starts_with(root),"invalid mount registration prefix");auto suffix=key.substr(root.size());auto a=suffix.find('/'),b=a==std::string::npos?a:suffix.find('/',a+1);require(a!=std::string::npos&&b!=std::string::npos&&suffix.find('/',b+1)==std::string::npos,"invalid mount registration key");MountRegistration r{suffix.substr(0,a),suffix.substr(a+1,b-a-1),suffix.substr(b+1),false,revision};mount_name(r.view);require(mount_id(r.generation)&&mount_id(r.token),"invalid mount registration key");auto value=journal.get(key);check_mount_record(value);r.read_only=value[9]!=0;require(r.read_only==revision,"mount registration mode mismatch");out.push_back(std::move(r));}
    return out;
}
}
void Container::mount_register(const std::string& view,const std::string& generation,const std::string& token,bool read_only,bool revision){
    mount_name(view);require(mount_id(generation)&&mount_id(token),"invalid mount identity");std::lock_guard lock(mutex_);
    require(read_only==revision,"mount registration mode mismatch");auto records=mount_records(*journal_);require(records.size()<128,"mount registration limit reached");require(std::none_of(records.begin(),records.end(),[&](const auto& r){return r.token==token;}),"mount token already registered");auto root=revision?revision_mount_root:mount_root;auto prefix=root+view+"/";
    std::vector<unsigned char> value{'F','F','M','O','U','N','T','0','1',static_cast<unsigned char>(read_only)};
    journal_->transact(journal_->sequence(),{{prefix+generation+"/"+token,std::move(value)}});
}
MountRegistration Container::mount_check(const std::string& token,const std::string& generation) const{
    require(mount_id(token)&&mount_id(generation),"invalid mount identity");std::lock_guard lock(mutex_);for(auto& r:mount_records(*journal_))if(r.token==token&&r.generation==generation)return r;
    throw std::runtime_error("mount registration not found");
}
std::vector<MountRegistration> Container::mount_list() const{std::lock_guard lock(mutex_);return mount_records(*journal_);}
void Container::mount_retire(const std::string& token,const std::string& old_generation){
    require(mount_id(token)&&mount_id(old_generation),"invalid mount identity");std::lock_guard lock(mutex_);auto records=mount_records(*journal_);std::vector<Mutation> changes;
    for(const auto& r:records)if(r.token==token&&r.generation==old_generation){auto root=r.revision?revision_mount_root:mount_root;auto key=root+r.view+"/"+r.generation+"/"+r.token;changes.push_back({key,std::nullopt});}
    if(!changes.empty())journal_->transact(journal_->sequence(),changes);
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
std::string Container::collect_step(size_t max_copy_bytes,size_t max_objects,size_t max_metadata_bytes,size_t mark_work) {
    // The storage coordinator owns publication locks. Do not hold the namespace
    // lock through graph marking: ordinary operations must keep making progress.
    require(mode_==OpenMode::ReadWrite,"revision preview is read-only");
    return journal_->collect_step(max_copy_bytes,max_objects,max_metadata_bytes,mark_work);
}
std::string Container::collection_status() const {
    std::lock_guard lock(collection_mutex_);
    return "{\"state\":"+json_string(collection_state_)+
        ",\"active\":"+(collection_active_?"true":"false")+
        ",\"steps\":"+std::to_string(collection_steps_)+
        ",\"last_step\":"+collection_last_+",\"error\":"+json_string(collection_error_)+"}\n";
}
std::string Container::start_collection(size_t max_copy_bytes,size_t max_objects,size_t max_steps,size_t max_metadata_bytes,size_t mark_work) {
    require(mode_==OpenMode::ReadWrite && journal_->is_leveldb(),"online GC requires writable LevelDB");
    require(max_copy_bytes>=65580 && max_copy_bytes<=8*1024*1024,"invalid GC copy budget");
    require(max_objects>0 && max_objects<=4096 && max_steps>0 && max_steps<=1024,"invalid GC step budget");
    require(max_metadata_bytes>=64*1024 && max_metadata_bytes<=256*1024*1024,"invalid GC metadata memory budget");
    require(mark_work>0 && mark_work<=65536,"invalid GC mark work budget");
    std::lock_guard lifecycle(collection_lifecycle_mutex_);
    {std::lock_guard lock(collection_mutex_);require(!collection_active_,"online GC is already running");}
    if(collection_thread_.joinable())collection_thread_.join();
    {std::lock_guard lock(collection_mutex_);collection_active_=true;collection_stop_=false;
        collection_steps_=0;collection_last_="null";collection_error_.clear();collection_state_="running";}
    try {collection_thread_=std::thread([this,max_copy_bytes,max_objects,max_steps,max_metadata_bytes,mark_work]{
        std::string state="budget_exhausted";
        try {
            for(size_t step=0;step<max_steps && !collection_stop_;++step){
                auto report=collect_step(max_copy_bytes,max_objects,max_metadata_bytes,mark_work);
                auto count=[&](const std::string& name){
                    auto marker="\""+name+"\":";auto at=report.find(marker);
                    require(at!=std::string::npos,"incomplete GC progress report");at+=marker.size();
                    auto end=report.find_first_not_of("0123456789",at);
                    require(end!=at,"invalid GC progress report");return std::stoull(report.substr(at,end-at));
                };
                const bool reclaimed=count("objects_deleted") || count("chunks_deleted") || count("chunks_moved") || count("segments_deleted");
                const bool progress=reclaimed || count("mark_work_done") || count("physical_work_done");
                const bool mark_pending=report.find("\"mark_pending\":true")!=std::string::npos;
                const bool physical_pending=report.find("\"physical_pending\":true")!=std::string::npos;
                const bool pending=count("pending_segments")!=0 || mark_pending || physical_pending;
                {std::lock_guard lock(collection_mutex_);collection_last_=report;++collection_steps_;}
                const bool skipped=report.find("\"logical_sweep_skipped\":true")!=std::string::npos;
                if(!pending && !reclaimed && !skipped){state="complete";break;}
                if(!progress){state=pending || skipped?"blocked":"complete";break;}
                std::this_thread::yield();
            }
            if(collection_stop_)state="stopped";
        }catch(const std::exception& e){std::lock_guard lock(collection_mutex_);collection_error_=e.what();state="failed";}
        std::lock_guard lock(collection_mutex_);collection_state_=state;collection_active_=false;
    });}catch(...){std::lock_guard lock(collection_mutex_);collection_active_=false;collection_state_="failed";throw;}
    return collection_status();
}
void Container::stop_collection() {
    std::lock_guard lifecycle(collection_lifecycle_mutex_);collection_stop_=true;
    if(collection_thread_.joinable())collection_thread_.join();
    journal_->cancel_collection();
    std::lock_guard state(collection_mutex_);
    if(collection_state_=="blocked" || collection_state_=="budget_exhausted")collection_state_="stopped";
}
void Container::sync() const {std::lock_guard lock(mutex_);journal_->barrier();}
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
    std::string receipt="{\"repository\":"+json_string(id_)+",\"sequence\":"+std::to_string(task.sequence)+",\"chain_hash\":"+json_string(chain)+"}";
    std::string checkpoint=task.state=="durable"?
        "{\"repository\":"+json_string(id_)+",\"sequence\":"+std::to_string(task.sequence)+",\"chain_hash\":"+json_string(chain)+"}":"null";
    return "{\"request_id\":"+json_string(request_id)+",\"state\":"+json_string(task.state)+
        ",\"archive\":"+json_string(task.archive)+",\"kind\":"+json_string(task.after?"incremental":"base")+
        ",\"sequence\":"+(task.state=="durable"?std::to_string(task.sequence):"null")+
        ",\"checkpoint\":"+checkpoint+",\"error\":"+json_string(task.error)+
        ",\"catalog\":"+json_string(task.catalog)+",\"archive_durable\":"+(task.archive_durable?"true":"false")+
        ",\"catalog_published\":"+(!task.catalog.empty() && task.state=="durable"?"true":"false")+
        ",\"archive_checkpoint\":"+(task.archive_durable?receipt:"null")+"}\n";
}
std::string Container::backup_status(const std::string& request_id) {
    std::lock_guard lock(backup_mutex_);load_backup_tasks_locked();return backup_status_locked(request_id);
}
std::string Container::start_backup(const std::string& request_id,const std::string& archive,
                                    std::optional<uint64_t> after,const std::string& catalog,bool retry_catalog) {
    require(mode_==OpenMode::ReadWrite && journal_->is_leveldb(),"backup requires writable LevelDB owner");
    require(!request_id.empty() && request_id.size()<=128,"invalid backup request ID");
    require(request_id.find('\0')==std::string::npos && archive.find('\0')==std::string::npos,"NUL in backup argument");
    require(archive.size()<=4096,"backup archive path too long");
    require(std::filesystem::path(archive).is_absolute(),"backup archive path must be absolute");
    require(catalog.size()<=4096 && catalog.find('\0')==std::string::npos && (catalog.empty() || std::filesystem::path(catalog).is_absolute()),"invalid backup catalog path");
    uint64_t resume_sequence=0;std::string resume_hash;
    std::lock_guard lifecycle(backup_lifecycle_mutex_);
    {
        std::lock_guard lock(backup_mutex_);
        load_backup_tasks_locked();
        require(backup_catalog_error_.empty(),"backup catalog persistence failed; restart and inspect task state");
        auto i=backup_tasks_.find(request_id);
        if(i!=backup_tasks_.end()) {
            require(i->second.archive==archive && i->second.after==after && i->second.catalog==catalog,"backup request ID reused with different arguments");
            if(!retry_catalog || i->second.state=="durable" || i->second.state=="running")return backup_status_locked(request_id);
            require(!catalog.empty() && i->second.archive_durable,"no recorded archive checkpoint to publish");
            resume_sequence=i->second.sequence;resume_hash=i->second.chain_hash;
        } else require(!retry_catalog,"unknown backup request");
        require(!backup_active_,"another backup export is running");
        require(retry_catalog || backup_tasks_.size()<128,"backup task history full; forget a terminal task before starting another");
    }
    // A finished worker is joined without holding the mutex it uses to publish.
    if(backup_thread_.joinable())backup_thread_.join();
    std::lock_guard lock(backup_mutex_);
    if(!retry_catalog){BackupTask task;task.archive=archive;task.after=after;task.catalog=catalog;backup_tasks_.emplace(request_id,std::move(task));}
    auto& started=backup_tasks_.at(request_id);started.state="running";started.error.clear();
    try {save_backup_tasks_locked();}
    catch(const std::exception& e){backup_catalog_error_=e.what();auto& task=backup_tasks_.at(request_id);task.state="interrupted";task.error="task intent persistence failed";throw;}
    backup_fault("backup_task_after_intent");
    backup_active_=true;
    try {
        backup_thread_=std::thread([this,request_id,archive,after,catalog,retry_catalog,resume_sequence,resume_hash]{
            uint64_t sequence=resume_sequence;std::string error,chain_hash=resume_hash;bool archive_durable=retry_catalog;
            try {
                if(!retry_catalog){sequence=after?journal_->export_replay(*after,archive,&chain_hash):journal_->backup(archive,&chain_hash);archive_durable=true;backup_fault("backup_task_after_archive");}
                if(!catalog.empty()){
                    {std::lock_guard lock(backup_mutex_);auto& task=backup_tasks_.at(request_id);
                        task.sequence=sequence;task.chain_hash=chain_hash;task.archive_durable=true;
                        try{save_backup_tasks_locked();}catch(const std::exception& e){backup_catalog_error_=e.what();throw;}}
                    backup_fault("backup_task_after_archive_checkpoint");
                    std::string hex;for(unsigned char c:chain_hash){hex+="0123456789abcdef"[c>>4];hex+="0123456789abcdef"[c&15];}
                    BackupCatalog::add(catalog,archive,LevelStore::RecoveryPoint{sequence,id_,hex},after);
                    backup_fault("backup_task_after_catalog");
                }
            }
            catch(const std::exception& e){error=e.what();}
            catch(...){error="unknown backup export/publication failure";}
            std::lock_guard done(backup_mutex_);
            auto& task=backup_tasks_.at(request_id);task.sequence=sequence;task.archive_durable=archive_durable;
            task.state=!backup_catalog_error_.empty()?"interrupted":error.empty()?"durable":"failed";task.error=std::move(error);task.chain_hash=std::move(chain_hash);
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
std::string Container::retry_backup_catalog(const std::string& request_id){
    BackupTask task;{std::lock_guard lock(backup_mutex_);load_backup_tasks_locked();auto found=backup_tasks_.find(request_id);
        require(found!=backup_tasks_.end(),"unknown backup request");task=found->second;}
    require(!task.catalog.empty(),"task has no catalog destination");
    return start_backup(request_id,task.archive,task.after,task.catalog,true);
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
    replay::Input input{content};auto version=input.take(8);require(version=="FFBKT001" || version=="FFBKT002","unsupported backup task catalog");
    require(input.take(32)==id_,"backup task catalog repository mismatch");
    auto count=input.integer(4);require(count<=128,"too many backup tasks");
    auto text=[&](size_t limit){auto size=input.integer(4);require(size<=limit,"oversized backup task field");return input.take(size);};
    std::map<std::string,BackupTask> tasks;
    for(uint64_t n=0;n<count;++n){
        auto id=text(128);BackupTask task;
        task.archive=text(4096);task.state=text(16);task.error=text(8192);task.chain_hash=text(32);
        auto incremental=input.integer(1),after=input.integer(8);task.sequence=input.integer(8);
        if(version=="FFBKT002"){task.catalog=text(4096);auto durable=input.integer(1);require(durable<=1,"invalid archive durability flag");task.archive_durable=durable;}
        else task.archive_durable=task.state=="durable";
        require(task.catalog.find('\0')==std::string::npos && (task.catalog.empty() || std::filesystem::path(task.catalog).is_absolute()),"invalid persisted catalog path");
        require(!task.archive_durable || (task.chain_hash.size()==32 && (!incremental || task.sequence>=after)),"invalid persisted archive checkpoint");
        require(task.state!="durable" || task.archive_durable,"durable task lacks archive checkpoint");
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
    bool extended=false;for(const auto& [id,task]:backup_tasks_)if(!task.catalog.empty())extended=true;
    std::string content=std::string(extended?"FFBKT002":"FFBKT001")+id_+replay::le(backup_tasks_.size(),4);
    auto text=[&](const std::string& value){content+=replay::le(value.size(),4);content+=value;};
    for(const auto& [id,task]:backup_tasks_){
        text(id);text(task.archive);text(task.state);text(task.error.substr(0,8192));text(task.chain_hash);
        content+=char(task.after.has_value());content+=replay::le(task.after.value_or(0));content+=replay::le(task.sequence);
        if(extended){text(task.catalog);content+=char(task.archive_durable);}
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

namespace forkfs {std::pair<uint64_t,uint64_t> Container::storage_space() const {std::lock_guard lock(mutex_);return journal_->space();}}
