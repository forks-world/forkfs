#include "namespace.h"
#include "file_content.h"
#include <filesystem>
#include <random>
#include <iostream>
#include <unistd.h>
using namespace forkfs;
using Bytes=std::vector<unsigned char>;
void check_at(bool ok,int line){if(!ok)throw std::runtime_error("large file check failed at line "+std::to_string(line));}
#define check(x) check_at((x),__LINE__)
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
uint64_t disk(const std::string& path){uint64_t n=0;for(auto& e:std::filesystem::recursive_directory_iterator(path))if(e.is_regular_file())n+=e.file_size();return n;}
void content_unit(){
    using Id=FileContent::Id;std::map<Id,Bytes> objects;size_t reads=0;
    auto reader=[&](const Id& id){++reads;return objects.at(id);};
    auto persist=[&](FileContent& file){std::vector<Bytes> added;auto descriptor=file.encode(added);for(auto& b:added)objects[Journal::object_id(b)]=b;return descriptor;};
    uint64_t far=(uint64_t(1)<<40)+65535;
    FileContent file(reader,{},0,false);file.write(far,Bytes{1,2,3});auto descriptor=persist(file);
    FileContent reopened(reader,descriptor,far+3,true);reads=0;
    check(reopened.read(far-1,4)==Bytes({0,1,2,3}));check(reads<=14);reopened.verify();
    // Huge requests and overflow must be rejected without allocating file-size buffers.
    rejects([&]{reopened.read(0,UINT64_MAX);});rejects([&]{reopened.write(INT64_MAX,Bytes{1});});
    auto bad=descriptor;bad[8]^=1;rejects([&]{FileContent(reader,bad,far+3,true);});
    auto root=Id{};std::copy_n(descriptor.data()+16,32,root.begin());auto node=objects.at(root);
    auto replace_root=[&](Bytes bytes){auto hash=Journal::object_id(bytes);objects[hash]=bytes;auto copy=descriptor;std::copy(hash.begin(),hash.end(),copy.begin()+16);return copy;};
    auto invalid=node;invalid[8]=4;bad=replace_root(invalid);rejects([&]{FileContent(reader,bad,far+3,true).verify();});
    invalid=node;invalid[9]=0;invalid[10]=0;bad=replace_root(invalid);rejects([&]{FileContent(reader,bad,far+3,true).verify();});
    // Correctly hashed root referencing an absent object must fail closed.
    invalid=node;std::fill(invalid.begin()+12,invalid.begin()+44,0xff);bad=replace_root(invalid);rejects([&]{FileContent(reader,bad,far+3,true).verify();});
    // An index addressing beyond a shorter descriptor EOF is invalid.
    bad=descriptor;for(unsigned i=0;i<8;++i)bad[8+i]=static_cast<unsigned char>(uint64_t(65536)>>(8*i));
    rejects([&]{FileContent(reader,bad,65536,true).verify();});
    auto child=root;for(unsigned i=0;i<6;++i){Id next{};auto b=objects.at(child);std::copy_n(b.data()+12,32,next.begin());child=next;}
    auto saved=objects.at(child);objects[child].resize(65535);rejects([&]{FileContent(reader,descriptor,far+3,true).verify();});objects[child]=saved;
    FileContent shorter(reader,descriptor,far+3,true);shorter.resize(65537);shorter.resize(far+3);auto clean=persist(shorter);
    FileContent regrown(reader,clean,far+3,true);check(regrown.read(far,3)==Bytes(3,0));regrown.verify();
}

