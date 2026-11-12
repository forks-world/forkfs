// Manual Namespace API probe. Always creates a NEW owned disposable repository.
#include "namespace.h"
#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv){if(argc!=3)return 2;
 try{size_t used=0;auto count=std::stoul(argv[2],&used);if(used!=std::string(argv[2]).size() || !count || count>100000)throw std::runtime_error("count must be 1..100000");
  forkfs::Container::create(argv[1]);double seconds=0,last=0;
  {forkfs::Container store(argv[1]);forkfs::Namespace fs(store);fs.initialize();fs.mkdir("/created");
   auto begin=std::chrono::steady_clock::now(),window=begin;
   for(unsigned i=0;i<count;++i){if(count>=1000 && i==count-1000)window=std::chrono::steady_clock::now();
    char name[64];snprintf(name,sizeof name,"/created/n%06u",i);fs.write(name,{});}
   auto end=std::chrono::steady_clock::now();seconds=std::chrono::duration<double>(end-begin).count();last=std::chrono::duration<double>(end-window).count();
   fs.snapshot("create_base");fs.verify();store.verify();}
  {forkfs::Container store(argv[1]);forkfs::Namespace fs(store),old(store,"create_base",true);
   if(fs.list("/created").size()!=count)throw std::runtime_error("created entries missing");
   fs.write_at("/created/n000000",0,{'M'});if(!old.read("/created/n000000").empty())throw std::runtime_error("revision changed");
   store.collect_garbage();old.verify();fs.verify();store.verify();}
  std::cout<<"{\"entries\":"<<count<<",\"create_seconds\":"<<seconds<<",\"last_1000_seconds\":"<<last<<",\"reopen_gc_revision_verified\":true}\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
