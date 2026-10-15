#pragma once
#include "metadata_view.h"
#include "revision.h"
#include "replay_format.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace forkfs::replay {
// This layer verifies revision ancestry and transitive immutable references. It
// reuses the CoW/run parsers; it is not an alternative inode/fsck implementation.
class Graph {
    using Bytes=std::vector<unsigned char>;
    using Id=NamespaceView::ObjectId;
    std::function<Bytes(const Id&)> read_;
    std::set<Id> manifests_;
    RevisionDag revisions_;
    void manifest(const Id& id){
        require(id!=Id{},"zero revision/World manifest");
        if(manifests_.contains(id))return;
        NamespaceView::verify_manifest(read_,id);manifests_.insert(id);
    }
    void revision(Id id){revisions_.validate(id);}

public:
    Graph(std::function<Bytes(const Id&)> read,uint64_t sequence):read_(std::move(read)),revisions_([this](const Id& id){return RevisionRecord::read(read_,id);},[this](const Id& id){manifest(id);}){(void)sequence;}
    void root(const std::string& key,const std::string& value){
        const std::string history("\0forkfs-history/",16);
        for(const auto& type:{std::string("revision/"),std::string("world/"),std::string("base/")}){
            auto prefix="R/"+history+type;if(!key.starts_with(prefix))continue;
            NamespaceView::valid_name(key.substr(prefix.size()));require(value.size()==32,"invalid replay namespace root");
            Id id{};std::copy(value.begin(),value.end(),id.begin());
            if(type=="world/")manifest(id);
            else if(type=="revision/")revision(id);
            else {auto b=read_(id);require(b.size()==32,"invalid replay World base");std::copy(b.begin(),b.end(),id.begin());revision(id);}
            return;
        }
    }
};
}
