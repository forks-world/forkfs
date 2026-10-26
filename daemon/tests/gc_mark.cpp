#include "gc_mark.h"
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace forkfs;
using Id=GcMark::Id;
using Bytes=GcMark::Bytes;
using Objects=std::map<Id,Bytes>;

#define CHECK(x) do { if(!(x)) throw std::runtime_error("line "+std::to_string(__LINE__)+": " #x); } while(0)

struct TempDir {
    std::string path;
    TempDir(){char value[]="/tmp/forkfs-gc-mark-XXXXXX";char* p=::mkdtemp(value);if(!p)throw std::runtime_error("mkdtemp failed");path=p;}
    ~TempDir(){std::error_code ec;std::filesystem::remove_all(path,ec);}
};

static void put(Bytes& b,size_t at,uint64_t value,unsigned width){
    CHECK(at<=b.size()&&b.size()-at>=width);
    for(unsigned i=0;i<width;++i)b[at+i]=static_cast<unsigned char>(value>>(8*i));
}
static Id add(Objects& objects,Bytes bytes){auto id=Journal::object_id(bytes);objects.emplace(id,std::move(bytes));return id;}
static Id zero_id(){return {};}
static Bytes inode(const char* magic,uint32_t type,uint64_t links,uint64_t size,uint32_t flags=0){
    bool extended=!memcmp(magic,"FFINOD03",8)||!memcmp(magic,"FFINOD04",8)||!memcmp(magic,"FFINOD05",8);
    bool xattrs=!memcmp(magic,"FFINOD04",8)||!memcmp(magic,"FFINOD05",8);
    Bytes b(xattrs?84:extended?80:48);memcpy(b.data(),magic,8);put(b,8,type,4);put(b,12,0644,4);
    put(b,16,links,8);put(b,24,size,8);if(extended)put(b,48,flags,4);if(xattrs)put(b,80,0,4);return b;
}
static Bytes descriptor(uint64_t size,const Id& root){Bytes b(48);memcpy(b.data(),"FFFILE01",8);put(b,8,size,8);memcpy(b.data()+16,root.data(),32);return b;}
static Bytes extent_node(unsigned level,const std::vector<std::pair<unsigned,Id>>& children){
    Bytes b(11+33*children.size());memcpy(b.data(),"FFEXT001",8);b[8]=static_cast<unsigned char>(level);put(b,9,children.size(),2);
    size_t at=11;for(const auto& [slot,id]:children){b[at++]=static_cast<unsigned char>(slot);memcpy(b.data()+at,id.data(),32);at+=32;}return b;
}
static void full_entry(Bytes& run,const std::string& key,const Id& id){
    auto at=run.size();run.resize(at+4+key.size()+32);put(run,at,key.size(),4);memcpy(run.data()+at+4,key.data(),key.size());memcpy(run.data()+at+4+key.size(),id.data(),32);
}
static void delta_entry(Bytes& run,uint32_t op,const std::string& key,const Id& id){
    auto at=run.size();run.resize(at+8+key.size()+32);put(run,at,op,4);put(run,at+4,key.size(),4);memcpy(run.data()+at+8,key.data(),key.size());memcpy(run.data()+at+8+key.size(),id.data(),32);
}
static Bytes full_run(const std::map<std::string,Id>& entries){
    Bytes b(12);memcpy(b.data(),"FFMAN001",8);put(b,8,entries.size(),4);for(const auto& [key,id]:entries)full_entry(b,key,id);return b;
}
static Bytes delta_run(const std::map<std::string,std::pair<uint32_t,Id>>& entries){
    Bytes b(12);memcpy(b.data(),"FFRUN001",8);put(b,8,entries.size(),4);for(const auto& [key,value]:entries)delta_entry(b,value.first,key,value.second);return b;
}
static Bytes manifest(const std::vector<Id>& runs){
    Bytes b(12+32*runs.size());memcpy(b.data(),"FFMAN002",8);put(b,8,runs.size(),4);for(size_t i=0;i<runs.size();++i)memcpy(b.data()+12+32*i,runs[i].data(),32);return b;
}
static std::string root_inode_key(const std::string& id){return std::string("R/\0forkfs/inode/",16)+id;}
static std::string root_content_key(const std::string& id){return std::string("R/\0forkfs/content/",18)+id;}
static std::string inode_key(const std::string& id){return std::string("\0forkfs/inode/",14)+id;}
static std::string content_key(const std::string& id){return std::string("\0forkfs/content/",16)+id;}
static GcMark::Reader reader_for(const Objects& objects){return [&objects](const Id& id){auto it=objects.find(id);if(it==objects.end())throw std::runtime_error("fixture object missing");return it->second;};}
static void drain(GcMark& marker,const Objects& objects){size_t units=0;auto read=reader_for(objects);while(marker.has_work()){CHECK(marker.advance(1,read)==1);CHECK(++units<100000);}}
template<class F> static void rejects(F&& f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}CHECK(caught);}

