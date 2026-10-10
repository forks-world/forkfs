#include "metadata_tree.h"
#include "level_store.h"
#include <cstring>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace forkfs;
void check(bool ok) {if(!ok)throw std::runtime_error("metadata tree check failed");}
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
    char pattern[]="/tmp/ff-tree-XXXXXX";auto directory=mkdtemp(pattern);if(!directory)return 1;
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
            check(MetadataTree(journal,old_root).lookup(key("test/50"))==id);journal.verify();
        }
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";std::filesystem::remove_all(directory);return 1;}
    std::filesystem::remove_all(directory);return 0;
}
