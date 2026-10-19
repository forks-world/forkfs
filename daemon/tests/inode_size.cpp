#include "namespace.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace forkfs;
using Bytes=std::vector<unsigned char>;
void check(bool ok){if(!ok)throw std::runtime_error("inode size regression failed");}
template<class F>void error(int code,F f){try{f();}catch(const std::system_error& e){check(e.code().value()==code);return;}throw std::runtime_error("expected inode size error");}
int main(){
    auto pattern=(std::filesystem::temp_directory_path()/"ff-inode-size-XXXXXX").string();auto tmp=mkdtemp(pattern.data());if(!tmp)return 1;
    try{for(bool legacy:{false,true}){
        auto path=std::string(tmp)+(legacy?"/legacy":"/leveldb");if(legacy)Container::create_legacy(path);else Container::create(path);std::string id;
        {Container store(path);Namespace fs(store);fs.initialize();auto root=fs.root_inode().id;
            id=fs.create_child(root,"file",false,0644).id;fs.write("/file",Bytes{'A','B','C'});fs.link_child(id,root,"alias");
            auto sym=fs.symlink_child(root,"sym","file");fs.snapshot("base");fs.fork("base","work");Namespace revision(store,"base",true),work(store,"work");
            check(!fs.stat_inode(id).extended_attributes && fs.stat_inode(id).uid==0 && fs.stat_inode(id).created_ns==0);
            Namespace::Attributes extra;extra.uid=501;extra.gid=20;extra.accessed_ns=111;extra.created_ns=222;
            fs.set_inode_attributes(id,extra);auto owned=fs.stat("/alias");
            check(owned.extended_attributes && owned.uid==501 && owned.gid==20 && owned.accessed_ns==111 && owned.created_ns==222);
            check(revision.stat_inode(id).uid==0 && !work.stat_inode(id).extended_attributes);
            fs.set_inode_attributes(root,extra);check(fs.parent_inode(root).id==root && fs.root_inode().indexed_directory && fs.root_inode().uid==501);
            fs.set_inode_attributes(sym.id,extra);check(fs.readlink_inode(sym.id)=="file" && fs.stat_inode(sym.id).uid==501);
            auto unchanged=store.status();Namespace::Attributes invalid;invalid.mode=0777;invalid.uid=99;invalid.accessed_ns=UINT64_MAX;
            error(EINVAL,[&]{fs.set_inode_attributes(id,invalid);});invalid.accessed_ns=1;invalid.created_ns=UINT64_MAX;
            error(EINVAL,[&]{fs.set_inode_attributes(id,invalid);});error(EROFS,[&]{revision.set_inode_attributes(id,extra);});check(store.status()==unchanged);
            fs.set_inode_attributes(id,{0600,123,5});check(fs.read("/alias")==Bytes({'A','B','C',0,0}));
            auto n=fs.stat_inode(id);check(n.size==5 && n.mode==0600 && n.modified_ns==123 && n.links==2);
            check(n.uid==501 && n.gid==20 && n.accessed_ns==111 && n.created_ns==222);
            check(revision.read_inode(id,0,9)==Bytes({'A','B','C'}) && work.stat_inode(id).size==3);
            auto stable=store.status();fs.set_inode_attributes(id,{{},{},5});check(store.status()==stable);
            error(EFBIG,[&]{fs.set_inode_attributes(id,{0777,999,UINT64_MAX});});error(EFBIG,[&]{fs.set_inode_attributes(id,{{},{},UINT64_MAX});});
            error(EISDIR,[&]{fs.set_inode_attributes(root,{0777,999,0});});error(EINVAL,[&]{fs.set_inode_attributes(sym.id,{0600,999,0});});
            error(EROFS,[&]{revision.set_inode_attributes(id,{{},{},0});});check(store.status()==stable);
            fs.set_attributes("/alias",{0640,456,1});check(fs.read_inode(id,0,9)==Bytes{'A'} && fs.stat_inode(id).modified_ns==456);
            fs.set_inode_attributes(id,{{},{},256*1024});auto data=fs.read_inode(id,0,UINT64_MAX);check(data.size()==256*1024 && data[0]=='A');
            check(std::all_of(data.begin()+1,data.end(),[](auto c){return c==0;}));fs.set_inode_attributes(id,{{},{},0});check(fs.read("/file").empty());
            if(!legacy){auto h=fs.open_inode(id,Namespace::Access::ReadWrite),r=fs.open_inode(id);stable=store.status();
                error(EBADF,[&]{fs.handle_set_attributes(r,{0600,999,1});});check(store.status()==stable);
                fs.handle_set_attributes(h,{0600,789,2});check(fs.handle_read(r,0,9)==Bytes({0,0}));
                fs.remove_child(root,"file",id,false);fs.remove_child(root,"alias",id,false);
                fs.handle_set_attributes(h,{0644,987,4});check(fs.handle_stat(h).links==0 && fs.handle_stat(h).size==4 && fs.handle_stat(h).modified_ns==987);
                check(fs.handle_stat(h).uid==501 && fs.handle_stat(h).created_ns==222);
                Namespace::Attributes owner;owner.uid=0;owner.gid=0;owner.accessed_ns=0;owner.created_ns=0;fs.handle_set_attributes(h,owner);
                check(fs.handle_stat(r).uid==0 && fs.handle_stat(r).gid==0 && fs.handle_stat(r).accessed_ns==0 && fs.handle_stat(r).created_ns==0);
                check(fs.handle_read(r,0,9)==Bytes({0,0,0,0}));stable=store.status();
                error(ENOENT,[&]{fs.set_inode_attributes(id,{{},{},1});});error(EFBIG,[&]{fs.handle_set_attributes(h,{0600,1,UINT64_MAX});});check(store.status()==stable);
                fs.close(h);fs.close(r);
            }
            fs.verify();revision.verify();work.verify();store.verify();
        }
        {Container store(path);Namespace fs(store),revision(store,"base",true);check(revision.stat_inode(id).size==3);check(fs.root_inode().uid==501 && fs.root_inode().created_ns==222 && fs.root_inode().indexed_directory);fs.verify();store.verify();}
    }}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
    std::filesystem::remove_all(tmp);return 0;
}