static std::pair<Id,std::vector<Id>> extent_tree(Objects& objects,const std::vector<unsigned>& leaf_slots){
    Bytes block(65536);block[0]=1;std::vector<Id> data;std::vector<std::pair<unsigned,Id>> leaves;
    for(auto slot:leaf_slots){Bytes b=block;b[0]=static_cast<unsigned char>(slot+1);auto id=add(objects,std::move(b));data.push_back(id);leaves.emplace_back(slot,id);}
    auto node=add(objects,extent_node(0,leaves));std::vector<Id> nodes{node};
    for(unsigned level=1;level<=5;++level){node=add(objects,extent_node(level,{{0,node}}));nodes.push_back(node);}
    return {node,[&]{std::vector<Id> ids=nodes;ids.insert(ids.end(),data.begin(),data.end());return ids;}()};
}

static void test_sparse_extent_and_zero_root(){
    TempDir dir;Objects objects;auto [root,reachable]=extent_tree(objects,{0,1});
    // A two block file uses one allocated block and a seven byte partial tail.
    auto inode_id=add(objects,inode("FFINOD05",1,1,65543,2));auto content=add(objects,descriptor(65543,root));
    const std::string name(32,'a'),ik=root_inode_key(name),ck=root_content_key(name);
    std::map<std::string,Id> roots{{ik,inode_id},{ck,content}};GcMark marker(dir.path);
    marker.seed_root(ik,inode_id,[&](const std::string& key)->std::optional<Id>{auto it=roots.find(key);if(it==roots.end())return {};return it->second;});
    auto garbage=add(objects,Bytes{'g','a','r','b','a','g','e'});drain(marker,objects);
    CHECK(marker.live(inode_id)&&marker.live(content)&&marker.live(root));for(const auto& id:reachable)CHECK(marker.live(id));CHECK(!marker.live(garbage));

    TempDir holes;Objects hole_objects;auto hole_inode=add(hole_objects,inode("FFINOD05",1,1,100000,2));auto hole_content=add(hole_objects,descriptor(100000,zero_id()));
    const std::string hole_name(32,'b'),hik=root_inode_key(hole_name),hck=root_content_key(hole_name);
    std::map<std::string,Id> hole_roots{{hik,hole_inode},{hck,hole_content}};GcMark all_holes(holes.path);
    all_holes.seed_root(hik,hole_inode,[&](const std::string& key)->std::optional<Id>{auto it=hole_roots.find(key);if(it==hole_roots.end())return {};return it->second;});drain(all_holes,hole_objects);
    CHECK(all_holes.live(hole_inode)&&all_holes.live(hole_content));CHECK(!all_holes.has_work());
}

static void test_extent_out_of_eof_rejected(){
    TempDir dir;Objects objects;Bytes block(65536,1);auto data=add(objects,block);auto leaf=add(objects,extent_node(0,{{1,data}}));Id root=leaf;
    for(unsigned level=1;level<=5;++level)root=add(objects,extent_node(level,{{0,root}}));
    auto inode_id=add(objects,inode("FFINOD05",1,1,1,2));auto content=add(objects,descriptor(1,root));const std::string name(32,'c'),ik=root_inode_key(name),ck=root_content_key(name);
    std::map<std::string,Id> roots{{ik,inode_id},{ck,content}};GcMark marker(dir.path);
    marker.seed_root(ik,inode_id,[&](const std::string& key)->std::optional<Id>{auto it=roots.find(key);if(it==roots.end())return {};return it->second;});
    rejects([&]{auto read=reader_for(objects);size_t units=0;while(marker.has_work()){marker.advance(1,read);CHECK(++units<1000);}});
}

static void test_raw_content_magic_is_opaque(){
    TempDir dir;Objects objects;auto inode_id=add(objects,inode("FFINOD01",1,1,48));Bytes raw(48);memcpy(raw.data(),"FFFILE01",8);put(raw,8,0,8);auto content=add(objects,raw);
    const std::string name(32,'d'),ik=root_inode_key(name),ck=root_content_key(name);std::map<std::string,Id> roots{{ik,inode_id},{ck,content}};GcMark marker(dir.path);
    marker.seed_root(ik,inode_id,[&](const std::string& key)->std::optional<Id>{auto it=roots.find(key);if(it==roots.end())return {};return it->second;});drain(marker,objects);CHECK(marker.live(inode_id)&&marker.live(content));
}

