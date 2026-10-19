#pragma once
#include "replay_format.h"
#include <filesystem>
namespace forkfs {
// Admission budget for DATA segments, not a kernel-enforced whole-volume quota.
struct RepositoryBudget {
    uint64_t max_data=0,min_free=0;
    bool enabled() const{return max_data || min_free;}
    static uint64_t data_bytes(const std::string& path){
        uint64_t total=0;
        for(const auto& entry:std::filesystem::directory_iterator(path)){
            auto name=entry.path().filename().string();if(!name.starts_with("DATA-") || !name.ends_with(".seg"))continue;
            struct stat st{};replay::require(lstat(entry.path().c_str(),&st)==0 && S_ISREG(st.st_mode) && st.st_size>=0,"invalid budget data segment");
            replay::require(uint64_t(st.st_size)<=UINT64_MAX-total,"data usage overflow");total+=st.st_size;
        }return total;
    }
    static RepositoryBudget load(const std::string& path){
        int fd=open((path+"/CAPACITY").c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(fd<0){if(errno==ENOENT)return {};throw std::system_error(errno,std::generic_category(),"read capacity policy");}
        struct Close{int fd;~Close(){close(fd);}} close_fd{fd};struct stat st{};
        replay::require(fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size==56,"invalid capacity policy size");
        std::string value(56,0);size_t at=0;while(at<value.size()){auto n=::read(fd,value.data()+at,value.size()-at);if(n<0 && errno==EINTR)continue;replay::require(n>0,"truncated capacity policy");at+=n;}
        replay::require(value.substr(0,8)=="FFCAP001" && replay::hash(value.substr(0,24))==value.substr(24),"capacity policy checksum/format mismatch");
        return {replay::num(value.substr(8,8)),replay::num(value.substr(16,8))};
    }
    void admit(const std::string& path,uint64_t used,uint64_t incoming) const{
        if(max_data && (used>max_data || incoming>max_data-used))throw std::system_error(ENOSPC,std::generic_category(),"DATA segment budget exhausted");
        if(min_free){auto free=std::filesystem::space(path).available;
            constexpr uint64_t metadata_reserve=4*1024*1024;
            if(free<min_free || incoming>free-min_free || free-min_free-incoming<metadata_reserve)
                throw std::system_error(ENOSPC,std::generic_category(),"host free-space reserve reached");}
    }
    void save(const std::string& path) const{
        auto value=std::string("FFCAP001")+replay::le(max_data)+replay::le(min_free);value+=replay::hash(value);
        auto temporary=path+"/.capacity-XXXXXX";int fd=mkstemp(temporary.data());if(fd<0)throw std::system_error(errno,std::generic_category(),"create capacity policy");
        try{size_t at=0;while(at<value.size()){auto n=::write(fd,value.data()+at,value.size()-at);if(n<0 && errno==EINTR)continue;if(n<=0)throw std::system_error(n<0?errno:EIO,std::generic_category(),"write capacity policy");at+=n;}
            if(fsync(fd)<0)throw std::system_error(errno,std::generic_category(),"sync capacity policy");close(fd);fd=-1;
            if(rename(temporary.c_str(),(path+"/CAPACITY").c_str())<0)throw std::system_error(errno,std::generic_category(),"publish capacity policy");
            int directory=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(directory<0)throw std::system_error(errno,std::generic_category(),"open policy directory");
            int rc=fsync(directory),saved=errno;close(directory);if(rc<0)throw std::system_error(saved,std::generic_category(),"sync policy directory");
        }catch(...){if(fd>=0)close(fd);unlink(temporary.c_str());throw;}
    }
};
}
