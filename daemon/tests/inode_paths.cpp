#include "namespace.h"
#include "metadata_view.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
using forkfs::Journal;using forkfs::NamespaceView;using forkfs::Mutation;
static const std::string ns_prefix("\0forkfs/",8),history_prefix("\0forkfs-history/",16);
static std::string hex_id(const std::array<unsigned char,32>& id){const char* digits="0123456789abcdef";std::string out;for(auto b:id){out+=digits[b>>4];out+=digits[b&15];}return out;}
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("inode path check failed at "+std::to_string(line));}
static std::string sequence(const std::string& status){
 const std::string key="\"last_committed_seq\":";auto begin=status.find(key);check(begin!=status.npos);begin+=key.size();
 auto end=status.find_first_not_of("0123456789",begin);check(end!=begin);return status.substr(begin,end-begin);
}
template<class F> static void missing(F f){bool rejected=false;try{f();}catch(const std::runtime_error& e){rejected=std::string(e.what())=="inode is no longer linked";}check(rejected);}
static void corrupt_entry_name(const std::string& path,const std::string& parent,const std::string& old_name,const std::string& bad_name){
 Journal journal(path);NamespaceView view(journal,"main");auto old_key=ns_prefix+"entry/"+parent+"/"+old_name;
 auto new_key=ns_prefix+"entry/"+parent+"/"+bad_name;auto value=view.reference(old_key);check(value.has_value());
 NamespaceView::Changes delta{{old_key,std::nullopt},{new_key,*value}};auto plan=view.plan(delta);std::vector<Mutation> batch;
 for(auto& object:plan.objects){auto id=Journal::object_id(object);auto key=history_prefix+"object/"+hex_id(id);if(!journal.contains(key))batch.push_back({key,std::move(object)});}
 batch.push_back({history_prefix+"world/main",std::move(plan.manifest)});journal.transact(journal.sequence(),batch);
}
int main(int argc,char** argv){
 if(argc==3){try{Container store(argv[1]);Namespace fs(store);auto id=fs.stat(argv[2]).id;auto before=sequence(store.status());
   auto begin=std::chrono::steady_clock::now();for(unsigned i=0;i<10;++i){auto path=fs.path_for_inode(id);check(fs.lstat(path).id==id);}
   double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();check(sequence(store.status())==before);
   std::cout<<"{\"seeded_inode_lookups\":10,\"seconds\":"<<seconds<<",\"identity_and_sequence_preserved\":true}\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
 if(argc!=1)return 2;char pattern[]="/tmp/ff-path-hints-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
 try{for(bool legacy:{false,true}){
  auto path=std::string(tmp)+(legacy?"/legacy":"/db");if(legacy)Container::create_legacy(path);else Container::create(path);
  std::string id;std::vector<std::string> control_names;for(unsigned char c=1;c<32;++c)control_names.emplace_back(1,static_cast<char>(c));
  control_names.emplace_back(1,static_cast<char>(0x7f));control_names.emplace_back("\xc2\x80");control_names.emplace_back("\xc2\x9f");
  {Container store(path);Namespace fs(store);fs.initialize();fs.mkdir("/dir");fs.write("/dir/a",Bytes{'A'});id=fs.stat("/dir/a").id;
   // Directory names and symlink targets accept all non-NUL UTF-8 scalars.
   for(size_t i=0;i<control_names.size();++i){auto name=control_names[i];fs.write("/"+name,Bytes{static_cast<unsigned char>(i)});check(fs.read("/"+name)==Bytes{static_cast<unsigned char>(i)});}
   auto listed=fs.list("/");for(const auto& name:control_names)check(std::find(listed.begin(),listed.end(),name)!=listed.end());
   fs.link("/"+control_names[1],"/hard\nlink");fs.rename("/hard\nlink","/renamed\rlink");
   check(fs.read("/renamed\rlink")==Bytes{1});
   fs.mkdir("/directory\n\t");fs.write("/directory\n\t/child\rfile",Bytes{'D'});check(fs.read("/directory\n\t/child\rfile")==Bytes{'D'});
   fs.symlink(control_names[8],"/follow\tlink");check(fs.read("/follow\tlink")==Bytes{8});
   std::string target="target\n\t\r";target.push_back(static_cast<char>(0x7f));target+="\xc2\x80\xc2\x9f";
   fs.symlink(target,"/symbolic\nlink");check(fs.readlink("/symbolic\nlink")==target);check(fs.lstat("/symbolic\nlink").symlink);
   fs.snapshot("controls");
   auto unchanged=sequence(store.status());auto rejects=[&](const std::string& value){bool failed=false;try{fs.write(value,Bytes{'X'});}catch(const std::exception&){failed=true;}check(failed && sequence(store.status())==unchanged);};
   rejects(std::string("/bad\0name",9));rejects("/"+std::string(256,'x'));
   std::string long_path="/";for(unsigned i=0;i<16;++i){if(i)long_path+='/';long_path+=std::string(255,'p');}long_path+="/x";rejects(long_path);
   bool strict=false;try{fs.set_volume_name("bad\nlabel");}catch(const std::exception&){strict=true;}check(strict && sequence(store.status())==unchanged);
   strict=false;try{fs.set_xattr("/dir/a","user.bad\tname",Bytes{'x'},Namespace::XattrPolicy::Upsert);}catch(const std::exception&){strict=true;}check(strict && sequence(store.status())==unchanged);
   Namespace controls_view(store,"controls",true);for(size_t i=0;i<control_names.size();++i)check(controls_view.read("/"+control_names[i])==Bytes{static_cast<unsigned char>(i)});controls_view.verify();
   // A long syntactic alias must not become an inode hint used as the base
   // for RPC child paths: path_for_inode should return the compact physical path.
   std::string dotted="/";for(unsigned i=0;i<2046;++i)dotted+="./";dotted+="dir";check(dotted.size()==4096);
   auto dirid=fs.lstat(dotted).id;check(fs.path_for_inode(dirid)=="/dir");
   fs.write(fs.path_for_inode(dirid)+"/rpc-child",Bytes{'C'});check(fs.read("/dir/rpc-child")==Bytes{'C'});
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
    store.collect_garbage();auto current_id=fs.stat("/moved/a").id;
    check(fs.path_for_inode(current_id)=="/moved/a" && fs.read("/moved/./a")==Bytes{'R'});
    check(old.path_for_inode(id)=="/dir/a" && old.read("/dir/../dir/./a")==Bytes{'A'});}
   fs.verify();old.verify();store.verify();}
  {Container store(path);Namespace fs(store),old(store,"before",true),controls(store,"controls",true);missing([&]{fs.path_for_inode(id);});check(old.path_for_inode(id)=="/dir/a");
   for(size_t i=0;i<control_names.size();++i)check(controls.read("/"+control_names[i])==Bytes{static_cast<unsigned char>(i)});
   check(controls.read("/directory\n\t/child\rfile")==Bytes{'D'} && controls.read("/follow\tlink")==Bytes{8});
   if(!legacy){check(fs.read("/moved/./a")==Bytes{'R'} && fs.stat("/moved/a").id!=id);
    check(old.path_for_inode(id)=="/dir/a" && old.read("/dir/../dir/./a")==Bytes{'A'});}
   fs.verify();old.verify();store.verify();}
 }
 for(const auto& bad_name:std::vector<std::string>{".","..","/a","a//"}){
  auto path=std::string(tmp)+"/bad-name-"+std::to_string(bad_name.size())+"-"+std::to_string(static_cast<unsigned char>(bad_name[0]));
  std::string parent;{Container::create(path);Container store(path);Namespace fs(store);fs.initialize();fs.mkdir("/parent");fs.write("/parent/file",Bytes{'X'});
   parent=fs.stat("/parent").id;fs.snapshot("clean");}
  corrupt_entry_name(path,parent,"file",bad_name);
  {Container store(path);Namespace fs(store);fs.snapshot("corrupt");auto rejects=[](auto action){bool failed=false;try{action();}catch(const std::exception& e){failed=std::string(e.what())=="invalid directory name";}check(failed);};
   rejects([&]{fs.verify();});rejects([&]{fs.diff("clean","corrupt");});}
 }
 std::filesystem::remove_all(tmp);std::cout<<"validated inode hints, rename/link/symlink retarget/reuse, views, eviction, orphan, GC and reopen passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}}
