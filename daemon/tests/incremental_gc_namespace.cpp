#include "container.h"
#include "namespace.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <regex>
#include <stdexcept>
#include <unistd.h>

using namespace forkfs;
namespace {
using Bytes=std::vector<unsigned char>;
using Model=std::map<std::string,Bytes>;
using Views=std::map<std::string,Model>;
using Revisions=std::map<std::string,Model>;
uint32_t seed=1;size_t operation=0;
#define CHECK(x) do {if(!(x))throw std::runtime_error("seed="+std::to_string(seed)+" operation="+std::to_string(operation)+": " #x);} while(0)

uint64_t field(const std::string& json,const char* name){std::smatch m;std::regex re(std::string("\\\"")+name+"\\\":([0-9]+)");CHECK(std::regex_search(json,m,re));return std::stoull(m[1]);}
bool boolean(const std::string& json,const char* name,bool value){return json.find(std::string("\"")+name+"\":"+(value?"true":"false"))!=std::string::npos;}
bool converged(const std::string& report){return !boolean(report,"mark_pending",true)&&field(report,"objects_deleted")==0&&field(report,"chunks_moved")==0&&field(report,"segments_deleted")==0&&field(report,"pending_segments")==0;}

Bytes bytes(std::mt19937& random,size_t length){Bytes value(length);for(auto& b:value)b=static_cast<unsigned char>(random());return value;}
void create_initial(Namespace& ns,Views& views){
    auto& model=views["main"];
    for(unsigned i=0;i<6;++i){auto path="/f"+std::to_string(i);Bytes value(96+i*17,static_cast<unsigned char>(i+1));ns.write(path,value);model[path.substr(1)]=std::move(value);}
    ns.write("/large",{});ns.truncate("/large",700000);Bytes patch{0x91,0x00,0x7e};ns.write_at("/large",65537,patch);
    Bytes sparse(700000,0);std::copy(patch.begin(),patch.end(),sparse.begin()+65537);model["large"]=std::move(sparse);
}
void update_at(Model& model,const std::string& name,uint64_t offset,const Bytes& value){
    auto& file=model[name];if(offset>file.size())file.resize(offset,0);if(offset+value.size()>file.size())file.resize(offset+value.size(),0);std::copy(value.begin(),value.end(),file.begin()+offset);
}
std::string choose_source(const Model& model,std::mt19937& random){auto it=model.begin();std::advance(it,random()%model.size());return it->first;}
void mutate(Namespace& ns,Model& model,std::mt19937& random,unsigned step){
    static const std::vector<std::string> names={"f0","f1","f2","f3","f4","f5","f6","f7","f8","f9"};
    unsigned op=random()%5;if(model.empty())op=0;
    if(op==0){auto name=names[random()%names.size()];auto value=bytes(random,random()%257);ns.write("/"+name,value);model[name]=std::move(value);return;}
    if(op==1){auto name=model.empty()?names[random()%names.size()]:choose_source(model,random);if(!model.contains(name)){ns.write("/"+name,{});model[name]={};}auto offset=uint64_t(random()%(model[name].size()+64));auto value=bytes(random,1+random()%24);ns.write_at("/"+name,offset,value);update_at(model,name,offset,value);return;}
    if(op==2){auto name=model.empty()?names[random()%names.size()]:choose_source(model,random);if(!model.contains(name)){ns.write("/"+name,{});model[name]={};}auto size=uint64_t(random()%2049);ns.truncate("/"+name,size);model[name].resize(size,0);return;}
    if(op==3){auto source=choose_source(model,random);auto destination=names[random()%names.size()];if(destination==source)destination=names[(std::find(names.begin(),names.end(),destination)-names.begin()+1)%names.size()];ns.rename("/"+source,"/"+destination);auto value=std::move(model.at(source));model.erase(source);model.erase(destination);model[destination]=std::move(value);return;}
    auto name=choose_source(model,random);ns.remove("/"+name,false);model.erase(name);(void)step;
}
void verify_view(Namespace& ns,const Model& expected){
    std::vector<std::string> names;for(const auto& [name,value]:expected){(void)value;names.push_back(name);}
    CHECK(ns.list("/")==names);for(const auto& [name,value]:expected)CHECK(ns.read("/"+name)==value);
}
void verify_all(Container& container,const Views& views,const Revisions& revisions){
    for(const auto& [name,model]:views){Namespace ns(container,name);verify_view(ns,model);}
    for(const auto& [name,model]:revisions){Namespace ns(container,name,true);verify_view(ns,model);}
}
void collect_one(Container& container,std::mt19937& random){
    const size_t work=7+random()%25;auto report=container.collect_step(128*1024,4,4*1024*1024,work);
    CHECK(field(report,"mark_work_done")<=work);
}
void converge(Container& container){
    for(unsigned i=0;i<4096;++i){auto report=container.collect_step(128*1024,4,4*1024*1024,128);CHECK(field(report,"mark_work_done")<=128);if(converged(report))return;}
    throw std::runtime_error("seed="+std::to_string(seed)+" operation="+std::to_string(operation)+": namespace GC did not converge");
}
void run(uint32_t seed){
    operation=0;auto directory=(std::filesystem::temp_directory_path()/"forkfs-incremental-gc-namespace-XXXXXX").string();if(!mkdtemp(directory.data()))throw std::runtime_error("cannot create namespace GC test directory");
    try{
        const auto path=directory+"/repo";Container::create(path);std::mt19937 random(seed);Views views;Revisions revisions;
        {
            Container container(path);Namespace main(container);main.initialize();create_initial(main,views);
            main.snapshot("r0");revisions["r0"]=views.at("main");main.fork("r0","child");views["child"]=revisions.at("r0");
            collect_one(container,random);
            for(operation=0;operation<88;++operation){
                if(operation==22){main.snapshot("r1");revisions["r1"]=views.at("main");main.fork("r1","branch");views["branch"]=revisions.at("r1");}
                if(operation==46){Namespace child(container,"child");child.snapshot("r2");revisions["r2"]=views.at("child");}
                if(operation==68){Namespace branch(container,"branch");branch.snapshot("r3");revisions["r3"]=views.at("branch");}
                auto it=views.begin();std::advance(it,random()%views.size());Namespace ns(container,it->first);mutate(ns,it->second,random,operation);
                collect_one(container,random);
                if(operation%4==3)verify_all(container,views,revisions);
            }
            converge(container);verify_all(container,views,revisions);container.verify();main.verify();
        }
        {
            Container reopened(path);verify_all(reopened,views,revisions);reopened.verify();Namespace main(reopened);main.verify();
        }
        std::filesystem::remove_all(directory);
    }catch(const std::exception& error){std::filesystem::remove_all(directory);throw std::runtime_error("seed="+std::to_string(seed)+" operation="+std::to_string(operation)+": "+error.what());}
}
}

int main(int argc,char** argv){try{if(argc>1)seed=static_cast<uint32_t>(std::stoul(argv[1]));run(seed);std::cout<<"namespace incremental GC model seed "<<seed<<" passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
