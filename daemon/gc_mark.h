#pragma once

// Disk-backed, restart-discardable incremental object reachability marker.
// The database is deliberately private to one mark instance: callers must
// abandon it after any source read/validation error.
#include "revision.h"
#include "journal.h"
#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/options.h>
#include <leveldb/write_batch.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace forkfs {
class GcMark {
    using Clock=std::chrono::steady_clock;
    static uint64_t ns(Clock::time_point start){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();}
    struct HoldTimer {std::atomic<uint64_t>& target;Clock::time_point start=Clock::now();explicit HoldTimer(std::atomic<uint64_t>& value):target(value){}~HoldTimer(){target+=ns(start);}};
public:
    using Id=std::array<unsigned char,32>;
    using Bytes=std::vector<unsigned char>;
    using Reader=std::function<Bytes(const Id&)>;
    using RootReader=std::function<std::optional<Id>(const std::string&)>;
    static constexpr size_t max_protected_chunks=16384;
    struct MutexMetrics {uint64_t foreground_wait_ns=0,foreground_hold_ns=0,advance_wait_ns=0,advance_hold_ns=0;};
    struct MutexCounters {std::atomic<uint64_t> foreground_wait_ns=0,foreground_hold_ns=0,advance_wait_ns=0,advance_hold_ns=0;};
    MutexMetrics mutex_metrics() const {return {mutex_counters_->foreground_wait_ns.load(),mutex_counters_->foreground_hold_ns.load(),mutex_counters_->advance_wait_ns.load(),mutex_counters_->advance_hold_ns.load()};}

    explicit GcMark(const std::string& repository_path,std::shared_ptr<MutexCounters> counters={}):mutex_counters_(counters?std::move(counters):std::make_shared<MutexCounters>()) {
        auto pattern=(std::filesystem::path(repository_path)/".gc-mark-XXXXXX").string();
        std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
        char* made=::mkdtemp(name.data());if(!made)throw std::runtime_error("create GC mark scratch directory: "+std::string(strerror(errno)));
        scratch_=made;
        try {
            struct stat st{};if(::lstat(scratch_.c_str(),&st)<0 || !S_ISDIR(st.st_mode) || (st.st_mode&0777)!=0700)
                throw std::runtime_error("GC mark scratch directory is not private");
            // Publication writes this scratch DB while holding the main writer
            // fence. A tiny memtable forces frequent flush/compaction stalls there.
            // LevelDB may retain both mutable and immutable buffers; this cache
            // is separate from the per-step metadata charge, not a strict RSS cap.
            leveldb::Options options;options.create_if_missing=true;options.write_buffer_size=1024*1024;
            options.max_open_files=16;options.paranoid_checks=true;options.compression=leveldb::kNoCompression;
            cache_.reset(leveldb::NewLRUCache(0));options.block_cache=cache_.get();
            leveldb::DB* raw=nullptr;auto status=leveldb::DB::Open(options,scratch_+"/db",&raw);
            if(!status.ok())throw std::runtime_error("open GC mark scratch database: "+status.ToString());db_.reset(raw);
        } catch(...) { std::error_code ec;std::filesystem::remove_all(scratch_,ec);scratch_.clear();throw; }
    }
    ~GcMark(){db_.reset();cache_.reset();if(!scratch_.empty()){std::error_code ec;std::filesystem::remove_all(scratch_,ec);}}
    GcMark(const GcMark&)=delete;GcMark& operator=(const GcMark&)=delete;

