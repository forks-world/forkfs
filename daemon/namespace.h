#pragma once
#include "container.h"
#include "filesystem_limits.h"
#include <functional>
#include <map>

namespace forkfs {
struct InodeInfo {
    std::string id;
    bool directory;
    uint32_t mode;
    uint64_t links, size, modified_ns, changed_ns;
    bool symlink=false;
    bool chunked=false;
    bool xattrs=false;
    uint64_t accessed_ns=0, birth_ns=0;
    // Derived backed data allocation in 512-byte units, not exclusive ownership
    // of LevelDB/SST bytes. Not serialized into the inode record.
    uint64_t allocated_size=0;
    uint32_t flags=0;
};
class NamespaceView;
class FileContent;
class Namespace {
public:
    static constexpr uint64_t maximum_file_size=maximum_file_bytes;
    static constexpr size_t maximum_io_size=256*1024;
    static constexpr size_t maximum_xattr_count=128;
    static constexpr uint64_t maximum_xattr_storage_bytes=8*1024*1024;
    struct Attributes {std::optional<uint32_t> mode;std::optional<uint64_t> modified_ns;std::optional<uint64_t> size;std::optional<uint64_t> accessed_ns,birth_ns;std::optional<uint32_t> flags;};
    class DirectoryCursor {
        friend class Namespace;
        std::weak_ptr<const int> lifetime_;
        std::shared_ptr<const int> gc_pin_;
        std::string view_,prefix_,after_;bool revision_=false,done_=false;
        std::array<unsigned char,32> manifest_{};
    };
    struct DirectoryEntry {std::string name;InodeInfo inode;};
    struct DirectoryPage {std::vector<std::string> names;bool eof;std::vector<DirectoryEntry> entries;};
    DirectoryCursor open_directory(const std::string& path) const;
    DirectoryPage read_directory(DirectoryCursor& cursor,size_t limit,bool attributes=false) const;
    void rewind_directory(DirectoryCursor& cursor) const;
    enum class Access {Read,Write,ReadWrite};
    struct OpenOptions {bool create=false,exclusive=false,truncate=false;uint32_t mode=0644;bool append=false,nofollow=false,symlink=false;};
    explicit Namespace(Container& container,std::string view="main",bool revision=false)
        :container_(container),view_(std::move(view)),revision_(revision) {}
    // Stateless page over two immutable revisions with an exclusive path cursor.
    std::string diff(const std::string& before,const std::string& after,
                     const std::string& after_path="",size_t limit=64) const;
    std::string revision_history(const std::string& tip,const std::string& after_id="",size_t limit=64) const;
    std::string snapshot(const std::string& name);
    std::string restore(const std::string& revision,const std::string& safety_revision);
    // Offline retention operations; descendants and other roots keep their data.
    std::string drop_world(const std::string& name);
    std::string forget_revision(const std::string& name);
    void fork(const std::string& revision,const std::string& world);
    std::vector<std::string> worlds() const;
    std::vector<std::string> revisions() const;
    void initialize();
    void compact();
    size_t run_count() const;
    void mkdir(const std::string& path,uint32_t mode=0755);
    std::string path_for_inode(const std::string& id) const;
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
    void set_link_attributes(const std::string& path,const Attributes& attributes);
    void handle_set_attributes(const std::string& handle,const Attributes& attributes);
    InodeInfo lstat(const std::string& path) const;
    void symlink(const std::string& target,const std::string& path);
    std::string readlink(const std::string& path) const;
    enum class XattrPolicy {Upsert,Create,Replace};
    static XattrPolicy xattr_policy(const std::string& value);
    std::vector<unsigned char> get_xattr(const std::string& path,const std::string& name) const {return read_xattr(path,name,false);}
    std::vector<std::string> list_xattrs(const std::string& path) const {return read_xattr_names(path,false);}
    void set_xattr(const std::string& path,const std::string& name,const std::vector<unsigned char>& value,XattrPolicy policy=XattrPolicy::Upsert) {edit_xattr(path,name,value,policy,false);}
    void remove_xattr(const std::string& path,const std::string& name) {edit_xattr(path,name,std::nullopt,XattrPolicy::Upsert,false);}
    std::vector<unsigned char> handle_get_xattr(const std::string& handle,const std::string& name) const {return read_xattr(handle,name,true);}
    std::vector<std::string> handle_list_xattrs(const std::string& handle) const {return read_xattr_names(handle,true);}
    void handle_set_xattr(const std::string& handle,const std::string& name,const std::vector<unsigned char>& value,XattrPolicy policy=XattrPolicy::Upsert) {edit_xattr(handle,name,value,policy,true);}
    void handle_remove_xattr(const std::string& handle,const std::string& name) {edit_xattr(handle,name,std::nullopt,XattrPolicy::Upsert,true);}

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
    void set_path_attributes(const std::string& path,const Attributes& attributes,bool follow_final);
    void require_offline_locked() const;
    static void recover_orphans(Container& container);
    void verify_locked() const;
    void allocation_info(InodeInfo& inode,const NamespaceView& view) const;
    std::array<unsigned char,32> prepare_snapshot_locked(const NamespaceView& view,const std::string& name,std::vector<Mutation>& batch);
    const Container::OpenHandle& find_handle(const std::string& handle) const;
    void edit_handle(const std::string& handle,const std::function<bool(FileContent&,bool)>& edit,const std::function<void()>& committed={});
    std::vector<unsigned char> read_handle_locked(const Container::OpenHandle& info,uint64_t offset,uint64_t count) const;
    friend class FileContent;
    struct Changes:std::map<std::string,std::optional<std::vector<unsigned char>>> {
        std::vector<std::pair<std::string,std::string>> erased;
        bool empty() const {return std::map<std::string,std::optional<std::vector<unsigned char>>>::empty() && erased.empty();}
        void erase_range(const std::string& lower,const std::string& upper) {
            erase(lower_bound(lower),lower_bound(upper));erased.emplace_back(lower,upper);
        }
    };
    std::pair<InodeInfo,bool> xattr_inode(const NamespaceView& view,const std::string& target,bool handle) const;
    std::vector<unsigned char> read_xattr(const std::string& target,const std::string& name,bool handle) const;
    std::vector<std::string> read_xattr_names(const std::string& target,bool handle) const;
    void edit_xattr(const std::string& target,const std::string& name,const std::optional<std::vector<unsigned char>>& value,XattrPolicy policy,bool handle);
    void retire(const NamespaceView& view,Changes& changes,const InodeInfo& inode);
    void commit_changes(const NamespaceView& view,Changes& changes);
    void commit_orphan_changes(const NamespaceView& retained,const std::string& prefix,const InodeInfo& inode,Changes& changes);
    Container& container_;
    std::string view_;
    bool revision_;
    void update(const std::function<void(const NamespaceView&,Changes&)>& operation);
    void edit_content(const std::string& path,const std::function<bool(FileContent&)>& edit);
};
}
