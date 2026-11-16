#include "namespace.h"
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("rename_exclusive check failed at "+std::to_string(line));}
static int errcode(auto f){try{f();}catch(const std::system_error& e){return e.code().value();}catch(const std::exception&){return 1;}return 0;}
static std::string seq(const std::string& s){const std::string key="\"last_committed_seq\":";auto p=s.find(key);check(p!=s.npos);p+=key.size();auto e=s.find_first_not_of("0123456789",p);return s.substr(p,e-p);}
int main(){char pattern[]="/tmp/ff-rename-excl-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
try{for(bool legacy:{false,true}){auto path=std::string(tmp)+(legacy?"/legacy":"/modern");if(legacy)Container::create_legacy(path);else Container::create(path);
 {Container store(path);Namespace fs(store);fs.initialize();fs.mkdir("/a");fs.mkdir("/b");fs.mkdir("/a/tree");fs.write("/a/source",Bytes{'s','r','c'});fs.write("/b/existing",Bytes{'d','s','t'});fs.link("/a/source","/b/alias");fs.symlink("absent","/a/dangle");fs.write("/a/tree/child",Bytes{'c'});
  auto original=fs.lstat("/a/source");Namespace::Attributes attrs;attrs.mode=0600;attrs.modified_ns=123456789;fs.set_attributes("/a/source",attrs);fs.set_xattr("/a/source","user.test",Bytes{'x'});original=fs.lstat("/a/source");
  fs.mkdir("/b/nonempty");fs.write("/b/nonempty/child",Bytes{'n'});auto destination=fs.lstat("/b/existing");auto before=seq(store.status());
  check(errcode([&]{fs.rename_exclusive("/a/source","/b/existing");})==EEXIST&&seq(store.status())==before);
  check(fs.lstat("/a/source").id==original.id&&fs.lstat("/b/existing").id==destination.id&&fs.read("/a/source")==Bytes({'s','r','c'})&&fs.read("/b/existing")==Bytes({'d','s','t'}));
  check(errcode([&]{fs.rename_exclusive("/a/source","/a/source");})==EEXIST&&seq(store.status())==before);
  check(errcode([&]{fs.rename_exclusive("/a/source","/b/alias");})==EEXIST&&seq(store.status())==before);
  check(errcode([&]{fs.rename_exclusive("/a/source","/b/nonempty");})==EEXIST&&seq(store.status())==before);
  check(errcode([&]{fs.rename_exclusive("/a/source","/missing-parent/new");})!=0&&seq(store.status())==before);
  check(errcode([&]{fs.rename_exclusive("/a/tree","/a/tree/inside");})!=0&&seq(store.status())==before);
  check(errcode([&]{fs.rename_exclusive("/a/source","/a/..");})!=0&&seq(store.status())==before);
  check(errcode([&]{fs.rename_exclusive("/a/source","/a/dangle");})==EEXIST&&seq(store.status())==before);
  check(errcode([&]{fs.rename_exclusive("/a/source","/a/tree");})==EEXIST&&seq(store.status())==before);
  fs.rename_exclusive("/a/source","/b/moved");auto moved=fs.lstat("/b/moved");check(moved.id==original.id&&moved.mode==original.mode&&moved.modified_ns==original.modified_ns&&fs.get_xattr("/b/moved","user.test")==Bytes{'x'});check(fs.lstat("/b/alias").id==moved.id&&moved.links==2&&fs.read("/b/moved")==Bytes({'s','r','c'}));
  fs.rename_exclusive("/a/tree","/b/tree");check(fs.read("/b/tree/child")==Bytes{'c'});
  fs.snapshot("base");fs.fork("base","work");Namespace work(store,"work"),old(store,"base",true);auto tree_id=fs.lstat("/b/tree").id;fs.rename_exclusive("/b/tree","/a/main-tree");
  check(fs.lstat("/a/main-tree").id==tree_id&&fs.read("/a/main-tree/child")==Bytes{'c'}&&errcode([&]{fs.lstat("/b/tree");})!=0);
  check(work.lstat("/b/tree").id==tree_id&&work.read("/b/tree/child")==Bytes{'c'}&&old.lstat("/b/tree").id==tree_id&&old.read("/b/tree/child")==Bytes{'c'});
  before=seq(store.status());check(errcode([&]{old.rename_exclusive("/b/tree","/old-tree");})!=0&&seq(store.status())==before);
  fs.write("/a/fork-source",Bytes{'f'});check(errcode([&]{work.lstat("/a/fork-source");})!=0);work.verify();
  // Existing rename retains its replacing semantics and same-inode no-op.
  fs.rename("/b/moved","/b/existing");check(fs.read("/b/existing")==Bytes({'s','r','c'}));before=seq(store.status());fs.rename("/b/existing","/b/alias");check(seq(store.status())==before);
  Namespace::Attributes flag;flag.flags=2;fs.set_link_attributes("/a/fork-source",flag);before=seq(store.status());check(errcode([&]{fs.rename_exclusive("/a/fork-source","/b/existing");})==EPERM&&seq(store.status())==before);flag.flags=0;fs.set_link_attributes("/a/fork-source",flag);
  flag.flags=2;fs.set_link_attributes("/b/existing",flag);before=seq(store.status());check(errcode([&]{fs.rename_exclusive("/a/fork-source","/b/existing");})==EEXIST&&seq(store.status())==before);flag.flags=0;fs.set_link_attributes("/b/existing",flag);
  flag.flags=2;fs.set_link_attributes("/b",flag);before=seq(store.status());check(errcode([&]{fs.rename_exclusive("/a/fork-source","/b/existing");})==EPERM&&seq(store.status())==before);check(errcode([&]{fs.rename_exclusive("/a/fork-source","/b/fresh");})==EPERM&&seq(store.status())==before);flag.flags=0;fs.set_link_attributes("/b",flag);
  flag.flags=4;fs.set_link_attributes("/b",flag);fs.write("/a/append-reject",Bytes{'r'});before=seq(store.status());check(errcode([&]{fs.rename_exclusive("/a/append-reject","/b/existing");})==EEXIST&&seq(store.status())==before);fs.rename_exclusive("/a/fork-source","/b/append-ok");flag.flags=0;fs.set_link_attributes("/b",flag);
  if(!legacy){fs.write("/h",Bytes(8,'h'));auto h=fs.open("/h",Namespace::Access::ReadWrite),d=fs.dup(h);fs.seek(h,3,Namespace::Seek::Set);auto id=fs.handle_stat(h).id;fs.rename_exclusive("/h","/h-moved");check(fs.handle_stat(d).id==id&&fs.seek(d,0,Namespace::Seek::Current)==3);fs.handle_write(h,0,Bytes{'z'});fs.close(d);fs.close(h);store.collect_garbage();check(fs.read_at("/h-moved",0,1)==Bytes{'z'});}
  fs.verify();work.verify();old.verify();store.verify();
 }
 {Container store(path);Namespace fs(store),work(store,"work"),old(store,"base",true);fs.verify();work.verify();old.verify();check(fs.read("/a/main-tree/child")==Bytes{'c'}&&work.read("/b/tree/child")==Bytes{'c'}&&old.read("/b/tree/child")==Bytes{'c'});store.verify();}
 }
 std::filesystem::remove_all(tmp);std::cout<<"exclusive rename, destination atomicity, metadata, protections, revisions, handles and GC passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}}
