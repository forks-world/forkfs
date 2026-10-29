#include "level_store.h"
#include "object_store.h"
#include <leveldb/write_batch.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <leveldb/db.h>
#include <leveldb/iterator.h>
#include <regex>
#include <unistd.h>

using namespace forkfs;
namespace fs=std::filesystem;
namespace {
#define CHECK(x) do { if(!(x)) throw std::runtime_error("line "+std::to_string(__LINE__)+": " #x); } while(0)
uint64_t field(const std::string& json,const char* key){std::regex r(std::string("\\\"")+key+"\\\":([0-9]+)");std::smatch m;CHECK(std::regex_search(json,m,r));return std::stoull(m[1]);}
bool boolean(const std::string& json,const char* key){std::regex r(std::string("\\\"")+key+"\\\":(true|false)");std::smatch m;CHECK(std::regex_search(json,m,r));return m[1]=="true";}
struct Temp {fs::path path;Temp(){char p[]="/tmp/ff-gc-segment-index-XXXXXX";auto* q=mkdtemp(p);if(!q)throw std::runtime_error("mkdtemp failed");path=q;}~Temp(){std::error_code e;fs::remove_all(path,e);}};
std::vector<unsigned char> payload(unsigned i){std::vector<unsigned char> b(4096);for(size_t n=0;n<b.size();++n)b[n]=static_cast<unsigned char>((i*29+n*17)%251);return b;}
std::vector<fs::path> segments(const fs::path& p){std::vector<fs::path> v;for(auto& e:fs::directory_iterator(p))if(e.path().filename().string().starts_with("DATA-")&&e.path().extension()==".seg")v.push_back(e.path());std::sort(v.begin(),v.end(),[](const auto& a,const auto& b){return std::stoull(a.stem().string().substr(5))<std::stoull(b.stem().string().substr(5));});return v;}
size_t c_rows(const fs::path& p){leveldb::Options o;o.create_if_missing=false;leveldb::DB* raw=nullptr;CHECK(leveldb::DB::Open(o,p.string(),&raw).ok());std::unique_ptr<leveldb::DB> db(raw);leveldb::ReadOptions ro;ro.fill_cache=false;std::unique_ptr<leveldb::Iterator> it(db->NewIterator(ro));size_t n=0;for(it->Seek("C/");it->Valid()&&it->key().starts_with("C/");it->Next())++n;CHECK(it->status().ok());return n;}
void drain(LevelStore& s,uint64_t& indexed,uint64_t& refs,uint64_t& candidates,uint64_t& chunks,const std::function<void(const std::string&)>& observe={}){
    for(size_t n=0;n<200000;++n){auto r=s.collect_step(65580,8,65536,16);
        CHECK(field(r,"physical_work_done")<=16);CHECK(field(r,"physical_index_work")<=field(r,"physical_work_done"));
        CHECK(field(r,"physical_reference_work")<=field(r,"physical_work_done"));CHECK(field(r,"physical_candidate_work")<=field(r,"physical_work_done"));
        CHECK(field(r,"physical_index_work")+field(r,"physical_reference_work")+field(r,"physical_candidate_work")<=field(r,"physical_work_done"));
        indexed+=field(r,"physical_index_work");refs+=field(r,"physical_reference_work");candidates+=field(r,"physical_candidate_work");chunks+=field(r,"chunks_moved");
        if(observe)observe(r);
        if(!boolean(r,"mark_pending")&&!boolean(r,"physical_pending"))return;
    }throw std::runtime_error("segment-index collection failed to converge");
}
struct Result{uint64_t index=0,refs=0,candidates=0,chunks=0;size_t segs=0,rows=0;};
Result run(unsigned count,bool dirty_tail=true){
    Temp t;auto repo=t.path/"repo",archive=t.path/"backup";LevelStore::create(repo.string());Result out;
    std::vector<std::vector<unsigned char>> expected;
    const auto concurrent=payload(count*8+3);bool injected=false;
    {
        LevelStore s(repo.string());
        // Keep each payload version live through the replay stream, while the
        // current roots give us a deterministic read-after-reopen oracle.
        for(unsigned i=0;i<count*4;++i){auto b=payload(i);expected.push_back(b);s.transact(s.sequence(),{{"root-"+std::to_string(i),b}});}
        s.backup(archive.string());
    }
    // Sparse unindexed tails fill each owner segment without allocating disk
    // blocks. Reopening makes the object store roll to a fresh DATA segment.
    for(unsigned i=0;i<count-1;++i){auto files=segments(repo);CHECK(!files.empty());fs::resize_file(files.back(),64ULL*1024*1024);LevelStore s(repo.string());auto b=payload(count*4+i);expected.push_back(b);s.transact(s.sequence(),{{"roll-"+std::to_string(i),b}});}
    // Make the active segment a candidate, which forces a fresh active C scan
    // after the concurrent publication below.
    if(dirty_tail){auto files=segments(repo);CHECK(!files.empty());std::ofstream tail(files.back(),std::ios::binary|std::ios::app);CHECK(tail.good());std::vector<char> junk(4096,'x');tail.write(junk.data(),static_cast<std::streamsize>(junk.size()));CHECK(tail.good());}
    out.segs=segments(repo).size();CHECK(out.segs>=count);
    out.rows=c_rows(repo); // Exact pre-GC C population, with no LevelDB owner open.
    {
        LevelStore s(repo.string());s.verify();
        bool scratch=false;
        for(size_t step=0;step<20000&&!scratch;++step){(void)s.collect_step(65580,1,65536,1);
            for(const auto& e:fs::directory_iterator(repo))if(e.path().filename().string().starts_with(".gc-physical-")){scratch=true;break;}}
        CHECK(scratch);s.cancel_collection();
        for(const auto& e:fs::directory_iterator(repo))CHECK(!e.path().filename().string().starts_with(".gc-physical-"));
        drain(s,out.index,out.refs,out.candidates,out.chunks,[&](const std::string& report){if(!injected&&report.find("\"physical_phase\":\"index\"")!=std::string::npos){s.transact(s.sequence(),{{"concurrent-live",concurrent}});injected=true;}});s.verify();
        CHECK(injected);
        for(unsigned i=0;i<count*4;++i)CHECK(s.object(s.root("root-"+std::to_string(i)))==expected[i]);
        for(unsigned i=0;i<count-1;++i)CHECK(s.object(s.root("roll-"+std::to_string(i)))==expected[count*4+i]);
        if(injected)CHECK(s.object(s.root("concurrent-live"))==concurrent);
    }
    {LevelStore reopened(repo.string());reopened.verify();CHECK(reopened.object(reopened.root("concurrent-live"))==concurrent);}
    CHECK(c_rows(repo)==out.rows+1);
    return out;
}
// A corrupt marker must not become believable just because foreground writes
// rotate the captured active segment while the initial scratch index is built.
void forged_tail_during_build(){
    Temp t;auto repo=t.path/"repo";LevelStore::create(repo.string());
    {LevelStore s(repo.string());s.transact(0,{{"base",payload(0)}});}
    {ObjectStore objects(repo.string(),0);objects.seal_and_rotate();}
    fs::resize_file(repo/"DATA-2.seg",64ULL*1024*1024-8192);
    {
        leveldb::Options options;leveldb::DB* raw=nullptr;
        CHECK(leveldb::DB::Open(options,repo.string(),&raw).ok());std::unique_ptr<leveldb::DB> db(raw);
        std::string sid(8,0);sid[0]=2;leveldb::WriteBatch b;b.Put("G/"+sid,"FFGCRET1");b.Put("M/format","FFLDB007");
        leveldb::WriteOptions wo;wo.sync=true;CHECK(db->Write(wo,&b).ok());
    }
    {
        LevelStore s(repo.string());auto first=s.collect_step(65580,8,65536,1);
        CHECK(first.find("\"physical_phase\":\"index\"")!=std::string::npos);
        auto a=payload(199),b=payload(200);s.transact(s.sequence(),{{"new-a",a},{"new-b",b}});
        CHECK(fs::exists(repo/"DATA-3.seg"));
        bool refused=false;
        for(unsigned i=0;i<100&&!refused;++i){
            try{(void)s.collect_step(65580,8,65536,1);}
            catch(const std::exception& e){refused=std::string(e.what()).find("outside indexed generation")!=std::string::npos;CHECK(refused);}
        }
        CHECK(refused);CHECK(fs::exists(repo/"DATA-2.seg"));
        CHECK(s.object(s.root("new-a"))==a);CHECK(s.object(s.root("new-b"))==b);
        s.barrier(); // Scratch refusal does not poison the main owner.
    }
}

}
int main(){try{forged_tail_during_build();auto packed=run(4,false);CHECK(packed.chunks==packed.rows-1);auto small=run(4),large=run(12);const auto small_rows=small.rows+1,large_rows=large.rows+1;CHECK(small.index>0&&large.index>0);CHECK(large.segs>small.segs);CHECK(small.index<=5*small_rows);CHECK(large.index<=5*large_rows);CHECK(large.index*small_rows*2<=small.index*large_rows*3);CHECK(large.refs>0&&small.refs>0);std::cout<<"scratch reverse segment index regression passed; segments="<<small.segs<<'/'<<large.segs<<" index rows scanned="<<small.index<<'/'<<large.index<<" pre-GC C rows="<<small.rows<<'/'<<large.rows<<" packed-tail chunks moved="<<packed.chunks<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
