#include "namespace.h"
#include <barrier>
#include <exception>
#include <filesystem>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace forkfs;
using Bytes=std::vector<unsigned char>;
void check(bool ok){if(!ok)throw std::runtime_error("inode contract regression failed");}
template<class F>void error(int code,F f){try{f();}catch(const std::system_error& e){check(e.code().value()==code);return;}throw std::runtime_error("expected inode errno");}
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("expected revoked cursor");}
int main(){
    auto pattern=(std::filesystem::temp_directory_path()/"ff-inode-contracts-XXXXXX").string();auto tmp=mkdtemp(pattern.data());if(!tmp)return 1;
    try{
        for(bool legacy:{false,true}){
            auto path=std::string(tmp)+(legacy?"/legacy":"/leveldb");if(legacy)Container::create_legacy(path);else Container::create(path);
            Container store(path);Namespace fs(store);fs.initialize();auto root=fs.root_inode().id;
            auto dir=fs.create_child(root,"dir",true,0755);auto file=fs.create_child(dir.id,"a",false,0644);
            fs.write("/dir/a",Bytes{'A','B'});auto sym=fs.symlink_child(dir.id,"b","a");auto missing=std::string(32,'f');
            check(fs.read_inode(file.id,1,UINT64_MAX)==Bytes{'B'} && fs.read_inode(file.id,2,1).empty());
            auto stable=store.status();
            error(EINVAL,[&]{fs.stat_inode("bad");});error(ENOENT,[&]{fs.stat_inode(missing);});
            error(ENOENT,[&]{fs.lookup_child(missing,"a");});error(ENOTDIR,[&]{fs.lookup_child(file.id,"a");});
            error(EINVAL,[&]{fs.lookup_child(dir.id,"../a");});error(ENAMETOOLONG,[&]{fs.lookup_child(dir.id,std::string(256,'x'));});
            error(ENOENT,[&]{fs.read_inode(missing,0,1);});error(EISDIR,[&]{fs.read_inode(dir.id,0,1);});
            error(ELOOP,[&]{fs.read_inode(sym.id,0,1);});error(EINVAL,[&]{fs.read_inode(file.id,UINT64_MAX,1);});
            error(EINVAL,[&]{fs.readlink_inode(file.id);});error(ENOENT,[&]{fs.readlink_inode(missing);});
            error(ENOTDIR,[&]{fs.list_inode(file.id,"",1);});error(EINVAL,[&]{fs.list_inode(dir.id,"",0);});
            error(EINVAL,[&]{fs.list_inode(dir.id,"a/b",1);});error(ENOENT,[&]{fs.list_inode(missing,"",1);});
            error(ENOTDIR,[&]{fs.open_directory_inode(file.id);});error(ENOENT,[&]{fs.open_directory_inode(missing);});
            check(store.status()==stable);
            fs.snapshot("base");fs.fork("base","work");Namespace revision(store,"base",true),work(store,"work");
            if(legacy)error(ENOTSUP,[&]{fs.open_directory_inode(dir.id);});
            else{
                auto cursor=fs.open_directory_inode(dir.id);auto frozen=revision.open_directory_inode(dir.id);
                fs.rename("/dir","/moved");fs.set_inode_attributes(file.id,{0600,{}});fs.create_child(dir.id,"c",false,0644);
                auto page=fs.read_directory(cursor,1,true);check(page.names==std::vector<std::string>{"a"} && !page.eof && page.entries[0].inode.mode==0644);
                fs.remove_child(dir.id,"a",file.id,false);fs.remove_child(dir.id,"b",sym.id,false);
                fs.remove_child(dir.id,"c",fs.lookup_child(dir.id,"c").id,false);fs.remove_child(root,"moved",dir.id,true);
                page=fs.read_directory(cursor,1,true);check(page.names==std::vector<std::string>{"b"} && page.eof && page.entries[0].inode.symlink);
                fs.rewind_directory(cursor);check(fs.read_directory(cursor,9).names==std::vector<std::string>({"a","b"}));
                error(ENOENT,[&]{fs.open_directory_inode(dir.id);});rejects([&]{work.read_directory(cursor,1);});
                fs.restore("base");rejects([&]{fs.read_directory(cursor,1);});rejects([&]{fs.rewind_directory(cursor);});
                check(revision.read_directory(frozen,9).names==std::vector<std::string>({"a","b"}));
                auto fresh=fs.open_directory_inode(dir.id);check(fs.read_directory(fresh,9).names==std::vector<std::string>({"a","b"}));
            }
            // Exactly one transaction may publish a concurrently created name.
            for(unsigned round=0;round<8;++round){std::barrier start(2);int codes[2]={-1,-1};std::exception_ptr failures[2];
                auto name="race"+std::to_string(round);auto create=[&](int slot){start.arrive_and_wait();try{fs.create_child(root,name,false,0644);codes[slot]=0;}
                    catch(const std::system_error& e){codes[slot]=e.code().value();}catch(...){failures[slot]=std::current_exception();}};
                std::thread a(create,0),b(create,1);a.join();b.join();for(auto f:failures)if(f)std::rethrow_exception(f);
                check((codes[0]==0 && codes[1]==EEXIST)||(codes[1]==0 && codes[0]==EEXIST));
                auto n=fs.lookup_child(root,name);check(n.links==1 && n.size==0);fs.remove_child(root,name,n.id,false);
            }
            fs.verify();revision.verify();work.verify();store.verify();
        }
        // A cursor cannot be reused against a reopened Container.
        auto path=std::string(tmp)+"/lifetime";Container::create(path);Namespace::DirectoryCursor cursor;
        {Container store(path);Namespace fs(store);fs.initialize();cursor=fs.open_directory_inode(fs.root_inode().id);}
        {Container store(path);Namespace fs(store);rejects([&]{fs.read_directory(cursor,1);});rejects([&]{fs.rewind_directory(cursor);});}
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
    std::filesystem::remove_all(tmp);return 0;
}
