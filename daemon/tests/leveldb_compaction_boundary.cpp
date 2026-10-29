#include "port/port_stdcxx.h"
#include "db/db_impl.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "db/version_set.h"
#include "util/mutexlock.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/table_builder.h"
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

using namespace leveldb;

#define CHECK(x) do { if(!(x)) throw std::runtime_error("line "+std::to_string(__LINE__)+": " #x); } while(0)

struct TempDir {
    std::string path;
    TempDir(){static unsigned sequence=0;path=(std::filesystem::temp_directory_path()/("forkfs-leveldb-boundary-"+std::to_string(::getpid())+"-"+std::to_string(++sequence))).string();if(!std::filesystem::create_directory(path))throw std::runtime_error("create temporary directory failed");}
    ~TempDir(){std::error_code ec;std::filesystem::remove_all(path,ec);}
};

static void check_status(const Status& status,const char* operation){
    if(!status.ok())throw std::runtime_error(std::string(operation)+": "+status.ToString());
}

struct TableInfo {
    uint64_t number=0;
    uint64_t size=0;
    InternalKey smallest;
    InternalKey largest;
};

static TableInfo write_table(const std::string& dbname,const Options& options,
                             uint64_t number,
                             const std::vector<std::pair<InternalKey,std::string>>& entries){
    CHECK(!entries.empty());
    WritableFile* raw=nullptr;
    check_status(Env::Default()->NewWritableFile(TableFileName(dbname,number),&raw),"create SST");
    std::unique_ptr<WritableFile> file(raw);
    TableBuilder builder(options,file.get());
    for(const auto& [key,value]:entries)builder.Add(key.Encode(),value);
    check_status(builder.Finish(),"finish SST");
    const auto size=builder.FileSize();
    check_status(file->Sync(),"sync SST");check_status(file->Close(),"close SST");
    return {number,size,entries.front().first,entries.back().first};
}

static void add_file(VersionEdit& edit,int level,const TableInfo& file){
    edit.AddFile(level,file.number,file.size,file.smallest,file.largest);
}

static void expect_not_found(DB* db,const std::string& key,const char* where){
    std::string value;auto status=db->Get(ReadOptions(),key,&value);
    if(!status.IsNotFound())throw std::runtime_error(std::string(where)+": expected NotFound, got "+status.ToString());
    std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));it->Seek(key);
    check_status(it->status(),"iterate database");
    CHECK(!it->Valid()||it->key().ToString()!=key);
}

static void expect_survivors(DB* db,const std::string& prefix){
    for(const auto& [suffix,expected]:std::vector<std::pair<std::string,std::string>>{
            {"a","a@1"},{"b","b@5"},{"m","m@6"},{"z","z@3"}}){
        std::string value;check_status(db->Get(ReadOptions(),prefix+suffix,&value),"read surviving key");
        CHECK(value==expected);
    }
}

static void run_case(const std::string& prefix){
    TempDir temp;const auto dbname=(std::filesystem::path(temp.path)/"db").string();
    Options db_options;db_options.create_if_missing=true;
    DB* raw_initialized=nullptr;check_status(DB::Open(db_options,dbname,&raw_initialized),"initialize DB");
    std::unique_ptr<DB> initialized(raw_initialized);initialized.reset();

    InternalKeyComparator internal_comparator(BytewiseComparator());
    Options internal_options;internal_options.comparator=&internal_comparator;internal_options.compression=kNoCompression;
    const std::string a=prefix+"a",b=prefix+"b",m=prefix+"m",n=prefix+"n",z=prefix+"z";
    {
        TableCache table_cache(dbname,internal_options,32);
        VersionSet versions(dbname,&internal_options,&table_cache,&internal_comparator);
        bool save_manifest=false;check_status(versions.Recover(&save_manifest),"recover empty VersionSet");
        auto l1=write_table(dbname,internal_options,versions.NewFileNumber(),{
            {InternalKey(b,5,kTypeValue),"b@5"},{InternalKey(m,6,kTypeValue),"m@6"}});
        auto l2_first=write_table(dbname,internal_options,versions.NewFileNumber(),{
            {InternalKey(a,1,kTypeValue),"a@1"},{InternalKey(n,4,kTypeDeletion),""}});
        auto l2_second=write_table(dbname,internal_options,versions.NewFileNumber(),{
            {InternalKey(n,2,kTypeValue),"old-n@2"},{InternalKey(z,3,kTypeValue),"z@3"}});
        versions.SetLastSequence(6);
        VersionEdit edit;add_file(edit,1,l1);add_file(edit,2,l2_first);add_file(edit,2,l2_second);
        port::Mutex mutex;MutexLock lock(&mutex);auto applied=versions.LogAndApply(&edit,&mutex);
        check_status(applied,"install seeded SST metadata");
    }

    db_options.create_if_missing=false;
    DB* raw_db=nullptr;check_status(DB::Open(db_options,dbname,&raw_db),"open seeded DB");std::unique_ptr<DB> db(raw_db);
    expect_not_found(db.get(),n,"before compaction");expect_survivors(db.get(),prefix);
    const Slice begin(b),end(m);
    static_cast<DBImpl*>(db.get())->TEST_CompactRange(1,&begin,&end);
    expect_not_found(db.get(),n,"after compaction");expect_survivors(db.get(),prefix);
    db.reset();
    raw_db=nullptr;check_status(DB::Open(db_options,dbname,&raw_db),"reopen compacted DB");db.reset(raw_db);
    expect_not_found(db.get(),n,"after reopen");expect_survivors(db.get(),prefix);
}

int main(){try{
    run_case("");
    run_case(std::string("\0forkfs/chunk/",14));
    run_case(std::string("\0forkfs/segment/",16));
    std::cout<<"LevelDB compaction retains boundary tombstones across SST inputs\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
