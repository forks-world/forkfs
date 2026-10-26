#include "revision.h"
#include "rpc.h"
#ifdef FORKFS_TEST_FAULTS
#include <cstdlib>
#endif
#include "namespace.h"
#include "json_string.h"
#include <algorithm>
#include <chrono>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace forkfs {
namespace {
constexpr size_t limit=1024*1024,header=12;
using Clock=std::chrono::steady_clock;
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
uint32_t u32(const std::string& b,size_t at){
    need(at<=b.size() && b.size()-at>=4,"truncated RPC integer");uint32_t n=0;
    for(unsigned i=0;i<4;++i)n|=uint32_t(static_cast<unsigned char>(b[at+i]))<<(8*i);return n;
}
void put32(std::string& b,uint32_t n){for(unsigned i=0;i<4;++i)b+=static_cast<char>(n>>(8*i));}
std::string frame(const char* magic,const std::string& payload){
    need(payload.size()<=limit,"RPC payload exceeds 1 MiB");std::string result(magic,8);
    put32(result,payload.size());return result+payload;
}
std::string response(uint32_t code,const std::string& value){std::string b;put32(b,code);return frame("FFRSP001",b+value);}
std::vector<std::string> decode(const std::string& b){
    size_t at=header;auto count=u32(b,at);at+=4;need(count>0 && count<=16,"RPC requires 1..16 fields");
    std::vector<std::string> result;
    for(uint32_t i=0;i<count;++i){auto n=u32(b,at);at+=4;need(n<=b.size()-at,"truncated RPC field");result.push_back(b.substr(at,n));at+=n;}
    need(at==b.size(),"trailing RPC bytes");return result;
}
uint64_t number(const std::string& text,int base=10){
    uint64_t value=0;auto parsed=std::from_chars(text.data(),text.data()+text.size(),value,base);
    need(parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size(),"invalid file offset/count");return value;
}
// Parse the complete patch before publishing any metadata changes.
Namespace::Attributes attributes(const std::string& patch){
    Namespace::Attributes a;std::set<std::string> seen;size_t at=0;
    auto invalid=[](){throw std::system_error(EINVAL,std::generic_category(),"invalid attribute patch");};
    while(true){
        auto end=patch.find(',',at);auto field=patch.substr(at,end==std::string::npos?end:end-at);
        auto equal=field.find('=');if(equal==std::string::npos)invalid();
        auto key=field.substr(0,equal),text=field.substr(equal+1);
        if(!seen.insert(key).second)invalid();
        uint64_t n=0;auto parsed=std::from_chars(text.data(),text.data()+text.size(),n,key=="mode"?8:10);
        if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size())invalid();
        if(key=="mode"){if(n>0777)invalid();a.mode=static_cast<uint32_t>(n);}
        else if(key=="uid" || key=="gid"){
            if(n>UINT32_MAX)invalid();
            if(key=="uid")a.uid=static_cast<uint32_t>(n);else a.gid=static_cast<uint32_t>(n);
        } else if(key=="size")a.size=n;
        else if(key=="modified_ns" || key=="accessed_ns" || key=="created_ns"){
            if(n>INT64_MAX)invalid();
            if(key=="modified_ns")a.modified_ns=n;
            else if(key=="accessed_ns")a.accessed_ns=n;else a.created_ns=n;
        } else invalid();
        if(end==std::string::npos)break;at=end+1;
    }
    return a;
}
int64_t signed_number(const std::string& text){
    int64_t value=0;auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    need(parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size(),"invalid signed offset");return value;
}
std::string names(const std::vector<std::string>& values){std::string out;for(const auto& s:values)out+=s+'\n';return out;}
std::string hex_bytes(const std::string& value){
    static constexpr char digits[]="0123456789abcdef";std::string out;out.reserve(value.size()*2);
    for(unsigned char c:value){out+=digits[c>>4];out+=digits[c&15];}return out;
}
std::string raw_names(const std::vector<std::string>& values){
    std::string out="[";bool first=true;for(const auto& value:values){if(!first)out+=',';first=false;out+="\""+hex_bytes(value)+"\"";}return out+"]\n";
}
std::string rpc_json_quote(const std::string& value){
    return json_string::quote(value);
}
std::string inode_json(const InodeInfo& n){
    std::ostringstream out;out<<"{\"inode_id\":\""<<n.id<<"\",\"directory\":"<<(n.directory?"true":"false")
        <<",\"symlink\":"<<(n.symlink?"true":"false")<<",\"mode\":"<<n.mode<<",\"links\":"<<n.links<<",\"size\":"<<n.size
        <<",\"modified_ns\":"<<n.modified_ns<<",\"changed_ns\":"<<n.changed_ns
        <<",\"uid\":"<<n.uid<<",\"gid\":"<<n.gid<<",\"accessed_ns\":"<<n.accessed_ns<<",\"created_ns\":"<<n.created_ns<<"}";return out.str();
}
std::string random_token(){
    int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);need(fd>=0,"session entropy unavailable");
    unsigned char bytes[16];size_t done=0;
    while(done<sizeof(bytes)) {
        auto n=read(fd,bytes+done,sizeof(bytes)-done);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){close(fd);throw std::runtime_error("session entropy unavailable");}done+=n;
    }
    close(fd);std::string result;const char* hex="0123456789abcdef";
    for(auto c:bytes){result+=hex[c>>4];result+=hex[c&15];}return result;
}
struct OwnedHandle {std::string view;bool revision;};
struct OwnedDirectory {std::string view;bool revision;Namespace::DirectoryCursor cursor;};
struct Session {
    std::chrono::milliseconds lease;Clock::time_point deadline;bool closing=false;
    std::map<std::string,OwnedHandle> handles;
    std::map<std::string,OwnedDirectory> directories;
    std::optional<OwnedHandle> mount;
};
class Sessions {
public:
    explicit Sessions(Container& store):store_(store),instance_(random_token()){}
    const std::string& instance()const{return instance_;}
    std::string create(uint64_t lease,std::optional<OwnedHandle> mount=std::nullopt){
        need(lease>=100 && lease<=300000,"session lease must be 100..300000 ms");
        need(values_.size()<128,"session limit reached");auto token=random_token();
        while(values_.contains(token))token=random_token();
        auto duration=std::chrono::milliseconds(lease);
        values_.emplace(token,Session{duration,Clock::now()+duration,false,{},{},std::move(mount)});return token;
    }
    std::string create_mount(uint64_t lease,const std::string& token,OwnedHandle mount){
        need(lease>=100&&lease<=300000,"session lease must be 100..300000 ms");need(values_.size()<128,"session limit reached");need(!values_.contains(token),"session token collision");auto duration=std::chrono::milliseconds(lease);values_.emplace(token,Session{duration,Clock::now()+duration,false,{},{},std::move(mount)});return token;
    }
    Session& inspect(const std::string& token){
        auto it=values_.find(token);need(it!=values_.end() && !it->second.closing && Clock::now()<it->second.deadline,"invalid or expired session");return it->second;
    }
    Session& acquire(const std::string& token){
        auto& session=inspect(token);session.deadline=Clock::now()+session.lease;return session;
    }
    void close(const std::string& token){
        auto it=values_.find(token);need(it!=values_.end(),"invalid or expired session");auto& session=it->second;session.closing=true;
        while(!session.handles.empty()){
            auto h=session.handles.begin();Namespace fs(store_,h->second.view,h->second.revision);
            fs.close(h->first);session.handles.erase(h);
        }
        values_.erase(it);
    }
    void retire_mount(const std::string& token){auto it=values_.find(token);if(it!=values_.end()&&it->second.mount)close(token);}
    void expire(){
        std::vector<std::string> dead;
        for(const auto& [token,s]:values_)if(s.closing || Clock::now()>=s.deadline)dead.push_back(token);
        for(const auto& token:dead)close(token);
    }
    size_t directory_count() const {size_t n=0;for(const auto& [token,s]:values_)n+=s.directories.size();return n;}
    void revoke_world(const std::string& world){
        // Namespace::restore already removed engine handles/orphans. Remove
        // session ownership without attempting to close those revoked tokens.
        std::vector<std::string> revoked;
        for(auto& [token,session]:values_) {
            if(session.mount && !session.mount->revision && session.mount->view==world)revoked.push_back(token);
            std::erase_if(session.handles,[&](const auto& item){return !item.second.revision && item.second.view==world;});
            std::erase_if(session.directories,[&](const auto& item){return !item.second.revision && item.second.view==world;});
        }
        for(const auto& token:revoked)close(token);
    }
    void shutdown(){while(!values_.empty())close(values_.begin()->first);}
private:
    Container& store_;const std::string instance_;std::map<std::string,Session> values_;
};
struct Activity {
    Session* session;
    ~Activity(){if(session && !session->closing)session->deadline=Clock::now()+session->lease;}
};
std::string dispatch(Container& store,Sessions& sessions,std::vector<std::string> args,bool manager){
    const auto op=args[0];std::string view="main";bool revision=false;
    if(op=="gc-step" || op=="gc-start"){
        need(manager,"operation requires manager connection");
        need(args.size()<= (op=="gc-start"?6u:5u),"invalid GC arguments");
        if(op=="gc-step")return store.collect_step(args.size()>1?number(args[1]):1024*1024,args.size()>2?number(args[2]):128,args.size()>3?number(args[3]):64*1024*1024,args.size()>4?number(args[4]):128)+"\n";
        need(args.size()==1 || args.size()==4 || args.size()==5 || args.size()==6,"gc-start expects zero, three, four or five budgets");
        return store.start_collection(args.size()>1?number(args[1]):1024*1024,args.size()>1?number(args[2]):128,args.size()>1?number(args[3]):64,args.size()>4?number(args[4]):64*1024*1024,args.size()>5?number(args[5]):128);
    }
    if(op=="gc-status" || op=="gc-stop"){
        need(manager && args.size()==1,"GC control requires manager connection and no selectors");
        if(op=="gc-stop")store.stop_collection();return store.collection_status();
    }
    size_t count=2;
    if(op=="status" || op=="storage-profile" || op=="fs-init" || op=="fs-check" || op=="fs-worlds" || op=="fs-revisions" || op=="fs-compact" || op=="fs-statfs" || op=="fs-sync")count=1;
    if(op=="fs-write" || op=="fs-save" || op=="fs-rename" || op=="fs-link" || op=="fs-fork" || op=="fs-append" || op=="fs-truncate" || op=="fs-open" || op=="fs-happend" || op=="fs-htruncate" || op=="fs-hread-next" || op=="fs-hwrite-next" || op=="fs-symlink" || op=="fs-chmod" || op=="fs-setmtime" || op=="fs-hchmod" || op=="fs-hsetmtime" || op=="fs-dir-page" || op=="fs-dir-entries" || op=="fs-dir-entries-raw" || op=="fs-hlock")count=3;
    if(op=="fs-snapshot-parents")count=3;
    if(op=="backup-start")count=3;
    if(op=="backup-catalog-start")count=4;
    if(op=="replay-catalog-start")count=5;
    if(op=="replay-export-start")count=4;
    if(op=="fs-iread" || op=="fs-read-at" || op=="fs-write-at" || op=="fs-hread" || op=="fs-hwrite" || op=="fs-hseek" || op=="fs-hrange-lock")count=4;
    if(op=="fs-ilistxattr-raw" || op=="fs-hlistxattr-raw")count=2;
    if(op=="fs-setattr" || op=="fs-isetattr" || op=="fs-hsetattr" ||
       op=="fs-igetxattr" || op=="fs-hgetxattr" || op=="fs-iremovexattr" || op=="fs-hremovexattr")count=3;
    if(op=="fs-isetxattr" || op=="fs-hsetxattr")count=5;
    if(op=="fs-iroot" || op=="service-hello")count=1;
    if(op=="mount-open")count=4;
    if(op=="mount-check")count=2;
    if(op=="mount-list")count=1;
    if(op=="mount-retire")count=3;
    if(op=="fs-ilookup" || op=="fs-iopen")count=3;
    if(op=="fs-ilink")count=4;
    if(op=="fs-icreate" || op=="fs-isymlink" || op=="fs-iremove")count=5;
    if(op=="fs-irename")count=7;
    std::string session_token,generation;bool selected_view=false;
    while(args.size()>=count+2) {
        const auto& selector=args[args.size()-2];const auto& value=args.back();
        if(selector=="--world" || selector=="--revision") {
            need(op.starts_with("fs-") && !selected_view,"invalid or duplicate view selector");
            view=value;revision=selector=="--revision";selected_view=true;
        } else if(selector=="--generation"){
            need(generation.empty() && !value.empty(),"invalid or duplicate service generation");generation=value;
        } else if(selector=="--session") {
            need(!op.starts_with("session-") && session_token.empty(),"invalid or duplicate session selector");
            need(!value.empty(),"empty session token");session_token=value;
        } else break;
        args.resize(args.size()-2);
    }
    need(args.size()==count,"invalid RPC command arguments");
    for(size_t i=0;i<args.size();++i)if(!(((op=="fs-write" || op=="fs-save" || op=="fs-append" || op=="fs-happend" || op=="fs-hwrite-next") && i==2) || ((op=="fs-write-at" || op=="fs-hwrite") && i==3) || ((op=="fs-isetxattr" || op=="fs-hsetxattr") && i==4)))need(args[i].find('\0')==std::string::npos,"NUL in RPC argument");
    if(!generation.empty())need(generation==sessions.instance(),"service generation mismatch");
    if(op=="mount-check"){need(manager,"operation requires manager connection");need(!generation.empty()&&session_token.empty(),"mount-check requires service generation");auto& live=sessions.inspect(args[1]);need(live.mount.has_value(),"not a live mount session");auto record=store.mount_check(args[1],generation);need(record.view==live.mount->view&&record.revision==live.mount->revision,"mount registration does not match live session");return "{\"generation\":"+rpc_json_quote(generation)+",\"name\":"+rpc_json_quote(record.view)+",\"read_only\":"+(record.read_only?"true":"false")+"}\n";}
    if(op=="mount-list"){need(manager,"operation requires manager connection");need(!generation.empty()&&session_token.empty(),"mount-list requires service generation");auto records=store.mount_list();std::string out="{\"generation\":"+rpc_json_quote(generation)+",\"mounts\":[";bool first=true;for(const auto& r:records){if(!first)out+=',';first=false;out+="{\"generation\":"+rpc_json_quote(r.generation)+",\"token\":"+rpc_json_quote(r.token)+",\"view\":"+rpc_json_quote(r.view)+",\"read_only\":"+(r.read_only?"true":"false")+",\"revision\":"+(r.revision?"true":"false")+"}";}return out+"]}\n";}
    if(op=="mount-retire"){need(manager,"operation requires manager connection");need(!generation.empty()&&session_token.empty(),"mount-retire requires current service generation");if(args[1]==generation)sessions.retire_mount(args[2]);store.mount_retire(args[2],args[1]);return "{\"generation\":"+rpc_json_quote(generation)+"}\n";}
    if(!manager && op!="service-hello" && op!="mount-keepalive" && op!="mount-revoke"){
        need(!session_token.empty() && sessions.inspect(session_token).mount.has_value(),"frontend requires a mount session");
    }
    if(!session_token.empty())if(auto& current=sessions.inspect(session_token);current.mount){
        need(!generation.empty(),"mount request requires service generation");
        static const std::set<std::string> allowed={
            "fs-iroot","fs-iparent","fs-ilookup","fs-istat","fs-iread","fs-ireadlink",
            "fs-icreate","fs-isymlink","fs-ilink","fs-iremove","fs-irename","fs-iopen",
            "fs-isetattr","fs-ilistxattr","fs-ilistxattr-raw","fs-igetxattr","fs-isetxattr","fs-iremovexattr",
            "fs-close","fs-dup","fs-hread","fs-hwrite","fs-hstat","fs-htruncate","fs-happend",
            "fs-hread-next","fs-hwrite-next","fs-hseek","fs-hlock","fs-hrange-lock",
            "fs-hsetattr","fs-hlistxattr","fs-hlistxattr-raw","fs-hgetxattr","fs-hsetxattr","fs-hremovexattr",
            "fs-dir-iopen","fs-dir-page","fs-dir-entries","fs-dir-entries-raw","fs-dir-rewind","fs-dir-close","fs-statfs","fs-sync"};
        need(allowed.contains(op),"operation is not permitted for a mount session");
        need(!selected_view || (view==current.mount->view && revision==current.mount->revision),"mount session belongs to another view");
        view=current.mount->view;revision=current.mount->revision;
    }
    if(op=="service-hello")return "{\"protocol\":1,\"generation\":"+rpc_json_quote(sessions.instance())+"}\n";
    if(op=="mount-open"){
        need(!generation.empty(),"mount requires service generation");
        need(args[1]=="world" || args[1]=="revision","invalid mount view type");
        need(session_token.empty(),"mount-open does not accept a session");
        Namespace selected(store,args[2],args[1]=="revision");selected.root_inode();
        auto token=random_token();sessions.create_mount(number(args[3]),token,OwnedHandle{args[2],args[1]=="revision"});
        try{store.mount_register(args[2],sessions.instance(),token,args[1]=="revision",args[1]=="revision");}catch(...){sessions.retire_mount(token);throw;}
        return "{\"session\":"+rpc_json_quote(token)+",\"generation\":"+rpc_json_quote(sessions.instance())+"}\n";
    }
    if(op=="mount-keepalive" || op=="mount-revoke"){
        need(!generation.empty() && session_token.empty(),"mount lifecycle requires generation and positional session");
        need(sessions.inspect(args[1]).mount.has_value(),"not a mount session");
        if(op=="mount-revoke")sessions.close(args[1]);else sessions.acquire(args[1]);
        return "{\"generation\":"+rpc_json_quote(sessions.instance())+"}\n";
    }
    if(op=="session-open") {
        auto lease=number(args[1]);auto token=sessions.create(lease);
        return "{\"session\":\""+token+"\",\"lease_ms\":"+std::to_string(lease)+"}\n";
    }
    if(op=="session-keepalive"){need(!sessions.inspect(args[1]).mount,"use mount-keepalive for mount sessions");sessions.acquire(args[1]);return store.status();}
    if(op=="session-close"){need(!sessions.inspect(args[1]).mount,"use mount-revoke for mount sessions");sessions.acquire(args[1]);sessions.close(args[1]);return store.status();}
    bool handle_op=op=="fs-iopen" || op=="fs-open" || op=="fs-close" || op=="fs-dup" || op.starts_with("fs-h");
    bool dir_op=op.starts_with("fs-dir-");
    need(!(handle_op || dir_op || op=="fs-icreate") || !session_token.empty(),"handle operations require --session TOKEN");
    Activity activity{session_token.empty()?nullptr:&sessions.acquire(session_token)};
    if(handle_op && op!="fs-open" && op!="fs-iopen")need(activity.session->handles.contains(args[1]),"handle belongs to another session");
    if(dir_op && op!="fs-dir-open" && op!="fs-dir-iopen") {
        auto i=activity.session->directories.find(args[1]);need(i!=activity.session->directories.end(),"invalid directory cursor/session");
        need(i->second.view==view && i->second.revision==revision,"directory cursor belongs to another view");
    }
    if(op=="status")return store.status();
    if(op=="storage-profile")return store.storage_profile();
    if(op=="fs-statfs"){auto [total,available]=store.storage_space();return "{\"total_bytes\":"+std::to_string(total)+",\"available_bytes\":"+std::to_string(available)+"}\n";}
    // Mutations publish object data and metadata synchronously before returning.
    // Dispatch is serial: this ordered barrier must not trigger compaction.
    if(op=="fs-sync"){store.sync();return "{\"synced\":true}\n";}
    if(op=="backup-catalog-start"){need(!args[3].empty(),"catalog destination is required");return store.start_backup(args[1],args[2],std::nullopt,args[3]);}
    if(op=="replay-catalog-start"){need(!args[4].empty(),"catalog destination is required");return store.start_backup(args[1],args[3],number(args[2]),args[4]);}
    if(op=="backup-catalog-retry")return store.retry_backup_catalog(args[1]);
    if(op=="backup-start")return store.start_backup(args[1],args[2]);
    if(op=="replay-export-start")return store.start_backup(args[1],args[3],number(args[2]));
    if(op=="backup-status")return store.backup_status(args[1]);
    if(op=="backup-forget"){store.forget_backup(args[1]);return "{\"forgotten\":true}\n";}
    Namespace fs(store,view,revision);
    if(op=="fs-iroot")return inode_json(fs.root_inode())+"\n";
    if(op=="fs-ireadlink")return fs.readlink_inode(args[1]);
    if(op=="fs-iread"){auto b=fs.read_inode(args[1],number(args[2]),std::min<uint64_t>(number(args[3]),limit-4));return std::string(b.begin(),b.end());}
    if(op=="fs-iparent")return inode_json(fs.parent_inode(args[1]))+"\n";
    if(op=="fs-ilookup")return inode_json(fs.lookup_child(args[1],args[2]))+"\n";
    if(op=="fs-icreate"){
        need(args[3]=="file" || args[3]=="directory","invalid inode creation type");
        if(args[3]=="file" && activity.session->handles.size()>=256)throw std::system_error(EMFILE,std::generic_category(),"session handle limit reached");
        std::string handle;auto n=fs.create_child(args[1],args[2],args[3]=="directory",attributes(args[4]),args[3]=="file"?&handle:nullptr);
        if(!handle.empty())try{activity.session->handles.emplace(handle,OwnedHandle{view,revision});}catch(...){fs.close(handle);throw;}
        return "{\"inode\":"+inode_json(n)+",\"handle\":"+(handle.empty()?"null":rpc_json_quote(handle))+"}\n";
    }
    if(op=="fs-isymlink")return inode_json(fs.symlink_child(args[1],args[2],args[3],attributes(args[4])))+"\n";
    if(op=="fs-ilink")return inode_json(fs.link_child(args[1],args[2],args[3]))+"\n";
    if(op=="fs-iremove"){
        need(args[4]=="file" || args[4]=="directory","invalid inode removal type");
        fs.remove_child(args[1],args[2],args[3],args[4]=="directory");return store.status();
    }
    if(op=="fs-irename"){
        fs.rename_child(args[1],args[2],args[3],args[4],args[5],args[6].empty()?std::nullopt:std::optional<std::string>(args[6]));
        return store.status();
    }
    if(op=="fs-istat")return inode_json(fs.stat_inode(args[1]))+"\n";
    if(op=="fs-setattr" || op=="fs-isetattr" || op=="fs-hsetattr"){
        auto a=attributes(args[2]);
        if(revision)throw std::system_error(EROFS,std::generic_category(),"revision view is read-only");
        if(op=="fs-isetattr")fs.set_inode_attributes(args[1],a);
        else if(op=="fs-hsetattr")fs.handle_set_attributes(args[1],a);
        else fs.set_attributes(args[1],a);
        return store.status();
    }
    if(op=="fs-ilistxattr" || op=="fs-hlistxattr")
        return names(handle_op?fs.list_handle_xattrs(args[1]):fs.list_inode_xattrs(args[1]));
    if(op=="fs-ilistxattr-raw" || op=="fs-hlistxattr-raw")
        return raw_names(handle_op?fs.list_handle_xattrs(args[1]):fs.list_inode_xattrs(args[1]));
    if(op=="fs-igetxattr" || op=="fs-hgetxattr"){
        auto b=handle_op?fs.get_handle_xattr(args[1],args[2]):fs.get_inode_xattr(args[1],args[2]);
        return std::string(b.begin(),b.end());
    }
    if(op=="fs-isetxattr" || op=="fs-hsetxattr"){
        const auto& policy=args[3];
        if(policy!="upsert" && policy!="create" && policy!="replace")
            throw std::system_error(EINVAL,std::generic_category(),"invalid xattr policy");
        auto mode=policy=="create"?Namespace::XattrMode::Create:policy=="replace"?Namespace::XattrMode::Replace:Namespace::XattrMode::Upsert;
        std::vector<unsigned char> value(args[4].begin(),args[4].end());
        if(handle_op)fs.set_handle_xattr(args[1],args[2],value,mode);
        else fs.set_inode_xattr(args[1],args[2],value,mode);
        return store.status();
    }
    if(op=="fs-iremovexattr" || op=="fs-hremovexattr"){
        if(handle_op)fs.remove_handle_xattr(args[1],args[2]);else fs.remove_inode_xattr(args[1],args[2]);
        return store.status();
    }
    if(op=="fs-dir-open" || op=="fs-dir-iopen") {
        if(activity.session->directories.size()>=64)throw std::system_error(EMFILE,std::generic_category(),"session directory cursor limit reached");
        if(sessions.directory_count()>=1024)throw std::system_error(ENFILE,std::generic_category(),"global directory cursor limit reached");
        auto cursor=op=="fs-dir-iopen"?fs.open_directory_inode(args[1]):fs.open_directory(args[1]);auto token=random_token();
        while(activity.session->directories.contains(token))token=random_token();
        activity.session->directories.emplace(token,OwnedDirectory{view,revision,std::move(cursor)});
        return "{\"cursor\":\""+token+"\"}\n";
    }
    if(op=="fs-dir-rewind"){fs.rewind_directory(activity.session->directories.at(args[1]).cursor);return store.status();}
    if(op=="fs-dir-close"){activity.session->directories.erase(args[1]);return store.status();}
    if(op=="fs-dir-page" || op=="fs-dir-entries" || op=="fs-dir-entries-raw") {
        auto page_limit=number(args[2]);need(page_limit>=1 && page_limit<=1024,"directory page limit must be 1..1024");
        bool attrs=op!="fs-dir-page";bool raw=op=="fs-dir-entries-raw";
        auto& cursor=activity.session->directories.at(args[1]).cursor;
        auto trial=cursor;
        auto page=fs.read_directory(trial,page_limit,attrs);
        std::string result=attrs?"{\"entries\":[":"{\"names\":[";bool first=true;
        if(attrs)for(const auto& entry:page.entries){if(!first)result+=',';first=false;result+=raw?"{\"name_hex\":\""+hex_bytes(entry.name)+"\",\"inode\":"+inode_json(entry.inode)+"}":"{\"name\":"+rpc_json_quote(entry.name)+",\"inode\":"+inode_json(entry.inode)+"}";}
        else for(const auto& name:page.names){if(!first)result+=',';first=false;result+=rpc_json_quote(name);}
        result += "],\"eof\":";
        result += page.eof?"true":"false";
        result += "}\n";
        if(result.size()>limit-4) throw std::system_error(E2BIG,std::generic_category(),"directory JSON page exceeds RPC limit");
        cursor=std::move(trial);
        return result;
    }
    if(op=="fs-cat"){auto b=fs.read(args[1]);return std::string(b.begin(),b.end());}
    if(op=="fs-read-at"){auto b=fs.read_at(args[1],number(args[2]),std::min<uint64_t>(number(args[3]),limit-4));return std::string(b.begin(),b.end());}
    if(op=="fs-open" || op=="fs-iopen") {
        auto comma=args[2].find(',');auto mode=args[2].substr(0,comma);
        need(mode=="r" || mode=="w" || mode=="rw","invalid handle access mode");
        auto access=mode=="r"?Namespace::Access::Read:mode=="w"?Namespace::Access::Write:Namespace::Access::ReadWrite;
        Namespace::OpenOptions options;std::set<std::string> flags;
        while(comma!=std::string::npos) {
            auto start=comma+1;comma=args[2].find(',',start);auto flag=args[2].substr(start,comma==std::string::npos?comma:comma-start);
            need(flags.insert(flag).second,"duplicate open flag");
            if(flag=="create")options.create=true;
            else if(flag=="exclusive")options.exclusive=true;
            else if(flag=="truncate")options.truncate=true;
            else if(flag=="append")options.append=true;
            else if(flag=="nofollow")options.nofollow=true;
            else throw std::runtime_error("unsupported open flag");
        }
        if(activity.session->handles.size()>=256)throw std::system_error(EMFILE,std::generic_category(),"session handle limit reached");
        auto handle=op=="fs-iopen"?fs.open_inode(args[1],access,options):fs.open(args[1],access,options);
        try{activity.session->handles.emplace(handle,OwnedHandle{view,revision});}catch(...){fs.close(handle);throw;}
        return "{\"handle\":\""+handle+"\"}\n";
    }
    if(op=="fs-dup") {
        if(activity.session->handles.size()>=256)throw std::system_error(EMFILE,std::generic_category(),"session handle limit reached");auto handle=fs.dup(args[1]);
        try{activity.session->handles.emplace(handle,OwnedHandle{view,revision});}catch(...){fs.close(handle);throw;}
        return "{\"handle\":\""+handle+"\"}\n";
    }
    if(op=="fs-hlock") {
        need(args[2]=="shared" || args[2]=="exclusive" || args[2]=="unlock","invalid lock mode");
        auto mode=args[2]=="shared"?Namespace::Lock::Shared:args[2]=="exclusive"?Namespace::Lock::Exclusive:Namespace::Lock::Unlock;
        return std::string("{\"granted\":")+(fs.try_lock(args[1],mode)?"true":"false")+"}\n";
    }
    if(op=="fs-hrange-lock") {
        need(args[2]=="shared" || args[2]=="exclusive" || args[2]=="unlock","invalid lock mode");
        auto colon=args[3].find(':');need(colon!=std::string::npos && colon==args[3].rfind(':'),"lock range must be START:LENGTH");
        auto start=number(args[3].substr(0,colon)),length=number(args[3].substr(colon+1));
        auto mode=args[2]=="shared"?Namespace::Lock::Shared:args[2]=="exclusive"?Namespace::Lock::Exclusive:Namespace::Lock::Unlock;
        return std::string("{\"granted\":")+(fs.try_lock_range(args[1],mode,start,length)?"true":"false")+"}\n";
    }
    if(op=="fs-hseek") {
        need(args[3]=="set" || args[3]=="cur" || args[3]=="end","invalid seek origin");
        auto origin=args[3]=="set"?Namespace::Seek::Set:args[3]=="cur"?Namespace::Seek::Current:Namespace::Seek::End;
        return "{\"offset\":"+std::to_string(fs.seek(args[1],signed_number(args[2]),origin))+"}\n";
    }
    if(op=="fs-hread-next"){auto b=fs.handle_read_next(args[1],std::min<uint64_t>(number(args[2]),limit-4));return std::string(b.begin(),b.end());}
    if(op=="fs-hwrite-next") {
        auto written=fs.handle_write_next(args[1],std::vector<unsigned char>(args[2].begin(),args[2].end()));
        return "{\"written\":"+std::to_string(written)+"}\n";
    }
    if(op=="fs-hread"){auto b=fs.handle_read(args[1],number(args[2]),std::min<uint64_t>(number(args[3]),limit-4));return std::string(b.begin(),b.end());}
    if(op=="fs-happend") {
        auto offset=fs.handle_append(args[1],std::vector<unsigned char>(args[2].begin(),args[2].end()));
        return "{\"offset\":"+std::to_string(offset)+",\"written\":"+std::to_string(args[2].size())+"}\n";
    }
    if(op=="fs-readlink")return fs.readlink(args[1]);
    if(op=="fs-ls")return names(fs.list(args[1]));
    if(op=="fs-worlds")return names(fs.worlds());
    if(op=="fs-revisions")return names(fs.revisions());
    if(op=="fs-stat" || op=="fs-hstat" || op=="fs-lstat"){
        auto n=op=="fs-stat"?fs.stat(args[1]):op=="fs-lstat"?fs.lstat(args[1]):fs.handle_stat(args[1]);return inode_json(n)+"\n";
    }
    if(op=="fs-revision-point")return fs.revision_point(args[1]);
    if(op=="fs-snapshot-parents")return "{\"revision_id\":\""+fs.snapshot(args[1],revision_parent_names(args[2]))+"\"}\n";
    if(op=="fs-snapshot")return "{\"revision_id\":\""+fs.snapshot(args[1])+"\"}\n";
    if(op=="fs-append") {
        auto offset=fs.append(args[1],std::vector<unsigned char>(args[2].begin(),args[2].end()));
        return "{\"offset\":"+std::to_string(offset)+",\"written\":"+std::to_string(args[2].size())+"}\n";
    }
    if(op=="fs-chmod" || op=="fs-hchmod" || op=="fs-setmtime" || op=="fs-hsetmtime") {
        Namespace::Attributes a;
        if(op=="fs-chmod" || op=="fs-hchmod"){auto mode=number(args[2],8);need(mode<=0777,"unsupported mode bits");a.mode=static_cast<uint32_t>(mode);}
        else a.modified_ns=number(args[2]);
        if(op=="fs-hchmod" || op=="fs-hsetmtime")fs.handle_set_attributes(args[1],a);else fs.set_attributes(args[1],a);
        return store.status();
    }
    if(op=="fs-symlink")fs.symlink(args[1],args[2]);
    else if(op=="fs-close"){fs.close(args[1]);activity.session->handles.erase(args[1]);}
    else if(op=="fs-hwrite")fs.handle_write(args[1],number(args[2]),std::vector<unsigned char>(args[3].begin(),args[3].end()));
    else if(op=="fs-htruncate")fs.handle_truncate(args[1],number(args[2]));
    else if(op=="fs-init")fs.initialize();
    else if(op=="fs-write-at")fs.write_at(args[1],number(args[2]),std::vector<unsigned char>(args[3].begin(),args[3].end()));
    else if(op=="fs-truncate")fs.truncate(args[1],number(args[2]));
    else if(op=="fs-save")fs.replace_file(args[1],std::vector<unsigned char>(args[2].begin(),args[2].end()));
    else if(op=="fs-write")fs.write(args[1],std::vector<unsigned char>(args[2].begin(),args[2].end()));
    else if(op=="fs-mkdir")fs.mkdir(args[1]);
    else if(op=="fs-unlink")fs.remove(args[1],false);
    else if(op=="fs-rmdir")fs.remove(args[1],true);
    else if(op=="fs-rename")fs.rename(args[1],args[2]);
    else if(op=="fs-link")fs.link(args[1],args[2]);
    else if(op=="fs-fork")fs.fork(args[1],args[2]);
    else if(op=="fs-restore"){fs.restore(args[1]);sessions.revoke_world(view);}
    else if(op=="fs-compact")fs.compact();
    else if(op=="fs-check"){store.verify();fs.verify();}
    else throw std::runtime_error("unsupported RPC command");
    return store.status();
}
struct Peer {
    int fd;std::string in,out;size_t sent=0;bool done=false;bool manager=false;
    Clock::time_point deadline=Clock::now()+std::chrono::seconds(5);
    explicit Peer(int n):fd(n){}
    ~Peer(){if(fd>=0)close(fd);}
};
bool owner(int fd,uid_t control_uid){
#ifdef __APPLE__
    uid_t uid;gid_t gid;return getpeereid(fd,&uid,&gid)==0 && uid==control_uid;
#else
    struct ucred creds{};socklen_t n=sizeof(creds);
    return getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&creds,&n)==0 && creds.uid==control_uid;
