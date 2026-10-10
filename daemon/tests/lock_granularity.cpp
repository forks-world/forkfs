#include "level_store.h"
#include <condition_variable>
#include <filesystem>
#include <future>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace forkfs;
using namespace std::chrono_literals;
namespace {
enum class Stage {prepare,verify,compact};
struct Gate {
    std::mutex mutex;std::condition_variable cv;
    Stage stage=Stage::prepare;bool entered=false,released=false;
    void arm(Stage next){std::lock_guard lock(mutex);stage=next;entered=false;released=false;}
    void pause(Stage here){std::unique_lock lock(mutex);if(stage!=here)return;entered=true;cv.notify_all();cv.wait(lock,[&]{return released;});}
    bool wait(){std::unique_lock lock(mutex);return cv.wait_for(lock,5s,[&]{return entered;});}
    void release(){std::lock_guard lock(mutex);released=true;cv.notify_all();}
} gate;
thread_local bool targeted=false;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
// Always release paused work before checking a timeout or rethrowing, including
// the intentionally failing old implementation. No timing race picks the winner.
template<class Paused,class Concurrent>
void while_paused(Stage stage,Paused paused,Concurrent concurrent) {
    gate.arm(stage);
    auto worker=std::async(std::launch::async,[&]{targeted=true;paused();});
    bool entered=gate.wait();
    if(!entered){gate.release();worker.get();check(false,"test hook did not run");}
    auto other=std::async(std::launch::async,concurrent);
    bool progressed=other.wait_for(5s)==std::future_status::ready;
    gate.release();std::exception_ptr worker_error,other_error;
    try{worker.get();}catch(...){worker_error=std::current_exception();}
    try{other.get();}catch(...){other_error=std::current_exception();}
    check(progressed,"writer blocked by unrelated preparation/maintenance");
    if(worker_error)std::rethrow_exception(worker_error);
    if(other_error)std::rethrow_exception(other_error);
}
}
namespace forkfs {
void test_prepared(){if(targeted)gate.pause(Stage::prepare);}
void test_verify_snapshot(){if(targeted)gate.pause(Stage::verify);}
void test_compact(){if(targeted)gate.pause(Stage::compact);}
}
int main() {
    auto pattern=(std::filesystem::temp_directory_path()/"ff-locks-XXXXXX").string();auto directory=mkdtemp(pattern.data());if(!directory)return 1;
    try {
        auto path=std::string(directory)+"/store";LevelStore::create(path);
        {
            LevelStore store(path);store.transact(0,{{"head",std::vector<unsigned char>{1}}});
            // Preparation cannot reserve the writer. The winning commit makes
            // the prepared transaction stale, which must publish nothing.
            auto sequence=store.sequence();bool conflict=false;
            while_paused(Stage::prepare,[&]{
                try{store.transact(sequence,{{"loser",std::vector<unsigned char>{2}}});}
                catch(const std::runtime_error& e){conflict=std::string(e.what()).starts_with("transaction conflict:");}
            },[&]{store.transact(sequence,{{"winner",std::vector<unsigned char>{3}}});});
            check(conflict && !store.contains("loser"),"stale prepared write was published");
            check(store.sequence()==sequence+1 && store.roots()==2 && store.objects()==2,"conflict changed counters");
            // Commit after snapshot capture, before any scan/counter reads.
            // Verification must use the old snapshot counters, not atomics.
            while_paused(Stage::verify,[&]{store.verify();},[&]{store.transact(store.sequence(),{{"after-snapshot",std::vector<unsigned char>{4}}});});
            while_paused(Stage::compact,[&]{store.compact();},[&]{store.transact(store.sequence(),{{"during-maintenance",std::vector<unsigned char>{5}}});});
            std::atomic<bool> done=false;std::exception_ptr failure,compaction_failure;
            std::thread verifier([&]{try{while(!done)store.verify();}catch(...){failure=std::current_exception();}});
            std::thread compactor([&]{try{for(unsigned i=0;i<5;++i)store.compact();}catch(...){compaction_failure=std::current_exception();}});
            try {
                for(unsigned i=0;i<100;++i)store.transact(store.sequence(),{{"head",std::vector<unsigned char>{static_cast<unsigned char>(i),42}}});
            }catch(...){done=true;verifier.join();compactor.join();throw;}
            done=true;verifier.join();compactor.join();if(compaction_failure)std::rethrow_exception(compaction_failure);if(failure)std::rethrow_exception(failure);
            store.verify();store.compact();check(store.roots()==4,"unexpected final roots");
            bool poisoned_rejected=false;
            while_paused(Stage::verify,[&]{
                try{store.verify();}catch(const std::runtime_error& e){poisoned_rejected=std::string(e.what())=="LevelDB store requires reopen after write failure";}
            },[&]{store.poison();});
            check(poisoned_rejected,"scan hid a concurrent poisoned write");
        }
        {LevelStore store(path);store.verify();check(store.roots()==4 && !store.contains("loser"),"reopen changed roots");}
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(directory);return 1;}
    std::filesystem::remove_all(directory);return 0;
}
