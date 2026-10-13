#pragma once
#include "metadata_view.h"
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
    std::map<Id,uint64_t> revisions_;
    uint64_t sequence_;
    void manifest(const Id& id){
        require(id!=Id{},"zero revision/World manifest");
        if(manifests_.contains(id))return;
        NamespaceView::verify_manifest(read_,id);manifests_.insert(id);
    }
    void revision(Id id){
        uint64_t bound=sequence_;std::vector<std::pair<Id,uint64_t>> checked;
        require(id!=Id{},"zero revision reference");
        while(id!=Id{}){
            auto cached=revisions_.find(id);
            if(cached!=revisions_.end()){require(cached->second<bound,"nondecreasing revision ancestry sequence");break;}
            auto b=read_(id);require(b.size()==80 && !memcmp(b.data(),"FFREV001",8),"invalid replay revision descriptor");
            uint64_t seq=num(std::string(b.begin()+72,b.end()));require(seq<bound,"invalid revision ancestry sequence");
            Id root{},parent{};std::copy_n(b.data()+8,32,root.begin());std::copy_n(b.data()+40,32,parent.begin());
            manifest(root);checked.emplace_back(id,seq);id=parent;bound=seq;
        }
        for(const auto& [id,seq]:checked)revisions_.emplace(id,seq);
    }
public:
    Graph(std::function<Bytes(const Id&)> read,uint64_t sequence):read_(std::move(read)),sequence_(sequence){}
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
