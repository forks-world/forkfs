#include "rpc.h"
#include "namespace.h"
#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <sstream>
#include <stdexcept>
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
    size_t at=header;auto count=u32(b,at);at+=4;need(count>0 && count<=8,"RPC requires 1..8 fields");
    std::vector<std::string> result;
    for(uint32_t i=0;i<count;++i){auto n=u32(b,at);at+=4;need(n<=b.size()-at,"truncated RPC field");result.push_back(b.substr(at,n));at+=n;}
    need(at==b.size(),"trailing RPC bytes");return result;
}
uint64_t number(const std::string& text,int base=10){
    uint64_t value=0;auto parsed=std::from_chars(text.data(),text.data()+text.size(),value,base);
    need(parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size(),"invalid file offset/count");return value;
}
int64_t signed_number(const std::string& text){
    int64_t value=0;auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    need(parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size(),"invalid signed offset");return value;
}
std::string names(const std::vector<std::string>& values){std::string out;for(const auto& s:values)out+=s+'\n';return out;}
std::string quoted(const std::string& value){
    std::string out="\"";for(char c:value){if(c=='\"' || c=='\\')out+='\\';out+=c;}return out+"\"";
}
std::string inode_json(const InodeInfo& n){
    std::ostringstream out;out<<"{\"inode_id\":\""<<n.id<<"\",\"directory\":"<<(n.directory?"true":"false")
        <<",\"symlink\":"<<(n.symlink?"true":"false")<<",\"mode\":"<<n.mode<<",\"links\":"<<n.links<<",\"size\":"<<n.size
        <<",\"modified_ns\":"<<n.modified_ns<<",\"changed_ns\":"<<n.changed_ns<<"}";return out.str();
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
};
class Sessions {
public:
    explicit Sessions(Container& store):store_(store){}
    std::string create(uint64_t lease){
        need(lease>=100 && lease<=300000,"session lease must be 100..300000 ms");
        need(values_.size()<128,"session limit reached");auto token=random_token();
        while(values_.contains(token))token=random_token();
        auto duration=std::chrono::milliseconds(lease);
        values_.emplace(token,Session{duration,Clock::now()+duration,false,{},{}});return token;
    }
    Session& acquire(const std::string& token){
        auto it=values_.find(token);need(it!=values_.end() && !it->second.closing && Clock::now()<it->second.deadline,"invalid or expired session");
        it->second.deadline=Clock::now()+it->second.lease;return it->second;
    }
    void close(const std::string& token){
        auto it=values_.find(token);need(it!=values_.end(),"invalid or expired session");auto& session=it->second;session.closing=true;
        while(!session.handles.empty()){
            auto h=session.handles.begin();Namespace fs(store_,h->second.view,h->second.revision);
            fs.close(h->first);session.handles.erase(h);
        }
        values_.erase(it);
    }
    void expire(){
        std::vector<std::string> dead;
        for(const auto& [token,s]:values_)if(s.closing || Clock::now()>=s.deadline)dead.push_back(token);
        for(const auto& token:dead)close(token);
    }
    size_t directory_count() const {size_t n=0;for(const auto& [token,s]:values_)n+=s.directories.size();return n;}
    void shutdown(){while(!values_.empty())close(values_.begin()->first);}
private:
    Container& store_;std::map<std::string,Session> values_;
};
struct Activity {
    Session* session;
    ~Activity(){if(session && !session->closing)session->deadline=Clock::now()+session->lease;}
};
std::string dispatch(Container& store,Sessions& sessions,std::vector<std::string> args){
    const auto op=args[0];std::string view="main";bool revision=false;
    size_t count=2;
    if(op=="status" || op=="storage-profile" || op=="fs-init" || op=="fs-check" || op=="fs-worlds" || op=="fs-revisions" || op=="fs-compact")count=1;
    if(op=="fs-write" || op=="fs-save" || op=="fs-rename" || op=="fs-link" || op=="fs-fork" || op=="fs-append" || op=="fs-truncate" || op=="fs-open" || op=="fs-happend" || op=="fs-htruncate" || op=="fs-hread-next" || op=="fs-hwrite-next" || op=="fs-symlink" || op=="fs-chmod" || op=="fs-setmtime" || op=="fs-hchmod" || op=="fs-hsetmtime" || op=="fs-dir-page" || op=="fs-dir-entries" || op=="fs-hlock")count=3;
    if(op=="fs-read-at" || op=="fs-write-at" || op=="fs-hread" || op=="fs-hwrite" || op=="fs-hseek" || op=="fs-hrange-lock")count=4;
    std::string session_token;bool selected_view=false;
    while(args.size()>=count+2) {
        const auto& selector=args[args.size()-2];const auto& value=args.back();
        if(selector=="--world" || selector=="--revision") {
            need(op.starts_with("fs-") && !selected_view,"invalid or duplicate view selector");
            view=value;revision=selector=="--revision";selected_view=true;
        } else if(selector=="--session") {
            need(!op.starts_with("session-") && session_token.empty(),"invalid or duplicate session selector");
            need(!value.empty(),"empty session token");session_token=value;
        } else break;
        args.resize(args.size()-2);
    }
    need(args.size()==count,"invalid RPC command arguments");
    for(size_t i=0;i<args.size();++i)if(!(((op=="fs-write" || op=="fs-save" || op=="fs-append" || op=="fs-happend" || op=="fs-hwrite-next") && i==2) || ((op=="fs-write-at" || op=="fs-hwrite") && i==3)))need(args[i].find('\0')==std::string::npos,"NUL in RPC argument");
    if(op=="session-open") {
        auto lease=number(args[1]);auto token=sessions.create(lease);
        return "{\"session\":\""+token+"\",\"lease_ms\":"+std::to_string(lease)+"}\n";
    }
    if(op=="session-keepalive"){sessions.acquire(args[1]);return store.status();}
    if(op=="session-close"){sessions.acquire(args[1]);sessions.close(args[1]);return store.status();}
    bool handle_op=op=="fs-open" || op=="fs-close" || op=="fs-dup" || op.starts_with("fs-h");
    bool dir_op=op.starts_with("fs-dir-");
    need(!(handle_op || dir_op) || !session_token.empty(),"handle operations require --session TOKEN");
    Activity activity{session_token.empty()?nullptr:&sessions.acquire(session_token)};
    if(handle_op && op!="fs-open")need(activity.session->handles.contains(args[1]),"handle belongs to another session");
    if(dir_op && op!="fs-dir-open") {
        auto i=activity.session->directories.find(args[1]);need(i!=activity.session->directories.end(),"invalid directory cursor/session");
        need(i->second.view==view && i->second.revision==revision,"directory cursor belongs to another view");
    }
    if(op=="status")return store.status();
    if(op=="storage-profile")return store.storage_profile();
    Namespace fs(store,view,revision);
    if(op=="fs-dir-open") {
        need(activity.session->directories.size()<64 && sessions.directory_count()<1024,"directory cursor limit reached");
        auto cursor=fs.open_directory(args[1]);auto token=random_token();
        while(activity.session->directories.contains(token))token=random_token();
        activity.session->directories.emplace(token,OwnedDirectory{view,revision,std::move(cursor)});
        return "{\"cursor\":\""+token+"\"}\n";
    }
    if(op=="fs-dir-rewind"){fs.rewind_directory(activity.session->directories.at(args[1]).cursor);return store.status();}
    if(op=="fs-dir-close"){activity.session->directories.erase(args[1]);return store.status();}
    if(op=="fs-dir-page" || op=="fs-dir-entries") {
        auto limit=number(args[2]);need(limit>=1 && limit<=1024,"directory page limit must be 1..1024");
        bool attrs=op=="fs-dir-entries";auto page=fs.read_directory(activity.session->directories.at(args[1]).cursor,limit,attrs);
        std::string result=attrs?"{\"entries\":[":"{\"names\":[";bool first=true;
        if(attrs)for(const auto& entry:page.entries){if(!first)result+=',';first=false;result+="{\"name\":"+quoted(entry.name)+",\"inode\":"+inode_json(entry.inode)+"}";}
        else for(const auto& name:page.names){if(!first)result+=',';first=false;result+=quoted(name);}
        return result+"],\"eof\":"+(page.eof?"true":"false")+"}\n";
    }
    if(op=="fs-cat"){auto b=fs.read(args[1]);return std::string(b.begin(),b.end());}
    if(op=="fs-read-at"){auto b=fs.read_at(args[1],number(args[2]),number(args[3]));return std::string(b.begin(),b.end());}
    if(op=="fs-open") {
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
        need(activity.session->handles.size()<256,"session handle limit reached");
        auto handle=fs.open(args[1],access,options);
        try{activity.session->handles.emplace(handle,OwnedHandle{view,revision});}catch(...){fs.close(handle);throw;}
        return "{\"handle\":\""+handle+"\"}\n";
    }
    if(op=="fs-dup") {
        need(activity.session->handles.size()<256,"session handle limit reached");auto handle=fs.dup(args[1]);
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
    if(op=="fs-hread-next"){auto b=fs.handle_read_next(args[1],number(args[2]));return std::string(b.begin(),b.end());}
    if(op=="fs-hwrite-next") {
        auto written=fs.handle_write_next(args[1],std::vector<unsigned char>(args[2].begin(),args[2].end()));
        return "{\"written\":"+std::to_string(written)+"}\n";
    }
    if(op=="fs-hread"){auto b=fs.handle_read(args[1],number(args[2]),number(args[3]));return std::string(b.begin(),b.end());}
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
    else if(op=="fs-compact")fs.compact();
    else if(op=="fs-check"){store.verify();fs.verify();}
    else throw std::runtime_error("unsupported RPC command");
    return store.status();
}
struct Peer {
    int fd;std::string in,out;size_t sent=0;bool done=false;
    Clock::time_point deadline=Clock::now()+std::chrono::seconds(5);
    explicit Peer(int n):fd(n){}
    ~Peer(){if(fd>=0)close(fd);}
};
bool owner(int fd){
#ifdef __APPLE__
    uid_t uid;gid_t gid;return getpeereid(fd,&uid,&gid)==0 && uid==geteuid();
#else
    struct ucred creds{};socklen_t n=sizeof(creds);
    return getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&creds,&n)==0 && creds.uid==geteuid();
#endif
}
void wait_fd(int fd,short events,Clock::time_point deadline){
    while(true){auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        need(left>0,"RPC timeout");pollfd p{fd,events,0};int n=poll(&p,1,std::min<int64_t>(left,100));
        if(n<0 && errno==EINTR)continue;need(n>=0,"RPC poll failed");if(n && (p.revents&(events|POLLERR|POLLHUP)))return;
    }
}
}
void serve_rpc(int listener,Container& container,const volatile sig_atomic_t& stopping){
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
                            if(p.in.size()==header+length)p.out=response(0,dispatch(container,sessions,decode(p.in)));
                        }
                    }catch(const std::exception& e){p.out=response(1,e.what());}
                }
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
                if(peers.size()<16 && owner(fd) && fcntl(fd,F_SETFD,FD_CLOEXEC)==0 && fcntl(fd,F_SETFL,O_NONBLOCK)==0)peers.push_back(std::move(p));
            }else need(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR,"RPC accept failed");
        }
    }
    sessions.shutdown();
}
std::string request_rpc(const std::string& path,const std::vector<std::string>& fields){
    need(!fields.empty() && fields.size()<=8,"RPC requires 1..8 fields");std::string payload;put32(payload,fields.size());
    for(const auto& field:fields){need(field.size()<=limit,"oversized RPC field");put32(payload,field.size());payload+=field;}
    auto request=frame("FFRPC001",payload);sockaddr_un addr{};
    need(path.size()<sizeof(addr.sun_path),"socket path too long");addr.sun_family=AF_UNIX;memcpy(addr.sun_path,path.c_str(),path.size()+1);
    Peer peer(socket(AF_UNIX,SOCK_STREAM,0));need(peer.fd>=0,"RPC socket failed");
    need(fcntl(peer.fd,F_SETFD,FD_CLOEXEC)==0 && fcntl(peer.fd,F_SETFL,O_NONBLOCK)==0,"RPC socket flags failed");
#ifdef __APPLE__
    int yes=1;need(setsockopt(peer.fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes))==0,"RPC socket SIGPIPE option failed");
