#include "rpc.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __APPLE__
#include <os/log.h>
#include <CommonCrypto/CommonHMAC.h>
#include <netinet/in.h>
#include <cstdlib>
#endif
namespace forkfs { namespace {
constexpr size_t limit=1024*1024,header=12;
using Clock=std::chrono::steady_clock;
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
uint32_t u32(const std::string& b,size_t at){need(at<=b.size() && b.size()-at>=4,"truncated RPC integer");uint32_t n=0;for(unsigned i=0;i<4;++i)n|=uint32_t(static_cast<unsigned char>(b[at+i]))<<(8*i);return n;}
void put32(std::string& b,uint32_t n){for(unsigned i=0;i<4;++i)b+=static_cast<char>(n>>(8*i));}
std::string frame(const char* magic,const std::string& payload){need(payload.size()<=limit,"RPC payload exceeds 1 MiB");std::string r(magic,8);put32(r,payload.size());return r+payload;}
struct Peer{int fd;explicit Peer(int f):fd(f){}~Peer(){if(fd>=0)close(fd);}};
void wait_fd(int fd,short events,Clock::time_point deadline){
    for(;;){auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();need(left>0,"RPC timeout");
    pollfd p{fd,events,0};auto n=poll(&p,1,static_cast<int>(std::min<int64_t>(left,10000)));
    if(n<0 && errno==EINTR)continue;need(n>=0,"RPC poll failed");if(!n)continue;
    need(!(p.revents&(POLLERR|POLLNVAL)),"RPC connection failed");if(p.revents&(events|POLLHUP))return;}
}
#ifdef __APPLE__
void authenticate(int fd,const LoopbackEndpoint& endpoint,Clock::time_point deadline){
    need(endpoint.port!=0 && endpoint.key.size()==64,"invalid loopback endpoint");
    unsigned char key[32],client[32],server[64],digest[32];
    auto hex=[](char c)->int{if(c>='0' && c<='9')return c-'0';if(c>='a' && c<='f')return c-'a'+10;return -1;};
    for(size_t i=0;i<32;++i){int a=hex(endpoint.key[2*i]),b=hex(endpoint.key[2*i+1]);need(a>=0 && b>=0,"invalid loopback key");key[i]=static_cast<unsigned char>((a<<4)|b);}
    auto transfer=[&](void* data,size_t size,bool write){
        size_t at=0;while(at<size){wait_fd(fd,write?POLLOUT:POLLIN,deadline);
            auto n=write?send(fd,static_cast<char*>(data)+at,size-at,0):recv(fd,static_cast<char*>(data)+at,size-at,0);
            if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))continue;
            need(n>0,"loopback authentication failed");at+=n;
        }
    };
    arc4random_buf(client,sizeof client);std::string hello("FFBR001!",8);hello.append(reinterpret_cast<char*>(client),32);
    transfer(hello.data(),hello.size(),true);transfer(server,sizeof server,false);
    std::string proof("FFBR001S",8);proof.append(reinterpret_cast<char*>(client),32);proof.append(reinterpret_cast<char*>(server),32);
    CCHmac(kCCHmacAlgSHA256,key,sizeof key,proof.data(),proof.size(),digest);
    unsigned difference=0;for(size_t i=0;i<32;++i)difference|=digest[i]^server[32+i];
    need(difference==0,"loopback server authentication failed");
    proof[7]='C';CCHmac(kCCHmacAlgSHA256,key,sizeof key,proof.data(),proof.size(),digest);
    transfer(digest,sizeof digest,true);
}
#endif
}
std::string request_rpc(const std::string& path,const std::vector<std::string>& fields,const LoopbackEndpoint* loopback){
    need(!fields.empty() && fields.size()<=16,"RPC requires 1..16 fields");std::string payload;put32(payload,fields.size());
    for(const auto& field:fields){need(field.size()<=limit,"oversized RPC field");put32(payload,field.size());payload+=field;}
    auto request=frame("FFRPC001",payload);sockaddr_un addr{};
    if(!loopback){need(path.size()<sizeof(addr.sun_path),"socket path too long");addr.sun_family=AF_UNIX;memcpy(addr.sun_path,path.c_str(),path.size()+1);}
    Peer peer(socket(loopback?AF_INET:AF_UNIX,SOCK_STREAM,0));need(peer.fd>=0,"RPC socket failed");
    need(fcntl(peer.fd,F_SETFD,FD_CLOEXEC)==0 && fcntl(peer.fd,F_SETFL,O_NONBLOCK)==0,"RPC socket flags failed");
#ifdef __APPLE__
    int yes=1;need(setsockopt(peer.fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes))==0,"RPC socket SIGPIPE option failed");
#endif
    auto deadline=Clock::now()+std::chrono::seconds(10);
    const sockaddr* address=reinterpret_cast<sockaddr*>(&addr);socklen_t address_size=sizeof(addr);
#ifdef __APPLE__
    sockaddr_in internet{};
    if(loopback){need(loopback->port!=0,"invalid loopback port");internet.sin_family=AF_INET;internet.sin_addr.s_addr=htonl(INADDR_LOOPBACK);internet.sin_port=htons(loopback->port);address=reinterpret_cast<sockaddr*>(&internet);address_size=sizeof(internet);}
#else
    need(!loopback,"loopback RPC transport is supported only by the macOS frontend");
#endif
    if(connect(peer.fd,address,address_size)<0){
#ifdef __APPLE__
        if(errno!=EINPROGRESS){
            const int error=errno;
            os_log_error(OS_LOG_DEFAULT,"ForkFS RPC connect failed: errno=%d (%{public}s), socket=%{public}s",error,strerror(error),path.c_str());
            errno=error;
        }
#endif
        need(errno==EINPROGRESS,"RPC connect failed");wait_fd(peer.fd,POLLOUT,deadline);
        int error=0;socklen_t len=sizeof(error);need(getsockopt(peer.fd,SOL_SOCKET,SO_ERROR,&error,&len)==0 && error==0,"RPC connect failed");
    }
#ifdef __APPLE__
    if(loopback)authenticate(peer.fd,*loopback,deadline);
    else {uid_t uid;gid_t gid;need(getpeereid(peer.fd,&uid,&gid)==0 && uid==geteuid(),"RPC server identity mismatch");}
#else
    struct ucred credentials{};socklen_t credential_size=sizeof(credentials);
    need(getsockopt(peer.fd,SOL_SOCKET,SO_PEERCRED,&credentials,&credential_size)==0 && credentials.uid==geteuid(),"RPC server identity mismatch");
#endif
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
    need(code<=4096,"invalid RPC result code");
    if(code>1)throw RpcError(static_cast<int>(code-1),value);
    if(code)throw std::runtime_error(value);return value;
}
}