int main(){
    auto pattern=(std::filesystem::temp_directory_path()/"ff-large-XXXXXX").string();auto tmp=mkdtemp(pattern.data());if(!tmp)return 1;auto path=std::string(tmp)+"/store";
    try{
        content_unit();Container::create(path);std::string id;
        {Container store(path);Namespace fs(store);fs.initialize();Bytes model(3*1024*1024+71);std::mt19937 rng(173);
            for(auto& c:model)c=rng();const auto original=model;fs.write("/f",model);id=fs.stat("/f").id;check(fs.stat("/f").indexed_content);check(fs.read("/f")==model);
            fs.link("/f","/alias");fs.snapshot("base");fs.fork("base","work");Namespace work(store,"work"),old(store,"base",true);
            // Independent byte model, crossing block boundaries and truncate/regrow.
            for(unsigned i=0;i<300;++i){
                if(i%100==0)rng.seed(i==0?1u:i==100?173u:4294967295u);
                if(i%5==0){auto size=rng()%(4*1024*1024);work.truncate("/f",size);model.resize(size,0);}
                else {uint64_t at=rng()%(4*1024*1024);Bytes value(1+rng()%100000);for(auto& c:value)c=rng();work.write_at("/alias",at,value);if(at+value.size()>model.size())model.resize(at+value.size(),0);std::copy(value.begin(),value.end(),model.begin()+at);}
                auto at=model.empty()?0:rng()%model.size();auto n=std::min<size_t>(100000,model.size()-at);check(work.read_at("/f",at,n)==Bytes(model.begin()+at,model.begin()+at+n));
                if(i%10==0){check(work.read("/f")==model);work.verify();}
            }
            check(old.read("/f")==original && fs.read("/f")==original);check(work.read("/f")==model);work.write_at("/f",0,Bytes{0});check(fs.read_at("/f",0,1)==old.read_at("/f",0,1));
            uint64_t far=(uint64_t(1)<<40)+65535;auto before=disk(path);work.write_at("/f",far,Bytes{11,22,33});
            check(work.stat("/f").size==far+3 && work.read_at("/f",far-1,5)==Bytes({0,11,22,33}));
            check(disk(path)-before<1024*1024);rejects([&]{work.read("/f");});
            auto h=work.open("/f",Namespace::Access::ReadWrite);work.remove("/f",false);work.remove("/alias",false);
            work.handle_write(h,far+1,Bytes{44});check(work.handle_read(h,far,3)==Bytes({11,44,33}));work.handle_set_attributes(h,{{},{},far+100});
            work.verify();work.handle_truncate(h,65537);work.handle_truncate(h,far+3);check(work.handle_read(h,far,3)==Bytes(3,0));work.close(h);work.restore("base");check(work.read("/f")==original);work.verify();
            fs.replace_file("/replacement",original);check(fs.read("/replacement")==original);fs.remove("/replacement",false);
            fs.write("/sparse",{});fs.truncate("/sparse",INT64_MAX);fs.write_at("/sparse",INT64_MAX-1,Bytes{99});check(fs.read_at("/sparse",INT64_MAX-2,8)==Bytes({0,99}));
            auto status=store.status();rejects([&]{fs.append("/sparse",Bytes{1});});rejects([&]{fs.truncate("/sparse",UINT64_MAX);});check(store.status()==status);
            auto root=fs.root_inode().id;Namespace::Attributes a;a.size=uint64_t(1)<<40;auto created=fs.create_child(root,"created",false,a);check(created.indexed_content && fs.read_inode(created.id,a.size.value()-1,8)==Bytes{0});
            Bytes batch(FileContent::max_io,42);fs.write("/dense",batch);std::fill(batch.begin(),batch.end(),43);fs.write_at("/dense",FileContent::max_io,batch);
            check(fs.stat("/dense").size==2*FileContent::max_io);
            check(fs.read_at("/dense",FileContent::max_io-1,2)==Bytes({42,43}));
            auto growth=disk(path);fs.write_at("/dense",5*1024*1024,Bytes{17});check(disk(path)-growth<1024*1024);
            check(fs.read_at("/dense",5*1024*1024-1,3)==Bytes({42,17,42}));rejects([&]{fs.read("/dense");});
            fs.set_inode_xattr(id,"user.test",Bytes{7});check(fs.get_inode_xattr(id,"user.test")==Bytes{7});fs.verify();old.verify();store.verify();
        }
        {Container store(path);Namespace fs(store);check(fs.read_at("/sparse",INT64_MAX-1,1)==Bytes{99});check(fs.read_inode(id,0,7).size()==7);fs.verify();}
        Namespace::verify_recovery(path);std::filesystem::remove_all(tmp);return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