    void seed_root(const std::string& key,const Id& id,RootReader roots) {
        auto waiting=Clock::now();std::unique_lock<std::mutex> guard(mu_);mutex_counters_->foreground_wait_ns+=ns(waiting);HoldTimer held(mutex_counters_->foreground_hold_ns);const std::string full_key=key;std::string root_key=key;
        if(root_key.starts_with("R/"))root_key.erase(0,2);
        const std::string alias("\0forkfs-history/object/",23);if(root_key.starts_with(alias)){static const char* hex="0123456789abcdef";if(root_key.size()!=alias.size()+64)throw std::runtime_error("invalid history object alias");std::string spelling;for(auto c:id){spelling+=hex[c>>4];spelling+=hex[c&15];}if(root_key.substr(alias.size())!=spelling)throw std::runtime_error("noncanonical history object alias");return;}
        if(roots){auto observed=roots(full_key);if(!observed||*observed!=id)throw std::runtime_error("GC root changed during seeding");}
        Task t;t.kind=ROOT;t.id=id;t.key=root_key;
        const std::string orphan_prefix("\0forkfs-history/orphan/",23);
        const std::string inode_prefix("\0forkfs/inode/",14),content_prefix("\0forkfs/content/",16);
        bool orphan=root_key.starts_with(orphan_prefix),inode_key=root_key.starts_with(inode_prefix),content_key=root_key.starts_with(content_prefix);
        if((orphan&&(root_key.ends_with("/inode")||root_key.ends_with("/content")))||(inode_key||content_key)){
            std::string paired=full_key;std::string normalized=root_key;
            if(orphan){auto suffix=paired.rfind(root_key.ends_with("/inode")?"/inode":"/content");paired.replace(suffix,root_key.ends_with("/inode")?6:8,root_key.ends_with("/inode")?"/content":"/inode");if(root_key.ends_with("/content")){auto pos=normalized.rfind("/content");normalized.replace(pos,8,"/inode");}else{auto pos=normalized.rfind("/inode");normalized.replace(pos,6,"/inode");}}
            else if(inode_key){auto pos=paired.find("inode/");paired.replace(pos,6,"content/");normalized.replace(8,6,"inode/");}
            else {auto pos=paired.find("content/");paired.replace(pos,8,"inode/");normalized.replace(8,8,"inode/");}
            auto paired_id=roots?roots(paired):std::optional<Id>{};bool content_half=root_key.ends_with("/content")||content_key;
            if(!paired_id&&(content_half||orphan))throw std::runtime_error("paired inode/content key missing");
            t.kind=orphan?ORPHAN_INODE:DIRECT_INODE;t.key=normalized;
            if(content_half){t.id=*paired_id;t.context=id;}else if(paired_id)t.context=*paired_id;
        }
        // A root key is retained as a typed task so revision and World roots
        // are interpreted only after their referenced bytes are verified.
        if(t.kind==DIRECT_INODE||t.kind==ORPHAN_INODE){Task keep;keep.kind=OPAQUE;keep.id=id;enqueue_now(keep);}
        enqueue_now(t);
    }
    // Only the publication barrier uses this path. Baseline roots and leases
    // retain independent immutable tasks even when a World advances again.
    void seed_published_root(const std::string& key,const Id& id,RootReader roots) {
        std::string normalized=key;if(normalized.starts_with("R/"))normalized.erase(0,2);
        const std::string world_prefix("\0forkfs-history/world/",22);
        if(!normalized.starts_with(world_prefix)||!roots){seed_root(key,id,std::move(roots));return;}
        if(id==Id{})throw std::runtime_error("zero GC object reference");
        // The production caller holds the store writer mutex for source roots.
        auto observed=roots(key);
        if(!observed||*observed!=id)throw std::runtime_error("GC root changed during seeding");
        Task task;task.kind=ROOT;task.id=id;task.key=normalized;
        auto encoded=encode(task),publication_key="P/"+normalized;
        auto waiting=Clock::now();std::unique_lock<std::mutex> guard(mu_);
        mutex_counters_->foreground_wait_ns+=ns(waiting);HoldTimer held(mutex_counters_->foreground_hold_ns);
        // One pending row per World key coalesces rapid immutable-root updates.
        std::string existing;auto found=db_->Get(read_options(),publication_key,&existing);
        if(!found.IsNotFound())check(found,"read pending GC publication root");
        leveldb::WriteBatch batch;batch.Put(live_key(id),"1");batch.Put(publication_key,encoded);
        check(db_->Write({},&batch),"queue GC publication root");
        if(found.IsNotFound())++pending_count_;
    }
    void seed_manifest(const Id& id){auto waiting=Clock::now();std::unique_lock<std::mutex> guard(mu_);mutex_counters_->foreground_wait_ns+=ns(waiting);HoldTimer held(mutex_counters_->foreground_hold_ns);Task t;t.kind=MANIFEST;t.id=id;enqueue_now(t);}
    void protect(const Id& id){auto waiting=Clock::now();std::unique_lock<std::mutex> guard(mu_);mutex_counters_->foreground_wait_ns+=ns(waiting);HoldTimer held(mutex_counters_->foreground_hold_ns);Task t;t.kind=OPAQUE;t.id=id;enqueue_now(t);}
    void protect_chunk(const std::string& hash){auto waiting=Clock::now();std::unique_lock<std::mutex> guard(mu_);mutex_counters_->foreground_wait_ns+=ns(waiting);HoldTimer held(mutex_counters_->foreground_hold_ns);if(hash.size()!=32)throw std::runtime_error("invalid GC chunk hash");leveldb::WriteBatch b;b.Put("H/"+hash,"1");check(db_->Write({},&b),"protect GC chunk");}
    void protect_chunks(const std::vector<std::string>& hashes){
        if(hashes.empty())return;
        if(hashes.size()>max_protected_chunks)throw std::runtime_error("too many GC chunks to protect");
        for(const auto& hash:hashes)if(hash.size()!=32)throw std::runtime_error("invalid GC chunk hash");
        auto waiting=Clock::now();std::unique_lock<std::mutex> guard(mu_);mutex_counters_->foreground_wait_ns+=ns(waiting);HoldTimer held(mutex_counters_->foreground_hold_ns);
        leveldb::WriteBatch batch;for(const auto& hash:hashes)batch.Put("H/"+hash,"1");check(db_->Write({},&batch),"protect GC chunks");
    }
    bool chunk_live(const std::string& hash) const {std::lock_guard<std::mutex> guard(mu_);std::string value;auto s=db_->Get(read_options(),"H/"+hash,&value);if(!s.IsNotFound())check(s,"read GC chunk marker");return !s.IsNotFound();}
    bool live(const Id& id) const {std::lock_guard<std::mutex> guard(mu_);std::string value;auto s=db_->Get(read_options(),live_key(id),&value);if(!s.IsNotFound())check(s,"read GC live marker");return !s.IsNotFound();}
    bool has_work() const {std::lock_guard<std::mutex> guard(mu_);auto it=iterator();it->Seek("Q/");check(it->status(),"scan GC tasks");if(it->Valid()&&it->key().ToString().starts_with("Q/"))return true;it->Seek("P/");check(it->status(),"scan GC publication roots");return it->Valid()&&it->key().ToString().starts_with("P/");}
    uint64_t processed() const {std::lock_guard<std::mutex> guard(mu_);return processed_;}
    size_t pending() const {std::lock_guard<std::mutex> guard(mu_);return pending_count_;}

