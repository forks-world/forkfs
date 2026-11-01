#include "namespace.h"
#include "compaction.h"
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

using Bytes=std::vector<unsigned char>;
using Id=std::array<unsigned char,32>;
const std::string prefix("\0forkfs/",8),history("\0forkfs-history/",16);
void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
void u32(Bytes& b,size_t at,uint32_t n) {for(unsigned i=0;i<4;++i)b[at+i]=static_cast<unsigned char>(n>>(8*i));}
std::string hex(const Id& id) {
    std::string result;const char* chars="0123456789abcdef";
    for(auto c:id){result+=chars[c>>4];result+=chars[c&15];}return result;
}
void fixture(const std::string& path) {
    forkfs::Container::create(path);
    {forkfs::Container c(path);forkfs::Namespace(c).initialize();}
    int fd=open(path.c_str(),O_RDWR|O_CLOEXEC);check(fd>=0,"fixture open failed");
    try {
        check(flock(fd,LOCK_EX|LOCK_NB)==0,"fixture lock failed");
        Id empty=forkfs::Journal::object_id(Bytes{});
        std::array<unsigned char,16> repo{};check(pread(fd,repo.data(),16,16)==16,"fixture identity failed");
        forkfs::Journal journal(fd,repo);std::map<std::string,Id> entries;
        for(const auto& key:journal.keys(prefix))entries[key]=journal.root_object(key);
        for(unsigned i=1;i<=14000;++i) {
            Id bytes{};for(unsigned n=0;n<4;++n)bytes[12+n]=static_cast<unsigned char>(i>>(8*n));
            auto id=hex(bytes).substr(0,32);entries.emplace(prefix+"allocated/"+id,empty);
        }
        std::vector<Id> runs;Bytes part(12);memcpy(part.data(),"FFRUN001",8);uint32_t count=0;
        auto flush=[&] {
            if(!count)return;u32(part,8,count);auto id=forkfs::Journal::object_id(part);
            journal.put(history+"object/"+hex(id),part);runs.push_back(id);
            part.assign(12,0);memcpy(part.data(),"FFRUN001",8);count=0;
        };
        for(const auto& [key,id]:entries) {
            if(part.size()+40+key.size()>200*1024)flush();
            size_t at=part.size();part.resize(at+40+key.size());u32(part,at,1);u32(part,at+4,key.size());
            memcpy(part.data()+at+8,key.data(),key.size());memcpy(part.data()+at+8+key.size(),id.data(),32);++count;
        }
        flush();check(runs.size()<16,"fixture has too many source runs");
        Bytes manifest(12+runs.size()*32);memcpy(manifest.data(),"FFMAN002",8);u32(manifest,8,runs.size());
        for(size_t i=0;i<runs.size();++i)memcpy(manifest.data()+12+i*32,runs[i].data(),32);
        journal.put(history+"world/main",manifest);close(fd);fd=-1;
    } catch(...) {if(fd>=0)close(fd);throw;}
    {forkfs::Container c(path);forkfs::Namespace(c).snapshot("original");}
}
int main(int argc,char** argv) {
    try {
        if(argc==3 && std::string(argv[1])=="--generate") {fixture(argv[2]);return 0;}
        char pattern[]="/tmp/ff-staged-XXXXXX";char* directory=mkdtemp(pattern);check(directory,"temporary directory failed");
        try {
            auto path=std::string(directory)+"/large";fixture(path);
            {
                forkfs::Container c(path);forkfs::Namespace main(c);
                auto old=main.run_count();forkfs::Compaction task(c,"main");task.prepare();task.build();
                check(task.output_runs()>16,"fixture did not exceed single-batch compaction size");
                check(task.install(),"large staged compaction failed");
                check(main.run_count()==task.output_runs(),"final manifest was not published");
                check(forkfs::Namespace(c,"original",true).run_count()==old,"staging mutated revision manifest");
                main.verify();c.checkpoint();
            }
            {
                forkfs::Container c(path);forkfs::Namespace main(c);main.verify();c.verify();
                check(main.list("/").empty(),"large compaction changed namespace");
            }
            auto raced=std::string(directory)+"/race";fixture(raced);
            {
                forkfs::Container c(raced);forkfs::Namespace main(c);
                forkfs::Compaction task(c,"main");task.prepare();task.build();auto old=main.run_count();
                check(task.stage_next()==forkfs::Compaction::StageResult::Staged,"first output batch did not stage");
                check(main.run_count()==old,"staging exposed a partial manifest");
                main.write("/new",Bytes{42});auto before=std::filesystem::file_size(raced);
                check(!task.install(),"staged candidate overwrote a concurrent write");
                check(std::filesystem::file_size(raced)==before,"stale staged job kept appending");
                check(main.read("/new")==Bytes{42},"concurrent value lost");
                forkfs::Compaction retry(c,"main");retry.prepare();retry.build();check(retry.install(),"staged retry failed");
                check(main.read("/new")==Bytes{42},"retry lost concurrent value");
                check(forkfs::Namespace(c,"original",true).list("/").empty(),"retry changed revision");
                c.checkpoint();
            }
            std::filesystem::remove_all(directory);
            std::cout<<"large staged output, atomic head, stale mid-job rejection and recovery passed\n";
        } catch(...) {std::filesystem::remove_all(directory);throw;}
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
