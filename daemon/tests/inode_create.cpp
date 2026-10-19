#include "namespace.h"
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace forkfs;
#define CHECK(x) do{if(!(x))throw std::runtime_error("inode create failure at "+std::to_string(__LINE__));}while(0)
template<class F>void error(int code,F fn){try{fn();}catch(const std::system_error& e){CHECK(e.code().value()==code);return;}throw std::runtime_error("expected error");}
int main(){auto pattern=(std::filesystem::temp_directory_path()/"ff-create-XXXXXX").string();auto dir=mkdtemp(pattern.data());if(!dir)return 1;
try{for(bool legacy:{false,true}){
    auto path=std::string(dir)+(legacy?"/legacy":"/leveldb");if(legacy)Container::create_legacy(path);else Container::create(path);
    {
        Container store(path);Namespace fs(store);fs.initialize();auto root=fs.root_inode().id;fs.snapshot("base");fs.fork("base","work");
        Namespace::Attributes a;a.mode=0600;a.size=7;a.uid=501;a.gid=20;a.modified_ns=33;a.accessed_ns=44;a.created_ns=55;
        std::string h;auto n=fs.create_child(root,"file",false,a,legacy?nullptr:&h);
        CHECK(n.mode==0600 && n.size==7 && n.uid==501 && n.gid==20 && n.modified_ns==33 && n.accessed_ns==44 && n.created_ns==55);
        CHECK(fs.read("/file")==std::vector<unsigned char>(7,0));
        auto stable=store.status();
        error(EEXIST,[&]{fs.create_child(root,"file",false,a);});
        auto bad=a;bad.size=UINT64_MAX;error(EFBIG,[&]{fs.create_child(root,"big",false,bad);});
        bad=a;bad.created_ns=UINT64_MAX;error(EINVAL,[&]{fs.create_child(root,"bad",false,bad);});
        error(EISDIR,[&]{fs.create_child(root,"dir",true,a);});
        error(EINVAL,[&]{fs.symlink_child(root,"link","file",a);});
        error(EROFS,[&]{Namespace(store,"base",true).create_child(root,"new",false,a);});
        CHECK(store.status()==stable && fs.list("/")==std::vector<std::string>{"file"});
        a.size.reset();auto folder=fs.create_child(root,"dir",true,a);CHECK(folder.uid==501 && fs.parent_inode(folder.id).id==root);
        auto link=fs.symlink_child(root,"link","file",a);CHECK(link.symlink && link.uid==501 && link.mode==0600 && fs.readlink_inode(link.id)=="file");
        CHECK(Namespace(store,"base",true).list("/").empty() && Namespace(store,"work").list("/").empty());
        if(!legacy){
            std::vector<std::string> held;for(unsigned i=1;i<4096;++i)held.push_back(fs.dup(h));
            stable=store.status();std::string full;
            error(ENFILE,[&]{fs.create_child(root,"quota",false,a,&full);});CHECK(full.empty() && store.status()==stable);
            for(const auto& token:held)fs.close(token);
            fs.remove_child(root,"file",n.id,false);CHECK(fs.handle_stat(h).links==0 && fs.handle_read(h,0,20)==std::vector<unsigned char>(7,0));fs.close(h);
        }
        fs.verify();store.verify();
    }
    {Container store(path);Namespace fs(store);CHECK(fs.stat("/dir").created_ns==55 && fs.lstat("/link").uid==501);fs.verify();store.verify();}
}}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(dir);return 1;}
std::filesystem::remove_all(dir);return 0;}
