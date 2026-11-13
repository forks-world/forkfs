#include "namespace.h"
#include "metadata_view.h"
#include <cstring>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using forkfs::Journal;using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("file flags check failed at "+std::to_string(line));}
template<class F> static void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
static void flags(Namespace& fs,const std::string& path,uint32_t value,bool follow=true){Namespace::Attributes a;a.flags=value;if(follow)fs.set_attributes(path,a);else fs.set_link_attributes(path,a);}
static const std::string prefix("\0forkfs/",8),history("\0forkfs-history/",16);
static void identity(const std::string& path){
    Container::create(path);std::string id;uint64_t changed=0;
    {Container store(path);Namespace fs(store);fs.initialize();fs.write("/f",{});flags(fs,"/f",forkfs::file_flag_hidden);fs.snapshot("before");auto n=fs.stat("/f");id=n.id;changed=n.changed_ns;}
    {Journal j(path);forkfs::NamespaceView view(j,"main");auto bytes=view.get(prefix+"inode/"+id);
        check(bytes.size()==72 && !memcmp(bytes.data(),"FFINOD04",8));bytes[64]=1;bytes[65]=bytes[66]=bytes[67]=0;
        forkfs::NamespaceView::Changes delta{{prefix+"inode/"+id,Journal::object_id(bytes)}};auto plan=view.plan(delta);
        std::map<std::string,Bytes> objects;auto add=[&](Bytes b){auto hash=Journal::object_id(b);std::string key=history+"object/";const char* digits="0123456789abcdef";
            for(auto c:hash){key+=digits[c>>4];key+=digits[c&15];}objects.emplace(key,std::move(b));};
        add(bytes);for(auto& b:plan.objects)add(std::move(b));std::vector<forkfs::Mutation> batch;
        for(auto& [key,b]:objects)batch.push_back({key,std::move(b)});batch.push_back({history+"world/main",std::move(plan.manifest)});j.transact(j.sequence(),batch);}
    {Container store(path);Namespace fs(store);check(fs.stat("/f").flags==1 && fs.stat("/f").changed_ns==changed);fs.snapshot("after");
        auto diff=fs.diff("before","after");check(diff.find("\"metadata_changed\":true")!=diff.npos && diff.find("\"content_changed\":true")==diff.npos && diff.find("\"xattrs_changed\":true")==diff.npos);fs.verify();store.verify();}
}
int main(){
    char pattern[]="/tmp/ff-flags-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
    try{
        identity(std::string(tmp)+"/identity");
        for(bool legacy:{false,true}){
            auto path=std::string(tmp)+(legacy?"/legacy":"/db");if(legacy)Container::create_legacy(path);else Container::create(path);
            {Container store(path);Namespace fs(store);fs.initialize();fs.write("/f",Bytes{'D'});fs.link("/f","/alias");fs.mkdir("/dir");fs.symlink("/f","/link");
                auto before=fs.stat("/f");check(before.flags==0);flags(fs,"/f",(forkfs::file_flag_nodump|forkfs::file_flag_hidden));
                auto n=fs.stat("/alias");check(n.flags==(forkfs::file_flag_nodump|forkfs::file_flag_hidden) && n.modified_ns==before.modified_ns && fs.read("/f")==Bytes{'D'});
                flags(fs,"/dir",forkfs::file_flag_hidden);flags(fs,"/link",1,false);check(fs.lstat("/link").flags==1 && fs.stat("/link").flags==(forkfs::file_flag_nodump|forkfs::file_flag_hidden));
                auto stable=store.status();Namespace::Attributes bad;bad.flags=4;bad.size=0;bad.mode=0700;rejects([&]{fs.set_attributes("/f",bad);});check(store.status()==stable && fs.stat("/f").size==1 && fs.stat("/f").mode==before.mode);
                fs.snapshot("flagged");fs.fork("flagged","work");Namespace old(store,"flagged",true),work(store,"work");flags(work,"/f",0);check(work.stat("/f").flags==0 && old.stat("/f").flags==(forkfs::file_flag_nodump|forkfs::file_flag_hidden));
                rejects([&]{flags(old,"/f",0);});
                // Metadata hints do not introduce protection semantics.
                fs.write_at("/f",0,Bytes{'W'});check(fs.stat("/f").flags==(forkfs::file_flag_nodump|forkfs::file_flag_hidden));
                if(!legacy){auto fd=fs.open("/f");fs.remove("/f",false);fs.remove("/alias",false);Namespace::Attributes a;a.flags=1;fs.handle_set_attributes(fd,a);
                    check(fs.handle_stat(fd).flags==1);fs.write("/f",Bytes{'N'});check(fs.stat("/f").flags==0);a.flags=4;stable=store.status();rejects([&]{fs.handle_set_attributes(fd,a);});check(store.status()==stable && fs.handle_stat(fd).flags==1);fs.close(fd);
                    fs.write("/sparse",{});fs.truncate("/sparse",Namespace::maximum_file_size);flags(fs,"/sparse",forkfs::file_flag_hidden);check(fs.stat("/sparse").size==Namespace::maximum_file_size && fs.stat("/sparse").flags==forkfs::file_flag_hidden);}
                fs.verify();old.verify();if(!legacy)store.collect_garbage();store.verify();}
            {Container store(path);Namespace old(store,"flagged",true);check(old.stat("/alias").flags==(forkfs::file_flag_nodump|forkfs::file_flag_hidden) && old.stat("/dir").flags==forkfs::file_flag_hidden && old.lstat("/link").flags==1);old.verify();store.verify();}
        }
        std::filesystem::remove_all(tmp);std::cout<<"BSD metadata flags, atomic rejection, frozen-clock diff, legacy reads, revisions, orphans, restart and GC passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
