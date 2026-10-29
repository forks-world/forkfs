#include "level_store.h"
#include "journal.h"
#include "replay_format.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <stdexcept>
#include <unistd.h>

using namespace forkfs;
namespace {
#define CHECK(x) do { if(!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while(0)
uint64_t number(const std::string& json,const char* key) {
    std::smatch m; const std::regex re(std::string("\\\"")+key+"\\\":([0-9]+)");
    CHECK(std::regex_search(json,m,re)); return std::stoull(m[1]);
}
bool boolean(const std::string& json,const char* key) {
    std::smatch m; const std::regex re(std::string("\\\"")+key+"\\\":(true|false)");
    CHECK(std::regex_search(json,m,re)); return m[1]=="true";
}
std::string string_field(const std::string& json,const char* key) {
    std::smatch m; const std::regex re(std::string("\\\"")+key+"\\\":\\\"([^\\\"]*)\\\"");
    CHECK(std::regex_search(json,m,re)); return m[1];
}
std::vector<unsigned char> payload(unsigned n) {
    std::string s="FFFILE01 opaque payload "+std::to_string(n);
    s.resize(4096,static_cast<char>('a'+n%26)); return {s.begin(),s.end()};
}
bool converged(const std::string& r) {
    return !boolean(r,"mark_pending") && number(r,"objects_deleted")==0 &&
        number(r,"chunks_moved")==0 && number(r,"segments_deleted")==0 && number(r,"pending_segments")==0;
}
std::string step(LevelStore& store,size_t work=1) {
    auto r=store.collect_step(1024*1024,128,256*1024*1024,work);
    CHECK(number(r,"mark_work_done")<=work);
    CHECK(!string_field(r,"mark_phase").empty());
    return r;
}
void converge(LevelStore& store,unsigned limit=10000) {
    std::string last;
    for(unsigned i=0;i<limit;++i){last=step(store);if(converged(last)) return;}
    throw std::runtime_error("incremental GC did not converge in "+std::to_string(limit)+" calls; last report="+last);
}
void bounded_mark_and_writes(const std::string& path) {
    LevelStore::create(path);
    std::map<std::string,std::vector<unsigned char>> live;
    {
        LevelStore s(path);
        for(const size_t invalid:{size_t(0),size_t(65537)}){
            bool rejected=false;try{(void)s.collect_step(1024*1024,128,256*1024*1024,invalid);}
            catch(const std::exception&){rejected=true;}CHECK(rejected);
        }
        std::vector<Mutation> roots;
        for(unsigned i=0;i<96;++i){auto key="live/"+std::to_string(i);live[key]=payload(i);roots.push_back({key,live[key]});}
        for(unsigned i=0;i<180;++i) roots.push_back({"garbage/"+std::to_string(i),payload(1000+i)});
        s.transact(s.sequence(),roots);
        std::vector<Mutation> erase;for(unsigned i=0;i<180;++i)erase.push_back({"garbage/"+std::to_string(i),std::nullopt});
        s.transact(s.sequence(),erase);
        auto first=step(s);
        CHECK(boolean(first,"mark_pending"));
        CHECK(number(first,"objects_deleted")==0);
        CHECK(number(first,"mark_work_done")<=1);
        bool reclaimed=false;
        bool writing=true;
        for(unsigned i=0;i<1000;++i){
            // Each GC call is interleaved with a small replacement write.
            const auto index=i%96;
            const auto key="live/"+std::to_string(index);
            live[key]=payload(2000+i);
            s.transact(s.sequence(),{{key,live[key]}});
            auto r=step(s,16); reclaimed |= number(r,"objects_deleted")>0;
            if(reclaimed){writing=false;break;}
            CHECK(i!=999);
        }
        CHECK(!writing);
        CHECK(reclaimed);
        converge(s);
        for(const auto& [key,value]:live)CHECK(s.object(s.root(key))==value);
        s.verify();
    }
    LevelStore reopened(path);for(const auto& [key,value]:live)CHECK(reopened.object(reopened.root(key))==value);reopened.verify();
}
void revival_and_pins(const std::string& path) {
    LevelStore::create(path);const auto value=payload(77),other=payload(78);const auto key="revive";
    {
        LevelStore s(path);s.transact(0,{{"a-live",other},{key,value}});const auto old_id=s.root(key);
        s.transact(s.sequence(),{{key,std::nullopt}});
        auto first=step(s);CHECK(boolean(first,"mark_pending"));
        CHECK(number(first,"objects_deleted")==0&&s.object(old_id)==value);
        // Reusing the old object ID during marking must revive that exact ID;
        // putting the bytes again could mask an unsafe early deletion.
        s.transact(s.sequence(),{{key,std::nullopt,old_id}});
        CHECK(s.root(key)==old_id&&s.object(old_id)==value);
        auto pin=s.pin_objects();auto pinned=step(s);
        CHECK(number(pinned,"mark_work_done")<=1);
        pin.reset();converge(s);CHECK(s.object(s.root(key))==value);
        s.transact(s.sequence(),{{key,std::nullopt}});converge(s);
        CHECK(!s.contains(key)&&s.object(s.root("a-live"))==other);s.verify();
    }
}
void deletion_limits(const std::string& path,bool order_chunks) {
    LevelStore::create(path);
    LevelStore s(path);
    std::vector<Mutation> create;
    std::map<std::string,std::vector<unsigned char>> values;
    constexpr unsigned count=300;
    for(unsigned i=0;i<count;++i){auto key="candidate/"+std::to_string(i);values[key]=payload(10000+i);create.push_back({key,values[key]});}
    s.transact(s.sequence(),create);

    std::vector<std::pair<std::string,std::string>> ordered;
    for(unsigned i=0;i<count;++i){auto key="candidate/"+std::to_string(i);std::string order;if(order_chunks)order=replay::hash(std::string(values[key].begin(),values[key].end()));else {auto id=s.root(key);order.assign(reinterpret_cast<const char*>(id.data()),id.size());}ordered.emplace_back(order,key);}
    std::sort(ordered.begin(),ordered.end());
    // Keep the first key in the phase under test live. The following 127
    // candidates are dead, exposing an overrun if the next batch ignores the
    // allowance already consumed by this call.
    const auto keeper=ordered.front().second;
    std::vector<Mutation> erase;
    for(unsigned i=1;i<count;++i)erase.push_back({ordered[i].second,std::nullopt});
    s.transact(s.sequence(),erase);

    bool objects_reaped=false,chunks_reaped=false;
    std::string last;
    for(unsigned i=0;i<1000;++i){
        last=s.collect_step(1024*1024,128,256*1024*1024,65536);
        const auto objects=number(last,"objects_deleted"),chunks=number(last,"chunks_deleted");
        CHECK(objects<=128&&chunks<=128);
        objects_reaped|=objects>0;chunks_reaped|=chunks>0;
        CHECK(s.object(s.root(keeper))==values[keeper]);
        if(converged(last))break;
        CHECK(i!=999);
    }
    CHECK(objects_reaped&&chunks_reaped);
    CHECK(s.contains(keeper));s.verify();
}
void interrupted_cycle(const std::string& path) {
    LevelStore::create(path);
    {LevelStore s(path);for(unsigned i=0;i<80;++i)s.transact(s.sequence(),{{"root/"+std::to_string(i),payload(i)}});
     for(unsigned i=0;i<80;++i)s.transact(s.sequence(),{{"root/"+std::to_string(i),std::nullopt}});}
    auto scratch=std::filesystem::path(path)/".gc-mark-abandoned";
    std::filesystem::create_directory(scratch);
    std::filesystem::permissions(scratch,std::filesystem::perms::owner_all);
    {std::ofstream corrupt(scratch/"corrupt",std::ios::binary);corrupt<<"partial mark database";}
    // Scratch is not a recovery authority: startup discards corrupt leftovers
    // and starts a fresh pass, which still reclaims the old garbage.
    {LevelStore s(path);CHECK(!std::filesystem::exists(scratch));converge(s);s.verify();}

    // A symlink with the private scratch name is unlinked without traversing
    // its target during startup cleanup.
    const auto outside=std::filesystem::path(path).parent_path()/"outside-scratch";
    std::filesystem::create_directory(outside);{std::ofstream sentinel(outside/"keep");sentinel<<"safe";}
    scratch=std::filesystem::path(path)/".gc-mark-symlink";
    std::filesystem::create_directory_symlink(outside,scratch);
    {LevelStore s(path);s.verify();}
    CHECK(!std::filesystem::exists(scratch));CHECK(std::filesystem::exists(outside/"keep"));
    auto physical=std::filesystem::path(path)/".gc-physical-abandoned";
    std::filesystem::create_directory(physical);
    {std::ofstream corrupt(physical/"corrupt",std::ios::binary);corrupt<<"untrusted partial physical index";}
    {LevelStore s(path);CHECK(!std::filesystem::exists(physical));converge(s);s.verify();}
    physical=std::filesystem::path(path)/".gc-physical-symlink";
    std::filesystem::create_directory_symlink(outside,physical);
    {LevelStore s(path);CHECK(!std::filesystem::exists(physical));CHECK(std::filesystem::exists(outside/"keep"));s.verify();}

    std::filesystem::remove_all(outside);
}
}
int main(){
    auto p=(std::filesystem::temp_directory_path()/"forkfs-incremental-gc-XXXXXX").string();CHECK(mkdtemp(p.data()));
    try {bounded_mark_and_writes(p+"/writes");revival_and_pins(p+"/revival");deletion_limits(p+"/limits-objects",false);deletion_limits(p+"/limits-chunks",true);interrupted_cycle(p+"/restart");
        std::filesystem::remove_all(p);std::cout<<"incremental GC bounded marking, writes, revival and restart passed\n";}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(p);return 1;}
}
