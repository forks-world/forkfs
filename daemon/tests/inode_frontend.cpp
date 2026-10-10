#include "namespace.h"
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace forkfs;
void check(bool ok){if(!ok)throw std::runtime_error("inode frontend regression failed");}
template<class F> void rejects(F f){bool bad=false;try{f();}catch(const std::exception&){bad=true;}check(bad);}
int main(){
    auto pattern=(std::filesystem::temp_directory_path()/"ff-inode-XXXXXX").string();auto dir=mkdtemp(pattern.data());if(!dir)return 1;
    try{
        auto path=std::string(dir)+"/store";Container::create(path);std::string file,root;
        {
            Container store(path);Namespace fs(store);fs.initialize();root=fs.root_inode().id;
            fs.mkdir("/dir");auto directory=fs.lookup_child(root,"dir");fs.write("/dir/a",{1,2,3});file=fs.lookup_child(directory.id,"a").id;
            fs.link("/dir/a","/dir/b");check(fs.lookup_child(directory.id,"b").id==file);
            fs.symlink("a","/dir/link");auto link=fs.lookup_child(directory.id,"link");check(link.symlink && fs.readlink_inode(link.id)=="a");
            rejects([&]{fs.read_inode(link.id,0,5);});rejects([&]{fs.lookup_child(root,"dir/a");});rejects([&]{fs.list_inode(directory.id,"../x",2);});
            auto page=fs.list_inode(directory.id,"",1);check(!page.eof && page.names==std::vector<std::string>{"a"} && page.entries[0].inode.id==file);
            auto rest=fs.list_inode(directory.id,"a",3);check(rest.eof && rest.names==std::vector<std::string>({"b","link"}));
            fs.snapshot("base");Namespace revision(store,"base",true);fs.rename("/dir","/moved");
            check(fs.lookup_child(root,"moved").id==directory.id && fs.lookup_child(directory.id,"a").id==file);
            fs.write_at("/moved/a",1,{9});check(fs.read_inode(file,0,UINT64_MAX)==std::vector<unsigned char>({1,9,3}));
            check(revision.read_inode(file,0,UINT64_MAX)==std::vector<unsigned char>({1,2,3}));
            check(fs.stat_inode(file).links==2 && fs.read_inode(file,2,UINT64_MAX)==std::vector<unsigned char>{3});
            fs.remove("/moved/a",false);fs.remove("/moved/b",false);rejects([&]{fs.stat_inode(file);});
            check(revision.stat_inode(file).links==2);store.verify();
        }
        {Container store(path);Namespace fs(store),revision(store,"base",true);check(fs.root_inode().id==root);rejects([&]{fs.read_inode(file,0,1);});check(revision.read_inode(file,0,3)==std::vector<unsigned char>({1,2,3}));}
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(dir);return 1;}
    std::filesystem::remove_all(dir);return 0;
}
