#pragma once
#include "journal.h"
#include <functional>
namespace forkfs {
// Persistent ordered index above LevelDB. It stores no WAL/SST/compaction logic.
// Content-derived priorities make the CoW treap deterministic across devices.
class MetadataTree {
public:
    using Id=std::array<unsigned char,32>;
    using Bytes=std::vector<unsigned char>;
    using Map=std::map<std::string,Id>;
    using Changes=std::map<std::string,std::optional<Id>>;
    using Reader=std::function<Bytes(const Id&)>;
    using Charge=std::function<void(size_t)>;
    MetadataTree(const Journal& store,Id root={}):read_([&store](const Id& id){return store.get_object(id);}),root_(root),shared_heights_(&store.tree_heights_),object_pin_(store.pin_objects()) {}
    // A shared cache requires readers from the same immutable object domain.
    MetadataTree(Reader read,Id root,TreeHeightCache* heights=nullptr,Charge charge={}):read_(std::move(read)),root_(root),shared_heights_(heights),charge_(std::move(charge)) {}
    std::optional<Id> lookup(const std::string& key) const;
    std::vector<std::string> keys(const std::string& prefix) const;
    std::vector<std::string> page(const std::string& prefix,const std::string& after,size_t limit) const;
    Map entries() const;
    struct Plan {Bytes manifest;std::vector<Bytes> objects;};
    // One editor per publication session. Plans may be retried or composed
    // before publication; pending nodes must remain available until then.
    // After successful publication, use a fresh editor at the committed root
    // before the next apply. Returning a Plan does not acknowledge a commit.
    // Persisted subtree heights are reused across editors in this repository.
    // A cold/evicted subtree is validated before its height can be reused.
    Plan apply(const Changes& changes);
private:
    struct Node {std::string key;Id value{},left{},right{};};
    Reader read_;
    Id root_;
    TreeHeightCache* shared_heights_=nullptr;
    Charge charge_;
    std::shared_ptr<void> object_pin_;
    mutable std::map<Id,Node> cache_;
    mutable std::map<Id,unsigned> heights_;
    std::map<Id,Bytes> pending_;
    const Node& node(const Id& id) const;
    static bool higher(const Node& a,const Node& b);
    Id make(const Node& node);
    unsigned height(Id root,unsigned depth=0) const;
    // Editing is iterative: intermediate batch trees may exceed the final limit.
    Id set(Id root,const std::string& key,const std::optional<Id>& value);
    Id merge(Id left,Id right);
};
}
