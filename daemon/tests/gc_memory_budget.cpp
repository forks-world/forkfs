#include "level_store.h"
#include "namespace.h"
#include "file_content.h"
#include "metadata_view.h"
#include "revision.h"
#include <cerrno>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <regex>
#include <stdexcept>
#include <unistd.h>

using namespace forkfs;
namespace {
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (0)
void budget_error(auto&& operation);
uint64_t field(const std::string& json,const char* name) {
    std::regex expression(std::string("\\\"")+name+"\\\":([0-9]+)");
    std::smatch match;CHECK(std::regex_search(json,match,expression));return std::stoull(match[1]);
}
bool boolean_field(const std::string& json,const char* name) {
    std::regex expression(std::string("\\\"")+name+"\\\":(true|false)");
    std::smatch match;CHECK(std::regex_search(json,match,expression));return match[1]=="true";
}
std::vector<unsigned char> value(size_t size,unsigned seed) {
    std::vector<unsigned char> out(size);
    for(size_t i=0;i<size;++i)out[i]=static_cast<unsigned char>((i*37+seed*19+(i>>8))%251);
    return out;
}
std::vector<unsigned char> text(unsigned n) {
    std::string out="opaque-"+std::to_string(n);
    out.resize(128,static_cast<char>('a'+n%26));return {out.begin(),out.end()};
}
void persist_unrooted(LevelStore& store,const std::vector<std::vector<unsigned char>>& objects) {
    for(size_t base=0;base<objects.size();) {
        std::vector<Mutation> batch;size_t bytes=0;
        while(base<objects.size() && batch.size()<180 && bytes+objects[base].size()<=12*1024*1024) {
            batch.push_back({"gc-fixture/"+std::to_string(base),objects[base]});bytes+=objects[base].size();++base;
        }
        store.transact(store.sequence(),batch);
    }
    for(size_t base=0;base<objects.size();) {
        std::vector<Mutation> batch;while(base<objects.size() && batch.size()<180){batch.push_back({"gc-fixture/"+std::to_string(base),std::nullopt});++base;}
        store.transact(store.sequence(),batch);
    }
}
std::vector<unsigned char> legacy_manifest(const NamespaceView::Changes& entries) {
    std::vector<unsigned char> full(12);memcpy(full.data(),"FFMAN001",8);
    auto put32=[](std::vector<unsigned char>& out,size_t at,uint32_t number){for(unsigned i=0;i<4;++i)out[at+i]=static_cast<unsigned char>(number>>(8*i));};
    put32(full,8,entries.size());
    for(const auto& [key,id]:entries){CHECK(id.has_value());auto at=full.size();full.resize(at+4+key.size()+32);put32(full,at,key.size());
        std::copy(key.begin(),key.end(),full.begin()+at+4);std::copy(id->begin(),id->end(),full.begin()+at+4+key.size());}
    return full;
}
void check_recoverable_graph(const std::string& repo,const std::string& root_key,
                             const std::vector<unsigned char>& root_value,
                             const std::vector<unsigned char>& sample) {
    LevelStore store(repo);const auto roots=store.roots();const auto objects=store.objects();const auto sequence=store.sequence();
    const auto root_id=Journal::object_id(root_value),sample_id=Journal::object_id(sample);
    budget_error([&]{(void)store.collect_step(1024*1024,128,64*1024);});
    CHECK(store.roots()==roots&&store.objects()==objects&&store.sequence()==sequence);
    CHECK(store.root(root_key)==root_id&&store.object(sample_id)==sample);
    store.verify();store.barrier();store.transact(store.sequence(),{{"post-refusal",sample}});
    CHECK(store.object(sample_id)==sample);store.verify();
    for(unsigned i=0;i<512;++i){auto report=store.collect_step(1024*1024,128,256*1024*1024);
        CHECK(field(report,"metadata_bytes_charged")<=field(report,"metadata_budget_bytes"));
        if(field(report,"objects_deleted")==0&&field(report,"chunks_moved")==0&&field(report,"segments_deleted")==0&&field(report,"pending_segments")==0)return;}
    throw std::runtime_error("GC did not converge for metadata graph fixture");
}

void legacy_graph(const std::string& repo) {
    LevelStore::create(repo);auto payload=text(801);auto target=Journal::object_id(payload);
    NamespaceView::Changes entries;const std::string ns("\0forkfs/",8);entries[ns+"root"]=target;
    for(unsigned i=0;i<700;++i)entries[ns+"entry/"+std::to_string(i)]=target;
    auto manifest=legacy_manifest(entries);
    const std::string world("\0forkfs-history/world/legacy",28);
    {LevelStore store(repo);persist_unrooted(store,{payload,manifest});store.transact(store.sequence(),{{world,manifest}});}
    check_recoverable_graph(repo,world,manifest,payload);
}
void revision_graph(const std::string& repo) {
    LevelStore::create(repo);auto payload=text(802);auto target=Journal::object_id(payload);
    NamespaceView::Changes entries;const std::string ns("\0forkfs/",8);entries[ns+"root"]=target;
    auto manifest=legacy_manifest(entries);auto manifest_id=Journal::object_id(manifest);
    std::vector<std::vector<unsigned char>> descriptors;RevisionRecord::Id parent{};
    for(unsigned i=0;i<500;++i){RevisionRecord record{manifest_id,i,i?std::vector<RevisionRecord::Id>{parent}:std::vector<RevisionRecord::Id>{}};
        auto bytes=record.encode();parent=Journal::object_id(bytes);descriptors.push_back(std::move(bytes));}
    std::vector<std::vector<unsigned char>> all{payload,manifest};all.insert(all.end(),descriptors.begin(),descriptors.end());
    const std::string key("\0forkfs-history/revision/head",29);
    {LevelStore store(repo);persist_unrooted(store,all);store.transact(store.sequence(),{{key,descriptors.back()}});}
    check_recoverable_graph(repo,key,descriptors.back(),payload);
}
void extent_graph(const std::string& repo) {
    LevelStore::create(repo);using Id=FileContent::Id;using Bytes=FileContent::Bytes;
    std::map<Id,Bytes> content_objects;
    auto reader=[&](const Id& id){return content_objects.at(id);};
    FileContent file(reader,{},0,false);constexpr uint64_t blocks=700;
    for(uint64_t base=0;base<blocks;base+=128){const auto count=std::min<uint64_t>(128,blocks-base);Bytes chunk(count*FileContent::block_size,0);
        for(uint64_t i=0;i<count;++i){auto block=base+i;for(unsigned b=0;b<8;++b)chunk[i*FileContent::block_size+b]=static_cast<unsigned char>(block>>(b*8));chunk[i*FileContent::block_size+8]=static_cast<unsigned char>((block*29+17)%251);}
        file.write(base*FileContent::block_size,chunk);}
    std::vector<Bytes> extent_objects;auto descriptor=file.encode(extent_objects);
    auto inode=Bytes(84,0);memcpy(inode.data(),"FFINOD05",8);inode[8]=1;inode[48]=2;
    const auto size=file.size();for(unsigned i=0;i<8;++i)inode[24+i]=static_cast<unsigned char>(size>>(i*8));
    auto root_payload=text(803);const auto inode_id=Journal::object_id(inode),content_id=Journal::object_id(descriptor),root_id=Journal::object_id(root_payload);
    const std::string ns("\0forkfs/",8);NamespaceView::Changes entries;entries[ns+"root"]=root_id;
    entries[ns+"inode/fixture"]=inode_id;entries[ns+"content/fixture"]=content_id;
    auto put32=[](Bytes& out,size_t at,uint32_t number){for(unsigned i=0;i<4;++i)out[at+i]=static_cast<unsigned char>(number>>(8*i));};
    Bytes run(12);memcpy(run.data(),"FFRUN001",8);put32(run,8,entries.size());
    for(const auto& [key,id]:entries){CHECK(id.has_value());auto at=run.size();run.resize(at+8+key.size()+32);put32(run,at,1);put32(run,at+4,key.size());
        std::copy(key.begin(),key.end(),run.begin()+at+8);std::copy(id->begin(),id->end(),run.begin()+at+8+key.size());}
    auto run_id=Journal::object_id(run);Bytes manifest(44);memcpy(manifest.data(),"FFMAN002",8);put32(manifest,8,1);std::copy(run_id.begin(),run_id.end(),manifest.begin()+12);
    std::vector<Bytes> all{root_payload,inode,descriptor,run,manifest};all.insert(all.end(),extent_objects.begin(),extent_objects.end());
    const std::string world("\0forkfs-history/world/extents",29);
    {LevelStore store(repo);persist_unrooted(store,all);store.transact(store.sequence(),{{world,manifest}});}
    check_recoverable_graph(repo,world,manifest,extent_objects.front());
}
void late_refusal(const std::string& repo) {
    LevelStore::create(repo);LevelStore store(repo);std::vector<Mutation> roots;
    std::vector<std::vector<unsigned char>> expected;
    for(unsigned i=0;i<96;++i){expected.push_back(value(4096,i+900));roots.push_back({"live/"+std::to_string(i),expected.back()});}
    auto stale=value(4096,1900);roots.push_back({"stale",stale});store.transact(store.sequence(),roots);
    store.transact(store.sequence(),{{"stale",std::nullopt}});
    const auto before_objects=store.objects(),before_roots=store.roots();
    budget_error([&]{(void)store.collect_step(1024*1024,128,64*1024);});
    // The rooted objects and healthy engine survive even when collection had
    // already durably removed the stale object before the physical scan refused.
    CHECK(store.objects()==before_objects-1&&store.roots()==before_roots);
    for(unsigned i=0;i<expected.size();++i)CHECK(store.object(store.root("live/"+std::to_string(i)))==expected[i]);
    store.verify();store.barrier();store.transact(store.sequence(),{{"after-late-refusal",expected.front()}});
    CHECK(store.object(store.root("after-late-refusal"))==expected.front());store.verify();
    for(unsigned i=0;i<512;++i){auto report=store.collect_step(1024*1024,128,256*1024*1024);
        CHECK(field(report,"metadata_bytes_charged")<=field(report,"metadata_budget_bytes"));
        if(field(report,"objects_deleted")==0&&field(report,"chunks_moved")==0&&field(report,"segments_deleted")==0&&field(report,"pending_segments")==0)break;
        if(i==511)throw std::runtime_error("GC did not converge after late budget refusal");}
    CHECK(store.object(store.root("live/95"))==expected.back());store.verify();
}
void streaming_indexes(const std::string& repo) {
    LevelStore::create(repo);LevelStore store(repo);const auto sample=text(2891);
    store.transact(store.sequence(),{{"sample",sample}});const auto sample_id=store.root("sample");
    std::vector<std::vector<unsigned char>> garbage;garbage.reserve(8000);
    for(unsigned i=0;i<8000;++i)garbage.push_back(text(30000+i));
    persist_unrooted(store,garbage);
    const auto objects_before=store.objects();CHECK(objects_before>=8001);
    auto pin=store.pin_objects();
    const auto report=store.collect_step(65580,1,64*1024);
    CHECK(field(report,"metadata_bytes_charged")<=64*1024);
    CHECK(field(report,"metadata_budget_bytes")==64*1024);
    CHECK(boolean_field(report,"logical_sweep_skipped"));
    CHECK(field(report,"objects_deleted")==0&&store.objects()==objects_before);
    CHECK(store.root("sample")==sample_id&&store.object(sample_id)==sample);
    store.verify();
}
void budget_error(auto&& operation) {
    bool expected=false;
    try { operation(); }
    catch(const std::exception& error) { expected=std::string(error.what()).find("GC metadata memory budget exceeded")!=std::string::npos; }
    CHECK(expected);
}
}

