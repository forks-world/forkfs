#include "namespace.h"
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
using namespace forkfs;
void check(bool value){if(!value)throw std::runtime_error("parent index regression failed");}
template<class F> void rejects(F operation){try{operation();}catch(const std::exception&){return;}throw std::runtime_error("expected parent index rejection");}
void unindex_fixture(const std::string& path,bool legacy,const std::string& id) {
    auto edit=[&](Journal& journal){auto pre=std::string("\0forkfs/",8);auto value=journal.get(pre+"inode/"+id);value[7]='1';
        journal.transact(journal.sequence(),{{pre+"parent-index",std::nullopt},{pre+"parent/"+id,std::nullopt},{pre+"inode/"+id,value}});
    };
    if(!legacy){Journal journal(path);edit(journal);return;}
    int fd=open(path.c_str(),O_RDWR|O_CLOEXEC);check(fd>=0);
    try{std::array<unsigned char,16> repository{};check(pread(fd,repository.data(),repository.size(),16)==static_cast<ssize_t>(repository.size()));
        Journal journal(fd,repository);edit(journal);
    }catch(...){close(fd);throw;}close(fd);
}
int main(){
    auto pattern=(std::filesystem::temp_directory_path()/"ff-parent-index-XXXXXX").string();auto temp=mkdtemp(pattern.data());if(!temp)return 1;
    auto path=std::string(temp)+"/new",old=std::string(temp)+"/old";
    try{
        Container::create(path);std::string p,nested,root;
        {
            Container store(path);Namespace fs(store);fs.initialize();root=fs.root_inode().id;check(fs.has_parent_index() && fs.parent_inode(root).id==root && fs.root_inode().indexed_directory);
            fs.mkdir("/p");p=fs.stat("/p").id;auto n=fs.create_child(p,"nested",true,0755);nested=n.id;
            auto q=fs.create_child(root,"q",true,0755);fs.write("/file",{1});fs.link("/file","/alias");fs.symlink("p","/link");
            rejects([&]{fs.parent_inode(fs.stat("/file").id);});check(fs.parent_inode(nested).id==p);
            fs.snapshot("base");fs.fork("base","work");Namespace revision(store,"base",true),work(store,"work");
            fs.rename_child(p,"nested",q.id,"moved",nested);check(fs.parent_inode(nested).id==q.id);
            check(revision.parent_inode(nested).id==p && work.parent_inode(nested).id==p);
            fs.rename("/q/moved","/q/renamed");check(fs.parent_inode(nested).id==q.id); // Same-parent rename updates backlink name.
            auto stable=store.status();rejects([&]{fs.rename_child(root,"q",nested,"cycle",q.id);});check(store.status()==stable);
            auto empty=fs.create_child(p,"empty",true,0755);fs.rename_child(q.id,"renamed",p,"empty",nested,empty.id);
            rejects([&]{fs.parent_inode(empty.id);});check(fs.parent_inode(nested).id==p);fs.verify();
            fs.remove_child(p,"empty",nested,true);rejects([&]{fs.parent_inode(nested);});fs.verify();fs.compact();store.verify();
            fs.restore("base");check(fs.parent_inode(nested).id==p);stable=store.status();fs.build_parent_index();check(store.status()==stable);
        }
        {Container store(path);Namespace fs(store),revision(store,"base",true);check(fs.parent_inode(nested).id==p && revision.parent_inode(nested).id==p);store.verify();}
        // Historical flat initialization without an index marker. Later writes use old-view semantics.
        Container::create(old);std::string oldroot;
        {Container store(old);Namespace fs(store);fs.initialize();oldroot=fs.root_inode().id;}
        {Journal journal(old);auto pre=std::string("\0forkfs/",8);auto value=journal.get(pre+"inode/"+oldroot);check(value[7]=='2');value[7]='1';journal.transact(journal.sequence(),{{pre+"parent-index",std::nullopt},{pre+"parent/"+oldroot,std::nullopt},{pre+"inode/"+oldroot,value}});}
        {
            Container store(old);Namespace fs(store);check(!fs.has_parent_index() && !fs.root_inode().indexed_directory);fs.mkdir("/a");fs.mkdir("/a/b");auto a=fs.stat("/a").id,b=fs.stat("/a/b").id;
            fs.snapshot("before");fs.fork("before","work");Namespace before(store,"before",true),work(store,"work");
            auto stable=store.status();check(before.parent_inode(b).id==a && store.status()==stable);rejects([&]{before.build_parent_index();});
            fs.build_parent_index();check(fs.has_parent_index() && !before.has_parent_index() && !work.has_parent_index());check(fs.stat_inode(b).indexed_directory && !before.stat_inode(b).indexed_directory);
            fs.rename("/a/b","/b");check(fs.parent_inode(b).id==oldroot && before.parent_inode(b).id==a);
            work.build_parent_index();check(work.has_parent_index() && work.parent_inode(b).id==a);
            fs.verify();before.verify();work.verify();fs.compact();store.verify();fs.restore("before");check(!fs.has_parent_index());fs.build_parent_index();fs.verify();
        }
        {Container store(old);Namespace fs(store),before(store,"before",true);check(fs.has_parent_index() && !before.has_parent_index());store.verify();}
        // The same directory count has different admission limits by backend.
        for(bool legacy : {false,true})for(unsigned count : {2u,64u}) {
            auto upgrade=std::string(temp)+(legacy?"/legacy-":"/leveldb-")+std::to_string(count);
            if(legacy)Container::create_legacy(upgrade);else Container::create(upgrade);
            std::string id;{Container store(upgrade);Namespace fs(store);fs.initialize();id=fs.root_inode().id;}
            unindex_fixture(upgrade,legacy,id);
            Container store(upgrade);Namespace fs(store);
            for(unsigned i=0;i<count;++i)fs.mkdir("/d"+std::to_string(i));
            fs.snapshot("before");Namespace before(store,"before",true);auto stable=store.status();
            if(legacy && count==64) {
                bool overflow=false;try{fs.build_parent_index();}catch(const std::system_error& e){overflow=e.code().value()==E2BIG && std::string(e.what()).find("object-journal 64-mutation limit")!=std::string::npos;}
                check(overflow && store.status()==stable && !fs.has_parent_index() && !fs.root_inode().indexed_directory);
                check(!fs.stat("/d63").indexed_directory && !before.has_parent_index());
                check(fs.parent_inode(fs.stat("/d63").id).id==id); // Original fallback remains usable.
            }else{fs.build_parent_index();check(fs.has_parent_index() && fs.root_inode().indexed_directory && !before.has_parent_index());}
            fs.verify();before.verify();store.verify();
        }
        auto bad=std::string(temp)+"/bad";Container::create(bad);std::string badroot;
        {Container store(bad);Namespace fs(store);fs.initialize();badroot=fs.root_inode().id;}
        {Journal journal(bad);journal.put(std::string("\0forkfs/",8)+"parent/"+badroot,{'b','a','d'});}
        {Container store(bad);Namespace fs(store);auto stable=store.status();rejects([&]{fs.parent_inode(badroot);});rejects([&]{fs.verify();});check(store.status()==stable);}
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(temp);return 1;}
    std::filesystem::remove_all(temp);return 0;
}