#endif
    auto deadline=Clock::now()+std::chrono::seconds(10);
    if(connect(peer.fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0){
        need(errno==EINPROGRESS,"RPC connect failed");wait_fd(peer.fd,POLLOUT,deadline);
        int error=0;socklen_t len=sizeof(error);need(getsockopt(peer.fd,SOL_SOCKET,SO_ERROR,&error,&len)==0 && error==0,"RPC connect failed");
    }
    size_t sent=0;
    while(sent<request.size()){
        wait_fd(peer.fd,POLLOUT,deadline);
#ifdef __APPLE__
        auto n=send(peer.fd,request.data()+sent,request.size()-sent,0);
#else
        auto n=send(peer.fd,request.data()+sent,request.size()-sent,MSG_NOSIGNAL);
#endif
        if(n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK))continue;
        need(n>0,"RPC send failed");sent+=n;
    }
    std::string reply;size_t target=header;
    while(reply.size()<target){wait_fd(peer.fd,POLLIN,deadline);char buf[16384];
        auto n=recv(peer.fd,buf,std::min(sizeof(buf),target-reply.size()),0);
        if(n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK))continue;
        need(n>0,"truncated RPC reply");reply.append(buf,n);
        if(reply.size()==header){need(!memcmp(reply.data(),"FFRSP001",8),"unsupported RPC reply");auto length=u32(reply,8);
            need(length>=4 && length<=limit,"invalid RPC reply length");target=header+length;}
    }
    auto code=u32(reply,header);auto value=reply.substr(header+4);
    if(code)throw std::runtime_error(value);return value;
}
}