static void test_effective_overlay_keeps_shadowed_put_opaque(){
    TempDir dir;Objects objects;const std::string suffix(32,'e'),ik=inode_key(suffix),ck=content_key(suffix),rk("\0forkfs/root",12);
    auto shadow=add(objects,Bytes{'n','o','t','-','a','n','-','i','n','o','d','e'});auto winner=add(objects,inode("FFINOD01",1,1,0));auto raw=add(objects,{});auto root_obj=add(objects,Bytes{'r'});
    auto old=add(objects,full_run({{ck,raw},{ik,shadow},{rk,root_obj}}));auto newer=add(objects,delta_run({{ck,{1,raw}},{ik,{1,winner}}}));auto world=add(objects,manifest({newer,old}));
    GcMark marker(dir.path);marker.seed_manifest(world);drain(marker,objects);CHECK(marker.live(shadow)&&marker.live(winner)&&marker.live(raw));
    CHECK(marker.live(root_obj));
}

static void test_invalid_run_operation_rejected(){
    TempDir dir;Objects objects;const std::string rk("\0forkfs/root",12);auto payload=add(objects,Bytes{'r'});
    Bytes bad(12+8+rk.size()+32);memcpy(bad.data(),"FFRUN001",8);put(bad,8,1,4);put(bad,12,3,4);put(bad,16,rk.size(),4);memcpy(bad.data()+20,rk.data(),rk.size());memcpy(bad.data()+20+rk.size(),payload.data(),32);auto run=add(objects,bad);auto world=add(objects,manifest({run}));
    GcMark marker(dir.path);marker.seed_manifest(world);rejects([&]{auto read=reader_for(objects);size_t units=0;while(marker.has_work()){marker.advance(1,read);CHECK(++units<1000);}});
}

static void test_content_only_orphan_and_inode_roots(){
    TempDir direct_dir;Objects direct_objects;auto [direct_extent,direct_reachable]=extent_tree(direct_objects,{0});auto direct_inode=add(direct_objects,inode("FFINOD05",1,1,1,2));auto direct_content=add(direct_objects,descriptor(1,direct_extent));const std::string direct_name(32,'f'),direct_ik=root_inode_key(direct_name),direct_ck=root_content_key(direct_name);std::map<std::string,Id> direct_roots{{direct_ik,direct_inode},{direct_ck,direct_content}};GcMark direct(direct_dir.path);
    direct.seed_root(direct_ck,direct_content,[&](const std::string& key)->std::optional<Id>{auto it=direct_roots.find(key);if(it==direct_roots.end())return {};return it->second;});drain(direct,direct_objects);CHECK(direct.live(direct_inode)&&direct.live(direct_content));for(const auto& id:direct_reachable)CHECK(direct.live(id));

    TempDir dir;Objects objects;auto [root,reachable]=extent_tree(objects,{0});auto orphan_inode=add(objects,inode("FFINOD05",1,0,1,2));auto orphan_content=add(objects,descriptor(1,root));
    std::string base=std::string("R/\0forkfs-history/orphan/demo/",30)+std::string(32,'f');auto oi=base+"/inode",oc=base+"/content";
    GcMark marker(dir.path);marker.seed_root(oc,orphan_content,[&](const std::string& key)->std::optional<Id>{if(key==oc)return orphan_content;if(key==oi)return orphan_inode;return {};});drain(marker,objects);
    CHECK(marker.live(orphan_inode)&&marker.live(orphan_content)&&marker.live(root));for(const auto& id:reachable)CHECK(marker.live(id));

    TempDir dir_only;Objects dirs;auto directory=add(dirs,inode("FFINOD02",2,2,0));auto dk=root_inode_key(std::string(32,'1'));GcMark directory_marker(dir_only.path);
    directory_marker.seed_root(dk,directory,[&](const std::string& key)->std::optional<Id>{return key==dk?std::optional<Id>(directory):std::nullopt;});drain(directory_marker,dirs);CHECK(directory_marker.live(directory));

    TempDir symlink_dir;Objects symlinks;auto symlink=add(symlinks,inode("FFINOD01",3,1,3));auto target=add(symlinks,Bytes{'a','b','c'});auto sk=root_inode_key(std::string(32,'2')),sck=root_content_key(std::string(32,'2'));GcMark symlink_marker(symlink_dir.path);
    symlink_marker.seed_root(sk,symlink,[&](const std::string& key)->std::optional<Id>{if(key==sk)return symlink;if(key==sck)return target;return {};});drain(symlink_marker,symlinks);CHECK(symlink_marker.live(symlink)&&symlink_marker.live(target));
}

int main(){try{
    test_sparse_extent_and_zero_root();test_extent_out_of_eof_rejected();test_raw_content_magic_is_opaque();
    test_effective_overlay_keeps_shadowed_put_opaque();test_invalid_run_operation_rejected();test_content_only_orphan_and_inode_roots();
    std::cout<<"incremental GC marker roots, overlays, sparse extents and corruption checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
