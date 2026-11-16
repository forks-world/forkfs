#include "namespace.h"
#include "metadata_view.h"
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using forkfs::NamespaceView;using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("clone_file check failed at "+std::to_string(line));}
template<class F> static int error(F f){try{f();}catch(const std::system_error& e){return e.code().value();}catch(const std::exception&){return 1;}return 0;}
static std::string state(const std::string& status){std::string out;for(const auto* key:{"last_committed_seq","object_roots","indexed_objects"}){auto p=status.find("\""+std::string(key)+"\":");check(p!=status.npos);p+=strlen(key)+3;auto e=status.find_first_not_of("0123456789",p);out+=status.substr(p,e-p)+"/";}return out;}
static std::string prefix(const std::string& kind,const std::string& id){return std::string("\0forkfs/",8)+kind+"/"+id+"/";}
static std::map<std::string,NamespaceView::ObjectId> refs(const NamespaceView& view,const std::string& p){
 std::map<std::string,NamespaceView::ObjectId> out;for(const auto& key:view.keys(p)){auto id=view.reference(key);check(id.has_value());out.emplace(key.substr(p.size()),*id);}return out;
}
int main(){char pattern[]="/tmp/ff-clone-file-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
 try{for(bool legacy:{false,true}){auto path=std::string(tmp)+(legacy?"/legacy":"/modern");if(legacy)Container::create_legacy(path);else Container::create(path);
  std::string source_id,clone_id,sparse_id,sparse_clone_id,empty_id,empty_clone_id,inline_id,inline_clone_id;
  {Container store(path);Namespace fs(store);fs.initialize();fs.mkdir("/d");
   fs.write("/d/empty",{});fs.clone_file("/d/empty","/empty-copy");empty_id=fs.lstat("/d/empty").id;empty_clone_id=fs.lstat("/empty-copy").id;
   fs.write("/inline",Bytes{'b',0,'i','n'});fs.clone_file("/inline","/inline-copy");inline_id=fs.lstat("/inline").id;inline_clone_id=fs.lstat("/inline-copy").id;
   Bytes data(300000);for(size_t i=0;i<data.size();++i)data[i]=static_cast<unsigned char>((i*31)%251+1);
   fs.write("/d/source",Bytes(data.begin(),data.begin()+Namespace::maximum_io_size));
   fs.write_at("/d/source",Namespace::maximum_io_size,Bytes(data.begin()+Namespace::maximum_io_size,data.end()));
   fs.link("/d/source","/hard-source");
   Namespace::Attributes attrs;attrs.mode=0601;attrs.accessed_ns=111;attrs.modified_ns=222;attrs.birth_ns=333;fs.set_attributes("/d/source",attrs);
   fs.set_xattr("/d/source","user.clone",Bytes{'x',0,'y'});attrs={};attrs.flags=forkfs::file_flag_immutable|forkfs::file_flag_append;fs.set_attributes("/d/source",attrs);
   auto source_before=fs.lstat("/d/source");fs.symlink("/d/source","/source-link");fs.clone_file("/source-link","/clone");auto clone=fs.lstat("/clone");
   source_id=source_before.id;clone_id=clone.id;
   check(source_id!=clone_id&&source_before.links==2&&clone.links==1&&clone.mode==source_before.mode&&clone.size==source_before.size);
   check(clone.modified_ns==222&&clone.accessed_ns==111&&clone.birth_ns==333&&clone.flags==source_before.flags);
   Bytes clone_read=fs.read_at("/clone",0,Namespace::maximum_io_size);auto tail=fs.read_at("/clone",Namespace::maximum_io_size,data.size()-Namespace::maximum_io_size);clone_read.insert(clone_read.end(),tail.begin(),tail.end());
   check(fs.get_xattr("/clone","user.clone")==Bytes({'x',0,'y'})&&clone_read==data);
   check(fs.lstat("/d/source").id==source_before.id&&fs.lstat("/d/source").changed_ns==source_before.changed_ns);
   auto before=state(store.status());check(error([&]{fs.clone_file("/d/source","/d/source");})==EEXIST&&state(store.status())==before);
   before=state(store.status());check(error([&]{fs.clone_file("/d/source","/source-link");})==EEXIST&&state(store.status())==before);
   fs.mkdir("/occupied");before=state(store.status());check(error([&]{fs.clone_file("/d/source","/occupied");})==EEXIST&&state(store.status())==before);
   fs.symlink("absent","/dangling");before=state(store.status());check(error([&]{fs.clone_file("/d/source","/dangling");})==EEXIST&&state(store.status())==before);
   before=state(store.status());check(error([&]{fs.clone_file("/missing","/missing-target");})!=0&&state(store.status())==before);
   fs.mkdir("/dir");before=state(store.status());check(error([&]{fs.clone_file("/dir","/dir-copy");})==EISDIR&&state(store.status())==before);
   before=state(store.status());check(error([&]{fs.clone_file("/d/source","/no-parent/target");})!=0&&state(store.status())==before);
   fs.mkdir("/append-dir");attrs={};attrs.flags=forkfs::file_flag_append;fs.set_attributes("/append-dir",attrs);fs.clone_file("/inline","/append-dir/copy");check(fs.read("/append-dir/copy")==Bytes({'b',0,'i','n'}));attrs.flags=0;fs.set_attributes("/append-dir",attrs);
   fs.mkdir("/immutable-dir");attrs={};attrs.flags=forkfs::file_flag_immutable;fs.set_attributes("/immutable-dir",attrs);before=state(store.status());check(error([&]{fs.clone_file("/inline","/immutable-dir/copy");})==EPERM&&state(store.status())==before);attrs.flags=0;fs.set_attributes("/immutable-dir",attrs);
   fs.clone_file("/hard-source","/hard-copy");check(fs.lstat("/hard-copy").id!=fs.lstat("/hard-source").id&&fs.stat("/hard-source").links==2);
   fs.write("/sparse",{});fs.truncate("/sparse",Namespace::maximum_file_size);fs.write_at("/sparse",0,Bytes{'l'});fs.write_at("/sparse",Namespace::maximum_file_size-1,Bytes{'h'});
   fs.clone_file("/sparse","/sparse-copy");sparse_id=fs.lstat("/sparse").id;sparse_clone_id=fs.lstat("/sparse-copy").id;
   check(fs.stat("/sparse-copy").size==Namespace::maximum_file_size&&fs.stat("/sparse-copy").allocated_size==fs.stat("/sparse").allocated_size);
   fs.snapshot("base");fs.fork("base","work");Namespace work(store,"work"),old(store,"base",true);before=state(store.status());
   check(error([&]{old.clone_file("/inline","/revision-copy");})!=0&&state(store.status())==before);
   attrs={};attrs.flags=0;fs.set_attributes("/clone",attrs);fs.write_at("/clone",0,Bytes{'C'});fs.set_xattr("/clone","user.clone",Bytes{'z'});
   check(fs.stat("/d/source").flags==source_before.flags&&fs.stat("/clone").flags==0);
   check(fs.read_at("/d/source",0,1)==Bytes(data.begin(),data.begin()+1)&&fs.get_xattr("/d/source","user.clone")==Bytes({'x',0,'y'}));
   attrs={};attrs.flags=0;fs.set_attributes("/sparse-copy",attrs);fs.write_at("/sparse-copy",Namespace::maximum_file_size/2,Bytes{'m'});
   fs.punch_hole("/sparse-copy",Namespace::maximum_file_size/2,1);check(fs.read_at("/sparse-copy",Namespace::maximum_file_size/2,1)==Bytes{0});
   fs.truncate("/sparse-copy",Namespace::maximum_file_size-1);
   check(fs.read_at("/sparse",Namespace::maximum_file_size-1,1)==Bytes{'h'}&&fs.stat("/sparse").size==Namespace::maximum_file_size);
   fs.clone_file("/d/empty","/empty-copy2");check(fs.read("/empty-copy2").empty());
   if(!legacy){fs.write("/handle-source",Bytes{'h','a','n','d','l','e'});auto handle=fs.open("/handle-source",Namespace::Access::Read),duplicate=fs.dup(handle);
    fs.clone_file("/handle-source","/handle-copy");fs.remove("/handle-source",false);fs.write("/handle-source",Bytes{'n','e','w'});
    check(fs.handle_read(handle,0,6)==Bytes({'h','a','n','d','l','e'})&&fs.handle_read(duplicate,0,6)==Bytes({'h','a','n','d','l','e'}));
    check(fs.read("/handle-copy")==Bytes({'h','a','n','d','l','e'}));fs.close(duplicate);fs.close(handle);store.collect_garbage();
    check(fs.read("/handle-copy")==Bytes({'h','a','n','d','l','e'}));}
   auto opened=legacy?std::string{}:fs.open("/clone",Namespace::Access::Read);
   fs.remove("/clone",false);if(!legacy){check(fs.handle_read(opened,0,1)==Bytes{'C'});fs.close(opened);store.collect_garbage();}
   check(work.read_at("/clone",0,1)==Bytes(data.begin(),data.begin()+1));fs.verify();work.verify();store.verify();
   if(!legacy)store.collect_garbage();
  }
  if(!legacy){forkfs::Journal journal(path);NamespaceView view(journal,"main");
   // Main removed /clone; its snapshot and fork still retain the original key pair.
   NamespaceView base(journal,"base",true);
   auto source_chunks=refs(base,prefix("chunk",source_id)),clone_chunks=refs(base,prefix("chunk",clone_id));check(source_chunks==clone_chunks&&source_chunks.size()==5);
   auto sparse=refs(base,prefix("chunk",sparse_id)),sparse_clone=refs(base,prefix("chunk",sparse_clone_id));check(sparse==sparse_clone&&sparse.size()==2);
   auto source_xattrs=refs(base,prefix("xattr",source_id)),clone_xattrs=refs(base,prefix("xattr",clone_id));check(source_xattrs==clone_xattrs&&source_xattrs.size()==1);
   auto a=base.reference(std::string("\0forkfs/content/",16)+inline_id),b=base.reference(std::string("\0forkfs/content/",16)+inline_clone_id);check(a&&b&&*a==*b);
   auto e=base.reference(std::string("\0forkfs/content/",16)+empty_id),ec=base.reference(std::string("\0forkfs/content/",16)+empty_clone_id);check(e&&ec&&*e==*ec);
  }
  {Container store(path);Namespace fs(store),base(store,"base",true),work(store,"work");
   check(base.read_at("/clone",0,1)==Bytes{1}&&work.read_at("/clone",0,1)==Bytes{1});
   check(base.stat("/sparse-copy").size==Namespace::maximum_file_size&&base.read_at("/sparse-copy",0,1)==Bytes{'l'});
   fs.verify();base.verify();work.verify();store.verify();
  }
 }
 std::filesystem::remove_all(tmp);std::cout<<"daemon CoW file clone, sparse references, independent metadata/data, snapshots, legacy and GC passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}}
