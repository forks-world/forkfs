#include "namespace.h"
#include "metadata_view.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static void check(bool ok){if(!ok)throw std::runtime_error("restore check failed");}
template<class F> static void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
static void invalid_target(const std::string& path){
    Container::create(path);std::string inode;
    {Container store(path);Namespace fs(store);fs.initialize();fs.write("/f",Bytes{'G'});inode=fs.stat("/f").id;fs.snapshot("good");}
    // Build a physically valid revision tree missing required file content,
    // leaving the current World intact. Restore must validate the target first.
    {forkfs::Journal journal(path);forkfs::NamespaceView view(journal,"main");const std::string h("\0forkfs-history/",16),p("\0forkfs/",8);
        auto plan=view.plan({{p+"content/"+inode,std::nullopt}});std::map<std::string,Bytes> objects;
        auto add=[&](const Bytes& value){auto hash=forkfs::Journal::object_id(value);std::string key=h+"object/";const char* hex="0123456789abcdef";for(auto c:hash){key+=hex[c>>4];key+=hex[c&15];}objects.emplace(key,value);};
        for(const auto& object:plan.objects)add(object);add(plan.manifest);Bytes descriptor(80);memcpy(descriptor.data(),"FFREV001",8);
        auto manifest=forkfs::Journal::object_id(plan.manifest),parent=journal.root_object(h+"revision/good");std::copy(manifest.begin(),manifest.end(),descriptor.begin()+8);std::copy(parent.begin(),parent.end(),descriptor.begin()+40);
        auto sequence=journal.sequence();for(unsigned i=0;i<8;++i)descriptor[72+i]=static_cast<unsigned char>(sequence>>(8*i));
        std::vector<forkfs::Mutation> batch;for(auto& [key,value]:objects)batch.push_back({key,std::move(value)});batch.push_back({h+"revision/broken",std::move(descriptor)});journal.transact(sequence,batch);
    }
    {Container store(path);Namespace fs(store);auto before=store.status();rejects([&]{fs.restore("broken","unused");});check(store.status()==before && fs.read("/f")==Bytes{'G'});auto names=fs.revisions();check(std::find(names.begin(),names.end(),"unused")==names.end());fs.verify();store.verify();}
}
int main(){
    char pattern[]="/tmp/ff-restore-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;auto path=std::string(tmp)+"/db";
    try {
        invalid_target(path+"-invalid");Container::create(path);
        {
            Container store(path);Namespace fs(store);fs.initialize();fs.snapshot("empty");fs.restore("empty","flat-safety"); // flat bootstrap
            fs.write("/f",Bytes{'A'});fs.link("/f","/alias");fs.set_xattr("/f","user.a",Bytes{0,0xff});fs.mkdir("/dir");fs.symlink("/f","/sym");
            fs.truncate("/f",2*1024*1024);fs.write_at("/f",1024*1024-1,Bytes{'Z'});fs.snapshot("base");fs.fork("base","other");Namespace other(store,"other"),old(store,"base",true);
            fs.write_at("/f",0,Bytes{'B'});fs.set_xattr("/f","user.a",Bytes{'B'});fs.write("/later",Bytes{'L'});fs.snapshot("dirty");fs.write_at("/f",0,Bytes{'C'});
            auto stable=store.status();auto handle=fs.open("/f");rejects([&]{fs.restore("base","blocked");});check(store.status()==stable);fs.close(handle);
            auto writable=fs.open("/later",Namespace::Access::Write);fs.remove("/later",false);stable=store.status();rejects([&]{fs.restore("base","blocked");});check(store.status()==stable);fs.close(writable);
            fs.write("/later",Bytes{'L'});auto cursor=fs.open_directory("/");
            auto revision_handle=old.open("/f"),foreign=other.open("/f");fs.restore("base","safety");
            check(fs.read_at("/f",0,1)==Bytes{'A'} && fs.stat("/f").size==2*1024*1024 && fs.stat("/f").id==fs.stat("/alias").id);
            check(fs.get_xattr("/alias","user.a")==Bytes({0,0xff}) && fs.readlink("/sym")=="/f");rejects([&]{fs.stat("/later");});
            auto page=fs.read_directory(cursor,64);check(page.eof && std::find(page.names.begin(),page.names.end(),"later")!=page.names.end());cursor={};
            check(old.handle_read(revision_handle,0,1)==Bytes{'A'} && other.handle_read(foreign,0,1)==Bytes{'A'});old.close(revision_handle);other.close(foreign);
            Namespace safety(store,"safety",true);check(safety.read_at("/f",0,1)==Bytes{'C'} && safety.read("/later")==Bytes{'L'} && safety.get_xattr("/f","user.a")==Bytes{'B'});
            fs.snapshot("restored");auto changes=fs.diff("base","restored");check(changes.find("\"changes\":[]")!=changes.npos);
            stable=store.status();rejects([&]{fs.restore("missing","unused");});rejects([&]{fs.restore("base","safety");});rejects([&]{old.restore("base","unused");});check(store.status()==stable);
            fs.restore("safety","undo-safety");check(fs.read_at("/f",0,1)==Bytes{'C'} && fs.read("/later")==Bytes{'L'});fs.verify();safety.verify();old.verify();other.verify();store.collect_garbage();
        }
        {Container store(path);Namespace fs(store),old(store,"base",true);check(fs.read_at("/f",0,1)==Bytes{'C'} && old.read_at("/f",0,1)==Bytes{'A'});fs.verify();old.verify();store.verify();}
        std::filesystem::remove_all(tmp);std::cout<<"restore safety snapshots, closed-handle guards, immutable cursors, metadata, undo, target validation, restart and GC passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
