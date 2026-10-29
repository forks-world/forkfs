#include "gc_mark.h"
#include <filesystem>
#include <exception>
#include <condition_variable>
#include <mutex>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <chrono>
#include <sys/stat.h>
#include <unistd.h>

using namespace forkfs;
using Id=GcMark::Id;
using Bytes=GcMark::Bytes;
using Objects=std::map<Id,Bytes>;

#define CHECK(x) do { if(!(x)) throw std::runtime_error("line "+std::to_string(__LINE__)+": " #x); } while(0)

struct TempDir {
    std::string path;
    TempDir(){char value[]="/tmp/forkfs-gc-mark-XXXXXX";char* p=::mkdtemp(value);if(!p)throw std::runtime_error("mkdtemp failed");path=p;}
    ~TempDir(){std::error_code ec;std::filesystem::remove_all(path,ec);}
};

static void put(Bytes& b,size_t at,uint64_t value,unsigned width){
    CHECK(at<=b.size()&&b.size()-at>=width);
    for(unsigned i=0;i<width;++i)b[at+i]=static_cast<unsigned char>(value>>(8*i));
}
static Id add(Objects& objects,Bytes bytes){auto id=Journal::object_id(bytes);objects.emplace(id,std::move(bytes));return id;}
static Id zero_id(){return {};}
static Bytes inode(const char* magic,uint32_t type,uint64_t links,uint64_t size,uint32_t flags=0){
    bool extended=!memcmp(magic,"FFINOD03",8)||!memcmp(magic,"FFINOD04",8)||!memcmp(magic,"FFINOD05",8);
    bool xattrs=!memcmp(magic,"FFINOD04",8)||!memcmp(magic,"FFINOD05",8);
    Bytes b(xattrs?84:extended?80:48);memcpy(b.data(),magic,8);put(b,8,type,4);put(b,12,0644,4);
    put(b,16,links,8);put(b,24,size,8);if(extended)put(b,48,flags,4);if(xattrs)put(b,80,0,4);return b;
}
static Bytes descriptor(uint64_t size,const Id& root){Bytes b(48);memcpy(b.data(),"FFFILE01",8);put(b,8,size,8);memcpy(b.data()+16,root.data(),32);return b;}
static Bytes extent_node(unsigned level,const std::vector<std::pair<unsigned,Id>>& children){
    Bytes b(11+33*children.size());memcpy(b.data(),"FFEXT001",8);b[8]=static_cast<unsigned char>(level);put(b,9,children.size(),2);
    size_t at=11;for(const auto& [slot,id]:children){b[at++]=static_cast<unsigned char>(slot);memcpy(b.data()+at,id.data(),32);at+=32;}return b;
}
static void full_entry(Bytes& run,const std::string& key,const Id& id){
    auto at=run.size();run.resize(at+4+key.size()+32);put(run,at,key.size(),4);memcpy(run.data()+at+4,key.data(),key.size());memcpy(run.data()+at+4+key.size(),id.data(),32);
}
static void delta_entry(Bytes& run,uint32_t op,const std::string& key,const Id& id){
    auto at=run.size();run.resize(at+8+key.size()+32);put(run,at,op,4);put(run,at+4,key.size(),4);memcpy(run.data()+at+8,key.data(),key.size());memcpy(run.data()+at+8+key.size(),id.data(),32);
}
static Bytes full_run(const std::map<std::string,Id>& entries){
    Bytes b(12);memcpy(b.data(),"FFMAN001",8);put(b,8,entries.size(),4);for(const auto& [key,id]:entries)full_entry(b,key,id);return b;
}
static Bytes delta_run(const std::map<std::string,std::pair<uint32_t,Id>>& entries){
    Bytes b(12);memcpy(b.data(),"FFRUN001",8);put(b,8,entries.size(),4);for(const auto& [key,value]:entries)delta_entry(b,value.first,key,value.second);return b;
}
static Bytes manifest(const std::vector<Id>& runs){
    Bytes b(12+32*runs.size());memcpy(b.data(),"FFMAN002",8);put(b,8,runs.size(),4);for(size_t i=0;i<runs.size();++i)memcpy(b.data()+12+32*i,runs[i].data(),32);return b;
}
static std::string root_inode_key(const std::string& id){return std::string("R/\0forkfs/inode/",16)+id;}
static std::string root_content_key(const std::string& id){return std::string("R/\0forkfs/content/",18)+id;}
static std::string inode_key(const std::string& id){return std::string("\0forkfs/inode/",14)+id;}
static std::string content_key(const std::string& id){return std::string("\0forkfs/content/",16)+id;}
static GcMark::Reader reader_for(const Objects& objects){return [&objects](const Id& id){auto it=objects.find(id);if(it==objects.end())throw std::runtime_error("fixture object missing");return it->second;};}
static void drain(GcMark& marker,const Objects& objects){size_t units=0;auto read=reader_for(objects);while(marker.has_work()){CHECK(marker.advance(1,read)==1);CHECK(++units<100000);}}
template<class F> static void rejects(F&& f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}CHECK(caught);}

