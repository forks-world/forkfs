#include "namespace.h"
#include "metadata_view.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("timestamp check failed at line "+std::to_string(line));}
template<class F> static void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
// Publish exact inode bytes independently of the namespace encoder. This tests
// old-format reads and diff's timestamp comparisons with unchanged ctime/mtime.
static void publish(const std::string& path,bool legacy,bool access_only=false){
    forkfs::Journal j(path);forkfs::NamespaceView view(j,"main");const std::string h("\0forkfs-history/",16),p("\0forkfs/",8);
    std::map<std::string,Bytes> objects;forkfs::NamespaceView::Changes delta;
    auto add=[&](const Bytes& b){auto hash=forkfs::Journal::object_id(b);std::string key=h+"object/";const char* hex="0123456789abcdef";for(auto c:hash){key+=hex[c>>4];key+=hex[c&15];}objects.emplace(key,b);};
    for(const auto& key:view.keys(p+"inode/")){
        auto b=view.get(key);if(legacy){b.resize(48);memcpy(b.data(),"FFINOD01",8);}else{check(b.size()==64);b[access_only?48:56]^=1;}
        add(b);delta.emplace(key,forkfs::Journal::object_id(b));
    }
    auto plan=view.plan(delta);for(const auto& b:plan.objects)add(b);
    std::vector<forkfs::Mutation> batch;for(auto& [key,b]:objects)batch.push_back({key,std::move(b)});batch.push_back({h+"world/main",std::move(plan.manifest)});j.transact(j.sequence(),batch);
}
int main(){
    char pattern[]="/tmp/ff-times-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;auto path=std::string(tmp)+"/db";
    try{
        Container::create(path);forkfs::InodeInfo original;
        {Container store(path);Namespace fs(store);fs.initialize();fs.write("/f",Bytes{'A'});original=fs.stat("/f");check(original.accessed_ns>0 && original.birth_ns>0 && original.accessed_ns<=original.modified_ns && original.birth_ns<=original.changed_ns);}
        publish(path,true);
        {
            Container store(path);Namespace fs(store);auto n=fs.stat("/f");check(n.accessed_ns==n.modified_ns && n.birth_ns==n.changed_ns);fs.snapshot("old");
            Namespace::Attributes a;a.accessed_ns=0;a.birth_ns=42;a.modified_ns=123;a.mode=0700;a.size=2;fs.set_attributes("/f",a);
            n=fs.stat("/f");check(n.accessed_ns==0 && n.birth_ns==42 && n.modified_ns==123 && n.mode==0700 && n.size==2);
            fs.link("/f","/alias");check(fs.stat("/alias").birth_ns==42);fs.snapshot("base");fs.fork("base","work");
            auto stable=store.status();fs.read("/f");fs.stat("/f");check(store.status()==stable && fs.stat("/f").accessed_ns==0);
            a={};a.accessed_ns=uint64_t(INT64_MAX)+1;rejects([&]{fs.set_attributes("/f",a);});check(store.status()==stable);
            a={};a.birth_ns=uint64_t(INT64_MAX)+1;a.mode=0777;rejects([&]{fs.set_attributes("/f",a);});check(store.status()==stable);
            Namespace old(store,"old",true),base(store,"base",true),work(store,"work");check(old.stat("/f").birth_ns==original.changed_ns);
            a={};a.accessed_ns=456;a.birth_ns=789;work.set_attributes("/f",a);check(base.stat("/f").accessed_ns==0 && work.stat("/f").birth_ns==789);rejects([&]{base.set_attributes("/f",a);});
            auto handle=fs.open("/f");fs.remove("/f",false);fs.remove("/alias",false);fs.handle_set_attributes(handle,a);check(fs.handle_stat(handle).birth_ns==789 && fs.handle_stat(handle).links==0);fs.close(handle);
            fs.restore("base","safety");check(fs.stat("/f").accessed_ns==0 && fs.stat("/f").birth_ns==42);fs.snapshot("before");fs.verify();store.verify();
        }
        publish(path,false,true);
        {Container store(path);Namespace fs(store);fs.snapshot("atime");auto diff=fs.diff("before","atime");check(diff.find("\"metadata_changed\":true")!=diff.npos && diff.find("\"content_changed\":true")==diff.npos);}
        publish(path,false);
        {Container store(path);Namespace fs(store);fs.snapshot("birth");auto diff=fs.diff("atime","birth");check(diff.find("\"metadata_changed\":true")!=diff.npos && diff.find("\"content_changed\":true")==diff.npos);store.collect_garbage();fs.verify();Namespace(store,"old",true).verify();}
        {Container store(path);Namespace fs(store);check(fs.stat("/f").accessed_ns==1 && fs.stat("/f").birth_ns==43);fs.verify();store.verify();}
        std::filesystem::remove_all(tmp);std::cout<<"timestamp v1 migration, v2 persistence, metadata diff, snapshots, handles, restore, noatime reads and GC passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
