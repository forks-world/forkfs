#include "backup_catalog.h"
#include "replay_format.h"
#include <algorithm>
#include <filesystem>
#include <sys/file.h>
#ifdef FORKFS_TEST_FAULTS
#include <cstdlib>
#endif
namespace forkfs {
namespace {
using replay::require;
void fault(const char* stage){
#ifdef FORKFS_TEST_FAULTS
    const char* requested=std::getenv("FORKFS_TEST_CRASH");if(requested && !strcmp(requested,stage))_exit(86);
#else
    (void)stage;
#endif
}
std::string hex(const std::string& bytes){std::string out;for(unsigned char c:bytes){out+="0123456789abcdef"[c>>4];out+="0123456789abcdef"[c&15];}return out;}
void sync_directory(const std::string& path){
    int fd=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)throw std::system_error(errno,std::generic_category(),"open catalog directory");
    int rc=fsync(fd),error=errno;close(fd);if(rc<0)throw std::system_error(error,std::generic_category(),"sync catalog directory");
}
struct Lock {
    int fd;
    Lock(const std::string& directory,bool create):fd(-1){
        if(create){
            if(mkdir(directory.c_str(),0700)!=0 && errno!=EEXIST)throw std::system_error(errno,std::generic_category(),"create catalog");
            // Another importer may have created the directory and crashed before
            // syncing its parent. Every successful importer must cover that case.
            auto parent=std::filesystem::path(directory).parent_path();
            sync_directory(std::filesystem::canonical(parent.empty()?".":parent).string());
        }
        fd=open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0)throw std::system_error(errno,std::generic_category(),"open catalog");
        if(flock(fd,create?LOCK_EX:LOCK_SH)<0){int error=errno;close(fd);throw std::system_error(error,std::generic_category(),"lock catalog");}
    }
    ~Lock(){close(fd);}
};
struct Package {replay::Header header;uint64_t size;std::string digest;};
Package inspect(const std::string& path){
    replay::File file(path);auto header=replay::validate(file);struct stat st{};
    require(fstat(file.fd,&st)==0 && st.st_size>=0,"cannot stat catalog archive");
    replay::Reader reader(file);require(reader.header.encode()==header.encode(),"catalog archive changed during validation");
    while(!reader.ended)reader.next();
    // The canonical FFRPLY01 transport digest covers the header and every frame.
    auto digest=replay::hash("forkfs.catalog.package.v1"+replay::le(st.st_size)+reader.digest);
    return {header,static_cast<uint64_t>(st.st_size),digest};
}
std::string record(const Package& p){auto payload="FFCAT001"+p.header.encode()+replay::le(p.size)+p.digest;return payload+replay::hash(payload);}
constexpr size_t record_size=8+105+8+32+32;
std::string read_record(const std::string& path){replay::File f(path);auto data=f.read(record_size);f.eof();return data;}
std::vector<BackupCatalog::Entry> load(const std::string& directory){
    std::vector<std::string> records;
    for(const auto& item:std::filesystem::directory_iterator(directory))if(item.path().extension()==".entry"){
        require(records.size()<4096,"catalog entry limit exceeded");records.push_back(item.path().filename().string());
    }
    std::sort(records.begin(),records.end());std::vector<BackupCatalog::Entry> result;std::string repository;std::set<std::string> digests;
    for(const auto& name:records){
        auto data=read_record(directory+"/"+name);auto checksum=replay::hash(data.substr(0,record_size-32));
        require(data.starts_with("FFCAT001") && data.substr(record_size-32)==checksum && name==hex(checksum)+".entry","invalid catalog record");
        auto digest=data.substr(121,32);auto archive=directory+"/"+hex(digest)+".pack";
        auto package=inspect(archive);require(data==record(package),"catalog archive identity mismatch");
        require(digests.insert(digest).second,"duplicate catalog package");
        auto identity=hex(package.header.repository);if(repository.empty())repository=identity;
        require(identity==repository,"catalog contains multiple repositories");
        result.push_back({archive,hex(digest),package.header.kind,package.header.from,package.size,
            {package.header.to,identity,hex(package.header.head)}});
    }return result;
}
void publish(const std::string& temporary,const std::string& path){
    if(link(temporary.c_str(),path.c_str())!=0 && errno!=EEXIST)throw std::system_error(errno,std::generic_category(),"publish catalog file");
    // An existing immutable name is checked by the caller before success.
}
}
std::vector<BackupCatalog::Entry> BackupCatalog::entries(const std::string& input){
    auto directory=std::filesystem::absolute(input).lexically_normal().string();Lock lock(directory,false);return load(directory);
}
void BackupCatalog::add(const std::string& input,const std::string& archive){
    auto directory=std::filesystem::absolute(input).lexically_normal().string();Lock lock(directory,true);auto existing=load(directory);
    auto staging=directory+"/.catalog-add-XXXXXX";if(!mkdtemp(staging.data()))throw std::system_error(errno,std::generic_category(),"create catalog staging");
    struct Cleanup{std::string path;~Cleanup(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}}cleanup{staging};
    auto temporary=staging+"/package";
    {
        replay::File source(archive),destination(temporary,true);struct stat st{};
        require(fstat(source.fd,&st)==0 && st.st_size>=0,"cannot stat import archive");uint64_t remaining=st.st_size;
        while(remaining){auto count=std::min<uint64_t>(remaining,1024*1024);destination.write(source.read(count));remaining-=count;}
        source.eof();destination.sync();
    }
    auto package=inspect(temporary);auto identity=hex(package.header.repository);
    if(!existing.empty())require(existing.front().checkpoint.repository==identity,"catalog repository mismatch");
    const auto digest=hex(package.digest);bool duplicate=std::any_of(existing.begin(),existing.end(),[&](const auto& e){return e.digest==digest;});
    require(duplicate || existing.size()<4096,"catalog entry limit exceeded");
    fault("catalog_before_package");auto published=directory+"/"+digest+".pack";publish(temporary,published);
    require(record(inspect(published))==record(package),"existing catalog package mismatch");
    sync_directory(directory);fault("catalog_after_package");
    auto bytes=record(package),receipt=staging+"/entry";
    {replay::File file(receipt,true);file.write(bytes);file.sync();}
    fault("catalog_before_entry");auto entry=directory+"/"+hex(bytes.substr(bytes.size()-32))+".entry";publish(receipt,entry);
    require(read_record(entry)==bytes,"existing catalog record mismatch");sync_directory(directory);fault("catalog_after_entry");
}
}
