#include "level_store.h"
#include "namespace.h"
#include <leveldb/db.h>
#include <filesystem>
#include <future>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace forkfs;
#define CHECK(x) do{if(!(x))throw std::runtime_error("line "+std::to_string(__LINE__)+": " #x);}while(0)
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}CHECK(rejected);}
static void corrupt(const std::string& path,const std::string& key,const std::string& value){
    leveldb::DB* raw=nullptr;leveldb::Options options;CHECK(leveldb::DB::Open(options,path,&raw).ok());std::unique_ptr<leveldb::DB> db(raw);
    leveldb::WriteOptions write;write.sync=true;CHECK(db->Put(write,key,value).ok());
}
int main(){
    auto pattern=(std::filesystem::temp_directory_path()/"ff-replay-core-XXXXXX").string();CHECK(mkdtemp(pattern.data()));
    try{
        auto source=pattern+"/source",archive=pattern+"/base",target=pattern+"/target";
        LevelStore::create(source);
        {
            LevelStore store(source);store.transact(0,{{"binary",std::vector<unsigned char>{0,255}}});
            auto base=store.backup(archive);CHECK(base==1);auto original=store.root("binary");
            // Publish references and deletion, preserving the content object.
            store.transact(1,{{"alias",std::nullopt,original},{"binary",std::nullopt}});
            CHECK(store.backup(pattern+"/second-base")==2);
            store.transact(2,{{"alias",std::vector<unsigned char>{42}}});
            CHECK(store.export_replay(base,pattern+"/delta")==3);store.verify();
        }
        CHECK(LevelStore::replay_archive(archive,target)==1);
        CHECK(LevelStore::replay_archive(pattern+"/delta",target)==3);
        {
            LevelStore store(target);CHECK(!store.contains("binary"));CHECK(store.object(store.root("alias"))==std::vector<unsigned char>{42});CHECK(store.objects()==2);CHECK(store.roots()==1);store.verify();
        }
        // Inode identity, ownership/times and binary xattrs survive both stages.
        auto ns_source=pattern+"/ns",ns_target=pattern+"/ns-target";
        Container::create(ns_source);std::string inode;
        {Container store(ns_source);Namespace fs(store);fs.initialize();fs.write("/file",{0,1,255});fs.link("/file","/alias");
            inode=fs.stat("/file").id;Namespace::Attributes attrs;attrs.uid=123;attrs.gid=456;attrs.mode=0600;attrs.accessed_ns=17;attrs.created_ns=19;attrs.modified_ns=23;
            fs.set_inode_attributes(inode,attrs);fs.set_inode_xattr(inode,"user.binary",{0,255,0});fs.snapshot("before");}
        uint64_t ns_base;
        {LevelStore store(ns_source);ns_base=store.backup(pattern+"/ns-base");}
        {Container store(ns_source);Namespace fs(store);fs.set_inode_xattr(inode,"user.binary",{42});fs.snapshot("after");}
        {LevelStore store(ns_source);store.export_replay(ns_base,pattern+"/ns-delta");}
        LevelStore::replay_archive(pattern+"/ns-base",ns_target);LevelStore::replay_archive(pattern+"/ns-delta",ns_target);
        {Container store(ns_target);Namespace fs(store),before(store,"before",true);auto info=fs.stat("/alias");
            CHECK(info.id==inode && info.links==2 && info.uid==123 && info.gid==456 && info.mode==0600);
            CHECK(info.accessed_ns==17 && info.created_ns==19 && info.modified_ns==23);
            CHECK(fs.get_inode_xattr(inode,"user.binary")==std::vector<unsigned char>{42});
            CHECK(before.get_inode_xattr(inode,"user.binary")==std::vector<unsigned char>({0,255,0}));fs.verify();store.verify();}
        // Corrupt the actual LevelDB backing, not just an archive checksum.
        // Each copy is closed before mutation and reopened by every assertion.
        auto raw_get=[](const std::string& path,const std::string& key){
            leveldb::DB* raw=nullptr;leveldb::Options options;CHECK(leveldb::DB::Open(options,path,&raw).ok());std::unique_ptr<leveldb::DB> db(raw);
            std::string value;CHECK(db->Get({},key,&value).ok());return value;
        };
        // Build binary keys without including the string literal terminator.
        const auto revision_key=std::string("R/")+std::string("\0forkfs-history/",16)+"revision/before";
        auto revision_id=raw_get(ns_source,revision_key),revision_bytes=raw_get(ns_source,"O/"+revision_id);
        auto manifest_id=revision_bytes.substr(8,32),manifest_bytes=raw_get(ns_source,"O/"+manifest_id);
        CHECK(manifest_bytes.substr(0,8)=="FFTREE01");auto tree_id=manifest_bytes.substr(8,32);
        auto content_hash=Journal::object_id({0,1,255});std::string content_id(reinterpret_cast<const char*>(content_hash.data()),32);
        unsigned damage_case=0;
        for(const auto& damaged_id:{revision_id,manifest_id,tree_id,content_id})for(bool erase:{false,true}){
            auto copy=pattern+"/damaged-source-"+std::to_string(damage_case++);
            std::filesystem::copy(ns_source,copy,std::filesystem::copy_options::recursive);
            auto key="O/"+damaged_id;
            if(erase){leveldb::DB* raw=nullptr;leveldb::Options options;CHECK(leveldb::DB::Open(options,copy,&raw).ok());std::unique_ptr<leveldb::DB> db(raw);leveldb::WriteOptions write;write.sync=true;CHECK(db->Delete(write,key).ok());}
            else {auto value=raw_get(copy,key);CHECK(!value.empty());value.back()^=1;corrupt(copy,key,value);}
            LevelStore damaged(copy);auto sequence=damaged.sequence();
            rejects([&]{damaged.verify();});rejects([&]{damaged.backup(copy+".pack");});
            rejects([&]{damaged.export_replay(sequence,copy+".delta");});CHECK(damaged.sequence()==sequence);
            CHECK(!std::filesystem::exists(copy+".pack") && !std::filesystem::exists(copy+".delta"));
        }
        // Even checksum-valid malformed revision records must fail at export.
        for(unsigned kind=0;kind<4;++kind){
            auto copy=pattern+"/logical-source-"+std::to_string(kind);
            std::filesystem::copy(ns_source,copy,std::filesystem::copy_options::recursive);
            LevelStore damaged(copy);auto bad=std::vector<unsigned char>(revision_bytes.begin(),revision_bytes.end());
            if(kind==0)bad.resize(79);else if(kind==1)bad[0]='X';else if(kind==2)std::fill(bad.begin()+8,bad.begin()+40,0);else std::fill(bad.begin()+40,bad.begin()+72,255);
            damaged.transact(damaged.sequence(),{{revision_key.substr(2),bad}});damaged.verify();auto sequence=damaged.sequence();
            rejects([&]{damaged.backup(copy+".pack");});rejects([&]{damaged.export_replay(sequence,copy+".delta");});CHECK(damaged.sequence()==sequence);
        }
        // The format guard is persisted, including with a reopened source.
        {
            leveldb::DB* raw=nullptr;leveldb::Options options;CHECK(leveldb::DB::Open(options,source,&raw).ok());std::unique_ptr<leveldb::DB> db(raw);
            std::string format;CHECK(db->Get({},"M/format",&format).ok());CHECK(format=="FFLDB002");
        }
        // A backup racing a writer is a single sequence, never a mixture of roots.
        auto concurrent=pattern+"/concurrent";LevelStore::create(concurrent);
        {
            LevelStore store(concurrent);store.transact(0,{{"left",std::vector<unsigned char>{0}},{"right",std::vector<unsigned char>{0}}});
            std::promise<void> ready;auto wait=ready.get_future();
            auto writer=std::async(std::launch::async,[&]{wait.wait();for(unsigned char i=1;i<=80;++i)store.transact(store.sequence(),{{"left",std::vector<unsigned char>{i}},{"right",std::vector<unsigned char>{i}}});});
            ready.set_value();auto seq=store.backup(pattern+"/concurrent-base");writer.get();
            CHECK(LevelStore::replay_archive(pattern+"/concurrent-base",pattern+"/concurrent-target")==seq);
            {LevelStore restored(pattern+"/concurrent-target");auto left=restored.object(restored.root("left"));CHECK(left==restored.object(restored.root("right")));CHECK(left==std::vector<unsigned char>{static_cast<unsigned char>(seq-1)});restored.verify();}
            CHECK(store.export_replay(seq,pattern+"/concurrent-delta")==81);
            CHECK(LevelStore::replay_archive(pattern+"/concurrent-delta",pattern+"/concurrent-target")==81);
            {LevelStore restored(pattern+"/concurrent-target");CHECK(restored.object(restored.root("left"))==std::vector<unsigned char>{80});restored.verify();}
        }
        // Full verification rejects log damage even if the final record survived.
        std::string key="L/";key.append(7,'\0');key.push_back(2);
        corrupt(source,key,"damaged retained record");
        {LevelStore damaged(source);rejects([&]{damaged.verify();});rejects([&]{damaged.export_replay(1,pattern+"/bad-export");});}
        // Corrupt base objects are detected before publishing a recovered name.
        std::string object_key="O/";auto hash=Journal::object_id({42});object_key.append(reinterpret_cast<const char*>(hash.data()),hash.size());
        corrupt(target,object_key,"wrong bytes");
        {LevelStore damaged(target);rejects([&]{damaged.backup(pattern+"/bad-backup");});}
        // A missing format feature flag cannot silently disable retained logging.
        corrupt(concurrent,"M/format","FFLDB001");rejects([&]{LevelStore wrong_format(concurrent);});
        std::filesystem::remove_all(pattern);std::cout<<"replay reference/delete, snapshot race, format and corruption passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";std::filesystem::remove_all(pattern);return 1;}
}
