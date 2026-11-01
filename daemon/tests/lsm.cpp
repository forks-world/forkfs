#include "namespace.h"
#include <filesystem>
#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <stdexcept>
#include <unistd.h>

using Bytes=std::vector<unsigned char>;
void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
int main() {
    char pattern[]="/tmp/ff-lsm-XXXXXX";char* directory=mkdtemp(pattern);if(!directory)return 1;
    std::string path=std::string(directory)+"/store";
    try {
        forkfs::Container::create(path);
        {
            forkfs::Container container(path);forkfs::Namespace main(container);
            main.initialize();main.mkdir("/files");main.write("/file",Bytes{0});main.snapshot("base");main.fork("base","branch");
            forkfs::Namespace branch(container,"branch"),base(container,"base",true);
            for(int i=1;i<=80;++i) {
                branch.write("/file",Bytes{static_cast<unsigned char>(i)});
                check(branch.read("/file")==Bytes{static_cast<unsigned char>(i)},"newest run did not win");
                check(base.read("/file")==Bytes{0} && main.read("/file")==Bytes{0},"branch leaked into shared base");
                check(branch.run_count()<=16,"automatic compaction failed to bound run count");
                if(i==10)branch.snapshot("ten");
                if(i==35)branch.snapshot("thirty-five");
            }
            for(int i=0;i<160;++i)branch.write("/files/f"+std::to_string(i),Bytes{static_cast<unsigned char>(i)});
            check(branch.list("/files").size()==160,"directory scan lost keys across runs");
            branch.compact();check(branch.run_count()<=3,"image partitioning produced too many runs");
            auto before=std::filesystem::file_size(path);
            branch.write("/file",Bytes{81});auto appended=std::filesystem::file_size(path)-before;
            check(appended<8192,"single-key update rewrote full namespace metadata");
            branch.snapshot("before-delete");branch.remove("/file",false);
            branch.compact();
            bool absent=false;try{branch.read("/file");}catch(...){absent=true;}
            check(absent,"compaction resurrected deleted file");
            check(forkfs::Namespace(container,"before-delete",true).read("/file")==Bytes{81},"old revision lost tombstoned content");
            branch.write("/file",Bytes{99});branch.rename("/file","/renamed");branch.compact();
            check(branch.read("/renamed")==Bytes{99},"rename/compaction lost content");
            check(forkfs::Namespace(container,"ten",true).read("/file")==Bytes{10},"old delta revision changed after compaction");
            check(forkfs::Namespace(container,"thirty-five",true).read("/file")==Bytes{35},"old compacted revision changed");
            branch.verify();main.verify();base.verify();container.checkpoint();
        }
        {
            forkfs::Container container(path);forkfs::Namespace branch(container,"branch");
            check(branch.read("/renamed")==Bytes{99},"head did not recover");
            check(branch.list("/files").size()==160,"range scan did not recover");
            check(forkfs::Namespace(container,"ten",true).read("/file")==Bytes{10},"historical runs did not recover");
            branch.verify();container.verify();
        }
        // Build a preceding FFMAN001 fixture through the trusted journal API.
        // It must stay readable and become a shared base during incremental writes.
        std::string legacy_path=std::string(directory)+"/legacy";
        forkfs::Container::create(legacy_path);
        {forkfs::Container container(legacy_path);forkfs::Namespace(container).initialize();}
        int fd=open(legacy_path.c_str(),O_RDWR|O_CLOEXEC);
        check(fd>=0,"cannot open legacy fixture");
        try {
            check(flock(fd,LOCK_EX|LOCK_NB)==0,"cannot lock legacy fixture");
            std::array<unsigned char,16> repo{};check(pread(fd,repo.data(),16,16)==16,"cannot read fixture identity");
            forkfs::Journal journal(fd,repo);auto keys=journal.keys(std::string("\0forkfs/",8));
            Bytes manifest(12);memcpy(manifest.data(),"FFMAN001",8);
            auto store32=[](unsigned char* p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=static_cast<unsigned char>(n>>(8*i));};
            store32(manifest.data()+8,keys.size());
            for(const auto& key:keys) {
                auto at=manifest.size();manifest.resize(at+4+key.size()+32);store32(manifest.data()+at,key.size());
                memcpy(manifest.data()+at+4,key.data(),key.size());auto id=journal.root_object(key);
                memcpy(manifest.data()+at+4+key.size(),id.data(),32);
            }
            journal.transact(journal.sequence(),{{std::string("\0forkfs-history/",16)+"world/legacy",manifest}});
            close(fd);fd=-1;
        } catch(...) {if(fd>=0)close(fd);throw;}
        {
            forkfs::Container container(legacy_path);forkfs::Namespace legacy(container,"legacy");
            auto original=legacy.stat("/").id;legacy.snapshot("full-map");legacy.write("/new",Bytes{7});
            check(legacy.run_count()==2 && legacy.read("/new")==Bytes{7},"legacy base did not migrate incrementally");
            check(forkfs::Namespace(container,"full-map",true).list("/").empty(),"legacy revision was mutated");
            legacy.compact();check(legacy.run_count()==1 && legacy.stat("/").id==original,"legacy compaction lost identity");
            legacy.verify();container.checkpoint();
        }
        std::filesystem::remove_all(directory);
        std::cout<<"incremental runs, bounded compaction, tombstones, revisions and restart passed\n";return 0;
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n';std::filesystem::remove_all(directory);return 1;
    }
}
