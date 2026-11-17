#include "namespace.h"
#include "journal.h"
#include "filesystem_limits.h"
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using forkfs::Container;using forkfs::Journal;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static const std::string prefix("\0forkfs/",8),history("\0forkfs-history/",16);
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("directory metadata handle check failed at line "+std::to_string(line));}
template<class F> static void rejects(F f,int code=0){try{f();}catch(const std::system_error& e){if(code)check(e.code().value()==code);return;}catch(...){if(!code)return;}check(false);}
static void put64(Bytes& b,size_t at,uint64_t v){for(unsigned i=0;i<8;++i)b[at+i]=static_cast<unsigned char>(v>>(8*i));}
static Bytes flat_dir(uint32_t mode,uint64_t links,bool xattrs=false){Bytes b(64);memcpy(b.data(),"FFINOD02",8);b[8]=2;for(unsigned i=0;i<4;++i)b[12+i]=static_cast<unsigned char>((mode|(xattrs?0x80000000U:0))>>(8*i));put64(b,16,links);put64(b,32,1);put64(b,40,1);put64(b,48,1);put64(b,56,1);return b;}
static std::string flat_fixture(const std::string& path){
    Container::create(path);std::string root;
    {Container store(path);Namespace fs(store);fs.initialize();root=fs.stat("/").id;}
    const std::string victim(32,'a');Journal j(path);check(!j.contains(history+"world/main"));auto root_inode=j.get(prefix+"inode/"+root);put64(root_inode,16,3);
    std::vector<forkfs::Mutation> batch{{prefix+"inode/"+root,root_inode},
        {prefix+"entry/"+root+"/victim",Bytes(victim.begin(),victim.end())},{prefix+"inode/"+victim,flat_dir(0712,2,true)},
        {prefix+"allocated/"+victim,Bytes{}},{prefix+"xattr/"+victim+"/user.flat",Bytes{'F'}}};
    j.transact(j.sequence(),batch);return victim;
}
static void exercise(const std::string& path,bool flat){
    if(flat)flat_fixture(path);else Container::create(path);
    Container store(path);Namespace fs(store);if(!flat){fs.initialize();fs.mkdir("/remove",0711);fs.mkdir("/old",0701);fs.mkdir("/source",0750);}
    auto target=flat?"/victim":"/remove";auto before=fs.lstat(target);Namespace old(store,"dir-base",true);std::string revision_handle;
    Namespace::OpenOptions pin;pin.directory=true;pin.nofollow=true;
    if(!flat){fs.set_xattr(target,"user.keep",Bytes{'A'});Namespace::Attributes times;times.modified_ns=101;times.accessed_ns=202;fs.set_attributes(target,times);before=fs.lstat(target);fs.snapshot("dir-base");revision_handle=old.open(target,Namespace::Access::Read,pin);}
    rejects([&]{fs.open(target);},EISDIR);rejects([&]{fs.open(target,Namespace::Access::Write,pin);});
    const auto stable=store.status();auto handle=fs.open(target,Namespace::Access::Read,pin);check(store.status()==stable && fs.handle_stat(handle).id==before.id);auto duplicate=fs.dup(handle);
    rejects([&]{fs.handle_read(handle,0,0);},EISDIR);rejects([&]{fs.handle_read_next(handle,1);},EISDIR);rejects([&]{fs.handle_write(handle,0,Bytes{'X'});},EBADF);rejects([&]{fs.seek(handle,0,Namespace::Seek::Set);},ENOTSUP);rejects([&]{fs.handle_seek_region(handle,0,true);},ENOTSUP);
    Namespace::Attributes size;size.size=0;rejects([&]{fs.handle_set_attributes(handle,size);},EISDIR);size.mode=0600;rejects([&]{fs.handle_set_attributes(handle,size);},EISDIR);
    auto state=store.status();for(int flag=0;flag<7;++flag){auto invalid=pin;if(flag==0)invalid.create=true;if(flag==1)invalid.exclusive=true;if(flag==2)invalid.truncate=true;if(flag==3)invalid.append=true;if(flag==4)invalid.positioned_append=true;if(flag==5)invalid.fifo=true;if(flag==6)invalid.symlink=true;rejects([&]{fs.open(target,Namespace::Access::Read,invalid);});}check(store.status()==state);
    fs.remove(target,true);
    check(fs.handle_stat(handle).id==before.id && fs.handle_stat(handle).links==2 && fs.handle_stat(handle).allocated_size==0);
    if(!flat){fs.mkdir(target,0755);check(fs.lstat(target).id!=before.id);}
    fs.handle_set_xattr(handle,flat?"user.flat":"user.keep",Bytes{'O'});
    Namespace::Attributes edit;edit.mode=0703;edit.modified_ns=303;edit.accessed_ns=404;fs.handle_set_attributes(duplicate,edit);
    check(fs.handle_stat(handle).mode==0703 && fs.handle_stat(handle).modified_ns==303 && fs.handle_stat(handle).accessed_ns==404);
    check(fs.handle_get_xattr(handle,flat?"user.flat":"user.keep")==Bytes{'O'});
    if(!flat)check(fs.list_xattrs(target).empty());
    if(!flat){rejects([&]{old.handle_set_xattr(revision_handle,"user.keep",Bytes{'R'});});rejects([&]{old.handle_set_attributes(revision_handle,edit);});check(old.handle_stat(revision_handle).id==before.id && old.handle_get_xattr(revision_handle,"user.keep")==Bytes{'A'});old.close(revision_handle);old.verify();}
    fs.verify();fs.close(handle);check(fs.handle_stat(duplicate).id==before.id);fs.close(duplicate);fs.verify();store.collect_garbage();fs.verify();
    if(!flat){fs.mkdir("/protected",0700);auto protected_handle=fs.open("/protected",Namespace::Access::Read,pin);Namespace::Attributes flags;flags.flags=forkfs::file_flag_immutable;fs.handle_set_attributes(protected_handle,flags);auto protected_state=store.status();Namespace::Attributes mixed;mixed.size=0;mixed.mode=0701;rejects([&]{fs.handle_set_attributes(protected_handle,mixed);},EPERM);check(store.status()==protected_state);flags.flags=0;fs.handle_set_attributes(protected_handle,flags);flags.flags=forkfs::file_flag_append;fs.handle_set_attributes(protected_handle,flags);protected_state=store.status();rejects([&]{fs.handle_set_attributes(protected_handle,mixed);},EPERM);check(store.status()==protected_state);flags.flags=0;fs.handle_set_attributes(protected_handle,flags);fs.close(protected_handle);fs.verify();}
}
static void rename_replace(const std::string& path){
    Container::create(path);Container store(path);Namespace fs(store);fs.initialize();fs.mkdir("/source",0750);fs.mkdir("/target",0701);fs.set_xattr("/target","user.old",Bytes{'T'});
    auto old=fs.lstat("/target");Namespace::OpenOptions pin;pin.directory=true;auto h=fs.open("/target",Namespace::Access::Read,pin);
    fs.rename("/source","/target");check(fs.lstat("/target").id!=old.id && fs.handle_stat(h).id==old.id && fs.handle_stat(h).links==2);
    check(fs.handle_get_xattr(h,"user.old")==Bytes{'T'});fs.handle_set_xattr(h,"user.old",Bytes{'X'});check(fs.list_xattrs("/target").empty());
    fs.verify();fs.close(h);fs.verify();
}
static void mkdir_by_handle(const std::string& path){
    Container::create(path);Container store(path);Namespace fs(store);fs.initialize();fs.mkdir("/parent",0750);
    Namespace::OpenOptions pin;pin.directory=true;auto handle=fs.open("/parent",Namespace::Access::Read,pin),duplicate=fs.dup(handle);
    auto linked_state=store.status();rejects([&]{fs.handle_mkdir(handle,"overflow",uint64_t(1)<<32);});check(store.status()==linked_state);
    auto original=fs.handle_stat(handle).id;fs.rename("/parent","/moved");fs.mkdir("/parent",0700);
    auto created=fs.handle_mkdir(handle,"child",0711);check(fs.lstat("/moved/child").id==created.id && fs.lstat("/parent").id!=original);
    fs.remove("/moved/child",true);fs.remove("/moved",true);fs.mkdir("/moved",0700);auto replacement=fs.lstat("/moved").id;
    auto before=store.status();rejects([&]{fs.handle_mkdir(handle,"orphan",0755);},ENOENT);
    check(store.status()==before && fs.lstat("/moved").id==replacement && fs.list("/moved").empty());
    rejects([&]{fs.handle_mkdir(duplicate,"orphan",0755);},ENOENT);check(store.status()==before);
    fs.close(handle);rejects([&]{fs.handle_mkdir(handle,"closed",0755);},EBADF);
    rejects([&]{fs.handle_mkdir(duplicate,"still-open",0701);},ENOENT);fs.close(duplicate);
    fs.write("/file",Bytes{'x'});auto file=fs.open("/file",Namespace::Access::Read);rejects([&]{fs.handle_mkdir(file,"child",0755);},ENOTDIR);fs.close(file);
    Namespace old(store,"mkdir-base",true);fs.snapshot("mkdir-base");auto revision_handle=old.open("/moved",Namespace::Access::Read,pin);
    rejects([&]{old.handle_mkdir(revision_handle,"child",0755);},EROFS);old.close(revision_handle);fs.verify();
}
int main(){char pattern[]="/tmp/ff-dirhandle-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;try{
    exercise(std::string(tmp)+"/modern",false);exercise(std::string(tmp)+"/flat",true);rename_replace(std::string(tmp)+"/rename");
    mkdir_by_handle(std::string(tmp)+"/mkdir");
    std::filesystem::remove_all(tmp);std::cout<<"directory metadata pins, rmdir, flat retirement, rename replacement, identity reuse, xattrs and final close passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}}
