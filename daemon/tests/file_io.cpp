#include "namespace.h"
#include <atomic>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <unistd.h>
using Bytes=std::vector<unsigned char>;
void check(bool ok){if(!ok)throw std::runtime_error("file I/O check failed");}
template<class F> void rejects(F f){bool bad=false;try{f();}catch(const std::exception&){bad=true;}check(bad);}
int main(){
    auto pattern=(std::filesystem::temp_directory_path()/"ff-io-XXXXXX").string();auto tmp=mkdtemp(pattern.data());if(!tmp)return 1;
    auto path=std::string(tmp)+"/store";
    try{
        forkfs::Container::create(path);
        {
            forkfs::Container store(path);forkfs::Namespace fs(store);fs.initialize();fs.write("/f",Bytes{'a','b','c','d','e','f'});
            fs.link("/f","/alias");auto identity=fs.stat("/f").id;
            fs.snapshot("base");fs.fork("base","work");forkfs::Namespace work(store,"work"),old(store,"base",true);
            work.write_at("/f",2,Bytes{'X','Y'});check(work.read("/alias")==Bytes({'a','b','X','Y','e','f'}));
            work.write_at("/f",9,Bytes{'Z'});check(work.read("/f")==Bytes({'a','b','X','Y','e','f',0,0,0,'Z'}));
            check(work.read_at("/f",5,UINT64_MAX)==Bytes({'f',0,0,0,'Z'}));
            check(work.read_at("/f",INT64_MAX,5).empty());check(work.read_at("/f",0,0).empty());
            work.truncate("/f",3);work.truncate("/f",6);check(work.read("/f")==Bytes({'a','b','X',0,0,0}));
            check(work.append("/alias",Bytes{'!','?'})==6);
            check(work.stat("/alias").id==identity && work.stat("/alias").size==8);
            check(old.read("/f")==Bytes({'a','b','c','d','e','f'}) && fs.read("/f")==old.read("/f"));
            auto before=store.status();auto attrs=work.stat("/f");
            work.write_at("/f",INT64_MAX,{});work.truncate("/f",8);check(work.append("/f",{})==8);
            check(before==store.status() && attrs.modified_ns==work.stat("/f").modified_ns);
            rejects([&]{old.write_at("/f",0,{});});rejects([&]{old.append("/f",{});});rejects([&]{old.truncate("/f",6);});
            rejects([&]{work.write_at("/f",UINT64_MAX,Bytes{1});});rejects([&]{work.read_at("/f",UINT64_MAX,1);});
            rejects([&]{work.write_at("/f",256*1024,Bytes{1});});rejects([&]{work.truncate("/f",256*1024+1);});
            rejects([&]{work.write_at("/missing",0,{});});rejects([&]{work.truncate("/",0);});
            check(before==store.status());
            work.truncate("/f",256*1024);work.write_at("/f",256*1024-1,Bytes{42});
            check(work.read_at("/f",256*1024-1,8)==Bytes{42});rejects([&]{work.append("/f",Bytes{1});});
            work.truncate("/f",0);
            std::atomic<bool> failed=false;std::mutex mutex;std::set<uint64_t> offsets;
            std::vector<std::thread> workers;
            for(unsigned t=0;t<8;++t)workers.emplace_back([&,t]{
                try{forkfs::Namespace writer(store,"work");for(unsigned i=0;i<16;++i){
                    Bytes record(8,t);record[1]=i;auto offset=writer.append("/f",record);
                    std::lock_guard lock(mutex);if(!offsets.insert(offset).second)failed=true;
                }}catch(...){failed=true;}
            });
            for(auto& w:workers)w.join();check(!failed && offsets.size()==128);
            auto bytes=work.read("/f");check(bytes.size()==128*8);std::set<std::pair<unsigned,unsigned>> records;
            for(size_t i=0;i<bytes.size();i+=8){check(bytes[i]<8 && bytes[i+1]<16);
                for(unsigned j=2;j<8;++j)check(bytes[i+j]==bytes[i]);records.emplace(bytes[i],bytes[i+1]);}
            check(records.size()==128);
            // Concurrent disjoint writes must read/modify/publish under one lock.
            work.truncate("/f",16);workers.clear();
            for(unsigned t=0;t<2;++t)workers.emplace_back([&,t]{try{
                forkfs::Namespace writer(store,"work");for(unsigned i=0;i<16;++i)writer.write_at("/f",t*8,Bytes(8,t+10));
            }catch(...){failed=true;}});
            for(auto& w:workers)w.join();check(!failed);
            check(work.read_at("/f",0,8)==Bytes(8,10) && work.read_at("/f",8,8)==Bytes(8,11));
            fs.verify();work.verify();old.verify();store.verify();
        }
        {forkfs::Container store(path);forkfs::Namespace work(store,"work"),old(store,"base",true);
            check(work.read_at("/alias",8,8)==Bytes(8,11));check(old.read("/f")==Bytes({'a','b','c','d','e','f'}));work.verify();}
        std::filesystem::remove_all(tmp);std::cout<<"offset I/O, zero fill, truncation, atomic append and concurrent updates passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
