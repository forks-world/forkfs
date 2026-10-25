#include "level_store.h"
#include "namespace.h"
#include "repository_budget.h"
#include <leveldb/db.h>
#include <leveldb/write_batch.h>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <system_error>
#include <unistd.h>

using namespace forkfs;
namespace {
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (0)
uint64_t field(const std::string& json,const char* name) {
    std::regex expression(std::string("\\\"")+name+"\\\":([0-9]+)");
    std::smatch match;CHECK(std::regex_search(json,match,expression));return std::stoull(match[1]);
}
bool boolean_field(const std::string& json,const char* name) {
    std::regex expression(std::string("\\\"")+name+"\\\":(true|false)");
    std::smatch match;CHECK(std::regex_search(json,match,expression));return match[1]=="true";
}
uint64_t segment_bytes(const std::string& path) {
    uint64_t result=0;
    for (const auto& item:std::filesystem::directory_iterator(path))
        if(item.path().extension()==".seg") result+=item.file_size();
    return result;
}
std::vector<unsigned char> payload(size_t size,unsigned seed) {
    std::vector<unsigned char> value(size);
    for(size_t i=0;i<size;++i)value[i]=static_cast<unsigned char>((i*37+seed*19+(i>>8))%251);
    return value;
}
void drain(LevelStore& store) {
    for(unsigned i=0;i<256;++i) {
        auto report=store.collect_step(256*1024,32);
        CHECK(field(report,"bytes_copied")<=256*1024);
        CHECK(field(report,"objects_deleted")<=128);
        store.verify();
        if(field(report,"objects_deleted")==0 && field(report,"chunks_moved")==0 &&
           field(report,"segments_deleted")==0 && field(report,"pending_segments")==0) return;
    }
    throw std::runtime_error("online GC did not converge within bounded steps");
}
template<class F> void rejects(F operation) {
    bool rejected=false;try{operation();}catch(const std::exception&){rejected=true;}CHECK(rejected);
}
void forge_retired_marker(const std::string& path,uint64_t sid) {
    leveldb::DB* db=nullptr;leveldb::Options options;
    CHECK(leveldb::DB::Open(options,path,&db).ok());std::unique_ptr<leveldb::DB> owner(db);
    std::string encoded(8,'\0');for(unsigned i=0;i<8;++i)encoded[i]=static_cast<char>(sid>>(8*i));
    leveldb::WriteBatch batch;batch.Put("M/format","FFLDB007");batch.Put("G/"+encoded,"FFGCRET1");
    leveldb::WriteOptions write;write.sync=true;CHECK(owner->Write(write,&batch).ok());
}
}

