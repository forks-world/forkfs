#include "level_store.h"
#include "replay_format.h"
#include <leveldb/db.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sys/wait.h>
#include <spawn.h>
extern char** environ;
std::string executable;
using namespace forkfs;using Bytes=std::vector<unsigned char>;namespace fs=std::filesystem;
#define CHECK(x) do{if(!(x))throw std::runtime_error("line "+std::to_string(__LINE__)+": " #x);}while(0)
template<class F>void rejects(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}CHECK(caught);}
using Rows=std::map<std::string,std::string>;
Rows rows(const std::string& path){
    leveldb::DB* raw=nullptr;leveldb::Options options;CHECK(leveldb::DB::Open(options,path,&raw).ok());std::unique_ptr<leveldb::DB> db(raw);
    leveldb::ReadOptions read;read.verify_checksums=true;std::unique_ptr<leveldb::Iterator> it(db->NewIterator(read));Rows result;
    for(it->SeekToFirst();it->Valid();it->Next())result[it->key().ToString()]=it->value().ToString();CHECK(it->status().ok());return result;
}
std::map<std::string,uint64_t> sizes(const std::string& path){std::map<std::string,uint64_t> result;for(const auto& e:fs::directory_iterator(path))if(e.path().extension()==".seg")result[e.path().filename().string()]=e.file_size();return result;}
void prefix_equal(const std::string& original,const std::string& actual,uint64_t length){
    std::ifstream a(original,std::ios::binary),b(actual,std::ios::binary);CHECK(a && b);char left[65536],right[65536];
    while(length){auto n=std::min<uint64_t>(sizeof(left),length);a.read(left,n);b.read(right,n);CHECK(a.gcount()==std::streamsize(n) && b.gcount()==std::streamsize(n) && !memcmp(left,right,n));length-=n;}
}
void crash_write(const std::string& path,const Bytes& value,const std::string& stage,size_t cut=1,size_t ordinal=1){
    // A fresh executable avoids inheriting LevelDB runtime mutexes/worker state.
    {std::ofstream input(path+".input",std::ios::binary);input.write(reinterpret_cast<const char*>(value.data()),value.size());CHECK(bool(input));}
    auto cut_text=std::to_string(cut),ordinal_text=std::to_string(ordinal);
    const char* args[]={executable.c_str(),"--crash-write",path.c_str(),stage.c_str(),cut_text.c_str(),ordinal_text.c_str(),nullptr};
    pid_t pid;CHECK(posix_spawn(&pid,executable.c_str(),nullptr,nullptr,const_cast<char**>(args),environ)==0);
    int result=0;pid_t waited;do{waited=waitpid(pid,&result,0);}while(waited<0 && errno==EINTR);
    CHECK(waited==pid && WIFEXITED(result) && WEXITSTATUS(result)==86);fs::remove(path+".input");
}
int main(int argc,char** argv){
    if(argc==6 && std::string(argv[1])=="--crash-write"){
        setenv("FORKFS_TEST_CRASH",argv[3],1);setenv("FORKFS_TEST_SEGMENT_CUT",argv[4],1);setenv("FORKFS_TEST_SEGMENT_RECORD",argv[5],1);
        try{std::ifstream input(std::string(argv[2])+".input",std::ios::binary);CHECK(bool(input));Bytes value{std::istreambuf_iterator<char>(input),{}};
            LevelStore store(argv[2]);store.transact(store.sequence(),{{"head",value}});
        }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 91;}return 92;
    }
    executable=fs::absolute(argv[0]).string();auto temp=(fs::temp_directory_path()/"ff-object-recovery-XXXXXX").string();CHECK(mkdtemp(temp.data()));try{
    std::mt19937 random(0x8173);Bytes old(180000),next(180000);for(size_t i=0;i<old.size();++i){old[i]=random();next[i]=old[i]^0xa7;}
    auto baseline=temp+"/baseline";LevelStore::create(baseline);uint64_t sequence;
    {LevelStore store(baseline);store.transact(0,{{"head",old},{"frozen",old}});sequence=store.backup(temp+"/base.pack");}
    const auto before=rows(baseline);
    struct Case{std::string stage;size_t cut=1,ordinal=1;bool rollover=false,discard_tail=false;};
    std::vector<Case> cases;
    // Cut magic, ID, length, and payload at boundaries; also crash during the
    // second record after a complete first record has been appended.
    for(size_t cut:{1u,7u,8u,39u,40u,43u,44u,45u,32768u,65579u})cases.push_back({"segment_partial_record",cut});
    for(size_t cut:{1u,43u,32768u})cases.push_back({"segment_partial_record",cut,2});
    // Seeded cuts broaden the matrix beyond hand-selected record boundaries.
    for(unsigned i=0;i<8;++i)cases.push_back({"segment_partial_record",1+random()%65579,1+random()%2});
    cases.push_back({"segment_after_append"});cases.push_back({"segment_after_sync"});cases.push_back({"segment_before_metadata"});
    cases.push_back({"segment_partial_record",17,1,true});cases.push_back({"segment_after_sync",1,1,true});
    cases.push_back({"segment_partial_record",32768,2,false,true});
    unsigned number=0;
    for(const auto& c:cases){
        auto path=temp+"/case-"+std::to_string(number++);fs::copy(baseline,path,fs::copy_options::recursive);
        if(c.rollover)fs::resize_file(path+"/DATA-1.seg",64*1024*1024);
        const auto initial=sizes(path);crash_write(path,next,c.stage,c.cut,c.ordinal);auto crashed=sizes(path);
        uint64_t added=0;for(const auto& [name,length]:crashed)added+=length-(initial.contains(name)?initial.at(name):0);CHECK(added>0);
        CHECK(rows(path)==before); // Includes all C/O/R keys, sequence, counters and replay head/logs.
        for(const auto& [name,length]:sizes(baseline))prefix_equal(baseline+"/"+name,path+"/"+name,length);
        if(c.discard_tail){ // Simulated persisted image: discard only unindexed suffix bytes.
            for(const auto& [name,length]:crashed){(void)length;fs::resize_file(path+"/"+name,initial.contains(name)?initial.at(name):48);}crashed=sizes(path);
        }
        // Reopen repeatedly: recovery must neither infer nor publish a transaction
        // from complete-looking unindexed objects, nor repair indexes by scanning.
        for(unsigned reopen=0;reopen<3;++reopen){LevelStore store(path);CHECK(store.sequence()==sequence);CHECK(store.object(store.root("head"))==old);CHECK(store.object(store.root("frozen"))==old);store.verify();}
        CHECK(rows(path)==before && sizes(path)==crashed);
        auto backup=path+".pack";{LevelStore store(path);store.backup(backup);}
        CHECK(rows(path)==before);auto recovered=path+"-recovered";LevelStore::replay_archive(backup,recovered);
        {LevelStore store(recovered);CHECK(store.object(store.root("head"))==old);store.verify();}
        // Preserve every crash-tail byte to prove the next writer appends beyond
        // EOF rather than treating a partial record as free space or valid data.
        auto tails=path+"-tails";fs::create_directory(tails);for(const auto& [name,length]:crashed){(void)length;fs::copy_file(path+"/"+name,tails+"/"+name);}
        {LevelStore store(path);store.transact(sequence,{{"head",next}});CHECK(store.sequence()==sequence+1);CHECK(store.object(store.root("head"))==next);CHECK(store.object(store.root("frozen"))==old);store.verify();store.export_replay(sequence,path+".delta");}
        for(const auto& [name,length]:crashed)prefix_equal(tails+"/"+name,path+"/"+name,length);
        const auto after=rows(path);unsigned new_chunks=0;
        for(const auto& [key,value]:after)if(key.starts_with("C/") && !before.contains(key)){
            ++new_chunks;auto id=replay::num(value.substr(0,8)),offset=replay::num(value.substr(8,8));auto name="DATA-"+std::to_string(id)+".seg";
            CHECK(offset>=(crashed.contains(name)?crashed.at(name):48));
        }
        CHECK(new_chunks==3 && after.at("M/sequence")==replay::le(sequence+1));
        // Retry generates exactly one recoverable transaction, including after a
        // previous complete-but-unpublished copy of all chunks was left behind.
        LevelStore::replay_archive(path+".delta",recovered);{LevelStore store(recovered);CHECK(store.sequence()==sequence+1);CHECK(store.object(store.root("head"))==next);store.verify();}
        fs::remove_all(path);fs::remove_all(tails);fs::remove_all(recovered);fs::remove(backup);fs::remove(path+".delta");
    }
    // Commit completed but the client received no reply: the new state is durable.
    auto committed=temp+"/committed";fs::copy(baseline,committed,fs::copy_options::recursive);crash_write(committed,next,"segment_after_metadata");
    {LevelStore store(committed);CHECK(store.sequence()==sequence+1 && store.object(store.root("head"))==next && store.object(store.root("frozen"))==old);store.verify();}
    auto published=rows(committed);std::string location;for(const auto& [key,value]:published)if(key.starts_with("C/") && !before.contains(key)){location=value;break;}CHECK(!location.empty());
    // In contrast to unindexed tails, any damage to an indexed record is fatal.
    auto id=replay::num(location.substr(0,8)),offset=replay::num(location.substr(8,8));auto length=uint64_t(static_cast<unsigned char>(location[16])) | uint64_t(static_cast<unsigned char>(location[17]))<<8 | uint64_t(static_cast<unsigned char>(location[18]))<<16;
    for(bool truncate:{false,true}){auto broken=temp+(truncate?"/truncated":"/corrupt");fs::copy(committed,broken,fs::copy_options::recursive);auto file=broken+"/DATA-"+std::to_string(id)+".seg";
        if(truncate)fs::resize_file(file,offset+44+length-1);
        else{std::fstream stream(file,std::ios::binary|std::ios::in|std::ios::out);stream.seekg(offset+44);char byte;stream.get(byte);CHECK(bool(stream));stream.seekp(offset+44);stream.put(byte^1);}
        rejects([&]{LevelStore store(broken);(void)store.object(store.root("head"));});rejects([&]{LevelStore store(broken);store.verify();});rejects([&]{LevelStore store(broken);store.backup(broken+".pack");});
        CHECK(rows(broken)==published && !fs::exists(broken+".pack"));
    }
    fs::remove_all(temp);std::cout<<number<<" uncommitted crash cases, exact index equality, repeated reopen, append-only retry, replay and committed corruption passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';fs::remove_all(temp);return 1;}}