static std::pair<Id,std::vector<Id>> extent_tree(Objects& objects,const std::vector<unsigned>& leaf_slots){
    Bytes block(65536);block[0]=1;std::vector<Id> data;std::vector<std::pair<unsigned,Id>> leaves;
    for(auto slot:leaf_slots){Bytes b=block;b[0]=static_cast<unsigned char>(slot+1);auto id=add(objects,std::move(b));data.push_back(id);leaves.emplace_back(slot,id);}
    auto node=add(objects,extent_node(0,leaves));std::vector<Id> nodes{node};
    for(unsigned level=1;level<=5;++level){node=add(objects,extent_node(level,{{0,node}}));nodes.push_back(node);}
    return {node,[&]{std::vector<Id> ids=nodes;ids.insert(ids.end(),data.begin(),data.end());return ids;}()};
}

struct WorldFixture {Id world{},root{},child{};};
static WorldFixture world_fixture(Objects& objects,char tag){
    WorldFixture value;value.root=add(objects,Bytes{'r',static_cast<unsigned char>(tag)});value.child=add(objects,Bytes{'c',static_cast<unsigned char>(tag)});
    const std::string root_key("\0forkfs/root",12),child_key=std::string("\0forkfs/payload/",16)+tag;
    value.world=add(objects,full_run({{root_key,value.root},{child_key,value.child}}));return value;
}
static std::string world_root_key(const std::string& name){return std::string("R/\0forkfs-history/world/",24)+name;}

static void test_sparse_extent_and_zero_root(){
    TempDir dir;Objects objects;auto [root,reachable]=extent_tree(objects,{0,1});
    // A two block file uses one allocated block and a seven byte partial tail.
    auto inode_id=add(objects,inode("FFINOD05",1,1,65543,2));auto content=add(objects,descriptor(65543,root));
    const std::string name(32,'a'),ik=root_inode_key(name),ck=root_content_key(name);
    std::map<std::string,Id> roots{{ik,inode_id},{ck,content}};GcMark marker(dir.path);
    marker.seed_root(ik,inode_id,[&](const std::string& key)->std::optional<Id>{auto it=roots.find(key);if(it==roots.end())return {};return it->second;});
    auto garbage=add(objects,Bytes{'g','a','r','b','a','g','e'});drain(marker,objects);
    CHECK(marker.live(inode_id)&&marker.live(content)&&marker.live(root));for(const auto& id:reachable)CHECK(marker.live(id));CHECK(!marker.live(garbage));

    TempDir holes;Objects hole_objects;auto hole_inode=add(hole_objects,inode("FFINOD05",1,1,100000,2));auto hole_content=add(hole_objects,descriptor(100000,zero_id()));
    const std::string hole_name(32,'b'),hik=root_inode_key(hole_name),hck=root_content_key(hole_name);
    std::map<std::string,Id> hole_roots{{hik,hole_inode},{hck,hole_content}};GcMark all_holes(holes.path);
    all_holes.seed_root(hik,hole_inode,[&](const std::string& key)->std::optional<Id>{auto it=hole_roots.find(key);if(it==hole_roots.end())return {};return it->second;});drain(all_holes,hole_objects);
    CHECK(all_holes.live(hole_inode)&&all_holes.live(hole_content));CHECK(!all_holes.has_work());
}

