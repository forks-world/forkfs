#pragma once
#include "journal.h"
#include <leveldb/write_batch.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace forkfs::replay {
using Hash=std::array<unsigned char,32>;
constexpr size_t max_frame=24*1024*1024;
inline void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
inline void checked(const leveldb::Status& s){if(!s.ok())throw std::runtime_error(s.ToString());}
inline std::string raw(const Hash& h){return {reinterpret_cast<const char*>(h.data()),h.size()};}
inline std::string hash(const std::string& s){return raw(Journal::object_id({s.begin(),s.end()}));}
inline std::string le(uint64_t n,unsigned width=8){std::string s;for(unsigned i=0;i<width;++i)s+=char(n>>(8*i));return s;}
inline uint64_t num(const std::string& s){require(s.size()==8,"invalid replay integer");uint64_t n=0;for(unsigned i=0;i<8;++i)n|=uint64_t(static_cast<unsigned char>(s[i]))<<(8*i);return n;}
inline std::string logkey(uint64_t n){std::string s="L/";for(int i=7;i>=0;--i)s+=char(n>>(8*i));return s;}
struct Input {
    const std::string& data;size_t pos=0;
    std::string take(size_t n){require(n<=data.size()-pos,"truncated replay record");auto s=data.substr(pos,n);pos+=n;return s;}
    uint64_t integer(unsigned n){auto s=take(n);s.resize(8,0);return num(s);}
    void end(){require(pos==data.size(),"trailing replay record bytes");}
};
struct Op {std::string key,value;bool erase=false;};
inline std::string encode(const std::vector<Op>& ops){
    std::string out=le(ops.size(),4);
    for(const auto& op:ops){out+=char(op.erase?0:1);out+=le(op.key.size(),4);out+=le(op.value.size(),4);out+=op.key;out+=op.value;}
    require(out.size()<=max_frame-41,"replay transaction too large");return out;
}
inline std::vector<Op> decode(const std::string& data){
    Input in{data};auto count=in.integer(4);require(count>0 && count<=8195,"invalid replay operation count");
    std::vector<Op> ops;std::set<std::string> keys;
    for(uint64_t i=0;i<count;++i){auto kind=in.integer(1),ks=in.integer(4),vs=in.integer(4);
        require(kind<=1 && ks>0 && ks<=1026 && vs<=256*1024 && (kind || !vs),"invalid replay operation lengths");
        Op op{in.take(ks),in.take(vs),kind==0};require(keys.insert(op.key).second,"duplicate replay key");
        if(op.key.starts_with("O/"))require(!op.erase && op.key.size()==34 && hash(op.value)==op.key.substr(2),"invalid replay object hash");
        else if(op.key.starts_with("R/"))require(op.key.size()>2 && (op.erase || op.value.size()==32),"invalid replay root");
        else require(!op.erase && (op.key=="M/sequence" || op.key=="M/objects" || op.key=="M/roots") && op.value.size()==8,"invalid replay metadata operation");
        ops.push_back(std::move(op));
    }
    in.end();return ops;
}
struct Capture:leveldb::WriteBatch::Handler {
    std::vector<Op> ops;
    void Put(const leveldb::Slice& k,const leveldb::Slice& v) override {ops.push_back({k.ToString(),v.ToString(),false});}
    void Delete(const leveldb::Slice& k) override {ops.push_back({k.ToString(),{},true});}
};
struct Transaction {uint64_t sequence;std::string previous;std::vector<Op> ops;};
inline Transaction transaction(const std::string& payload){
    Input in{payload};require(in.take(1)=="T","expected replay transaction");
    auto seq=in.integer(8);auto previous=in.take(32);auto ops=decode(in.take(payload.size()-in.pos));
    unsigned metadata=0;
    for(const auto& op:ops)if(op.key.starts_with("M/")){++metadata;if(op.key=="M/sequence")require(num(op.value)==seq,"replay sequence mismatch");}
    require(metadata==3,"missing replay counters");return {seq,previous,std::move(ops)};
}
struct File {
    int fd=-1;
    explicit File(const std::string& path,bool output=false){
        fd=open(path.c_str(),output?(O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW):(O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK),0600);
        if(fd<0)throw std::system_error(errno,std::generic_category(),"open replay file");
        struct stat st{};if(fstat(fd,&st)<0 || !S_ISREG(st.st_mode)){close(fd);fd=-1;throw std::runtime_error("replay input/output must be a regular file");}
    }
    ~File(){if(fd>=0)close(fd);}File(const File&)=delete;
    void write(const std::string& s){size_t done=0;while(done<s.size()){auto n=::write(fd,s.data()+done,s.size()-done);if(n<0 && errno==EINTR)continue;if(n<=0)throw std::system_error(n<0?errno:EIO,std::generic_category(),"write replay archive");done+=n;}}
    std::string read(size_t size){std::string s(size,0);size_t done=0;while(done<size){auto n=::read(fd,s.data()+done,size-done);if(n<0 && errno==EINTR)continue;require(n>0,"truncated or unreadable replay archive");done+=n;}return s;}
    void rewind(){if(lseek(fd,0,SEEK_SET)<0)throw std::system_error(errno,std::generic_category(),"rewind replay archive");}
    void eof(){char c;ssize_t n;do{n=::read(fd,&c,1);}while(n<0 && errno==EINTR);require(n==0,"trailing replay archive bytes");}
    void sync(){if(fsync(fd)<0)throw std::system_error(errno,std::generic_category(),"sync replay archive");}
};
struct Header {
    char kind;std::string repository;uint64_t from,to;std::string previous,head;
    std::string encode() const{return "FFRPLY01"+std::string(1,kind)+repository+le(from)+le(to)+previous+head;}
    static Header read(File& f){auto s=f.read(105);Input in{s};require(in.take(8)=="FFRPLY01","unsupported replay archive");
        Header h;h.kind=in.take(1)[0];h.repository=in.take(16);h.from=in.integer(8);h.to=in.integer(8);h.previous=in.take(32);h.head=in.take(32);
        require((h.kind=='B' && h.from==h.to && h.previous==h.head) || (h.kind=='I' && h.from<=h.to),"invalid replay archive range");return h;}
};
struct Writer {
    File file;std::string digest;
    Writer(const std::string& path,const Header& h):file(path,true),digest(hash(h.encode())){file.write(h.encode());}
    void frame(const std::string& payload){require(!payload.empty() && payload.size()<=max_frame,"invalid replay frame size");auto checksum=hash(payload);file.write(le(payload.size(),4)+payload+checksum);digest=hash(digest+checksum);}
    void finish(){auto footer="E"+digest;frame(footer);file.sync();}
};
struct Reader {
    File& file;Header header;std::string digest;bool ended=false;
    explicit Reader(File& f):file(f),header(Header::read(f)),digest(hash(header.encode())){}
    std::string next(){require(!ended,"read beyond replay footer");auto size=file.read(4);size.resize(8,0);auto n=num(size);require(n>0 && n<=max_frame,"invalid replay frame length");
        auto data=file.read(n),checksum=file.read(32);require(hash(data)==checksum,"replay frame checksum mismatch");
        if(data[0]=='E'){require(data=="E"+digest,"replay archive footer mismatch");file.eof();ended=true;return {};}
        digest=hash(digest+checksum);return data;
    }
};
inline Op base_row(const std::string& payload){
    Input in{payload};require(in.take(1)=="B","expected backup row");auto ks=in.integer(4),vs=in.integer(4);
    require(ks>0 && ks<=1026 && vs<=256*1024,"invalid backup row length");Op op{in.take(ks),in.take(vs),false};in.end();
    if(op.key.starts_with("O/") || op.key.starts_with("R/"))decode(encode({op}));
    else if(op.key=="M/format")require(op.value=="FFLDB002","unsupported backup format");
    else if(op.key=="M/repository")require(op.value.size()==16,"invalid backup identity");
    else require((op.key=="M/sequence" || op.key=="M/objects" || op.key=="M/roots") && op.value.size()==8,"unknown backup key");
    return op;
}
// Complete framing/checksum preflight before opening a destination for replay.
inline Header validate(File& file,std::optional<std::pair<uint64_t,std::string>> until=std::nullopt){
    Reader r(file);uint64_t seq=r.header.from;std::string head=r.header.previous,last;
    if(until){require(until->first>=r.header.from && until->first<=r.header.to,"recovery point outside archive range");
        if(until->first==seq)require(until->second==head,"recovery point chain hash mismatch");}
    while(!r.ended){auto payload=r.next();if(r.ended)break;
        if(r.header.kind=='B'){auto op=base_row(payload);require(last.empty() || last<op.key,"unordered/duplicate backup row");last=op.key;}
        else {auto tx=transaction(payload);require(seq!=UINT64_MAX && tx.sequence==seq+1 && tx.previous==head,"missing, reordered or divergent replay transaction");seq=tx.sequence;head=hash(payload);
            if(until && seq==until->first)require(head==until->second,"recovery point chain hash mismatch");}
    }
    if(r.header.kind=='I')require(seq==r.header.to && head==r.header.head,"incomplete replay range");
    auto h=r.header;file.rewind();return h;
}
}
