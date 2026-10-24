#pragma once
#include <algorithm>
#include <optional>
#import "MountTransport.h"
namespace forkfs {
// Keeps the existing callback behavior for in-process tests while the resource
// loader can select a session-only backend with no Container ownership.
class VolumeView {
public:
    struct RemoteCursor {
        std::shared_ptr<MountTransport> transport;std::string token;uint64_t position=0;
        uint64_t cachedStart=0;size_t cachedCount=0;std::optional<Namespace::DirectoryPage> cachedPage;
        ~RemoteCursor(){try{if(transport && !token.empty() && transport->healthy())transport->call({"fs-dir-close",token});}catch(...) {}}
    };
    struct DirectoryCursor {Namespace::DirectoryCursor local;std::shared_ptr<RemoteCursor> remote;uint64_t index=0;};
    VolumeView(Container& store,const std::string& name,bool revision):local_(std::make_unique<Namespace>(store,name,revision)){}
    explicit VolumeView(std::shared_ptr<MountTransport> remote):remote_(std::move(remote)){}
    bool healthy()const{return !remote_ || remote_->healthy();}
    bool remote()const{return bool(remote_);}
    void revoke(){if(remote_)remote_->revoke();}
    void sync(){if(remote_)remote_->call({"fs-sync"});}
    std::pair<uint64_t,uint64_t> space(){auto v=remote_->json({"fs-statfs"});return {[v[@"total_bytes"] unsignedLongLongValue],[v[@"available_bytes"] unsignedLongLongValue]};}
    InodeInfo root_inode(){return local_?local_->root_inode():remote_inode(remote_->json({"fs-iroot"}));}
    InodeInfo stat_inode(const std::string& id){return local_?local_->stat_inode(id):remote_inode(remote_->json({"fs-istat",id}));}
    InodeInfo parent_inode(const std::string& id){return local_?local_->parent_inode(id):remote_inode(remote_->json({"fs-iparent",id}));}
    InodeInfo lookup_child(const std::string& id,const std::string& name){return local_?local_->lookup_child(id,name):remote_inode(remote_->json({"fs-ilookup",id,name}));}
    std::string open_inode(const std::string& id,Namespace::Access access){
        if(local_)return local_->open_inode(id,access);
        return remote_text(remote_->json({"fs-iopen",id,access==Namespace::Access::Read?"r":access==Namespace::Access::Write?"w":"rw"})[@"handle"]);
    }
    void close(const std::string& handle){if(local_)local_->close(handle);else remote_->call({"fs-close",handle});}
    InodeInfo handle_stat(const std::string& h){return local_?local_->handle_stat(h):remote_inode(remote_->json({"fs-hstat",h}));}
    std::vector<unsigned char> read_inode(const std::string& id,uint64_t offset,uint64_t count){
        if(local_)return local_->read_inode(id,offset,count);return bytes(remote_->call({"fs-iread",id,std::to_string(offset),std::to_string(std::min<uint64_t>(count,io_limit))}));
    }
    std::vector<unsigned char> handle_read(const std::string& id,uint64_t offset,uint64_t count){
        if(local_)return local_->handle_read(id,offset,count);return bytes(remote_->call({"fs-hread",id,std::to_string(offset),std::to_string(std::min<uint64_t>(count,io_limit))}));
    }
    void handle_write(const std::string& h,uint64_t offset,const std::vector<unsigned char>& data){
        if(local_){local_->handle_write(h,offset,data);return;}
        // The callback caps remote requests to one frame, so no mutation retry or
        // hidden multi-frame partial write occurs here.
        if(data.size()>io_limit)throw std::system_error(E2BIG,std::generic_category(),"remote write exceeds frame");
        remote_->call({"fs-hwrite",h,std::to_string(offset),text(data)});
    }
    size_t max_io()const{return remote_?io_limit:8*1024*1024;}
    std::string readlink_inode(const std::string& id){return local_?local_->readlink_inode(id):remote_->call({"fs-ireadlink",id});}
    void handle_set_attributes(const std::string& h,const Namespace::Attributes& a){if(local_)local_->handle_set_attributes(h,a);else if(has(a))remote_->call({"fs-hsetattr",h,attribute_patch(a)});}
    void set_inode_attributes(const std::string& id,const Namespace::Attributes& a){if(local_)local_->set_inode_attributes(id,a);else if(has(a))remote_->call({"fs-isetattr",id,attribute_patch(a)});}
    InodeInfo create_child(const std::string& p,const std::string& name,bool directory,const Namespace::Attributes& a,std::string* h){
        if(local_)return local_->create_child(p,name,directory,a,h);
        auto attributes=a;if(!has(a))attributes.mode=directory?0755:0644;
        auto v=remote_->json({"fs-icreate",p,name,directory?"directory":"file",attribute_patch(attributes)});
        if(h)*h=remote_text(v[@"handle"]);return remote_inode(v[@"inode"]);
    }
    InodeInfo symlink_child(const std::string& p,const std::string& name,const std::string& target,const Namespace::Attributes& a){
        if(local_)return local_->symlink_child(p,name,target,a);auto attrs=a;if(!has(a))attrs.mode=0777;
        return remote_inode(remote_->json({"fs-isymlink",p,name,target,attribute_patch(attrs)}));
    }
    InodeInfo link_child(const std::string& id,const std::string& p,const std::string& name){return local_?local_->link_child(id,p,name):remote_inode(remote_->json({"fs-ilink",id,p,name}));}
    void remove_child(const std::string& p,const std::string& name,const std::string& id,bool directory){if(local_)local_->remove_child(p,name,id,directory);else remote_->call({"fs-iremove",p,name,id,directory?"directory":"file"});}
    void rename_child(const std::string& p,const std::string& name,const std::string& dest,const std::string& dest_name,const std::string& id,const std::optional<std::string>& over){
        if(local_)local_->rename_child(p,name,dest,dest_name,id,over);else remote_->call({"fs-irename",p,name,dest,dest_name,id,over.value_or("")});
    }
    std::vector<std::string> list_inode_xattrs(const std::string& id){return local_?local_->list_inode_xattrs(id):xnames("fs-ilistxattr-raw",id);}
    std::vector<std::string> list_handle_xattrs(const std::string& h){return local_?local_->list_handle_xattrs(h):xnames("fs-hlistxattr-raw",h);}
    std::vector<unsigned char> get_inode_xattr(const std::string& id,const std::string& key){return local_?local_->get_inode_xattr(id,key):bytes(remote_->call({"fs-igetxattr",id,key}));}
    std::vector<unsigned char> get_handle_xattr(const std::string& id,const std::string& key){return local_?local_->get_handle_xattr(id,key):bytes(remote_->call({"fs-hgetxattr",id,key}));}
    void set_inode_xattr(const std::string& id,const std::string& key,const std::vector<unsigned char>& data,Namespace::XattrMode mode){if(local_)local_->set_inode_xattr(id,key,data,mode);else remote_->call({"fs-isetxattr",id,key,xmode(mode),text(data)});}
    void set_handle_xattr(const std::string& id,const std::string& key,const std::vector<unsigned char>& data,Namespace::XattrMode mode){if(local_)local_->set_handle_xattr(id,key,data,mode);else remote_->call({"fs-hsetxattr",id,key,xmode(mode),text(data)});}
    void remove_inode_xattr(const std::string& id,const std::string& key){if(local_)local_->remove_inode_xattr(id,key);else remote_->call({"fs-iremovexattr",id,key});}
    void remove_handle_xattr(const std::string& id,const std::string& key){if(local_)local_->remove_handle_xattr(id,key);else remote_->call({"fs-hremovexattr",id,key});}
    DirectoryCursor open_directory_inode(const std::string& id){
        if(local_)return {local_->open_directory_inode(id),{},0};
        auto cursor=std::make_shared<RemoteCursor>();cursor->transport=remote_;cursor->token=remote_text(remote_->json({"fs-dir-iopen",id})[@"cursor"]);return {{},cursor,0};
    }
    Namespace::DirectoryPage read_directory(DirectoryCursor& c,size_t count,bool attrs){
        if(local_)return local_->read_directory(c.local,count,attrs);
        if(!remote_->healthy())throw std::system_error(ENOTCONN,std::generic_category(),"mount session is permanently disconnected");
        if(c.remote->cachedPage && c.index==c.remote->cachedStart && count==c.remote->cachedCount){
            c.index=c.remote->cachedStart+c.remote->cachedPage->entries.size();return *c.remote->cachedPage;
        }
        // Copies retain one server-pinned snapshot. Rewind only when an earlier
        // cookie is requested; sequential batches require a single RPC.
        if(c.remote->position>c.index){remote_->call({"fs-dir-rewind",c.remote->token});c.remote->position=0;}
        while(c.remote->position<c.index){auto n=std::min<uint64_t>(128,c.index-c.remote->position);auto page=page_read(c,n);if(page.eof && c.remote->position<c.index)throw std::system_error(ESTALE,std::generic_category(),"cursor position lost");}
        auto start=c.remote->position;auto page=page_read(c,count);c.index=c.remote->position;
        c.remote->cachedStart=start;c.remote->cachedCount=count;c.remote->cachedPage=page;return page;
    }
    Namespace::DirectoryPage list_inode(const std::string& id,const std::string& after,size_t count){
        if(!local_)throw std::system_error(ENOTSUP,std::generic_category(),"remote inode listing requires a pinned directory cursor");
        return local_->list_inode(id,after,count);
    }
private:
    static constexpr size_t io_limit=1024*1024-4096;
    std::unique_ptr<Namespace> local_;std::shared_ptr<MountTransport> remote_;
    static bool has(const Namespace::Attributes& a){return a.mode||a.size||a.uid||a.gid||a.modified_ns||a.accessed_ns||a.created_ns;}
    static std::vector<unsigned char> bytes(const std::string& s){return {s.begin(),s.end()};}
    static std::string text(const std::vector<unsigned char>& b){return {b.begin(),b.end()};}
    static std::string xmode(Namespace::XattrMode m){return m==Namespace::XattrMode::Create?"create":m==Namespace::XattrMode::Replace?"replace":"upsert";}
    std::vector<std::string> xnames(const std::string& op,const std::string& id){
        auto b=remote_->call({op,id});NSArray *array=[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:b.data() length:b.size()] options:0 error:nil];
        if(![array isKindOfClass:[NSArray class]])throw std::runtime_error("invalid xattr names");std::vector<std::string> out;for(NSObject *v in array)out.push_back(remote_unhex(v));return out;
    }
    Namespace::DirectoryPage page_read(DirectoryCursor& c,size_t n){
        auto v=remote_->json({"fs-dir-entries-raw",c.remote->token,std::to_string(n)});Namespace::DirectoryPage p;
        if(![v[@"entries"] isKindOfClass:[NSArray class]] || ![v[@"eof"] isKindOfClass:[NSNumber class]])throw std::runtime_error("invalid directory page");
        p.eof=[v[@"eof"] boolValue];for(NSDictionary *e in v[@"entries"]){auto name=remote_unhex(e[@"name_hex"]);p.names.push_back(name);p.entries.push_back({name,remote_inode(e[@"inode"])});}
        if(p.entries.size()>n || (p.entries.empty()&&!p.eof))throw std::runtime_error("invalid directory progress");c.remote->position+=p.entries.size();return p;
    }
};
}
