#include "level_store.h"
#include "replay_format.h"
#include <leveldb/db.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
using namespace forkfs;using Bytes=std::vector<unsigned char>;
#define CHECK(x) do{if(!(x))throw std::runtime_error("line "+std::to_string(__LINE__)+": " #x);}while(0)
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}CHECK(failed);}
std::unique_ptr<leveldb::DB> raw(const std::string& path,bool create=false){leveldb::DB* p=nullptr;leveldb::Options o;o.create_if_missing=create;CHECK(leveldb::DB::Open(o,path,&p).ok());return std::unique_ptr<leveldb::DB>(p);}
std::string get(leveldb::DB& db,const std::string& key){std::string v;CHECK(db.Get({},key,&v).ok());return v;}
void put(leveldb::DB& db,const std::string& k,const std::string& v){leveldb::WriteOptions w;w.sync=true;CHECK(db.Put(w,k,v).ok());}
uint64_t physical_size(const std::string& path){uint64_t n=0;for(const auto& e:std::filesystem::directory_iterator(path))if(e.path().extension()==".seg")n+=e.file_size();return n;}
int main(){auto temp=(std::filesystem::temp_directory_path()/"ff-segments-XXXXXX").string();CHECK(mkdtemp(temp.data()));try{
    auto path=temp+"/source";LevelStore::create(path);std::mt19937 rng(381);
    Bytes first(4*65536);for(auto& b:first)b=rng();auto changed=first;changed[65536+77]^=0xff;
    Journal::object_id(first);uint64_t base=0;std::array<unsigned char,32> first_id{};
    {
        LevelStore store(path);store.transact(0,{{"head",first}});first_id=store.root("head");
        auto bytes=physical_size(path);CHECK(bytes==48+4*(65536+44));
        base=store.backup(temp+"/base.pack");store.transact(store.sequence(),{{"head",changed}});
        CHECK(physical_size(path)-bytes==65536+44); // One changed block, not the whole file.
        CHECK(store.object(first_id)==first);CHECK(store.object(store.root("head"))==changed);
        store.export_replay(base,temp+"/delta.pack");store.verify();
    }
    {
        auto db=raw(path);CHECK(get(*db,"M/format")=="FFLDB004");
        std::unique_ptr<leveldb::Iterator> it(db->NewIterator({}));unsigned chunks=0,objects=0,logs=0;
        for(it->SeekToFirst();it->Valid();it->Next()){
            auto k=it->key().ToString(),v=it->value().ToString();
            if(k.starts_with("C/")){++chunks;CHECK(v.size()==20);}
            if(k.starts_with("O/")){++objects;CHECK(v.starts_with("FFOBJ001") && v.size()==148);}
            if(k.starts_with("L/")){++logs;CHECK(v.starts_with("FFLOGR01") && v.size()<1024);}
            CHECK(v.find(std::string(changed.begin()+70000,changed.begin()+70100))==std::string::npos);
        }
        CHECK(it->status().ok() && chunks==5 && objects==2 && logs==1);
    }
    auto target=temp+"/target";LevelStore::replay_archive(temp+"/base.pack",target);LevelStore::replay_archive(temp+"/delta.pack",target);
    {LevelStore store(target);CHECK(store.object(store.root("head"))==changed);CHECK(store.object(first_id)==first);store.verify();}
    // Reopen appends to the same segment; tiny values remain inline.
    auto previous_size=physical_size(path);{LevelStore store(path);auto next=changed;next.back()^=1;store.transact(store.sequence(),{{"head",next},{"tiny",Bytes{0,255}}});store.verify();}
    CHECK(physical_size(path)-previous_size==65536+44);
    // Independently mutate physical chunk locations/data, bypassing archive checksums.
    for(unsigned kind=0;kind<7;++kind){auto copy=temp+"/bad-"+std::to_string(kind);std::filesystem::copy(target,copy,std::filesystem::copy_options::recursive);
        auto file=copy+"/DATA-1.seg";
        if(kind==0)std::filesystem::remove(file);
        if(kind==1)std::filesystem::resize_file(file,60);
        if(kind==2 || kind==3){std::fstream f(file,std::ios::binary|std::ios::in|std::ios::out);f.seekp(kind==2?100:0);f.put('!');}
        if(kind>=4){auto db=raw(copy);std::unique_ptr<leveldb::Iterator> it(db->NewIterator({}));it->Seek("C/");CHECK(it->Valid());auto key=it->key().ToString(),v=it->value().ToString();
            if(kind==4)v=replay::le(UINT64_MAX)+v.substr(8);
            if(kind==5)v=v.substr(0,8)+replay::le(UINT64_MAX)+v.substr(16);
            if(kind==6)v=v.substr(0,16)+replay::le(UINT32_MAX,4);
            put(*db,key,v);
        }
        rejects([&]{LevelStore broken(copy);broken.verify();});rejects([&]{LevelStore broken(copy);broken.backup(copy+".pack");});
    }
    // An old raw-value repository remains readable; restore migrates physical layout
    // while retaining object IDs and recovery-chain hashes.
    auto legacy=temp+"/legacy";{
        auto db=raw(legacy,true);put(*db,"M/format","FFLDB001");put(*db,"M/repository",std::string(16,'r'));
        for(const auto& key:{"M/sequence","M/roots","M/objects"})put(*db,key,replay::le(0));
    }
    {LevelStore store(legacy);store.transact(0,{{"head",first}});store.backup(temp+"/legacy.pack");}
    auto migrated=temp+"/migrated";LevelStore::replay_archive(temp+"/legacy.pack",migrated);
    {LevelStore store(migrated);CHECK(store.root("head")==first_id && store.object(first_id)==first);store.verify();}
    CHECK(physical_size(migrated)>0);
    std::filesystem::remove_all(path);std::filesystem::remove_all(legacy); // Recovery does not consult source paths.
    {LevelStore store(target);CHECK(store.object(store.root("head"))==changed);store.verify();}
    std::filesystem::remove_all(temp);std::cout<<"segmented content, block reuse, portable replay, migration and corruption passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(temp);return 1;}}
