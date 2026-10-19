#pragma once
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <system_error>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace forkfs {
// Stable inode shared by both directory generations during an offline GC swap.
// Keep this lease after closing LevelDB so another owner cannot enter that gap.
struct RepositoryOwner {
    int fd=-1;
    explicit RepositoryOwner(const std::string& path){
        fd=open((path+"/OWNERSHIP").c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd<0)throw std::system_error(errno,std::generic_category(),"open repository ownership lease");
        struct stat st{};
        if(fstat(fd,&st)<0 || !S_ISREG(st.st_mode)){close(fd);fd=-1;throw std::system_error(EINVAL,std::generic_category(),"invalid repository ownership lease");}
        if(flock(fd,LOCK_EX|LOCK_NB)<0){int e=errno;close(fd);fd=-1;throw std::system_error(e,std::generic_category(),"repository already has an owner");}
    }
    ~RepositoryOwner(){if(fd>=0)close(fd);}
    RepositoryOwner(const RepositoryOwner&)=delete;
};
}
