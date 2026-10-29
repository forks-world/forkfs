#include "level_store.h"
#include "namespace.h"
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <exception>
#include <regex>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

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
std::mutex demand_mutex;
std::condition_variable demand_cv;
bool demand_gate_enabled=false,demand_captured=false,demand_released=false;
std::mutex work_boundary_mutex;
std::condition_variable work_boundary_cv;
bool work_boundary_enabled=false,work_boundary_captured=false,work_boundary_released=false;
uint64_t work_boundary_target=0;

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
void arm_demand_gate(){
    std::lock_guard lock(demand_mutex);demand_gate_enabled=true;demand_captured=false;demand_released=false;
}
bool wait_for_demand(std::chrono::seconds timeout){
    std::unique_lock lock(demand_mutex);return demand_cv.wait_for(lock,timeout,[]{return demand_captured;});
}
void release_demand_gate(){
    std::lock_guard lock(demand_mutex);demand_gate_enabled=false;demand_released=true;demand_cv.notify_all();
}
void arm_work_boundary(uint64_t target){
    std::lock_guard lock(work_boundary_mutex);work_boundary_enabled=true;work_boundary_captured=false;work_boundary_released=false;work_boundary_target=target;
}
bool wait_for_work_boundary(std::chrono::seconds timeout){
    std::unique_lock lock(work_boundary_mutex);return work_boundary_cv.wait_for(lock,timeout,[]{return work_boundary_captured;});
}
void release_work_boundary(){
    std::lock_guard lock(work_boundary_mutex);work_boundary_enabled=false;work_boundary_released=true;work_boundary_cv.notify_all();
}
uint64_t number(const std::string& json,const char* key){
    std::regex expression(std::string("\\\"")+key+"\\\":([0-9]+)");
    std::smatch match;CHECK(std::regex_search(json,match,expression));return std::stoull(match[1]);
}
bool flag(const std::string& json,const char* key){
    std::regex expression(std::string("\\\"")+key+"\\\":(true|false)");
    std::smatch match;CHECK(std::regex_search(json,match,expression));return match[1]=="true";
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
void test_gc_foreground_demand(){
    std::unique_lock lock(demand_mutex);if(!demand_gate_enabled)return;
    demand_gate_enabled=false;demand_captured=true;demand_cv.notify_all();
    demand_cv.wait(lock,[]{return demand_released;});
}
void test_gc_work_boundary(uint64_t work_done){
    std::unique_lock lock(work_boundary_mutex);if(!work_boundary_enabled||work_done!=work_boundary_target)return;
    work_boundary_enabled=false;work_boundary_captured=true;work_boundary_cv.notify_all();
    work_boundary_cv.wait(lock,[]{return work_boundary_released;});
}
}