static void test_extent_out_of_eof_rejected(){
    TempDir dir;Objects objects;Bytes block(65536,1);auto data=add(objects,block);auto leaf=add(objects,extent_node(0,{{1,data}}));Id root=leaf;
    for(unsigned level=1;level<=5;++level)root=add(objects,extent_node(level,{{0,root}}));
    auto inode_id=add(objects,inode("FFINOD05",1,1,1,2));auto content=add(objects,descriptor(1,root));const std::string name(32,'c'),ik=root_inode_key(name),ck=root_content_key(name);
    std::map<std::string,Id> roots{{ik,inode_id},{ck,content}};GcMark marker(dir.path);
    marker.seed_root(ik,inode_id,[&](const std::string& key)->std::optional<Id>{auto it=roots.find(key);if(it==roots.end())return {};return it->second;});
    rejects([&]{auto read=reader_for(objects);size_t units=0;while(marker.has_work()){marker.advance(1,read);CHECK(++units<1000);}});
}

static void test_raw_content_magic_is_opaque(){
    TempDir dir;Objects objects;auto inode_id=add(objects,inode("FFINOD01",1,1,48));Bytes raw(48);memcpy(raw.data(),"FFFILE01",8);put(raw,8,0,8);auto content=add(objects,raw);
    const std::string name(32,'d'),ik=root_inode_key(name),ck=root_content_key(name);std::map<std::string,Id> roots{{ik,inode_id},{ck,content}};GcMark marker(dir.path);
    marker.seed_root(ik,inode_id,[&](const std::string& key)->std::optional<Id>{auto it=roots.find(key);if(it==roots.end())return {};return it->second;});drain(marker,objects);CHECK(marker.live(inode_id)&&marker.live(content));
}

static void test_effective_overlay_keeps_shadowed_put_opaque(){
    TempDir dir;Objects objects;const std::string suffix(32,'e'),ik=inode_key(suffix),ck=content_key(suffix),rk("\0forkfs/root",12);
    auto shadow=add(objects,Bytes{'n','o','t','-','a','n','-','i','n','o','d','e'});auto winner=add(objects,inode("FFINOD01",1,1,0));auto raw=add(objects,{});auto root_obj=add(objects,Bytes{'r'});
    auto old=add(objects,full_run({{ck,raw},{ik,shadow},{rk,root_obj}}));auto newer=add(objects,delta_run({{ck,{1,raw}},{ik,{1,winner}}}));auto world=add(objects,manifest({newer,old}));
    GcMark marker(dir.path);marker.seed_manifest(world);drain(marker,objects);CHECK(marker.live(shadow)&&marker.live(winner)&&marker.live(raw));
    CHECK(marker.live(root_obj));
}

static void test_invalid_run_operation_rejected(){
    TempDir dir;Objects objects;const std::string rk("\0forkfs/root",12);auto payload=add(objects,Bytes{'r'});
    Bytes bad(12+8+rk.size()+32);memcpy(bad.data(),"FFRUN001",8);put(bad,8,1,4);put(bad,12,3,4);put(bad,16,rk.size(),4);memcpy(bad.data()+20,rk.data(),rk.size());memcpy(bad.data()+20+rk.size(),payload.data(),32);auto run=add(objects,bad);auto world=add(objects,manifest({run}));
    GcMark marker(dir.path);marker.seed_manifest(world);rejects([&]{auto read=reader_for(objects);size_t units=0;while(marker.has_work()){marker.advance(1,read);CHECK(++units<1000);}});
}

