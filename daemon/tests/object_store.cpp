#include "object_store.h"
#include <filesystem>
#include <iostream>
#include <random>
using namespace forkfs;
#define CHECK(x) do{if(!(x))throw std::runtime_error("line "+std::to_string(__LINE__)+": " #x);}while(0)
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}CHECK(failed);}
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
    std::filesystem::remove_all(path);std::cout<<"standalone ObjectStore, staged indexes, empty/tiny objects, dedup and cache passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(path);return 1;}}
