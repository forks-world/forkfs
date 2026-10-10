#include "namespace.h"
#include "metadata_view.h"
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace forkfs;
void check(bool ok){if(!ok)throw std::runtime_error("revision preview regression failed");}
template<class F> void rejects(F f){bool bad=false;try{f();}catch(const std::exception&){bad=true;}check(bad);}
int main(){
    auto pattern=(std::filesystem::temp_directory_path()/"ff-preview-XXXXXX").string();auto dir=mkdtemp(pattern.data());if(!dir)return 1;
    try{
        auto path=std::string(dir)+"/store";Container::create(path);
        {Container store(path);Namespace fs(store);fs.initialize();fs.write("/f",{1,2});fs.snapshot("base");auto handle=fs.open("/f");fs.remove("/f",false);}
        const std::string prefix=std::string("\0forkfs-history/",16)+"orphan/";uint64_t sequence;size_t roots;
        {Journal j(path,true);sequence=j.sequence();roots=j.keys(prefix).size();check(roots==2);
            rejects([&]{j.put("illegal",{3});});rejects([&]{j.transact(sequence,{{"illegal",std::vector<unsigned char>{3}}});});
            rejects([&]{j.compact();});rejects([&]{j.checkpoint();});check(j.sequence()==sequence && j.keys(prefix).size()==roots);}
        {Container store(path,Container::OpenMode::RevisionPreview);Namespace revision(store,"base",true);check(revision.read("/f")==std::vector<unsigned char>({1,2}));
            rejects([&]{Namespace missing(store,"missing",true);missing.root_inode();});
            rejects([&]{store.put("illegal",{3});});rejects([&]{store.checkpoint();});rejects([&]{Namespace(store).write("/new",{3});});store.verify();}
        {Journal j(path,true);check(j.sequence()==sequence && j.keys(prefix).size()==roots);}
        {Container store(path);store.verify();}
        {Journal j(path,true);check(j.sequence()==sequence+1 && j.keys(prefix).empty());}
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(dir);return 1;}
    std::filesystem::remove_all(dir);return 0;
}
