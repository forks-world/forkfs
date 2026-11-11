#pragma once
#include "journal.h"
namespace leveldb {class Cache;}
namespace forkfs {
// Immutable-node summaries are safe across Worlds/revisions and never use
// size/mtime as content identity. The LRU is bounded to one MiB per repository.
class AllocationCache {
public:
    struct Summary {uint64_t bytes;unsigned height;};
    AllocationCache();
    ~AllocationCache();
    bool get(const std::array<unsigned char,32>& id,Summary& value);
    void put(const std::array<unsigned char,32>& id,Summary value);
    void prune();
private:
    std::unique_ptr<leveldb::Cache> cache_;
};
// Persistent ordered index above LevelDB. It stores no WAL/SST/compaction logic.
// Content-derived priorities make the CoW treap deterministic across devices.
class MetadataTree {
public:
    using Id=std::array<unsigned char,32>;
    using Bytes=std::vector<unsigned char>;
    using Map=std::map<std::string,Id>;
    using Changes=std::map<std::string,std::optional<Id>>;
    using Range=std::pair<std::string,std::string>;
    MetadataTree(const Journal& store,Id root={}):store_(store),root_(root) {}
    std::optional<Id> lookup(const std::string& key) const;
    std::vector<std::string> keys(const std::string& prefix) const;
    std::vector<std::string> page(const std::string& prefix,const std::string& after,size_t limit) const;
    Map entries() const;
    struct Plan {Bytes manifest;std::vector<Bytes> objects;};
    // One editor per publication session. Plans may be retried or composed
    // before publication; pending nodes must remain available until then.
    // After successful publication, use a fresh editor at the committed root
    // before the next apply. Returning a Plan does not acknowledge a commit.
    // Height validation walks a cold persisted tree once per editor; immutable
    // node heights are cached for subsequent changes in that session.
    // Erase half-open ranges before applying point changes. Whole subtrees can
    // be detached without generating a mutation for every removed leaf.
    Plan apply(const Changes& changes,const std::vector<Range>& erased={});
    // Sum each referenced payload's length rounded to 512-byte units. Missing
    // sparse positions have no leaf and contribute zero; shared positions count
    // per reference, not as exclusive ownership of repository disk blocks.
    uint64_t allocated_bytes(const std::string& prefix,AllocationCache& cache) const;
private:
    struct Node {std::string key;Id value{},left{},right{};};
    const Journal& store_;
    Id root_;
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
    std::pair<Id,Id> split(Id root,const std::string& boundary);
};
}
