#pragma once
#include <string>
#include <vector>
#include <csignal>
#include <stdexcept>
#include <cerrno>
#include <string_view>
namespace forkfs {
class Container;
inline int rpc_posix_error(std::string_view message) {
    if(message=="RPC timeout")return ETIMEDOUT;
    if(message=="RPC connect failed" || message=="RPC connection failed" || message=="RPC send failed" || message=="truncated RPC reply")return ENOTCONN;
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
    if(message.find("256 KiB")!=message.npos)return EFBIG;
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
std::string request_rpc(const std::string& socket,const std::vector<std::string>& fields);
}
