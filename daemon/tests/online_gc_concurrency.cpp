#include "level_store.h"
#include "namespace.h"
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <regex>
#include <unistd.h>

using namespace forkfs;
using namespace std::chrono_literals;
namespace {
std::mutex gate_mutex;
std::condition_variable gate_cv;
bool snapshot_captured=false, snapshot_released=false;
std::mutex mark_mutex;
std::condition_variable mark_cv;
bool mark_gate_enabled=false, mark_captured=false, mark_released=false;
std::mutex indexes_mutex;
std::condition_variable indexes_cv;
bool indexes_gate_enabled=false, indexes_captured=false, indexes_released=false;

#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (0)
void release_snapshot() {
    std::lock_guard lock(gate_mutex);snapshot_released=true;gate_cv.notify_all();
}
struct ReleaseSnapshot { ~ReleaseSnapshot(){release_snapshot();} };
struct DrainAsync {
    std::future<uint64_t>& backup;
    std::future<std::string>& gc;
    std::future<void>& writer;
    ~DrainAsync(){release_snapshot();if(backup.valid())backup.wait();if(gc.valid())gc.wait();if(writer.valid())writer.wait();}
};
void arm_mark_gate(){
    std::lock_guard lock(mark_mutex);mark_gate_enabled=true;mark_captured=false;mark_released=false;
}
bool wait_for_mark(std::chrono::seconds timeout){
    std::unique_lock lock(mark_mutex);return mark_cv.wait_for(lock,timeout,[]{return mark_captured;});
}
void release_mark_gate(){
    std::lock_guard lock(mark_mutex);mark_gate_enabled=false;mark_released=true;mark_cv.notify_all();
}
void arm_indexes_gate(){
    std::lock_guard lock(indexes_mutex);indexes_gate_enabled=true;indexes_captured=false;indexes_released=false;
}
bool wait_for_indexes(std::chrono::seconds timeout){
    std::unique_lock lock(indexes_mutex);return indexes_cv.wait_for(lock,timeout,[]{return indexes_captured;});
}
void release_indexes_gate(){
    std::lock_guard lock(indexes_mutex);indexes_gate_enabled=false;indexes_released=true;indexes_cv.notify_all();
}
uint64_t number(const std::string& json,const char* key){
    std::regex expression(std::string("\\\"")+key+"\\\":([0-9]+)");
    std::smatch match;CHECK(std::regex_search(json,match,expression));return std::stoull(match[1]);
}
std::vector<unsigned char> value(size_t size,unsigned salt){
    std::vector<unsigned char> result(size);
    for(size_t i=0;i<size;++i)result[i]=static_cast<unsigned char>((i*43+salt*23+(i>>7))%253);
    return result;
}
uint64_t data_bytes(const std::string& path){
    uint64_t bytes=0;for(const auto& entry:std::filesystem::directory_iterator(path))
        if(entry.path().extension()==".seg")bytes+=entry.file_size();return bytes;
}
}

namespace forkfs {
void test_backup_snapshot(){
    std::unique_lock lock(gate_mutex);snapshot_captured=true;gate_cv.notify_all();
    gate_cv.wait(lock,[]{return snapshot_released;});
}
void test_gc_marked(){
    std::unique_lock lock(mark_mutex);if(!mark_gate_enabled)return;
    mark_gate_enabled=false;mark_captured=true;mark_cv.notify_all();
    mark_cv.wait(lock,[]{return mark_released;});
}
void test_gc_indexes_published(){
    std::unique_lock lock(indexes_mutex);if(!indexes_gate_enabled)return;
    indexes_gate_enabled=false;indexes_captured=true;indexes_cv.notify_all();
    indexes_cv.wait(lock,[]{return indexes_released;});
}
}

