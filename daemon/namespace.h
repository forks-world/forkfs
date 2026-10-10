#pragma once
#include "container.h"
#include <functional>
#include <map>

namespace forkfs {
struct InodeInfo {
    std::string id;
    bool directory;
    uint32_t mode;
    uint64_t links, size, modified_ns, changed_ns;
    bool symlink=false;
};
class NamespaceView;
class Namespace {
public:
    struct Attributes {std::optional<uint32_t> mode;std::optional<uint64_t> modified_ns;};
    class DirectoryCursor {
        friend class Namespace;
        std::weak_ptr<const int> lifetime_;
        std::string view_,prefix_,after_;bool revision_=false,done_=false;
        std::array<unsigned char,32> manifest_{};
    };
    struct DirectoryEntry {std::string name;InodeInfo inode;};
    struct DirectoryPage {std::vector<std::string> names;bool eof;std::vector<DirectoryEntry> entries;};
    DirectoryCursor open_directory(const std::string& path) const;
    DirectoryPage read_directory(DirectoryCursor& cursor,size_t limit,bool attributes=false) const;
    void rewind_directory(DirectoryCursor& cursor) const;
    enum class Access {Read,Write,ReadWrite};
    struct OpenOptions {bool create=false,exclusive=false,truncate=false;uint32_t mode=0644;bool append=false,nofollow=false;};
    explicit Namespace(Container& container,std::string view="main",bool revision=false)
        :container_(container),view_(std::move(view)),revision_(revision) {}
    std::string snapshot(const std::string& name);
    void fork(const std::string& revision,const std::string& world);
    std::vector<std::string> worlds() const;
    std::vector<std::string> revisions() const;
    // Frontend reads address logical inodes, never cached host/path names.
    InodeInfo root_inode() const;
    InodeInfo lookup_child(const std::string& parent_inode,const std::string& name) const;
    InodeInfo stat_inode(const std::string& inode_id) const;
    std::vector<unsigned char> read_inode(const std::string& inode_id,uint64_t offset,uint64_t count) const;
    std::string readlink_inode(const std::string& inode_id) const;
    DirectoryPage list_inode(const std::string& inode_id,const std::string& after,size_t limit) const;
    void initialize();
    void compact();
    size_t run_count() const;
    void mkdir(const std::string& path);
    void write(const std::string& path,const std::vector<unsigned char>& value);
    void replace_file(const std::string& path,const std::vector<unsigned char>& value);
    void write_at(const std::string& path,uint64_t offset,const std::vector<unsigned char>& value);
    uint64_t append(const std::string& path,const std::vector<unsigned char>& value);
    void truncate(const std::string& path,uint64_t size);
    std::vector<unsigned char> read(const std::string& path) const;
    std::vector<unsigned char> read_at(const std::string& path,uint64_t offset,uint64_t count) const;
    std::vector<std::string> list(const std::string& path) const;
    InodeInfo stat(const std::string& path) const;
    void set_attributes(const std::string& path,const Attributes& attributes);
    void handle_set_attributes(const std::string& handle,const Attributes& attributes);
    InodeInfo lstat(const std::string& path) const;
    void symlink(const std::string& target,const std::string& path);
    std::string readlink(const std::string& path) const;
    void rename(const std::string& source,const std::string& target);
    void link(const std::string& source,const std::string& target);
    void remove(const std::string& path,bool directory);
    void verify() const;
    std::string open(const std::string& path,Access access=Access::Read);
    std::string open(const std::string& path,Access access,const OpenOptions& options);
    std::string dup(const std::string& handle);
    enum class Seek {Set,Current,End};
    enum class Lock {Shared,Exclusive,Unlock};
    bool try_lock(const std::string& handle,Lock mode);
    bool try_lock_range(const std::string& handle,Lock mode,uint64_t start,uint64_t length);
    uint64_t seek(const std::string& handle,int64_t offset,Seek origin);
    std::vector<unsigned char> handle_read_next(const std::string& handle,uint64_t count);
    size_t handle_write_next(const std::string& handle,const std::vector<unsigned char>& value);
    void close(const std::string& handle);
    InodeInfo handle_stat(const std::string& handle) const;
    std::vector<unsigned char> handle_read(const std::string& handle,uint64_t offset,uint64_t count) const;
    void handle_write(const std::string& handle,uint64_t offset,const std::vector<unsigned char>& value);
    uint64_t handle_append(const std::string& handle,const std::vector<unsigned char>& value);
    void handle_truncate(const std::string& handle,uint64_t size);
private:
    friend class Container;
    static void recover_orphans(Container& container);
    const Container::OpenHandle& find_handle(const std::string& handle) const;
    void edit_handle(const std::string& handle,const std::function<bool(std::vector<unsigned char>&,bool)>& edit,const std::function<void()>& committed={});
    std::vector<unsigned char> read_handle_locked(const Container::OpenHandle& info,uint64_t offset,uint64_t count) const;
    using Changes=std::map<std::string,std::optional<std::vector<unsigned char>>>;
    void retire(const NamespaceView& view,Changes& changes,const InodeInfo& inode);
    void commit_changes(const NamespaceView& view,Changes& changes);
    Container& container_;
    std::string view_;
    bool revision_;
    void update(const std::function<void(const NamespaceView&,Changes&)>& operation);
    void edit_content(const std::string& path,const std::function<bool(std::vector<unsigned char>&)>& edit);
};
}
