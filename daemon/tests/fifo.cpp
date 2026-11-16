#include "namespace.h"
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <unistd.h>

using forkfs::Container;
using forkfs::Namespace;
using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("FIFO check failed at line "+std::to_string(line));}
template<class F> static void rejects(F f,int code){try{f();}catch(const std::system_error& e){check(e.code().value()==code);return;}throw std::runtime_error("expected FIFO errno "+std::to_string(code));}
template<class F> static void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
static std::string logical_state(const std::string& status){std::string result;for(const auto* field:{"last_committed_seq","object_roots","indexed_objects"}){auto key="\""+std::string(field)+"\":";auto begin=status.find(key);check(begin!=status.npos);begin+=key.size();auto end=status.find_first_not_of("0123456789",begin);check(end!=begin);result+=status.substr(begin,end-begin)+"/";}return result;}

int main(){
    char pattern[]="/tmp/ff-fifo-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
    try{
        for(bool legacy:{false,true}){
            auto path=std::string(tmp)+(legacy?"/legacy":"/modern");if(legacy)Container::create_legacy(path);else Container::create(path);
            {
                Container store(path);Namespace fs(store);fs.initialize();fs.mkfifo("/pipe",0620);
                auto pipe=fs.lstat("/pipe");check(pipe.fifo && !pipe.directory && !pipe.symlink && pipe.size==0 && !pipe.chunked && pipe.mode==0620);
                auto stable=logical_state(store.status());rejects([&]{fs.mkfifo("/new-pipe/",0600);});rejects([&]{fs.mkfifo("/pipe/",0600);});check(fs.lstat("/pipe").id==pipe.id);rejects([&]{fs.lstat("/new-pipe");});
                rejects([&]{fs.write("/pipe",Bytes{'X'});},ENOTSUP);
                rejects([&]{fs.replace_file("/pipe",Bytes{'X'});},ENOTSUP);
                rejects([&]{fs.append("/pipe",Bytes{'X'});},ENOTSUP);
                rejects([&]{fs.truncate("/pipe",0);},ENOTSUP);
                rejects([&]{fs.punch_hole("/pipe",0,1);},ENOTSUP);
                rejects([&]{fs.read_at("/pipe",0,1);},ENOTSUP);
                if(!legacy)rejects([&]{fs.open("/pipe");},ENOTSUP);else rejects([&]{fs.open("/pipe");});
                Namespace::Attributes size;size.size=0;size.mode=0600;rejects([&]{fs.set_attributes("/pipe",size);},EINVAL);
                check(logical_state(store.status())==stable && fs.lstat("/pipe").mode==0620);
                rejects([&]{fs.seek_region("/pipe",0,true);},ESPIPE);
                fs.set_xattr("/pipe","user.test",Bytes{'v'});fs.link("/pipe","/alias");
                check(fs.lstat("/alias").fifo && fs.lstat("/alias").id==pipe.id && fs.lstat("/alias").links==2);
                if(!legacy){
                    rejects([&]{fs.clone_file("/pipe","/fifo-copy");},ENOTSUP);
                    fs.symlink("/pipe","/pipe-link");rejects([&]{fs.clone_file("/pipe-link","/fifo-link-copy");},ENOTSUP);
                    Namespace::OpenOptions metadata;metadata.fifo=true;metadata.nofollow=true;
                    fs.mkfifo("/pinned",0600);fs.set_xattr("/pinned","user.test",Bytes{'v'});
                    auto pinned=fs.lstat("/pinned");auto pin=fs.open("/pinned",Namespace::Access::Read,metadata);auto duplicate=fs.dup(pin);
                    check(fs.handle_stat(pin).fifo && fs.handle_stat(pin).id==pinned.id);
                    fs.snapshot("fifo-pin-base");Namespace read_only(store,"fifo-pin-base",true);
                    auto revision_pin=read_only.open("/pinned",Namespace::Access::Read,metadata);
                    check(read_only.handle_stat(revision_pin).fifo);
                    Namespace::Attributes revision_mode;revision_mode.mode=0640;
                    rejects([&]{read_only.handle_set_attributes(revision_pin,revision_mode);});
                    read_only.close(revision_pin);
                    rejects([&]{fs.seek(pin,0,Namespace::Seek::Set);},ESPIPE);
                    rejects([&]{fs.handle_read(pin,0,1);},ENOTSUP);
                    rejects([&]{fs.open("/pinned",Namespace::Access::Read);},ENOTSUP);
                    rejects([&]{fs.open("/pipe",Namespace::Access::ReadWrite,metadata);});
                    Namespace::Attributes invalid_size;invalid_size.size=0;rejects([&]{fs.handle_set_attributes(pin,invalid_size);},EINVAL);
                    fs.write("/new",Bytes{'n','e','w'});fs.rename("/new","/pinned");
                    check(!fs.lstat("/pinned").fifo && fs.lstat("/pinned").id!=pinned.id);
                    check(fs.handle_stat(pin).fifo && fs.handle_stat(pin).links==0 && fs.handle_stat(pin).size==0);
                    check(fs.handle_get_xattr(pin,"user.test")==Bytes{'v'});
                    Namespace::Attributes orphan_mode;orphan_mode.mode=0640;fs.handle_set_attributes(pin,orphan_mode);
                    check(fs.handle_stat(pin).mode==0640 && fs.lstat("/pinned").mode==0644);
                    fs.close(pin);check(fs.handle_stat(duplicate).links==0);
                    fs.close(duplicate);store.collect_garbage();
                    rejects([&]{fs.handle_stat(duplicate);},EBADF);
                    fs.remove("/pinned",false);
                }
                fs.rename("/alias","/renamed");check(fs.lstat("/renamed").fifo && fs.lstat("/renamed").id==pipe.id);
                fs.snapshot("fifo-base");Namespace base(store,"fifo-base",true);check(base.lstat("/renamed").fifo);
                fs.verify();base.verify();store.verify();
                fs.remove("/renamed",false);fs.remove("/pipe",false);fs.verify();
                if(!legacy)store.collect_garbage();store.verify();
            }
            {Container store(path);Namespace fs(store);fs.verify();store.verify();}
        }
        std::filesystem::remove_all(tmp);std::cout<<"FIFO metadata, regular I/O and size guards, link/rename, snapshots, verification and GC passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
