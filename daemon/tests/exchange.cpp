#include "namespace.h"
#include "metadata_view.h"
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <system_error>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("exchange check failed at "+std::to_string(line));}
static int errcode(auto f){try{f();}catch(const std::system_error& e){return e.code().value();}catch(const std::exception&){return 1;}return 0;}
static std::string seq(const std::string& status){auto key=std::string("\"last_committed_seq\":");auto at=status.find(key);check(at!=status.npos);at+=key.size();auto end=status.find_first_not_of("0123456789",at);return status.substr(at,end-at);}
static const std::string prefix("\0forkfs/",8),history("\0forkfs-history/",16);
static std::string hex(const std::array<unsigned char,32>& id){const char* d="0123456789abcdef";std::string out;for(auto c:id){out+=d[c>>4];out+=d[c&15];}return out;}
static void patch_counts(const std::string& path,const std::map<std::string,uint64_t>& counts){
 forkfs::Journal journal(path);forkfs::NamespaceView view(journal,"main");forkfs::NamespaceView::Changes delta;std::map<std::string,Bytes> objects;
 auto add=[&](const Bytes& bytes){auto id=forkfs::Journal::object_id(bytes);objects.emplace(history+"object/"+hex(id),bytes);return id;};
 for(const auto& [id,count]:counts){auto key=prefix+"inode/"+id;auto bytes=view.get(key);check(bytes.size()>=24);for(unsigned i=0;i<8;++i)bytes[16+i]=count>>(8*i);delta[key]=add(bytes);}
 auto plan=view.plan(delta);for(const auto& bytes:plan.objects)add(bytes);std::vector<forkfs::Mutation> batch;
 for(auto& [key,bytes]:objects)if(!journal.contains(key))batch.push_back({key,std::move(bytes)});
 batch.push_back({history+"world/main",std::move(plan.manifest)});journal.transact(journal.sequence(),batch);
}
static void link_limit_cases(const std::string& path){
 std::string left_id,right_id;uint64_t left_links,right_links;
 {Container store(path);Namespace fs(store);left_id=fs.stat("/left").id;right_id=fs.stat("/right").id;left_links=fs.stat("/left").links;right_links=fs.stat("/right").links;fs.mkdir("/left/cap-dir");fs.mkdir("/right/cap-dir");}
 patch_counts(path,{{left_id,forkfs::maximum_link_count}});
 {Container store(path);Namespace fs(store);fs.exchange("/left/plain","/left/sub");}
 patch_counts(path,{{left_id,left_links+1}});
 patch_counts(path,{{left_id,forkfs::maximum_link_count},{right_id,forkfs::maximum_link_count}});
 {Container store(path);Namespace fs(store);fs.exchange("/left/sub","/right/cap-dir");}
 patch_counts(path,{{left_id,left_links+1},{right_id,right_links+1}});
 // With the source parent full, importing a directory must fail atomically.
 auto full_before=[&](){Container store(path);Namespace fs(store);return seq(store.status());};
 patch_counts(path,{{left_id,forkfs::maximum_link_count}});auto before=full_before();
 {Container store(path);Namespace fs(store);check(errcode([&]{fs.exchange("/left/plain","/right/cap-dir");})==EMLINK);check(seq(store.status())==before);}
 patch_counts(path,{{left_id,left_links+1}});
}
int main(){char pattern[]="/tmp/ff-exchange-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
try{for(bool legacy:{false,true}){auto path=std::string(tmp)+(legacy?"/legacy":"/db");if(legacy)Container::create_legacy(path);else Container::create(path);
 {Container store(path);Namespace fs(store);fs.initialize();
  fs.mkdir("/left");fs.mkdir("/right");fs.mkdir("/left/tree");fs.write("/left/a",Bytes{'A'});fs.write("/right/b",Bytes{'B'});fs.symlink("missing-target","/left/dangling");fs.link("/left/a","/right/hard");
  auto a=fs.lstat("/left/a"),b=fs.lstat("/right/b"),dangling=fs.lstat("/left/dangling"),left=fs.stat("/left"),right=fs.stat("/right");
  fs.exchange("/left/a","/right/b");auto after_a=fs.lstat("/right/b"),after_b=fs.lstat("/left/a");check(fs.read("/left/a")==Bytes{'B'}&&fs.read("/right/b")==Bytes{'A'});check(after_a.id==a.id&&after_b.id==b.id&&after_a.mode==a.mode&&after_b.mode==b.mode&&after_a.size==a.size&&after_b.size==b.size&&after_a.modified_ns==a.modified_ns&&after_b.modified_ns==b.modified_ns);check(fs.lstat("/right/hard").id==a.id&&fs.lstat("/right/hard").links==2);
  fs.exchange("/left/dangling","/right/b");check(fs.lstat("/left/dangling").id==a.id&&fs.readlink("/right/b")=="missing-target");check(fs.lstat("/right/b").id==dangling.id);
  fs.write("/left/tree/child",Bytes{'T'});fs.exchange("/left/tree","/right/b");check(fs.lstat("/right/b").directory&&fs.read("/right/b/child")==Bytes{'T'});check(fs.lstat("/left/tree").symlink&&fs.readlink("/left/tree")=="missing-target");
  check(fs.stat("/left").links==left.links-1&&fs.stat("/right").links==right.links+1);
  fs.mkdir("/left/sub");fs.write("/left/plain",Bytes{'P'});auto old_links=fs.stat("/left").links;fs.exchange("/left/sub","/left/plain");check(fs.stat("/left").links==old_links&&fs.lstat("/left/plain").directory&&fs.read("/left/sub")==Bytes{'P'});
  fs.mkdir("/left/plain/inner");auto before=seq(store.status());check(errcode([&]{fs.exchange("/left/plain","/left/plain/inner");})==1);check(errcode([&]{fs.exchange("/left/plain/inner","/left/plain");})==1);check(seq(store.status())==before);
  before=seq(store.status());check(errcode([&]{fs.exchange("/left/no-such","/right/b");})==1);check(errcode([&]{fs.exchange("/left/a","/right/no-such");})==1);check(seq(store.status())==before);
  before=seq(store.status());for(const auto& pair:std::vector<std::pair<std::string,std::string>>{{"/","/right/b"},{"/left/a/","/right/b"},{"/left/.","/right/b"}})check(errcode([&]{fs.exchange(pair.first,pair.second);})==1);check(seq(store.status())==before);
  before=seq(store.status());auto unchanged=fs.lstat("/left/sub");fs.exchange("/left/sub","/left/sub");check(seq(store.status())==before&&fs.lstat("/left/sub").changed_ns==unchanged.changed_ns);
  before=seq(store.status());fs.exchange("/left/dangling","/right/hard");check(seq(store.status())==before&&fs.lstat("/left/dangling").id==fs.lstat("/right/hard").id);
  fs.write("/immutable",Bytes{'I'});Namespace::Attributes attrs;attrs.flags=2;fs.set_link_attributes("/immutable",attrs);before=seq(store.status());check(errcode([&]{fs.exchange("/immutable","/left/a");})==EPERM);check(seq(store.status())==before);attrs.flags=0;fs.set_link_attributes("/immutable",attrs);
  attrs.flags=2;fs.set_link_attributes("/left/a",attrs);before=seq(store.status());check(errcode([&]{fs.exchange("/left/sub","/left/a");})==EPERM);check(seq(store.status())==before);attrs.flags=0;fs.set_link_attributes("/left/a",attrs);
  fs.write("/append",Bytes{'X'});attrs.flags=4;fs.set_link_attributes("/append",attrs);before=seq(store.status());check(errcode([&]{fs.exchange("/append","/left/a");})==EPERM);check(seq(store.status())==before);attrs.flags=0;fs.set_link_attributes("/append",attrs);
  attrs.flags=4;fs.set_link_attributes("/append",attrs);before=seq(store.status());check(errcode([&]{fs.exchange("/left/sub","/append");})==EPERM);check(seq(store.status())==before);attrs.flags=0;fs.set_link_attributes("/append",attrs);
  attrs.flags=2;fs.set_link_attributes("/left",attrs);before=seq(store.status());check(errcode([&]{fs.exchange("/left/a","/right/b");})==EPERM);check(seq(store.status())==before);attrs.flags=0;fs.set_link_attributes("/left",attrs);
  attrs.flags=4;fs.set_link_attributes("/right",attrs);before=seq(store.status());check(errcode([&]{fs.exchange("/left/a","/right/b");})==EPERM);check(seq(store.status())==before);attrs.flags=0;fs.set_link_attributes("/right",attrs);
  attrs.flags=4;fs.set_link_attributes("/left",attrs);before=seq(store.status());check(errcode([&]{fs.exchange("/left/a","/right/b");})==EPERM);check(seq(store.status())==before);attrs.flags=0;fs.set_link_attributes("/left",attrs);
  fs.snapshot("before");fs.fork("before","work");Namespace old(store,"before",true),work(store,"work");before=seq(store.status());check(errcode([&]{old.exchange("/left/a","/right/b");})==1&&seq(store.status())==before);fs.exchange("/left/a","/right/b");check(old.lstat("/left/a").id==work.lstat("/left/a").id&&old.lstat("/left/a").id!=fs.lstat("/left/a").id);
  if(!legacy){fs.write("/h1",Bytes(8,'1'));fs.write("/h2",Bytes(8,'2'));auto h1=fs.open("/h1",Namespace::Access::ReadWrite),d1=fs.dup(h1);auto h2=fs.open("/h2",Namespace::Access::ReadWrite),d2=fs.dup(h2);fs.seek(h1,3,Namespace::Seek::Set);fs.seek(h2,5,Namespace::Seek::Set);auto id1=fs.handle_stat(h1).id,id2=fs.handle_stat(h2).id;fs.exchange("/h1","/h2");check(fs.handle_stat(h1).id==id1&&fs.handle_stat(d1).id==id1&&fs.handle_stat(h2).id==id2&&fs.handle_stat(d2).id==id2);check(fs.lstat("/h1").id==id2&&fs.lstat("/h2").id==id1);check(fs.seek(d1,0,Namespace::Seek::Current)==3&&fs.seek(d2,0,Namespace::Seek::Current)==5);fs.handle_write(h1,0,Bytes{'Z'});check(fs.read_at("/h2",0,1)==Bytes{'Z'});fs.close(d1);fs.close(h1);fs.close(d2);fs.close(h2);}
  fs.verify();old.verify();work.verify();if(!legacy)store.collect_garbage();store.verify();
 }
 {Container store(path);Namespace fs(store),old(store,"before",true);fs.verify();old.verify();store.verify();if(!legacy)check(fs.read_at("/h2",0,1)==Bytes{'Z'});}
 if(!legacy)link_limit_cases(path);
 {Container store(path);Namespace fs(store);fs.verify();store.verify();}
 }
 std::filesystem::remove_all(tmp);std::cout<<"atomic entry exchange, identity, mixed types, cycles, protections, snapshots, handles, reopen and GC passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}}
