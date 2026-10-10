#include "metadata_view.h"
#include "metadata_tree.h"
#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

namespace forkfs {
namespace {
using Bytes=NamespaceView::Bytes;
using Id=NamespaceView::ObjectId;
const std::string prefix("\0forkfs/",8),history("\0forkfs-history/",16);
void need(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
uint32_t u32(const Bytes& b,size_t at) {
    need(at<=b.size() && b.size()-at>=4,"truncated metadata integer");
    uint32_t n=0;for(unsigned i=0;i<4;++i)n|=uint32_t(b[at+i])<<(8*i);return n;
}
void put32(Bytes& b,size_t at,uint32_t n) {
    for(unsigned i=0;i<4;++i)b[at+i]=static_cast<unsigned char>(n>>(8*i));
}
bool magic(const Bytes& b,const char* text) {return b.size()>=12 && !memcmp(b.data(),text,8);}
}
void NamespaceView::valid_name(const std::string& name) {
    need(!name.empty() && name.size()<=64 && name!="." && name!=".." &&
        std::all_of(name.begin(),name.end(),[](unsigned char c){
            return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';
        }),"invalid World/revision name");
}
NamespaceView::~NamespaceView()=default;
NamespaceView::NamespaceView(const Journal& journal,const std::string& name,bool revision,bool validate_root):journal_(journal) {
    valid_name(name);
    legacy_=name=="main" && !revision && !journal.contains(history+"world/main");
    if(legacy_)return;
    Id manifest{};
    if(revision) {
        auto descriptor=journal.get(history+"revision/"+name);
        need(descriptor.size()==80 && !memcmp(descriptor.data(),"FFREV001",8),"invalid revision record");
        std::copy_n(descriptor.data()+8,32,manifest.begin());
    } else manifest=journal.root_object(history+"world/"+name);
    load(manifest,validate_root);
}
NamespaceView::NamespaceView(const Journal& journal,const ObjectId& manifest):journal_(journal) {load(manifest,true);}
void NamespaceView::load(const ObjectId& manifest,bool validate_root) {
    const auto& journal=journal_;
    auto b=journal.get_object(manifest);
    if(b.size()==40 && !memcmp(b.data(),"FFTREE01",8)) {
        Id root{};memcpy(root.data(),b.data()+8,32);need(root!=Id{},"empty metadata tree root");
        tree_root_=root;tree_=std::make_unique<MetadataTree>(journal,root);
    } else if(magic(b,"FFMAN001")) {
        // Old full-map manifests can themselves serve as immutable base runs.
        runs_.push_back(manifest);
    } else {
        need(magic(b,"FFMAN002"),"unsupported namespace manifest");
        auto n=u32(b,8);
        need(n>0 && n<=32 && b.size()==12+uint64_t(n)*32,"invalid manifest run count");
        std::set<Id> unique;
        for(uint32_t i=0;i<n;++i) {
            Id id{};std::copy_n(b.data()+12+i*32,32,id.begin());
            need(id!=Id{} && unique.insert(id).second,"duplicate or zero run reference");runs_.push_back(id);
        }
    }
    if(validate_root)need(lookup(prefix+"root").has_value(),"manifest missing namespace root");
}
const NamespaceView::Changes& NamespaceView::run(const Id& id) const {
    auto cached=cache_.find(id);if(cached!=cache_.end())return cached->second;
    auto b=journal_.get_object(id);
    return cache_.emplace(id,decode_run(b)).first->second;
}
NamespaceView::Changes NamespaceView::decode_run(const Bytes& b) {
    bool full=magic(b,"FFMAN001");need(full || magic(b,"FFRUN001"),"unsupported sorted run");
    auto count=u32(b,8);need(count>0 && count<=(b.size()-12)/(full?44:48),"invalid sorted run count");
    Changes result;size_t at=12;std::string previous;
    for(uint32_t i=0;i<count;++i) {
        uint32_t op=full?1:u32(b,at);if(!full)at+=4;
        auto n=u32(b,at);at+=4;
        need((op==1 || op==2) && n>=8 && n<=1024 && b.size()-at>=uint64_t(n)+32,"invalid run entry");
        std::string key(b.begin()+at,b.begin()+at+n);at+=n;
        Id target{};std::copy_n(b.data()+at,32,target.begin());at+=32;
        need(key.starts_with(prefix) && (i==0 || previous<key),"unordered run keys");previous=key;
        if(op==1){need(target!=Id{},"zero PUT reference");result.emplace(key,target);}
        else {need(target==Id{},"nonzero tombstone reference");result.emplace(key,std::nullopt);}
    }
    need(at==b.size(),"unexpected sorted run suffix");
    return result;
}
std::optional<Id> NamespaceView::lookup(const std::string& key) const {
    if(tree_)return tree_->lookup(key);
    if(legacy_)return journal_.contains(key)?std::optional<Id>(journal_.root_object(key)):std::nullopt;
    for(const auto& id:runs_) {const auto& entries=run(id);auto it=entries.find(key);if(it!=entries.end())return it->second;}
    return std::nullopt;
}
bool NamespaceView::contains(const std::string& key) const {
    const auto allocations=prefix+"allocated/";
    if(key.starts_with(allocations) &&
       (journal_.contains(key) || journal_.contains(history+"allocated/"+key.substr(allocations.size()))))return true;
    return lookup(key).has_value();
}
Bytes NamespaceView::get(const std::string& key) const {
    auto id=lookup(key);need(id.has_value(),"namespace key not found");return journal_.get_object(*id);
}
std::vector<std::string> NamespaceView::keys(const std::string& key_prefix) const {
    if(tree_)return tree_->keys(key_prefix);
    if(legacy_)return journal_.keys(key_prefix);
    Changes selected;
    // First occurrence wins, including a tombstone; never fall through a deletion.
    for(const auto& id:runs_) {
        const auto& entries=run(id);
        for(auto it=entries.lower_bound(key_prefix);it!=entries.end() && it->first.starts_with(key_prefix);++it)
            selected.emplace(it->first,it->second);
    }
    std::vector<std::string> result;for(const auto& [key,id]:selected)if(id)result.push_back(key);return result;
}
std::vector<std::string> NamespaceView::page(const std::string& prefix,const std::string& after,size_t limit) const {
    need(bool(tree_),"paged directory enumeration requires a CoW tree");return tree_->page(prefix,after,limit);
}
NamespaceView::Map NamespaceView::references() const {
    if(tree_)return tree_->entries();
    Map result;
    if(legacy_){for(const auto& key:journal_.keys(prefix))result[key]=journal_.root_object(key);return result;}
    for(auto it=runs_.rbegin();it!=runs_.rend();++it)
        for(const auto& [key,id]:run(*it)){if(id)result[key]=*id;else result.erase(key);}
    return result;
}
Bytes NamespaceView::encode_run(const Changes& changes) {
    need(!changes.empty(),"cannot encode empty run");uint64_t bytes=12;
    for(const auto& [key,id]:changes){(void)id;need(key.starts_with(prefix) && key.size()<=1024,"invalid run key");bytes+=40+key.size();}
    need(bytes<=256*1024,"sorted run exceeds 256 KiB");Bytes b(bytes);memcpy(b.data(),"FFRUN001",8);
    put32(b,8,changes.size());size_t at=12;
    for(const auto& [key,id]:changes) {
        put32(b,at,id?1:2);put32(b,at+4,key.size());at+=8;
        memcpy(b.data()+at,key.data(),key.size());at+=key.size();
        if(id)memcpy(b.data()+at,id->data(),32);at+=32;
    }
    return b;
}
Bytes NamespaceView::encode_manifest(const std::vector<Id>& runs) {
    need(!runs.empty() && runs.size()<=32,"manifest run count exceeds prototype limit");
    Bytes b(12+runs.size()*32);memcpy(b.data(),"FFMAN002",8);put32(b,8,runs.size());
    for(size_t i=0;i<runs.size();++i)memcpy(b.data()+12+i*32,runs[i].data(),32);return b;
}
NamespaceView::Plan NamespaceView::plan(const Changes& changes,bool compact) const {
    if(journal_.is_leveldb()) {
        Changes all=changes;
        if(!tree_) {for(const auto& [key,value]:references())if(!all.contains(key))all[key]=value;}
        // Plans are independent candidates; this immutable view retains its root.
        MetadataTree editor(journal_,tree_root_);auto plan=editor.apply(all);
        return {std::move(plan.objects),std::move(plan.manifest),1};
    }
    Plan result;std::vector<Id> runs=runs_;
    // Migration of a flat main namespace, and bounded full compaction, produce
    // partitioned image runs. No file content bytes are copied here.
    if(legacy_ || compact) {
        auto complete=references();
        for(const auto& [key,id]:changes){if(id)complete[key]=*id;else complete.erase(key);}
        need(complete.contains(prefix+"root"),"compaction would lose namespace root");
        runs.clear();Changes part;size_t bytes=12;
        auto flush=[&] {
            if(part.empty())return;auto b=encode_run(part);runs.push_back(Journal::object_id(b));
            result.objects.push_back(std::move(b));part.clear();bytes=12;
        };
        for(const auto& [key,id]:complete) {
            if(bytes+40+key.size()>64*1024)flush();
            part.emplace(key,id);bytes+=40+key.size();
        }
        flush();
    } else if(!changes.empty()) {
        auto b=encode_run(changes);auto id=Journal::object_id(b);
        // Identical delta is already represented at the front; avoid duplicates.
        if(runs.empty() || runs.front()!=id) {
            runs.erase(std::remove(runs.begin(),runs.end(),id),runs.end());
            runs.insert(runs.begin(),id);result.objects.push_back(std::move(b));
        }
    }
    result.manifest=encode_manifest(runs);result.run_count=runs.size();return result;
}
NamespaceView::Frozen NamespaceView::freeze() const {
    Frozen input;
    if(legacy_ || tree_)input.flat=references();
    else for(const auto& id:runs_)input.runs.push_back(journal_.get_object(id));
    return input;
}
NamespaceView::Plan NamespaceView::compact_frozen(const Frozen& input) {
    Map complete=input.flat;
    for(auto it=input.runs.rbegin();it!=input.runs.rend();++it)
        for(const auto& [key,id]:decode_run(*it)){if(id)complete[key]=*id;else complete.erase(key);}
    need(complete.contains(prefix+"root"),"compaction would lose namespace root");
    Plan result;std::vector<Id> runs;Changes part;size_t bytes=12;
    auto flush=[&] {
        if(part.empty())return;
        auto payload=encode_run(part);runs.push_back(Journal::object_id(payload));
        result.objects.push_back(std::move(payload));part.clear();bytes=12;
    };
    for(const auto& [key,id]:complete) {
        if(bytes+40+key.size()>64*1024)flush();part.emplace(key,id);bytes+=40+key.size();
    }
    flush();result.manifest=encode_manifest(runs);result.run_count=runs.size();return result;
}

}
