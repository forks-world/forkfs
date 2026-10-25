#pragma once
#include "container.h"
#include <functional>
#include <map>

namespace forkfs {
struct InodeInfo {
    using Xattrs=std::map<std::string,std::vector<unsigned char>>;
    std::string id;
    bool directory;
    uint32_t mode;
    uint64_t links, size, modified_ns, changed_ns;
    bool symlink=false;
    bool indexed_directory=false; // FFINOD02: old writers must reject indexed directories.
    uint32_t uid=0,gid=0;
    uint64_t accessed_ns=0,created_ns=0;
    bool extended_attributes=false; // FFINOD03 retains ownership and extra timestamps.
    std::shared_ptr<const Xattrs> xattrs={}; // Non-null retains FFINOD04, including an empty map.
    bool indexed_content=false; // FFINOD05: immutable sparse file radix tree.
};
class NamespaceView;
class FileContent;
class Namespace {
public:
    struct Attributes {
        std::optional<uint32_t> mode;std::optional<uint64_t> modified_ns;std::optional<uint64_t> size;
        std::optional<uint32_t> uid,gid;std::optional<uint64_t> accessed_ns,created_ns;
    };
    class DirectoryCursor {
        friend class Namespace;
        std::weak_ptr<const int> lifetime_;
        std::shared_ptr<void> object_pin_;
        std::string view_,prefix_,after_;bool revision_=false,done_=false;
        uint64_t generation_=0;
        std::array<unsigned char,32> manifest_{};
    };
    struct DirectoryEntry {std::string name;InodeInfo inode;};
    struct DirectoryPage {std::vector<std::string> names;bool eof;std::vector<DirectoryEntry> entries;};
    DirectoryCursor open_directory(const std::string& path) const;
    DirectoryCursor open_directory_inode(const std::string& inode_id) const;
    DirectoryPage read_directory(DirectoryCursor& cursor,size_t limit,bool attributes=false) const;
    void rewind_directory(DirectoryCursor& cursor) const;
    enum class Access {Read,Write,ReadWrite};
    struct OpenOptions {bool create=false,exclusive=false,truncate=false;uint32_t mode=0644;bool append=false,nofollow=false;};
    explicit Namespace(Container& container,std::string view="main",bool revision=false)
        :container_(container),view_(std::move(view)),revision_(revision) {}
    std::string snapshot(const std::string& name,std::optional<std::vector<std::string>> parents=std::nullopt);
    std::string revision_point(const std::string& name) const;
    void fork(const std::string& revision,const std::string& world);
    void restore(const std::string& revision);
    std::vector<std::string> worlds() const;
    std::vector<std::string> revisions() const;
    // Frontend reads address logical inodes, never cached host/path names.
    InodeInfo root_inode() const;
    bool has_parent_index() const;
    void build_parent_index(); // Explicit atomic upgrade of this writable view only.
    InodeInfo parent_inode(const std::string& directory_inode) const;
    InodeInfo lookup_child(const std::string& parent_inode,const std::string& name) const;
    InodeInfo stat_inode(const std::string& inode_id) const;
    std::vector<unsigned char> read_inode(const std::string& inode_id,uint64_t offset,uint64_t count) const;
    // Open an existing regular inode in this view, without resolving a host/path name.
    std::string open_inode(const std::string& inode_id,Access access=Access::Read);
    std::string open_inode(const std::string& inode_id,Access access,const OpenOptions& options);
    std::string readlink_inode(const std::string& inode_id) const;
    DirectoryPage list_inode(const std::string& inode_id,const std::string& after,size_t limit) const;
    InodeInfo create_child(const std::string& parent_inode,const std::string& name,bool directory,uint32_t mode);
    InodeInfo create_child(const std::string& parent_inode,const std::string& name,bool directory,
                           const Attributes& attributes,std::string* opened_handle=nullptr);
    InodeInfo symlink_child(const std::string& parent_inode,const std::string& name,const std::string& target,const Attributes& attributes);
    InodeInfo symlink_child(const std::string& parent_inode,const std::string& name,const std::string& target);
    InodeInfo link_child(const std::string& source_inode,const std::string& parent_inode,const std::string& name);
    void remove_child(const std::string& parent_inode,const std::string& name,
                      const std::string& expected_inode,bool directory);
    void rename_child(const std::string& source_parent,const std::string& source_name,
                      const std::string& destination_parent,const std::string& destination_name,
                      const std::string& expected_source,
                      const std::optional<std::string>& expected_target=std::nullopt);
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
    void set_inode_attributes(const std::string& inode_id,const Attributes& attributes);
    enum class XattrMode {Upsert,Create,Replace};
    std::vector<std::string> list_inode_xattrs(const std::string& inode_id) const;
    std::vector<unsigned char> get_inode_xattr(const std::string& inode_id,const std::string& name) const;
    void set_inode_xattr(const std::string& inode_id,const std::string& name,const std::vector<unsigned char>& value,XattrMode mode=XattrMode::Upsert);
    void remove_inode_xattr(const std::string& inode_id,const std::string& name);
    std::vector<std::string> list_handle_xattrs(const std::string& handle) const;
    std::vector<unsigned char> get_handle_xattr(const std::string& handle,const std::string& name) const;
    void set_handle_xattr(const std::string& handle,const std::string& name,const std::vector<unsigned char>& value,XattrMode mode=XattrMode::Upsert);
    void remove_handle_xattr(const std::string& handle,const std::string& name);
    void handle_set_attributes(const std::string& handle,const Attributes& attributes);
    InodeInfo lstat(const std::string& path) const;
    void symlink(const std::string& target,const std::string& path);
    std::string readlink(const std::string& path) const;
    void rename(const std::string& source,const std::string& target);
    void link(const std::string& source,const std::string& target);
    void remove(const std::string& path,bool directory);
    void verify() const;
    static void verify_recovery(const std::string& repository);
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
    DirectoryCursor directory_cursor(const NamespaceView& view,const std::string& inode_id) const;
    friend class Container;
    static void recover_orphans(Container& container);
    const Container::OpenHandle& find_handle(const std::string& handle) const;
    void edit_handle(const std::string& handle,const std::function<bool(FileContent&,bool)>& edit,const std::function<void()>& committed={});
    std::vector<unsigned char> read_handle_locked(const Container::OpenHandle& info,uint64_t offset,uint64_t count) const;
    using Changes=std::map<std::string,std::optional<std::vector<unsigned char>>>;
    std::pair<InodeInfo,std::string> xattr_inode(const NamespaceView& view,const std::string& address,bool handle) const;
    void edit_xattrs(const std::string& address,bool handle,const std::string& name,
                    const std::optional<std::vector<unsigned char>>& value,XattrMode mode);
    void attribute_changes(const NamespaceView& view,Changes& changes,InodeInfo inode,const Attributes& attributes,
                           const std::string& inode_key,const std::string& content_key,bool linked);
    void retire(const NamespaceView& view,Changes& changes,const InodeInfo& inode);
    InodeInfo create_entry(const NamespaceView& view,Changes& changes,const std::string& parent,
                          const std::string& name,bool directory,const Attributes& attributes);
    InodeInfo symlink_entry(const NamespaceView& view,Changes& changes,const std::string& parent,
                           const std::string& name,const std::string& target);
    InodeInfo link_entry(const NamespaceView& view,Changes& changes,const std::string& source,
                        const std::string& parent,const std::string& name);
    void remove_entry(const NamespaceView& view,Changes& changes,const std::string& parent,
                      const std::string& name,bool directory);
    void rename_entry(const NamespaceView& view,Changes& changes,const std::string& source_parent,
                      const std::string& source_name,const std::string& destination_parent,
                      const std::string& destination_name,const std::vector<std::string>* ancestry=nullptr);
    std::vector<Mutation> prepare_changes(const NamespaceView& view,const Changes& changes);
    void commit_changes(const NamespaceView& view,Changes& changes);
    // Caller holds container_.mutex_; pin and publish create/truncate atomically.
    std::string publish_open(const NamespaceView& view,const std::string& inode_id,
                             Access access,const OpenOptions& options,Changes& changes);
    Container& container_;
    std::string view_;
    bool revision_;
    void update(const std::function<void(const NamespaceView&,Changes&)>& operation);
    void edit_content(const std::string& path,const std::function<bool(FileContent&)>& edit);
};
}
