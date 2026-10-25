#include "namespace.h"
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <system_error>
#include <unistd.h>
using namespace forkfs;
using Bytes=std::vector<unsigned char>;
void check(bool ok){if(!ok)throw std::runtime_error("filename namespace regression failed");}
template<class F> void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("expected invalid filename rejection");}
int main(){
    auto pattern=(std::filesystem::temp_directory_path()/"ff-filenames-XXXXXX").string();auto temp=mkdtemp(pattern.data());if(!temp)return 1;
    auto path=std::string(temp)+"/store";
    try {
        Container::create(path);
        std::map<std::string,Bytes> expected;
        std::string unicode_dir="目录😀", composed="é", decomposed="e\xcc\x81", emoji="🧪";
        {
            Container store(path);Namespace fs(store);fs.initialize();auto root=fs.root_inode().id;
            fs.write("/ascii-source",Bytes{'A'});check(fs.read("/ascii-source")==Bytes{'A'});
            fs.mkdir("/"+unicode_dir);auto dir=fs.stat("/"+unicode_dir).id;
            const std::vector<std::string> names={composed,decomposed,emoji,"line\nbreak","tab\tname","quote'\"name","back\\slash",std::string("raw\xff",4)};
            for(size_t i=0;i<names.size();++i){auto value=Bytes{static_cast<unsigned char>(i),0x42};fs.write("/"+unicode_dir+"/"+names[i],value);expected[names[i]]=value;}
            fs.write("/"+unicode_dir+"/É",Bytes{0x45});expected["É"]={0x45};
            check(fs.read("/"+unicode_dir+"/"+names.back())==expected[names.back()]);
            check(fs.stat("/"+unicode_dir+"/"+composed).id!=fs.stat("/"+unicode_dir+"/"+decomposed).id);
            check(fs.stat("/"+unicode_dir+"/"+composed).id!=fs.stat("/"+unicode_dir+"/"+"É").id);
            auto longest=std::string(251,'a')+"😀";check(longest.size()==255);fs.write("/"+unicode_dir+"/"+longest,{9});expected[longest]={9};
            rejects([&]{fs.write("/"+unicode_dir+"/"+longest+"x",{1});});
            rejects([&]{fs.create_child(dir,std::string("bad\0name",8),false,0644);});
            rejects([&]{fs.create_child(dir,"bad/name",false,0644);});rejects([&]{fs.create_child(dir,".",false,0644);});rejects([&]{fs.create_child(dir,"..",false,0644);});
            // Exercise inode addressing, rename and hard links with non-ASCII bytes.
            auto file=fs.stat("/"+unicode_dir+"/"+composed);fs.rename_child(dir,composed,dir,"renamed🛰️",file.id);
            expected.erase(composed);expected["renamed🛰️"]={0,0x42};
            fs.link_child(file.id,root,"hard📎");auto sym_target=std::string("../hard📎");auto sym=fs.symlink_child(dir,"sym🔗",sym_target);expected["sym🔗"]=Bytes(sym_target.begin(),sym_target.end());
            check(fs.read("/"+unicode_dir+"/sym🔗")==Bytes({0,0x42}));
            std::string arbitrary_target="../"+unicode_dir+"/"+decomposed;fs.symlink_child(root,"target-link",arbitrary_target);
            check(fs.readlink("/target-link")==arbitrary_target && fs.read("/target-link")==expected[decomposed]);
            rejects([&]{fs.symlink_child(root,"nul-target",std::string("a\0b",3));});
            std::mt19937 rng(0x51a7u);for(unsigned i=0;i<140;++i){std::string name="随机"+std::to_string(rng())+"🧵";auto value=Bytes{static_cast<unsigned char>(i),static_cast<unsigned char>(i>>1)};fs.write("/"+unicode_dir+"/"+name,value);expected[name]=value;}
            // Enumerate in pages and compare every byte string against an independent oracle.
            std::map<std::string,Bytes> actual;std::string after;
            for(;;){auto page=fs.list_inode(dir,after,37);for(const auto& entry:page.entries){if(entry.inode.symlink){auto target=fs.readlink_inode(entry.inode.id);actual[entry.name]=Bytes(target.begin(),target.end());}else actual[entry.name]=fs.read_inode(entry.inode.id,0,entry.inode.size);}if(page.eof)break;check(!page.names.empty());after=page.names.back();}
            check(actual==expected && actual.size()>128);
            fs.snapshot("before");fs.fork("before","unicode-work");Namespace revision(store,"before",true),work(store,"unicode-work");
            fs.rename("/"+unicode_dir+"/renamed🛰️","/"+unicode_dir+"/final🛰️");
            check(revision.read("/"+unicode_dir+"/renamed🛰️")==Bytes({0,0x42}));check(work.read("/"+unicode_dir+"/renamed🛰️")==Bytes({0,0x42}));
            auto stable=store.status();bool readonly=false;try{revision.create_child(revision.root_inode().id,"追加",false,0644);}catch(const std::system_error& e){readonly=e.code().value()==EROFS;}check(readonly && store.status()==stable);
            check(fs.parent_inode(dir).id==root);fs.verify();revision.verify();work.verify();store.verify();
        }
        {Container store(path);Namespace fs(store),revision(store,"before",true);check(fs.read("/"+unicode_dir+"/final🛰️")==Bytes({0,0x42}));check(revision.read("/"+unicode_dir+"/renamed🛰️")==Bytes({0,0x42}));fs.verify();revision.verify();store.verify();}
        // Legacy object-journal codec remains usable without a format migration.
        auto legacy_path=std::string(temp)+"/legacy";Container::create_legacy(legacy_path);std::string legacy_dir,legacy_root;
        {Container store(legacy_path);Namespace fs(store);fs.initialize();legacy_root=fs.root_inode().id;fs.write("/ascii",Bytes{'L'});check(fs.read("/ascii")==Bytes{'L'});}
        {Container store(legacy_path);Namespace fs(store);check(fs.read("/ascii")==Bytes{'L'});fs.mkdir("/目录");legacy_dir=fs.stat("/目录").id;fs.write("/目录/名字😀",Bytes{'U'});check(fs.parent_inode(legacy_dir).id==legacy_root);fs.verify();store.verify();}
        {Container store(legacy_path);Namespace fs(store);check(fs.read("/ascii")==Bytes{'L'} && fs.read("/目录/名字😀")==Bytes{'U'});check(fs.parent_inode(fs.stat("/目录").id).id==legacy_root);fs.verify();store.verify();}
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(temp);return 1;}
    std::filesystem::remove_all(temp);return 0;
}
