#pragma once
#include "repository_budget.h"
#include "replay_format.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace forkfs::gc_checkpoint {
struct Receipt {
    std::string repository,identity,archive,report,anchor;
    uint64_t sequence=0,size=0;
    std::string fingerprint;
};
inline void need(bool ok,const char* message){replay::require(ok,message);}
inline void sync_directory(const std::filesystem::path& path){
    int fd=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)throw std::system_error(errno,std::generic_category(),"open GC checkpoint directory");
    int rc=fsync(fd),saved=errno;close(fd);if(rc<0)throw std::system_error(saved,std::generic_category(),"sync GC checkpoint directory");
}
class FingerprintBuilder {
    std::array<char,64*1024> block_{};size_t used_=0;uint64_t total_=0;
    std::string chain_=replay::hash("forkfs.gc.archive.v1");
    void flush(){chain_=replay::hash(chain_+std::string(block_.data(),used_));used_=0;}
public:
    void append(const char* data,size_t size){
        need(uint64_t(size)<=UINT64_MAX-total_,"GC archive size overflow");total_+=size;
        while(size){auto n=std::min(size,block_.size()-used_);memcpy(block_.data()+used_,data,n);used_+=n;data+=n;size-=n;if(used_==block_.size())flush();}
    }
    uint64_t size()const{return total_;}
    std::string finish(){if(used_)flush();return replay::hash(chain_+replay::le(total_));}
};
inline std::string read_file(const std::filesystem::path& path,size_t limit){
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0){if(errno==ENOENT)return {};throw std::system_error(errno,std::generic_category(),"open GC receipt");}
    struct Close{int fd;~Close(){close(fd);}} close{fd};struct stat st{};
    need(fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size>=0 && uint64_t(st.st_size)<=limit,"invalid GC receipt file");
    std::string value(st.st_size,0);size_t at=0;
    while(at<value.size()){auto n=::read(fd,value.data()+at,value.size()-at);if(n<0 && errno==EINTR)continue;need(n>0,"truncated GC receipt");at+=n;}
    return value;
}
inline void append_field(std::string& out,const std::string& value){need(value.size()<=4096,"oversized GC receipt field");out+=replay::le(value.size(),4);out+=value;}
inline std::string encode(const Receipt& r){
    std::string out="FFGCP001";append_field(out,r.repository);append_field(out,r.identity);append_field(out,r.archive);
    append_field(out,r.fingerprint);append_field(out,r.report);append_field(out,r.anchor);
    out+=replay::le(r.sequence);out+=replay::le(r.size);out+=replay::hash(out);return out;
}
inline Receipt decode(const std::string& value){
    need(value.size()<=32*1024 && value.size()>=8+8*6+16+32 && value.starts_with("FFGCP001"),"invalid GC receipt size/format");
    need(replay::hash(value.substr(0,value.size()-32))==value.substr(value.size()-32),"GC receipt checksum mismatch");
    replay::Input in{value};need(in.take(8)=="FFGCP001","invalid GC receipt format");Receipt r;
    auto field=[&]{auto n=in.integer(4);need(n<=4096,"oversized GC receipt field");return in.take(n);};
    r.repository=field();r.identity=field();r.archive=field();r.fingerprint=field();r.report=field();r.anchor=field();
    r.sequence=in.integer(8);r.size=in.integer(8);need(in.take(32)==value.substr(value.size()-32),"GC receipt checksum mismatch");in.end();
    need(r.identity.size()==16 && r.fingerprint.size()==32 && r.anchor.size()==32 && !r.repository.empty() && !r.archive.empty() && !r.report.empty(),"invalid GC receipt fields");return r;
}
inline std::string fingerprint(const std::filesystem::path& path,uint64_t* size_out=nullptr){
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)throw std::system_error(errno,std::generic_category(),"open GC archive");
    struct Close{int fd;~Close(){close(fd);}} close{fd};struct stat st{};
    need(fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size>=0,"GC archive is not a regular file");
    std::array<char,64*1024> buffer{};FingerprintBuilder builder;
    for(;;){auto n=::read(fd,buffer.data(),buffer.size());if(n<0 && errno==EINTR)continue;if(n<0)throw std::system_error(errno,std::generic_category(),"read GC archive");if(!n)break;builder.append(buffer.data(),n);}
    need(uint64_t(st.st_size)==builder.size(),"GC archive changed while hashing");if(size_out)*size_out=builder.size();return builder.finish();
}
inline void sync_existing_archive(const std::filesystem::path& path){
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)throw std::system_error(errno,std::generic_category(),"open published GC checkpoint");
    struct Close{int fd;~Close(){close(fd);}} close{fd};struct stat st{};
    need(fstat(fd,&st)==0 && S_ISREG(st.st_mode),"GC checkpoint destination is not a regular file");
    if(fsync(fd)<0)throw std::system_error(errno,std::generic_category(),"sync published GC checkpoint");sync_directory(path.parent_path());
}
inline void write_atomic(const std::filesystem::path& path,const std::string& value){
    auto temporary=path.string()+".tmp-XXXXXX";int fd=mkstemp(temporary.data());
    if(fd<0)throw std::system_error(errno,std::generic_category(),"create GC receipt temporary");
    try{size_t at=0;while(at<value.size()){auto n=::write(fd,value.data()+at,value.size()-at);if(n<0 && errno==EINTR)continue;if(n<=0)throw std::system_error(n<0?errno:EIO,std::generic_category(),"write GC receipt");at+=n;}
        if(fsync(fd)<0)throw std::system_error(errno,std::generic_category(),"sync GC receipt");close(fd);fd=-1;
        if(rename(temporary.c_str(),path.c_str())<0)throw std::system_error(errno,std::generic_category(),"publish GC receipt");sync_directory(path.parent_path());
    }catch(...){if(fd>=0)close(fd);unlink(temporary.c_str());throw;}
}
inline bool exists_nofollow(const std::filesystem::path& path){struct stat st{};if(lstat(path.c_str(),&st)==0)return true;if(errno==ENOENT)return false;throw std::system_error(errno,std::generic_category(),"inspect GC receipt");}
inline void publish_archive(const std::filesystem::path& source,const Receipt& receipt,uint64_t min_free){
    const auto destination=std::filesystem::path(receipt.archive);const auto parent=destination.parent_path();
    if(exists_nofollow(destination)){
        struct stat st{};need(lstat(destination.c_str(),&st)==0 && S_ISREG(st.st_mode),"GC checkpoint destination is not a regular file");
        uint64_t size=0;need(fingerprint(destination,&size)==receipt.fingerprint && size==receipt.size,"GC checkpoint destination conflicts with pending checkpoint");
        sync_existing_archive(destination);return;
    }
    RepositoryBudget{0,min_free}.admit(parent.string(),0,receipt.size);
    auto temporary=(parent/".forkfs-gc-checkpoint-XXXXXX").string();int fd=mkstemp(temporary.data());
    if(fd<0)throw std::system_error(errno,std::generic_category(),"create GC checkpoint temporary");
    try{
        int in=open(source.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(in<0)throw std::system_error(errno,std::generic_category(),"open internal GC checkpoint");
        struct CloseInput{int fd;~CloseInput(){close(fd);}} close_input{in};struct stat input_stat{};
        need(fstat(in,&input_stat)==0 && S_ISREG(input_stat.st_mode),"internal GC checkpoint is not a regular file");
        std::array<char,64*1024> buffer{};FingerprintBuilder builder;
        for(;;){auto n=::read(in,buffer.data(),buffer.size());if(n<0 && errno==EINTR)continue;if(n<0){int e=errno;throw std::system_error(e,std::generic_category(),"read internal GC checkpoint");}if(!n)break;
            RepositoryBudget{0,min_free}.admit(parent.string(),0,n);builder.append(buffer.data(),n);size_t at=0;
            while(at<size_t(n)){auto w=::write(fd,buffer.data()+at,n-at);if(w<0 && errno==EINTR)continue;if(w<=0){int e=w<0?errno:EIO;throw std::system_error(e,std::generic_category(),"write external GC checkpoint");}at+=w;}}
        need(builder.size()==receipt.size && builder.finish()==receipt.fingerprint,"internal GC checkpoint fingerprint mismatch");
        if(fsync(fd)<0)throw std::system_error(errno,std::generic_category(),"sync external GC checkpoint");close(fd);fd=-1;
        if(link(temporary.c_str(),destination.c_str())<0){if(errno!=EEXIST)throw std::system_error(errno,std::generic_category(),"publish external GC checkpoint");
            struct stat st{};need(lstat(destination.c_str(),&st)==0 && S_ISREG(st.st_mode),"GC checkpoint destination is not a regular file");uint64_t size=0;
            need(fingerprint(destination,&size)==receipt.fingerprint && size==receipt.size,"GC checkpoint destination conflicts with pending checkpoint");sync_existing_archive(destination);}
        if(unlink(temporary.c_str())<0)throw std::system_error(errno,std::generic_category(),"remove GC checkpoint temporary");sync_directory(parent);
    }catch(...){if(fd>=0)close(fd);unlink(temporary.c_str());throw;}
}
}
