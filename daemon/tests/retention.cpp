#include "namespace.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("retention check failed at line "+std::to_string(line));}
template<class F> static void rejects(F f,int line=__builtin_LINE()){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,line);}
int main(){
    char pattern[]="/tmp/ff-retention-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
    auto path=std::string(tmp)+"/db";Bytes abandoned(65536,0xa7);std::string old_inode;
    try {
        Container::create(path);
        {
            Container store(path);Namespace fs(store);fs.initialize();fs.write("/f",Bytes{'K'});fs.snapshot("base");fs.fork("base","discard");
            Namespace work(store,"discard"),revision(store,"base",true);work.write("/unique",abandoned);old_inode=work.stat("/unique").id;work.snapshot("branch");
            auto blocked=[&]{rejects([&]{fs.drop_world("discard");});rejects([&]{fs.forget_revision("branch");});check(Namespace(store,"branch",true).read("/unique")==abandoned);};
            auto handle=revision.open("/f");blocked();revision.close(handle);
            handle=work.open("/unique",Namespace::Access::Write);work.remove("/unique",false);blocked(); // orphan handles also block
            work.close(handle);
            auto cursor=fs.open_directory("/");blocked();cursor={};
            rejects([&]{revision.forget_revision("branch");});rejects([&]{work.drop_world("discard");});rejects([&]{fs.drop_world("main");});
            fs.drop_world("discard");fs.forget_revision("branch");check(store.collect_garbage()>0);
            check(fs.read("/f")==Bytes{'K'});revision.verify();fs.verify();store.verify();
            fs.fork("base","discard");Namespace fresh(store,"discard");fresh.write("/unique",Bytes{'N'});check(fresh.stat("/unique").id!=old_inode);
        }
        {
            forkfs::Journal journal(path);rejects([&]{journal.get_object(forkfs::Journal::object_id(abandoned));});
        }
        // LevelDB delegates compaction internally. The explicit background
        // compactor exists only for the single-file compatibility backend.
        {
            auto legacy_path=path+"-legacy";Container::create_legacy(legacy_path);Container store(legacy_path);Namespace fs(store);
            fs.initialize();fs.snapshot("base");fs.fork("base","discard");store.start_compaction();
            rejects([&]{fs.drop_world("discard");});rejects([&]{fs.forget_revision("base");});store.stop_compaction();
            fs.drop_world("discard");fs.forget_revision("base");fs.verify();store.verify();
        }
        std::filesystem::remove_all(tmp);std::cout<<"retention idle guards, physical payload reclamation, surviving revisions and allocation markers passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
