#include "namespace.h"
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace forkfs;using Bytes=std::vector<unsigned char>;
#ifdef ENOATTR
constexpr int missing=ENOATTR;
#else
constexpr int missing=ENODATA;
#endif
void check(bool ok){if(!ok)throw std::runtime_error("inode xattr regression failed");}
template<class F>void error(int code,F f){try{f();}catch(const std::system_error& e){check(e.code().value()==code);return;}throw std::runtime_error("expected xattr error");}
int main(){auto pattern=(std::filesystem::temp_directory_path()/"ff-xattr-XXXXXX").string();auto tmp=mkdtemp(pattern.data());if(!tmp)return 1;
    try{for(bool legacy:{false,true}){
        auto path=std::string(tmp)+(legacy?"/legacy":"/leveldb");if(legacy)Container::create_legacy(path);else Container::create(path);std::string id;
        {Container store(path);Namespace fs(store);fs.initialize();auto root=fs.root_inode().id;id=fs.create_child(root,"file",false,0644).id;
            fs.link_child(id,root,"alias");auto sym=fs.symlink_child(root,"sym","file");check(fs.list_inode_xattrs(id).empty());
            fs.set_inode_xattr(id,"user.binary",{0,255},Namespace::XattrMode::Create);fs.set_inode_xattr(id,"user.empty",{});
            check(fs.get_inode_xattr(fs.stat("/alias").id,"user.binary")==Bytes({0,255}) && fs.get_inode_xattr(id,"user.empty").empty());
            fs.set_inode_xattr(root,"user.root",{1});fs.set_inode_xattr(sym.id,"user.sym",{2});check(fs.parent_inode(root).id==root);
            fs.snapshot("base");fs.fork("base","work");Namespace revision(store,"base",true),work(store,"work");auto stable=store.status();
            error(EEXIST,[&]{fs.set_inode_xattr(id,"user.binary",{},Namespace::XattrMode::Create);});
            error(missing,[&]{fs.set_inode_xattr(id,"user.absent",{},Namespace::XattrMode::Replace);});error(missing,[&]{fs.remove_inode_xattr(id,"user.absent");});
            error(EINVAL,[&]{fs.set_inode_xattr(id,"",{});});error(EINVAL,[&]{fs.set_inode_xattr(id,std::string("bad\0name",8),{});});
            error(E2BIG,[&]{fs.set_inode_xattr(id,"user.large",Bytes(64*1024+1));});error(EROFS,[&]{revision.remove_inode_xattr(id,"user.binary");});
            check(store.status()==stable);fs.set_inode_xattr(id,"user.binary",{3},Namespace::XattrMode::Replace);
            check(revision.get_inode_xattr(id,"user.binary")==Bytes({0,255}) && work.get_inode_xattr(id,"user.binary")==Bytes({0,255}));
            fs.set_inode_attributes(id,{0600,123,4});check(fs.get_inode_xattr(id,"user.binary")==Bytes{3});
            fs.set_inode_xattr(id,"user.big",Bytes(64*1024,7));stable=store.status();error(E2BIG,[&]{fs.set_inode_xattr(id,"user.big2",Bytes(64*1024,8));});check(store.status()==stable);
            fs.remove_inode_xattr(id,"user.big");fs.remove_inode_xattr(id,"user.empty");check(fs.list_inode_xattrs(id)==std::vector<std::string>{"user.binary"});
            if(!legacy){auto h=fs.open_inode(id,Namespace::Access::ReadWrite);fs.remove_child(root,"file",id,false);fs.remove_child(root,"alias",id,false);
                check(fs.get_handle_xattr(h,"user.binary")==Bytes{3});fs.set_handle_xattr(h,"user.orphan",{4});fs.handle_truncate(h,2);
                check(fs.get_handle_xattr(h,"user.orphan")==Bytes{4});error(ENOENT,[&]{fs.get_inode_xattr(id,"user.binary");});
                fs.remove_handle_xattr(h,"user.binary");fs.remove_handle_xattr(h,"user.orphan");check(fs.list_handle_xattrs(h).empty());fs.close(h);
            }else{fs.remove_inode_xattr(id,"user.binary");check(fs.list_inode_xattrs(id).empty());}
            fs.verify();revision.verify();work.verify();fs.compact();store.verify();
        }
        {Container store(path);Namespace fs(store),revision(store,"base",true);check(fs.get_inode_xattr(fs.root_inode().id,"user.root")==Bytes{1});check(revision.get_inode_xattr(id,"user.binary")==Bytes({0,255}));fs.verify();store.verify();}
    }}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
    std::filesystem::remove_all(tmp);return 0;
}