static void test_content_only_orphan_and_inode_roots(){
    TempDir direct_dir;Objects direct_objects;auto [direct_extent,direct_reachable]=extent_tree(direct_objects,{0});auto direct_inode=add(direct_objects,inode("FFINOD05",1,1,1,2));auto direct_content=add(direct_objects,descriptor(1,direct_extent));const std::string direct_name(32,'f'),direct_ik=root_inode_key(direct_name),direct_ck=root_content_key(direct_name);std::map<std::string,Id> direct_roots{{direct_ik,direct_inode},{direct_ck,direct_content}};GcMark direct(direct_dir.path);
    direct.seed_root(direct_ck,direct_content,[&](const std::string& key)->std::optional<Id>{auto it=direct_roots.find(key);if(it==direct_roots.end())return {};return it->second;});drain(direct,direct_objects);CHECK(direct.live(direct_inode)&&direct.live(direct_content));for(const auto& id:direct_reachable)CHECK(direct.live(id));

    TempDir dir;Objects objects;auto [root,reachable]=extent_tree(objects,{0});auto orphan_inode=add(objects,inode("FFINOD05",1,0,1,2));auto orphan_content=add(objects,descriptor(1,root));
    std::string base=std::string("R/\0forkfs-history/orphan/demo/",30)+std::string(32,'f');auto oi=base+"/inode",oc=base+"/content";
    GcMark marker(dir.path);marker.seed_root(oc,orphan_content,[&](const std::string& key)->std::optional<Id>{if(key==oc)return orphan_content;if(key==oi)return orphan_inode;return {};});drain(marker,objects);
    CHECK(marker.live(orphan_inode)&&marker.live(orphan_content)&&marker.live(root));for(const auto& id:reachable)CHECK(marker.live(id));

    TempDir dir_only;Objects dirs;auto directory=add(dirs,inode("FFINOD02",2,2,0));auto dk=root_inode_key(std::string(32,'1'));GcMark directory_marker(dir_only.path);
    directory_marker.seed_root(dk,directory,[&](const std::string& key)->std::optional<Id>{return key==dk?std::optional<Id>(directory):std::nullopt;});drain(directory_marker,dirs);CHECK(directory_marker.live(directory));

    TempDir symlink_dir;Objects symlinks;auto symlink=add(symlinks,inode("FFINOD01",3,1,3));auto target=add(symlinks,Bytes{'a','b','c'});auto sk=root_inode_key(std::string(32,'2')),sck=root_content_key(std::string(32,'2'));GcMark symlink_marker(symlink_dir.path);
    symlink_marker.seed_root(sk,symlink,[&](const std::string& key)->std::optional<Id>{if(key==sk)return symlink;if(key==sck)return target;return {};});drain(symlink_marker,symlinks);CHECK(symlink_marker.live(symlink)&&symlink_marker.live(target));
}

static void test_foreground_seeds_while_reader_is_blocked(){
    TempDir dir;Objects objects;
    const std::string root_key("\0forkfs/root",12),child_key("\0forkfs/child",13);
    auto root_value=add(objects,Bytes{'r'});
    auto shared_child=add(objects,Bytes{'s','h','a','r','e','d'});
    auto first_run=add(objects,full_run({{root_key,root_value},{child_key,shared_child}}));
    auto second_value=add(objects,Bytes{'o','t','h','e','r'});
    auto second_run=add(objects,full_run({{root_key,second_value},{child_key,shared_child}}));
    GcMark marker(dir.path);
    marker.seed_manifest(first_run);

    std::mutex gate_mutex;std::condition_variable gate;
    bool reader_entered=false,release_reader=false;
    std::exception_ptr advance_error,foreground_error;
    std::thread advance_thread([&]{try{
        auto read=[&](const Id& id){
            {std::unique_lock<std::mutex> lock(gate_mutex);reader_entered=true;gate.notify_all();gate.wait(lock,[&]{return release_reader;});}
            auto it=objects.find(id);if(it==objects.end())throw std::runtime_error("fixture object missing");return it->second;
        };
        CHECK(marker.advance(1,read)==1);
    }catch(...){advance_error=std::current_exception();}});
    struct JoinOnExit {
        std::mutex& mutex;std::condition_variable& condition;bool& release;std::thread& worker;
        ~JoinOnExit(){{std::lock_guard<std::mutex> lock(mutex);release=true;}condition.notify_all();if(worker.joinable())worker.join();}
    } join_guard{gate_mutex,gate,release_reader,advance_thread};

    bool entered=false;
    {std::unique_lock<std::mutex> lock(gate_mutex);entered=gate.wait_for(lock,std::chrono::seconds(10),[&]{return reader_entered;});}
    CHECK(entered);
    std::mutex foreground_mutex;std::condition_variable foreground_done;bool finished=false;
    std::thread foreground([&]{try{
        marker.seed_manifest(second_run);
        marker.protect(shared_child); // Duplicates a child that the blocked run scan will enqueue.
        marker.protect_chunks({std::string(32,'a'),std::string(32,'b'),std::string(32,'c')});
    }catch(...){foreground_error=std::current_exception();}
        {std::lock_guard<std::mutex> lock(foreground_mutex);finished=true;}foreground_done.notify_all();});
    bool foreground_finished=false;
    {std::unique_lock<std::mutex> lock(foreground_mutex);foreground_finished=foreground_done.wait_for(lock,std::chrono::seconds(10),[&]{return finished;});}
    if(!foreground_finished){
        {std::lock_guard<std::mutex> lock(gate_mutex);release_reader=true;}gate.notify_all();
        if(advance_thread.joinable())advance_thread.join();
        if(foreground.joinable())foreground.join();
        throw std::runtime_error("foreground GC seed blocked while Reader was stalled");
    }
    if(foreground.joinable())foreground.join();
    if(foreground_error)std::rethrow_exception(foreground_error);
    CHECK(marker.has_work());
    {std::lock_guard<std::mutex> lock(gate_mutex);release_reader=true;}gate.notify_all();
    advance_thread.join();
    if(advance_error)std::rethrow_exception(advance_error);
    drain(marker,objects);
    CHECK(marker.live(first_run)&&marker.live(second_run));
    CHECK(marker.live(root_value)&&marker.live(second_value)&&marker.live(shared_child));
    CHECK(marker.chunk_live(std::string(32,'a'))&&marker.chunk_live(std::string(32,'b'))&&marker.chunk_live(std::string(32,'c')));CHECK(marker.pending()==0);
}

