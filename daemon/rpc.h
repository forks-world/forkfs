#pragma once
#include <string>
#include <vector>
#include <csignal>
#include <stdexcept>
#include <cerrno>
#include <string_view>
#include <cstdint>
namespace forkfs {
class Container;
inline int rpc_posix_error(std::string_view message) {
    if(message=="RPC timeout")return ETIMEDOUT;
    if(message=="rename target is a directory")return EISDIR;
    if(message=="directory hard links are forbidden")return EPERM;
    if(message=="cannot unlink a directory"){
#ifdef __APPLE__
        return EPERM;
#else
        return EISDIR;
#endif
    }
    if(message=="symbolic link handle forbids data I/O")return EPERM;
    if(message=="mtime exceeds supported nanosecond range" || message=="atime exceeds supported nanosecond range" || message=="birth time exceeds supported nanosecond range")return EINVAL;
    if(message=="RPC connect failed" || message=="RPC connection failed" || message=="RPC send failed" || message=="truncated RPC reply")return ENOTCONN;
    if(message=="extended attribute not found"){
#ifdef __APPLE__
        return ENOATTR;
#else
        return ENODATA;
#endif
    }
    if(message=="extended attribute exceeds 64 KiB")return E2BIG;
    if(message=="extended attribute name exceeds 255 bytes")return ENAMETOOLONG;
    if(message=="extended attribute count limit reached")return ENOSPC;
    if(message=="object root not found")return ENOENT;
    if(message=="path exceeds mount protocol limit")return ENAMETOOLONG;
    if(message=="path not found" || message=="source not found")return ENOENT;
    if((message.find("invalid stale")!=message.npos || message.find("invalid mount view")!=message.npos))return ESTALE;
    if(message=="inode is no longer linked" || message.find("invalid or expired session")!=message.npos || message.find("invalid file handle")!=message.npos)return ESTALE;
    if(message.find("already exists")!=message.npos)return EEXIST;
    if(message.find("busy")!=message.npos || message.find("outstanding handles")!=message.npos)return EBUSY;
    if(message.find("not empty")!=message.npos)return ENOTEMPTY;
    if(message.find("not a directory")!=message.npos)return ENOTDIR;
    if(message.find("cannot read a directory")!=message.npos)return EISDIR;
    if(message.find("read-only")!=message.npos)return EROFS;
    if(message.find("256 KiB")!=message.npos || message=="file exceeds 64 MiB")return EFBIG;
    if(message.find("too many symbolic links")!=message.npos)return ELOOP;
    if(message.find("handle limit")!=message.npos || message.find("cursor limit")!=message.npos)return EMFILE;
    if(message.find("not readable")!=message.npos || message.find("not writable")!=message.npos)return EBADF;
    if(message.find("unsupported")!=message.npos)return ENOTSUP;
    if(message.find("NUL")!=message.npos || message.find("printable ASCII")!=message.npos || message.find("path must")!=message.npos || message.find("invalid")!=message.npos || message.find("forbidden")!=message.npos || message.find("inode types differ")!=message.npos || message.find("cannot move")!=message.npos)return EINVAL;
    return EIO;
}
class RpcError:public std::runtime_error {
public:int code;RpcError(int value,const std::string& message):std::runtime_error(message),code(value){}
};
// Same-UID control protocol; the optional FSKit frontend uses the inode operations.
// This transport is not an agent sandbox.
void serve_rpc(int listener,Container& container,const volatile sig_atomic_t& stopping);
// FSKit uses mutually authenticated loopback TCP to cross App Sandbox.
// The private mount descriptor carries a fresh per-bridge 256-bit key.
struct LoopbackEndpoint { uint16_t port=0; std::string key; };
std::string request_rpc(const std::string& socket,const std::vector<std::string>& fields,const LoopbackEndpoint* loopback=nullptr);
}
