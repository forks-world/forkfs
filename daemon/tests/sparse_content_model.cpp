#include "file_content.h"
#include <iostream>
#include <random>
#include <sstream>
using namespace forkfs;
using Bytes=FileContent::Bytes;
using Id=FileContent::Id;
#define CHECK(ok) do{if(!(ok))throw std::runtime_error("sparse model mismatch line "+std::to_string(__LINE__));}while(0)
// Deliberately byte-addressed: no block, radix or descriptor logic in the oracle.
struct Model {
    uint64_t size=0;std::map<uint64_t,unsigned char> bytes;
    void write(uint64_t at,const Bytes& value){if(value.empty())return;size=std::max(size,at+value.size());for(auto c:value){if(c)bytes[at]=c;else bytes.erase(at);++at;}}
    void resize(uint64_t n){size=n;bytes.erase(bytes.lower_bound(n),bytes.end());}
    Bytes read(uint64_t at,uint64_t count) const{if(at>=size)return {};Bytes out(std::min(count,size-at),0);for(auto it=bytes.lower_bound(at);it!=bytes.end() && it->first-at<out.size();++it)out[it->first-at]=it->second;return out;}
};
struct Snapshot {Bytes descriptor;Model model;bool indexed;};
int main(int argc,char** argv){
    uint64_t seed=argc>1?std::stoull(argv[1]):173;unsigned step=0;
    try{
        std::mt19937_64 rng(seed);std::map<Id,Bytes> objects;size_t reads=0;
        auto reader=[&](const Id& id){++reads;const auto& b=objects.at(id);CHECK(Journal::object_id(b)==id);return b;};
        Snapshot current{{},{},false};std::vector<Snapshot> history;
        auto save=[&](FileContent& f,Model model){std::vector<Bytes> added;auto b=f.encode(added);for(auto& value:added){auto id=Journal::object_id(value);auto [it,inserted]=objects.emplace(id,value);CHECK(inserted || it->second==value);}return Snapshot{std::move(b),std::move(model),f.indexed()};};
        auto validate=[&](const Snapshot& s,uint64_t probe){FileContent file(reader,s.descriptor,s.model.size,s.indexed);CHECK(file.size()==s.model.size);file.verify();
            CHECK(file.read(probe,1024)==s.model.read(probe,1024));CHECK(file.read(s.model.size,1024).empty());
            for(auto it=s.model.bytes.begin();it!=s.model.bytes.end();){auto begin=it->first?it->first-1:0;CHECK(file.read(begin,256)==s.model.read(begin,256));it=s.model.bytes.lower_bound(begin+256);}
        };
        std::vector<uint64_t> boundaries{0,1,262143,262144,262145,uint64_t(INT64_MAX)};
        for(unsigned bit:{16u,24u,32u,40u,48u,56u,62u})for(int delta:{-1,0,1})boundaries.push_back((uint64_t(1)<<bit)+delta);
        // Every radix carry boundary, including shrink precisely at the carry.
        for(auto at:boundaries){if(at>=uint64_t(INT64_MAX)-3)continue;FileContent file(reader,current.descriptor,current.model.size,current.indexed);auto model=current.model;
            file.write(at,Bytes{7,0,9});model.write(at,{7,0,9});current=save(file,model);validate(current,at?at-1:0);
            file.resize(at+1);model.resize(at+1);file.resize(at+3);model.resize(at+3);current=save(file,model);validate(current,at);}
        history.push_back(current);
        for(step=0;step<160;++step){
            if(step && step%17==0){validate(current,boundaries[rng()%boundaries.size()]);history.push_back(current);}
            if(step%29==28)current=history[rng()%history.size()];
            FileContent file(reader,current.descriptor,current.model.size,current.indexed);auto model=current.model;
            auto at=boundaries[rng()%boundaries.size()];auto op=rng()%4;
            if(op==0){file.resize(at);model.resize(at);}
            else {auto length=std::min<uint64_t>(1+rng()%97,uint64_t(INT64_MAX)-at);Bytes value(length,0);if(op!=1)for(auto& c:value)c=rng();file.write(at,value);model.write(at,value);}
            current=save(file,model);validate(current,at);
        }
        for(const auto& s:history)validate(s,0);
        // 256-way leaf fanout; deleting a full subtree must not read its data.
        FileContent full(reader,{},0,false);Model fanout;
        for(unsigned slot=0;slot<256;++slot){uint64_t at=uint64_t(slot)*65536;full.write(at,{1});fanout.write(at,{1});}
        auto dense=save(full,fanout);validate(dense,65535);reads=0;
        FileContent dropped(reader,dense.descriptor,dense.model.size,true);dropped.resize(0);CHECK(reads==0);
        dropped.resize(dense.model.size);CHECK(dropped.read(65535,3)==Bytes(3,0));dropped.verify();validate(dense,0);
        std::cout<<"seed="<<seed<<" sparse oracle, radix carries, immutable histories and fanout passed\n";
    }catch(const std::exception& e){std::cerr<<"seed="<<seed<<" step="<<step<<": "<<e.what()<<'\n';return 1;}
}
