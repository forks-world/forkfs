#include "namespace.h"
#include "metadata_view.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <fcntl.h>
#include <sys/file.h>
#include <array>
#include <sys/wait.h>
#include <vector>
#include <unistd.h>

using namespace forkfs;
using Bytes=std::vector<unsigned char>;
static const std::string ns_prefix("\0forkfs/",8),history("\0forkfs-history/",16);
struct ScopedFd{int value=-1;~ScopedFd(){if(value>=0)close(value);}};
static void check(bool ok){if(!ok)throw std::runtime_error("large clone regression check failed");}
static std::string hex(const NamespaceView::ObjectId& id){const char* digits="0123456789abcdef";std::string out;for(auto c:id){out+=digits[c>>4];out+=digits[c&15];}return out;}
static std::string chunk_suffix(uint64_t index){auto digits=std::to_string(index);return index<1024?std::string(4-digits.size(),'0')+digits:"x"+std::string(8-digits.size(),'0')+digits;}
static std::array<unsigned char,16> repository_id(int fd){std::array<unsigned char,16> id{};if(pread(fd,id.data(),id.size(),16)!=static_cast<ssize_t>(id.size()))throw std::runtime_error("read repository identity");return id;}
static std::map<std::string,NamespaceView::ObjectId> chunks(const NamespaceView& view,const std::string& id){
 std::string pre=ns_prefix+"chunk/"+id+"/";std::map<std::string,NamespaceView::ObjectId> out;
 for(const auto& key:view.keys(pre)){auto ref=view.reference(key);check(ref.has_value());out.emplace(key.substr(pre.size()),*ref);}return out;
}
static void prepare(const std::string& path,bool legacy,uint64_t count);
static void staged_crash_retry(const std::string& path,uint64_t count){
 prepare(path,false,count);pid_t child=fork();check(child>=0);
 if(child==0){setenv("FORKFS_TEST_CRASH","leveldb_after_write",1);try{Container store(path);Namespace fs(store);fs.clone_file("/large","/copy");}catch(...){_exit(1);}_exit(2);}
 int status=0;check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86);
 {Container store(path);Namespace fs(store);check(fs.stat("/large").size==(count-1)*file_chunk_bytes+1);
  bool absent=false;try{(void)fs.stat("/copy");}catch(...){absent=true;}check(absent);store.collect_garbage();
  fs.clone_file("/large","/copy");check(fs.stat("/copy").size==fs.stat("/large").size);fs.verify();store.verify();}
}
static void prepare(const std::string& path,bool legacy,uint64_t count){
 if(legacy)Container::create_legacy(path);else Container::create(path);
 std::string inode;
 {Container store(path);Namespace fs(store);fs.initialize();fs.write("/large",{});fs.truncate("/large",(count-1)*file_chunk_bytes+1);fs.write_at("/large",0,Bytes{0x5a});inode=fs.stat("/large").id;}
 ScopedFd owned;std::unique_ptr<Journal> journal;
 if(legacy){owned.value=open(path.c_str(),O_RDWR|O_CLOEXEC);check(owned.value>=0);if(flock(owned.value,LOCK_EX|LOCK_NB)<0)throw std::runtime_error("lock legacy fixture");journal=std::make_unique<Journal>(owned.value,repository_id(owned.value));}
 else journal=std::make_unique<Journal>(path);
 Bytes payload{0x5a};auto payload_id=Journal::object_id(payload);auto payload_root=history+"object/"+hex(payload_id);
 for(uint64_t begin=0;begin<count;begin+=256){
  NamespaceView view(*journal,"main");NamespaceView::Changes delta;
  auto end=std::min<uint64_t>(count,begin+256);
  for(auto index=begin;index<end;++index)delta[ns_prefix+"chunk/"+inode+"/"+chunk_suffix(index)]=payload_id;
  auto plan=view.plan(delta);std::vector<Mutation> batch;
  if(!journal->contains(payload_root))batch.push_back({payload_root,payload});
  for(auto& object:plan.objects){auto key=history+"object/"+hex(Journal::object_id(object));if(!journal->contains(key))batch.push_back({key,std::move(object)});}
  batch.push_back({history+"world/main",std::move(plan.manifest)});journal->transact(journal->sequence(),batch);
 }
 journal.reset();
}
int main(int argc,char** argv){char pattern[]="/tmp/ff-clone-scale-XXXXXX";char* temp=mkdtemp(pattern);if(!temp)return 1;
 try{
  constexpr uint64_t count=4097;
  std::vector<bool> backends{false,true};
  if(argc==3&&std::string(argv[1])=="--backend"){auto name=std::string(argv[2]);if(name=="modern")backends={false};else if(name=="legacy")backends={true};else throw std::runtime_error("backend must be modern or legacy");}
  else if(argc!=1)throw std::runtime_error("usage: clone_file_scale_test [--backend modern|legacy]");
  for(bool legacy:backends){
   auto path=std::string(temp)+(legacy?"/legacy":"/modern");prepare(path,legacy,count);std::string source_id,clone_id;
   {
    Container store(path);Namespace fs(store);source_id=fs.stat("/large").id;
    fs.clone_file("/large","/copy");clone_id=fs.stat("/copy").id;fs.snapshot("large_base");
    auto src=fs.stat("/large"),dst=fs.stat("/copy");check(src.size==dst.size&&src.allocated_size==dst.allocated_size);
    check(dst.allocated_size==count*512);check(fs.read_at("/copy",0,1)==Bytes{0x5a});
    check(fs.read_at("/copy",2048*file_chunk_bytes,2)==Bytes({0x5a,0}));
    check(fs.read_at("/copy",(count-1)*file_chunk_bytes,1)==Bytes{0x5a});
    fs.write_at("/copy",128*file_chunk_bytes,Bytes{0x31});
    check(fs.read_at("/large",128*file_chunk_bytes,1)==Bytes{0x5a});
    fs.verify();store.verify();if(!legacy)store.collect_garbage();
   }
   {
    ScopedFd owned;std::unique_ptr<Journal> journal;
    if(legacy){owned.value=open(path.c_str(),O_RDWR|O_CLOEXEC);check(owned.value>=0);if(flock(owned.value,LOCK_EX|LOCK_NB)<0)throw std::runtime_error("lock legacy check");journal=std::make_unique<Journal>(owned.value,repository_id(owned.value));}
    else journal=std::make_unique<Journal>(path);
    NamespaceView base(*journal,"large_base",true);
    check(chunks(base,source_id)==chunks(base,clone_id));check(chunks(base,source_id).size()==count);
    journal.reset();
   }
   {Container store(path);Namespace fs(store),base(store,"large_base",true);check(base.stat("/large").size==count*file_chunk_bytes- file_chunk_bytes+1);
    check(base.read_at("/large",0,1)==Bytes{0x5a});fs.verify();base.verify();store.verify();}
  }
  if(argc==1||std::string(argv[2])=="modern")staged_crash_retry(std::string(temp)+"/crash",count);
  std::filesystem::remove_all(temp);std::cout<<"large chunk map clone transaction limits and sharing passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(temp);return 1;}
}