#endif
}
}
void serve_rpc(int listener,Container& container,const volatile sig_atomic_t& stopping,uid_t control_uid,std::optional<uid_t> frontend_uid){
    std::vector<std::unique_ptr<Peer>> peers;Sessions sessions(container);
    while(!stopping){
        sessions.expire();
        std::vector<pollfd> polls{{listener,POLLIN,0}};
        for(const auto& p:peers)polls.push_back({p->fd,static_cast<short>(p->out.empty()?POLLIN:POLLOUT),0});
        int n=poll(polls.data(),polls.size(),100);
        if(n<0 && errno==EINTR)continue;need(n>=0,"RPC listener poll failed");
        need(!(polls[0].revents&(POLLERR|POLLHUP|POLLNVAL)),"RPC listener failed");
        for(size_t i=0;i<peers.size();++i){auto& p=*peers[i];auto events=polls[i+1].revents;
            if(Clock::now()>=p.deadline || (events&(POLLERR|POLLNVAL))){p.done=true;continue;}
            if(p.out.empty() && (events&(POLLIN|POLLHUP))){
                char buf[16384];auto got=recv(p.fd,buf,sizeof(buf),0);
                if(got<0){if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)p.done=true;}
                else if(got==0){p.out=response(1,"truncated RPC request");}
                else {
                    p.in.append(buf,got);
                    try{
                        if(p.in.size()>=header){
                            need(!memcmp(p.in.data(),"FFRPC001",8),"unsupported RPC protocol");
                            auto length=u32(p.in,8);need(length>=4 && length<=limit,"invalid RPC request length");
                            need(p.in.size()<=header+length,"trailing RPC frame");
                            if(p.in.size()==header+length){
                                auto command=decode(p.in);
#ifdef FORKFS_TEST_FAULTS
                                // A completed dispatch can outlive the receive
                                // deadline; deterministic regression, no sleep.
                                if(std::getenv("FORKFS_TEST_RPC_EXPIRE_DURING_DISPATCH") && command[0]=="fs-check")
                                    p.deadline=Clock::now()-std::chrono::milliseconds(1);
#endif
                                p.out=response(0,dispatch(container,sessions,std::move(command),p.manager));
                            }
                        }
                    }catch(const std::system_error& e){
                        // Existing clients treat any nonzero status as failure.
                        bool posix=(e.code().category()==std::generic_category() || e.code().category()==std::system_category()) && e.code().value()>0;
                        p.out=response(posix?(0x80000000u|static_cast<uint32_t>(e.code().value())):1,e.what());
                    }catch(const std::exception& e){p.out=response(1,e.what());}
                }
                // Give a newly prepared response its own bounded send window,
                // once, regardless of time spent executing the command.
                if(!p.out.empty())p.deadline=Clock::now()+std::chrono::seconds(5);
            }
            if(!p.out.empty() && (events&POLLOUT)){
                auto sent=send(p.fd,p.out.data()+p.sent,p.out.size()-p.sent,0);
                if(sent>0){p.sent+=sent;if(p.sent==p.out.size())p.done=true;}
                else if(sent==0 || (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR))p.done=true;
            }
        }
        peers.erase(std::remove_if(peers.begin(),peers.end(),[](const auto& p){return p->done;}),peers.end());
        if(polls[0].revents&POLLIN){int fd=accept(listener,nullptr,nullptr);
            if(fd>=0){auto p=std::make_unique<Peer>(fd);
                p->manager=owner(fd,control_uid);
                if(peers.size()<16 && (p->manager || (frontend_uid && owner(fd,*frontend_uid))) && fcntl(fd,F_SETFD,FD_CLOEXEC)==0 && fcntl(fd,F_SETFL,O_NONBLOCK)==0)peers.push_back(std::move(p));
            }else need(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR,"RPC accept failed");
        }
    }
    sessions.shutdown();
}
}