int main() {
    auto temp=(std::filesystem::temp_directory_path()/"ff-gc-budget-XXXXXX").string();
    CHECK(mkdtemp(temp.data()));
    try {
        const auto repo=temp+"/opaque";LevelStore::create(repo);
        {
            LevelStore store(repo);
            bool low=false,high=false;
            try{(void)store.collect_step(1024*1024,128,0);}catch(const std::exception&){low=true;}
            try{(void)store.collect_step(1024*1024,128,256*1024*1024+1);}catch(const std::exception&){high=true;}
            CHECK(low&&high);

            // Keep a sizeable rooted set and create additional historical objects
            // so the bounded mark walk has to account for real repository state.
            constexpr unsigned rooted=700;
            for(unsigned i=0;i<rooted;++i)
                store.transact(store.sequence(),{{"live-"+std::to_string(i),text(i)}});
            const auto objects_before=store.objects();
            const auto sample_id=store.root("live-350");const auto sample=text(350);
            for(unsigned i=0;i<300;++i)
                store.transact(store.sequence(),{{"waste",text(10000+i)}});
            const auto roots_before=store.roots();
            const auto before_failure=store.objects();
            const auto before_sequence=store.sequence();
            budget_error([&]{(void)store.collect_step(1024*1024,128,64*1024);});
            CHECK(store.objects()==before_failure && store.roots()==roots_before);
            CHECK(store.sequence()==before_sequence && store.root("live-350")==sample_id);
            CHECK(store.object(sample_id)==sample);
            store.verify();store.barrier();
            store.transact(store.sequence(),{{"healthy-alias",std::nullopt,sample_id}});
            CHECK(store.root("healthy-alias")==sample_id);
            CHECK(store.objects()>=objects_before);
            bool failed_again=false;
            try{(void)store.collect_step(1024*1024,128,256*1024*1024);}catch(const std::exception&){failed_again=true;}
            CHECK(!failed_again);
            for(unsigned i=0;i<256;++i) {
                auto report=store.collect_step(1024*1024,128,256*1024*1024);
                CHECK(field(report,"metadata_bytes_charged")<=field(report,"metadata_budget_bytes"));
                if(field(report,"objects_deleted")==0&&field(report,"chunks_moved")==0&&
                   field(report,"segments_deleted")==0&&field(report,"pending_segments")==0)break;
                if(i==255)throw std::runtime_error("GC did not drain garbage within bounded steps");
            }
            CHECK(store.root("live-350")==sample_id);CHECK(store.object(sample_id)==sample);
            store.verify();
        }

        // A real namespace closure exercises the metadata tree traversal that
        // opaque LevelStore values do not contain.
        const auto fs_path=temp+"/namespace";Container::create(fs_path);
        {
            Container container(fs_path);Namespace fs(container);fs.initialize();
            for(unsigned i=0;i<500;++i)fs.write("/dir-"+std::to_string(i),value(96,i));
            budget_error([&]{(void)container.collect_step(1024*1024,128,64*1024);});
            container.verify();
            const auto probe=value(96,271);fs.write("/after-refusal",probe);
            CHECK(fs.read("/after-refusal")==probe);
            auto report=container.collect_step(1024*1024,128,256*1024*1024);
            CHECK(field(report,"metadata_bytes_charged")<=field(report,"metadata_budget_bytes"));
            container.verify();
        }
        legacy_graph(temp+"/legacy");
        revision_graph(temp+"/revision");
        extent_graph(temp+"/extent");
        late_refusal(temp+"/late");
        streaming_indexes(temp+"/streaming");
        std::filesystem::remove_all(temp);
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';std::filesystem::remove_all(temp);return 1;
    }
}
