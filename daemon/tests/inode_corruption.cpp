#include "namespace.h"
#include "metadata_view.h"
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
using namespace forkfs;using Bytes=std::vector<unsigned char>;
void check(bool ok){if(!ok)throw std::runtime_error("inode corruption regression failed");}
template<class F>void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("malformed inode accepted");}
void put(Bytes& b,size_t at,uint64_t v,size_t width=4){for(size_t i=0;i<width;++i)b.at(at+i)=v>>(8*i);}
Bytes record(){Bytes b(80);memcpy(b.data(),"FFINOD03",8);put(b,8,1);put(b,12,0644);put(b,16,1,8);put(b,52,501);put(b,56,20);put(b,64,111,8);put(b,72,222,8);return b;}
Bytes xattrs(){auto b=record();memcpy(b.data(),"FFINOD04",8);b.resize(84);return b;}
void entry(Bytes& b,const std::string& name,const Bytes& value){auto at=b.size();b.resize(at+8);put(b,at,name.size());put(b,at+4,value.size());b.insert(b.end(),name.begin(),name.end());b.insert(b.end(),value.begin(),value.end());}
std::string object_name(const std::array<unsigned char,32>& id){std::string s("\0forkfs-history/object/",23);const char* hex="0123456789abcdef";for(auto c:id){s+=hex[c>>4];s+=hex[c&15];}return s;}
void publish(Journal& journal,const std::string& id,const Bytes& b){NamespaceView view(journal,"main");auto key=std::string("\0forkfs/inode/",14)+id;
    auto plan=view.plan({{key,Journal::object_id(b)}});std::map<std::string,Bytes> objects{{object_name(Journal::object_id(b)),b}};
    for(const auto& payload:plan.objects)objects[object_name(Journal::object_id(payload))]=payload;
    std::vector<Mutation> batch;for(auto& [name,value]:objects)batch.push_back({name,value});batch.push_back({std::string("\0forkfs-history/world/main",26),plan.manifest});journal.transact(journal.sequence(),batch);journal.verify();
}
int main(){auto pattern=(std::filesystem::temp_directory_path()/"ff-inode-corrupt-XXXXXX").string();auto tmp=mkdtemp(pattern.data());if(!tmp)return 1;
    try{
        std::vector<Bytes> bad;auto add=[&](Bytes b){bad.push_back(std::move(b));};
        add(Bytes{'F','F','I','N','O','D','0'});auto b=record();b.resize(79);add(b);b=record();b.push_back(0);add(b);
        for(auto at:{48u,60u}){b=record();put(b,at,2);add(b);}b=record();put(b,48,1);add(b);
        for(auto at:{64u,72u}){b=record();put(b,at,UINT64_MAX,8);add(b);}b=record();put(b,8,4);add(b);b=record();put(b,12,01000);add(b);
        b=xattrs();b.resize(83);add(b);b=xattrs();b.resize(128*1024+1);add(b);b=xattrs();put(b,80,129);add(b);
        b=xattrs();put(b,80,1);add(b);b=xattrs();b.push_back(0);add(b);
        for(auto length:{0u,256u,UINT32_MAX}){b=xattrs();put(b,80,1);b.resize(92);put(b,84,length);add(b);}
        b=xattrs();put(b,80,1);b.resize(92);put(b,84,1);put(b,88,65537);add(b);
        b=xattrs();put(b,80,1);entry(b,std::string("a\0b",3),{});add(b);
        for(bool duplicate:{false,true}){b=xattrs();put(b,80,2);entry(b,"b",{});entry(b,duplicate?"b":"a",{});add(b);}
        // Independently authored valid records prove the fixture publisher works.
        auto valid=xattrs();put(valid,80,2);entry(valid,"a",{});entry(valid,"b",{0,255});
        for(bool legacy:{false,true})for(size_t i=0;i<=bad.size();++i){auto path=std::string(tmp)+"/"+(legacy?"legacy":"leveldb")+std::to_string(i);if(legacy)Container::create_legacy(path);else Container::create(path);std::string id;
            {Container store(path);Namespace fs(store);fs.initialize();auto root=fs.root_inode().id;id=fs.create_child(root,"victim",false,0644).id;fs.create_child(root,"healthy",false,0644);fs.snapshot("base");}
            const auto& payload=i==bad.size()?valid:bad[i];
            if(!legacy){Journal journal(path);publish(journal,id,payload);}else{int fd=open(path.c_str(),O_RDWR|O_CLOEXEC);check(fd>=0);try{std::array<unsigned char,16> repository{};check(pread(fd,repository.data(),16,16)==16);Journal journal(fd,repository);publish(journal,id,payload);}catch(...){close(fd);throw;}close(fd);}
            for(unsigned reopen=0;reopen<2;++reopen){Container store(path);Namespace fs(store),revision(store,"base",true);auto before=store.status();
                if(i==bad.size()){auto n=fs.stat_inode(id);check(n.uid==501 && n.created_ns==222);check(fs.list_inode_xattrs(id)==std::vector<std::string>({"a","b"}));check(fs.get_inode_xattr(id,"b")==Bytes({0,255}));fs.verify();}
                else{rejects([&]{fs.stat_inode(id);});rejects([&]{fs.get_inode_xattr(id,"a");});rejects([&]{fs.verify();});}
                check(fs.stat("/healthy").size==0 && revision.stat_inode(id).size==0 && revision.list_inode_xattrs(id).empty());revision.verify();check(store.status()==before);
            }
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
    std::filesystem::remove_all(tmp);return 0;
}
