#include "namespace.h"
#include <atomic>
#include <barrier>
#include <filesystem>
#include <iostream>
#include <set>
#include <thread>
#include <unistd.h>
using namespace forkfs;
using Bytes=std::vector<unsigned char>;
#define CHECK(ok) do{if(!(ok))throw std::runtime_error("large concurrency mismatch line "+std::to_string(__LINE__));}while(0)
int main(){
    auto name=(std::filesystem::temp_directory_path()/"ff-large-race-XXXXXX").string();auto tmp=mkdtemp(name.data());if(!tmp)return 1;auto path=std::string(tmp)+"/store";
    try{
        Container::create(path);
        {Container store(path);Namespace fs(store);fs.initialize();fs.write("/f",{});fs.truncate("/f",uint64_t(1)<<40);fs.snapshot("base");fs.fork("base","branch");
            constexpr unsigned threads=4,rounds=24;std::barrier start(threads);std::atomic<bool> failed=false;
            std::vector<std::thread> workers;
            // Different byte ranges in the SAME data block detect lost RMW updates.
            auto base=(uint64_t(1)<<32)+65520;
            std::vector<std::string> positioned;
            for(unsigned t=0;t<threads;++t)positioned.push_back(fs.open("/f",Namespace::Access::ReadWrite));
            for(unsigned t=0;t<threads;++t)workers.emplace_back([&,t]{try{Namespace writer(store);start.arrive_and_wait();
                for(unsigned i=0;i<rounds;++i){Bytes record(8,t+1);record[1]=i;writer.handle_write(positioned[t],base+i*threads*8+t*8,record);start.arrive_and_wait();}
            }catch(...){failed=true;start.arrive_and_drop();}});
            for(auto& worker:workers)worker.join();CHECK(!failed);
            for(unsigned i=0;i<rounds;++i)for(unsigned t=0;t<threads;++t){Bytes record(8,t+1);record[1]=i;CHECK(fs.read_at("/f",base+i*threads*8+t*8,8)==record);}
            for(const auto& h:positioned)fs.close(h);
            Namespace old(store,"base",true),branch(store,"branch");CHECK(old.read_at("/f",base,32)==Bytes(32,0));CHECK(branch.read_at("/f",base,32)==Bytes(32,0));
            // Appenders cross a block boundary on a retained, unlinked large file.
            fs.write("/append",{});fs.truncate("/append",262140);Namespace::OpenOptions opt;opt.append=true;
            std::vector<std::string> handles;for(unsigned t=0;t<threads;++t)handles.push_back(fs.open("/append",Namespace::Access::ReadWrite,opt));
            fs.snapshot("before-unlink");fs.remove("/append",false);std::barrier append_start(threads);workers.clear();
            for(unsigned t=0;t<threads;++t)workers.emplace_back([&,t]{try{Namespace writer(store);append_start.arrive_and_wait();for(unsigned i=0;i<rounds;++i){Bytes record(16,t+1);record[1]=i;writer.handle_write(handles[t],0,record);}}catch(...){failed=true;}});
            for(auto& worker:workers)worker.join();CHECK(!failed);
            auto output=fs.handle_read(handles[0],262140,UINT64_MAX);CHECK(output.size()==threads*rounds*16);
            std::set<std::pair<unsigned,unsigned>> found;
            for(size_t i=0;i<output.size();i+=16){auto t=output[i],n=output[i+1];CHECK(t>=1 && t<=threads && n<rounds);for(unsigned j=2;j<16;++j)CHECK(output[i+j]==t);CHECK(found.emplace(t,n).second);}
            CHECK(found.size()==threads*rounds);Namespace before(store,"before-unlink",true);CHECK(before.stat("/append").size==262140);fs.verify();before.verify();
            // Atomic replacement leaves old inode handles attached to the old data.
            fs.write("/replace",Bytes(400000,1));auto h=fs.open("/replace",Namespace::Access::ReadWrite);auto id=fs.handle_stat(h).id;
            fs.replace_file("/replace",Bytes(500000,2));CHECK(fs.stat("/replace").id!=id);CHECK(fs.handle_stat(h).links==0);
            fs.handle_write(h,350000,Bytes{9});CHECK(fs.handle_read(h,349999,3)==Bytes({1,9,1}));CHECK(fs.read_at("/replace",349999,3)==Bytes(3,2));fs.close(h);
            for(const auto& handle:handles)fs.close(handle);fs.verify();store.verify();
        }
        {Container store(path);Namespace fs(store);CHECK(fs.read_at("/replace",499999,2)==Bytes{2});fs.verify();}
        Namespace::verify_recovery(path);std::filesystem::remove_all(tmp);return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
