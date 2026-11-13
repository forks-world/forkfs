#include "namespace.h"
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("volume name check at "+std::to_string(line));}
static uint64_t sequence(Container& store){auto s=store.status();auto key=std::string("\"last_committed_seq\":");auto at=s.find(key);check(at!=s.npos);return std::stoull(s.substr(at+key.size()));}
template<class F>static void rejected(Container& store,F fn){auto before=sequence(store);bool failed=false;try{fn();}catch(const std::exception&){failed=true;}check(failed && sequence(store)==before);}
int main(){char pattern[]="/tmp/ff-volume-name-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
 try{for(bool legacy:{false,true}){auto path=std::string(tmp)+(legacy?"/legacy":"/db");if(legacy)Container::create_legacy(path);else Container::create(path);
  {Container store(path);Namespace fs(store);fs.initialize();fs.write("/file",{'D'});auto root=fs.stat("/");check(fs.volume_name()=="ForkFS");fs.snapshot("default");
   fs.set_volume_name("Research 卷📦");check(fs.volume_name()=="Research 卷📦");auto seq=sequence(store);fs.set_volume_name("Research 卷📦");check(sequence(store)==seq);
   check(fs.stat("/").id==root.id && fs.stat("/").changed_ns==root.changed_ns && fs.read("/file")==std::vector<unsigned char>{'D'});
   for(auto name:{std::string(""),std::string("."),std::string(".."),std::string("a/b"),std::string("a\nb"),std::string("a\0b",3),std::string("\xff"),std::string(256,'x')})rejected(store,[&]{fs.set_volume_name(name);});
   fs.snapshot("named");fs.fork("named","work");Namespace old(store,"named",true),work(store,"work"),def(store,"default",true);
   check(def.volume_name()=="ForkFS");work.set_volume_name("work");check(old.volume_name()=="Research 卷📦" && fs.volume_name()==old.volume_name());
   rejected(store,[&]{old.set_volume_name("bad");});
   auto diff=fs.diff("default","named");check(diff.find("\"volume_name_changed\":true")!=diff.npos && diff.find("\"changes\":[]")!=diff.npos);
   fs.set_volume_name(std::string(255,'x'));fs.set_volume_name("quote\"slash\\");fs.snapshot("escaped");check(fs.diff("named","escaped").find("quote\\\"slash\\\\")!=diff.npos);
   fs.restore("named","before_restore");check(fs.volume_name()=="Research 卷📦");fs.set_volume_name("ForkFS");check(old.volume_name()=="Research 卷📦" && fs.volume_name()=="ForkFS");
   fs.verify();old.verify();work.verify();def.verify();if(!legacy)store.collect_garbage();store.verify();}
  {Container store(path);Namespace old(store,"named",true),fs(store),work(store,"work"),safety(store,"before_restore",true);check(fs.volume_name()=="ForkFS" && old.volume_name()=="Research 卷📦" && work.volume_name()=="work" && safety.volume_name()=="quote\"slash\\");old.verify();fs.verify();work.verify();store.verify();}
 }
 std::filesystem::remove_all(tmp);std::cout<<"revision-backed volume names: validation, identity, idempotency, diff, fork, restore, legacy, restart and GC passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}}
