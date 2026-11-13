#include "namespace.h"
#include "metadata_view.h"
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <cstdio>
#include <system_error>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using forkfs::Journal;using Bytes=std::vector<unsigned char>;
static const std::string prefix("\0forkfs/",8),history("\0forkfs-history/",16);
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("link limit check at "+std::to_string(line));}
static std::string hex(const std::array<unsigned char,32>& id){const char* d="0123456789abcdef";std::string out;for(auto c:id){out+=d[c>>4];out+=d[c&15];}return out;}
static void count(Bytes& inode,uint64_t value){check(inode.size()>=48);for(unsigned i=0;i<8;++i)inode[16+i]=value>>(8*i);}
static void publish(Journal& journal,const forkfs::NamespaceView& view,const std::map<std::string,Bytes>& values){
 forkfs::NamespaceView::Changes delta;std::map<std::string,Bytes> objects;
 auto add=[&](const Bytes& b){auto id=Journal::object_id(b);objects.emplace(history+"object/"+hex(id),b);return id;};
 for(const auto& [key,bytes]:values)delta[key]=add(bytes);auto plan=view.plan(delta);for(const auto& bytes:plan.objects)add(bytes);
 std::vector<forkfs::Mutation> batch;for(auto& [key,bytes]:objects)if(!journal.contains(key))batch.push_back({key,std::move(bytes)});
 batch.push_back({history+"world/main",std::move(plan.manifest)});journal.transact(journal.sequence(),batch);
}
static void patch_count(const std::string& path,const std::string& id,uint64_t value){Journal journal(path);forkfs::NamespaceView view(journal,"main");auto key=prefix+"inode/"+id;auto bytes=view.get(key);count(bytes,value);publish(journal,view,{{key,bytes}});}
static std::string logical_state(const std::string& status){
 std::string result;for(const auto* field:{"last_committed_seq","object_roots","indexed_objects"}){
  auto key="\""+std::string(field)+"\":";auto begin=status.find(key);check(begin!=status.npos);begin+=key.size();
  auto end=status.find_first_not_of("0123456789",begin);check(end!=begin);result+=status.substr(begin,end-begin)+"/";
 }return result;
}
template<class F> static void expect_error(Container& store,int expected,F fn,int line=__builtin_LINE()){
 // Background storage metrics can change on reads; compare publication/root
 // state, not timings or counters unrelated to namespace mutation.
 auto before=logical_state(store.status());int error=0;try{fn();}catch(const std::system_error& e){error=e.code().value();}
 if(error!=expected)throw std::runtime_error("expected errno "+std::to_string(expected)+" at "+std::to_string(line)+", got "+std::to_string(error));
 check(logical_state(store.status())==before,line);
}
template<class F> static void full(Container& store,F fn,int line=__builtin_LINE()){expect_error(store,EMLINK,fn,line);}
static void flags(Namespace& fs,const std::string& path,uint32_t value){Namespace::Attributes a;a.flags=value;fs.set_link_attributes(path,a);}
static void fixture(const std::string& path){
 Container::create(path);std::string id,dir;
 {Container store(path);Namespace fs(store);fs.initialize();fs.mkdir("/links");fs.write("/file",{'D'});id=fs.stat("/file").id;dir=fs.stat("/links").id;}
 {Journal journal(path);for(unsigned begin=0;begin<forkfs::maximum_link_count-1;begin+=256){forkfs::NamespaceView view(journal,"main");std::map<std::string,Bytes> values;
   auto end=std::min<uint64_t>(begin+256,forkfs::maximum_link_count-1);
   for(unsigned i=begin;i<end;++i){char name[32];snprintf(name,sizeof name,"n%06u",i);values[prefix+"entry/"+dir+"/"+name]=Bytes(id.begin(),id.end());}
   auto key=prefix+"inode/"+id;auto bytes=view.get(key);count(bytes,end+1);values[key]=bytes;publish(journal,view,values);
  }}
 {Container store(path);Namespace fs(store);check(fs.stat("/file").links==forkfs::maximum_link_count);fs.snapshot("link_limit_base");fs.verify();store.verify();}
}
int main(int argc,char** argv){
 if(argc==3 && std::string(argv[1])=="--fixture"){try{fixture(argv[2]);std::cout<<"{\"links\":"<<forkfs::maximum_link_count<<",\"bulk_fixture\":true}\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
 if(argc!=1)return 2;char pattern[]="/tmp/ff-link-limits-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
 try{auto path=std::string(tmp)+"/db";Container::create(path);std::string file,parent;
  {Container store(path);Namespace fs(store);fs.initialize();fs.write("/file",{'D'});fs.link("/file","/alias");fs.mkdir("/full");fs.mkdir("/full/empty");fs.mkdir("/from");fs.mkdir("/from/moving");file=fs.stat("/file").id;parent=fs.stat("/full").id;fs.snapshot("valid");}
  // Bounded synthetic counters isolate each arithmetic boundary. They are
  // restored to actual reference counts before full namespace verification.
  patch_count(path,file,forkfs::maximum_link_count-1);patch_count(path,parent,forkfs::maximum_link_count);
  {Container store(path);Namespace fs(store);fs.link("/file","/last");check(fs.stat("/file").links==forkfs::maximum_link_count);
   flags(fs,"/file",2);expect_error(store,EPERM,[&]{fs.link("/file","/overflow");});flags(fs,"/file",0);
   full(store,[&]{fs.link("/file","/overflow");});check(fs.list("/")==std::vector<std::string>({"alias","file","from","full","last"}));
   fs.remove("/last",false);check(fs.stat("/file").links==forkfs::maximum_link_count-1);fs.link("/file","/reused");
   flags(fs,"/full",2);expect_error(store,EPERM,[&]{fs.mkdir("/full/overflow");});flags(fs,"/full",0);
   full(store,[&]{fs.mkdir("/full/overflow");});
   flags(fs,"/full",2);expect_error(store,EPERM,[&]{fs.rename("/from/moving","/full/new");});flags(fs,"/full",0);
   full(store,[&]{fs.rename("/from/moving","/full/new");});
   check(fs.list("/from")==std::vector<std::string>{"moving"});fs.rename("/from/moving","/full/empty");check(fs.stat("/full").links==forkfs::maximum_link_count);}
  patch_count(path,file,3);patch_count(path,parent,3);
  {Container store(path);Namespace fs(store),old(store,"valid",true);check(old.stat("/file").links==2 && fs.read("/reused")==Bytes{'D'});fs.verify();old.verify();store.collect_garbage();store.verify();}
  {Container store(path);Namespace fs(store);check(fs.stat("/file").links==3 && fs.stat("/full").links==3);fs.verify();store.verify();}
  std::filesystem::remove_all(tmp);std::cout<<"EMLINK boundaries, failure atomicity, capacity reuse, net-zero directory replacement, revision and GC passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}}