    size_t advance(size_t units,const Reader& read) {
        // Only advancement accesses process_stage_ and semantic K/V/M records.
        // Foreground insertion has its own path and never observes staged state.
        std::lock_guard<std::mutex> advance_guard(advance_mu_);
        size_t done=0;
        while(done<units){
            std::string selected_key,selected_encoded;Task task;bool publication=false;
            {
                auto waiting=Clock::now();std::unique_lock<std::mutex> guard(mu_);
                mutex_counters_->advance_wait_ns+=ns(waiting);HoldTimer held(mutex_counters_->advance_hold_ns);
                auto it=iterator();it->Seek("Q/");check(it->status(),"scan GC tasks");
                if(it->Valid()&&it->key().ToString().starts_with("Q/")){selected_key=it->key().ToString();selected_encoded=it->value().ToString();}
                else {it->Seek("P/");check(it->status(),"scan GC publication roots");if(!it->Valid()||!it->key().ToString().starts_with("P/"))break;selected_key=it->key().ToString();selected_encoded=it->value().ToString();publication=true;}
                task=decode(selected_encoded);
            }
            // Keep the selected Q/P row visible while immutable reads block.
            Stage stage;process_stage_=&stage;bool more=false;
            try{more=process(task,read);}catch(...){process_stage_=nullptr;throw;}
            process_stage_=nullptr;
            if(publication&&more)throw std::runtime_error("GC publication root unexpectedly produced a continuation");
            {
                auto waiting=Clock::now();std::unique_lock<std::mutex> guard(mu_);
                mutex_counters_->advance_wait_ns+=ns(waiting);HoldTimer held(mutex_counters_->advance_hold_ns);
                auto& batch=stage.batch;bool publication_deleted=false;
                // Delete only the version processed: a newer foreground write may have replaced it.
                if(publication){std::string current;auto found=db_->Get(read_options(),selected_key,&current);if(!found.IsNotFound())check(found,"recheck pending GC publication root");if(!found.IsNotFound()&&current==selected_encoded){batch.Delete(selected_key);publication_deleted=true;}}
                else batch.Delete(selected_key);
                size_t queued=0;std::set<std::string> staged;
                // Resolve dedup after concurrent publication; free child encodings
                // as the commit batch grows to bound temporary marker metadata.
                for(auto& child:stage.children){
                    auto dk=dedup_encoded(child);std::string marker;
                    auto status=db_->Get(read_options(),dk,&marker);
                    if(!status.IsNotFound())check(status,"check GC mark dedup");
                    if(!status.IsNotFound()||!staged.insert(dk).second){std::string{}.swap(child);continue;}
                    if(next_==UINT64_MAX)throw std::runtime_error("GC task sequence overflow");
                    batch.Put(dk,"1");batch.Put("Q/"+be(next_),child);
                    std::string{}.swap(child);++next_;++queued;
                }
                if(more){
                    if(next_==UINT64_MAX)throw std::runtime_error("GC task sequence overflow");
                    batch.Put(dedup_key(task),"1");batch.Put("Q/"+be(next_),encode(task));++next_;
                }
                if(more||queued)batch.Put("C/next",le(next_));
                batch.Put("C/processed",le(processed_+1));
                check(db_->Write({},&batch),"commit GC mark task");
                pending_count_+=queued;if((!publication&&!more)||(publication&&publication_deleted))--pending_count_;++processed_;++done;
            }
        }
        return done;
    }

private:
    enum : unsigned char {ROOT=1,MANIFEST=2,OPAQUE=3,REVISION=4,RUN=5,TREE=6,INODE=7,EXTENT=8,CONTENT=9,BLOCK=10,ORPHAN_INODE=11,DIRECT_INODE=12,RESOLVE=13};
    struct Task {unsigned char kind=OPAQUE;Id id{};Id context{};uint64_t first=0,size=0,depth=0;unsigned char level=0;std::string key,previous,lower,upper;};
    struct Stage {leveldb::WriteBatch batch;std::vector<std::string> children;};
    std::string scratch_;std::unique_ptr<leveldb::Cache> cache_;std::unique_ptr<leveldb::DB> db_;mutable std::mutex mu_,advance_mu_;std::shared_ptr<MutexCounters> mutex_counters_;Stage* process_stage_=nullptr;uint64_t next_=0,processed_=0;size_t pending_count_=0;
    static leveldb::ReadOptions read_options(){leveldb::ReadOptions ro;ro.verify_checksums=true;ro.fill_cache=false;return ro;}
    static void check(const leveldb::Status& s,const char* what){if(!s.ok())throw std::runtime_error(std::string(what)+": "+s.ToString());}
    static std::string le(uint64_t n,unsigned w=8){std::string s(w,0);for(unsigned i=0;i<w;++i)s[i]=char(n>>(8*i));return s;}
    static uint64_t number(const unsigned char* p,unsigned w){uint64_t n=0;for(unsigned i=0;i<w;++i)n|=uint64_t(p[i])<<(8*i);return n;}
    static std::string idstr(const Id& id){return std::string(reinterpret_cast<const char*>(id.data()),id.size());}
    static std::string live_key(const Id& id){return "L/"+idstr(id);}
    std::unique_ptr<leveldb::Iterator> iterator() const {return std::unique_ptr<leveldb::Iterator>(db_->NewIterator(read_options()));}
    static void field(std::string& s,const std::string& v){s+=le(v.size(),4);s+=v;}
    static std::string take_field(const std::string& s,size_t& p){if(p>s.size()||s.size()-p<4)throw std::runtime_error("truncated GC task field");auto n=number(reinterpret_cast<const unsigned char*>(s.data()+p),4);p+=4;if(n>1024||s.size()<p+n)throw std::runtime_error("invalid GC task field");auto v=s.substr(p,n);p+=n;return v;}
    static std::string encode(const Task& t){std::string s="GCT001";s.push_back(char(t.kind));s+=idstr(t.id);s+=idstr(t.context);s+=le(t.first);s+=le(t.size);s+=le(t.depth);s.push_back(char(t.level));field(s,t.key);field(s,t.previous);field(s,t.lower);field(s,t.upper);return s;}
    static Task decode(const std::string& s){if(s.size()<6+1+64+24+1+16||s.substr(0,6)!="GCT001")throw std::runtime_error("invalid GC mark task");size_t p=6;Task t;t.kind=static_cast<unsigned char>(s[p++]);if(t.kind<ROOT||t.kind>RESOLVE)throw std::runtime_error("unknown GC task kind");memcpy(t.id.data(),s.data()+p,32);p+=32;memcpy(t.context.data(),s.data()+p,32);p+=32;t.first=number(reinterpret_cast<const unsigned char*>(s.data()+p),8);p+=8;t.size=number(reinterpret_cast<const unsigned char*>(s.data()+p),8);p+=8;t.depth=number(reinterpret_cast<const unsigned char*>(s.data()+p),8);p+=8;t.level=s[p++];t.key=take_field(s,p);t.previous=take_field(s,p);t.lower=take_field(s,p);t.upper=take_field(s,p);if(p!=s.size())throw std::runtime_error("invalid GC mark task suffix");return t;}
    static std::string dedup_encoded(const std::string& value){Bytes bytes(value.begin(),value.end());return "D/"+idstr(Journal::object_id(bytes));}
    std::string dedup_key(const Task& t)const{return dedup_encoded(encode(t));}
    void stage_put(const std::string& key,const std::string& value){if(!process_stage_)throw std::runtime_error("GC staged write outside advance");process_stage_->batch.Put(key,value);}
    void enqueue(const Task& t){if(t.id==Id{})throw std::runtime_error("zero GC object reference");if(!process_stage_)throw std::runtime_error("GC process enqueue outside advance");if(process_stage_->children.size()>=65)throw std::runtime_error("GC task fanout exceeds codec bound");stage_put(live_key(t.id),"1");process_stage_->children.push_back(encode(t));}
    void enqueue_now(const Task& t){if(t.id==Id{})throw std::runtime_error("zero GC object reference");auto encoded=encode(t);std::string marker;auto dk=dedup_key(t);auto status=db_->Get(read_options(),dk,&marker);if(!status.IsNotFound())check(status,"check GC mark dedup");
        leveldb::WriteBatch b;b.Put(live_key(t.id),"1");if(!status.IsNotFound()){check(db_->Write({},&b),"mark GC object live");return;}
        if(next_==UINT64_MAX)throw std::runtime_error("GC task sequence overflow");
        auto seq=next_++;std::string q="Q/"+be(seq);b.Put(dk,"1");b.Put(q,encoded);b.Put("C/next",le(next_));check(db_->Write({},&b),"enqueue GC mark task");++pending_count_;}
    static std::string be(uint64_t n){std::string s(8,0);for(unsigned i=0;i<8;++i)s[i]=char(n>>(8*(7-i)));return s;}
    void mark(const Id& id,unsigned char kind=OPAQUE,const Id& context={},uint64_t first=0,uint64_t size=0,unsigned char level=0,const std::string& key={}){if(id==Id{})throw std::runtime_error("zero GC object reference");Task t;t.kind=kind;t.id=id;t.context=context;t.first=first;t.size=size;t.level=level;t.key=key;enqueue(t);}
    bool process(Task& t,const Reader& reader){auto b=reader(t.id);if(b.size()>256*1024)throw std::runtime_error("GC object exceeds 256 KiB");if(Journal::object_id(b)!=t.id)throw std::runtime_error("GC object checksum mismatch");
        if(t.kind==ROOT){if(t.key.starts_with(std::string("\0forkfs-history/revision/",25))){RevisionRecord::decode(b,t.id);mark(t.id,REVISION);}else if(t.key.starts_with(std::string("\0forkfs-history/world/",22))){mark(t.id,MANIFEST);}else if(t.key.starts_with(std::string("\0forkfs-history/base/",21))){if(b.size()!=32)throw std::runtime_error("invalid World base revision reference");Id revision{};memcpy(revision.data(),b.data(),32);if(revision==Id{})throw std::runtime_error("zero World base revision reference");mark(revision,REVISION);}return false;}
        if(t.kind==REVISION){auto r=RevisionRecord::decode(b,t.id);mark(r.manifest,MANIFEST);for(auto& p:r.parents)mark(p,REVISION);return false;}
        if(t.kind==MANIFEST){auto root_value=lookup_manifest(t.id,std::string("\0forkfs/root",12),reader);if(!root_value)throw std::runtime_error("manifest missing namespace root");mark(*root_value,OPAQUE);if(b.size()==40&&!memcmp(b.data(),"FFTREE01",8)){Id root{};memcpy(root.data(),b.data()+8,32);if(root==Id{})throw std::runtime_error("empty metadata tree root");Task tree;tree.kind=TREE;tree.id=root;tree.context=t.id;enqueue(tree);return false;}
            if(b.size()>=12&&!memcmp(b.data(),"FFMAN001",8)){stage_put("V/"+idstr(t.id),"r");stage_put("M/"+idstr(t.id),le(1,4));Task run;run.kind=RUN;run.id=t.id;run.context=t.id;run.size=12;run.key="full";enqueue(run);return false;}
            if(b.size()<12||memcmp(b.data(),"FFMAN002",8))throw std::runtime_error("unsupported namespace manifest");auto n=number(b.data()+8,4);if(!n||n>32||b.size()!=12+32*n)throw std::runtime_error("invalid namespace run count");std::set<Id> unique;stage_put("V/"+idstr(t.id),"r");stage_put("M/"+idstr(t.id),le(n,4));for(size_t i=0;i<n;++i){Id run{};memcpy(run.data(),b.data()+12+32*i,32);if(run==Id{}||!unique.insert(run).second)throw std::runtime_error("duplicate or zero namespace run reference");Task task;task.kind=RUN;task.id=run;task.context=t.id;task.size=12;task.depth=i;task.key="delta";enqueue(task);}return false;}
        if(t.kind==RUN)return scan_run(b,t,reader);
        if(t.kind==TREE){scan_tree(b,t,reader);return false;}
        if(t.kind==EXTENT)return scan_extent(b,t);
        if(t.kind==INODE){scan_inode(b,t,reader);return false;}
        if(t.kind==ORPHAN_INODE){scan_inode(b,t,reader);return false;}
        if(t.kind==DIRECT_INODE){scan_inode(b,t,reader);return false;}
        if(t.kind==RESOLVE)return scan_resolve(t);
        if(t.kind==CONTENT){if(b.size()!=48||memcmp(b.data(),"FFFILE01",8))throw std::runtime_error("invalid indexed content descriptor");auto size=number(b.data()+8,8);if(size>INT64_MAX||size!=t.size)throw std::runtime_error("inode/content size mismatch");Id root{};memcpy(root.data(),b.data()+16,32);if(size==0&&root!=Id{})throw std::runtime_error("nonempty zero-size extent tree");if(root!=Id{})mark(root,EXTENT,{},0,size,5);return false;}
        if(t.kind==BLOCK){if(b.size()!=65536||std::all_of(b.begin(),b.end(),[](unsigned char c){return !c;}))throw std::runtime_error("invalid extent data block");if(t.first==t.size/65536&&t.size%65536&&!std::all_of(b.begin()+t.size%65536,b.end(),[](unsigned char c){return !c;}))throw std::runtime_error("nonzero extent tail");return false;}
        // Opaque roots and payloads are retained as bytes without interpreting magic.
        return false;
    }
    bool scan_run(const Bytes& b,Task& t,const Reader&){bool full=b.size()>=8&&!memcmp(b.data(),"FFMAN001",8);if(b.size()<12||(!full&&memcmp(b.data(),"FFRUN001",8)))throw std::runtime_error("unsupported namespace run");auto count=number(b.data()+8,4);if(!count||t.first>=count)throw std::runtime_error("empty run or cursor outside entry range");size_t at=t.size;if(at<12||at>b.size()||b.size()-at<(full?4:8))throw std::runtime_error("truncated namespace run entry");auto op=full?1:number(b.data()+at,4);if(!full)at+=4;auto n=number(b.data()+at,4);at+=4;if((op!=1&&op!=2)||n<8||n>1024||b.size()-at<n+32)throw std::runtime_error("invalid namespace run entry");std::string key(b.begin()+at,b.begin()+at+n);at+=n;Id value{};memcpy(value.data(),b.data()+at,32);at+=32;if(!key.starts_with(std::string("\0forkfs/",8))||(!t.previous.empty()&&t.previous>=key)||(op==1&&value==Id{})||(op==2&&value!=Id{}))throw std::runtime_error("invalid namespace run ordering/reference");t.previous=key;
        std::string winner="K/"+idstr(t.context)+key,old;auto found=db_->Get(read_options(),winner,&old);if(!found.IsNotFound())check(found,"read effective namespace entry");bool replace=found.IsNotFound();if(!replace){if(old.size()!=37)throw std::runtime_error("corrupt effective namespace entry");replace=number(reinterpret_cast<const unsigned char*>(old.data()),4)>t.depth;}if(replace){std::string value_record=le(t.depth,4);value_record.push_back(char(op));value_record+=idstr(value);stage_put(winner,value_record);}
        if(op==1)mark(value,OPAQUE);
        ++t.first;t.size=at;if(t.first<count)return true;if(at!=b.size())throw std::runtime_error("namespace run suffix");std::string remaining;auto rs=db_->Get(read_options(),"M/"+idstr(t.context),&remaining);check(rs,"read manifest run barrier");if(remaining.size()!=4)throw std::runtime_error("corrupt manifest run barrier");auto left=number(reinterpret_cast<const unsigned char*>(remaining.data()),4);if(!left)throw std::runtime_error("manifest run barrier underflow");--left;stage_put("M/"+idstr(t.context),le(left,4));if(!left){Task resolve;resolve.kind=RESOLVE;resolve.id=t.context;resolve.context=t.context;enqueue(resolve);}return false;}
    bool scan_resolve(Task& t){std::string prefix="K/"+idstr(t.context),seek=prefix+t.previous;auto it=iterator();it->Seek(seek);check(it->status(),"scan effective namespace entries");if(!t.previous.empty()&&it->Valid()&&it->key().ToString()==seek)it->Next();check(it->status(),"scan effective namespace entries");if(!it->Valid()||!it->key().ToString().starts_with(prefix))return false;auto dbkey=it->key().ToString(),record=it->value().ToString();if(record.size()!=37||dbkey.size()<=prefix.size())throw std::runtime_error("corrupt effective namespace entry");std::string key=dbkey.substr(prefix.size());auto op=static_cast<unsigned char>(record[4]);Id value{};memcpy(value.data(),record.data()+5,32);if((op!=1&&op!=2)||(op==1&&value==Id{})||(op==2&&value!=Id{}))throw std::runtime_error("invalid effective namespace entry");if(op==1){mark(value,OPAQUE);if(key.starts_with(std::string("\0forkfs/inode/",14)))mark(value,INODE,t.context,0,0,0,key);}t.previous=key;return true;}
    static Id priority(const std::string& key){std::string input="forkfs.metadata.priority.v1"+key;return Journal::object_id(Bytes(input.begin(),input.end()));}
    static bool higher(const std::string& a,const std::string& b){auto pa=priority(a),pb=priority(b);return pa==pb?a>b:pa>pb;}
    void scan_tree(const Bytes& b,const Task& t,const Reader& reader){if(t.depth>=256)throw std::runtime_error("metadata tree depth limit");if(b.size()<108||memcmp(b.data(),"FFTRNO01",8))throw std::runtime_error("invalid metadata tree node");auto n=number(b.data()+8,4);if(n<8||n>1024||b.size()!=108+n)throw std::runtime_error("invalid metadata node length");std::string key(b.begin()+108,b.end());if(!key.starts_with(std::string("\0forkfs/",8))||(!t.lower.empty()&&key<=t.lower)||(!t.upper.empty()&&key>=t.upper)||(t.key.size()&&!higher(t.key,key)))throw std::runtime_error("metadata tree BST/heap violation");Id value{},left{},right{};memcpy(value.data(),b.data()+12,32);memcpy(left.data(),b.data()+44,32);memcpy(right.data(),b.data()+76,32);if(value==Id{})throw std::runtime_error("zero metadata value");if(left!=Id{}){Task child=t;child.id=left;child.key=key;child.lower=t.lower;child.upper=key;++child.depth;enqueue(child);}if(right!=Id{}){Task child=t;child.id=right;child.key=key;child.lower=key;child.upper=t.upper;++child.depth;enqueue(child);}if(key.starts_with(std::string("\0forkfs/inode/",14))){auto effective=lookup_manifest(t.context,key,reader);if(effective&&*effective==value)mark(value,INODE,t.context,0,0,0,key);else mark(value,OPAQUE);}else mark(value,OPAQUE);}
    void scan_inode(const Bytes& b,const Task& t,const Reader& reader){if(b.size()<48)throw std::runtime_error("invalid inode record");bool orphan=t.kind==ORPHAN_INODE,direct=t.kind==DIRECT_INODE;bool v5=!memcmp(b.data(),"FFINOD05",8);bool xattr=!memcmp(b.data(),"FFINOD04",8)||v5;bool ext=!memcmp(b.data(),"FFINOD03",8)||xattr;bool v2=!memcmp(b.data(),"FFINOD02",8);if(!ext&&!v2&&memcmp(b.data(),"FFINOD01",8))throw std::runtime_error("invalid inode record");auto type=number(b.data()+8,4),mode=number(b.data()+12,4),links=number(b.data()+16,8),size=number(b.data()+24,8);if((type<1||type>3)||mode>0777||(!orphan&&!links)||(orphan&&(type!=1||links!=0))||((type==2)&&(links<2||size))||(v2&&type!=2))throw std::runtime_error("invalid inode attributes");if(!ext&&b.size()!=48)throw std::runtime_error("invalid inode record length");if(ext){if((v5?(b.size()<84||b.size()>128*1024):(memcmp(b.data(),"FFINOD03",8)?(b.size()<84||b.size()>128*1024):b.size()!=80))||number(b.data()+48,4)>(v5?3:1)||number(b.data()+60,4))throw std::runtime_error("invalid inode flags/length");if(number(b.data()+64,8)>INT64_MAX||number(b.data()+72,8)>INT64_MAX)throw std::runtime_error("invalid inode timestamp");}
        if(xattr){auto count=number(b.data()+80,4);if(count>128)throw std::runtime_error("too many inode xattrs");size_t at=84;std::string previous;for(uint64_t i=0;i<count;++i){if(b.size()-at<8)throw std::runtime_error("truncated inode xattr");auto names=number(b.data()+at,4),values=number(b.data()+at+4,4);at+=8;if(!names||names>255||values>65536||names+values>b.size()-at)throw std::runtime_error("invalid inode xattr length");std::string name(b.begin()+at,b.begin()+at+names);at+=names;if(!std::all_of(name.begin(),name.end(),[](unsigned char c){return c>=33&&c<=126;})||(!previous.empty()&&previous>=name))throw std::runtime_error("invalid inode xattr name/order");previous=name;at+=values;}if(at!=b.size())throw std::runtime_error("inode xattr suffix");}
        if(ext&&(number(b.data()+48,4)&1)&&type!=2)throw std::runtime_error("indexed directory inode type mismatch");
        if(type==1||type==3){std::optional<Id> content;if(orphan||direct){if(t.context==Id{})throw std::runtime_error("inode content missing");content=t.context;}else{std::string content_key=std::string("\0forkfs/content/",16)+t.key.substr(14);content=lookup_effective(t.context,content_key,reader);}if(!content)throw std::runtime_error("inode content missing");mark(*content,OPAQUE);if(type==1&&v5){if(size>INT64_MAX||!(number(b.data()+48,4)&2))throw std::runtime_error("invalid extent inode flags/size");mark(*content,CONTENT,{},0,size);}else{auto bytes=reader(*content);if(bytes.size()>256*1024||Journal::object_id(bytes)!=*content||bytes.size()!=size)throw std::runtime_error("inode content size/checksum mismatch");}}
        else if(v5)throw std::runtime_error("invalid extent inode type");
    }
    bool scan_extent(const Bytes& b,Task& t){if(b.size()<11||memcmp(b.data(),"FFEXT001",8)||b[8]!=t.level)throw std::runtime_error("invalid extent node");auto n=number(b.data()+9,2);if(!n||n>256||b.size()!=11+33*n||t.depth>=n)throw std::runtime_error("invalid extent node length/cursor");auto at=11+33*t.depth;auto slot=b[at];Id child{};memcpy(child.data(),b.data()+at+1,32);if(child==Id{}||(t.depth&&(!t.previous.empty()&&slot<=static_cast<unsigned char>(t.previous[0]))))throw std::runtime_error("invalid extent child ordering");t.previous=std::string(1,char(slot));auto start=t.first+(uint64_t(slot)<<(8*t.level));if(start>=(t.size+65535)/65536)throw std::runtime_error("extent beyond EOF");if(!t.level)mark(child,BLOCK,{},start,t.size);else mark(child,EXTENT,t.context,start,t.size,t.level-1);++t.depth;return t.depth<n;}
    std::optional<Id> lookup_effective(const Id& manifest,const std::string& key,const Reader& reader){std::string kind;auto s=db_->Get(read_options(),"V/"+idstr(manifest),&kind);if(!s.IsNotFound())check(s,"read manifest encoding");if(kind=="r"){std::string record;auto k=db_->Get(read_options(),"K/"+idstr(manifest)+key,&record);if(k.IsNotFound())return std::nullopt;check(k,"read effective namespace value");if(record.size()!=37)throw std::runtime_error("corrupt effective namespace entry");auto op=static_cast<unsigned char>(record[4]);if(op==2)return std::nullopt;if(op!=1)throw std::runtime_error("invalid effective namespace operation");Id value{};memcpy(value.data(),record.data()+5,32);if(value==Id{})throw std::runtime_error("zero effective namespace reference");return value;}return lookup_manifest(manifest,key,reader);}
    std::optional<Id> lookup_manifest(const Id& manifest,const std::string& wanted,const Reader& reader){auto m=reader(manifest);if(m.size()>256*1024||Journal::object_id(m)!=manifest)throw std::runtime_error("manifest checksum/size mismatch");
        if(m.size()==40&&!memcmp(m.data(),"FFTREE01",8)){Id node{};memcpy(node.data(),m.data()+8,32);std::string lower,upper,parent;unsigned depth=0;while(node!=Id{}){if(depth++>=256)throw std::runtime_error("metadata tree depth limit");auto b=reader(node);if(Journal::object_id(b)!=node||b.size()<108||memcmp(b.data(),"FFTRNO01",8))throw std::runtime_error("invalid metadata tree node");auto n=number(b.data()+8,4);if(n<8||n>1024||b.size()!=108+n)throw std::runtime_error("invalid metadata node length");std::string key(b.begin()+108,b.end());if((!lower.empty()&&key<=lower)||(!upper.empty()&&key>=upper)||(!parent.empty()&&!higher(parent,key)))throw std::runtime_error("metadata tree BST/heap violation");parent=key;Id value{},left{},right{};memcpy(value.data(),b.data()+12,32);memcpy(left.data(),b.data()+44,32);memcpy(right.data(),b.data()+76,32);if(value==Id{})throw std::runtime_error("zero metadata value");if(wanted==key)return value;if(wanted<key){upper=key;node=left;}else{lower=key;node=right;}}return std::nullopt;}
        std::vector<Id> runs;if(m.size()>=8&&!memcmp(m.data(),"FFMAN001",8))runs.push_back(manifest);else{if(m.size()<12||memcmp(m.data(),"FFMAN002",8))throw std::runtime_error("unsupported namespace manifest");auto count=number(m.data()+8,4);if(!count||count>32||m.size()!=12+32*count)throw std::runtime_error("invalid namespace run count");for(size_t i=0;i<count;++i){Id id{};memcpy(id.data(),m.data()+12+32*i,32);if(id==Id{})throw std::runtime_error("zero namespace run reference");runs.push_back(id);}}
        for(auto& id:runs){auto b=reader(id);bool full=b.size()>=8&&!memcmp(b.data(),"FFMAN001",8);if(b.size()>256*1024||Journal::object_id(b)!=id||b.size()<12||(!full&&memcmp(b.data(),"FFRUN001",8)))throw std::runtime_error("invalid namespace run");auto count=number(b.data()+8,4);size_t at=12;std::string previous;for(uint64_t i=0;i<count;++i){if(at>b.size()||b.size()-at<(full?4:8))throw std::runtime_error("truncated namespace run");auto op=full?1:number(b.data()+at,4);if(!full)at+=4;auto len=number(b.data()+at,4);at+=4;if((op!=1&&op!=2)||len<8||len>1024||b.size()-at<len+32)throw std::runtime_error("invalid namespace run entry");std::string key(b.begin()+at,b.begin()+at+len);at+=len;Id value{};memcpy(value.data(),b.data()+at,32);at+=32;if((!previous.empty()&&previous>=key)||(op==1&&value==Id{})||(op==2&&value!=Id{}))throw std::runtime_error("invalid namespace run order/reference");previous=key;if(key==wanted)return op==1?std::optional<Id>(value):std::nullopt;}if(at!=b.size())throw std::runtime_error("namespace run suffix");}return std::nullopt;}
};
}
