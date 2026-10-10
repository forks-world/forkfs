#include "metadata_tree.h"
#include <cstring>
#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>
namespace forkfs {
namespace {
using Id=MetadataTree::Id;
using Bytes=MetadataTree::Bytes;
void need(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
uint32_t u32(const unsigned char* p) {uint32_t n=0;for(unsigned i=0;i<4;++i)n|=uint32_t(p[i])<<(8*i);return n;}
bool valid_key(const std::string& key) {
    return key.size()>=8 && key.size()<=1024 && key.starts_with(std::string("\0forkfs/",8));
}
Id priority(const std::string& key) {
    std::string input="forkfs.metadata.priority.v1"+key;return Journal::object_id(Bytes(input.begin(),input.end()));
}
}
const MetadataTree::Node& MetadataTree::node(const Id& id) const {
    auto cached=cache_.find(id);if(cached!=cache_.end())return cached->second;
    auto b=store_.get_object(id);need(b.size()>=108 && !memcmp(b.data(),"FFTRNO01",8),"invalid metadata tree node");
    auto n=u32(b.data()+8);need(n>=8 && n<=1024 && b.size()==108+uint64_t(n),"invalid metadata node length");
    Node result;result.key.assign(b.begin()+108,b.end());
    need(valid_key(result.key),"invalid metadata node key");
    memcpy(result.value.data(),b.data()+12,32);memcpy(result.left.data(),b.data()+44,32);memcpy(result.right.data(),b.data()+76,32);
    need(result.value!=Id{},"zero metadata value reference");return cache_.emplace(id,std::move(result)).first->second;
}
bool MetadataTree::higher(const Node& a,const Node& b) {
    auto pa=priority(a.key),pb=priority(b.key);return pa==pb?a.key>b.key:pa>pb;
}
Id MetadataTree::make(const Node& n) {
    need(valid_key(n.key) && n.value!=Id{},"invalid metadata node");
    Bytes b(108+n.key.size());memcpy(b.data(),"FFTRNO01",8);
    for(unsigned i=0;i<4;++i)b[8+i]=static_cast<unsigned char>(n.key.size()>>(8*i));
    memcpy(b.data()+12,n.value.data(),32);memcpy(b.data()+44,n.left.data(),32);memcpy(b.data()+76,n.right.data(),32);
    memcpy(b.data()+108,n.key.data(),n.key.size());auto id=Journal::object_id(b);
    cache_[id]=n;pending_.emplace(id,std::move(b));return id;
}
unsigned MetadataTree::height(Id root,unsigned depth) const {
    if(root==Id{})return 0;
    need(depth<256,"metadata tree depth limit");
    auto known=heights_.find(root);
    if(known!=heights_.end()){need(known->second<=256-depth,"metadata tree depth limit");return known->second;}
    const auto& n=node(root);
    auto result=1+std::max(height(n.left,depth+1),height(n.right,depth+1));
    heights_.emplace(root,result);return result;
}
Id MetadataTree::merge(Id left,Id right) {
    struct Step {Node node;bool replace_right;};
    std::vector<Step> path;std::set<Id> visited;
    while(left!=Id{} && right!=Id{}) {
        auto a=node(left),b=node(right);
        if(higher(a,b)) {
            need(visited.insert(left).second,"metadata tree cycle");
            path.push_back({a,true});left=a.right;
        } else {
            need(visited.insert(right).second,"metadata tree cycle");
            path.push_back({b,false});right=b.left;
        }
    }
    auto result=left==Id{}?right:left;
    for(auto i=path.rbegin();i!=path.rend();++i) {
        if(i->replace_right)i->node.right=result;else i->node.left=result;
        result=make(i->node);
    }
    return result;
}
Id MetadataTree::set(Id root,const std::string& key,const std::optional<Id>& value) {
    struct Step {Node node;bool went_left;};
    std::vector<Step> path;std::set<Id> visited;auto current=root;Id result{};
    while(current!=Id{}) {
        need(visited.insert(current).second,"metadata tree cycle");auto n=node(current);
        if(key==n.key) {
            if(value && n.value==*value)return root;
            if(!value)result=merge(n.left,n.right);
            else {n.value=*value;result=make(n);}
            break;
        }
        auto left=key<n.key;path.push_back({n,left});current=left?n.left:n.right;
    }
    if(current==Id{}) {if(!value)return root;result=make({key,*value,{},{}});}
    for(auto i=path.rbegin();i!=path.rend();++i) {
        auto n=i->node;
        if(i->went_left) {
            n.left=result;
            if(n.left!=Id{} && higher(node(n.left),n)) {
                auto top=node(n.left);n.left=top.right;top.right=make(n);result=make(top);continue;
            }
        } else {
            n.right=result;
            if(n.right!=Id{} && higher(node(n.right),n)) {
                auto top=node(n.right);n.right=top.left;top.left=make(n);result=make(top);continue;
            }
        }
        result=make(n);
    }
    return result;
}
std::optional<Id> MetadataTree::lookup(const std::string& key) const {
    Id current=root_;unsigned depth=0;
    while(current!=Id{}) {
        need(depth++<256,"metadata tree depth limit");const auto& n=node(current);
        if(key==n.key)return n.value;current=key<n.key?n.left:n.right;
    }
    return std::nullopt;
}
std::vector<std::string> MetadataTree::keys(const std::string& prefix) const {
    std::string upper=prefix;bool bounded=false;
    for(size_t i=upper.size();i>0;--i) {
        auto c=static_cast<unsigned char>(upper[i-1]);
        if(c!=255){upper[i-1]=static_cast<char>(c+1);upper.resize(i);bounded=true;break;}
    }
    std::vector<std::string> result;
    std::function<void(Id,unsigned)> walk=[&](Id id,unsigned depth) {
        if(id==Id{})return;need(depth<256,"metadata tree depth limit");const auto& n=node(id);
        if(n.key>=prefix)walk(n.left,depth+1);
        if(n.key.starts_with(prefix))result.push_back(n.key);
        if(!bounded || n.key<upper)walk(n.right,depth+1);
    };
    walk(root_,0);return result;
}
std::vector<std::string> MetadataTree::page(const std::string& prefix,const std::string& after,size_t limit) const {
    need(limit>0 && limit<=1025 && (after.empty() || after.starts_with(prefix)),"invalid metadata page request");
    auto lower=after.empty()?prefix:after;std::string upper=prefix;bool bounded=false;
    for(size_t i=upper.size();i>0;--i){auto c=static_cast<unsigned char>(upper[i-1]);if(c!=255){upper[i-1]=static_cast<char>(c+1);upper.resize(i);bounded=true;break;}}
    std::vector<std::string> result;
    std::function<void(Id,unsigned)> walk=[&](Id id,unsigned depth) {
        if(id==Id{} || result.size()==limit)return;need(depth<256,"metadata tree depth limit");const auto& n=node(id);
        if(n.key>lower)walk(n.left,depth+1);
        if(result.size()==limit)return;
        if(n.key.starts_with(prefix) && (after.empty() || n.key>after))result.push_back(n.key);
        if(result.size()<limit && (!bounded || n.key<upper))walk(n.right,depth+1);
    };walk(root_,0);return result;
}
MetadataTree::Map MetadataTree::entries() const {
    Map result;std::set<Id> visited;
    std::function<void(Id,const std::string*,const std::string*,unsigned)> walk=[&](Id id,const std::string* lo,const std::string* hi,unsigned depth) {
        if(id==Id{})return;need(depth<256 && visited.insert(id).second,"metadata tree cycle/depth");const auto& n=node(id);
        need((!lo || *lo<n.key) && (!hi || n.key<*hi),"metadata tree ordering mismatch");
        if(n.left!=Id{})need(!higher(node(n.left),n),"metadata tree heap mismatch");
        if(n.right!=Id{})need(!higher(node(n.right),n),"metadata tree heap mismatch");
        walk(n.left,lo,&n.key,depth+1);result.emplace(n.key,n.value);walk(n.right,&n.key,hi,depth+1);
    };
    walk(root_,nullptr,nullptr,0);return result;
}
MetadataTree::Plan MetadataTree::apply(const Changes& changes) {
    const std::string namespace_root("\0forkfs/root",12);
    // Reject malformed batches before changing the editor, including absent
    // deletes and no-op updates that never reach make().
    for(const auto& [key,value]:changes) {
        need(valid_key(key),"invalid metadata change key");
        need(!value || *value!=Id{},"zero metadata value reference");
        need(key!=namespace_root || value.has_value(),"cannot delete namespace root");
    }
    need(changes.contains(namespace_root) || lookup(namespace_root).has_value(),"metadata tree missing namespace root");
    // Validate the complete candidate: rotations can deepen untouched subtrees.
    // Immutable-node heights are memoized; a cold persisted tree needs one
    // full walk per editor, while subsequent checks visit only new CoW nodes.
    auto candidate=root_;
    for(const auto& [key,value]:changes)candidate=set(candidate,key,value);
    if(candidate!=root_)height(candidate);
    Plan result;result.manifest.resize(40);memcpy(result.manifest.data(),"FFTREE01",8);memcpy(result.manifest.data()+8,candidate.data(),32);
    std::set<Id> visited;
    std::function<void(Id)> collect=[&](Id id) {
        auto it=pending_.find(id);if(it==pending_.end() || !visited.insert(id).second)return;
        auto n=node(id);collect(n.left);collect(n.right);result.objects.push_back(it->second);
    };
    collect(candidate);root_=candidate;return result;
}
}
