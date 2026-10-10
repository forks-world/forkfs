#include "metadata_view.h"
#include "level_store.h"
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <source_location>
#include <unistd.h>
using namespace forkfs;
void check(bool ok,std::source_location loc=std::source_location::current()) {
    if(!ok)throw std::runtime_error("metadata manager regression at line "+std::to_string(loc.line()));
}
const std::string ns("\0forkfs/",8),history("\0forkfs-history/",16);
std::vector<Mutation> publication(const Journal& j,const std::string& world,const NamespaceView::Plan& plan) {
    std::vector<Mutation> batch;size_t index=0;
    for(const auto& object:plan.objects)batch.push_back({"test-node/"+std::to_string(j.sequence())+"/"+std::to_string(index++),object});
    batch.push_back({history+"world/"+world,plan.manifest});return batch;
}
void publish(Journal& j,const std::string& world,const NamespaceView::Plan& plan) {j.transact(j.sequence(),publication(j,world,plan));}
int main() {
    auto pattern=(std::filesystem::temp_directory_path()/"ff-meta-manager-XXXXXX").string();auto dir=mkdtemp(pattern.data());if(!dir)return 1;
    try {
        auto path=std::string(dir)+"/store";LevelStore::create(path);
        NamespaceView::Map expected;
        std::vector<std::pair<std::string,NamespaceView::Map>> revisions;
        {
            Journal j(path);j.put(ns+"root",{0});std::vector<NamespaceView::ObjectId> values;
            for(unsigned i=0;i<4;++i){auto key="value/"+std::to_string(i);j.put(key,{static_cast<unsigned char>(i+1)});values.push_back(j.root_object(key));}
            expected[ns+"root"]=j.root_object(ns+"root");publish(j,"main",NamespaceView(j,"main").plan({}));
            std::mt19937 random(0xF04C);
            for(unsigned round=0;round<200;++round) {
                NamespaceView before(j,"main");auto old_manifest=j.root_object(history+"world/main");auto old_map=expected;
                NamespaceView::Changes changes;
                auto batch_size=1+random()%5;
                for(unsigned i=0;i<batch_size;++i){auto key=ns+"files/k"+std::to_string(random()%64);changes[key]=random()%3?std::optional(values[random()%values.size()]):std::nullopt;}
                auto plan=before.plan(changes);
                check(before.references()==old_map); // Planning never modifies a captured view.
                if(round%17==0) {
                    auto stale=j.sequence();j.put("unrelated",{static_cast<unsigned char>(round)});
                    auto sequence=j.sequence();auto count=j.count();auto objects=j.objects();bool conflict=false;
                    try{j.transact(stale,publication(j,"main",plan));}catch(const std::runtime_error& e){conflict=std::string(e.what()).starts_with("transaction conflict:");}
                    check(conflict && j.sequence()==sequence && j.count()==count && j.objects()==objects);
                    check(j.root_object(history+"world/main")==old_manifest && before.references()==old_map);
                    auto retry=before.plan(changes);check(retry.manifest==plan.manifest && retry.objects==plan.objects);
                }
                for(const auto& [key,value]:changes){if(value)expected[key]=*value;else expected.erase(key);}
                publish(j,"main",plan);
                check(NamespaceView(j,"main").references()==expected);
                check(before.references()==old_map && NamespaceView(j,old_manifest).references()==old_map);
                // Page sizes vary independently of the changes and catch skipped/
                // duplicated keys or incorrect exclusive continuation boundaries.
                auto current=NamespaceView(j,"main");std::vector<std::string> observed;std::string after;
                for(unsigned page=0;page<65;++page){auto batch=current.page(ns+"files/",after,1+random()%7);if(batch.empty())break;check(after.empty() || after<batch.front());observed.insert(observed.end(),batch.begin(),batch.end());after=batch.back();}
                std::vector<std::string> wanted;for(const auto& [key,value]:expected){(void)value;if(key.starts_with(ns+"files/"))wanted.push_back(key);}check(observed==wanted);
                if(round%25==0) {
                    auto name="r"+std::to_string(round);std::vector<unsigned char> descriptor(80);std::memcpy(descriptor.data(),"FFREV001",8);
                    auto manifest=j.root_object(history+"world/main");std::memcpy(descriptor.data()+8,manifest.data(),32);j.put(history+"revision/"+name,descriptor);revisions.push_back({name,expected});
                }
                if(round%40==0){j.compact();for(const auto& [name,model]:revisions)check(NamespaceView(j,name,true).references()==model);}
            }
            // Invalid plans must not poison the view or leak durable publication.
            NamespaceView view(j,"main");auto sequence=j.sequence();bool rejected=false;
            try{view.plan({{ns+"root",std::nullopt},{ns+"files/valid",values[0]}});}catch(const std::runtime_error&){rejected=true;}
            check(rejected && j.sequence()==sequence && view.references()==expected);
            // Malformed persisted descriptors must fail closed without affecting
            // unrelated live heads. These fixtures are authored as raw format bytes.
            std::vector<std::vector<unsigned char>> malformed;
            malformed.push_back({'F','F','T','R','E','E','0','1'}); // Truncated tree manifest.
            std::vector<unsigned char> zero_tree(40);std::memcpy(zero_tree.data(),"FFTREE01",8);malformed.push_back(zero_tree);
            std::vector<unsigned char> zero_runs(12);std::memcpy(zero_runs.data(),"FFMAN002",8);malformed.push_back(zero_runs);
            std::vector<unsigned char> duplicate_runs(76);std::memcpy(duplicate_runs.data(),"FFMAN002",8);duplicate_runs[8]=2;
            std::memcpy(duplicate_runs.data()+12,values[0].data(),32);std::memcpy(duplicate_runs.data()+44,values[0].data(),32);malformed.push_back(duplicate_runs);
            for(const auto& bytes:malformed){
                auto head=j.root_object(history+"world/main");j.put(history+"world/bad",bytes);auto sequence=j.sequence();bool bad=false;
                try{NamespaceView invalid(j,"bad");invalid.references();}catch(const std::runtime_error&){bad=true;}
                check(bad && j.sequence()==sequence && j.root_object(history+"world/main")==head);
                check(NamespaceView(j,"main").references()==expected);
            }
            j.transact(j.sequence(),{{history+"world/bad",std::nullopt}});
            auto noop=view.plan({});check(noop.objects.empty());publish(j,"main",noop);check(NamespaceView(j,"main").references()==expected);j.verify();
        }
        {
            Journal j(path);check(NamespaceView(j,"main").references()==expected);
            for(const auto& [name,model]:revisions)check(NamespaceView(j,name,true).references()==model);
            j.compact();j.verify();
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(dir);return 1;}
    std::filesystem::remove_all(dir);return 0;
}
