#include "namespace.h"
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static constexpr uint64_t C=65536;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("seek region check failed at "+std::to_string(line));}
template<class F> static void no_region(F f){try{f();}catch(const std::system_error& e){check(e.code().value()==ENXIO);return;}throw std::runtime_error("missing ENXIO");}
int main(){char pattern[]="/tmp/ff-seek-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
 try{for(bool legacy:{false,true}){
  auto path=std::string(tmp)+(legacy?"/legacy":"/db");if(legacy)Container::create_legacy(path);else Container::create(path);
  {Container store(path);Namespace fs(store);fs.initialize();fs.write("/f",{});
   for(bool data:{false,true})no_region([&]{fs.seek_region("/f",0,data);});
   fs.write("/f",Bytes{'A',0});check(fs.seek_region("/f",1,true)==1 && fs.seek_region("/f",0,false)==2);
   fs.truncate("/f",Namespace::maximum_file_size);check(fs.seek_region("/f",0,false)==C);
   fs.write_at("/f",1023*C+3,Bytes{1});fs.write_at("/f",1024*C,Bytes{1});fs.write_at("/f",Namespace::maximum_file_size-1,Bytes{1});
   check(fs.seek_region("/f",C,true)==1023*C);check(fs.seek_region("/f",1023*C+4,false)==1025*C);
   check(fs.seek_region("/f",1024*C+17,true)==1024*C+17);
   check(fs.seek_region("/f",Namespace::maximum_file_size-1,false)==Namespace::maximum_file_size);
   for(bool data:{false,true})no_region([&]{fs.seek_region("/f",Namespace::maximum_file_size,data);});
   fs.snapshot("sparse");fs.fork("sparse","work");Namespace old(store,"sparse",true),work(store,"work");
   fs.write_at("/f",1023*C+3,Bytes{0});check(fs.seek_region("/f",C,true)==1024*C && old.seek_region("/f",C,true)==1023*C);
   work.truncate("/f",C);check(work.seek_region("/f",0,false)==C);no_region([&]{work.seek_region("/f",C,true);});
   fs.write("/dense",{});fs.truncate("/dense",200*C);
   for(unsigned i=0;i<140;++i)fs.write_at("/dense",i*C,Bytes{1});
   check(fs.seek_region("/dense",7,false)==140*C);check(fs.seek_region("/dense",140*C,false)==140*C);
   no_region([&]{fs.seek_region("/dense",140*C,true);});
   auto stable=store.status();fs.seek_region("/dense",0,false);check(store.status()==stable);
   if(!legacy){auto fd=fs.open("/f",Namespace::Access::ReadWrite);auto dup=fs.dup(fd);fs.remove("/f",false);fs.write("/f",Bytes{'N'});
    check(fs.handle_seek_region(dup,C,true)==1024*C);fs.handle_write(fd,1024*C,Bytes{0});
    check(fs.handle_seek_region(dup,C,true)==Namespace::maximum_file_size-C);check(old.seek_region("/f",C,true)==1023*C);
    check(fs.seek_region("/f",0,false)==1);fs.close(fd);fs.close(dup);}
   fs.verify();old.verify();if(!legacy)store.collect_garbage();store.verify();}
  {Container store(path);Namespace old(store,"sparse",true);check(old.seek_region("/f",C,true)==1023*C);old.verify();store.verify();}
 }
 std::filesystem::remove_all(tmp);std::cout<<"sparse seek regions, EOF, paged dense extents, legacy, revisions, orphans and GC passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}}
