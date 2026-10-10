#include "journal.h"
#include "level_store.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#ifdef FORKFS_TEST_FAULTS
#include <cstdlib>
#endif
#include <limits>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif

namespace forkfs {
namespace {
using Bytes = std::vector<unsigned char>;
using Hash = std::array<unsigned char,32>;
constexpr uint64_t base = 65536, span = 16*1024*1024, header = 4096;
constexpr size_t max_payload = 256*1024;
void need(bool v, const char* msg) { if (!v) throw std::runtime_error(msg); }
[[noreturn]] void io(const char* msg) { throw std::system_error(errno, std::generic_category(), msg); }
uint64_t load(const unsigned char* p, size_t n) {
    uint64_t v=0; for(size_t i=0;i<n;++i) v |= uint64_t(p[i]) << (i*8); return v;
}
void store(unsigned char* p, uint64_t v, size_t n) {
    for(size_t i=0;i<n;++i) p[i]=static_cast<unsigned char>(v>>(i*8));
}
uint32_t crc(const unsigned char* p,size_t n) {
    uint32_t c=0xffffffff;
    for(size_t i=0;i<n;++i) { c ^= p[i]; for(int b=0;b<8;++b) c=(c>>1)^((c&1)?0x82f63b78:0); }
    return c^0xffffffff;
}
Hash digest(const Bytes& b) {
    Hash h{};
#ifdef __APPLE__
    CC_SHA256(b.data(), static_cast<CC_LONG>(b.size()), h.data());
#else
    SHA256(b.data(), b.size(), h.data());
#endif
    return h;
}
Hash tagged(const char* tag,const Bytes& b) {
    Bytes all(tag,tag+strlen(tag)); all.insert(all.end(),b.begin(),b.end()); return digest(all);
}
Hash object_hash(const Bytes& value) {
    // DATA=4, schema=0; framing prefix is excluded from canonical payload.
    const char* domain="forkfs.object.v0";
    Bytes b(domain,domain+strlen(domain)); size_t at=b.size(); b.resize(at+12);
    store(b.data()+at,4,2); store(b.data()+at+2,0,2); store(b.data()+at+4,value.size(),8);
    b.insert(b.end(),value.begin(),value.end()); return digest(b);
}
Bytes read_bytes(int fd,uint64_t at,size_t n) {
    Bytes b(n); size_t done=0;
    while(done<n) {
        auto r=pread(fd,b.data()+done,n-done,static_cast<off_t>(at+done));
        if(r<0 && errno==EINTR) continue;
        if(r<0) io("read journal");
        need(r>0,"short journal read"); done+=r;
    }
    return b;
}
void write_bytes(int fd,uint64_t at,const Bytes& b) {
    size_t done=0;
    while(done<b.size()) {
        auto r=pwrite(fd,b.data()+done,b.size()-done,static_cast<off_t>(at+done));
        if(r<0 && errno==EINTR) continue;
        if(r<0) io("write journal");
        need(r>0,"zero journal write"); done+=r;
    }
}
void sync_file(int fd) {
    if(fsync(fd)<0) io("sync journal");
#ifdef __APPLE__
    if(fcntl(fd,F_FULLFSYNC)<0) io("full sync journal");
#endif
}
Bytes record(uint16_t type,uint64_t seq,const Bytes& payload,const Hash& oid={}) {
    size_t len=(64+payload.size()+7)/8*8+16;
    Bytes b(len);
    memcpy(b.data(),"FFR0",4); store(b.data()+4,type,2);
    // schema 1 reserves this experimental named-object transaction profile.
    store(b.data()+6,1,2); store(b.data()+8,len,4); store(b.data()+12,payload.size(),4);
    store(b.data()+16,seq,8); memcpy(b.data()+24,oid.data(),32);
    store(b.data()+60,crc(b.data(),60),4);
    std::copy(payload.begin(),payload.end(),b.begin()+64);
    store(b.data()+len-16,crc(payload.data(),payload.size()),4);
    store(b.data()+len-12,len,4); memcpy(b.data()+len-8,"FFREND00",8);
    return b;
}
struct Rec { uint16_t type; uint64_t seq,offset; Hash oid; Bytes payload,encoded; };
// Incomplete final record is a recoverable tail; complete corrupt records fail closed.
bool read_record(int fd,uint64_t at,uint64_t size,Rec& out) {
    need(at<=size,"record offset past EOF");
    if(size-at<64) return false;
    auto h=read_bytes(fd,at,64);
    need(!memcmp(h.data(),"FFR0",4),"invalid record magic");
    need(load(h.data()+60,4)==crc(h.data(),60),"invalid record header checksum");
    uint64_t len=load(h.data()+8,4), n=load(h.data()+12,4);
    need(len>=80 && len<=1024*1024 && !(len%8) && n<=len-80 &&
         len==((64+n+7)/8*8+16),"invalid record length");
    need(len<=span-(at-base)%span,"record crosses segment boundary");
    if(size-at<len) return false;
    auto b=read_bytes(fd,at,len);
    need(load(b.data()+6,2)==1 && load(b.data()+56,4)==0,"unsupported record schema or flags");
    need(load(b.data()+len-12,4)==len && !memcmp(b.data()+len-8,"FFREND00",8),"invalid record trailer");
    need(load(b.data()+len-16,4)==crc(b.data()+64,n),"invalid payload checksum");
    for(size_t i=64+n;i<len-16;++i) need(b[i]==0,"nonzero record padding");
    out.type=load(b.data()+4,2); out.seq=load(b.data()+16,8); out.offset=at;
    std::copy_n(b.data()+24,32,out.oid.begin());
    out.payload.assign(b.begin()+64,b.begin()+64+n); out.encoded=std::move(b); return true;
}
Bytes segment_header(const std::array<unsigned char,16>& repo,uint64_t id) {
    Bytes b(header); memcpy(b.data(),"FFSEG000",8); memcpy(b.data()+8,repo.data(),16);
    store(b.data()+24,id,8); store(b.data()+32,1,8);
    store(b.data()+4092,crc(b.data(),4092),4); return b;
}
void fault(const char* stage) {
#ifdef FORKFS_TEST_FAULTS
    const char* requested = std::getenv("FORKFS_TEST_CRASH");
    if (requested && !strcmp(requested,stage)) _exit(86);
#else
    (void)stage;
#endif
}
void append(Bytes& a,const Bytes& b) { a.insert(a.end(),b.begin(),b.end()); }
}

Journal::~Journal()=default;
Journal::Journal(const std::string& directory):leveldb_(std::make_unique<LevelStore>(directory)),repository_{} {}
std::string Journal::repository() const {need(bool(leveldb_),"repository API requires LevelDB");return leveldb_->repository();}
uint64_t Journal::sequence() const {return leveldb_?leveldb_->sequence():seq_;}
size_t Journal::objects() const {return leveldb_?leveldb_->objects():objects_.size();}
size_t Journal::count() const {return leveldb_?leveldb_->roots():roots_.size();}
void Journal::poison() {if(leveldb_)leveldb_->poison();poisoned_=true;}
std::string Journal::metrics_json(bool detailed) const {return leveldb_?leveldb_->metrics_json(detailed):"null";}
void Journal::compact() {need(bool(leveldb_),"direct compaction requires LevelDB");leveldb_->compact();}
Journal::Journal(int fd,const std::array<unsigned char,16>& repo,const Checkpoint& checkpoint):fd_(fd),repository_(repo) {
    struct stat st{}; if(fstat(fd_,&st)<0) io("stat journal");
    const uint64_t size=st.st_size;
    need(size>=base,"truncated container");
    if(checkpoint.offset) restore_checkpoint(checkpoint,size);
    uint64_t at=end_,begin=0,prepare_at=0;
    // Starting inside a segment must still validate that segment's identity.
    if(at>base && (at-base)%span) {
        uint64_t seg=(at-base)/span;
        need(read_bytes(fd_,base+seg*span,header)==segment_header(repo,seg),"invalid replay segment");
    }
    int phase=0;
    Rec prep{};
    std::map<Hash,Location> pending_objects;
    std::map<std::string,std::optional<Hash>> changes;
    Bytes batch;
    while(at<size) {
        uint64_t in=(at-base)%span;
        if(in==0) {
            need(phase==0,"transaction spans segments in unsupported profile");
            if(size-at<header) break;
            auto h=read_bytes(fd_,at,header);
            need(h==segment_header(repo,(at-base)/span),"invalid segment header");
            at+=header; continue;
        }
        // Writer pads only between transactions; require the entire remainder zero.
        auto prefix=read_bytes(fd_,at,std::min<uint64_t>(4,size-at));
        if(std::all_of(prefix.begin(),prefix.end(),[](auto c){return c==0;})) {
            need(phase==0,"padding inside transaction");
            auto pad=read_bytes(fd_,at,std::min<uint64_t>(span-in,size-at));
            need(std::all_of(pad.begin(),pad.end(),[](auto c){return c==0;}),"invalid segment padding");
            at+=pad.size(); continue;
        }
        Rec r{}; if(!read_record(fd_,at,size,r)) break;
        if(phase==0 && r.type==5) {
            need(r.seq==seq_ && r.oid==Hash{} && r.payload.size()>=48 &&
                 load(r.payload.data(),8)==seq_ &&
                 !memcmp(r.payload.data()+8,last_hash_.data(),32),"invalid checkpoint in replay");
            // Unanchored checkpoints are physical hints, not logical commits.
            at+=r.encoded.size(); end_=at; continue;
        }
        need(seq_!=UINT64_MAX && r.seq==seq_+1,"journal sequence gap");
        const auto record_size=r.encoded.size();
        if(phase==0) { begin=at; batch.clear(); pending_objects.clear(); changes.clear(); }
        if(phase<2 && r.type==1) {
            need(r.payload.size()>=4 && r.payload.size()<=max_payload+4,"invalid DATA size");
            need(load(r.payload.data(),2)==4 && load(r.payload.data()+2,2)==0,"unsupported object type");
            Bytes value(r.payload.begin()+4,r.payload.end());
            need(object_hash(value)==r.oid,"object identity mismatch");
            need(pending_objects.size()<64 && !pending_objects.contains(r.oid),"duplicate or excess batch objects");
            pending_objects[r.oid]={at,static_cast<uint32_t>(record_size)};
            append(batch,r.encoded); phase=1;
        } else if(phase<2 && (r.type==2 || r.type==7 || r.type==8)) {
            need(r.oid==Hash{} && r.payload.size()>=36,"invalid prepare record");
            need(!memcmp(r.payload.data(),last_hash_.data(),32),"prepare predecessor mismatch");
            if(r.type==2) {
                const auto n=load(r.payload.data()+32,4);
                need(n>0 && n<=1024 && r.payload.size()==68+n && pending_objects.size()==1,
                     "invalid legacy prepare");
                Hash id{}; std::copy_n(r.payload.data()+36+n,32,id.begin());
                need(pending_objects.contains(id),"prepare object mismatch");
                changes.emplace(std::string(r.payload.begin()+36,r.payload.begin()+36+n),id);
            } else {
                need(r.payload.size()>=44 && load(r.payload.data()+32,8)==seq_,"batch sequence precondition mismatch");
                auto count=load(r.payload.data()+40,4);
                need(count>0 && count<=64,"invalid batch count");
                size_t cursor=44;
                std::string previous;
                std::map<Hash,bool> referenced;
                for(uint64_t i=0;i<count;++i) {
                    need(cursor<=r.payload.size() && r.payload.size()-cursor>=8,"truncated batch entry");
                    auto op=load(r.payload.data()+cursor,4),n=load(r.payload.data()+cursor+4,4); cursor+=8;
                    need((op==1 || op==2) && n>0 && n<=1024 && r.payload.size()-cursor>=n+32,"invalid batch entry");
                    std::string key(r.payload.begin()+cursor,r.payload.begin()+cursor+n); cursor+=n;
                    Hash id{}; std::copy_n(r.payload.data()+cursor,32,id.begin()); cursor+=32;
                    need(i==0 || previous<key,"batch keys must be unique and sorted"); previous=key;
                    if(op==1) {
                        need(pending_objects.contains(id) || (r.type==8 && objects_.contains(id)),"batch references missing object");
                        changes.emplace(key,id); if(pending_objects.contains(id))referenced[id]=true;
                    } else {
                        need(id==Hash{} && roots_.contains(key),"invalid batch deletion");
                        changes.emplace(key,std::nullopt);
                    }
                }
                need(cursor==r.payload.size() && referenced.size()==pending_objects.size(),"extra batch data");
            }
            prepare_at=at; append(batch,r.encoded); prep=std::move(r); phase=2;
        } else {
            need(phase==2 && r.type==3 && r.oid==Hash{} && r.payload.size()==136,"expected commit record");
            const auto* p=r.payload.data();
            need(load(p,8)==r.seq && !memcmp(p+8,last_hash_.data(),32),"commit predecessor mismatch");
            need(load(p+40,8)==prepare_at && load(p+48,4)==prep.encoded.size() && load(p+52,4)==0,
                 "commit prepare location mismatch");
            auto ph=digest(prep.payload), bh=digest(batch);
            need(!memcmp(p+56,ph.data(),32) && load(p+88,8)==begin && load(p+96,8)==at &&
                 !memcmp(p+104,bh.data(),32),"commit batch mismatch");
            for(const auto& [id,loc]:pending_objects) objects_[id]=loc;
            for(const auto& [key,id]:changes) { if(id) roots_[key]=*id; else roots_.erase(key); }
            ++replayed_;
            seq_=r.seq; last_hash_=tagged("forkfs.commit.v0",r.payload);
            end_=at+record_size; phase=0;
        }
        need(batch.size()<=2*1024*1024,"transaction batch too large");
        at+=record_size;
    }
    tail_=size!=end_;
}

void Journal::put(const std::string& key,const Bytes& value) {
    if(leveldb_){leveldb_->transact(leveldb_->sequence(),{{key,value}});return;}
    need(!poisoned_,"journal requires reopen after write failure");
    need(!key.empty() && key.size()<=1024,"root name must be 1..1024 bytes");
    need(value.size()<=max_payload,"object exceeds 256 KiB");
    need(seq_!=UINT64_MAX,"transaction sequence exhausted");
    const auto oid=object_hash(value);
    Bytes op(4); store(op.data(),4,2); append(op,value);
    auto object=record(1,seq_+1,op,oid);
    Bytes pp(36); memcpy(pp.data(),last_hash_.data(),32); store(pp.data()+32,key.size(),4);
    pp.insert(pp.end(),key.begin(),key.end()); pp.insert(pp.end(),oid.begin(),oid.end());
    auto prepare=record(2,seq_+1,pp);
    size_t required=object.size()+prepare.size()+216;
    uint64_t at=end_;
    const uint64_t in=(at-base)%span;
    bool new_segment=in==0 || span-in<required;
    if(new_segment && in) at+=span-in;
    need(at<=uint64_t(std::numeric_limits<off_t>::max())-span,"container offset exhausted");
    uint64_t begin=at+(new_segment?header:0), prepare_at=begin+object.size();
    Bytes batch=object; append(batch,prepare);
    auto ph=digest(pp),bh=digest(batch);
    Bytes cp(136); store(cp.data(),seq_+1,8); memcpy(cp.data()+8,last_hash_.data(),32);
    store(cp.data()+40,prepare_at,8); store(cp.data()+48,prepare.size(),4);
    memcpy(cp.data()+56,ph.data(),32); store(cp.data()+88,begin,8); store(cp.data()+96,begin+batch.size(),8);
    memcpy(cp.data()+104,bh.data(),32);
    auto commit=record(3,seq_+1,cp);
    try {
        if(tail_) {
            if(ftruncate(fd_,static_cast<off_t>(end_))<0) io("truncate uncommitted tail");
            sync_file(fd_);
        }
        if(new_segment) {
            if(at>end_) write_bytes(fd_,end_,Bytes(at-end_));
            write_bytes(fd_,at,segment_header(repository_,(at-base)/span));
        }
        write_bytes(fd_,begin,batch);
        fault("batch_written");
        sync_file(fd_);
        fault("batch_synced");
        write_bytes(fd_,begin+batch.size(),commit);
        fault("commit_written");
        sync_file(fd_);
        fault("commit_synced");
        objects_[oid]={begin,static_cast<uint32_t>(object.size())};
        roots_[key]=oid;
        ++seq_; last_hash_=tagged("forkfs.commit.v0",cp);
        end_=begin+batch.size()+commit.size(); tail_=false;
    } catch(...) { poisoned_=true; throw; }
}
void Journal::transact(uint64_t expected,const std::vector<Mutation>& mutations) {
    if(leveldb_){leveldb_->transact(expected,mutations);return;}
    need(!poisoned_,"journal requires reopen after write failure");
    need(expected==seq_,"transaction conflict: sequence changed");
    need(seq_!=UINT64_MAX,"transaction sequence exhausted");
    need(!mutations.empty() && mutations.size()<=64,"batch requires 1..64 mutations");
    std::map<std::string,std::optional<Hash>> changes;
    std::map<Hash,Bytes> values;
    uint64_t total=0;bool references=false;
    for(const auto& m:mutations) {
        need(!m.key.empty() && m.key.size()<=1024,"root name must be 1..1024 bytes");
        need(!changes.contains(m.key),"duplicate mutation key");
        need(!(m.value && m.reference),"mutation has both value and reference");
        if(m.reference) {
            need(objects_.contains(*m.reference),"reference object not committed");
            read_object(*m.reference);
            changes.emplace(m.key,*m.reference);references=true;
        } else if(m.value) {
            need(m.value->size()<=max_payload,"object exceeds 256 KiB");
            auto id=object_hash(*m.value); changes.emplace(m.key,id);
            if(!values.contains(id)) { total+=m.value->size(); values.emplace(id,*m.value); }
            need(total<=1024*1024,"batch values exceed 1 MiB");
        } else {
            need(roots_.contains(m.key),"cannot delete absent root");
            changes.emplace(m.key,std::nullopt);
        }
    }
    Bytes batch;
    std::map<Hash,Location> locations;
    for(const auto& [id,value]:values) {
        Bytes payload(4); store(payload.data(),4,2); append(payload,value);
        auto encoded=record(1,seq_+1,payload,id);
        locations[id]={batch.size(),static_cast<uint32_t>(encoded.size())}; append(batch,encoded);
    }
    const uint64_t prepare_relative=batch.size();
    Bytes pp(44); memcpy(pp.data(),last_hash_.data(),32); store(pp.data()+32,expected,8);
    store(pp.data()+40,changes.size(),4);
    for(const auto& [key,id]:changes) {
        size_t pos=pp.size(); pp.resize(pos+8+key.size()+32);
        store(pp.data()+pos,id?1:2,4); store(pp.data()+pos+4,key.size(),4);
        memcpy(pp.data()+pos+8,key.data(),key.size());
        if(id) memcpy(pp.data()+pos+8+key.size(),id->data(),32);
    }
    auto prepare=record(references?8:7,seq_+1,pp); append(batch,prepare);
    need(batch.size()<=2*1024*1024,"transaction batch too large");
    uint64_t at=end_,in=(at-base)%span;
    bool new_segment=in==0 || span-in<batch.size()+216;
    if(new_segment && in) at+=span-in;
    need(at<=uint64_t(std::numeric_limits<off_t>::max())-span,"container offset exhausted");
    const uint64_t begin=at+(new_segment?header:0);
    auto ph=digest(pp),bh=digest(batch);
    Bytes cp(136); store(cp.data(),seq_+1,8); memcpy(cp.data()+8,last_hash_.data(),32);
    store(cp.data()+40,begin+prepare_relative,8); store(cp.data()+48,prepare.size(),4);
    memcpy(cp.data()+56,ph.data(),32); store(cp.data()+88,begin,8); store(cp.data()+96,begin+batch.size(),8);
    memcpy(cp.data()+104,bh.data(),32); auto commit=record(3,seq_+1,cp);
    const auto next_hash=tagged("forkfs.commit.v0",cp);
    // Prepare allocations before durable publication. Publication itself cannot allocate.
    auto next_roots=roots_; auto next_objects=objects_;
    for(const auto& [id,loc]:locations) next_objects[id]={begin+loc.payload,loc.length};
    for(const auto& [key,id]:changes) { if(id) next_roots[key]=*id; else next_roots.erase(key); }
    try {
        if(tail_) {
            if(ftruncate(fd_,static_cast<off_t>(end_))<0) io("truncate uncommitted tail");
            sync_file(fd_);
        }
        if(new_segment) {
            if(at>end_) write_bytes(fd_,end_,Bytes(at-end_));
            write_bytes(fd_,at,segment_header(repository_,(at-base)/span));
        }
        write_bytes(fd_,begin,batch); fault("batch_written");
        sync_file(fd_); fault("batch_synced");
        write_bytes(fd_,begin+batch.size(),commit); fault("commit_written");
        sync_file(fd_); fault("commit_synced");
        roots_.swap(next_roots); objects_.swap(next_objects);
        ++seq_; last_hash_=next_hash; end_=begin+batch.size()+commit.size(); tail_=false;
    } catch(...) { poisoned_=true; throw; }
}

std::array<unsigned char,32> Journal::object_id(const Bytes& value) { return object_hash(value); }
std::array<unsigned char,32> Journal::root_object(const std::string& key) const {
    if(leveldb_)return leveldb_->root(key);
    need(!poisoned_,"journal requires reopen after write failure");
    auto it=roots_.find(key);need(it!=roots_.end(),"object root not found");return it->second;
}
Bytes Journal::get_object(const Hash& id) const {
    if(leveldb_)return leveldb_->object(id);
    need(!poisoned_,"journal requires reopen after write failure");return read_object(id);
}
bool Journal::contains(const std::string& key) const {
    if(leveldb_)return leveldb_->contains(key);
    need(!poisoned_,"journal requires reopen after write failure"); return roots_.contains(key);
}
std::vector<std::string> Journal::keys(const std::string& prefix) const {
    if(leveldb_)return leveldb_->keys(prefix);
    need(!poisoned_,"journal requires reopen after write failure");
    std::vector<std::string> result;
    for(auto it=roots_.lower_bound(prefix);it!=roots_.end() && it->first.starts_with(prefix);++it)
        result.push_back(it->first);
    return result;
}
void Journal::restore_checkpoint(const Checkpoint& c,uint64_t size) {
    need(c.offset>=base && c.offset<=size && c.length<=size-c.offset &&
         c.replay_start==c.offset+c.length,"invalid checkpoint range");
    need((c.offset-base)%span>=header,"checkpoint overlaps segment header");
    uint64_t seg=(c.offset-base)/span;
    need(read_bytes(fd_,base+seg*span,header)==segment_header(repository_,seg),"invalid checkpoint segment");
    Rec r{};
    need(read_record(fd_,c.offset,size,r) && r.type==5 && r.oid==Hash{} &&
         r.seq==c.sequence && r.encoded.size()==c.length && digest(r.payload)==c.hash,
         "invalid published checkpoint");
    const auto& p=r.payload;
    need(p.size()>=48 && load(p.data(),8)==c.sequence,"invalid checkpoint sequence");
    seq_=c.sequence; std::copy_n(p.data()+8,32,last_hash_.begin());
    need((seq_==0)==(last_hash_==Hash{}),"invalid checkpoint commit identity");
    uint64_t no=load(p.data()+40,4),nr=load(p.data()+44,4);
    need(no<=(p.size()-48)/44 && nr<=(p.size()-48-no*44)/37,"invalid checkpoint counts");
    size_t at=48;
    Hash previous{};
    for(uint64_t i=0;i<no;++i) {
        Hash id{}; std::copy_n(p.data()+at,32,id.begin());
        uint64_t off=load(p.data()+at+32,8),len=load(p.data()+at+40,4);
        need(id!=Hash{} && (i==0 || previous<id),"unordered checkpoint objects");
        need(off>=base && off<c.offset && len<=c.offset-off && len>=88 &&
             len<=max_payload+88 && !(off%8) && !(len%8) &&
             (off-base)%span>=header && len<=span-(off-base)%span,"invalid object location");
        objects_.emplace(id,Location{off,static_cast<uint32_t>(len)});
        previous=id; at+=44;
    }
    std::string previous_name;
    for(uint64_t i=0;i<nr;++i) {
        need(at<=p.size() && p.size()-at>=4,"truncated root index");
        size_t n=load(p.data()+at,4); at+=4;
        need(n>0 && n<=1024 && p.size()-at>=n+32,"invalid root index entry");
        std::string key(p.begin()+at,p.begin()+at+n); at+=n;
        Hash id{}; std::copy_n(p.data()+at,32,id.begin()); at+=32;
        need((i==0 || previous_name<key) && objects_.contains(id),"invalid root reference");
        roots_.emplace(key,id); previous_name=key;
    }
    need(at==p.size(),"unexpected checkpoint suffix");
    end_=c.replay_start;
}

Checkpoint Journal::checkpoint() {
    if(leveldb_){leveldb_->compact();return {};}
    need(!poisoned_,"journal requires reopen after write failure");
    // Bound the first checkpoint profile to a single record. Multipart follows later.
    uint64_t bytes=48+uint64_t(objects_.size())*44;
    for(const auto& [key,id]:roots_) { (void)id; bytes+=4+key.size()+32; }
    need(bytes<=1024*1024-87,"checkpoint exceeds single-record profile limit");
    Bytes payload(bytes);
    store(payload.data(),seq_,8); memcpy(payload.data()+8,last_hash_.data(),32);
    store(payload.data()+40,objects_.size(),4); store(payload.data()+44,roots_.size(),4);
    size_t pos=48;
    for(const auto& [id,loc]:objects_) {
        memcpy(payload.data()+pos,id.data(),32); store(payload.data()+pos+32,loc.payload,8);
        store(payload.data()+pos+40,loc.length,4); pos+=44;
    }
    for(const auto& [key,id]:roots_) {
        store(payload.data()+pos,key.size(),4); pos+=4;
        memcpy(payload.data()+pos,key.data(),key.size()); pos+=key.size();
        memcpy(payload.data()+pos,id.data(),32); pos+=32;
    }
    auto encoded=record(5,seq_,payload);
    uint64_t at=end_,in=(at-base)%span;
    bool new_segment=in==0 || span-in<encoded.size();
    if(new_segment && in) at+=span-in;
    need(at<=uint64_t(std::numeric_limits<off_t>::max())-span,"container offset exhausted");
    try {
        if(tail_) {
            if(ftruncate(fd_,static_cast<off_t>(end_))<0) io("truncate checkpoint tail");
            sync_file(fd_);
        }
        if(new_segment) {
            if(at>end_) write_bytes(fd_,end_,Bytes(at-end_));
            write_bytes(fd_,at,segment_header(repository_,(at-base)/span)); at+=header;
        }
        write_bytes(fd_,at,encoded); fault("checkpoint_written");
        sync_file(fd_); fault("checkpoint_synced");
        end_=at+encoded.size(); tail_=false;
        return {at,encoded.size(),seq_,end_,digest(payload)};
    } catch(...) { poisoned_=true; throw; }
}

std::vector<unsigned char> Journal::read_object(const Hash& id) const {
    auto it=objects_.find(id); need(it!=objects_.end(),"object not indexed");
    const auto& loc=it->second;
    Rec r{};
    need(read_record(fd_,loc.payload,loc.payload+loc.length,r) && r.type==1 &&
         r.encoded.size()==loc.length && r.seq>0 && r.seq<=seq_ && r.oid==id &&
         r.payload.size()>=4 && r.payload.size()<=max_payload+4 &&
         load(r.payload.data(),2)==4 && load(r.payload.data()+2,2)==0,"invalid indexed object");
    Bytes value(r.payload.begin()+4,r.payload.end());
    need(object_hash(value)==id,"indexed object hash mismatch"); return value;
}
void Journal::verify() const {
    if(leveldb_){leveldb_->verify();return;}
    need(!poisoned_,"journal requires reopen after write failure");
    for(const auto& [id,loc]:objects_) { (void)loc; read_object(id); }
}
std::vector<unsigned char> Journal::get(const std::string& key) const {
    if(leveldb_)return leveldb_->object(leveldb_->root(key));
    need(!poisoned_,"journal requires reopen after write failure");
    auto it=roots_.find(key); need(it!=roots_.end(),"object root not found");
    return read_object(it->second);
}
}
