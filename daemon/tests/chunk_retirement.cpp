#include "namespace.h"
#include "metadata_view.h"
#include <cstring>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using forkfs::Journal;
using Bytes=std::vector<unsigned char>;
static constexpr uint64_t chunks=5120,chunk=65536,dense_size=chunks*chunk;
static const std::string history("\0forkfs-history/",16),prefix("\0forkfs/",8);
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("chunk retirement check failed at line "+std::to_string(line));}
template<class F> static void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
static uint64_t number(const std::string& json,const std::string& key){auto at=json.find("\""+key+"\":");check(at!=json.npos);return std::stoull(json.substr(at+key.size()+3));}
static std::string hex(const std::array<unsigned char,32>& id){std::string result;const char* h="0123456789abcdef";for(auto c:id){result+=h[c>>4];result+=h[c&15];}return result;}
static void put64(Bytes& b,size_t at,uint64_t value){for(unsigned i=0;i<8;++i)b[at+i]=static_cast<unsigned char>(value>>(8*i));}
static void put32(Bytes& b,size_t at,uint32_t value){for(unsigned i=0;i<4;++i)b[at+i]=static_cast<unsigned char>(value>>(8*i));}
static Bytes flat_inode(uint32_t mode,uint64_t size,bool xattrs=false){
    Bytes b(64);memcpy(b.data(),"FFINOD02",8);put32(b,8,1);put32(b,12,mode|(xattrs?0x80000000U:0));
    put64(b,16,1);put64(b,24,size);put64(b,32,1);put64(b,40,1);put64(b,48,1);put64(b,56,1);return b;
}
static std::string suffix(uint64_t index){auto n=std::to_string(index);return index<1024?std::string(4-n.size(),'0')+n:"x"+std::string(8-n.size(),'0')+n;}
// Publish a small, independently encoded main namespace in the old flat-key
// layout. In particular, do not create the history/world/main root.
static std::string flat_retirement_fixture(const std::string& path){
    Container::create(path);std::string root;
    {Container store(path);Namespace fs(store);fs.initialize();root=fs.stat("/").id;}
    const auto victim=std::string(32,'a'),replacement=std::string(32,'b');
    Journal journal(path);check(!journal.contains(history+"world/main"));
    auto root_inode=journal.get(prefix+"inode/"+root);put64(root_inode,16,2);put64(root_inode,40,2);
    std::vector<forkfs::Mutation> batch{
        {prefix+"inode/"+root,root_inode},
        {prefix+"entry/"+root+"/victim",Bytes(victim.begin(),victim.end())},
        {prefix+"inode/"+victim,flat_inode(0644,8,true)},
        {prefix+"allocated/"+victim,Bytes{}},
        {prefix+"content/"+victim,Bytes{'o','l','d','-','d','a','t','a'}},
        {prefix+"xattr/"+victim+"/user.marker",Bytes{'X'}},
        {prefix+"entry/"+root+"/replacement",Bytes(replacement.begin(),replacement.end())},
        {prefix+"inode/"+replacement,flat_inode(0644,3)},
        {prefix+"allocated/"+replacement,Bytes{}},
        {prefix+"content/"+replacement,Bytes{'n','e','w'}}
    };
    journal.transact(journal.sequence(),batch);return victim;
}
static void flat_retirement(const std::string& path,bool replace){
    auto victim=flat_retirement_fixture(path);auto orphan=history+"orphan/main/"+victim+"/";
    {
        Container store(path);Namespace fs(store);auto before=store.status();auto handle=fs.open("/victim",Namespace::Access::ReadWrite);
        check(fs.handle_stat(handle).id==victim && fs.handle_read(handle,0,8)==Bytes({'o','l','d','-','d','a','t','a'}));
        check(number(store.status(),"last_committed_seq")==number(before,"last_committed_seq"));
        rejects([&]{store.get(history+"world/main");});
        before=store.status();if(replace)fs.rename("/replacement","/victim");else fs.remove("/victim",false);
        auto after=store.status();check(number(after,"last_committed_seq")==number(before,"last_committed_seq")+1);
        check(!store.get(history+"world/main").empty());check(store.get(orphan+"content")==Bytes({'o','l','d','-','d','a','t','a'}));
        check(fs.handle_stat(handle).id==victim && fs.handle_stat(handle).links==0);
        check(fs.handle_get_xattr(handle,"user.marker")==Bytes{'X'});
        fs.handle_write(handle,0,Bytes{'W'});fs.handle_truncate(handle,3);fs.handle_set_xattr(handle,"user.marker",Bytes{'Y'});
        check(fs.handle_stat(handle).id==victim && fs.handle_stat(handle).size==3);
        check(fs.handle_read(handle,0,8)==Bytes({'W','l','d'}));check(fs.handle_get_xattr(handle,"user.marker")==Bytes{'Y'});
        if(replace){check(fs.read("/victim")==Bytes({'n','e','w'}));check(fs.stat("/victim").id!=victim);rejects([&]{fs.stat("/replacement");});}
        else rejects([&]{fs.stat("/victim");});
        fs.verify();before=store.status();fs.close(handle);after=store.status();
        check(number(after,"last_committed_seq")==number(before,"last_committed_seq")+1);
        rejects([&]{store.get(orphan+"inode");});store.collect_garbage();fs.verify();
    }
    {Container store(path);Namespace fs(store);rejects([&]{store.get(orphan+"inode");});
        if(replace)check(fs.read("/victim")==Bytes({'n','e','w'}));else rejects([&]{fs.stat("/victim");});
        fs.verify();store.collect_garbage();store.verify();}
}
// Construct a valid dense file with independently encoded records. Stage
// immutable objects in bounded batches, then publish one World head. Reusing
// identical nonzero chunk objects keeps this 320 MiB logical fixture small.
static void fixture(const std::string& path){
    Container::create(path);std::string id;
    {Container store(path);Namespace fs(store);fs.initialize();fs.write("/dense",{});fs.write("/other",Bytes{'S'});
        fs.set_xattr("/dense","user.marker",Bytes{'X'});id=fs.stat("/dense").id;}
    {Journal j(path);forkfs::NamespaceView view(j,"main");forkfs::NamespaceView::Changes delta;
        std::map<std::string,Bytes> objects;
        auto add=[&](Bytes b){auto hash=Journal::object_id(b);objects.emplace(history+"object/"+hex(hash),std::move(b));return hash;};
        auto inode=view.get(prefix+"inode/"+id);memcpy(inode.data(),"FFINOD03",8);inode[8]=4;put64(inode,24,dense_size);
        delta[prefix+"inode/"+id]=add(inode);delta[prefix+"content/"+id]=std::nullopt;auto data=add(Bytes(chunk,0x5a));
        for(uint64_t i=0;i<chunks;++i)delta[prefix+"chunk/"+id+"/"+suffix(i)]=data;
        auto plan=view.plan(delta);for(auto& b:plan.objects)add(std::move(b));
        std::vector<forkfs::Mutation> batch;
        for(auto& [name,b]:objects){if(!j.contains(name))batch.push_back({name,std::move(b)});if(batch.size()==256){j.transact(j.sequence(),batch);batch.clear();}}
        if(!batch.empty())j.transact(j.sequence(),batch);
        j.transact(j.sequence(),{{history+"world/main",std::move(plan.manifest)}});
    }
    {Container store(path);Namespace fs(store);fs.verify();fs.snapshot("base");}
}
static void inspect(const std::string& path){
    Journal j(path);auto fields=j.keys(history+"orphan/main/");
    if(fields.empty()){std::cout<<"{\"roots\":0}\n";return;}
    check(fields.size()==2);auto pre=fields.front().substr(0,fields.front().rfind('/')+1);auto inode=j.get(pre+"inode");
    uint64_t size=0;for(unsigned i=0;i<8;++i)size|=uint64_t(inode[24+i])<<(8*i);
    auto id=pre.substr(pre.size()-33,32);forkfs::NamespaceView retained(j,j.root_object(pre+"tree"));
    auto bytes=retained.get(prefix+"chunk/"+id+"/0000");
    std::cout<<"{\"roots\":2,\"size\":"<<size<<",\"first\":"<<unsigned(bytes[0])<<"}\n";
}
static void crash(const std::string& path,const std::string& action,const char* stage){
    unsetenv("FORKFS_TEST_CRASH");Container store(path);Namespace fs(store);auto fd=fs.open("/dense",Namespace::Access::ReadWrite);
    if(action.starts_with("orphan-") || action=="last-close")fs.remove("/dense",false);
    setenv("FORKFS_TEST_CRASH",stage,1);
    if(action=="unlink")fs.remove("/dense",false);
    else if(action=="replace")fs.rename("/other","/dense");
    else if(action=="truncate" || action=="orphan-truncate")fs.handle_truncate(fd,17);
    else if(action=="orphan-write")fs.handle_write(fd,0,Bytes{0x99});
    else if(action=="last-close")fs.close(fd);
    else throw std::runtime_error("invalid crash action");
    throw std::runtime_error("fault injection did not terminate operation");
}
int main(int argc,char** argv){
    if(argc==3 && std::string(argv[1])=="--fixture"){fixture(argv[2]);return 0;}
    if(argc==3 && std::string(argv[1])=="--inspect"){inspect(argv[2]);return 0;}
    if(argc==4){crash(argv[1],argv[2],argv[3]);return 1;}
    char pattern[]="/tmp/ff-io-retire-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;auto path=std::string(tmp)+"/db";
    try{
        flat_retirement(path+"-flat-unlink",false);flat_retirement(path+"-flat-replace",true);
        fixture(path);
        {Container store(path);Namespace fs(store),old(store,"base",true);auto fd=fs.open("/dense",Namespace::Access::ReadWrite),dup=fs.dup(fd);auto id=fs.handle_stat(fd).id;
            auto pre=history+"orphan/main/"+id+"/";auto before=store.status();fs.remove("/dense",false);auto after=store.status();
            check(number(after,"last_committed_seq")==number(before,"last_committed_seq")+1);
            check(number(after,"object_roots")<number(before,"object_roots")+256);
            check(store.get(pre+"tree").size()==40 && store.get(pre+"inode").size()==64);
            check(fs.handle_stat(fd).links==0 && fs.handle_stat(fd).size==dense_size);
            check(fs.handle_read(dup,dense_size-4,8)==Bytes(4,0x5a));check(fs.handle_get_xattr(fd,"user.marker")==Bytes{'X'});
            fs.handle_write(fd,dense_size-2,Bytes{'A','B'});fs.handle_set_xattr(fd,"user.marker",Bytes{'Y'});
            Namespace::Attributes a;a.mode=0700;a.size=65537;fs.handle_set_attributes(fd,a);
            check(fs.handle_stat(fd).mode==0700 && fs.handle_stat(fd).size==65537);fs.verify();
            fs.handle_truncate(fd,dense_size);check(fs.handle_read(dup,65535,4)==Bytes({0x5a,0x5a,0,0}));
            check(fs.handle_read(dup,dense_size-4,8)==Bytes(4,0));check(fs.handle_get_xattr(fd,"user.marker")==Bytes{'Y'});
            fs.handle_remove_xattr(fd,"user.marker");check(fs.handle_list_xattrs(fd).empty());fs.verify();
            fs.write("/dense",Bytes{'N'});check(fs.handle_read(fd,0,1)==Bytes{0x5a});check(fs.read("/dense")==Bytes{'N'});
            fs.close(fd);check(!store.get(pre+"tree").empty());before=store.status();fs.close(dup);after=store.status();
            check(number(after,"last_committed_seq")==number(before,"last_committed_seq")+1);rejects([&]{store.get(pre+"tree");});
            check(old.read_at("/dense",dense_size-4,4)==Bytes(4,0x5a));check(old.get_xattr("/dense","user.marker")==Bytes{'X'});
            fs.restore("base","safety");before=store.status();fs.truncate("/dense",1);after=store.status();
            check(number(after,"last_committed_seq")==number(before,"last_committed_seq")+1);
            check(number(after,"object_roots")<number(before,"object_roots")+256);check(fs.read("/dense")==Bytes{0x5a});
            fs.restore("base","safety2");auto replaced=fs.open("/dense",Namespace::Access::ReadWrite);fs.rename("/other","/dense");
            check(fs.read("/dense")==Bytes{'S'} && fs.handle_read(replaced,dense_size-1,1)==Bytes{0x5a});fs.close(replaced);
            fs.verify();old.verify();store.collect_garbage();store.verify();}
        // Independently encode stale flat orphan roots from older daemons.
        // Startup must still recognize and release both inline/chunked layouts.
        {Journal j(path);std::vector<forkfs::Mutation> batch;
            for(unsigned kind=0;kind<2;++kind){auto id=std::string(32,kind?'f':'e');auto pre=history+"orphan/main/"+id+"/";
                Bytes inode(64);memcpy(inode.data(),"FFINOD02",8);inode[8]=kind?4:1;put64(inode,24,kind?1024*1024:6);
                batch.push_back({pre+"inode",inode});batch.push_back({pre+(kind?"chunk/0001":"content"),Bytes{'l','e','g','a','c','y'}});
                batch.push_back({pre+"xattr/user.legacy",Bytes{'X'}});
            }j.transact(j.sequence(),batch);}
        {Container store(path);Namespace fs(store),old(store,"base",true);check(fs.read("/dense")==Bytes{'S'});check(old.read_at("/dense",dense_size-1,1)==Bytes{0x5a});fs.verify();old.verify();}
        {Journal j(path);check(j.keys(history+"orphan/").empty());}
        std::filesystem::remove_all(tmp);std::cout<<"5120-chunk retirement, private orphan trees, atomic close/truncate, revisions and GC passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
