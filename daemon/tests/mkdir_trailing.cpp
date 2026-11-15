#include "namespace.h"
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;
static void check(bool value,int line=__builtin_LINE()){if(!value)throw std::runtime_error("mkdir trailing check failed at "+std::to_string(line));}
template<class F>static void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
template<class F>static void too_long(F f){try{f();}catch(const std::system_error& e){check(e.code()==std::error_code(ENAMETOOLONG,std::generic_category()));return;}check(false);}
static uint64_t sequence(const std::string& status){const std::string key="\"last_committed_seq\":";auto at=status.find(key);check(at!=status.npos);at+=key.size();auto end=status.find_first_not_of("0123456789",at);return std::stoull(status.substr(at,end-at));}
int main(){char pattern[]="/tmp/ff-mkdir-trailing-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
try{for(bool legacy:{false,true}){auto path=std::string(tmp)+(legacy?"/legacy":"/modern");if(legacy)Container::create_legacy(path);else Container::create(path);
 {Container store(path);Namespace fs(store);fs.initialize();fs.mkdir("/parent");fs.mkdir("/parent/sub");fs.symlink("/parent","/alias");fs.symlink("/parent/sub","/physical");fs.symlink("/parent/sub","/dirlink");fs.write("/file",{'F'});fs.symlink("/file","/file-link");fs.symlink("/missing","/dangling");
  fs.snapshot("before-create");Namespace before_create(store,"before-create",true);auto before_create_listing=before_create.list("/parent");
  auto parent=fs.stat("/parent");auto seq=sequence(store.status());fs.mkdir("/parent/new///",0700);auto made=fs.stat("/parent/new");check(made.directory && made.mode==0700 && made.links==2);check(sequence(store.status())>seq);check(fs.stat("/parent").links==parent.links+1);
  fs.mkdir("/alias/linked///",0700);check(fs.stat("/parent/linked").directory);fs.mkdir("/physical/../physical-new///",0700);check(fs.stat("/parent/physical-new").directory);
  std::string exact="/parent/padded";exact.append(4096-exact.size(),'/');fs.mkdir(exact,0700);check(fs.stat("/parent/padded").directory);too_long([&]{fs.mkdir(exact+"/");});
  auto before=sequence(store.status());auto links=fs.stat("/parent").links;auto mtime=fs.stat("/parent").modified_ns,ctime=fs.stat("/parent").changed_ns;auto listing=fs.list("/parent");
  for(const auto& bad:{"/","///","/parent/.","/parent/./","/parent/..","/parent/new/","/dirlink/","/file/","/file-link/","/dangling/","/missing/child/","/file/child/"})rejects([&]{fs.mkdir(bad);});
  too_long([&]{fs.mkdir("/parent/"+std::string(256,'x')+"/");});
  auto padded="/parent/"+std::string(4089,'/') ; // raw path over the limit must be rejected before separators are collapsed
  too_long([&]{fs.mkdir(padded);});
  check(sequence(store.status())==before);check(fs.stat("/parent").links==links);check(fs.stat("/parent").modified_ns==mtime&&fs.stat("/parent").changed_ns==ctime);check(fs.list("/parent")==listing);check(before_create.list("/parent")==before_create_listing);rejects([&]{fs.stat("/parent/missing");});
  rejects([&]{before_create.stat("/parent/new");});rejects([&]{before_create.stat("/parent/linked");});rejects([&]{before_create.stat("/parent/physical-new");});rejects([&]{before_create.stat("/parent/padded");});
  fs.snapshot("created");Namespace snap(store,"created",true);check(snap.stat("/parent/new").directory);rejects([&]{snap.stat("/parent/late");});
  fs.verify();store.verify();if(!legacy)store.collect_garbage();
 }
 {Container store(path);Namespace fs(store),before_create(store,"before-create",true);check(fs.stat("/parent/new").mode==0700);for(const auto& missing:{"/parent/new","/parent/linked","/parent/physical-new","/parent/padded"})rejects([&]{before_create.stat(missing);});fs.verify();before_create.verify();store.verify();}}
 std::filesystem::remove_all(tmp);std::cout<<"mkdir trailing slash behavior passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}}