int main(){
    auto temp=(std::filesystem::temp_directory_path()/"ff-online-gc-concurrency-XXXXXX").string();
    if(!mkdtemp(temp.data()))return 1;
    try {
        const auto repo=temp+"/repo",archive=temp+"/snapshot.pack",delta=temp+"/delta.pack",restored=temp+"/restored";
        const auto old_value=value(240*1024,1),new_value=value(180*1024,2);
        LevelStore::create(repo);
        {
            LevelStore store(repo);store.transact(0,{{"head",old_value}});store.verify();
        }
        // A valid but unindexed append tail makes this segment the deterministic
        // relocation source without corrupting any indexed chunk records.
        {
            std::ofstream tail(repo+"/DATA-1.seg",std::ios::binary|std::ios::app);
            CHECK(tail.good());std::vector<char> garbage(1024*1024,static_cast<char>(0xa7));
            tail.write(garbage.data(),static_cast<std::streamsize>(garbage.size()));CHECK(tail.good());
        }
        const auto source=repo+"/DATA-1.seg";const auto before=data_bytes(repo);
        std::future<uint64_t> backup_future;
        std::future<std::string> gc_future;
        std::future<void> writer_future;
        ReleaseSnapshot release_guard; // Destroyed before async futures on all exits.
        {
            std::lock_guard lock(gate_mutex);snapshot_captured=false;snapshot_released=false;
        }
        {
            LevelStore store(repo);
            DrainAsync drain{backup_future,gc_future,writer_future};
            backup_future=std::async(std::launch::async,[&]{return store.backup(archive);});
            bool captured;
            {std::unique_lock lock(gate_mutex);captured=gate_cv.wait_for(lock,10s,[]{return snapshot_captured;});}
            if(!captured){release_snapshot();backup_future.wait();CHECK(false);}

            gc_future=std::async(std::launch::async,[&]{return store.collect_step(8*1024*1024,128);});
            const bool gc_progressed=gc_future.wait_for(10s)==std::future_status::ready;
            if(!gc_progressed){release_snapshot();auto ignored=gc_future.get();(void)ignored;CHECK(false);}
            const auto report=gc_future.get();
            CHECK(number(report,"bytes_copied")<=8*1024*1024);
            CHECK(number(report,"chunks_moved")>0);
            CHECK(number(report,"segments_deleted")==0);
            CHECK(number(report,"pending_segments")>0);
            CHECK(std::filesystem::exists(source));
            CHECK(data_bytes(repo)>=before);

            // A location pin defers only unlink. Inventory must finish, then
            // repeated pin waits must stop reporting fictitious scan progress.
            bool waiting=false;
            for(unsigned i=0;i<512;++i){
                auto held=store.collect_step(8*1024*1024,128,64*1024*1024,16);
                CHECK(number(held,"physical_work_done")<=16);
                CHECK(number(held,"segments_deleted")==0);
                CHECK(std::filesystem::exists(source));
                if(held.find("\"physical_phase\":\"wait_pins\"")!=std::string::npos){waiting=true;break;}
            }
            CHECK(waiting);
            auto stalled=store.collect_step(8*1024*1024,128);
            CHECK(number(stalled,"physical_work_done")==0);
            CHECK(stalled.find("\"physical_pending\":true")!=std::string::npos);

            writer_future=std::async(std::launch::async,[&]{store.transact(store.sequence(),{{"head",new_value}});});
            const bool writer_progressed=writer_future.wait_for(10s)==std::future_status::ready;
            if(!writer_progressed){release_snapshot();writer_future.get();CHECK(false);}
            writer_future.get();
            CHECK(store.object(store.root("head"))==new_value);
            store.verify();

            release_snapshot();
            CHECK(backup_future.get()==1); // Archive's snapshot precedes the concurrent commit.
            auto cleanup=store.collect_step(8*1024*1024,128);
            CHECK(number(cleanup,"segments_deleted")>0);
            CHECK(!std::filesystem::exists(source));
            CHECK(store.object(store.root("head"))==new_value);store.verify();
            store.export_replay(1,delta);
        }
        std::filesystem::remove_all(repo);
        CHECK(!std::filesystem::exists(repo));
        LevelStore::replay_archive(archive,restored);
        {LevelStore base(restored);CHECK(base.object(base.root("head"))==old_value);base.verify();}
        LevelStore::replay_archive(delta,restored);
        {LevelStore latest(restored);CHECK(latest.object(latest.root("head"))==new_value);latest.verify();}

        // A writer revives an object after GC marks it unreachable but before
        // the prune gate. The sequence CAS must discard that stale mark set.
        const auto race_repo=temp+"/mark-race";LevelStore::create(race_repo);
        {
            LevelStore store(race_repo);auto head=value(4096,71),orphan=value(8192,72);
            const auto orphan_id=Journal::object_id(orphan);
            store.transact(0,{{"head",head}});
            store.transact(store.sequence(),{{"temporary",orphan}});
            store.transact(store.sequence(),{{"temporary",std::nullopt}});
            const auto marked_sequence=store.sequence();
            std::future<std::string> gc;
            arm_mark_gate();
            try {
                gc=std::async(std::launch::async,[&]{return store.collect_step(8*1024*1024,128);});
                if(!wait_for_mark(10s)){release_mark_gate();gc.wait();CHECK(false);}
                store.transact(marked_sequence,{{"revived",std::nullopt,orphan_id}});
                release_mark_gate();
                CHECK(gc.wait_for(10s)==std::future_status::ready);
                auto report=gc.get();
                CHECK(number(report,"objects_deleted")==0);
                CHECK(report.find("\"logical_sweep_skipped\":true")!=std::string::npos);
                CHECK(store.root("revived")==orphan_id);
                CHECK(store.object(orphan_id)==orphan);store.verify();
            } catch(...) {
                release_mark_gate();if(gc.valid())gc.wait();throw;
            }
        }

        // Registering a manifest lease after the mark snapshot changes its
        // epoch without changing the transaction sequence. The first sweep
        // must retry, and later sweeps must preserve that manifest's closure.
        const auto lease_repo=temp+"/manifest-lease";Container::create(lease_repo);
        const auto old_file=value(24*1024,81),new_file=value(24*1024,82);
        std::array<unsigned char,32> old_manifest{};
        {
            Container container(lease_repo);Namespace fs(container);fs.initialize();fs.write("/held",old_file);
        }
        {
            Journal journal(lease_repo);
            old_manifest=journal.root_object(std::string("\0forkfs-history/world/main",26));
            CHECK(journal.get_object(old_manifest).size()>0);
        }
        {
            Container container(lease_repo);Namespace fs(container);fs.write("/held",new_file);
        }
        const auto old_file_id=Journal::object_id(old_file);
        {
            LevelStore store(lease_repo);const auto sequence=store.sequence();
            CHECK(store.object(old_file_id)==old_file);
            std::future<std::string> gc;arm_mark_gate();
            try {
                gc=std::async(std::launch::async,[&]{return store.collect_step(8*1024*1024,128);});
                if(!wait_for_mark(10s)){release_mark_gate();gc.wait();CHECK(false);}
                auto lease=store.pin_manifest(old_manifest);
                release_mark_gate();
                CHECK(gc.wait_for(10s)==std::future_status::ready);
                auto report=gc.get();
                CHECK(number(report,"objects_deleted")==0);
                CHECK(report.find("\"logical_sweep_skipped\":true")!=std::string::npos);
                CHECK(store.sequence()==sequence);
                CHECK(store.object(old_file_id)==old_file);

                report=store.collect_step(8*1024*1024,128);
                CHECK(report.find("\"logical_sweep_skipped\":false")!=std::string::npos);
                CHECK(store.sequence()==sequence);
                CHECK(store.object(old_file_id)==old_file);store.verify();
                lease.reset();
                for(unsigned i=0;i<512;++i) {
                    report=store.collect_step(8*1024*1024,128);store.verify();
                    if(report.find("\"mark_pending\":true")==std::string::npos && number(report,"objects_deleted")==0 && number(report,"chunks_moved")==0 &&
                       number(report,"segments_deleted")==0 && number(report,"pending_segments")==0)break;
                }
                bool reclaimed=false;try{(void)store.object(old_file_id);}catch(const std::exception&){reclaimed=true;}
                CHECK(reclaimed);CHECK(store.sequence()==sequence);
            } catch(...) {release_mark_gate();if(gc.valid())gc.wait();throw;}
        }

        // A concurrent foreground failure can poison the owner without
        // advancing the sequence. GC must recheck health before publishing
        // its stale unreachable-object set.
        const auto poison_repo=temp+"/poison-race";LevelStore::create(poison_repo);
        const auto poison_head=value(4096,91),poison_orphan=value(8192,92);
        const auto poison_orphan_id=Journal::object_id(poison_orphan);
        uint64_t poison_sequence=0;size_t poison_objects=0,poison_roots=0;
        {
            LevelStore store(poison_repo);
            store.transact(0,{{"head",poison_head}});
            store.transact(store.sequence(),{{"temporary",poison_orphan}});
            store.transact(store.sequence(),{{"temporary",std::nullopt}});
            poison_sequence=store.sequence();poison_objects=store.objects();poison_roots=store.roots();
            std::future<bool> gc;arm_mark_gate();
            try {
                gc=std::async(std::launch::async,[&]{
                    try{(void)store.collect_step(8*1024*1024,128);return false;}
                    catch(const std::exception&){return true;}
                });
                if(!wait_for_mark(10s)){release_mark_gate();gc.wait();CHECK(false);}
                store.poison();release_mark_gate();
                CHECK(gc.wait_for(10s)==std::future_status::ready);
                CHECK(gc.get());
                bool barrier_rejected=false;try{store.barrier();}catch(const std::exception&){barrier_rejected=true;}
                CHECK(barrier_rejected);
            } catch(...) {release_mark_gate();if(gc.valid())gc.wait();throw;}
        }
        {
            LevelStore recovered(poison_repo);
            CHECK(recovered.sequence()==poison_sequence);
            CHECK(recovered.objects()==poison_objects && recovered.roots()==poison_roots);
            CHECK(recovered.root("head")==Journal::object_id(poison_head));
            CHECK(recovered.object(poison_orphan_id)==poison_orphan);
            recovered.verify();
        }

        // Exercise verification against one atomic DB snapshot while the
        // durable M/format and G index are visible before the cached format is
        // updated in memory.
        const auto format_repo=temp+"/format-publication";LevelStore::create(format_repo);
        const auto format_head=value(32*1024,103);
        {
            LevelStore store(format_repo);store.transact(0,{{"head",format_head}});store.verify();
        }
        {
            std::ofstream tail(format_repo+"/DATA-1.seg",std::ios::binary|std::ios::app);
            CHECK(tail.good());std::vector<char> garbage(1024*1024,static_cast<char>(0x6d));
            tail.write(garbage.data(),static_cast<std::streamsize>(garbage.size()));CHECK(tail.good());
        }
        {
            LevelStore store(format_repo);const auto head_id=store.root("head");
            std::future<std::string> gc;arm_indexes_gate();
            try {
                gc=std::async(std::launch::async,[&]{return store.collect_step(8*1024*1024,128);});
                if(!wait_for_indexes(10s)){release_indexes_gate();gc.wait();CHECK(false);}
                store.verify();
                CHECK(store.root("head")==head_id && store.object(head_id)==format_head);
                release_indexes_gate();
                CHECK(gc.wait_for(10s)==std::future_status::ready);
                (void)gc.get();store.verify();
                CHECK(store.root("head")==head_id && store.object(head_id)==format_head);
            } catch(...) {release_indexes_gate();if(gc.valid())gc.wait();throw;}
        }
        std::filesystem::remove_all(temp);
        std::cout<<"backup snapshot location pin, foreground write progress and online GC retirement passed\n";
    }catch(const std::exception& e){release_snapshot();std::cerr<<e.what()<<'\n';std::filesystem::remove_all(temp);return 1;}
}
