#include "namespace.h"
#include <filesystem>
#include <iostream>
#include <system_error>
#include <unistd.h>
using namespace forkfs;
using Bytes=std::vector<unsigned char>;
void check(bool value){if(!value)throw std::runtime_error("inode handle regression failed");}
template<class F> void error(int code,F operation){try{operation();}catch(const std::system_error& e){check(e.code().value()==code);return;}throw std::runtime_error("expected inode open error");}
template<class F> void rejects(F operation){try{operation();}catch(const std::exception&){return;}throw std::runtime_error("expected rejection");}
int main(){
    auto pattern=(std::filesystem::temp_directory_path()/"ff-inode-handles-XXXXXX").string();auto temporary=mkdtemp(pattern.data());if(!temporary)return 1;
    auto path=std::string(temporary)+"/store";
    try{
        Container::create(path);std::string id;
        {
            Container store(path);Namespace fs(store);fs.initialize();fs.mkdir("/dir");fs.write("/dir/a",Bytes{'a','b','c'});
            id=fs.stat("/dir/a").id;fs.link("/dir/a","/alias");fs.symlink("alias","/link");fs.snapshot("base");fs.fork("base","work");
            Namespace revision(store,"base",true),work(store,"work");
            auto unchanged=store.status();auto frozen=revision.open_inode(id);check(store.status()==unchanged);
            auto handle=fs.open_inode(id,Namespace::Access::ReadWrite),alias=fs.open("/alias"),child=work.open_inode(id,Namespace::Access::ReadWrite);
            fs.rename("/dir","/moved");fs.handle_write(handle,1,Bytes{'X'});check(fs.read("/alias")==Bytes({'a','X','c'}));
            work.handle_write(child,0,Bytes{'W'});check(work.read("/dir/a")==Bytes({'W','b','c'}));
            check(revision.handle_read(frozen,0,99)==Bytes({'a','b','c'}));
            fs.remove("/moved/a",false);fs.remove("/alias",false);check(fs.handle_stat(handle).links==0);
            auto unlinked=store.status();error(ENOENT,[&]{fs.open_inode(id);});check(store.status()==unlinked);
            fs.write("/alias",Bytes{'N'});check(fs.stat("/alias").id!=id);
            fs.handle_write(handle,4,Bytes{'!'});check(fs.handle_read(alias,0,99)==Bytes({'a','X','c',0,'!'}));
            auto duplicate=fs.dup(handle);fs.close(handle);fs.close(alias);check(fs.handle_read(duplicate,0,99)==Bytes({'a','X','c',0,'!'}));
            fs.close(duplicate);fs.compact();fs.verify();
            error(ENOENT,[&]{fs.open_inode(id);});
            error(EROFS,[&]{revision.open_inode(id,Namespace::Access::Write);});
            error(EISDIR,[&]{fs.open_inode(fs.root_inode().id);});
            error(ELOOP,[&]{fs.open_inode(fs.lstat("/link").id);});
            error(EINVAL,[&]{fs.open_inode("bad-id");});
            Namespace::OpenOptions create;create.create=true;error(EINVAL,[&]{fs.open_inode(fs.stat("/alias").id,Namespace::Access::ReadWrite,create);});
            Namespace::OpenOptions truncate;truncate.truncate=true;auto childTruncate=work.open_inode(id,Namespace::Access::ReadWrite,truncate);
            check(work.handle_stat(childTruncate).size==0 && revision.handle_read(frozen,0,99)==Bytes({'a','b','c'}));
            error(EINVAL,[&]{work.open_inode(id,Namespace::Access::Read,truncate);});
            Namespace::OpenOptions append;append.append=true;auto appender=work.open_inode(id,Namespace::Access::Write,append);
            work.handle_write(appender,999,Bytes{'A'});work.handle_write(appender,0,Bytes{'B'});check(work.read("/dir/a")==Bytes({'A','B'}));
            rejects([&]{work.handle_read(appender,0,1);});rejects([&]{revision.handle_write(frozen,0,Bytes{'!'});});
            work.close(childTruncate);work.close(appender);work.close(child);revision.close(frozen);
            auto restored=work.open_inode(id);work.restore("base");rejects([&]{work.handle_stat(restored);});
            auto reopened=work.open_inode(id);check(work.handle_read(reopened,0,99)==Bytes({'a','b','c'}));work.close(reopened);store.verify();
        }
        {Container store(path);Namespace fs(store),revision(store,"base",true);error(ENOENT,[&]{fs.open_inode(id);});auto h=revision.open_inode(id);check(revision.handle_read(h,0,99)==Bytes({'a','b','c'}));revision.close(h);store.verify();}
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(temporary);return 1;}
    std::filesystem::remove_all(temporary);return 0;
}
