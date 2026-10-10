#pragma once
#include "journal.h"

namespace forkfs {
// Namespace version view: persistent CoW tree on LevelDB, immutable sorted runs
// or flat roots for the legacy single-file backend.
class MetadataTree;
class NamespaceView {
public:
    using ObjectId=std::array<unsigned char,32>;
    using Bytes=std::vector<unsigned char>;
    using Map=std::map<std::string,ObjectId>;
    using Changes=std::map<std::string,std::optional<ObjectId>>;
    struct Frozen {Map flat;std::vector<Bytes> runs;};
    static constexpr size_t soft_run_limit=16, hard_run_limit=32;
    struct Plan {std::vector<Bytes> objects;Bytes manifest;size_t run_count;};
    NamespaceView(const Journal& journal,const std::string& name,bool revision=false,bool validate_root=true);
    NamespaceView(const Journal& journal,const ObjectId& manifest);
    ~NamespaceView();
    std::vector<std::string> page(const std::string& prefix,const std::string& after,size_t limit) const;
    bool contains(const std::string& key) const;
    Bytes get(const std::string& key) const;
    std::vector<std::string> keys(const std::string& prefix) const;
    Map references() const;
    bool legacy() const {return legacy_;}
    size_t run_count() const {return tree_?1:runs_.size();}
    Plan plan(const Changes& changes,bool compact=false) const;
    Frozen freeze() const;
    static Plan compact_frozen(const Frozen& input);
    static void valid_name(const std::string& name);
private:
    const Journal& journal_;
    std::unique_ptr<MetadataTree> tree_;
    ObjectId tree_root_{};
    bool legacy_=false;
    std::vector<ObjectId> runs_;
    mutable std::map<ObjectId,Changes> cache_;
    std::optional<ObjectId> lookup(const std::string& key) const;
    const Changes& run(const ObjectId& id) const;
    void load(const ObjectId& manifest,bool validate_root);
    static Changes decode_run(const Bytes& payload);
    static Bytes encode_run(const Changes& changes);
    static Bytes encode_manifest(const std::vector<ObjectId>& runs);
};
}
