#include "level_store.h"
#include <filesystem>
#include <iostream>
#include <iterator>
#include <random>
#include <regex>
#include <set>
#include <unistd.h>

using namespace forkfs;
namespace {
using Bytes=std::vector<unsigned char>;
using Roots=std::map<std::string,Bytes>;
#define MODEL_CHECK(test) do { if(!(test)) throw std::runtime_error("seed="+std::to_string(seed)+" step="+std::to_string(step)+": " #test); } while(0)

uint64_t report_number(const std::string& json,const char* name,uint32_t seed,size_t step){
    std::regex expression(std::string("\\\"")+name+"\\\":([0-9]+)");std::smatch match;
    if(!std::regex_search(json,match,expression))throw std::runtime_error("seed="+std::to_string(seed)+" step="+std::to_string(step)+": missing GC field "+name);
    return std::stoull(match[1]);
}
bool report_bool(const std::string& json,const char* name,const char* value){
    return json.find(std::string("\"")+name+"\":"+value)!=std::string::npos;
}
void verify_roots(LevelStore& store,const Roots& roots,uint32_t seed,size_t step){
    std::vector<std::string> expected;
    for(const auto& [key,value]:roots)expected.push_back(key);
    if(store.keys("")!=expected)throw std::runtime_error("seed="+std::to_string(seed)+" step="+std::to_string(step)+": root key set differs");
    if(store.roots()!=roots.size())throw std::runtime_error("seed="+std::to_string(seed)+" step="+std::to_string(step)+": root count differs");
    for(const auto& [key,value]:roots){
        const auto id=Journal::object_id(value);
        if(store.root(key)!=id || store.object(id)!=value)
            throw std::runtime_error("seed="+std::to_string(seed)+" step="+std::to_string(step)+": root payload differs for "+key);
    }
}
Bytes make_value(std::mt19937_64& random,size_t step,const Bytes& shared_chunk){
    static constexpr size_t sizes[]={0,1,3,255,256,257,65535,65536,65537,131072,256*1024};
    const size_t size=sizes[random()%(sizeof(sizes)/sizeof(sizes[0]))];Bytes value(size);
    if(step%4==0 && size>=shared_chunk.size()){
        for(size_t i=0;i<size;++i)value[i]=shared_chunk[i%shared_chunk.size()];
    }else for(auto& byte:value)byte=static_cast<unsigned char>(random());
    return value;
}
void run_seed(uint32_t seed){
    size_t step=0;auto directory=(std::filesystem::temp_directory_path()/"ff-online-gc-model-XXXXXX").string();
    if(!mkdtemp(directory.data()))throw std::runtime_error("cannot create online GC model directory");
    try {
        auto path=directory+"/repo";LevelStore::create(path);std::mt19937_64 random(seed);Roots roots;
        Bytes shared_chunk(65536);for(auto& byte:shared_chunk)byte=static_cast<unsigned char>(random());
        {
            LevelStore store(path);
            // A deliberately unreachable value remains readable throughout a
            // public logical pin, even after its only root is removed.
            Bytes pinned_value(20000,static_cast<unsigned char>((seed%251)+1));
            store.transact(store.sequence(),{{"pin-temp",pinned_value}});roots["pin-temp"]=pinned_value;
            auto pinned_id=Journal::object_id(pinned_value);auto pin=store.pin_objects();
            store.transact(store.sequence(),{{"pin-temp",std::nullopt}});roots.erase("pin-temp");
            const auto pin_sequence=store.sequence();
            auto pinned_report=store.collect_step(65580,3);
            MODEL_CHECK(store.sequence()==pin_sequence);
            MODEL_CHECK(report_number(pinned_report,"objects_deleted",seed,step)==0);
            MODEL_CHECK(report_bool(pinned_report,"logical_sweep_skipped","true"));
            MODEL_CHECK(store.object(pinned_id)==pinned_value);store.verify();pin.reset();

            const auto root_name=[&](uint64_t index){
                std::string key="root-"+std::to_string(index);if(index<10)key.insert(5,"0");return key;
            };
            auto write=[&](const std::string& key,const Bytes& value){
                store.transact(store.sequence(),{{key,value}});roots[key]=value;
            };
            for(step=0;step<84;++step){
                const auto key=root_name(random()%16);
                const auto operation=random()%100;
                if(roots.empty() || operation<62){
                    write(key,make_value(random,step,shared_chunk));
                }else if(operation<80){
                    auto found=roots.find(key);
                    if(found==roots.end())write(key,make_value(random,step,shared_chunk));
                    else {store.transact(store.sequence(),{{key,std::nullopt}});roots.erase(found);}
                }else{
                    auto source=roots.begin();std::advance(source,random()%roots.size());
                    const auto id=Journal::object_id(source->second);
                    store.transact(store.sequence(),{{key,std::nullopt,id}});roots[key]=source->second;
                }

                if(step%3==2){
                    const size_t budget=65580+random()%(256*1024-65580+1);
                    const size_t maximum=1+random()%7;const auto sequence=store.sequence();
                    auto report=store.collect_step(budget,maximum);
                    MODEL_CHECK(report_number(report,"bytes_copied",seed,step)<=budget);
                    MODEL_CHECK(report_number(report,"objects_deleted",seed,step)<=maximum);
                    MODEL_CHECK(store.sequence()==sequence);
                    verify_roots(store,roots,seed,step);store.verify();
                }
            }
            bool converged=false;
            for(step=84;step<340;++step){
                auto report=store.collect_step(1024*1024,128);
                MODEL_CHECK(report_number(report,"bytes_copied",seed,step)<=1024*1024);
                MODEL_CHECK(report_number(report,"objects_deleted",seed,step)<=128);
                verify_roots(store,roots,seed,step);store.verify();
                if(report_number(report,"objects_deleted",seed,step)==0 &&
                   report_number(report,"chunks_moved",seed,step)==0 &&
                   report_number(report,"segments_deleted",seed,step)==0 &&
                   report_number(report,"pending_segments",seed,step)==0){converged=true;break;}
            }
            MODEL_CHECK(converged);
            std::set<Bytes> unique;for(const auto& [key,value]:roots)unique.insert(value);
            MODEL_CHECK(store.objects()==unique.size());
            MODEL_CHECK(store.roots()==roots.size());
            verify_roots(store,roots,seed,step);store.verify();
        }
        std::filesystem::remove_all(directory);
    }catch(const std::exception& error){
        std::filesystem::remove_all(directory);
        throw std::runtime_error("seed="+std::to_string(seed)+" step="+std::to_string(step)+": "+error.what());
    }
}
}

int main(int argc,char** argv){
    const uint32_t seed=argc>1?static_cast<uint32_t>(std::stoul(argv[1])):1;
    try{run_seed(seed);std::cout<<"online GC model seed "<<seed<<" passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
