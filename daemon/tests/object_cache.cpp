#include "level_store.h"
#include <leveldb/db.h>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <thread>
#include <unistd.h>

using Bytes=std::vector<unsigned char>;
void check(bool ok) {if(!ok)throw std::runtime_error("verified object cache check failed");}
template<class F> void rejects(F f) {
    bool rejected=false;try{f();}catch(const std::runtime_error&){rejected=true;}check(rejected);
}
int main() {
    char pattern[]="/tmp/ff-cache-XXXXXX";auto directory=mkdtemp(pattern);if(!directory)return 1;
    auto path=std::string(directory)+"/store";
    try {
        forkfs::LevelStore::create(path);Bytes original(64,42);
        auto id=forkfs::Journal::object_id(original);
        {
            forkfs::LevelStore store(path,4096);
            check(!store.contains("head")); // Cached absence must expire on commit.
            store.transact(0,{{"head",original}});check(store.contains("head"));
            check(store.root("head")==id);
            check(store.object(id)==original);
            auto first=store.cache_stats();check(first.misses==1 && first.hits==0);
            auto copy=store.object(id);copy[0]=7; // Callers cannot mutate cached bytes.
            check(store.object(id)==original && store.cache_stats().hits==2);
            Bytes changed(64,43);store.transact(1,{{"head",changed}});check(store.root("head")==forkfs::Journal::object_id(changed));
            check(store.object(id)==original); // Retained old version stays valid.
            check(store.object(store.root("head"))==changed);
            std::atomic<bool> failed=false;
            std::vector<std::thread> readers;
            for(int t=0;t<8;++t)readers.emplace_back([&]{
                try{for(int i=0;i<100;++i)if(store.object(id)!=original)failed=true;}
                catch(...){failed=true;}
            });
            for(auto& reader:readers)reader.join();check(!failed);
            // Exceed the cache budget with distinct objects, then reload evicted data.
            std::atomic<bool> stop_roots=false,root_failed=false;
            std::thread root_reader([&]{try{while(!stop_roots){auto current=store.root("head");(void)store.object(current);}}catch(...){root_failed=true;}});
            try {for(unsigned i=0;i<200;++i) {
                Bytes value(64,static_cast<unsigned char>(i));value[0]=i;value[1]=i>>8;
                store.transact(store.sequence(),{{"head",value}});
                check(store.root("head")==forkfs::Journal::object_id(value));
                check(store.object(store.root("head"))==value);
            }}catch(...){stop_roots=true;root_reader.join();throw;}
            stop_roots=true;root_reader.join();check(!root_failed);
            check(store.cache_stats().charge<=4096);
            auto before=store.cache_stats().misses;
            check(store.object(id)==original && store.cache_stats().misses==before+1);
            store.compact();check(store.object(id)==original);
            store.transact(store.sequence(),{{"head",std::nullopt}});check(!store.contains("head"));
            store.poison();rejects([&]{store.contains("head");});rejects([&]{store.object(id);}); // No cache bypass of poison.
        }
        {
            forkfs::LevelStore store(path,0);check(store.object(id)==original);
            check(store.object(id)==original);
            check(store.cache_stats().hits==0 && store.cache_stats().misses==2);
        }
        // Trusted offline fixture corrupts a payload. Invalid bytes never enter cache.
        {
            leveldb::DB* raw=nullptr;leveldb::Options options;
            check(leveldb::DB::Open(options,path,&raw).ok());std::unique_ptr<leveldb::DB> db(raw);
            auto key="O/"+std::string(reinterpret_cast<const char*>(id.data()),id.size());
            leveldb::WriteOptions write;write.sync=true;
            check(db->Put(write,key,std::string(original.size(),'i')).ok());
        }
        {
            forkfs::LevelStore store(path);
            rejects([&]{store.object(id);});rejects([&]{store.object(id);});
            check(store.cache_stats().hits==0 && store.cache_stats().misses==2);
            auto sequence=store.sequence();
            rejects([&]{store.transact(sequence,{{"must-not-exist",original}});});
            check(store.sequence()==sequence && !store.contains("must-not-exist"));
            rejects([&]{store.verify();});
        }
        std::filesystem::remove_all(directory);
        std::cout<<"verified cache hits, immutability, concurrency, eviction and corruption rejection passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(directory);return 1;}
}
