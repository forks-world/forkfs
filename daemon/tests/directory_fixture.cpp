// Manual read/enumeration fixture; bulk metadata batches do not qualify create performance.
#include "namespace.h"
#include "metadata_view.h"
#include <cstring>
#include <iostream>
#include <cstdio>
using Bytes=std::vector<unsigned char>;
static const std::string prefix("\0forkfs/",8),history("\0forkfs-history/",16);
static std::string hex(const std::array<unsigned char,32>& id){const char* digits="0123456789abcdef";std::string out;for(auto c:id){out+=digits[c>>4];out+=digits[c&15];}return out;}
int main(int argc,char** argv){
 if(argc!=3)return 2;
 try{std::string count_arg=argv[2];size_t used=0;auto count=std::stoul(count_arg,&used);
  if(used!=count_arg.size() || count<1 || count>100000)throw std::runtime_error("count must be 1..100000");
  forkfs::Container::create(argv[1]);std::string directory;
  {forkfs::Container store(argv[1]);forkfs::Namespace fs(store);fs.initialize();fs.mkdir("/scale");directory=fs.stat("/scale").id;}
  {forkfs::Journal journal(argv[1]);
   // Existing unflagged empty-file inode layout: type=1, mode=0644, links=1,
   // size/timestamps=0. All fixture files intentionally share this value object.
   Bytes inode(64);memcpy(inode.data(),"FFINOD02",8);inode[8]=1;inode[12]=0644&255;inode[13]=0644>>8;inode[16]=1;
   for(unsigned begin=0;begin<count;begin+=256){forkfs::NamespaceView view(journal,"main");forkfs::NamespaceView::Changes delta;std::map<std::string,Bytes> objects;
    auto add=[&](const Bytes& bytes){auto id=forkfs::Journal::object_id(bytes);objects.emplace(history+"object/"+hex(id),bytes);return id;};
    auto inode_value=add(inode),empty=add({});auto end=std::min<unsigned long>(begin+256,count);
    for(unsigned i=begin;i<end;++i){char text[64];snprintf(text,sizeof(text),"%032x",i+1);std::string id=text;
     snprintf(text,sizeof(text),"n%06u",i);std::string name=text;
     delta[prefix+"entry/"+directory+"/"+name]=add(Bytes(id.begin(),id.end()));
     delta[prefix+"inode/"+id]=inode_value;delta[prefix+"allocated/"+id]=empty;delta[prefix+"content/"+id]=empty;
     objects.emplace(history+"allocated/"+id,Bytes{});
    }
    auto plan=view.plan(delta);for(const auto& bytes:plan.objects)add(bytes);
    std::vector<forkfs::Mutation> batch;for(auto& [key,bytes]:objects)if(!journal.contains(key))batch.push_back({key,std::move(bytes)});
    batch.push_back({history+"world/main",std::move(plan.manifest)});journal.transact(journal.sequence(),batch);
    if(end%4096==0 || end==count)std::cerr<<"prepared "<<end<<" entries\n";
   }
  }
  {forkfs::Container store(argv[1]);forkfs::Namespace fs(store);fs.snapshot("directory_base");fs.verify();store.verify();}
  std::cout<<"{\"entries\":"<<count<<",\"bulk_fixture\":true}\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
