#include "metadata_view.h"
#include "level_store.h"
#include <cstring>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace forkfs;
void check(bool ok){if(!ok)throw std::runtime_error("metadata view regression failed");}
const std::string ns("\0forkfs/",8),history("\0forkfs-history/",16);
void publish(Journal& journal,const std::string& name,const NamespaceView::Plan& plan){
    std::vector<Mutation> batch;size_t index=0;
    for(const auto& b:plan.objects)batch.push_back({"node/"+std::to_string(journal.sequence())+"/"+std::to_string(index++),b});
    batch.push_back({history+"world/"+name,plan.manifest});journal.transact(journal.sequence(),batch);
}
int main(){
    auto pattern=(std::filesystem::temp_directory_path()/"ff-view-XXXXXX").string();auto dir=mkdtemp(pattern.data());if(!dir)return 1;
    try{
        auto path=std::string(dir)+"/store";LevelStore::create(path);NamespaceView::Map expected;
        {
            Journal journal(path);journal.put(ns+"root",{1});journal.put("value",{2});
            auto root=journal.root_object(ns+"root"),value=journal.root_object("value");expected[ns+"root"]=root;
            NamespaceView flat(journal,"main");check(flat.legacy());
            auto migrated=flat.plan({{ns+"files/a",value}});publish(journal,"main",migrated);expected[ns+"files/a"]=value;
            NamespaceView base(journal,"main");check(!base.legacy() && base.references()==expected);
            auto before=base.references();auto first=base.plan({{ns+"files/b",value}});
            check(base.references()==before && !base.contains(ns+"files/b"));
            auto retry=base.plan({{ns+"files/b",value}});check(retry.manifest==first.manifest && retry.objects==first.objects);
            auto alternative=base.plan({{ns+"files/c",value}});publish(journal,"alternative",alternative);
            NamespaceView other(journal,"alternative");check(other.contains(ns+"files/c") && !other.contains(ns+"files/b"));
            publish(journal,"main",first);expected[ns+"files/b"]=value;
            check(base.references()==before);NamespaceView current(journal,"main");check(current.references()==expected);
            check(current.page(ns+"files/","",1)==std::vector<std::string>{ns+"files/a"});
            check(current.page(ns+"files/",ns+"files/a",1)==std::vector<std::string>{ns+"files/b"});
            auto frozen=current.freeze();check(frozen.flat==expected && frozen.runs.empty());
            auto compacted=NamespaceView::compact_frozen(frozen);publish(journal,"runs",compacted);
            NamespaceView runs(journal,"runs");check(runs.references()==expected);
            check(runs.page(ns+"files/","",1)==std::vector<std::string>{ns+"files/a"});
            check(runs.page(ns+"files/",ns+"files/a",1)==std::vector<std::string>{ns+"files/b"});
            auto converted=runs.plan({});publish(journal,"converted",converted);check(NamespaceView(journal,"converted").references()==expected);
            // Revision and fork descriptors retain the exact immutable manifest.
            std::vector<unsigned char> descriptor(80);std::memcpy(descriptor.data(),"FFREV001",8);
            auto manifest=journal.root_object(history+"world/main");std::memcpy(descriptor.data()+8,manifest.data(),32);
            journal.transact(journal.sequence(),{{history+"revision/r1",descriptor},{history+"world/fork",std::nullopt,manifest}});
            check(NamespaceView(journal,"r1",true).references()==expected);
            auto forked=NamespaceView(journal,"fork").plan({{ns+"files/a",std::nullopt}});publish(journal,"fork",forked);
            check(!NamespaceView(journal,"fork").contains(ns+"files/a"));check(NamespaceView(journal,"r1",true).references()==expected);
            journal.verify();
        }
        {Journal journal(path);check(NamespaceView(journal,"main").references()==expected);check(NamespaceView(journal,"r1",true).references()==expected);check(!NamespaceView(journal,"fork").contains(ns+"files/a"));journal.verify();}
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(dir);return 1;}
    std::filesystem::remove_all(dir);return 0;
}
