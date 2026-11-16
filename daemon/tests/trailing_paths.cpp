#include "namespace.h"
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <tuple>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("trailing path check failed at "+std::to_string(line));}
template<class F> static void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
template<class F> static int error(F f){try{f();}catch(const std::system_error& e){return e.code().value();}catch(const std::exception&){return -1;}return 0;}
static std::string sequence(const std::string& status){const std::string key="\"last_committed_seq\":";auto p=status.find(key);check(p!=status.npos);p+=key.size();auto e=status.find_first_not_of("0123456789",p);return status.substr(p,e-p);}
int main(){char pattern[]="/tmp/ff-trailing-paths-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
try{for(bool legacy:{false,true}){auto path=std::string(tmp)+(legacy?"/legacy":"/db");if(legacy)Container::create_legacy(path);else Container::create(path);
 {Container store(path);Namespace fs(store);fs.initialize();fs.mkdir("/dir");fs.mkdir("/dir/sub");fs.write("/file",Bytes{'F'});fs.symlink("/dir","/dirlink");fs.symlink("/file","/filelink");fs.symlink("/absent","/dangling");fs.symlink("/loop","/loop");
  fs.snapshot("before-trailing");fs.fork("before-trailing","trailing-work");Namespace old(store,"before-trailing",true),work(store,"trailing-work");
  auto stable=sequence(store.status());check(fs.stat("////").id==fs.stat("/").id);check(fs.stat("/dir/").id==fs.lstat("/dir/").id);check(fs.stat("/dirlink/").id==fs.stat("/dir").id);check(fs.lstat("/dirlink/").id==fs.stat("/dir").id);check(sequence(store.status())==stable);
  auto dir=fs.lstat("/dir/");check(fs.path_for_inode(dir.id)=="/dir");check(sequence(store.status())==stable);
  Namespace::Attributes attrs;attrs.mode=0700;fs.set_attributes("/dir/",attrs);check(fs.lstat("/dir").mode==0700);
  Namespace::Attributes alias_attrs;alias_attrs.mode=0701;auto link_id=fs.lstat("/dirlink").id;fs.set_link_attributes("/dirlink/",alias_attrs);
  check(fs.stat("/dir").mode==0701 && fs.lstat("/dirlink").id==link_id && fs.lstat("/dirlink").mode==0777);
  check(old.stat("/dir/").mode==0755 && work.stat("/dir/").mode==0755);
  auto invalid_seq=sequence(store.status());check(fs.read("/file")==Bytes{'F'});rejects([&]{fs.read("/dir/");});rejects([&]{fs.stat("/file/");});rejects([&]{fs.stat("/filelink/");});rejects([&]{fs.stat("/dangling/");});rejects([&]{fs.stat("/loop/");});rejects([&]{fs.readlink("/dir/");});check(sequence(store.status())==invalid_seq);
  fs.mkdir("/new///");check(fs.stat("/new").directory);fs.remove("/new",true);
  // Slash mutations act on the physically resolved directory and preserve
  // the final symlink entry, including destinations through dangling links.
  fs.mkdir("/remove-target");fs.symlink("/remove-target","/remove-alias");auto remove_link=fs.lstat("/remove-alias").id;
  auto before_slash=sequence(store.status());fs.remove("/remove-alias/",true);
  check(fs.lstat("/remove-alias").id==remove_link&&fs.lstat("/remove-alias").symlink&&error([&]{fs.stat("/remove-target");})!=0);
  fs.mkdir("/rename-target");fs.symlink("/rename-target","/rename-alias");auto rename_link=fs.lstat("/rename-alias").id;auto target_id=fs.stat("/rename-target").id;
  fs.rename("/rename-alias/","/renamed-target");check(fs.lstat("/rename-alias").id==rename_link&&fs.lstat("/rename-alias").symlink&&fs.lstat("/renamed-target").id==target_id);
  fs.mkdir("/replace-source");fs.mkdir("/replace-target");fs.symlink("/replace-target","/replace-alias");auto replace_link=fs.lstat("/replace-alias").id;auto source_id=fs.stat("/replace-source").id;
  fs.rename("/replace-source","/replace-alias/");check(fs.lstat("/replace-alias").id==replace_link&&fs.stat("/replace-target").id==source_id);
  fs.mkdir("/dangling-source");fs.symlink("/made-by-rename","/dangling-alias");auto dangling_link=fs.lstat("/dangling-alias").id;auto dangling_id=fs.stat("/dangling-source").id;
  fs.rename("/dangling-source","/dangling-alias/");check(fs.lstat("/dangling-alias").id==dangling_link&&fs.stat("/made-by-rename").id==dangling_id);
  for(const auto& [alias,target,physical]:std::vector<std::tuple<std::string,std::string,std::string>>{{"/relative-tail","new-relative/","/new-relative"},{"/dot-relative-tail","./new-dot-relative/","/new-dot-relative"},{"/parent-relative-tail","dir/../new-parent-relative/","/new-parent-relative"}}) {
   fs.mkdir("/tail-source");fs.symlink(target,alias);auto link_id=fs.lstat(alias).id;auto moved_id=fs.stat("/tail-source").id;
   fs.rename("/tail-source",alias+"/");check(fs.lstat(alias).id==link_id&&fs.stat(physical).id==moved_id);
  }
  fs.mkdir("/lexical-parent");fs.mkdir("/physical-parent");fs.mkdir("/move-dest");fs.mkdir("/physical-parent/tree");fs.write("/physical-parent/tree/data",Bytes{'d'});
  fs.symlink("/physical-parent/tree","/lexical-parent/alias");auto alias_id=fs.lstat("/lexical-parent/alias").id;
  auto lexical_links=fs.stat("/lexical-parent").links,physical_links=fs.stat("/physical-parent").links,dest_links=fs.stat("/move-dest").links;
  auto moved_tree=fs.stat("/physical-parent/tree").id;fs.rename("/lexical-parent/alias/","/move-dest/moved");
  check(fs.lstat("/lexical-parent/alias").id==alias_id&&fs.stat("/move-dest/moved").id==moved_tree&&fs.read("/move-dest/moved/data")==Bytes{'d'});
  check(fs.stat("/lexical-parent").links==lexical_links&&fs.stat("/physical-parent").links==physical_links-1&&fs.stat("/move-dest").links==dest_links+1);
  fs.symlink("explicit-dot/.","/explicit-dot-alias");fs.mkdir("/explicit-dot-source");fs.symlink("/","/root-alias");
  fs.write("/slash-file",Bytes{'f'});fs.mkdir("/slash-dir");fs.mkdir("/slash-empty");fs.symlink("/absent-trailing-target","/slash-dangling");
  auto failed_seq=sequence(store.status());
  check(error([&]{fs.remove("/slash-file/",false);})==ENOTDIR);
  check(error([&]{fs.remove("/slash-dangling/",true);})==ENOENT);
#ifdef __APPLE__
  check(error([&]{fs.remove("/slash-dir/",false);})==EPERM);
#else
  check(error([&]{fs.remove("/slash-dir/",false);})==EISDIR);
#endif
  check(error([&]{fs.rename("/slash-file/","/unused");})==ENOTDIR);
  check(error([&]{fs.rename("/slash-dangling/","/unused");})==ENOENT);
  check(error([&]{fs.rename("/missing-source","/slash-dangling/");})==ENOENT);
  check(error([&]{fs.rename("/slash-file","/explicit-dot-alias/");})==ENOENT);
  check(error([&]{fs.remove("/root-alias/",true);})!=0&&error([&]{fs.remove("/slash-dir/../",true);})!=0);
  check(error([&]{fs.rename("/slash-file","/slash-empty/");})==EISDIR);
  check(error([&]{fs.rename_exclusive("/slash-file","/slash-empty/");})==EEXIST);
  check(error([&]{fs.rename("/slash-file","/slash-dangling/");})==ENOENT);
  check(error([&]{fs.rename("/dir/","/dirlink/sub/new/");})!=0);
  std::string raw4096="/"+std::string(4093,'/')+"x/",raw4097="/"+std::string(4094,'/')+"x/";
  check(raw4096.size()==4096&&error([&]{fs.rename(raw4096,"/unused");})==ENOENT);
  check(error([&]{fs.rename(raw4097,"/unused");})==ENAMETOOLONG);
  check(error([&]{fs.remove("/"+std::string(256,'x')+"/",true);})==ENAMETOOLONG);
  check(sequence(store.status())==failed_seq&&sequence(store.status())!=before_slash);
  check(fs.read("/slash-file")==Bytes{'f'}&&fs.stat("/slash-empty").directory&&fs.lstat("/slash-dangling").symlink);
  auto before_mutations=sequence(store.status());rejects([&]{fs.remove("/dir/",true);});rejects([&]{fs.rename("/dir/","/dir/sub/new/");});rejects([&]{fs.link("/file","/new/");});Namespace::OpenOptions create;create.create=true;rejects([&]{fs.open("/created/",Namespace::Access::ReadWrite,create);});check(sequence(store.status())==before_mutations);
  check(fs.stat(std::string(4096,'/')).id==fs.stat("/").id);rejects([&]{fs.stat(std::string(4097,'/'));});
  std::string long_path="/";for(unsigned i=0;i<15;++i){std::string name(255,static_cast<char>('a'+i));long_path+=name;fs.mkdir(long_path);long_path+="/";}long_path+=std::string(253,'z');fs.mkdir(long_path);check(long_path.size()==4094);
  auto long_id=fs.lstat(long_path+"/").id;check(fs.path_for_inode(long_id)==long_path);auto child_path=fs.path_for_inode(long_id)+"/x";check(child_path.size()==4096);fs.write(child_path,Bytes{'X'});check(fs.read(child_path)==Bytes{'X'});
  fs.verify();old.verify();store.verify();if(!legacy)store.collect_garbage();}
 {Container store(path);Namespace fs(store),old(store,"before-trailing",true),work(store,"trailing-work");check(fs.stat("/dir/").directory);check(old.stat("/dir/").mode==0755);check(work.stat("/dir/").mode==0755);fs.verify();old.verify();work.verify();store.verify();}
 }
 std::filesystem::remove_all(tmp);std::cout<<"trailing slash resolution and mkdir behavior passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}}
