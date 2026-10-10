#include "namespace.h"
#include <atomic>
#include <barrier>
#include <filesystem>
#include <iostream>
#include <set>
#include <thread>
#include <unistd.h>
using Bytes=std::vector<unsigned char>;
void check(bool ok){if(!ok)throw std::runtime_error("file handle check failed");}
template<class F> void rejects(F f){bool bad=false;try{f();}catch(const std::exception&){bad=true;}check(bad);}
int main(){
    auto pattern=(std::filesystem::temp_directory_path()/"ff-handles-XXXXXX").string();auto tmp=mkdtemp(pattern.data());if(!tmp)return 1;
    auto path=std::string(tmp)+"/store";std::string stale;
    try{
        forkfs::Container::create(path);
        {
            forkfs::Container store(path);forkfs::Namespace fs(store);fs.initialize();fs.write("/f",Bytes{'o','l','d'});
            fs.link("/f","/alias");fs.snapshot("base");fs.fork("base","work");
            forkfs::Namespace work(store,"work"),revision(store,"base",true);
            auto a=fs.open("/f",forkfs::Namespace::Access::ReadWrite),b=fs.open("/alias");
            auto ro=revision.open("/f"),wh=work.open("/f",forkfs::Namespace::Access::ReadWrite);
            auto id=fs.handle_stat(a).id;auto orphan=std::string("\0forkfs-history/",16)+"orphan/main/"+id+"/";fs.rename("/f","/renamed");fs.handle_write(a,1,Bytes{'X'});
            check(fs.read("/alias")==Bytes({'o','X','d'}));
            fs.remove("/renamed",false);check(fs.handle_stat(a).links==1);
            fs.remove("/alias",false);check(fs.handle_stat(a).links==0 && fs.list("/").empty());
            fs.handle_write(a,5,Bytes{'!'});check(fs.handle_read(b,0,UINT64_MAX)==Bytes({'o','X','d',0,0,'!'}));
            rejects([&]{fs.handle_write(b,0,Bytes{1});});rejects([&]{work.handle_read(a,0,5);});
            rejects([&]{fs.handle_read(a,UINT64_MAX,5);});rejects([&]{revision.open("/f",forkfs::Namespace::Access::Write);});
            check(fs.handle_append(a,Bytes{'?'})==6);fs.handle_truncate(a,4);
            fs.snapshot("after-unlink");fs.fork("after-unlink","empty");forkfs::Namespace empty(store,"empty"),after(store,"after-unlink",true);
            check(empty.list("/").empty());after.verify();fs.verify();store.verify();
            fs.write("/f",Bytes{'n','e','w'});check(fs.stat("/f").id!=id);
            check(fs.handle_read(a,0,20)==Bytes({'o','X','d',0}));check(fs.read("/f")==Bytes({'n','e','w'}));
            check(work.handle_read(wh,0,20)==Bytes({'o','l','d'}));check(revision.handle_read(ro,0,20)==Bytes({'o','l','d'}));
            fs.close(a);check(fs.handle_read(b,0,20)==Bytes({'o','X','d',0}));
            check(store.get(orphan+"content")==Bytes({'o','X','d',0}));fs.close(b);
            rejects([&]{store.get(orphan+"inode");});rejects([&]{store.get(orphan+"content");});
            rejects([&]{fs.handle_stat(a);});rejects([&]{fs.close(b);});fs.verify();
            // Rename replacement retains the old destination inode for its handles.
            fs.write("/dst",Bytes{'D'});auto dst=fs.open("/dst",forkfs::Namespace::Access::ReadWrite);
            fs.rename("/f","/dst");check(fs.read("/dst")==Bytes({'n','e','w'}));check(fs.handle_stat(dst).links==0);
            fs.handle_write(dst,0,Bytes{'V'});check(fs.handle_read(dst,0,5)==Bytes{'V'});fs.close(dst);
            // Concurrent append/unlink cannot lose the inode or the writes.
            fs.write("/race",{});auto race=fs.open("/race",forkfs::Namespace::Access::ReadWrite);std::atomic<bool> failed=false;
            std::thread writer([&]{try{for(int i=0;i<32;++i)fs.handle_append(race,Bytes{42});}catch(...){failed=true;}});
            fs.remove("/race",false);writer.join();check(!failed && fs.handle_read(race,0,100)==Bytes(32,42));fs.close(race);
            auto wo=fs.open("/dst",forkfs::Namespace::Access::Write);rejects([&]{fs.handle_read(wo,0,1);});fs.close(wo);
            rejects([&]{fs.open("/");});
            work.close(wh);revision.close(ro);
            // Create/open and truncate pin the selected inode atomically.
            forkfs::Namespace::OpenOptions create{true,false,false,0600},exclusive{true,true,false,0600};
            auto created=fs.open("/created",forkfs::Namespace::Access::ReadWrite,exclusive);
            check(fs.stat("/created").mode==0600 && fs.handle_read(created,0,10).empty());
            fs.handle_write(created,0,Bytes{'C'});auto same=fs.open("/created",forkfs::Namespace::Access::Read,create);
            check(fs.handle_stat(same).id==fs.handle_stat(created).id);
            auto stable=store.status();rejects([&]{fs.open("/created",forkfs::Namespace::Access::ReadWrite,exclusive);});
            check(store.status()==stable && fs.read("/created")==Bytes{'C'});
            forkfs::Namespace::OpenOptions trunc{false,false,true,0644};
            auto truncating=fs.open("/created",forkfs::Namespace::Access::Write,trunc);
            check(fs.handle_read(created,0,10).empty());
            rejects([&]{fs.open("/created",forkfs::Namespace::Access::Read,trunc);});
            rejects([&]{revision.open("/f",forkfs::Namespace::Access::Read,create);});
            rejects([&]{fs.open("/created",forkfs::Namespace::Access::ReadWrite,{false,true,false,0644});});
            rejects([&]{fs.open("/",forkfs::Namespace::Access::ReadWrite,create);});
            fs.close(truncating);fs.close(same);fs.close(created);fs.remove("/created",false);
            std::atomic<int> wins=0,conflicts=0;std::vector<std::thread> creators;
            for(int i=0;i<8;++i)creators.emplace_back([&]{try{
                auto h=fs.open("/create-race",forkfs::Namespace::Access::ReadWrite,exclusive);++wins;fs.close(h);
            }catch(const std::exception& e){if(std::string(e.what())=="path already exists")++conflicts;else failed=true;}});
            for(auto& t:creators)t.join();check(!failed && wins==1 && conflicts==7);
            fs.remove("/create-race",false);fs.verify();
            // Append-open selects EOF for every write, including after unlink.
            fs.write("/append-test",Bytes{'B'});forkfs::Namespace::OpenOptions app;app.append=true;
            rejects([&]{fs.open("/append-test",forkfs::Namespace::Access::Read,app);});
            rejects([&]{revision.open("/f",forkfs::Namespace::Access::Read,app);});
            std::vector<std::string> appenders;
            for(int i=0;i<8;++i)appenders.push_back(fs.open("/append-test",forkfs::Namespace::Access::ReadWrite,app));
            fs.snapshot("append-base");fs.fork("append-base","append-fork");
            forkfs::Namespace append_base(store,"append-base",true),append_fork(store,"append-fork");
            std::vector<std::thread> writers;
            for(unsigned t=0;t<8;++t)writers.emplace_back([&,t]{try{
                for(unsigned i=0;i<16;++i){Bytes record(8,t);record[1]=i;fs.handle_write(appenders[t],0,record);}
            }catch(...){failed=true;}});
            fs.remove("/append-test",false);
            for(auto& writer:writers)writer.join();check(!failed);
            auto appended=fs.handle_read(appenders[0],0,UINT64_MAX);check(appended.size()==1+128*8 && appended[0]=='B');
            std::set<std::pair<unsigned,unsigned>> records;
            for(size_t i=1;i<appended.size();i+=8){check(appended[i]<8 && appended[i+1]<16);
                for(unsigned j=2;j<8;++j)check(appended[i+j]==appended[i]);records.emplace(appended[i],appended[i+1]);}
            check(records.size()==128);
            fs.handle_write(appenders[0],INT64_MAX,Bytes{'!'});check(fs.handle_read(appenders[0],appended.size(),5)==Bytes{'!'});
            check(append_base.read("/append-test")==Bytes{'B'} && append_fork.read("/append-test")==Bytes{'B'});
            fs.write("/append-test",Bytes{'N'});check(fs.read("/append-test")==Bytes{'N'});
            fs.handle_truncate(appenders[0],256*1024);stable=store.status();
            rejects([&]{fs.handle_write(appenders[0],0,Bytes{1});});rejects([&]{fs.handle_write(appenders[0],UINT64_MAX,{});});
            fs.handle_write(appenders[0],0,{});check(store.status()==stable);
            for(const auto& h:appenders)fs.close(h);fs.remove("/append-test",false);fs.verify();
            fs.write("/cursor",Bytes{'a','b','c','d'});auto cursor=fs.open("/cursor",forkfs::Namespace::Access::ReadWrite);
            auto duplicate=fs.dup(cursor),independent=fs.open("/cursor");
            check(fs.handle_read_next(cursor,1)==Bytes{'a'} && fs.handle_read_next(duplicate,1)==Bytes{'b'});
            check(fs.handle_read_next(independent,1)==Bytes{'a'});
            check(fs.seek(cursor,-1,forkfs::Namespace::Seek::Current)==1);
            fs.handle_write_next(duplicate,Bytes{'X'});check(fs.seek(cursor,0,forkfs::Namespace::Seek::Current)==2);
            fs.handle_write(cursor,0,Bytes{'Y'});check(fs.seek(cursor,0,forkfs::Namespace::Seek::Current)==2);
            check(fs.seek(cursor,-1,forkfs::Namespace::Seek::End)==3);
            check(fs.handle_read_next(duplicate,10)==Bytes{'d'} && fs.handle_read_next(cursor,1).empty());
            rejects([&]{fs.seek(cursor,INT64_MIN,forkfs::Namespace::Seek::Current);});
            check(fs.seek(cursor,INT64_MAX,forkfs::Namespace::Seek::Set)==INT64_MAX);
            rejects([&]{fs.seek(cursor,1,forkfs::Namespace::Seek::Current);});
            rejects([&]{fs.handle_write_next(cursor,Bytes{1});});check(fs.seek(cursor,0,forkfs::Namespace::Seek::Current)==INT64_MAX);
            fs.remove("/cursor",false);fs.seek(cursor,0,forkfs::Namespace::Seek::Set);
            fs.handle_write_next(cursor,Bytes{'Z'});fs.close(cursor);check(fs.handle_read_next(duplicate,1)==Bytes{'X'});
            fs.close(duplicate);fs.close(independent);
            auto ap=fs.open("/cursor-app",forkfs::Namespace::Access::ReadWrite,{true,false,false,0644,true});
            fs.seek(ap,99,forkfs::Namespace::Seek::Set);fs.handle_write_next(ap,Bytes{'A'});
            check(fs.seek(ap,0,forkfs::Namespace::Seek::Current)==1);auto ad=fs.dup(ap);
            fs.seek(ad,0,forkfs::Namespace::Seek::Set);fs.handle_write_next(ad,Bytes{'B'});
            check(fs.seek(ap,0,forkfs::Namespace::Seek::Current)==2);fs.close(ad);fs.close(ap);fs.remove("/cursor-app",false);
            // Concurrent readers on dup share one cursor without duplicate bytes.
            Bytes numbered(128);for(unsigned i=0;i<128;++i)numbered[i]=i;fs.write("/shared-read",numbered);
            auto reader=fs.open("/shared-read");std::vector<std::string> readers;std::set<unsigned> seen;std::mutex seen_mutex;
            for(int i=0;i<4;++i)readers.push_back(fs.dup(reader));writers.clear();
            for(const auto& h:readers)writers.emplace_back([&,h]{try{for(;;){auto b=fs.handle_read_next(h,1);if(b.empty())break;
                std::lock_guard lock(seen_mutex);if(!seen.insert(b[0]).second)failed=true;
            }}catch(...){failed=true;}});
            for(auto& t:writers)t.join();check(!failed && seen.size()==128);
            for(const auto& h:readers)fs.close(h);fs.close(reader);fs.remove("/shared-read",false);
            fs.symlink("/created-target","/dangling-a");fs.symlink("created-target","/dangling-b");
            forkfs::Namespace::OpenOptions follow_create;follow_create.create=true;
            auto nofollow=follow_create;nofollow.nofollow=true;stable=store.status();
            rejects([&]{fs.open("/dangling-a",forkfs::Namespace::Access::ReadWrite,nofollow);});check(store.status()==stable);
            std::set<std::string> created_ids;std::mutex created_mutex;writers.clear();
            for(int i=0;i<8;++i)writers.emplace_back([&,i]{try{
                auto h=fs.open(i%2?"/dangling-a":"/dangling-b",forkfs::Namespace::Access::ReadWrite,follow_create);
                auto inode=fs.handle_stat(h).id;{std::lock_guard lock(created_mutex);created_ids.insert(inode);}fs.close(h);
            }catch(...){failed=true;}});
            for(auto& t:writers)t.join();check(!failed && created_ids.size()==1);
            check(fs.readlink("/dangling-a")=="/created-target" && fs.readlink("/dangling-b")=="created-target");
            fs.remove("/created-target",false);fs.write("/dangling-a",Bytes{'W'});check(fs.read("/created-target")==Bytes{'W'});
            auto regular=fs.open("/created-target",forkfs::Namespace::Access::Read,nofollow);fs.close(regular);
            fs.remove("/dangling-a",false);fs.remove("/dangling-b",false);fs.remove("/created-target",false);fs.verify();
            auto attribute_handle=fs.open("/dst");
            fs.set_attributes("/dst",{0751,999});auto attrs=fs.handle_stat(attribute_handle);
            check(attrs.mode==0751 && attrs.modified_ns==999);
            fs.handle_set_attributes(attribute_handle,{0700,0});check(fs.stat("/dst").modified_ns==0 && fs.stat("/dst").mode==0700);
            stable=store.status();fs.handle_set_attributes(attribute_handle,{});check(store.status()==stable);
            fs.close(attribute_handle);
            // Whole-file advisory locks belong to the shared open description.
            fs.write("/locked",Bytes{'L'});fs.link("/locked","/locked-alias");
            auto la=fs.open("/locked"),lb=fs.open("/locked-alias"),ld=fs.dup(la);
            stable=store.status();check(fs.try_lock(la,forkfs::Namespace::Lock::Shared));
            check(fs.try_lock(lb,forkfs::Namespace::Lock::Shared));check(!fs.try_lock(ld,forkfs::Namespace::Lock::Exclusive));
            fs.close(lb);check(fs.try_lock(ld,forkfs::Namespace::Lock::Exclusive));fs.close(la);
            fs.rename("/locked","/locked-moved");auto lc=fs.open("/locked-moved");
            check(!fs.try_lock(lc,forkfs::Namespace::Lock::Shared));
            check(fs.try_lock(ld,forkfs::Namespace::Lock::Shared));check(fs.try_lock(lc,forkfs::Namespace::Lock::Shared));
            check(!fs.try_lock(ld,forkfs::Namespace::Lock::Exclusive));
            fs.remove("/locked-moved",false);fs.remove("/locked-alias",false);
            check(!fs.try_lock(lc,forkfs::Namespace::Lock::Exclusive));fs.close(ld);
            check(fs.try_lock(lc,forkfs::Namespace::Lock::Exclusive));
            fs.write("/locked",Bytes{'N'});auto fresh_lock=fs.open("/locked");check(fs.try_lock(fresh_lock,forkfs::Namespace::Lock::Exclusive));
            fs.close(fresh_lock);fs.remove("/locked",false);fs.close(lc);
            auto main_lock=fs.open("/dst");stable=store.status();
            check(fs.try_lock(main_lock,forkfs::Namespace::Lock::Exclusive));check(fs.try_lock(main_lock,forkfs::Namespace::Lock::Unlock));
            check(stable==store.status());fs.close(main_lock);
            auto contender_a=fs.open("/dst"),contender_b=fs.open("/dst");std::barrier gate(3);std::atomic<int> lock_winners=0;
            auto compete=[&](const std::string& h){gate.arrive_and_wait();if(fs.try_lock(h,forkfs::Namespace::Lock::Exclusive))++lock_winners;};
            std::thread lock_a(compete,contender_a),lock_b(compete,contender_b);gate.arrive_and_wait();lock_a.join();lock_b.join();
            check(lock_winners==1);fs.close(contender_a);fs.close(contender_b);
            auto range_a=fs.open("/dst"),range_b=fs.open("/dst"),range_dup=fs.dup(range_a);
            auto range=[&](const std::string& h,forkfs::Namespace::Lock mode,uint64_t start,uint64_t length){return fs.try_lock_range(h,mode,start,length);};
            auto shared=forkfs::Namespace::Lock::Shared,ex=forkfs::Namespace::Lock::Exclusive,unlock=forkfs::Namespace::Lock::Unlock;
            stable=store.status();check(range(range_a,ex,0,10));check(range(range_b,ex,10,10));
            check(!range(range_b,shared,9,2));check(!fs.try_lock(range_b,ex));
            check(range(range_dup,unlock,3,4));check(range(range_b,ex,3,4));
            check(!range(range_b,shared,0,1) && !range(range_b,shared,7,1));
            check(fs.try_lock(range_b,unlock));check(range(range_a,shared,0,10));check(range(range_b,shared,4,1));
            check(!range(range_a,ex,2,5));check(!range(range_b,ex,0,1)); // Failed upgrade kept old shared range.
            check(fs.try_lock(range_b,unlock));check(range(range_a,ex,2,5));
            check(range(range_b,shared,0,2) && !range(range_b,shared,2,1));
            check(fs.try_lock(range_b,unlock));check(fs.try_lock(range_a,unlock));
            check(range(range_a,ex,100,0));check(range(range_b,ex,0,100));check(!range(range_b,shared,1000000,1));
            check(fs.try_lock(range_b,unlock));check(range(range_dup,unlock,200,0));check(range(range_b,ex,200,0));
            check(!range(range_b,ex,199,1));check(fs.try_lock(range_a,unlock));check(fs.try_lock(range_b,unlock));
            check(range(range_a,ex,INT64_MAX,1));check(!range(range_b,shared,INT64_MAX,0));
            rejects([&]{range(range_a,shared,INT64_MAX,2);});rejects([&]{range(range_a,shared,UINT64_MAX,0);});
            check(fs.try_lock(range_a,unlock));
            for(uint64_t i=0;i<128;++i)check(range(range_a,shared,i*2,1));
            rejects([&]{range(range_a,shared,256,1);});check(!range(range_b,ex,0,1));
            check(fs.try_lock(range_a,unlock)); // Resource exhaustion cannot prevent full unlock.
            // Adjacent ranges merge instead of exhausting the interval budget.
            for(uint64_t i=0;i<160;++i)check(range(range_a,shared,i,1));
            fs.close(range_a);check(!range(range_b,ex,0,1));fs.close(range_dup);check(range(range_b,ex,0,1));fs.close(range_b);
            check(stable==store.status());
            fs.write("/leak",Bytes{'L'});stale=fs.open("/leak",forkfs::Namespace::Access::ReadWrite);fs.remove("/leak",false);
            fs.handle_write(stale,0,Bytes{'Q'});fs.verify();store.verify(); // Simulate process-lifetime handle loss.
        }
        {
            forkfs::Container store(path);forkfs::Namespace fs(store),revision(store,"base",true);
            fs.verify();store.verify();rejects([&]{fs.handle_stat(stale);});
            auto fresh=fs.open("/dst");check(fresh!=stale);rejects([&]{fs.handle_read(stale,0,1);});fs.close(fresh);
            check(revision.read("/f")==Bytes({'o','l','d'}));check(fs.list("/")==std::vector<std::string>{"dst"});
        }
        std::filesystem::remove_all(tmp);std::cout<<"inode handles, open-unlink, rename replacement, branch isolation and orphan recovery passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
}
