#import "MountTransport.h"
#include "rpc.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/acl.h>
#include <unistd.h>
#include <sstream>
#include <algorithm>

namespace forkfs {
namespace {
[[noreturn]] void bad(){throw std::system_error(EINVAL,std::generic_category(),"invalid mount handoff/response");}
bool hex32(const std::string& s){return s.size()==32 && s.find_first_not_of("0123456789abcdef")==std::string::npos;}
void no_acl(const char *path){errno=0;auto acl=acl_get_link_np(path,ACL_TYPE_EXTENDED);if(acl){acl_free(acl);bad();}if(errno!=ENOENT)bad();}
NSDictionary *object(const std::string& bytes){
    id value=[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:bytes.data() length:bytes.size()] options:0 error:nil];
    if(![value isKindOfClass:[NSDictionary class]])bad();return value;
}
uint64_t integer(id value,uint64_t maximum=UINT64_MAX){
    if(![value isKindOfClass:[NSNumber class]] || CFGetTypeID((__bridge CFTypeRef)value)==CFBooleanGetTypeID())bad();
    auto text=remote_text([value stringValue]);uint64_t n=0;
    if(text.empty())bad();for(char c:text){if(c<'0'||c>'9'||n>(maximum-(c-'0'))/10)bad();n=n*10+(c-'0');}return n;
}
bool boolean(id value){if(![value isKindOfClass:[NSNumber class]] || CFGetTypeID((__bridge CFTypeRef)value)!=CFBooleanGetTypeID())bad();return [value boolValue];}
}
std::string remote_text(id value){
    if(![value isKindOfClass:[NSString class]])bad();auto data=[value dataUsingEncoding:NSUTF8StringEncoding];
    if(!data)bad();return std::string(static_cast<const char *>(data.bytes),data.length);
}
std::string remote_unhex(id value){
    auto s=remote_text(value);if(s.size()%2)bad();std::string out;
    auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;bad();};
    for(size_t i=0;i<s.size();i+=2)out.push_back(static_cast<char>(digit(s[i])*16+digit(s[i+1])));return out;
}
std::optional<MountCredentials> mount_credentials(NSURL *resource){
    if(!resource.isFileURL)bad();
    auto marker=[resource URLByAppendingPathComponent:@".forkfs-fskit-session"];
    int fd=open(marker.fileSystemRepresentation,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);int saved=errno;
    if(fd<0){if(saved==ENOENT)return std::nullopt;bad();}
    if(geteuid()==0){close(fd);bad();}
    struct stat st{};std::string bytes;bool valid=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==geteuid() &&
        (st.st_mode&07777)==0400 && st.st_nlink==1 && st.st_size>0 && st.st_size<=4096;
    if(valid){bytes.resize(st.st_size);size_t done=0;while(done<bytes.size()){
        auto n=read(fd,bytes.data()+done,bytes.size()-done);if(n<0&&errno==EINTR)continue;if(n<=0){valid=false;break;}done+=n;}}
    close(fd);if(!valid)bad();
    NSDictionary *v=object(bytes);
    NSSet *keys=[NSSet setWithArray:@[@"version",@"socket",@"service_uid",@"frontend_uid",@"generation",@"session",@"name",@"read_only",@"lease_ms"]];
    if(![[NSSet setWithArray:v.allKeys] isEqual:keys] || integer(v[@"version"])!=1 || integer(v[@"frontend_uid"])!=geteuid())bad();
    MountCredentials c{remote_text(v[@"socket"]),remote_text(v[@"generation"]),remote_text(v[@"session"]),remote_text(v[@"name"]),
        static_cast<uid_t>(integer(v[@"service_uid"],UINT32_MAX)),boolean(v[@"read_only"]),integer(v[@"lease_ms"])};
    if(!c.service_uid || c.service_uid==geteuid() || !hex32(c.generation) || !hex32(c.session) || c.lease_ms<3000 || c.lease_ms>300000 ||
       c.name.empty() || c.name.size()>64 || c.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")!=std::string::npos)bad();
    // The socket lives inside the same security-scoped control directory. No
    // backing-store URL or arbitrary endpoint path can be granted by a marker.
    auto canonical=resource.URLByResolvingSymlinksInPath.path;
    if(![canonical isEqual:resource.path] || c.socket!=std::string(resource.fileSystemRepresentation)+"/rpc.sock")bad();
    for(NSString *path=resource.path; ; path=[path stringByDeletingLastPathComponent]){
        struct stat info{};if(lstat(path.fileSystemRepresentation,&info)<0 || !S_ISDIR(info.st_mode) ||
            (info.st_uid!=0 && info.st_uid!=c.service_uid) || (info.st_mode&07022))bad();
        no_acl(path.fileSystemRepresentation);if([path isEqual:@"/"])break;
    }
    no_acl([[resource URLByAppendingPathComponent:@".forkfs-fskit-session"] fileSystemRepresentation]);
    return c;
}
MountTransport::MountTransport(MountCredentials c):credentials_(std::move(c)){
    auto hello=object(request_rpc(credentials_.socket,{"service-hello"},credentials_.service_uid));
    if(integer(hello[@"protocol"])!=1 || remote_text(hello[@"generation"])!=credentials_.generation)bad();
    // Verify the token before starting renewal; never create/reopen a session.
    auto root=json({"fs-iroot"});remote_inode(root);
    renewer_=std::thread([this]{renew();});
}
MountTransport::~MountTransport(){
    {std::lock_guard guard(mutex_);stopping_=true;wake_.notify_all();}
    if(renewer_.joinable())renewer_.join();
}
bool MountTransport::healthy()const{std::lock_guard guard(mutex_);return !dead_;}
std::string MountTransport::request(std::vector<std::string> fields){
    if(dead_)throw std::system_error(ENOTCONN,std::generic_category(),"mount session is permanently disconnected");
    try{return request_rpc(credentials_.socket,fields,credentials_.service_uid);}
    catch(const std::system_error& e){if(e.code().value()==ENOTCONN || e.code().value()==EIO)dead_=true;throw;}
    catch(...){dead_=true;throw std::system_error(ENOTCONN,std::generic_category(),"mount RPC failed; remount required");}
}
std::string MountTransport::call(std::vector<std::string> fields){
    std::lock_guard guard(mutex_);
    if(fields.empty() || !fields.front().starts_with("fs-"))bad();
    fields.insert(fields.end(),{"--session",credentials_.session,"--generation",credentials_.generation});return request(std::move(fields));
}
NSDictionary *MountTransport::json(std::vector<std::string> fields){
    auto bytes=call(std::move(fields));try{return object(bytes);}catch(...){std::lock_guard guard(mutex_);dead_=true;throw;}
}
void MountTransport::renew(){
    std::unique_lock lock(mutex_);
    while(!wake_.wait_for(lock,std::chrono::milliseconds(std::min<uint64_t>(1000,credentials_.lease_ms/3)),[this]{return stopping_;})){
        @autoreleasepool {try{auto reply=object(request({"mount-keepalive",credentials_.session,"--generation",credentials_.generation}));
            if(remote_text(reply[@"generation"])!=credentials_.generation)bad();}catch(...){dead_=true;return;}}
    }
}
void MountTransport::revoke(){
    std::lock_guard guard(mutex_);stopping_=true;wake_.notify_all();
    if(dead_)return;
    try{request({"mount-revoke",credentials_.session,"--generation",credentials_.generation});dead_=true;}
    catch(...){dead_=true;throw;}
}
InodeInfo remote_inode(NSDictionary *v){
    if(![v isKindOfClass:[NSDictionary class]])bad();InodeInfo n{};n.id=remote_text(v[@"inode_id"]);
    if(!hex32(n.id))bad();n.directory=boolean(v[@"directory"]);n.symlink=boolean(v[@"symlink"]);
    n.mode=integer(v[@"mode"],0777);n.links=integer(v[@"links"]);n.size=integer(v[@"size"],INT64_MAX);
    n.uid=integer(v[@"uid"],UINT32_MAX);n.gid=integer(v[@"gid"],UINT32_MAX);
    n.modified_ns=integer(v[@"modified_ns"],INT64_MAX);n.changed_ns=integer(v[@"changed_ns"],INT64_MAX);
    n.accessed_ns=integer(v[@"accessed_ns"],INT64_MAX);n.created_ns=integer(v[@"created_ns"],INT64_MAX);return n;
}
std::string attribute_patch(const Namespace::Attributes& a){
    std::ostringstream s;auto put=[&](const char *key,auto v){if(v){if(s.tellp()>0)s<<',';s<<key<<'='<<*v;}};
    if(a.mode){s<<"mode="<<std::oct<<*a.mode<<std::dec;}put("size",a.size);put("uid",a.uid);put("gid",a.gid);
    put("modified_ns",a.modified_ns);put("accessed_ns",a.accessed_ns);put("created_ns",a.created_ns);
    // Empty creation attributes use the existing defaults without another RPC.
    return s.str().empty()?"mode=644":s.str();
}
}
