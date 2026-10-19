#include "object_store.h"
#include <filesystem>
#include <iostream>
#include <random>
#include <set>
#include <fcntl.h>
#include <unistd.h>
using namespace forkfs;
#define CHECK(x) do{if(!(x))throw std::runtime_error("line "+std::to_string(__LINE__)+": " #x);}while(0)
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}CHECK(failed);}
static uint64_t data_bytes(const std::string& path){uint64_t n=0;for(const auto& entry:std::filesystem::directory_iterator(path))if(entry.path().extension()==".seg")n+=entry.file_size();return n;}
int main(){auto path=(std::filesystem::temp_directory_path()/"ff-object-store-XXXXXX").string();CHECK(mkdtemp(path.data()));try{
    // No database: the owning metadata service can supply any location index.
    ObjectStore::Locations index;auto lookup=[&](const std::string& hash)->std::optional<std::string>{auto it=index.find(hash);return it==index.end()?std::nullopt:std::optional(it->second);};
    std::map<std::string,std::pair<std::string,std::string>> objects;std::mt19937 rng(511);
    {
        ObjectStore store(path,4096);
        for(size_t n:{0u,1u,255u,256u,257u,65536u,65537u,262144u}){
            std::string value(n,0);for(auto& byte:value)byte=char(rng());auto id=replay::hash(value);ObjectStore::Locations staged;
            auto descriptor=store.prepare(id,value,lookup,staged);
            CHECK(!staged.empty());rejects([&]{store.read(id,descriptor,lookup);}); // Not published yet.
            store.sync();index.insert(staged.begin(),staged.end());
            CHECK(store.read(id,descriptor,lookup)==value);objects[id]={descriptor,value};
            staged.clear();CHECK(store.prepare(id,value,lookup,staged)==descriptor && staged.empty());
        }
        auto id=replay::hash("x");ObjectStore::Locations staged;auto descriptor=store.prepare(id,"x",lookup,staged);store.sync();index.insert(staged.begin(),staged.end());
        unsigned loads=0;auto load=[&]{++loads;return store.read(id,descriptor,lookup);};
        auto copy=store.cached(id,load);copy[0]='!';CHECK(store.cached(id,load)=="x" && loads==1);
        CHECK(store.cache_stats().hits==1 && store.cache_stats().misses==1 && store.cache_stats().charge<=4096);
        rejects([&]{store.prepare(replay::hash("wrong"),"x",lookup,staged);});
        for(const auto& entry:std::filesystem::directory_iterator(path))CHECK(entry.path().extension()==".seg");
    }
    {ObjectStore reopened(path,0);for(const auto& [id,item]:objects)CHECK(reopened.read(id,item.first,lookup)==item.second);}
    {
        auto measured=(std::filesystem::temp_directory_path()/"ff-object-cost-XXXXXX").string();CHECK(mkdtemp(measured.data()));
        try{
            ObjectStore store(measured,0);ObjectStore::Locations index;
            auto lookup=[&](const std::string& hash)->std::optional<std::string>{auto it=index.find(hash);return it==index.end()?std::nullopt:std::optional(it->second);};
            std::mt19937 seeded(811);uint64_t actual=data_bytes(measured);
            for(size_t size:{1u,45u,65536u,65537u,131072u,257u}){
                std::string value(size,0);for(auto& byte:value)byte=char(seeded());
                std::vector<size_t> growth;std::set<std::string> batch_hashes;
                for(size_t at=0;at<std::max<size_t>(1,(value.size()+65535)/65536);++at){auto chunk=value.substr(at*65536,65536),hash=replay::hash(chunk);if(!index.contains(hash)&&batch_hashes.insert(hash).second)growth.push_back(chunk.size());}
                const auto expected=store.append_cost(growth);ObjectStore::Locations pending;
                auto descriptor=store.prepare(replay::hash(value),value,lookup,pending);(void)descriptor;store.sync();index.insert(pending.begin(),pending.end());
                const auto now=data_bytes(measured);CHECK(now-actual==expected);actual=now;
            }
            // Repeating an object is a pure metadata deduplication: no DATA growth.
            std::string duplicate(65537,'d');ObjectStore::Locations first;
            auto id=replay::hash(duplicate);const auto duplicate_growth=store.append_cost(std::vector<size_t>{65536,1});auto desc=store.prepare(id,duplicate,lookup,first);store.sync();index.insert(first.begin(),first.end());
            auto after_first=data_bytes(measured);CHECK(after_first-actual==duplicate_growth);actual=after_first;
            ObjectStore::Locations again;CHECK(store.prepare(id,duplicate,lookup,again)==desc && again.empty());CHECK(store.append_cost(std::vector<size_t>{})==0 && data_bytes(measured)==actual);
        }catch(...){std::filesystem::remove_all(measured);throw;}
        std::filesystem::remove_all(measured);
    }
    {
        auto near=(std::filesystem::temp_directory_path()/"ff-object-tail-XXXXXX").string();CHECK(mkdtemp(near.data()));
        try{
            const auto segment=near+"/DATA-1.seg";auto header=std::string("FFSEG001")+replay::le(1);header+=replay::hash(header);
            int fd=open(segment.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);CHECK(fd>=0);CHECK(write(fd,header.data(),header.size())==ssize_t(header.size()));
            constexpr uint64_t limit=64*1024*1024;CHECK(ftruncate(fd,off_t(limit-45))==0);CHECK(fsync(fd)==0);close(fd);
            ObjectStore store(near,0);CHECK(store.append_cost(size_t(1))==45);ObjectStore::Locations pending;auto value=std::string("z");auto id=replay::hash(value);
            auto descriptor=store.prepare(id,value,[](const std::string&)->std::optional<std::string>{return std::nullopt;},pending);(void)descriptor;store.sync();CHECK(std::filesystem::file_size(segment)==limit);
            CHECK(store.append_cost(size_t(1))==93);ObjectStore::Locations next;store.prepare(replay::hash("q"),"q",[](const std::string&)->std::optional<std::string>{return std::nullopt;},next);store.sync();CHECK(std::filesystem::file_size(near+"/DATA-2.seg")==93);
            CHECK(ObjectStore::new_store_cost(std::vector<size_t>{1})==93);CHECK(ObjectStore::new_store_cost(std::vector<size_t>{65536,1})==65673);
        }catch(...){std::filesystem::remove_all(near);throw;}
        std::filesystem::remove_all(near);
    }
    {
        auto limited=(std::filesystem::temp_directory_path()/"ff-object-limit-XXXXXX").string();CHECK(mkdtemp(limited.data()));
        try{
            auto segment=limited+"/DATA-1.seg";auto header=std::string("FFSEG001")+replay::le(1);header+=replay::hash(header);
            int fd=open(segment.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);CHECK(fd>=0);CHECK(write(fd,header.data(),header.size())==ssize_t(header.size()));
            constexpr uint64_t limit=64*1024*1024;CHECK(ftruncate(fd,off_t(limit-44))==0);CHECK(fsync(fd)==0);close(fd);
            RepositoryBudget{limit+48,0}.save(limited);ObjectStore store(limited,0);const auto before=data_bytes(limited);ObjectStore::Locations pending;
            rejects([&]{store.prepare(replay::hash("x"),"x",[](const std::string&)->std::optional<std::string>{return std::nullopt;},pending);});
            CHECK(pending.empty() && data_bytes(limited)==before && !std::filesystem::exists(limited+"/DATA-2.seg"));
        }catch(...){std::filesystem::remove_all(limited);throw;}
        std::filesystem::remove_all(limited);
    }
    std::filesystem::remove_all(path);std::cout<<"standalone ObjectStore, staged indexes, empty/tiny objects, dedup and cache passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(path);return 1;}}
