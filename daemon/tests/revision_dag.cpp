#include "revision.h"
#include <iostream>
#include <random>
using namespace forkfs;using Id=RevisionRecord::Id;using Bytes=RevisionRecord::Bytes;
#define CHECK(x) do{if(!(x))throw std::runtime_error("line "+std::to_string(__LINE__)+": " #x);}while(0)
template<class F>void rejects(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}CHECK(caught);}
Id id(unsigned n){Id v{};v[0]=n;v[1]=n>>8;return v;}
int main(){try{
    RevisionRecord r{id(500),UINT64_MAX,{id(1),id(2)}};auto bytes=r.encode();CHECK(bytes.size()==116);auto decoded=RevisionRecord::decode(bytes);CHECK(decoded.parents==r.parents && decoded.source_sequence==UINT64_MAX);
    std::swap(r.parents[0],r.parents[1]);CHECK(Journal::object_id(r.encode())!=Journal::object_id(bytes));
    for(auto b:{Bytes{},Bytes(bytes.begin(),bytes.end()-1)})rejects([&]{RevisionRecord::decode(b);});
    auto bad=bytes;bad.push_back(0);rejects([&]{RevisionRecord::decode(bad);});bad=bytes;bad[48]=65;rejects([&]{RevisionRecord::decode(bad);});
    for(auto parents:std::vector<std::vector<Id>>{{Id{}},{id(1),id(1)},std::vector<Id>(65,id(1))}){r.parents=parents;rejects([&]{r.encode();});}
    r.parents={id(1)};rejects([&]{RevisionRecord::decode(r.encode(),id(1));});
    Bytes old(80);memcpy(old.data(),"FFREV001",8);auto manifest=id(500);std::copy(manifest.begin(),manifest.end(),old.begin()+8);CHECK(RevisionRecord::decode(old).parents.empty());auto parent=id(1);std::copy(parent.begin(),parent.end(),old.begin()+40);CHECK(RevisionRecord::decode(old).parents==std::vector<Id>{parent});
    auto oid=Journal::object_id(old);CHECK(RevisionRecord::read([&](const Id&){return old;},oid).parents.size()==1);old.back()^=1;rejects([&]{RevisionRecord::read([&](const Id&){return old;},oid);});
    // Synthetic identities exercise cycles without needing SHA hash fixed points.
    for(unsigned seed=1;seed<=200;++seed){std::mt19937 rng(seed);std::map<Id,RevisionRecord> graph;
        for(unsigned n=1;n<=100;++n){std::set<Id> parents;if(n>1){parents.insert(id(n-1));for(unsigned j=0;j<4;++j)parents.insert(id(1+rng()%(n-1)));}
            graph.emplace(id(n),RevisionRecord{manifest,rng(),{parents.begin(),parents.end()}});}
        unsigned visited=0;RevisionDag dag([&](const Id& key){return graph.at(key);},[&](const Id&){++visited;});dag.validate(id(100));CHECK(visited==100);dag.validate(id(99));CHECK(visited==100);
        graph.at(id(1)).parents={id(100)};RevisionDag cycle([&](const Id& key){return graph.at(key);},[](const Id&){});rejects([&]{cycle.validate(id(100));});
        graph.at(id(1)).parents.clear();cycle.validate(id(100)); // Failed validation cannot poison a subsequent traversal.
        graph.erase(id(1));RevisionDag missing([&](const Id& key){return graph.at(key);},[](const Id&){});rejects([&]{missing.validate(id(100));});
    }
    // A daemon reuses one validator as immutable history grows: linear reads.
    unsigned reads=0;std::map<Id,RevisionRecord> growing;
    RevisionDag incremental([&](const Id& key){++reads;return growing.at(key);},[](const Id&){});
    for(unsigned n=1;n<=1000;++n){growing.emplace(id(n),RevisionRecord{manifest,n,n>1?std::vector<Id>{id(n-1)}:std::vector<Id>{}});incremental.validate(id(n));}
    CHECK(reads==1000);
    // Explicit stack handles a deep chain; budgets are a distinct incomplete result.
    auto read=[&](const Id& key){unsigned n=key[0]+256*key[1];return RevisionRecord{manifest,n,n>1?std::vector<Id>{id(n-1)}:std::vector<Id>{}};};
    RevisionDag deep(read,[](const Id&){});deep.validate(id(20000));
    for(auto limits:std::vector<std::pair<size_t,size_t>>{{2,1000000},{100,1}}){bool limited=false;RevisionDag bounded(read,[](const Id&){},limits.first,limits.second);try{bounded.validate(id(10));}catch(const RevisionDag::Limit&){limited=true;}CHECK(limited);}
    std::cout<<"revision codecs, mixed legacy, random DAGs, cycles, missing parents and resource limits passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
