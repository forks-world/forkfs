#include "namespace.h"
#include <filesystem>
#include <iostream>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("trailing path check failed at "+std::to_string(line));}
template<class F> static void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
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
  auto before_mutations=sequence(store.status());rejects([&]{fs.mkdir("/new/");});rejects([&]{fs.remove("/dir/",true);});rejects([&]{fs.rename("/dir/","/moved");});rejects([&]{fs.link("/file","/new/");});Namespace::OpenOptions create;create.create=true;rejects([&]{fs.open("/created/",Namespace::Access::ReadWrite,create);});check(sequence(store.status())==before_mutations);
  check(fs.stat(std::string(4096,'/')).id==fs.stat("/").id);rejects([&]{fs.stat(std::string(4097,'/'));});
  std::string long_path="/";for(unsigned i=0;i<15;++i){std::string name(255,static_cast<char>('a'+i));long_path+=name;fs.mkdir(long_path);long_path+="/";}long_path+=std::string(253,'z');fs.mkdir(long_path);check(long_path.size()==4094);
  auto long_id=fs.lstat(long_path+"/").id;check(fs.path_for_inode(long_id)==long_path);auto child_path=fs.path_for_inode(long_id)+"/x";check(child_path.size()==4096);fs.write(child_path,Bytes{'X'});check(fs.read(child_path)==Bytes{'X'});
  fs.verify();old.verify();store.verify();if(!legacy)store.collect_garbage();}
 {Container store(path);Namespace fs(store),old(store,"before-trailing",true),work(store,"trailing-work");check(fs.stat("/dir/").directory);check(old.stat("/dir/").mode==0755);check(work.stat("/dir/").mode==0755);fs.verify();old.verify();work.verify();store.verify();}
 }
 std::filesystem::remove_all(tmp);std::cout<<"trailing slash resolution, directory traversal and unsupported mutation paths passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}}
