#include "namespace.h"
#include <filesystem>
#include <iostream>
#include <system_error>
#include <unistd.h>
using namespace forkfs;
using Bytes=std::vector<unsigned char>;
void check(bool value){if(!value)throw std::runtime_error("inode entry regression failed");}
template<class F> void error(int code,F operation){try{operation();}catch(const std::system_error& e){check(e.code().value()==code);return;}throw std::runtime_error("expected inode entry error");}
int main(){
    auto pattern=(std::filesystem::temp_directory_path()/"ff-inode-entries-XXXXXX").string();auto temp=mkdtemp(pattern.data());if(!temp)return 1;
    auto path=std::string(temp)+"/store";
    try{
        Container::create(path);
        {
            Container store(path);Namespace fs(store);fs.initialize();auto root=fs.root_inode();
            auto p=fs.create_child(root.id,"p",true,0750),q=fs.create_child(root.id,"q",true,0755);
            auto a=fs.create_child(p.id,"a",false,0600);check(a.mode==0600 && a.links==1 && a.size==0);
            auto ah=fs.open_inode(a.id,Namespace::Access::ReadWrite);fs.handle_write(ah,0,Bytes{'A'});
            fs.rename("/p","/moved");auto b=fs.create_child(p.id,"b",false,0644);check(fs.stat("/moved/b").id==b.id);
            fs.snapshot("base");fs.fork("base","work");Namespace revision(store,"base",true),work(store,"work");
            auto stable=store.status();
            error(EEXIST,[&]{fs.create_child(p.id,"a",false,0644);});
            error(ESTALE,[&]{fs.remove_child(p.id,"a",b.id,false);});
            error(ESTALE,[&]{fs.rename_child(p.id,"a",q.id,"x",b.id);});
            error(EINVAL,[&]{fs.create_child(p.id,"../x",false,0644);});
            error(ENAMETOOLONG,[&]{fs.create_child(p.id,std::string(256,'x'),false,0644);});
            error(ENOTDIR,[&]{fs.create_child(a.id,"child",false,0644);});
            error(ENOTEMPTY,[&]{fs.remove_child(root.id,"moved",p.id,true);});
            error(EISDIR,[&]{fs.remove_child(root.id,"moved",p.id,false);});
            error(EROFS,[&]{revision.create_child(p.id,"x",false,0644);});
            error(EROFS,[&]{revision.remove_child(p.id,"a",a.id,false);});
            error(EROFS,[&]{revision.rename_child(p.id,"a",q.id,"x",a.id);});
            check(store.status()==stable);
            fs.rename_child(p.id,"a",q.id,"x",a.id);check(fs.stat("/q/x").id==a.id);
            fs.rename_child(q.id,"x",q.id,"x",a.id);check(fs.stat("/q/x").id==a.id);
            auto target=fs.create_child(q.id,"target",false,0644);auto th=fs.open_inode(target.id,Namespace::Access::ReadWrite);fs.handle_write(th,0,Bytes{'T'});
            stable=store.status();error(ESTALE,[&]{fs.rename_child(q.id,"x",q.id,"target",a.id);});check(store.status()==stable);
            fs.rename_child(q.id,"x",q.id,"target",a.id,target.id);check(fs.stat("/q/target").id==a.id);
            check(fs.handle_stat(th).links==0 && fs.handle_read(th,0,9)==Bytes{'T'});
            fs.remove_child(q.id,"target",a.id,false);check(fs.handle_stat(ah).links==0 && fs.handle_read(ah,0,9)==Bytes{'A'});
            auto replacement=fs.create_child(q.id,"target",false,0644);stable=store.status();
            error(ESTALE,[&]{fs.remove_child(q.id,"target",a.id,false);});check(store.status()==stable && fs.lookup_child(q.id,"target").id==replacement.id);
            auto nested=fs.create_child(p.id,"nested",true,0755);stable=store.status();
            error(EINVAL,[&]{fs.rename_child(root.id,"moved",nested.id,"cycle",p.id);});check(store.status()==stable);
            fs.rename_child(p.id,"nested",q.id,"nested",nested.id);check(fs.stat_inode(p.id).links==2 && fs.stat_inode(q.id).links==3);
            auto empty=fs.create_child(p.id,"empty",true,0755);fs.rename_child(q.id,"nested",p.id,"empty",nested.id,empty.id);
            check(fs.stat_inode(p.id).links==3 && fs.stat_inode(q.id).links==2);
            fs.remove_child(p.id,"empty",nested.id,true);check(fs.stat_inode(p.id).links==2);
            check(revision.lookup_child(p.id,"a").id==a.id && work.lookup_child(p.id,"a").id==a.id);
            fs.close(ah);fs.close(th);fs.verify();revision.verify();work.verify();fs.compact();store.verify();
        }
        {Container store(path);Namespace fs(store),revision(store,"base",true);check(fs.list("/moved")==std::vector<std::string>{"b"});check(revision.read("/moved/a")==Bytes{'A'});store.verify();}
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(temp);return 1;}
    std::filesystem::remove_all(temp);return 0;
}
