#include "namespace.h"
#include "level_store.h"
#include <condition_variable>
#include <filesystem>
#include <future>
#include <iostream>
#include <unistd.h>
using namespace forkfs;
using namespace std::chrono_literals;
namespace {
std::mutex mutex;
std::condition_variable cv;
bool captured=false,released=false;
void release(){std::lock_guard lock(mutex);released=true;cv.notify_all();}
struct Release {~Release(){release();}};
void check(bool ok){if(!ok)throw std::runtime_error("online backup invariant failed");}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected);}
}
namespace forkfs {
void test_backup_snapshot(){
    std::unique_lock lock(mutex);captured=true;cv.notify_all();cv.wait(lock,[]{return released;});
}
}
int main(){
    auto directory=(std::filesystem::temp_directory_path()/"ff-online-gate-XXXXXX").string();
    if(!mkdtemp(directory.data()))return 1;
    try {
        auto db=directory+"/db",base=directory+"/base",delta=directory+"/delta",restored=directory+"/restored";
        Container::create(db);
        {
            Container store(db);Release guard;
            Namespace fs(store);fs.initialize();fs.write("/file",{'o','l','d'});
            auto running=store.start_backup("base",base);
            bool entered;
            {std::unique_lock lock(mutex);entered=cv.wait_for(lock,5s,[]{return captured;});}
            if(!entered){release();check(false);}
            auto concurrent=std::async(std::launch::async,[&]{
                check(store.backup_status("base")==running);
                check(store.start_backup("base",base)==running);
                rejects([&]{store.start_backup("other",directory+"/other");});
                rejects([&]{store.start_backup("base",directory+"/different");});
                fs.write("/file",{'n','e','w'});check(fs.read("/file")==std::vector<unsigned char>({'n','e','w'}));
            });
            bool progressed=concurrent.wait_for(5s)==std::future_status::ready;
            release();concurrent.get();check(progressed);
            // Destruction joins the worker before destroying its journal.
        }
        auto sequence=LevelStore::replay_archive(base,restored);
        {Container store(restored);Namespace fs(store);check(fs.read("/file")==std::vector<unsigned char>({'o','l','d'}));}
        {
            Container store(db);
            store.start_backup("delta",delta,sequence);
        }
        check(LevelStore::replay_archive(delta,restored)>sequence);
        {Container store(restored);Namespace fs(store);check(fs.read("/file")==std::vector<unsigned char>({'n','e','w'}));fs.verify();}
        {
            Container preview(db,Container::OpenMode::RevisionPreview);
            rejects([&]{preview.start_backup("forbidden",directory+"/preview");});
        }
        check(!std::filesystem::exists(directory+"/preview"));
        std::filesystem::remove_all(directory);
        std::cout<<"snapshot isolation, foreground progress and worker ownership passed\n";
    } catch(const std::exception& e){release();std::cerr<<e.what()<<"\n";return 1;}
}
