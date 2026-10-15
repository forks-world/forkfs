#include "revision.h"
#include "namespace.h"
#include "metadata_view.h"
#include "level_store.h"
#include <cstring>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace forkfs;using Bytes=std::vector<unsigned char>;using Id=NamespaceView::ObjectId;
#define CHECK(x) do{if(!(x))throw std::runtime_error("line "+std::to_string(__LINE__)+": " #x);}while(0)
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}CHECK(failed);}
const std::string history("\0forkfs-history/",16),prefix("\0forkfs/",8);
std::string hex(const Id& id){std::string out;for(auto c:id){out+="0123456789abcdef"[c>>4];out+="0123456789abcdef"[c&15];}return out;}
void put(Bytes& b,size_t at,uint64_t v){for(unsigned i=0;i<8;++i)b.at(at+i)=v>>(8*i);}
void object(std::vector<Mutation>& batch,const Bytes& value){batch.push_back({history+"object/"+hex(Journal::object_id(value)),value});}
LevelStore::RecoveryPoint backup(const std::string& source,const std::string& pack){LevelStore store(source);std::string hash;auto seq=store.backup(pack,&hash);Id id{};std::copy(hash.begin(),hash.end(),id.begin());return {seq,store.repository(),hex(id)};}
int main(){auto path=(std::filesystem::temp_directory_path()/"ff-recovery-ns-XXXXXX").string();CHECK(mkdtemp(path.data()));
    try{
        auto source=path+"/source";Container::create(source);std::string file,dir,root;
        {Container c(source);Namespace fs(c);fs.initialize();fs.mkdir("/dir");fs.write("/file",{1,2,3});fs.link("/file","/alias");fs.symlink("/file","/sym");
            file=fs.stat("/file").id;dir=fs.stat("/dir").id;root=fs.root_inode().id;fs.snapshot("saved");fs.fork("saved","other");fs.verify();}
        Namespace::verify_recovery(source);
        // All records remain hash-valid, including tree pages and descriptors.
        for(unsigned view=0;view<4;++view)for(unsigned kind=0;kind<8;++kind){
            auto db=path+"/bad-"+std::to_string(view)+"-"+std::to_string(kind);std::filesystem::copy(source,db,std::filesystem::copy_options::recursive);
            {Journal j(db);NamespaceView v(j,view==1?"other":view>=2?"saved":"main",view>=2);
                NamespaceView::Changes delta;std::vector<Mutation> batch;
                auto set=[&](const std::string& key,Bytes bytes){delta[key]=Journal::object_id(bytes);object(batch,bytes);};
                if(kind<2){auto b=v.get(prefix+"inode/"+file);put(b,kind==0?24:16,99);set(prefix+"inode/"+file,b);}
                if(kind==2)set(prefix+"entry/"+root+"/missing",Bytes(32,'f'));
                if(kind==3)set(prefix+"parent/"+dir,Bytes{'b','a','d'});
                if(kind==4)delta[prefix+"allocated/"+file]=std::nullopt;
                if(kind==5)set(prefix+"entry/"+dir+"/cycle",Bytes(root.begin(),root.end()));
                if(kind==6)set(prefix+"content/"+file,Bytes{});
                if(kind==7){delta[prefix+"entry/"+root+"/file"]=std::nullopt;set(prefix+"entry/"+root+"/file/",Bytes(file.begin(),file.end()));}
                auto plan=v.plan(delta);for(const auto& b:plan.objects)object(batch,b);object(batch,plan.manifest);
                if(view<2)batch.push_back({history+"world/"+(view==1?"other":"main"),plan.manifest});
                else {auto descriptor=j.get(history+"revision/saved");auto manifest=Journal::object_id(plan.manifest);std::copy(manifest.begin(),manifest.end(),descriptor.begin()+8);
                    if(view==2)batch.push_back({history+"revision/saved",descriptor});
                    else {object(batch,descriptor);auto parent=Journal::object_id(descriptor);auto child=j.get(history+"revision/saved");
                        auto record=RevisionRecord::decode(child);record.parents={parent};record.source_sequence=j.sequence();child=record.encode();batch.push_back({history+"revision/child",child});}
                }
                j.transact(j.sequence(),batch);j.verify();
            }
            rejects([&]{Namespace::verify_recovery(db);});
            auto pack=db+".pack",dest=db+".restored";auto point=backup(db,pack); // structural graph checks accept this fixture
            rejects([&]{LevelStore::restore({pack},dest,point,Namespace::verify_recovery);});CHECK(!std::filesystem::exists(dest));
            for(const auto& entry:std::filesystem::directory_iterator(path))CHECK(!entry.path().string().starts_with(dest+".restore-job-"));
        }
        // Unlinked open files survive the checkpoint, without recreating handles.
        auto orphan=path+"/orphan";std::filesystem::copy(source,orphan,std::filesystem::copy_options::recursive);
        {Container c(orphan);Namespace fs(c);auto handle=fs.open("/file",Namespace::Access::ReadWrite);fs.remove("/file",false);fs.remove("/alias",false);}
        uint64_t before;{Journal j(orphan,true);before=j.sequence();CHECK(j.contains(history+"orphan/main/"+file+"/inode"));}
        Namespace::verify_recovery(orphan);
        auto pack=path+"/orphan.pack",dest=path+"/orphan-restored";auto point=backup(orphan,pack);
        CHECK(LevelStore::restore({pack},dest,point,Namespace::verify_recovery)==before);
        {Journal j(dest,true);CHECK(j.sequence()==before);CHECK(j.contains(history+"orphan/main/"+file+"/inode"));}
        for(unsigned kind=0;kind<4;++kind){auto db=path+"/orphan-bad-"+std::to_string(kind);std::filesystem::copy(orphan,db,std::filesystem::copy_options::recursive);
            {Journal j(db);auto key=history+"orphan/main/"+file+"/";
                if(kind==0)j.transact(j.sequence(),{{key+"content",std::nullopt}});
                if(kind==1)j.transact(j.sequence(),{{key+"content",Bytes{}}});
                if(kind==2)j.transact(j.sequence(),{{key+"unknown",Bytes{}}});
                if(kind==3)j.transact(j.sequence(),{{history+"orphan/missing/"+file+"/inode",j.get(key+"inode")}});
            }
            rejects([&]{Namespace::verify_recovery(db);});auto p=backup(db,db+".pack");
            rejects([&]{LevelStore::restore({db+".pack"},db+".restored",p,Namespace::verify_recovery);});CHECK(!std::filesystem::exists(db+".restored"));
        }
        // Empty repositories are valid and checks never initialize them.
        auto empty=path+"/empty";Container::create(empty);Namespace::verify_recovery(empty);
        {Journal j(empty,true);CHECK(j.sequence()==0);}
        std::filesystem::remove_all(path);std::cout<<"all Worlds/revisions/ancestry and orphan recovery validation passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(path);return 1;}
}
