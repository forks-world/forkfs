#include "namespace.h"
#include "compaction.h"
#include <atomic>
#include <filesystem>
#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using Bytes=std::vector<unsigned char>;
void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
int main() {
    char pattern[]="/tmp/ff-maintenance-XXXXXX";char* directory=mkdtemp(pattern);if(!directory)return 1;
    std::string path=std::string(directory)+"/store";
    try {
        forkfs::Container::create(path);
        {
            forkfs::Container container(path);forkfs::Namespace main(container);
            main.initialize();main.write("/file",Bytes{0});main.snapshot("base");main.fork("base","other");
            // Deterministic race: foreground write after capture must invalidate install.
            forkfs::Compaction stale(container,"main");stale.prepare();
            main.write("/file",Bytes{1});stale.build();auto before=std::filesystem::file_size(path);
            check(!stale.install(),"stale compaction overwrote current head");
            check(std::filesystem::file_size(path)==before,"stale result appended orphan output");
            check(main.read("/file")==Bytes{1},"concurrent write was lost");
            // An unrelated World write is not a false conflict.
            forkfs::Compaction independent(container,"main");independent.prepare();
            forkfs::Namespace other(container,"other");other.write("/file",Bytes{2});
            independent.build();check(independent.install(),"unrelated World caused compaction conflict");
            check(main.read("/file")==Bytes{1} && other.read("/file")==Bytes{2},"installation changed another World");
            container.start_compaction();container.start_compaction();
            std::atomic<bool> failed=false;
            auto writer=[&](forkfs::Namespace& fs,unsigned char base) {
                try {for(unsigned char i=0;i<70;++i)fs.write("/file",Bytes{static_cast<unsigned char>(base+i)});}
                catch(const std::exception& e){std::cerr<<e.what()<<'\n';failed=true;}
            };
            std::thread first(writer,std::ref(main),10),second(writer,std::ref(other),100);
            first.join();second.join();check(!failed,"foreground write/worker stress failed");
            check(container.wait_compaction(std::chrono::seconds(10)),"worker did not drain");
            check(container.compaction_error().empty(),"worker reported failure");
            check(main.read("/file")==Bytes{79} && other.read("/file")==Bytes{169},"worker lost latest values");
            check(main.run_count()<16 && other.run_count()<16,"worker did not reduce read amplification");
            check(forkfs::Namespace(container,"base",true).read("/file")==Bytes{0},"worker mutated retained revision");
            auto status=container.status();check(status.find("\"compaction_running\":true")!=std::string::npos,"missing worker status");
            check(status.find("\"compaction_completed\":0")==std::string::npos,"no background jobs completed");
            container.checkpoint();container.stop_compaction();container.stop_compaction();
            while(main.run_count()<16)main.write("/file",Bytes{79});
            container.start_compaction();container.stop_compaction(); // stop with queued/active work
            check(main.read("/file")==Bytes{79},"shutdown lost acknowledged write");
            container.start_compaction();check(container.wait_compaction(),"worker restart failed");container.stop_compaction();
            main.verify();other.verify();container.verify();
        }
        {
            forkfs::Container container(path);
            check(forkfs::Namespace(container).read("/file")==Bytes{79},"main head did not recover");
            check(forkfs::Namespace(container,"other").read("/file")==Bytes{169},"other head did not recover");
            check(forkfs::Namespace(container,"base",true).read("/file")==Bytes{0},"revision did not recover");
        }
        // Legacy capture uses a sequence fence until main has an immutable head.
        auto old=std::string(directory)+"/flat";forkfs::Container::create(old);
        {
            forkfs::Container container(old);forkfs::Namespace main(container);main.initialize();
            forkfs::Compaction task(container,"main");task.prepare();container.put("unrelated",Bytes{5});task.build();
            check(!task.install(),"legacy mutable capture lacked a sequence fence");
            main.compact();main.verify();
        }
        // A structurally invalid run fails only its job; the worker reports it
        // without poisoning the journal or preventing another World from writing.
        auto broken=std::string(directory)+"/bad-run";forkfs::Container::create(broken);
        int fd=open(broken.c_str(),O_RDWR|O_CLOEXEC);check(fd>=0,"cannot open error fixture");
        try {
            check(flock(fd,LOCK_EX|LOCK_NB)==0,"cannot lock error fixture");
            std::array<unsigned char,16> repo{};check(pread(fd,repo.data(),16,16)==16,"cannot read error fixture");
            forkfs::Journal journal(fd,repo);std::vector<forkfs::Mutation> values;
            Bytes manifest(12+16*32);memcpy(manifest.data(),"FFMAN002",8);manifest[8]=16;
            const std::string history("\0forkfs-history/",16);
            for(unsigned i=0;i<16;++i) {
                Bytes bad(16);memcpy(bad.data(),"BADRUN00",8);bad[12]=i;
                auto id=forkfs::Journal::object_id(bad);memcpy(manifest.data()+12+i*32,id.data(),32);
                values.push_back({history+"fixture/"+std::to_string(i),bad});
            }
            values.push_back({history+"world/broken",manifest});journal.transact(0,values);close(fd);fd=-1;
        } catch(...) {if(fd>=0)close(fd);throw;}
        {
            forkfs::Container container(broken);container.start_compaction();
            check(container.wait_compaction(),"failed job never drained");
            check(container.compaction_error().find("unsupported sorted run")!=std::string::npos,"missing worker error");
            check(container.status().find("\"compaction_failures\":1")!=std::string::npos,"missing failure counter");
            forkfs::Namespace main(container);main.initialize();main.write("/healthy",Bytes{6});
            check(main.read("/healthy")==Bytes{6},"failed World poisoned unrelated writes");
            container.stop_compaction();main.verify();container.verify();
        }
        std::filesystem::remove_all(directory);
        std::cout<<"stale-head rejection, unrelated writes, background queue, shutdown and restart passed\n";return 0;
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n';std::filesystem::remove_all(directory);return 1;
    }
}
