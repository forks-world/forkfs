#include "namespace.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <random>
#include <unistd.h>
using forkfs::Container;
using forkfs::Namespace;
using Bytes=std::vector<unsigned char>;
static void check(bool ok){if(!ok)throw std::runtime_error("chunked file check failed");}
template<class F> static void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
static Bytes read_all(Namespace& fs,const std::string& path){
    Bytes result;for(uint64_t offset=0;;){auto part=fs.read_at(path,offset,Namespace::maximum_io_size);if(part.empty())return result;result.insert(result.end(),part.begin(),part.end());offset+=part.size();}
}
int main(){
    char pattern[]="/tmp/ff-io-chunks-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
    auto path=std::string(tmp)+"/db";std::mt19937 random(1729);Bytes retained;
    try {
        Container::create(path);
        {
            Container store(path);Namespace fs(store);fs.initialize();fs.write("/f",Bytes(256*1024,0x71));fs.link("/f","/alias");
            fs.snapshot("inline");fs.write_at("/f",256*1024-13,Bytes(128*1024,0x83));
            Bytes expected(256*1024+128*1024-13,0x71);std::fill(expected.begin()+256*1024-13,expected.end(),0x83);
            check(read_all(fs,"/alias")==expected);fs.snapshot("chunked");retained=expected;
            fs.fork("chunked","work");Namespace work(store,"work"),old(store,"chunked",true),inlined(store,"inline",true);
            check(inlined.read_at("/f",256*1024-1,8)==Bytes{0x71});
            // Differential test against a byte vector: unaligned writes, holes,
            // shrink/extend and both inode aliases use one publication path.
            for(unsigned step=0;step<100;++step){
                if(step%4==0){auto size=random()%(3*1024*1024);work.truncate("/f",size);expected.resize(size,0);}
                else {auto offset=random()%(3*1024*1024);Bytes data(random()%(128*1024));for(auto& c:data)c=static_cast<unsigned char>(random());
                    work.write_at(step%2?"/f":"/alias",offset,data);if(!data.empty()){expected.resize(std::max<size_t>(expected.size(),offset+data.size()),0);std::copy(data.begin(),data.end(),expected.begin()+offset);}}
                check(read_all(work,"/alias")==expected);
            }
            Namespace::Attributes attrs;attrs.size=Namespace::maximum_file_size;work.set_attributes("/f",attrs);
            check(work.read_at("/f",Namespace::maximum_file_size-8,8)==Bytes(8,0));
            work.write_at("/f",Namespace::maximum_file_size-1,Bytes{0xff});
            auto before=store.status();rejects([&]{work.append("/f",Bytes{1});});rejects([&]{work.truncate("/f",Namespace::maximum_file_size+1);});
            rejects([&]{work.read_at("/f",0,UINT64_MAX);});rejects([&]{work.write_at("/f",0,Bytes(Namespace::maximum_io_size+1));});check(store.status()==before);
            // Retain every dense chunk as an orphan. This exceeds the 16 MiB
            // value batch cap, so unlink must publish existing object references.
            fs.write("/dense",{});Bytes block(Namespace::maximum_io_size);
            constexpr uint64_t dense_size=64*1024*1024;
            for(uint64_t offset=0;offset<dense_size;offset+=block.size()){
                for(auto& c:block)c=static_cast<unsigned char>(random());fs.write_at("/dense",offset,block);
            }
            auto handle=fs.open("/dense",Namespace::Access::ReadWrite);auto last=fs.read_at("/dense",dense_size-block.size(),block.size());
            fs.remove("/dense",false);fs.verify();check(fs.handle_read(handle,dense_size-block.size(),block.size())==last);
            fs.handle_write(handle,65533,Bytes{1,2,3,4,5,6});check(fs.handle_read(handle,65533,6)==Bytes({1,2,3,4,5,6}));
            attrs.size=65535;fs.handle_set_attributes(handle,attrs);fs.handle_truncate(handle,128*1024);
            check(fs.handle_read(handle,65533,6)==Bytes({1,2,0,0,0,0}));fs.verify();fs.close(handle);
            // Open hard links keep a sparse inode alive after its names are removed.
            fs.write("/sparse-orphan",{});fs.write_at("/sparse-orphan",1024*1024-1,Bytes{0x5a});fs.link("/sparse-orphan","/sparse-alias");
            auto sparse=fs.open("/sparse-alias",Namespace::Access::ReadWrite);fs.remove("/sparse-orphan",false);fs.remove("/sparse-alias",false);
            check(fs.handle_stat(sparse).size==1024*1024 && fs.handle_stat(sparse).links==0);
            fs.handle_write(sparse,0,Bytes{0});check(fs.handle_read(sparse,0,1)==Bytes{0});
            check(fs.handle_read(sparse,1024*1024-1,2)==Bytes{0x5a});check(fs.handle_stat(sparse).size==1024*1024);fs.verify();fs.close(sparse);
            // Promoting an unlinked inline zero file must tolerate absent zero chunks.
            fs.write("/inline-zero-orphan",Bytes(32,0));auto inline_zero=fs.open("/inline-zero-orphan",Namespace::Access::ReadWrite);fs.remove("/inline-zero-orphan",false);
            fs.handle_write(inline_zero,Namespace::maximum_io_size+17,Bytes(8,0));
            check(fs.handle_read(inline_zero,0,32)==Bytes(32,0));check(fs.handle_read(inline_zero,Namespace::maximum_io_size+17,8)==Bytes(8,0));
            check(fs.handle_read(inline_zero,Namespace::maximum_io_size+25,1).empty());fs.close(inline_zero);
            // Replacing an existing nonzero orphan chunk with zeros removes it.
            fs.write("/nonzero-orphan",{});fs.write_at("/nonzero-orphan",1024*1024-1,Bytes{0x7f});
            auto nonzero=fs.open("/nonzero-orphan",Namespace::Access::ReadWrite);fs.remove("/nonzero-orphan",false);
            fs.handle_write(nonzero,1024*1024-1,Bytes{0});check(fs.handle_stat(nonzero).size==1024*1024);
            check(fs.handle_read(nonzero,1024*1024-1,1)==Bytes{0});check(fs.handle_read(nonzero,1024*1024,1).empty());fs.verify();fs.close(nonzero);
            // Original inline and chunked revisions survive replacement and GC.
            fs.write("/f",Bytes{'n'});check(fs.read("/alias")==Bytes{'n'});
            check(read_all(old,"/f")==retained);fs.verify();work.verify();old.verify();store.verify();
            check(store.collect_garbage()>0);check(read_all(old,"/alias")==retained);
            auto dead=fs.open("/alias",Namespace::Access::ReadWrite);fs.handle_truncate(dead,1024*1024);fs.remove("/f",false);fs.remove("/alias",false);
            fs.handle_write(dead,900000,Bytes{'R'}); // leave a chunked orphan for recovery
        }
        {
            Container store(path);Namespace fs(store),old(store,"chunked",true),work(store,"work");
            fs.verify();old.verify();work.verify();check(read_all(old,"/f")==retained);
            check(work.read_at("/f",Namespace::maximum_file_size-1,8)==Bytes{0xff});store.collect_garbage();store.verify();
        }
        std::filesystem::remove_all(tmp);std::cout<<"chunked files, 64 MiB dense / 1 TiB sparse bounds, differential I/O, orphan retention, revisions, restart and GC passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
