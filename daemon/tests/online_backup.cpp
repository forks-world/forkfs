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
                rejects([&]{store.forget_backup("base");});
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
        // Completion persistence failure must not report a durable checkpoint or
        // admit another worker; filesystem data and the exported pack stay usable.
        {
            Container store(db);Release guard;
            {std::lock_guard lock(mutex);captured=false;released=false;}
            store.start_backup("io-error",directory+"/io-error.pack");
            bool entered;
            {std::unique_lock lock(mutex);entered=cv.wait_for(lock,5s,[]{return captured;});}
            if(!entered){release();check(false);}
            std::filesystem::rename(db+"/BACKUP-TASKS",db+"/saved-tasks");
            std::filesystem::create_directory(db+"/BACKUP-TASKS");
            release();
            auto deadline=std::chrono::steady_clock::now()+5s;
            std::string state;
            do {
                state=store.backup_status("io-error");
                if(state.find("\"state\":\"running\"")==std::string::npos)break;
                std::this_thread::sleep_for(1ms);
            } while(std::chrono::steady_clock::now()<deadline);
            check(state.find("\"state\":\"interrupted\"")!=std::string::npos);
            check(state.find("\"checkpoint\":null")!=std::string::npos);
            rejects([&]{store.start_backup("blocked",directory+"/blocked");});
            Namespace fs(store);fs.verify();
        }
        std::filesystem::remove(db+"/BACKUP-TASKS");
        std::filesystem::rename(db+"/saved-tasks",db+"/BACKUP-TASKS");
        {
            Container store(db);
            check(store.backup_status("io-error").find("\"state\":\"interrupted\"")!=std::string::npos);
            store.forget_backup("io-error");
        }
        {
            Container preview(db,Container::OpenMode::RevisionPreview);
            rejects([&]{preview.start_backup("forbidden",directory+"/preview");});
        }
        check(!std::filesystem::exists(directory+"/preview"));
        std::filesystem::remove_all(directory);
        std::cout<<"snapshot isolation, foreground progress and worker ownership passed\n";
    } catch(const std::exception& e){release();std::cerr<<e.what()<<"\n";return 1;}
}