static void test_protect_chunks_batch_atomicity_and_bounds(){
    const std::string a(32,'a'),b(32,'b'),c(32,'c'),legacy(32,'d');
    {
        TempDir dir;GcMark marker(dir.path);
        const auto before_processed=marker.processed();const auto before_pending=marker.pending();
        marker.protect_chunks({});
        CHECK(!marker.has_work());CHECK(marker.processed()==before_processed);CHECK(marker.pending()==before_pending);
        CHECK(!marker.chunk_live(a));
        marker.protect_chunks({a,b,a,c});
        CHECK(marker.chunk_live(a)&&marker.chunk_live(b)&&marker.chunk_live(c));
        CHECK(!marker.has_work());CHECK(marker.processed()==0);CHECK(marker.pending()==0);
        marker.protect_chunk(legacy);CHECK(marker.chunk_live(legacy));
    }
    {
        TempDir dir;GcMark marker(dir.path);const std::string malformed(31,'x');
        rejects([&]{marker.protect_chunks({a,malformed,b});});
        CHECK(!marker.chunk_live(a)&&!marker.chunk_live(b));
        CHECK(!marker.has_work());CHECK(marker.pending()==0);CHECK(marker.processed()==0);
    }
    {
        TempDir dir;GcMark marker(dir.path);std::vector<std::string> too_many(GcMark::max_protected_chunks+1,a);
        rejects([&]{marker.protect_chunks(too_many);});
        CHECK(!marker.chunk_live(a));CHECK(!marker.has_work());CHECK(marker.pending()==0);CHECK(marker.processed()==0);
    }
}

static void test_reader_failure_and_concurrent_advances(){
    TempDir dir;Objects objects;auto root_value=add(objects,Bytes{'r'});auto child=add(objects,Bytes{'c'});
    const std::string root_key("\0forkfs/root",12),child_key("\0forkfs/child",13);
    auto world=add(objects,full_run({{root_key,root_value},{child_key,child}}));
    auto counters=std::make_shared<GcMark::MutexCounters>();GcMark::MutexMetrics before_recreate;
    {
        GcMark marker(dir.path,counters);marker.seed_manifest(world);
        const auto initial_processed=marker.processed();const auto initial_pending=marker.pending();
        auto failing=[](const Id&)->Bytes{throw std::runtime_error("injected Reader failure");};
        rejects([&]{marker.advance(1,failing);});
        CHECK(marker.has_work());CHECK(marker.processed()==initial_processed);CHECK(marker.pending()==initial_pending);

        std::mutex gate_mutex;std::condition_variable gate;bool reader_entered=false,release_reader=false,second_started=false;
        std::exception_ptr first_error,second_error;std::thread first,second;
        struct JoinOnExit {
            std::mutex& mutex;std::condition_variable& condition;bool& release;std::thread& first;std::thread& second;
            ~JoinOnExit(){{std::lock_guard<std::mutex> lock(mutex);release=true;}condition.notify_all();if(first.joinable())first.join();if(second.joinable())second.join();}
        } join_guard{gate_mutex,gate,release_reader,first,second};
        first=std::thread([&]{try{
            auto read=[&](const Id& id){
                {std::unique_lock<std::mutex> lock(gate_mutex);reader_entered=true;gate.notify_all();gate.wait(lock,[&]{return release_reader;});}
                auto it=objects.find(id);if(it==objects.end())throw std::runtime_error("fixture object missing");return it->second;
            };
            marker.advance(1,read);
        }catch(...){first_error=std::current_exception();}});
        bool entered=false;{std::unique_lock<std::mutex> lock(gate_mutex);entered=gate.wait_for(lock,std::chrono::seconds(10),[&]{return reader_entered;});}
        CHECK(entered);
        second=std::thread([&]{
            {std::lock_guard<std::mutex> lock(gate_mutex);second_started=true;}gate.notify_all();
            try{marker.advance(1,reader_for(objects));}catch(...){second_error=std::current_exception();}
        });
        {std::unique_lock<std::mutex> lock(gate_mutex);CHECK(gate.wait_for(lock,std::chrono::seconds(10),[&]{return second_started;}));release_reader=true;}gate.notify_all();
        first.join();second.join();
        if(first_error)std::rethrow_exception(first_error);if(second_error)std::rethrow_exception(second_error);
        drain(marker,objects);CHECK(marker.live(world)&&marker.live(root_value)&&marker.live(child));CHECK(marker.pending()==0);
        before_recreate=marker.mutex_metrics();
    }
    {
        GcMark marker(dir.path,counters);const auto restored=marker.mutex_metrics();
        CHECK(restored.foreground_wait_ns==before_recreate.foreground_wait_ns);
        CHECK(restored.foreground_hold_ns==before_recreate.foreground_hold_ns);
        CHECK(restored.advance_wait_ns==before_recreate.advance_wait_ns);
        CHECK(restored.advance_hold_ns==before_recreate.advance_hold_ns);
        auto other=add(objects,Bytes{'x'});marker.protect(other);CHECK(marker.advance(1,reader_for(objects))==1);
        const auto after=marker.mutex_metrics();
        CHECK(after.foreground_wait_ns>=before_recreate.foreground_wait_ns);
        CHECK(after.foreground_hold_ns>=before_recreate.foreground_hold_ns);
        CHECK(after.advance_wait_ns>=before_recreate.advance_wait_ns);
        CHECK(after.advance_hold_ns>=before_recreate.advance_hold_ns);
    }
}

