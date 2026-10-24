#include "rpc.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
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
struct Peer {int fd;explicit Peer(int n):fd(n){}~Peer(){if(fd>=0)close(fd);}};
bool owner(int fd,uid_t expected){
#ifdef __APPLE__
    uid_t uid;gid_t gid;return getpeereid(fd,&uid,&gid)==0 && uid==expected;
#else
    struct ucred creds{};socklen_t n=sizeof(creds);
    return getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&creds,&n)==0 && creds.uid==expected;
#endif
}
void wait_fd(int fd,short events,Clock::time_point deadline){
    while(true){auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        need(left>0,"RPC timeout");pollfd p{fd,events,0};int n=poll(&p,1,std::min<int64_t>(left,100));
        if(n<0 && errno==EINTR)continue;need(n>=0,"RPC poll failed");if(n && (p.revents&(events|POLLERR|POLLHUP)))return;
    }
}
}
std::string request_rpc(const std::string& path,const std::vector<std::string>& fields){
    return request_rpc(path,fields,geteuid());
}
std::string request_rpc(const std::string& path,const std::vector<std::string>& fields,uid_t expected_server){
    need(!fields.empty() && fields.size()<=16,"RPC requires 1..16 fields");std::string payload;put32(payload,fields.size());
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
    need(owner(peer.fd,expected_server),"RPC server identity mismatch");
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
    if(code&0x80000000u)throw std::system_error(static_cast<int>(code&0x7fffffffu),std::generic_category(),value);
    if(code)throw std::runtime_error(value);return value;
}}