static void test_foreground_demand_pressure(const std::string& temp){
    const auto repo=temp+"/foreground-pressure",archive=temp+"/foreground-pressure.pack";
    const auto delta=temp+"/foreground-pressure.delta",restored=temp+"/foreground-pressure-restored";
    LevelStore::create(repo);
    std::vector<Mutation> initial;std::vector<std::pair<std::string,std::vector<unsigned char>>> expected;
    for(unsigned i=0;i<200;++i){
        auto key="root-"+std::to_string(i);auto bytes=value(96,i+11);
        initial.push_back({key,bytes});expected.emplace_back(std::move(key),std::move(bytes));
    }
    const auto pressure_write=value(128,811),replay_probe=value(128,812);
    {
        LevelStore store(repo);store.transact(0,initial);store.verify();
        const auto expected_sequence=store.sequence();
        std::mutex done_mutex;std::condition_variable done_cv;bool done=false;
        std::exception_ptr foreground_error;std::thread foreground;
        struct ReleaseAndJoin {
            std::thread& foreground;
            ~ReleaseAndJoin(){release_demand_gate();if(foreground.joinable())foreground.join();}
        } cleanup{foreground};

        arm_demand_gate();
        foreground=std::thread([&]{try{store.transact(expected_sequence,{{"pressure-write",pressure_write}});}
            catch(...){foreground_error=std::current_exception();}
            {std::lock_guard lock(done_mutex);done=true;}done_cv.notify_all();});
        const bool captured=wait_for_demand(10s);
        if(!captured){release_demand_gate();if(foreground.joinable())foreground.join();CHECK(captured);}

        uint64_t cumulative_mark_work=0,cumulative_physical_work=0;
        for(unsigned i=0;i<3;++i){
            auto report=store.collect_step(8*1024*1024,128,64*1024*1024,128);
            CHECK(flag(report,"foreground_pressure"));
            const auto mark_work=number(report,"mark_work_done");
            const auto physical_work=number(report,"physical_work_done");
            CHECK(mark_work<=8);CHECK(physical_work<=8);CHECK(number(report,"segments_deleted")==0);
            cumulative_mark_work+=mark_work;cumulative_physical_work+=physical_work;
        }
        CHECK(cumulative_mark_work>0);CHECK(cumulative_physical_work>0);
        release_demand_gate();
        bool foreground_done=false;
        {std::unique_lock lock(done_mutex);foreground_done=done_cv.wait_for(lock,10s,[&]{return done;});}
        if(!foreground_done){if(foreground.joinable())foreground.join();CHECK(foreground_done);}
        if(foreground.joinable())foreground.join();
        if(foreground_error)std::rethrow_exception(foreground_error);
        CHECK(store.sequence()==expected_sequence+1);CHECK(store.contains("pressure-write"));
        CHECK(store.object(store.root("pressure-write"))==pressure_write);store.verify();

        bool invalid_rejected=false;try{store.transact(store.sequence(),{});}catch(const std::exception&){invalid_rejected=true;}
        CHECK(invalid_rejected);
        bool conflict_rejected=false;try{store.transact(store.sequence()-1,{{"stale-write",replay_probe}});}catch(const std::exception&){conflict_rejected=true;}
        CHECK(conflict_rejected);CHECK(!store.contains("stale-write"));

        auto idle=store.collect_step(8*1024*1024,128,64*1024*1024,128);
        CHECK(!flag(idle,"foreground_pressure"));CHECK(number(idle,"mark_work_done")>8);
        for(const auto& [key,bytes]:expected)CHECK(store.object(store.root(key))==bytes);
        CHECK(store.object(store.root("pressure-write"))==pressure_write);store.verify();

        const auto base_sequence=store.backup(archive);CHECK(base_sequence==store.sequence());
        store.transact(store.sequence(),{{"replay-probe",replay_probe}});
        CHECK(store.export_replay(base_sequence,delta)==store.sequence());
        store.verify();
    }
    LevelStore::replay_archive(archive,restored);
    {
        LevelStore baseline(restored);CHECK(!baseline.contains("replay-probe"));
        for(const auto& [key,bytes]:expected)CHECK(baseline.object(baseline.root(key))==bytes);
        CHECK(baseline.object(baseline.root("pressure-write"))==pressure_write);baseline.verify();
    }
    LevelStore::replay_archive(delta,restored);
    {
        LevelStore replayed(restored);CHECK(replayed.object(replayed.root("replay-probe"))==replay_probe);
        CHECK(replayed.object(replayed.root("pressure-write"))==pressure_write);replayed.verify();
    }
    {
        LevelStore source(repo);CHECK(source.object(source.root("pressure-write"))==pressure_write);
        CHECK(source.object(source.root("replay-probe"))==replay_probe);source.verify();
    }
}