static void test_published_world_root_coalescing(){
    TempDir dir;Objects objects;const auto key=world_root_key("live");
    auto baseline=world_fixture(objects,'a');
    {
        GcMark marker(dir.path);std::map<std::string,Id> roots{{key,baseline.world}};
        marker.seed_root(key,baseline.world,[&](const std::string& candidate)->std::optional<Id>{auto it=roots.find(candidate);return it==roots.end()?std::nullopt:std::optional<Id>(it->second);});
        drain(marker,objects);CHECK(marker.live(baseline.world)&&marker.live(baseline.root)&&marker.live(baseline.child));
    }

    auto second=world_fixture(objects,'b');auto latest=world_fixture(objects,'c');GcMark marker(dir.path);
    std::map<std::string,Id> published{{key,baseline.world}};
    auto roots=[&](const std::string& candidate)->std::optional<Id>{auto it=published.find(candidate);return it==published.end()?std::nullopt:std::optional<Id>(it->second);};
    marker.seed_published_root(key,baseline.world,roots);CHECK(marker.has_work());CHECK(marker.pending()==1);
    published[key]=second.world;marker.seed_published_root(key,second.world,roots);CHECK(marker.pending()==1);
    published[key]=latest.world;marker.seed_published_root(key,latest.world,roots);CHECK(marker.pending()==1);
    marker.seed_published_root(key,latest.world,roots);CHECK(marker.pending()==1);
    drain(marker,objects);CHECK(!marker.has_work());CHECK(marker.pending()==0);
    CHECK(marker.live(baseline.world)&&marker.live(second.world)&&marker.live(latest.world));
    CHECK(marker.live(latest.root)&&marker.live(latest.child));
    CHECK(!marker.live(baseline.root)&&!marker.live(baseline.child));CHECK(!marker.live(second.root)&&!marker.live(second.child));

    marker.seed_manifest(baseline.world);CHECK(marker.pending()==1);drain(marker,objects);
    CHECK(marker.live(baseline.root)&&marker.live(baseline.child));CHECK(marker.pending()==0);
}

