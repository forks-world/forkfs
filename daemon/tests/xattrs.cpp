#include "namespace.h"
#include "metadata_view.h"
#include <filesystem>
#include <iostream>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static void check(bool ok){if(!ok)throw std::runtime_error("xattr check failed");}
template<class F> static void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
// Freeze inode bytes while changing an xattr leaf: diff must use object IDs,
// including when a clock has not advanced between metadata publications.
static void attribute_identity(const std::string& path){
    Container::create(path);forkfs::InodeInfo before;
    {Container store(path);Namespace fs(store);fs.initialize();fs.write("/f",{});fs.set_xattr("/f","user.test",Bytes{1});fs.snapshot("a");before=fs.stat("/f");}
    {forkfs::Journal journal(path);forkfs::NamespaceView view(journal,"main");
        const std::string history("\0forkfs-history/",16),prefix("\0forkfs/",8);Bytes value{2};
        forkfs::NamespaceView::Changes delta{{prefix+"xattr/"+before.id+"/user.test",forkfs::Journal::object_id(value)}};auto plan=view.plan(delta);
        std::map<std::string,Bytes> objects;
        auto add=[&](const Bytes& bytes){auto hash=forkfs::Journal::object_id(bytes);std::string key=history+"object/";const char* digits="0123456789abcdef";
            for(auto c:hash){key+=digits[c>>4];key+=digits[c&15];}objects.emplace(key,bytes);};
        add(value);for(const auto& object:plan.objects)add(object);std::vector<forkfs::Mutation> batch;
        for(auto& [key,bytes]:objects)batch.push_back({key,std::move(bytes)});batch.push_back({history+"world/main",std::move(plan.manifest)});journal.transact(journal.sequence(),batch);
    }
    {Container store(path);Namespace fs(store);auto after=fs.stat("/f");check(after.id==before.id && after.changed_ns==before.changed_ns && after.modified_ns==before.modified_ns);
        fs.snapshot("b");auto diff=fs.diff("a","b");check(diff.find("\"path\":\"/f\"")!=diff.npos && diff.find("\"xattrs_changed\":true")!=diff.npos && diff.find("\"content_changed\":true")==diff.npos);fs.verify();store.verify();}
}
static void large_values(const std::string& path,bool legacy){
    if(legacy)Container::create_legacy(path);else Container::create(path);
    Bytes value(forkfs::maximum_xattr_bytes);for(size_t i=0;i<value.size();++i)value[i]=i%251;
    {
        Container store(path);Namespace fs(store);fs.initialize();fs.write("/f",Bytes{'D'});fs.link("/f","/alias");
        fs.set_xattr("/f","com.apple.ResourceFork",value);fs.snapshot("large");fs.fork("large","work");
        Namespace old(store,"large",true),work(store,"work");auto stable=store.status();
        rejects([&]{fs.set_xattr("/alias","com.apple.ResourceFork",Bytes(value.size()+1),Namespace::XattrPolicy::Replace);});
        check(store.status()==stable && fs.get_xattr("/alias","com.apple.ResourceFork")==value);
        work.set_xattr("/f","com.apple.ResourceFork",Bytes{7});work.snapshot("changed");
        check(fs.diff("large","changed").find("\"xattrs_changed\":true")!=std::string::npos);
        auto changed=value;changed.back()^=1;
        if(!legacy){
            auto fd=fs.open("/f",Namespace::Access::ReadWrite);fs.remove("/f",false);fs.remove("/alias",false);
            check(fs.handle_get_xattr(fd,"com.apple.ResourceFork")==value);
            fs.handle_set_xattr(fd,"com.apple.ResourceFork",changed);check(fs.handle_get_xattr(fd,"com.apple.ResourceFork")==changed);
            fs.close(fd);
        }else fs.set_xattr("/f","com.apple.ResourceFork",changed);
        check(old.get_xattr("/f","com.apple.ResourceFork")==value);fs.verify();old.verify();store.verify();
        if(!legacy)store.collect_garbage();
    }
    {Container store(path);Namespace old(store,"large",true);check(old.get_xattr("/alias","com.apple.ResourceFork")==value);old.verify();store.verify();}
}
int main(){
    char pattern[]="/tmp/ff-attr-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;auto path=std::string(tmp)+"/db";
    try {
        large_values(path+"-large",false);large_values(path+"-legacy",true);
        attribute_identity(path+"-identity");Container::create(path);
        {
            Container store(path);Namespace fs(store);fs.initialize();fs.write("/file",Bytes{'D'});fs.link("/file","/alias");fs.mkdir("/dir");fs.symlink("/file","/link");
            check(fs.list_xattrs("/file").empty());rejects([&]{fs.get_xattr("/file","user.test");});
            auto before=fs.stat("/file");Bytes binary{'a',0,0xff};fs.set_xattr("/file","user.test",binary,Namespace::XattrPolicy::Create);
            auto after=fs.stat("/file");check(after.xattrs && after.mode==before.mode && after.size==before.size && after.modified_ns==before.modified_ns);
            check(fs.get_xattr("/alias","user.test")==binary);auto stable=store.status();
            rejects([&]{fs.set_xattr("/file","user.test",{},Namespace::XattrPolicy::Create);});
            rejects([&]{fs.set_xattr("/file","user.missing",{},Namespace::XattrPolicy::Replace);});
            rejects([&]{fs.remove_xattr("/file","user.missing");});check(store.status()==stable);
            fs.set_xattr("/file","empty",{});check(fs.get_xattr("/file","empty").empty());fs.remove_xattr("/alias","empty");
            fs.set_xattr("/dir","目录📄",binary);fs.set_xattr("/link","user.test",Bytes{'L'});fs.set_xattr("/","root.attr",{});
            check(fs.get_xattr("/file","user.test")==binary && fs.get_xattr("/link","user.test")==Bytes{'L'});
            fs.verify();fs.snapshot("base");fs.fork("base","work");Namespace old(store,"base",true),work(store,"work");
            work.set_xattr("/alias","user.test",Bytes{'W'},Namespace::XattrPolicy::Replace);work.remove_xattr("/dir","目录📄");
            check(old.get_xattr("/alias","user.test")==binary && work.get_xattr("/file","user.test")==Bytes{'W'});
            rejects([&]{old.set_xattr("/file","new",{});});rejects([&]{old.remove_xattr("/file","user.test");});
            work.snapshot("changed");auto diff=fs.diff("base","changed");
            check(diff.find("\"xattrs_changed\":true")!=diff.npos && diff.find("\"content_changed\":true")==diff.npos);
            fs.write("/quota",{});for(unsigned i=0;i<32;++i)fs.set_xattr("/quota","a"+std::to_string(i),Bytes{static_cast<unsigned char>(i)});
            stable=store.status();rejects([&]{fs.set_xattr("/quota","overflow",{});});rejects([&]{fs.set_xattr("/quota","a0",Bytes(forkfs::maximum_xattr_bytes+1));});
            for(const auto& name:std::vector<std::string>{"",std::string(256,'n'),std::string("bad\0name",8),std::string("\xc0\x80",2)})rejects([&]{fs.set_xattr("/quota",name,{});});
            check(store.status()==stable);fs.set_xattr("/quota","a0",Bytes(forkfs::maximum_xattr_bytes,0xff),Namespace::XattrPolicy::Replace);check(fs.get_xattr("/quota","a0")==Bytes(forkfs::maximum_xattr_bytes,0xff));
            fs.remove_xattr("/quota","a1");fs.set_xattr("/quota",std::string(255,'n'),{});check(fs.list_xattrs("/quota").size()==32);
            auto quota=fs.open("/quota",Namespace::Access::Read);fs.remove("/quota",false);check(fs.handle_list_xattrs(quota).size()==32);fs.verify();fs.close(quota);
            auto handle=fs.open("/file",Namespace::Access::Read);fs.remove("/file",false);fs.remove("/alias",false);
            check(fs.handle_get_xattr(handle,"user.test")==binary);fs.handle_set_xattr(handle,"user.test",Bytes{'O'});fs.handle_remove_xattr(handle,"user.test");
            check(fs.handle_list_xattrs(handle).empty() && !fs.handle_stat(handle).xattrs);fs.handle_set_xattr(handle,"new.orphan",{});
            check(fs.handle_get_xattr(handle,"new.orphan").empty() && fs.handle_stat(handle).xattrs);fs.write("/file",Bytes{'N'});check(fs.list_xattrs("/file").empty());fs.verify();fs.close(handle);
            fs.write("/source",{});fs.write("/destination",{});fs.set_xattr("/source","attr",Bytes{'S'});fs.set_xattr("/destination","attr",Bytes{'D'});
            auto replaced=fs.open("/destination");fs.rename("/source","/destination");check(fs.get_xattr("/destination","attr")==Bytes{'S'} && fs.handle_get_xattr(replaced,"attr")==Bytes{'D'});fs.close(replaced);
            replaced=fs.open("/destination");fs.replace_file("/destination",Bytes{'R'});check(fs.list_xattrs("/destination").empty() && fs.handle_get_xattr(replaced,"attr")==Bytes{'S'});fs.close(replaced);
            fs.remove("/dir",true);fs.remove("/link",false);fs.remove_xattr("/","root.attr");check(!fs.stat("/").xattrs);fs.verify();old.verify();work.verify();store.verify();
            auto dead=fs.open("/file");fs.handle_set_xattr(dead,"expired",binary);fs.remove("/file",false); // recovery releases xattr roots
        }
        {
            Container store(path);Namespace fs(store),old(store,"base",true),work(store,"work");fs.verify();old.verify();work.verify();
            check(old.get_xattr("/dir","目录📄")==Bytes({'a',0,0xff}) && old.get_xattr("/file","user.test")==Bytes({'a',0,0xff}));
            store.collect_garbage();check(work.get_xattr("/file","user.test")==Bytes{'W'});store.verify();
        }
        std::filesystem::remove_all(tmp);std::cout<<"xattrs, binary/empty values, bounds, policies, inode flags, revisions, diff, orphan lifetime, restart and GC passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
