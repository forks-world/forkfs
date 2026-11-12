#include "namespace.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("inode path check failed at "+std::to_string(line));}
static std::string sequence(const std::string& status){
 const std::string key="\"last_committed_seq\":";auto begin=status.find(key);check(begin!=status.npos);begin+=key.size();
 auto end=status.find_first_not_of("0123456789",begin);check(end!=begin);return status.substr(begin,end-begin);
}
template<class F> static void missing(F f){bool rejected=false;try{f();}catch(const std::runtime_error& e){rejected=std::string(e.what())=="inode is no longer linked";}check(rejected);}
int main(int argc,char** argv){
 if(argc==3){try{Container store(argv[1]);Namespace fs(store);auto id=fs.stat(argv[2]).id;auto before=sequence(store.status());
   auto begin=std::chrono::steady_clock::now();for(unsigned i=0;i<10;++i){auto path=fs.path_for_inode(id);check(fs.lstat(path).id==id);}
   double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();check(sequence(store.status())==before);
   std::cout<<"{\"seeded_inode_lookups\":10,\"seconds\":"<<seconds<<",\"identity_and_sequence_preserved\":true}\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
 if(argc!=1)return 2;char pattern[]="/tmp/ff-path-hints-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
 try{for(bool legacy:{false,true}){
  auto path=std::string(tmp)+(legacy?"/legacy":"/db");if(legacy)Container::create_legacy(path);else Container::create(path);
  std::string id;
  {Container store(path);Namespace fs(store);fs.initialize();fs.mkdir("/dir");fs.write("/dir/a",Bytes{'A'});id=fs.stat("/dir/a").id;
   auto stable=sequence(store.status());check(fs.path_for_inode(id)=="/dir/a" && sequence(store.status())==stable);
   fs.snapshot("before");fs.fork("before","work");Namespace old(store,"before",true),work(store,"work");
   check(old.path_for_inode(id)=="/dir/a");work.rename("/dir/a","/dir/work");check(work.path_for_inode(id)=="/dir/work" && fs.path_for_inode(id)=="/dir/a");
   fs.rename("/dir","/moved");check(fs.path_for_inode(id)=="/moved/a");fs.mkdir("/dir");fs.write("/dir/a",Bytes{'N'});
   check(fs.path_for_inode(id)=="/moved/a");fs.link("/moved/a","/alias");fs.lstat("/alias");fs.remove("/alias",false);check(fs.path_for_inode(id)=="/moved/a");
   fs.symlink("/moved/a","/final-link");fs.stat("/final-link");auto resolved=fs.path_for_inode(id);check(!fs.lstat(resolved).symlink && fs.lstat(resolved).id==id);
   fs.symlink("/moved","/via");fs.stat("/via/a");check(fs.path_for_inode(id)=="/via/a");
   fs.remove("/via",false);fs.symlink("/dir","/via");check(fs.path_for_inode(id)=="/moved/a");
   fs.remove("/via",false);fs.symlink("/moved","/via");fs.stat("/via/a");fs.remove("/via",false);fs.symlink("/via","/via");check(fs.path_for_inode(id)=="/moved/a");
   fs.remove("/via",false);fs.symlink("/moved","/via");fs.stat("/via/a");fs.remove("/via",false);fs.write("/via",{});check(fs.path_for_inode(id)=="/moved/a");
   if(!legacy){auto fd=fs.open("/moved/a",Namespace::Access::ReadWrite);fs.remove("/moved/a",false);fs.write("/moved/a",Bytes{'R'});
    missing([&]{fs.path_for_inode(id);});check(fs.handle_read(fd,0,1)==Bytes{'A'});fs.close(fd);}
   else {fs.remove("/moved/a",false);fs.write("/moved/a",Bytes{'R'});missing([&]{fs.path_for_inode(id);});}
   check(old.path_for_inode(id)=="/dir/a");fs.drop_world("work");fs.fork("before","work");check(work.path_for_inode(id)=="/dir/a");
   if(!legacy){ // Exceed the global 4096-hint budget using cheap shared Worlds.
    for(unsigned i=0;i<4100;++i){auto name="hint"+std::to_string(i);fs.fork("before",name);Namespace branch(store,name);check(branch.stat("/dir/a").id==id);}
    check(old.path_for_inode(id)=="/dir/a" && work.path_for_inode(id)=="/dir/a");
    store.collect_garbage();check(fs.path_for_inode(fs.stat("/moved/a").id)=="/moved/a");}
   fs.verify();old.verify();store.verify();}
  {Container store(path);Namespace fs(store),old(store,"before",true);missing([&]{fs.path_for_inode(id);});check(old.path_for_inode(id)=="/dir/a");fs.verify();old.verify();store.verify();}
 }
 std::filesystem::remove_all(tmp);std::cout<<"validated inode hints, rename/link/symlink retarget/reuse, views, eviction, orphan, GC and reopen passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}}