static void test_published_world_roots_coexist_with_retained_roots(){
    TempDir dir;Objects objects;auto superseded=world_fixture(objects,'s'),latest=world_fixture(objects,'l'),revision_world=world_fixture(objects,'v'),leased=world_fixture(objects,'x'),other_key_world=world_fixture(objects,'y');
    const auto key=world_root_key("coexist");std::map<std::string,Id> published{{key,superseded.world}};
    const auto second_key=world_root_key("other");published[second_key]=other_key_world.world;
    auto roots=[&](const std::string& candidate)->std::optional<Id>{auto it=published.find(candidate);return it==published.end()?std::nullopt:std::optional<Id>(it->second);};
    GcMark marker(dir.path);
    marker.seed_root(key,superseded.world,roots);
    published[key]=latest.world;marker.seed_published_root(key,latest.world,roots);
    marker.seed_published_root(second_key,other_key_world.world,roots);

    RevisionRecord retained;retained.manifest=revision_world.world;auto revision=add(objects,retained.encode());
    marker.seed_root(std::string("R/\0forkfs-history/revision/",27)+"retained",revision,{});
    marker.seed_manifest(leased.world);
    drain(marker,objects);
    CHECK(marker.live(superseded.world)&&marker.live(superseded.root)&&marker.live(superseded.child));
    CHECK(marker.live(latest.world)&&marker.live(latest.root)&&marker.live(latest.child));
    CHECK(marker.live(revision)&&marker.live(revision_world.world)&&marker.live(revision_world.root)&&marker.live(revision_world.child));
    CHECK(marker.live(leased.world)&&marker.live(leased.root)&&marker.live(leased.child));
    CHECK(marker.live(other_key_world.world)&&marker.live(other_key_world.root)&&marker.live(other_key_world.child));
    CHECK(!marker.has_work());CHECK(marker.pending()==0);
}

static void test_published_world_replacement_during_blocked_reader(bool aba){
    TempDir dir;Objects objects;auto previous=world_fixture(objects,'p'),replacement=world_fixture(objects,'q');
    const auto key=world_root_key("blocked");std::map<std::string,Id> published{{key,previous.world}};
    auto roots=[&](const std::string& candidate)->std::optional<Id>{auto it=published.find(candidate);return it==published.end()?std::nullopt:std::optional<Id>(it->second);};
    GcMark marker(dir.path);marker.seed_published_root(key,previous.world,roots);CHECK(marker.has_work());CHECK(marker.pending()==1);

    std::mutex gate_mutex;std::condition_variable gate;bool reader_entered=false,release_reader=false;
    std::exception_ptr advance_error,publish_error;
    std::thread advance([&]{try{
        auto read=[&](const Id& id){
            {std::unique_lock<std::mutex> lock(gate_mutex);reader_entered=true;gate.notify_all();gate.wait(lock,[&]{return release_reader;});}
            auto it=objects.find(id);if(it==objects.end())throw std::runtime_error("fixture object missing");return it->second;
        };
        CHECK(marker.advance(1,read)==1);
    }catch(...){advance_error=std::current_exception();}});
    struct JoinOnExit {
        std::mutex& mutex;std::condition_variable& condition;bool& release;std::thread& worker;
        ~JoinOnExit(){{std::lock_guard<std::mutex> lock(mutex);release=true;}condition.notify_all();if(worker.joinable())worker.join();}
    } join_guard{gate_mutex,gate,release_reader,advance};
    bool entered=false;{std::unique_lock<std::mutex> lock(gate_mutex);entered=gate.wait_for(lock,std::chrono::seconds(10),[&]{return reader_entered;});}CHECK(entered);
    std::mutex publish_mutex;std::condition_variable publish_done;bool finished=false;
    std::thread publisher([&]{try{
        published[key]=replacement.world;marker.seed_published_root(key,replacement.world,roots);
        if(aba){published[key]=previous.world;marker.seed_published_root(key,previous.world,roots);}
    }catch(...){publish_error=std::current_exception();}
        {std::lock_guard<std::mutex> lock(publish_mutex);finished=true;}publish_done.notify_all();});
    bool publisher_finished=false;{std::unique_lock<std::mutex> lock(publish_mutex);publisher_finished=publish_done.wait_for(lock,std::chrono::seconds(10),[&]{return finished;});}
    if(!publisher_finished){
        {std::lock_guard<std::mutex> lock(gate_mutex);release_reader=true;}gate.notify_all();if(advance.joinable())advance.join();if(publisher.joinable())publisher.join();
        throw std::runtime_error("published world replacement blocked while Reader was stalled");
    }
    publisher.join();if(publish_error)std::rethrow_exception(publish_error);
    CHECK(marker.has_work());CHECK(marker.pending()==1);
    {std::lock_guard<std::mutex> lock(gate_mutex);release_reader=true;}gate.notify_all();advance.join();if(advance_error)std::rethrow_exception(advance_error);
    CHECK(marker.has_work());CHECK(marker.pending()==(aba?1:2));drain(marker,objects);CHECK(marker.pending()==0);
    CHECK(marker.live(previous.world)&&marker.live(replacement.world));
    const auto& expected=aba?previous:replacement;CHECK(marker.live(expected.root)&&marker.live(expected.child));
}