static void test_late_foreground_demand_pressure(const std::string& temp){
    const auto repo=temp+"/late-foreground-pressure";LevelStore::create(repo);
    std::vector<Mutation> initial;std::vector<std::pair<std::string,std::vector<unsigned char>>> expected;
    for(unsigned i=0;i<200;++i){
        auto key="late-root-"+std::to_string(i);auto bytes=value(96,i+211);
        initial.push_back({key,bytes});expected.emplace_back(std::move(key),std::move(bytes));
    }
    const auto foreground_value=value(128,913);
    {
        LevelStore store(repo);store.transact(0,initial);store.verify();const auto expected_sequence=store.sequence();
        std::mutex gc_done_mutex;std::condition_variable gc_done_cv;bool gc_done=false;
        std::mutex foreground_done_mutex;std::condition_variable foreground_done_cv;bool foreground_done=false;
        std::exception_ptr gc_error,foreground_error;std::string gc_report;
        std::thread gc_worker,foreground;
        struct ReleaseAndJoin {
            std::thread& gc;std::thread& foreground;
            ~ReleaseAndJoin(){release_work_boundary();release_demand_gate();if(gc.joinable())gc.join();if(foreground.joinable())foreground.join();}
        } cleanup{gc_worker,foreground};

        arm_work_boundary(16);
        gc_worker=std::thread([&]{try{gc_report=store.collect_step(8*1024*1024,128,64*1024*1024,128);}
            catch(...){gc_error=std::current_exception();}
            {std::lock_guard lock(gc_done_mutex);gc_done=true;}gc_done_cv.notify_all();});
        const bool boundary_captured=wait_for_work_boundary(10s);
        if(!boundary_captured){release_work_boundary();if(gc_worker.joinable())gc_worker.join();CHECK(boundary_captured);}

        arm_demand_gate();
        foreground=std::thread([&]{try{store.transact(expected_sequence,{{"late-pressure-write",foreground_value}});}
            catch(...){foreground_error=std::current_exception();}
            {std::lock_guard lock(foreground_done_mutex);foreground_done=true;}foreground_done_cv.notify_all();});
        const bool demand_captured=wait_for_demand(10s);
        if(!demand_captured){release_work_boundary();release_demand_gate();if(gc_worker.joinable())gc_worker.join();if(foreground.joinable())foreground.join();CHECK(demand_captured);}

        // Keep the foreground caller pinned and active while GC resumes at work 16.
        release_work_boundary();
        bool gc_finished=false;{std::unique_lock lock(gc_done_mutex);gc_finished=gc_done_cv.wait_for(lock,10s,[&]{return gc_done;});}
        if(!gc_finished){release_demand_gate();if(gc_worker.joinable())gc_worker.join();if(foreground.joinable())foreground.join();CHECK(gc_finished);}
        if(gc_worker.joinable())gc_worker.join();if(gc_error)std::rethrow_exception(gc_error);
        CHECK(flag(gc_report,"foreground_pressure"));
        const auto reached_work=number(gc_report,"mark_work_done");
        CHECK(reached_work>=16);CHECK(reached_work<=128);CHECK(number(gc_report,"physical_work_done")<=8);

        const auto continued=store.collect_step(8*1024*1024,128,64*1024*1024,128);
        CHECK(flag(continued,"foreground_pressure"));CHECK(number(continued,"mark_work_done")<=8);
        CHECK(number(continued,"physical_work_done")<=8);CHECK(number(continued,"segments_deleted")==0);

        release_demand_gate();
        bool foreground_finished=false;{std::unique_lock lock(foreground_done_mutex);foreground_finished=foreground_done_cv.wait_for(lock,10s,[&]{return foreground_done;});}
        if(!foreground_finished){if(foreground.joinable())foreground.join();CHECK(foreground_finished);}
        if(foreground.joinable())foreground.join();if(foreground_error)std::rethrow_exception(foreground_error);
        CHECK(store.sequence()==expected_sequence+1);CHECK(store.object(store.root("late-pressure-write"))==foreground_value);

        const auto idle=store.collect_step(8*1024*1024,128,64*1024*1024,128);
        CHECK(!flag(idle,"foreground_pressure"));CHECK(number(idle,"mark_work_done")>8);
        for(const auto& [key,bytes]:expected)CHECK(store.object(store.root(key))==bytes);
        CHECK(store.object(store.root("late-pressure-write"))==foreground_value);store.verify();
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
        test_foreground_demand_pressure(temp);test_late_foreground_demand_pressure(temp);
        std::filesystem::remove_all(temp);
        std::cout<<"backup snapshot location pin, foreground write progress and online GC retirement passed\n";
    }catch(const std::exception& e){release_snapshot();std::cerr<<e.what()<<'\n';std::filesystem::remove_all(temp);return 1;}
}