int main() {
    auto temp=(std::filesystem::temp_directory_path()/"ff-online-gc-XXXXXX").string();
    CHECK(mkdtemp(temp.data()));
    try {
        const auto repo=temp+"/repo";
        LevelStore::create(repo);
        auto live=payload(192*1024,1);
        {
            LevelStore store(repo);
            store.transact(0,{{"head",live}});
            auto sequence=store.sequence();
            // Distinct immutable objects create reclaimable history and segment
            // fragmentation while the engine remains open.
            for(unsigned i=0;i<12;++i) {
                auto old=payload(96*1024,i+4);
                store.transact(store.sequence(),{{"head",old}});
            }
            store.transact(store.sequence(),{{"head",live}});
            const auto before=segment_bytes(repo);
            drain(store);
            CHECK(store.sequence()==sequence+13);
            CHECK(store.object(store.root("head"))==live);
            store.verify();
            CHECK(segment_bytes(repo)<before);
        }

        // A forged retirement record must not unlink a file that still has
        // indexed chunks, or name a segment beyond the durable tail.
        const auto marker_base=temp+"/marker-base";LevelStore::create(marker_base);
        {LevelStore store(marker_base);store.transact(0,{{"head",live}});store.verify();}
        for(const auto& [name,sid]:std::vector<std::pair<std::string,uint64_t>>{{"referenced-marker",1},{"future-marker",999}}){
            auto target=temp+"/"+name;std::filesystem::copy(marker_base,target,std::filesystem::copy_options::recursive);
            forge_retired_marker(target,sid);const auto protected_file=target+"/DATA-1.seg";
            const auto size=std::filesystem::file_size(protected_file);
            rejects([&]{LevelStore corrupted(target);corrupted.collect_step(65580,8);});
            CHECK(std::filesystem::exists(protected_file));CHECK(std::filesystem::file_size(protected_file)==size);
        }

        // A known preflight quota rejection must not poison the still-open
        // engine, even after the GC has retired unreachable object metadata.
        const auto quota_repo=temp+"/quota-preflight";LevelStore::create(quota_repo);
        {
            LevelStore store(quota_repo);store.transact(0,{{"head",live}});
            for(unsigned i=0;i<12;++i)store.transact(store.sequence(),{{"head",payload(96*1024,i+81)}});
            store.transact(store.sequence(),{{"head",live}});store.verify();
        }
        RepositoryBudget{segment_bytes(quota_repo)+48,0}.save(quota_repo);
        {
            LevelStore store(quota_repo);const auto live_id=store.root("head");const auto sequence=store.sequence();
            bool rejected_for_space=false;
            try{(void)store.collect_step(8*1024*1024,128);}
            catch(const std::system_error& error){rejected_for_space=error.code().value()==ENOSPC;}
            CHECK(rejected_for_space);
            store.barrier();CHECK(store.sequence()==sequence);
            CHECK(store.root("head")==live_id && store.object(live_id)==live);
            store.transact(store.sequence(),{{"alias",std::nullopt,live_id}});
            CHECK(store.root("alias")==live_id);store.verify();
        }

        // An explicit reader lifetime pin protects retired physical segments
        // through repeated GC steps; after release, a later pass can unlink.
        {
            LevelStore store(repo);
            auto pinned=store.pin_objects();
            auto old=payload(128*1024,99);
            store.transact(store.sequence(),{{"head",old}});
            CHECK(store.object(store.root("head"))==old); // Warm the cache before retirement.
            store.transact(store.sequence(),{{"head",live}});
            for(unsigned i=0;i<32;++i) {
                auto report=store.collect_step(1024*1024,128);
                CHECK(field(report,"bytes_copied")<=1024*1024);
                CHECK(field(report,"objects_deleted")==0);
                CHECK(boolean_field(report,"logical_sweep_skipped"));
                CHECK(store.object(Journal::object_id(old))==old);
                store.verify();
            }
            pinned.reset();
            drain(store);
            CHECK(store.object(store.root("head"))==live);
            rejects([&]{store.object(Journal::object_id(old));});
            rejects([&]{store.transact(store.sequence(),{{"dangling",std::nullopt,Journal::object_id(old)}});});
        }

        // A one object sweep may delete only one O row. Remaining unreachable
        // rows stay readable until their own bounded sweep, with valid chunks.
        {
            LevelStore store(repo);
            std::vector<std::vector<unsigned char>> stale;
            for(unsigned i=0;i<7;++i) {
                auto value=payload(32*1024,i+201);stale.push_back(value);
                store.transact(store.sequence(),{{"head",value}});
            }
            store.transact(store.sequence(),{{"head",live}});
            auto before_objects=store.objects();
            auto report=store.collect_step(1024*1024,1);
            CHECK(field(report,"objects_deleted")==1);
            CHECK(before_objects==store.objects()+1);store.verify();
            unsigned readable=0,missing=0;
            for(const auto& value:stale) {
                try { CHECK(store.object(Journal::object_id(value))==value);++readable; }
                catch(const std::exception&) { ++missing; }
            }
            CHECK(readable==stale.size()-1 && missing==1);
            drain(store);
        }

        // Replay mode intentionally retains all historical objects and leaves
        // its transaction sequence and exported recovery chain intact.
        const auto archive=temp+"/base.pack",delta=temp+"/delta.pack",restored=temp+"/restored";
        {
            LevelStore store(repo);
            auto base=store.backup(archive);
            auto newer=payload(64*1024,123);
            store.transact(store.sequence(),{{"head",newer}});
            auto before_objects=store.objects();
            auto before_sequence=store.sequence();
            auto report=store.collect_step(65580,8);
            CHECK(boolean_field(report,"logical_sweep_skipped"));
            CHECK(store.objects()==before_objects && store.sequence()==before_sequence);
            store.export_replay(base,delta);
            store.verify();
        }
        LevelStore::replay_archive(archive,restored);
        LevelStore::replay_archive(delta,restored);
        { LevelStore store(restored);store.verify();CHECK(store.object(store.root("head"))==payload(64*1024,123)); }

        // Namespace handles retain logical objects after unlink. This exercises
        // parent/index lifetimes while collection is advanced on the live engine.
        const auto fs_path=temp+"/fs",input=temp+"/input";
        Container::create(fs_path);
        {
            Container container(fs_path);Namespace fs(container);fs.initialize();
            auto bytes=payload(70000,44);
            fs.write("/held",bytes);
            auto handle=fs.open("/held",Namespace::Access::ReadWrite);
            fs.remove("/held",false);
            for(unsigned i=0;i<16;++i) {
                auto report=container.collect_step(128*1024,32);
                CHECK(field(report,"bytes_copied")<=128*1024);
                CHECK(fs.handle_read(handle,0,bytes.size())==bytes);
                container.verify();
            }
            fs.handle_write(handle,bytes.size(),{'!'});
            auto expected=bytes;expected.push_back('!');
            CHECK(fs.handle_read(handle,0,expected.size())==expected);
            fs.close(handle);

            auto alpha=payload(180000,51),beta=payload(170000,52);
            fs.write("/alpha",alpha);fs.write("/beta",beta);
            Namespace::Attributes attr;attr.modified_ns=123456789;
            fs.set_attributes("/alpha",attr);
            auto cursor=fs.open_directory("/");auto copy=cursor;
            // These versions are created after the cursor's manifest snapshot;
            // its targeted lease must retain the old directory closure while
            // allowing collection of unrelated unreachable objects.
            fs.replace_file("/alpha",payload(90000,53));fs.remove("/beta",false);
            fs.write("/gamma",payload(160000,54));
            for(unsigned i=0;i<8;++i)fs.replace_file("/gamma",payload(70000+i*1024,60+i));
            uint64_t cursor_pruned=0,cursor_reclaimed=0;
            for(unsigned i=0;i<8;++i) {
                auto report=container.collect_step(256*1024,32);
                CHECK(field(report,"objects_deleted")<=32);
                CHECK(!boolean_field(report,"logical_sweep_skipped"));
                cursor_pruned+=field(report,"objects_deleted");
                cursor_reclaimed+=field(report,"segments_deleted");
                container.verify();
            }
            CHECK(cursor_pruned>0 && cursor_reclaimed>0);
            auto first=fs.read_directory(cursor,1,true);
            CHECK(first.names==std::vector<std::string>{"alpha"} && !first.eof);
            auto copied=fs.read_directory(copy,16,true);
            CHECK(copied.eof && copied.names==std::vector<std::string>({"alpha","beta"}));
            CHECK(copied.entries.size()==2 && copied.entries[0].inode.size==alpha.size());
            CHECK(copied.entries[0].inode.modified_ns==123456789);
            CHECK(copied.entries[1].inode.size==beta.size());
            fs.rewind_directory(cursor);
            auto rewound=fs.read_directory(cursor,16,true);
            CHECK(rewound.names==copied.names && rewound.entries[0].inode.size==alpha.size());
            const auto before_reclaim=segment_bytes(fs_path);
            cursor={};
            fs.rewind_directory(copy);
            auto copy_still_valid=fs.read_directory(copy,16,true);
            CHECK(copy_still_valid.names==copied.names && copy_still_valid.entries[1].inode.size==beta.size());
            copy={};
            bool pruned=false,reclaimed=false;
            for(unsigned i=0;i<64;++i) {
                auto report=container.collect_step(256*1024,32);
                CHECK(field(report,"bytes_copied")<=256*1024);
                pruned|=field(report,"objects_deleted")>0;
                reclaimed|=field(report,"segments_deleted")>0;
                container.verify();
                if(pruned && reclaimed && field(report,"pending_segments")==0)break;
            }
            CHECK(pruned && reclaimed);
            CHECK(segment_bytes(fs_path)<before_reclaim);
            CHECK(fs.read("/alpha")==payload(90000,53));
            CHECK(fs.list("/")==std::vector<std::string>({"alpha","gamma"}));
            fs.verify();
        }
        std::filesystem::remove_all(temp);
        std::cout<<"bounded online collection, object pins, replay retention and live namespace handle passed\n";
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n';std::filesystem::remove_all(temp);return 1;
    }
}
