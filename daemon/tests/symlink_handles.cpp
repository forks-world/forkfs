#include "namespace.h"
#include <filesystem>
#include <iostream>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("symlink handle check failed at line "+std::to_string(line));}
template<class F> static void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
int main(){
    char pattern[]="/tmp/ff-linkhandle-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;auto path=std::string(tmp)+"/db";
    try{
        Container::create(path);
        {
            Container store(path);Namespace fs(store);fs.initialize();fs.write("/file",Bytes{'T'});fs.symlink("/file","/link");fs.set_xattr("/link","user.a",Bytes{'L'});fs.snapshot("base");fs.fork("base","work");
            Namespace::OpenOptions options;options.symlink=true;auto target=fs.stat("/file"),link=fs.lstat("/link");
            auto ordinary=fs.open("/link");check(fs.handle_stat(ordinary).id==target.id);fs.close(ordinary);
            ordinary=fs.open("/file",Namespace::Access::Read,options);check(fs.handle_read(ordinary,0,1)==Bytes{'T'});fs.close(ordinary);
            auto h=fs.open("/link",Namespace::Access::ReadWrite,options);check(fs.handle_stat(h).symlink && fs.handle_stat(h).id==link.id && link.id!=target.id);
            auto dup=fs.dup(h);auto stable=store.status();
            rejects([&]{fs.handle_read(h,0,1);});rejects([&]{fs.handle_read(h,0,0);});rejects([&]{fs.handle_read_next(h,1);});
            rejects([&]{fs.handle_write(h,0,Bytes{'X'});});rejects([&]{fs.handle_write_next(h,{});});rejects([&]{fs.handle_append(h,{});});rejects([&]{fs.handle_truncate(h,0);});
            check(fs.seek(h,0,Namespace::Seek::End)==link.size && fs.seek(dup,0,Namespace::Seek::Current)==link.size);
            check(store.status()==stable && fs.read("/file")==Bytes{'T'} && fs.stat("/file").changed_ns==target.changed_ns);
            rejects([&]{fs.restore("base","blocked");});rejects([&]{store.collect_garbage();});
            Namespace::Attributes a;a.mode=0701;a.accessed_ns=111;a.modified_ns=222;a.birth_ns=333;fs.handle_set_attributes(h,a);check(fs.lstat("/link").birth_ns==333);
            stable=store.status();a.size=link.size;a.mode=0777;rejects([&]{fs.handle_set_attributes(h,a);});check(store.status()==stable);
            fs.remove("/link",false);check(fs.handle_stat(h).links==0 && fs.handle_stat(h).symlink && fs.handle_get_xattr(h,"user.a")==Bytes{'L'});
            fs.symlink("/absent","/link");auto replacement=fs.lstat("/link").id;check(replacement!=link.id);
            fs.handle_set_xattr(h,"user.a",Bytes{'O'});check(fs.handle_get_xattr(dup,"user.a")==Bytes{'O'} && fs.list_xattrs("/link").empty());
            a={};a.modified_ns=444;fs.handle_set_attributes(dup,a);check(fs.handle_stat(h).modified_ns==444 && fs.lstat("/link").modified_ns!=444);fs.verify();
            fs.close(h);check(fs.handle_stat(dup).links==0);fs.close(dup);fs.verify();
            // Replacement by rename retains the old symlink for its open fd.
            h=fs.open("/link",Namespace::Access::Read,options);fs.symlink("/file","/new");fs.rename("/new","/link");check(fs.handle_stat(h).id==replacement && fs.handle_stat(h).links==0);fs.close(h);
            Namespace old(store,"base",true),work(store,"work");h=old.open("/link",Namespace::Access::Read,options);check(old.handle_get_xattr(h,"user.a")==Bytes{'L'});
            rejects([&]{old.handle_set_xattr(h,"user.a",Bytes{});});rejects([&]{old.handle_set_attributes(h,a);});rejects([&]{old.handle_read(h,0,1);});old.close(h);check(work.get_xattr("/link","user.a")==Bytes{'L'});
            for(int flag=0;flag<4;++flag){auto bad=options;if(flag==0)bad.create=true;if(flag==1)bad.truncate=true;if(flag==2)bad.append=true;if(flag==3)bad.nofollow=true;rejects([&]{fs.open("/link",Namespace::Access::ReadWrite,bad);});}
            fs.symlink("/cycle","/cycle");h=fs.open("/cycle",Namespace::Access::Read,options);check(fs.handle_stat(h).symlink);fs.close(h);
            fs.symlink("/missing","/recover");h=fs.open("/recover",Namespace::Access::ReadWrite,options);fs.remove("/recover",false);fs.handle_set_xattr(h,"user.recover",Bytes{'R'});fs.verify();
            // Leave this handle unclosed: the next process releases its orphan.
        }
        {Container store(path);Namespace fs(store);fs.verify();store.collect_garbage();check(fs.read("/file")==Bytes{'T'} && fs.readlink("/link")=="/file");Namespace(store,"base",true).verify();store.verify();}
        std::filesystem::remove_all(tmp);std::cout<<"symlink metadata handles, data guards, dup/orphans, replacement, revisions, option validation, restart and GC passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
