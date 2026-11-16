#include "namespace.h"
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <unistd.h>

using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("socket check failed at line "+std::to_string(line));}
template<class F> static void rejects(F f,int code){try{f();}catch(const std::system_error& e){check(e.code().value()==code);return;}throw std::runtime_error("expected socket errno "+std::to_string(code));}
template<class F> static void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
static std::string logical_state(const std::string& status){std::string out;for(const auto* f:{"last_committed_seq","object_roots","indexed_objects"}){auto p=status.find("\""+std::string(f)+"\":");check(p!=status.npos);p+=strlen(f)+3;auto e=status.find_first_not_of("0123456789",p);check(e!=p);out+=status.substr(p,e-p)+"/";}return out;}
int main(){
    char pattern[]="/tmp/ff-socket-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
    try{for(bool legacy:{false,true}){auto path=std::string(tmp)+(legacy?"/legacy":"/modern");if(legacy)Container::create_legacy(path);else Container::create(path);
        {Container store(path);Namespace fs(store);fs.initialize();fs.mksocket("/sock",0600);auto n=fs.lstat("/sock");
            check(n.socket&&!n.fifo&&!n.symlink&&!n.directory&&!n.chunked&&n.size==0&&n.allocated_size==0&&n.mode==0600&&n.links==1);
            fs.mksocket("/sticky-socket",01777);check(fs.lstat("/sticky-socket").socket&&fs.lstat("/sticky-socket").mode==01777);
            Namespace::Attributes sticky;sticky.mode=01000;fs.set_attributes("/sticky-socket",sticky);check(fs.lstat("/sticky-socket").mode==01000);fs.remove("/sticky-socket",false);
            auto stable=logical_state(store.status());rejects([&]{fs.mksocket("/sock",0777);});rejects([&]{fs.mksocket("/bad/",0777);});check(fs.lstat("/sock").id==n.id);
            rejects([&]{fs.write("/sock",Bytes{'x'});},ENOTSUP);rejects([&]{fs.replace_file("/sock",Bytes{'x'});},ENOTSUP);
            rejects([&]{fs.append("/sock",Bytes{'x'});},ENOTSUP);rejects([&]{fs.truncate("/sock",0);},ENOTSUP);
            rejects([&]{fs.punch_hole("/sock",0,1);},ENOTSUP);rejects([&]{fs.read_at("/sock",0,1);},ENOTSUP);
            if(!legacy){rejects([&]{fs.clone_file("/sock","/clone");},ENOTSUP);rejects([&]{fs.open("/sock");},ENOTSUP);}else{rejects([&]{fs.clone_file("/sock","/clone");});rejects([&]{fs.open("/sock");});}
            rejects([&]{fs.seek_region("/sock",0,true);},ESPIPE);
            Namespace::Attributes size;size.size=0;size.mode=0644;rejects([&]{fs.set_attributes("/sock",size);},EINVAL);
            check(fs.lstat("/sock").mode==0600&&logical_state(store.status())==stable);
            fs.set_xattr("/sock","user.test",Bytes{'v'});fs.link("/sock","/alias");check(fs.lstat("/alias").socket&&fs.lstat("/alias").id==n.id&&fs.lstat("/alias").links==2);
            fs.rename("/alias","/renamed");check(fs.lstat("/renamed").socket&&fs.lstat("/renamed").id==n.id);
            fs.mksocket("/replacement",0640);auto replacement=fs.lstat("/replacement");fs.rename("/replacement","/sock");
            check(fs.lstat("/sock").socket&&fs.lstat("/sock").id==replacement.id&&fs.lstat("/renamed").id==n.id&&fs.lstat("/renamed").links==1);
            fs.snapshot("socket-base");Namespace base(store,"socket-base",true);check(base.lstat("/renamed").socket&&base.lstat("/renamed").mode==0600&&base.get_xattr("/renamed","user.test")==Bytes{'v'});
            fs.remove("/renamed",false);fs.mksocket("/renamed",0777);check(fs.lstat("/renamed").socket&&fs.lstat("/renamed").id!=n.id&&base.lstat("/renamed").id==n.id);
            fs.snapshot("socket-after");fs.remove("/renamed",false);fs.write("/renamed",Bytes{'x'});
            auto diff=fs.diff("socket-base","socket-after");check(diff.find("\"before_type\":\"socket\"")!=std::string::npos&&diff.find("\"after_type\":\"socket\"")!=std::string::npos);
            fs.snapshot("socket-file");diff=fs.diff("socket-base","socket-file");check(diff.find("\"before_type\":\"socket\"")!=std::string::npos&&diff.find("\"after_type\":\"file\"")!=std::string::npos);
            fs.verify();base.verify();store.verify();if(!legacy)store.collect_garbage();store.verify();}
        {Container store(path);Namespace fs(store);fs.verify();Namespace base(store,"socket-base",true);check(base.lstat("/renamed").socket&&base.lstat("/renamed").mode==0600&&base.get_xattr("/renamed","user.test")==Bytes{'v'});base.verify();store.verify();}}
        std::filesystem::remove_all(tmp);std::cout<<"socket metadata, type guards, links, rename, snapshots, verification and GC passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
