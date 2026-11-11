#include "namespace.h"
#include "metadata_view.h"
#include <leveldb/db.h>
#include <leveldb/write_batch.h>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("allocation check failed at line "+std::to_string(line));}
template<class F> static void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
static std::string integer(uint64_t value){std::string b(8,0);for(unsigned i=0;i<8;++i)b[i]=static_cast<char>(value>>(8*i));return b;}
static std::unique_ptr<leveldb::DB> raw(const std::string& path){leveldb::DB* result=nullptr;auto status=leveldb::DB::Open({},path,&result);check(status.ok());return std::unique_ptr<leveldb::DB>(result);}
static std::string format(const std::string& path){auto db=raw(path);std::string value;check(db->Get({},"M/format",&value).ok());return value;}
static void legacy_format(const std::string& path){
    auto db=raw(path);std::unique_ptr<leveldb::Iterator> it(db->NewIterator({}));leveldb::WriteBatch batch;
    for(it->Seek("S/");it->Valid() && it->key().starts_with("S/");it->Next())batch.Delete(it->key());
    check(it->status().ok());batch.Put("M/format","FFLDB001");leveldb::WriteOptions write;write.sync=true;check(db->Write(write,&batch).ok());
}
int main(){
    char pattern[]="/tmp/ff-io-allocation-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
    try{
        for(bool legacy:{false,true}) {
            auto path=std::string(tmp)+(legacy?"/legacy":"/db");if(legacy)Container::create_legacy(path);else Container::create(path);
            {Container store(path);Namespace fs(store);fs.initialize();fs.write("/f",{});check(fs.stat("/f").allocated_size==0);
                fs.write("/f",Bytes{'A'});check(fs.stat("/f").allocated_size==512);fs.snapshot("inline");
                fs.truncate("/f",Namespace::maximum_file_size);check(fs.stat("/f").allocated_size==512);
                fs.write_at("/f",Namespace::maximum_file_size-1,Bytes{1});check(fs.stat("/f").allocated_size==512+65536);
                fs.write_at("/f",Namespace::maximum_file_size-1,Bytes{0});check(fs.stat("/f").allocated_size==512);
                fs.write_at("/f",65536,Bytes{'A'});check(fs.stat("/f").allocated_size==1024);fs.link("/f","/alias");
                auto old=fs.stat("/f");fs.snapshot("sparse");fs.fork("sparse","work");
                fs.write_at("/f",0,Bytes{0});Namespace::Attributes a;a.modified_ns=old.modified_ns;fs.set_attributes("/f",a);
                check(fs.stat("/f").size==old.size && fs.stat("/f").modified_ns==old.modified_ns && fs.stat("/f").allocated_size==512);
                check(fs.stat("/alias").allocated_size==512 && Namespace(store,"sparse",true).stat("/f").allocated_size==1024);
                Namespace work(store,"work");work.truncate("/f",1);check(work.stat("/f").allocated_size==512);
                auto stable=store.status();fs.stat("/f");fs.lstat("/f");check(store.status()==stable);
                if(!legacy){auto cursor=fs.open_directory("/");auto page=fs.read_directory(cursor,16,true);
                    check(page.entries.size()==2);for(const auto& entry:page.entries)check(entry.inode.allocated_size==512);}
                if(!legacy){auto fd=fs.open("/f",Namespace::Access::ReadWrite);fs.remove("/f",false);fs.remove("/alias",false);
                    check(fs.handle_stat(fd).allocated_size==512);fs.handle_write(fd,65536,Bytes{0});check(fs.handle_stat(fd).allocated_size==0);
                    fs.handle_truncate(fd,17);check(fs.handle_stat(fd).allocated_size==0);fs.close(fd);}
                fs.verify();if(!legacy)store.collect_garbage();store.verify();}
            {Container store(path);Namespace old(store,"sparse",true);check(old.stat("/f").allocated_size==1024);old.verify();}
        }
        auto path=std::string(tmp)+"/upgrade";
        {Container::create(path);Container store(path);Namespace fs(store);fs.initialize();fs.write("/f",Bytes{'L'});}
        legacy_format(path);check(format(path)=="FFLDB001");
        {Container store(path);Namespace fs(store);check(fs.stat("/f").allocated_size==512);store.verify();}
        check(format(path)=="FFLDB001");
        {Container store(path);Namespace fs(store);fs.write_at("/f",300000,Bytes{'N'});check(fs.stat("/f").allocated_size==512+37888);store.verify();}
        check(format(path)=="FFLDB002");
        std::array<unsigned char,32> data{};
        {forkfs::Journal j(path);forkfs::NamespaceView view(j,"main");auto keys=view.keys(std::string("\0forkfs/chunk/",14));check(keys.size()==2);data=*view.reference(keys.back());
            check(j.object_size(data)==37857);}
        {auto db=raw(path);leveldb::WriteOptions write;write.sync=true;check(db->Put(write,"S/"+std::string(reinterpret_cast<const char*>(data.data()),32),integer(999999)).ok());}
        {Container store(path);Namespace fs(store);rejects([&]{fs.stat("/f");});rejects([&]{store.verify();});}
        {auto db=raw(path);leveldb::WriteOptions write;write.sync=true;check(db->Put(write,"S/"+std::string(reinterpret_cast<const char*>(data.data()),32),integer(37857)).ok());}
        {Container store(path);Namespace fs(store);check(fs.stat("/f").allocated_size==512+37888);store.collect_garbage();store.verify();}
        std::filesystem::remove_all(tmp);std::cout<<"sparse allocation, per-reference sharing, revisions/orphans, size-index migration and corruption checks passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
