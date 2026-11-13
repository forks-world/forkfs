#include "namespace.h"
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("immutable check at "+std::to_string(line));}
static void flags(Namespace& fs,const std::string& path,uint32_t mask){Namespace::Attributes a;a.flags=mask;fs.set_link_attributes(path,a);}
template<class F> static void denied(Container& store,F fn){auto before=store.status();bool failed=false;try{fn();}catch(const std::system_error& e){failed=e.code().value()==EPERM;}check(failed && store.status()==before);}
int main(){char pattern[]="/tmp/ff-immutable-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
 try{for(bool legacy:{false,true}){auto path=std::string(tmp)+(legacy?"/legacy":"/db");if(legacy)Container::create_legacy(path);else Container::create(path);
  {Container store(path);Namespace fs(store);fs.initialize();fs.mkdir("/dir");fs.write("/f",{'D'});fs.link("/f","/alias");fs.write("/source",{'S'});fs.symlink("/f","/link");
   fs.set_xattr("/f","user.a",{'X'});std::string fd;if(!legacy)fd=fs.open("/f",Namespace::Access::ReadWrite);
   flags(fs,"/f",2);check(fs.read("/alias")==Bytes{'D'} && fs.stat("/f").flags==2);
   denied(store,[&]{fs.write("/alias",{'X'});});denied(store,[&]{fs.write_at("/f",0,{'X'});});denied(store,[&]{fs.append("/f",{'X'});});denied(store,[&]{fs.truncate("/f",0);});
   denied(store,[&]{fs.link("/f","/newlink");});denied(store,[&]{fs.remove("/alias",false);});denied(store,[&]{fs.rename("/f","/moved");});denied(store,[&]{fs.rename("/source","/f");});denied(store,[&]{fs.replace_file("/f",{'R'});});
   Namespace::Attributes a;a.flags=0;a.size=0;a.mode=0700;denied(store,[&]{fs.set_attributes("/f",a);});check(fs.stat("/f").size==1 && fs.stat("/f").flags==2);
   a={};a.modified_ns=1;denied(store,[&]{fs.set_attributes("/f",a);});denied(store,[&]{fs.set_xattr("/f","user.a",{'Y'});});denied(store,[&]{fs.remove_xattr("/f","user.a");});
   fs.snapshot("immutable");fs.fork("immutable","work");Namespace old(store,"immutable",true),work(store,"work");flags(work,"/f",0);work.write_at("/f",0,{'W'});check(old.read("/f")==Bytes{'D'} && old.stat("/f").flags==2);
   flags(fs,"/f",32770);check(fs.stat("/f").flags==32770);
   if(!legacy){denied(store,[&]{fs.open("/f",Namespace::Access::Write);});auto reader=fs.open("/f");check(fs.handle_read(reader,0,1)==Bytes{'D'});fs.close(reader);
    fs.handle_write(fd,0,{'P'});fs.handle_truncate(fd,3);check(fs.read("/f")==Bytes({'P',0,0}));a={};a.mode=0700;denied(store,[&]{fs.handle_set_attributes(fd,a);});denied(store,[&]{fs.handle_set_xattr(fd,"user.a",{'Y'});});
    a={};a.size=1;fs.handle_set_attributes(fd,a);fs.close(fd);}
   flags(fs,"/f",0);fs.write_at("/f",0,{'U'});check(fs.read("/f")[0]=='U');
   fs.write("/dir/child",{'C'});flags(fs,"/dir",2);
   denied(store,[&]{fs.mkdir("/dir/new");});denied(store,[&]{fs.write("/dir/new",{'N'});});denied(store,[&]{fs.symlink("/f","/dir/s");});denied(store,[&]{fs.link("/f","/dir/h");});denied(store,[&]{fs.remove("/dir/child",false);});denied(store,[&]{fs.rename("/dir/child","/out");});denied(store,[&]{fs.rename("/source","/dir/in");});denied(store,[&]{fs.replace_file("/dir/child",{'R'});});
   fs.write("/dir/child",{'M'});check(fs.read("/dir/child")==Bytes{'M'});flags(fs,"/dir",0);
   flags(fs,"/link",2);check(fs.lstat("/link").flags==2 && fs.stat("/link").flags==0);denied(store,[&]{fs.remove("/link",false);});flags(fs,"/link",0);fs.remove("/link",false);
   if(!legacy){auto orphan=fs.open("/f",Namespace::Access::ReadWrite);fs.remove("/f",false);fs.remove("/alias",false);a={};a.flags=2;fs.handle_set_attributes(orphan,a);fs.handle_write(orphan,0,{'O'});a.mode=0600;denied(store,[&]{fs.handle_set_attributes(orphan,a);});a={};a.flags=0;fs.handle_set_attributes(orphan,a);fs.close(orphan);}
   old.verify();fs.verify();if(!legacy)store.collect_garbage();store.verify();}
  {Container store(path);Namespace old(store,"immutable",true);check(old.stat("/f").flags==2 && old.read("/f")==Bytes{'D'});old.verify();store.verify();}
 }
 std::filesystem::remove_all(tmp);std::cout<<"user immutable admission, mutation atomicity, parents, old writers, revisions, legacy, orphan and GC passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}}
