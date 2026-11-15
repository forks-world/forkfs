#include "namespace.h"
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <unistd.h>
using forkfs::Container;using forkfs::Namespace;using Bytes=std::vector<unsigned char>;
static void check(bool ok,int line=__builtin_LINE()){if(!ok)throw std::runtime_error("hole punch check failed at "+std::to_string(line));}
static std::string seq(const std::string& status){auto key=std::string("\"last_committed_seq\":");auto at=status.find(key);check(at!=status.npos);at+=key.size();auto end=status.find_first_not_of("0123456789",at);return status.substr(at,end-at);}
static Bytes all(Namespace& fs,const std::string& path){Bytes out;for(uint64_t at=0;at<fs.stat(path).size;){auto part=fs.read_at(path,at,Namespace::maximum_io_size);check(!part.empty());out.insert(out.end(),part.begin(),part.end());at+=part.size();}return out;}
static int errcode(auto f){try{f();}catch(const std::system_error& e){return e.code().value();}return 0;}
int main(){char pattern[]="/tmp/ff-hole-punch-XXXXXX";auto tmp=mkdtemp(pattern);if(!tmp)return 1;
try{for(bool legacy:{false,true}){auto path=std::string(tmp)+(legacy?"/legacy":"/db");if(legacy)Container::create_legacy(path);else Container::create(path);
 {Container store(path);Namespace fs(store);fs.initialize();constexpr uint64_t C=64*1024;
  fs.write("/inline",Bytes(64,0x33));auto inline_alloc=fs.stat("/inline").allocated_size;fs.punch_hole("/inline",7,11);
  Bytes inline_expected(64,0x33);std::fill(inline_expected.begin()+7,inline_expected.begin()+18,0);
  check(fs.read("/inline")==inline_expected && fs.stat("/inline").size==64 && fs.stat("/inline").allocated_size==inline_alloc);
  Bytes dense(5*C,0x5a);fs.write("/f",{});for(uint64_t at=0;at<dense.size();at+=Namespace::maximum_io_size)fs.write_at("/f",at,Bytes(dense.begin()+at,dense.begin()+std::min<uint64_t>(dense.size(),at+Namespace::maximum_io_size)));
  fs.link("/f","/alias");fs.symlink("/f","/sym");fs.snapshot("before");fs.fork("before","work");Namespace old(store,"before",true),work(store,"work");
  fs.punch_hole("/sym",C-3,2*C+9);std::fill(dense.begin()+C-3,dense.begin()+3*C+6,0);
  check(all(fs,"/alias")==dense && fs.stat("/f").size==5*C);check(all(old,"/f")!=dense && all(work,"/f")==all(old,"/f"));
  auto stable=seq(store.status());auto meta=fs.stat("/f");fs.punch_hole("/f",5*C,1);check(seq(store.status())==stable);check(fs.stat("/f").modified_ns==meta.modified_ns);
  fs.punch_hole("/f",0,0);check(seq(store.status())==stable);
  fs.punch_hole("/f",C, C);check(fs.stat("/f").size==5*C);
  auto retained=all(old,"/alias");check(retained.size()==5*C && retained[2*C]==0x5a);
  Namespace::Attributes flags;fs.write("/append-only",Bytes{'A'});flags.flags=4;fs.set_link_attributes("/append-only",flags);
  auto before_append=seq(store.status());check(errcode([&]{fs.punch_hole("/append-only",0,0);})==EPERM);check(seq(store.status())==before_append);flags.flags=0;fs.set_link_attributes("/append-only",flags);
  if(!legacy){
   auto handle=fs.open("/f",Namespace::Access::ReadWrite);auto dup=fs.dup(handle);fs.seek(handle,37,Namespace::Seek::Set);
   fs.handle_punch_hole(handle,0,1);check(fs.seek(dup,0,Namespace::Seek::Current)==37);check(fs.handle_read(dup,C-3,8)==Bytes(8,0));
   fs.write("/orphan",{});for(uint64_t at=0;at<5*C;at+=Namespace::maximum_io_size)fs.write_at("/orphan",at,Bytes(std::min<uint64_t>(Namespace::maximum_io_size,5*C-at),0x61));
   auto sparse=fs.open("/orphan",Namespace::Access::ReadWrite);fs.remove("/orphan",false);fs.write("/orphan",Bytes{'N'});
   fs.handle_punch_hole(sparse,C,3*C);check(fs.handle_stat(sparse).size==5*C);check(fs.handle_read(sparse,C,4)==Bytes(4,0));check(fs.read("/orphan")==Bytes{'N'});fs.close(sparse);
   flags.flags=4;fs.set_link_attributes("/f",flags);before_append=seq(store.status());check(errcode([&]{fs.handle_punch_hole(handle,0,1);})==EPERM);check(seq(store.status())==before_append);flags.flags=0;fs.set_link_attributes("/f",flags);
   auto ro=fs.open("/f",Namespace::Access::Read);check(errcode([&]{fs.handle_punch_hole(ro,0,1);})==EBADF);check(errcode([&]{fs.handle_punch_hole(ro,0,0);})==EBADF);fs.close(ro);
   fs.write("/immutable",Bytes(32,0x44));auto admitted=fs.open("/immutable",Namespace::Access::ReadWrite);flags.flags=2;fs.set_link_attributes("/immutable",flags);
   check(errcode([&]{fs.punch_hole("/immutable",2,3);})==EPERM);fs.handle_punch_hole(admitted,2,3);check(fs.handle_read(admitted,0,8)==Bytes({0x44,0x44,0,0,0,0x44,0x44,0x44}));fs.close(admitted);
   fs.write("/append-description",Bytes(16,0x55));Namespace::OpenOptions append_description;append_description.append=true;auto append_fd=fs.open("/append-description",Namespace::Access::ReadWrite,append_description);
   fs.handle_punch_hole(append_fd,4,4);check(fs.handle_read(append_fd,4,4)==Bytes(4,0));fs.close(append_fd);
   fs.close(dup);fs.close(handle);
  }
  fs.verify();old.verify();work.verify();if(!legacy)store.collect_garbage();store.verify();
  fs.write("/huge",{});fs.truncate("/huge",Namespace::maximum_file_size);fs.write_at("/huge",C,Bytes{0x41});fs.write_at("/huge",Namespace::maximum_file_size-C,Bytes{0x42});
  auto allocated=fs.stat("/huge").allocated_size;fs.punch_hole("/huge",0,Namespace::maximum_file_size);check(fs.stat("/huge").size==Namespace::maximum_file_size);check(fs.stat("/huge").allocated_size<allocated);
  check(errcode([&]{fs.seek_region("/huge",0,true);})==ENXIO);check(fs.read_at("/huge",C,1)==Bytes{0});fs.verify();
 }
 {Container store(path);Namespace fs(store),old(store,"before",true);check(fs.stat("/huge").size==Namespace::maximum_file_size);check(old.stat("/f").size==5*64*1024);fs.verify();old.verify();store.verify();}
 }
 std::filesystem::remove_all(tmp);std::cout<<"inline/chunked hole punching, edge preservation, links, handles, sparse range, revisions, reopen and GC passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}}
