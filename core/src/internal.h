// Internal types of the WorldFS core. Not part of the C ABI.
// Dependency policy: arch.md §39. Strings: smallstring. Containers: Containa. No std::string /
// unordered_map / std::mutex. Bump allocation (when needed): Arena.
#pragma once
#include "worldfs/worldfs.h"

#include <container/dense_map.hpp>
#include <smallstring.hpp>

#include <mutex>   // std::lock_guard only (header-only)
#include <pthread.h>
#include <string_view>

struct sqlite3;

namespace wfs {

using String = small::small_string;

// Portable mutex wrapper (pthread now; SRWLOCK on Windows later). Works with std::lock_guard.
class Mutex {
public:
    Mutex() { pthread_mutex_init(&m_, nullptr); }
    ~Mutex() { pthread_mutex_destroy(&m_); }
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;
    void lock() { pthread_mutex_lock(&m_); }
    void unlock() { pthread_mutex_unlock(&m_); }
private:
    pthread_mutex_t m_;
};
using Guard = std::lock_guard<Mutex>;

// One live logical inode. Path is derived from the parent chain so that a
// directory rename is O(1) and children never hold stale paths.
struct NodeRec {
    wfs_ino parent;
    wfs_type type;
    String name;
};

using InodeTable = stdb::container::dense_map<uint64_t, NodeRec>;

// Metadata-store helper (store.cpp): resolve a world's base dir through the parent chain.
int store_world_base_dir(wfs_store *s, wfs_world w, String &out);

// Platform layer (platform_posix.cpp)
int fs_lstat(const char *path, wfs_attr &out);
int fs_readlink(const char *path, char *buf, size_t cap, size_t *len);
int fs_mkfile(const char *path, uint32_t mode);
int fs_mkdir(const char *path, uint32_t mode);
int fs_mkfifo(const char *path, uint32_t mode);
int fs_symlink(const char *target, const char *path);
int fs_link(const char *existing, const char *path);
int fs_unlink(const char *path, bool is_dir);
int fs_rename(const char *from, const char *to);
int fs_setattr(const char *path, const wfs_setattr_req &req);
int fs_getxattr(const char *path, const char *name, void *buf, size_t cap, size_t *len);
int fs_setxattr(const char *path, const char *name, const void *data, size_t len, int flags);
int fs_removexattr(const char *path, const char *name);
int fs_listxattr(const char *path, char *buf, size_t cap, size_t *len);
int fs_statfs(const char *path, wfs_statfs_info &out);
// True if `name` is a quarantine xattr that this very process stamped on the file (macOS only).
// Such entries are hidden from the namespace; see platform_posix.cpp for why.
bool fs_xattr_is_own_quarantine(const char *path, const char *name, size_t name_len);
int fs_realpath(const char *path, String &out);
int fs_mkdir_p(const char *path);
// Enumerate a directory in stable order, skipping the first `skip` entries. cb returns non-zero to stop.
int fs_readdir(const char *path, uint64_t skip,
               int (*cb)(void *ctx, const char *name, size_t len, uint64_t ino, wfs_type type, uint64_t index),
               void *ctx);

} // namespace wfs

struct wfs_store {
    wfs::String dir;
    sqlite3 *db = nullptr;
    wfs::Mutex mu;
};

struct wfs_view {
    wfs_store *store = nullptr;
    wfs_world world = 0;
    wfs::String base_dir;   // backing root of the base world this view descends from
    bool writable = false;  // M0: only the base world itself is writable
    wfs::Mutex mu;
    wfs::InodeTable nodes;
};
