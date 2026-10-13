#include "namespace.h"
#include <filesystem>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>
using namespace forkfs;using Bytes=std::vector<unsigned char>;
void check(bool ok){if(!ok)throw std::runtime_error("inode crash recovery regression failed");}
void baseline(const InodeInfo& n){check(n.mode==0644 && n.size==3 && n.uid==100 && n.gid==200 && n.modified_ns==33 && n.accessed_ns==22 && n.created_ns==11);}
int main(int argc,char** argv){
    try{if(argc==5 && std::string(argv[1])=="child"){
        Container store(argv[2]);Namespace fs(store);auto id=fs.stat("/file").id;int op=std::stoi(argv[3]);std::string handle;
        if(op==3){handle=fs.open_inode(id,Namespace::Access::ReadWrite);fs.remove("/file",false);fs.remove("/alias",false);}
        check(setenv("FORKFS_TEST_CRASH",argv[4],1)==0);
        if(op==0)fs.set_inode_attributes(id,{0600,333,8,501,20,444,555});
        else if(op==1)fs.set_inode_xattr(id,"user.binary",{1,0,2},Namespace::XattrMode::Replace);
        else if(op==2)fs.remove_inode_xattr(id,"user.binary");
        else if(op==3)fs.set_handle_xattr(handle,"user.binary",{9});else return 98;
        return 99; // A missing fault hook is a failure, never a passing recovery test.
    }}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 97;}
    auto pattern=(std::filesystem::temp_directory_path()/"ff-inode-recovery-XXXXXX").string();auto tmp=mkdtemp(pattern.data());if(!tmp)return 1;
    try{auto seed=std::string(tmp)+"/seed";Container::create(seed);std::string id;
        {Container store(seed);Namespace fs(store);fs.initialize();fs.write("/file",{'A','B','C'});id=fs.stat("/file").id;fs.link("/file","/alias");
            fs.set_inode_attributes(id,{0644,33,3,100,200,22,11});fs.set_inode_xattr(id,"user.binary",{0,255});fs.set_inode_xattr(id,"user.empty",{});
            fs.snapshot("base");fs.fork("base","work");fs.verify();store.verify();}
        for(int op=0;op<4;++op)for(const char* stage:{"leveldb_before_write","leveldb_after_write"}){
            std::cerr<<"recovery op="<<op<<" stage="<<stage<<'\n';auto path=std::string(tmp)+"/case"+std::to_string(op)+stage;
            std::filesystem::copy(seed,path,std::filesystem::copy_options::recursive);uint64_t sequence;
            {Journal journal(path);sequence=journal.sequence();}auto number=std::to_string(op);
            char* args[]={argv[0],const_cast<char*>("child"),path.data(),number.data(),const_cast<char*>(stage),nullptr};
            pid_t pid=fork();check(pid>=0);if(pid==0){execv(argv[0],args);_exit(127);}int status;check(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==86);
            bool committed=std::string(stage)=="leveldb_after_write";
            if(op!=3){Journal journal(path);check(journal.sequence()==sequence+(committed?1:0));journal.verify();}
            for(unsigned reopen=0;reopen<2;++reopen){Container store(path);Namespace fs(store),revision(store,"base",true),work(store,"work");
                for(auto* frozen:{&revision,&work}){baseline(frozen->stat_inode(id));check(frozen->read_inode(id,0,99)==Bytes({'A','B','C'}));check(frozen->get_inode_xattr(id,"user.binary")==Bytes({0,255}));frozen->verify();}
                if(op==3){check(fs.list("/").empty());}
                else{auto n=fs.stat_inode(id);
                    if(op==0 && committed){check(n.mode==0600 && n.size==8 && n.uid==501 && n.gid==20 && n.modified_ns==333 && n.accessed_ns==444 && n.created_ns==555);check(fs.read("/alias")==Bytes({'A','B','C',0,0,0,0,0}));}
                    else{baseline(n);check(fs.read("/alias")==Bytes({'A','B','C'}));}
                    if(op==2 && committed)check(fs.list_inode_xattrs(id)==std::vector<std::string>{"user.empty"});
                    else check(fs.get_inode_xattr(id,"user.binary")==((op==1 && committed)?Bytes({1,0,2}):Bytes({0,255})));
                    check(fs.get_inode_xattr(id,"user.empty").empty());
                }
                fs.verify();store.verify();
            }
            {Journal journal(path);check(journal.keys(std::string("\0forkfs-history/orphan/",23)).empty());}
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
    std::filesystem::remove_all(tmp);return 0;
}