static void test_published_world_reader_error_and_validation(){
    TempDir dir;Objects objects;auto world=world_fixture(objects,'e');const auto key=world_root_key("retry");
    std::map<std::string,Id> published{{key,world.world}};
    auto roots=[&](const std::string& candidate)->std::optional<Id>{auto it=published.find(candidate);return it==published.end()?std::nullopt:std::optional<Id>(it->second);};
    GcMark marker(dir.path);marker.seed_published_root(key,world.world,roots);CHECK(marker.has_work());CHECK(marker.pending()==1);
    auto failing=[](const Id&)->Bytes{throw std::runtime_error("injected publication Reader failure");};
    rejects([&]{marker.advance(1,failing);});CHECK(marker.has_work());CHECK(marker.pending()==1);CHECK(marker.processed()==0);
    drain(marker,objects);CHECK(marker.live(world.world)&&marker.live(world.root)&&marker.live(world.child));CHECK(marker.pending()==0);

    TempDir bad_dir;Objects bad_objects;auto malformed=add(bad_objects,Bytes{'n','o','t','-','a','-','m','a','n','i','f','e','s','t'});
    auto bad_key=world_root_key("malformed");std::map<std::string,Id> bad_published{{bad_key,malformed}};
    auto bad_roots=[&](const std::string& candidate)->std::optional<Id>{auto it=bad_published.find(candidate);return it==bad_published.end()?std::nullopt:std::optional<Id>(it->second);};
    GcMark bad_marker(bad_dir.path);bad_marker.seed_published_root(bad_key,malformed,bad_roots);
    rejects([&]{drain(bad_marker,bad_objects);});CHECK(bad_marker.has_work());CHECK(bad_marker.pending()==1);CHECK(bad_marker.live(malformed));

    TempDir invalid_dir;GcMark invalid(invalid_dir.path);Id zero{};
    rejects([&]{invalid.seed_published_root(key,zero,roots);});CHECK(!invalid.has_work());CHECK(invalid.pending()==0);
    auto mismatch=world_fixture(objects,'m').world;
    rejects([&]{invalid.seed_published_root(key,mismatch,roots);});CHECK(!invalid.has_work());CHECK(invalid.pending()==0);
}

static void test_maximum_revision_fanout(){
    TempDir dir;Objects objects;auto value=add(objects,Bytes{'r'});
    auto world=add(objects,full_run({{std::string("\0forkfs/root",12),value}}));
    RevisionRecord revision;revision.manifest=world;
    for(size_t i=0;i<RevisionRecord::max_parents;++i){
        RevisionRecord parent;parent.manifest=world;parent.source_sequence=i;
        revision.parents.push_back(add(objects,parent.encode()));
    }
    auto head=add(objects,revision.encode());GcMark marker(dir.path);
    marker.seed_root(std::string("R/\0forkfs-history/revision/",27)+"head",head,{});
    drain(marker,objects);CHECK(marker.pending()==0);CHECK(marker.live(head)&&marker.live(world)&&marker.live(value));
    for(const auto& parent:revision.parents)CHECK(marker.live(parent));
}

int main(){try{
    test_sparse_extent_and_zero_root();test_extent_out_of_eof_rejected();test_raw_content_magic_is_opaque();
    test_effective_overlay_keeps_shadowed_put_opaque();test_invalid_run_operation_rejected();test_content_only_orphan_and_inode_roots();
    test_foreground_seeds_while_reader_is_blocked();test_protect_chunks_batch_atomicity_and_bounds();
    test_reader_failure_and_concurrent_advances();test_maximum_revision_fanout();
    test_published_world_root_coalescing();test_published_world_roots_coexist_with_retained_roots();
    test_published_world_replacement_during_blocked_reader(false);test_published_world_replacement_during_blocked_reader(true);
    test_published_world_reader_error_and_validation();
    std::cout<<"incremental GC marker roots, overlays, sparse extents and corruption checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
