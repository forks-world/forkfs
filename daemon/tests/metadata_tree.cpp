#include "metadata_tree.h"
#include "level_store.h"
#include <cstring>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <unistd.h>
using namespace forkfs;
void check(bool ok,std::source_location where=std::source_location::current()) {if(!ok)throw std::runtime_error("metadata tree check failed at line "+std::to_string(where.line()));}
MetadataTree::Id tree_root(const MetadataTree::Plan& plan) {
    MetadataTree::Id root{};check(plan.manifest.size()==40);
    memcpy(root.data(),plan.manifest.data()+8,32);return root;
}
void publish(Journal& journal,const std::string& name,const MetadataTree::Plan& plan) {
    std::vector<Mutation> batch;size_t n=0;
    for(const auto& object:plan.objects)batch.push_back({"node/"+std::to_string(journal.sequence())+"/"+std::to_string(n++),object});
    batch.push_back({name,plan.manifest});journal.transact(journal.sequence(),batch);
}
int main() {
    auto pattern=(std::filesystem::temp_directory_path()/"ff-tree-XXXXXX").string();auto directory=mkdtemp(pattern.data());if(!directory)return 1;
    try {
        auto path=std::string(directory)+"/store";LevelStore::create(path);
        auto key=[](const std::string& suffix){return std::string("\0forkfs/",8)+suffix;};
        MetadataTree::Id old_root{},new_root{};
        auto id=Journal::object_id({42}),changed_id=Journal::object_id({43});
        MetadataTree::Map expected;
        {
            Journal journal(path);journal.put("value",{42});journal.put("changed-value",{43});
            MetadataTree::Changes changes{{key("root"),id}};expected[key("root")]=id;
            for(int i=0;i<100;++i){changes[key("test/"+std::to_string(i))]=id;expected[key("test/"+std::to_string(i))]=id;}
            MetadataTree initial(journal);auto plan=initial.apply(changes);publish(journal,"snapshot",plan);old_root=tree_root(plan);
            MetadataTree tree(journal,old_root);check(tree.entries()==expected);
            auto unchanged=tree.apply(changes);
            check(unchanged.objects.empty() && unchanged.manifest==plan.manifest);
            auto absent=tree.apply({{key("absent"),std::nullopt}});
            check(absent.objects.empty() && absent.manifest==plan.manifest);
            // Malformed batches must reject before changing this editor, even
            // if a valid mutation sorts before the invalid mutation.
            auto rejects_unchanged=[&](const MetadataTree::Changes& invalid) {
                MetadataTree editor(journal,old_root);auto before=editor.apply({});
                bool rejected=false;try{editor.apply(invalid);}catch(const std::runtime_error&){rejected=true;}
                check(rejected && editor.entries()==tree.entries());
                auto after=editor.apply({});check(after.manifest==before.manifest && after.objects.empty());
            };
            for(const auto& invalid:std::vector<std::string>{"",std::string("\0forkfs",7),"not-a-namespace",std::string("\0forkfsX",8),key(std::string(1017,'x'))}) {
                rejects_unchanged({{invalid,id}});
                rejects_unchanged({{invalid,std::nullopt}});
                rejects_unchanged({{key("root"),changed_id},{invalid,id}});
            }
            rejects_unchanged({{key("root"),changed_id},{"zzzzzzzz",std::nullopt}});
            rejects_unchanged({{key("root"),changed_id},{key("zzzz"),MetadataTree::Id{}}});
            // The exact prefix and maximum-length valid key survive decoding
            // in a fresh editor and after reopening the store.
            MetadataTree boundary(journal,old_root);auto maximum=key(std::string(1016,'x'));
            auto boundaries=boundary.apply({{key(""),id},{maximum,changed_id}});
            publish(journal,"boundary",boundaries);
            MetadataTree fresh_boundary(journal,tree_root(boundaries));
            check(fresh_boundary.lookup(key(""))==id && fresh_boundary.lookup(maximum)==changed_id);
            check(fresh_boundary.entries().size()==103);
            // Compare bounded, ordered pagination with the full index.
            std::vector<std::string> names;std::string after;
            while(true){auto page=tree.page(key("test/"),after,7);if(page.empty())break;names.insert(names.end(),page.begin(),page.end());after=page.back();}
            check(names==tree.keys(key("test/")) && names.size()==100);
            auto updated=tree.apply({{key("test/50"),std::nullopt},{key("test/51"),changed_id},{key("test/new"),changed_id}});
            publish(journal,"head",updated);new_root=tree_root(updated);
            expected.erase(key("test/50"));expected[key("test/51")]=changed_id;expected[key("test/new")]=changed_id;
            check(tree.entries()==expected && !tree.lookup(key("test/50")));
            check(MetadataTree(journal,old_root).lookup(key("test/50"))==id);
            check(MetadataTree(journal,old_root).lookup(key("test/51"))==id);
            // The same base and changes produce byte-identical immutable trees.
            MetadataTree replay(journal,old_root);auto repeated=replay.apply({{key("test/50"),std::nullopt},{key("test/51"),changed_id},{key("test/new"),changed_id}});
            check(repeated.manifest==updated.manifest && repeated.objects==updated.objects);
        }
        {
            Journal journal(path);check(MetadataTree(journal,new_root).entries()==expected);
            MetadataTree::Plan boundary_plan;boundary_plan.manifest=journal.get("boundary");
            MetadataTree boundary(journal,tree_root(boundary_plan));
            check(boundary.lookup(key(""))==id && boundary.lookup(key(std::string(1016,'x')))==changed_id);
            check(MetadataTree(journal,old_root).lookup(key("test/50"))==id);
            MetadataTree fresh(journal,new_root);
            auto before=fresh.apply({});
            auto same=fresh.apply({{key("test/51"),changed_id},{key("test/50"),std::nullopt}});
            check(same.objects.empty() && same.manifest==before.manifest);journal.verify();
        }
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";std::filesystem::remove_all(directory);return 1;}
    std::filesystem::remove_all(directory);return 0;
}
