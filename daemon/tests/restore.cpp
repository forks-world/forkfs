#include "revision.h"
#include "namespace.h"
#include "compaction.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace forkfs;
using Bytes=std::vector<unsigned char>;
void check(bool ok){if(!ok)throw std::runtime_error("restore check failed");}
template<class F> void rejects(F operation){bool rejected=false;try{operation();}catch(const std::exception&){rejected=true;}check(rejected);}
int main(){
    auto pattern=(std::filesystem::temp_directory_path()/"ff-restore-XXXXXX").string();auto tmp=mkdtemp(pattern.data());if(!tmp)return 1;
    auto path=std::string(tmp)+"/store";
    try {
        Container::create(path);
        {
            Container store(path);Namespace fs(store);fs.initialize();fs.write("/f",Bytes{'a'});fs.snapshot("r1");fs.fork("r1","work");
            Namespace history(store,"r1",true),work(store,"work");
            auto historical=history.open("/f"),other=work.open("/f",Namespace::Access::ReadWrite);
            auto old=fs.open("/f",Namespace::Access::ReadWrite),duplicate=fs.dup(old);
            check(fs.try_lock(old,Namespace::Lock::Exclusive));
            auto directory=fs.open_directory("/"),historical_directory=history.open_directory("/");
            auto empty=fs.open_directory("/");fs.read_directory(empty,1);
            fs.remove("/f",false);fs.write("/new",Bytes{'b'});fs.snapshot("r2");
            auto orphan=std::string("\0forkfs-history/",16)+"orphan/main/"+fs.handle_stat(old).id+"/inode";
            check(!store.get(orphan).empty());
            // Failed restore must preserve old access and durable generation.
            rejects([&]{fs.restore("missing");});check(fs.handle_read(old,0,1)==Bytes{'a'});
            rejects([&]{history.restore("r1");});
            fs.restore("r1");check(fs.read("/f")==Bytes{'a'});rejects([&]{fs.stat("/new");});
            rejects([&]{fs.handle_write(old,0,Bytes{'x'});});rejects([&]{fs.handle_read(duplicate,0,1);});
            rejects([&]{fs.read_directory(directory,1);});rejects([&]{fs.rewind_directory(empty);});
            rejects([&]{store.get(orphan);});
            check(history.handle_read(historical,0,1)==Bytes{'a'} && work.handle_read(other,0,1)==Bytes{'a'});
            check(history.read_directory(historical_directory,1).names==std::vector<std::string>{"f"});
            auto fresh=fs.open("/f",Namespace::Access::ReadWrite);check(fs.try_lock(fresh,Namespace::Lock::Exclusive));
            fs.close(fresh);history.close(historical);work.close(other);
            // Restore to the identical head still changes the access generation.
            Compaction stale(store,"main");stale.prepare();stale.build();auto cursor=fs.open_directory("/");
            fs.restore("r1");check(stale.stage_next()==Compaction::StageResult::Stale);check(!stale.install());
            rejects([&]{fs.read_directory(cursor,1);});
            auto generation=store.get(std::string("\0forkfs-history/",16)+"generation/main");
            check(generation==Bytes({2,0,0,0,0,0,0,0}));
            fs.write("/f",Bytes{'c'});fs.snapshot("r3");
            auto r1_descriptor=store.get(std::string("\0forkfs-history/",16)+"revision/r1");
            auto r3_descriptor=store.get(std::string("\0forkfs-history/",16)+"revision/r3");
            auto parent=Journal::object_id(r1_descriptor);
            check(RevisionRecord::decode(r3_descriptor).parents==std::vector<RevisionRecord::Id>{parent});
            Namespace r2(store,"r2",true);check(r2.read("/new")==Bytes{'b'});fs.verify();store.verify();
        }
        {
            Container store(path);Namespace fs(store),r1(store,"r1",true),r2(store,"r2",true),r3(store,"r3",true);
            check(fs.read("/f")==Bytes{'c'} && r1.read("/f")==Bytes{'a'} && r2.read("/new")==Bytes{'b'} && r3.read("/f")==Bytes{'c'});
            check(store.get(std::string("\0forkfs-history/",16)+"generation/main")==Bytes({2,0,0,0,0,0,0,0}));
            fs.restore("r1");
            check(store.get(std::string("\0forkfs-history/",16)+"generation/main")==Bytes({3,0,0,0,0,0,0,0}));
            fs.verify();store.verify();
        }
        std::filesystem::remove_all(tmp);std::cout<<"restore generation, orphan release, compaction fence and reopen passed\n";
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
