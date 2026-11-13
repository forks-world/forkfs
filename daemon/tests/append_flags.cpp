#include "namespace.h"
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("append flag check at "+std::to_string(line));}
static void flags(Namespace& fs,const std::string& path,uint32_t mask){Namespace::Attributes a;a.flags=mask;fs.set_link_attributes(path,a);}
static std::string logical_state(const std::string& status){
 std::string result;for(const auto* field:{"last_committed_seq","object_roots","indexed_objects"}){
  auto key="\""+std::string(field)+"\":";auto begin=status.find(key);check(begin!=status.npos);begin+=key.size();
  auto end=status.find_first_not_of("0123456789",begin);check(end!=begin);result+=status.substr(begin,end-begin)+"/";
 }return result;
}
template<class F> static void denied(Container& store,F fn,int line=__builtin_LINE()){
 auto before=logical_state(store.status());int error=0;try{fn();}catch(const std::system_error& e){error=e.code().value();}
 if(error!=EPERM)throw std::runtime_error("expected EPERM at "+std::to_string(line)+", got "+std::to_string(error));
 check(logical_state(store.status())==before,line);
}
int main(){char pattern[]="/tmp/ff-append-flags-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
 try{for(bool legacy:{false,true}){auto path=std::string(tmp)+(legacy?"/legacy":"/db");if(legacy)Container::create_legacy(path);else Container::create(path);
  {Container store(path);Namespace fs(store);fs.initialize();fs.mkdir("/dir");fs.write("/f",{'D'});fs.link("/f","/alias");fs.write("/source",{'S'});
   std::string oldfd;if(!legacy)oldfd=fs.open("/f",Namespace::Access::ReadWrite);
   flags(fs,"/f",4);check(fs.stat("/alias").flags==4);
   denied(store,[&]{fs.write("/f",{'X'});});denied(store,[&]{fs.write_at("/alias",0,{'X'});});denied(store,[&]{fs.write_at("/f",2,{'X'});});
   denied(store,[&]{fs.truncate("/f",0);});denied(store,[&]{fs.truncate("/f",1);});denied(store,[&]{fs.truncate("/f",2);});
   denied(store,[&]{fs.link("/f","/extra");});denied(store,[&]{fs.remove("/alias",false);});denied(store,[&]{fs.rename("/f","/moved");});denied(store,[&]{fs.rename("/source","/f");});denied(store,[&]{fs.replace_file("/f",{'X'});});
   Namespace::Attributes a;a.mode=0600;denied(store,[&]{fs.set_attributes("/f",a);});a={};a.flags=0;a.size=0;denied(store,[&]{fs.set_attributes("/f",a);});fs.set_xattr("/f","user.x",{'X'});check(fs.get_xattr("/f","user.x")==Bytes{'X'});fs.remove_xattr("/f","user.x");
   check(fs.append("/f",{'A'})==1);fs.write_at("/f",2,{'B'});check(fs.read("/f")==Bytes({'D','A','B'}));
   if(!legacy){denied(store,[&]{fs.open("/f",Namespace::Access::ReadWrite);});Namespace::OpenOptions opts;opts.append=true;auto fd=fs.open("/f",Namespace::Access::ReadWrite,opts);
    fs.handle_write(fd,0,{'C'});auto duplicate=fs.dup(fd);fs.handle_write_next(duplicate,{'D'});fs.close(duplicate);fs.close(fd);
    denied(store,[&]{fs.handle_write(oldfd,0,{'X'});});denied(store,[&]{fs.handle_truncate(oldfd,5);});a={};a.size=0;denied(store,[&]{fs.handle_set_attributes(oldfd,a);});
    opts={};opts.positioned_append=true;fd=fs.open("/f",Namespace::Access::ReadWrite,opts);denied(store,[&]{fs.handle_write(fd,0,{'X'});});fs.handle_writeback(fd,0,{'D','A','B','C','D'});
    denied(store,[&]{fs.handle_writeback(fd,0,{'X','A','B','C','D','E'});});
    denied(store,[&]{fs.handle_writeback(fd,9,{'X'});});
    fs.handle_writeback(fd,0,{'D','A','B','C','D','E'});check(fs.read("/f")==Bytes({'D','A','B','C','D','E'}));fs.close(fd);fs.close(oldfd);
   }
   fs.snapshot("append");fs.fork("append","work");Namespace old(store,"append",true),work(store,"work");flags(work,"/f",0);work.write("/f",{'W'});check(old.stat("/f").flags==4 && old.read("/f")[0]=='D');
   fs.write("/dir/child",{'C'});flags(fs,"/dir",4);fs.write("/dir/new",{'N'});fs.mkdir("/dir/sub");fs.symlink("/f","/dir/s");fs.rename("/source","/dir/in");fs.link("/dir/child","/dir/h");
   denied(store,[&]{fs.remove("/dir/child",false);});denied(store,[&]{fs.rename("/dir/child","/out");});denied(store,[&]{fs.rename("/dir/new","/dir/child");});denied(store,[&]{fs.replace_file("/dir/child",{'X'});});fs.write("/dir/child",{'M'});flags(fs,"/dir",0);
   flags(fs,"/f",6);denied(store,[&]{fs.append("/f",{'X'});});flags(fs,"/f",0);fs.write_at("/f",0,{'U'});
   if(!legacy){auto orphan=fs.open("/f",Namespace::Access::ReadWrite);fs.remove("/f",false);fs.remove("/alias",false);a={};a.flags=4;fs.handle_set_attributes(orphan,a);
    denied(store,[&]{fs.handle_write(orphan,0,{'X'});});denied(store,[&]{fs.handle_truncate(orphan,0);});fs.handle_append(orphan,{'O'});check(fs.handle_stat(orphan).flags==4);fs.close(orphan);}
   fs.write("/chunked",{});fs.truncate("/chunked",300000);flags(fs,"/chunked",4);fs.append("/chunked",{'Z'});denied(store,[&]{fs.write_at("/chunked",0,{'X'});});check(fs.read_at("/chunked",300000,1)==Bytes{'Z'});
   fs.verify();old.verify();if(!legacy)store.collect_garbage();store.verify();}
  {Container store(path);Namespace old(store,"append",true);check(old.stat("/f").flags==4 && old.read("/f")[0]=='D');old.verify();store.verify();}
 }
 std::filesystem::remove_all(tmp);std::cout<<"append-only data/metadata/namespace, old and duplicated handles, parents, revisions, chunked, orphan and GC passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}}
