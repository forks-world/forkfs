#include "namespace.h"
#include "metadata_view.h"
#include <cstring>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("large file check failed at line "+std::to_string(line));}
template<class F> static void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
int main(){
    char pattern[]="/tmp/ff-io-large-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
    auto path=std::string(tmp)+"/db";constexpr uint64_t gib=uint64_t{1}<<30,chunk=65536;
    const std::vector<uint64_t> offsets{64*1024*1024-2,10000*chunk+17,4*gib+123,5*gib-4};
    std::string id;
    try{
        Container::create(path);
        {
            Container store(path);Namespace fs(store);fs.initialize();fs.write("/f",Bytes{'o','l','d'});fs.snapshot("small");
            fs.truncate("/f",5*gib);id=fs.stat("/f").id;
            for(auto offset:offsets){fs.write_at("/f",offset,Bytes{'D','A','T','A'});check(fs.read_at("/f",offset,4)==Bytes({'D','A','T','A'}));check(fs.read_at("/f",offset-2,2)==Bytes(2,0));}
            fs.link("/f","/alias");check(fs.stat("/alias").size==5*gib);fs.snapshot("large");
            fs.fork("large","work");Namespace work(store,"work"),old(store,"large",true);
            // A metadata-only change must compare sparse allocation records,
            // rather than visiting every hole in the 1 TiB logical address space.
            work.truncate("/f",Namespace::maximum_file_size);work.write_at("/f",Namespace::maximum_file_size-4,Bytes{'T','A','I','L'});
            work.snapshot("limit");Namespace::Attributes a;a.mode=0700;work.set_attributes("/f",a);work.snapshot("mode");
            auto diff=fs.diff("limit","mode");check(diff.find("\"content_changed\":true")==diff.npos);
            work.write_at("/f",Namespace::maximum_file_size-1,Bytes{'!'});work.snapshot("edited");
            check(fs.diff("mode","edited").find("\"content_changed\":true")!=std::string::npos);
            // Separate inode identities and absent zero chunks still compare by bytes.
            work.write("/holes",{});work.truncate("/holes",Namespace::maximum_file_size);work.snapshot("holes");
            work.write("/replacement",{});work.truncate("/replacement",Namespace::maximum_file_size);work.rename("/replacement","/holes");work.snapshot("sameholes");
            check(fs.diff("holes","sameholes").find("\"content_changed\":true")==std::string::npos);
            auto stable=store.status();rejects([&]{work.write_at("/f",Namespace::maximum_file_size,Bytes{1});});
            rejects([&]{work.truncate("/f",Namespace::maximum_file_size+1);});rejects([&]{work.append("/f",Bytes{1});});check(store.status()==stable);
            check(old.read_at("/f",4*gib+123,4)==Bytes({'D','A','T','A'}));rejects([&]{old.write_at("/f",0,Bytes{1});});
            auto fd=work.open("/f",Namespace::Access::ReadWrite);work.remove("/f",false);work.remove("/alias",false);
            check(work.handle_stat(fd).size==Namespace::maximum_file_size && work.handle_stat(fd).links==0);
            work.handle_write(fd,4*gib+123,Bytes{'O','P','E','N'});check(work.handle_read(fd,4*gib+123,4)==Bytes({'O','P','E','N'}));
            work.handle_truncate(fd,4*gib+125);work.handle_truncate(fd,5*gib);
            check(work.handle_read(fd,4*gib+123,4)==Bytes({'O','P',0,0}));work.verify();work.close(fd);
            fs.truncate("/f",64*1024*1024);fs.truncate("/f",5*gib);
            check(fs.read_at("/f",64*1024*1024-2,4)==Bytes({'D','A',0,0}));check(fs.read_at("/f",4*gib+123,4)==Bytes(4,0));
            // Leave a large orphan for restart recovery (the handle is process-local).
            fs.write("/orphan",{});fs.write_at("/orphan",5*gib,Bytes{1});fs.open("/orphan",Namespace::Access::ReadWrite);fs.remove("/orphan",false);
            fs.verify();work.verify();old.verify();rejects([&]{store.collect_garbage();});store.verify();
        }
        {
            Container store(path);Namespace fs(store),old(store,"large",true),small(store,"small",true),limit(store,"limit",true);
            check(small.read("/f")==Bytes({'o','l','d'}));check(old.stat("/f").size==5*gib);
            check(old.read_at("/f",4*gib+123,4)==Bytes({'D','A','T','A'}));check(limit.read_at("/f",Namespace::maximum_file_size-4,8)==Bytes({'T','A','I','L'}));
            fs.verify();old.verify();limit.verify();store.collect_garbage();store.verify();
            // Shrinking a large file below the legacy limit discards all extended
            // keys; the surviving inode can safely use the small-file format.
            fs.truncate("/f",17);fs.verify();
        }
        {
            forkfs::Journal journal(path);forkfs::NamespaceView live(journal,"main"),large(journal,"large",true);
            const std::string prefix("\0forkfs/",8);auto key=prefix+"inode/"+id;
            check(std::memcmp(live.get(key).data(),"FFINOD02",8)==0);check(std::memcmp(large.get(key).data(),"FFINOD03",8)==0);
            auto records=large.keys(prefix+"chunk/"+id+"/");check(records.size()<16);
            check(large.contains(prefix+"chunk/"+id+"/1023"));check(large.contains(prefix+"chunk/"+id+"/x00001024"));
            check(large.contains(prefix+"chunk/"+id+"/x00010000"));
        }
        std::filesystem::remove_all(tmp);std::cout<<"1 TiB sparse files, 4 GiB offsets, sparse diff, orphan recovery, v2/v3 formats and GC passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
