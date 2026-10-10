#include "metadata_tree.h"
#include "level_store.h"
#include <cstring>
#include <algorithm>
#include <cstdio>
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
// Produce a valid depth-255 fixture with canonical deterministic priorities.
// Bounded longest-decreasing-subsequence selection avoids a hand-maintained
// list of hashes. The namespace-root leaf is left of the chain's head.
std::pair<MetadataTree::Plan,std::string> deep_fixture(const Journal& journal,const MetadataTree::Id& value,bool branching=false) {
    using Id=MetadataTree::Id;
    auto key=[](const char* kind,unsigned index){char number[16];std::snprintf(number,sizeof(number),"%08u",index);return std::string("\0forkfs/",8)+kind+number;};
    auto priority=[](const std::string& key){auto input="forkfs.metadata.priority.v1"+key;return Journal::object_id(MetadataTree::Bytes(input.begin(),input.end()));};
    std::string head;Id highest{};
    for(unsigned i=0;i<4096;++i){auto candidate=key("s/",i);auto p=priority(candidate);if(p>highest){highest=p;head=candidate;}}
    check(highest>priority(std::string("\0forkfs/root",12)));
    struct Candidate {std::string key;Id priority;size_t previous=SIZE_MAX;};
    std::vector<Candidate> candidates;std::vector<size_t> tails;
    for(unsigned i=0;i<60000 && tails.size()<255;++i) {
        auto name=key("z/",i);auto p=priority(name);if(p>=highest)continue;
        auto position=std::lower_bound(tails.begin(),tails.end(),p,[&](size_t index,const Id& value){return candidates[index].priority>value;});
        auto previous=position==tails.begin()?SIZE_MAX:*(position-1);auto index=candidates.size();
        candidates.push_back({std::move(name),p,previous});
        if(position==tails.end())tails.push_back(index);else *position=index;
    }
    check(tails.size()==255);std::vector<std::string> chain;
    for(size_t index=tails.back();index!=SIZE_MAX;index=candidates[index].previous)chain.push_back(candidates[index].key);
    std::reverse(chain.begin(),chain.end());chain.insert(chain.begin(),head);
    MetadataTree::Changes changes{{std::string("\0forkfs/root",12),value}};
    for(const auto& name:chain)changes[name]=value;
    if(branching) {
        const auto parent_priority=priority(chain[254]);bool found=false;
        for(unsigned i=0;i<200000;++i) {
            auto left=chain[253]+"/left-"+std::to_string(i);
            if(priority(left)<parent_priority){changes[left]=value;found=true;break;}
        }
        check(found);
    }
    MetadataTree editor(journal);return {editor.apply(changes),branching?chain[254]:chain.back()};
}
int main() {
    auto pattern=(std::filesystem::temp_directory_path()/"ff-tree-XXXXXX").string();auto directory=mkdtemp(pattern.data());if(!directory)return 1;
    try {
        auto path=std::string(directory)+"/store";LevelStore::create(path);
        auto key=[](const std::string& suffix){return std::string("\0forkfs/",8)+suffix;};
        MetadataTree::Id old_root{},new_root{},deep_deleted_root{},branch_deleted_root{};
        auto id=Journal::object_id({42}),changed_id=Journal::object_id({43});
        MetadataTree::Map expected,expected_branching;
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
            rejects_unchanged({{key("root"),std::nullopt}});
            rejects_unchanged({{key("aaa"),changed_id},{key("root"),std::nullopt}});
            MetadataTree empty(journal);bool missing_root=false;
            try{empty.apply({{key("aaa"),id}});}catch(const std::runtime_error&){missing_root=true;}
            check(missing_root && empty.entries().empty());
            // Returning a plan cannot discard unpublished nodes: retrying or
            // composing without publishing the first plan must stay complete.
            MetadataTree unpublished(journal,old_root);
            auto first=unpublished.apply({{key("retry-a"),changed_id}});
            auto retry=unpublished.apply({});
            check(first.manifest==retry.manifest && first.objects==retry.objects);
            auto composed=unpublished.apply({{key("retry-b"),changed_id}});
            publish(journal,"composed",composed); // Only this plan is published.
            MetadataTree committed(journal,tree_root(composed));
            check(committed.lookup(key("retry-a"))==changed_id && committed.lookup(key("retry-b"))==changed_id);
            auto committed_noop=committed.apply({{key("retry-a"),changed_id}});
            check(committed_noop.objects.empty() && committed_noop.manifest==composed.manifest);
            // The exact prefix and maximum-length valid key survive decoding
            // in a fresh editor and after reopening the store.
            MetadataTree boundary(journal,old_root);auto maximum=key(std::string(1016,'x'));
            auto boundaries=boundary.apply({{key(""),id},{maximum,changed_id}});
            publish(journal,"boundary",boundaries);
            MetadataTree fresh_boundary(journal,tree_root(boundaries));
            check(fresh_boundary.lookup(key(""))==id && fresh_boundary.lookup(maximum)==changed_id);
            check(fresh_boundary.entries().size()==103);
            auto [deep,leaf]=deep_fixture(journal,id);publish(journal,"deep",deep);
            auto deep_root=tree_root(deep);check(MetadataTree(journal,deep_root).entries().size()==257);
            MetadataTree absent_deep(journal,deep_root);
            auto absent_plan=absent_deep.apply({{leaf+"x",std::nullopt}});
            check(absent_plan.objects.empty() && absent_plan.manifest==deep.manifest);
            MetadataTree too_deep(journal,deep_root);bool depth_rejected=false;
            try{too_deep.apply({{leaf+"x",id}});}catch(const std::runtime_error& e){depth_rejected=std::string(e.what())=="metadata tree depth limit";}
            check(depth_rejected && too_deep.entries().size()==257);
            MetadataTree delete_deep(journal,deep_root);auto deleted=delete_deep.apply({{leaf,std::nullopt}});
            publish(journal,"deep-deleted",deleted);deep_deleted_root=tree_root(deleted);
            check(MetadataTree(journal,deep_deleted_root).entries().size()==256);
            check(MetadataTree(journal,branch_deleted_root).entries()==expected_branching);
            check(!MetadataTree(journal,deep_deleted_root).lookup(leaf));
            check(MetadataTree(journal,deep_root).lookup(leaf)==id);
            auto [branching,branch_parent]=deep_fixture(journal,id,true);publish(journal,"deep-branching",branching);
            auto branch_root=tree_root(branching);auto branch_before=MetadataTree(journal,branch_root).entries();
            check(branch_before.size()==258);expected_branching=branch_before;expected_branching.erase(branch_parent);
            MetadataTree branch_editor(journal,branch_root);auto branch_deleted=branch_editor.apply({{branch_parent,std::nullopt}});
            publish(journal,"deep-branch-deleted",branch_deleted);branch_deleted_root=tree_root(branch_deleted);
            check(MetadataTree(journal,branch_deleted_root).entries()==expected_branching);
            check(MetadataTree(journal,branch_root).entries()==branch_before);
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
            check(MetadataTree(journal,deep_deleted_root).entries().size()==256);
            check(MetadataTree(journal,branch_deleted_root).entries()==expected_branching);
            MetadataTree::Plan boundary_plan;boundary_plan.manifest=journal.get("boundary");
            MetadataTree boundary(journal,tree_root(boundary_plan));
            check(boundary.lookup(key(""))==id && boundary.lookup(key(std::string(1016,'x')))==changed_id);
            MetadataTree::Plan composed_plan;composed_plan.manifest=journal.get("composed");
            MetadataTree composed(journal,tree_root(composed_plan));
            check(composed.lookup(key("retry-a"))==changed_id && composed.lookup(key("retry-b"))==changed_id);
            check(MetadataTree(journal,old_root).lookup(key("test/50"))==id);
            MetadataTree fresh(journal,new_root);
            auto before=fresh.apply({});
            auto same=fresh.apply({{key("test/51"),changed_id},{key("test/50"),std::nullopt}});
            check(same.objects.empty() && same.manifest==before.manifest);journal.verify();
        }
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";std::filesystem::remove_all(directory);return 1;}
    std::filesystem::remove_all(directory);return 0;
}
