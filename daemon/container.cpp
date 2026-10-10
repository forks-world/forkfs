#include "container.h"
#include "level_store.h"
#include "namespace.h"
#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#ifdef FORKFS_TEST_FAULTS
#include <cstdlib>
#endif
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace forkfs {
namespace {
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

Container::Container(const std::string& path) {
    struct stat backing{};
    if(lstat(path.c_str(),&backing)==0 && S_ISDIR(backing.st_mode)) {
        journal_=std::make_unique<Journal>(path);id_=journal_->repository();Namespace::recover_orphans(*this);return;
    }
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
Container::~Container() { stop_compaction(); if (fd_ >= 0) close(fd_); }
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
