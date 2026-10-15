#pragma once
#include "replay_format.h"
#include <leveldb/db.h>
#include <filesystem>
#include <mutex>
#include <limits>

namespace forkfs {
// Physical adapter only: callers and portable archives see canonical object bytes.
// LevelDB retains roots, small objects, chunk locations and reference-only logs.
class SegmentDB final:public leveldb::DB {
    using Hash=replay::Hash;
    std::unique_ptr<leveldb::DB> db_;
    std::string path_;
    mutable std::mutex append_mutex_;
    int active_=-1;uint64_t segment_=0,end_=0;
    static constexpr uint64_t segment_limit=64*1024*1024,header_size=48;
    static constexpr size_t chunk_size=64*1024,inline_limit=256;
    static void need(bool ok,const char* message){replay::require(ok,message);}
    static void fault(const char* stage){
#ifdef FORKFS_TEST_FAULTS
        const char* p=getenv("FORKFS_TEST_CRASH");if(p && !strcmp(p,stage))_exit(86);
#else
        (void)stage;
#endif
    }
    static bool injected(const char* stage){
#ifdef FORKFS_TEST_FAULTS
        const char* p=getenv("FORKFS_TEST_SEGMENT_ERROR");return p && !strcmp(p,stage);
#else
        (void)stage;return false;
#endif
    }
    static std::string number(uint64_t n){return replay::le(n);}
    static uint64_t num(const std::string& s){return replay::num(s);}
    static void checked(const leveldb::Status& s){replay::checked(s);}
    static void sync(int fd){if(fsync(fd)<0)throw std::system_error(errno,std::generic_category(),"sync data segment");}
    static void write_at(int fd,const std::string& s,uint64_t at){
        size_t done=0;while(done<s.size()){auto n=pwrite(fd,s.data()+done,s.size()-done,at+done);if(n<0 && errno==EINTR)continue;if(n<=0)throw std::system_error(n<0?errno:EIO,std::generic_category(),"append data segment");done+=n;}
    }
    static std::string read_at(int fd,size_t n,uint64_t at){
        need(at<=uint64_t(INT64_MAX) && n<=uint64_t(INT64_MAX)-at,"invalid data segment offset");
        std::string s(n,0);size_t done=0;while(done<n){auto rc=pread(fd,s.data()+done,n-done,at+done);if(rc<0 && errno==EINTR)continue;need(rc>0,"truncated or unreadable data segment");done+=rc;}return s;
    }
    struct Fd{int fd;~Fd(){if(fd>=0)close(fd);}};
    std::string filename(uint64_t id) const {return path_+"/DATA-"+std::to_string(id)+".seg";}
    static std::string header(uint64_t id){auto h=std::string("FFSEG001")+number(id);return h+replay::hash(h);}
    void open_segment(size_t required){
        if(active_>=0)return;
        // Indexed extents are immutable. An unindexed crash tail can remain as
        // garbage; new records start after EOF, without rescanning file contents.
        uint64_t largest=segment_;
        for(const auto& entry:std::filesystem::directory_iterator(path_)){
            auto name=entry.path().filename().string();if(!name.starts_with("DATA-") || !name.ends_with(".seg"))continue;
            auto digits=name.substr(5,name.size()-9);need(!digits.empty() && digits.find_first_not_of("0123456789")==std::string::npos,"invalid data segment name");
            auto id=std::stoull(digits);need(std::to_string(id)==digits,"noncanonical data segment name");largest=std::max(largest,uint64_t(id));
        }
        if(largest>0){
            Fd prior{open(filename(largest).c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};
            if(prior.fd<0)throw std::system_error(errno,std::generic_category(),"reopen data segment");
            struct stat st{};need(fstat(prior.fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size>=int64_t(header_size) && uint64_t(st.st_size)<=segment_limit,"invalid data segment size/type");
            need(read_at(prior.fd,header_size,0)==header(largest),"data segment header mismatch");
            if(uint64_t(st.st_size)+required<=segment_limit){segment_=largest;end_=st.st_size;active_=prior.fd;prior.fd=-1;return;}
        }
        need(largest<UINT64_MAX,"data segment IDs exhausted");segment_=largest+1;
        active_=open(filename(segment_).c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(active_<0)throw std::system_error(errno,std::generic_category(),"create data segment");
        try{write_at(active_,header(segment_),0);sync(active_);Fd dir{open(path_.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC)};if(dir.fd<0)throw std::system_error(errno,std::generic_category(),"open segment directory");sync(dir.fd);end_=header_size;}
        catch(...){close(active_);active_=-1;throw;}
    }
    std::string append(const std::string& hash,const std::string& data){
        need(data.size()>0 && data.size()<=chunk_size,"invalid chunk size");
        auto record=std::string("FFCHNK01")+hash+replay::le(data.size(),4)+data;
        if(active_>=0 && end_+record.size()>segment_limit){sync(active_);close(active_);active_=-1;}
        open_segment(record.size());auto at=end_;
        if(injected("partial_write")){write_at(active_,record.substr(0,record.size()/2),at);throw std::system_error(ENOSPC,std::generic_category(),"injected segment write failure");}
        write_at(active_,record,at);end_+=record.size();fault("segment_after_append");
        return number(segment_)+number(at)+replay::le(data.size(),4);
    }
    std::string chunk(const leveldb::ReadOptions& options,const std::string& hash,const std::string* location=nullptr) const {
        std::string value;if(location)value=*location;else checked(db_->Get(options,"C/"+hash,&value));
        replay::Input in{value};auto segment=in.integer(8),offset=in.integer(8),length=in.integer(4);in.end();
        need(segment>0 && offset>=header_size && offset<segment_limit && length>0 && length<=chunk_size && length+44<=segment_limit-offset,"invalid chunk location");
        Fd fd{open(filename(segment).c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};
        if(fd.fd<0)throw std::system_error(errno,std::generic_category(),"open data segment");
        struct stat st{};need(fstat(fd.fd,&st)==0 && S_ISREG(st.st_mode),"data segment is not a regular file");
        need(read_at(fd.fd,header_size,0)==header(segment),"data segment header mismatch");
        auto record=read_at(fd.fd,44+length,offset);need(record.substr(0,8)=="FFCHNK01" && record.substr(8,32)==hash,"chunk header mismatch");
        auto bytes=record.substr(44);need(record.substr(40,4)==replay::le(length,4) && replay::hash(bytes)==hash,"chunk checksum mismatch");return bytes;
    }
    std::string decode(const leveldb::ReadOptions& options,const std::string& key,const std::string& value) const {
        if(key=="M/format"){need(value=="FFLDB003" || value=="FFLDB004","invalid segmented repository format");return value=="FFLDB003"?"FFLDB001":"FFLDB002";}
        if(key.starts_with("O/")){
            need(key.size()==34 && !value.empty(),"invalid physical object");std::string result;
            if(value[0]=='I'){need(value.size()<=inline_limit+1,"oversized inline object");result=value.substr(1);}
            else{
                replay::Input in{value};need(in.take(8)=="FFOBJ001","invalid content descriptor");auto size=in.integer(8),count=in.integer(4);
                need(size>inline_limit && size<=256*1024 && count==(size+chunk_size-1)/chunk_size,"invalid content descriptor size");
                for(uint64_t i=0;i<count;++i){auto data=chunk(options,in.take(32));need(data.size()==std::min<uint64_t>(chunk_size,size-i*chunk_size),"invalid extent length");result+=data;}in.end();
            }
            need(replay::hash(result)==key.substr(2),"segmented object checksum mismatch");return result;
        }
        if(key.starts_with("L/")){
            replay::Input in{value};need(in.take(8)=="FFLOGR01","invalid reference log");auto seq=in.integer(8);auto previous=in.take(32);auto count=in.integer(4);need(count>0 && count<=8195,"invalid reference log operation count");
            std::vector<replay::Op> ops;
            for(uint64_t i=0;i<count;++i){auto kind=in.integer(1),ks=in.integer(4),vs=in.integer(4);need(kind<=1 && ks>0 && ks<=1026 && vs<=256*1024,"invalid reference log lengths");replay::Op op{in.take(ks),in.take(vs),kind==0};
                if(op.key.starts_with("O/")){need(!op.erase && op.value.empty(),"reference log contains object payload");checked(get(options,op.key,&op.value));}ops.push_back(std::move(op));}
            in.end();auto result="T"+number(seq)+previous+replay::encode(ops);replay::transaction(result);need(key==replay::logkey(seq),"reference log sequence mismatch");return result;
        }
        return value;
    }
    class Iterator final:public leveldb::Iterator {
        const SegmentDB& owner_;leveldb::ReadOptions options_;std::unique_ptr<leveldb::Iterator> it_;mutable std::string value_;mutable leveldb::Status error_;
        bool hidden() const {return it_->Valid() && it_->key().starts_with("C/");}
        void skip(bool forward){while(hidden()){if(forward)it_->Next();else it_->Prev();}}
    public:
        Iterator(const SegmentDB& db,const leveldb::ReadOptions& o):owner_(db),options_(o),it_(db.db_->NewIterator(o)){}
        bool Valid() const override{return error_.ok() && it_->Valid();}
        void SeekToFirst() override{it_->SeekToFirst();skip(true);}
        void SeekToLast() override{it_->SeekToLast();skip(false);}
        void Seek(const leveldb::Slice& k) override{it_->Seek(k);skip(true);}
        void Next() override{it_->Next();skip(true);}
        void Prev() override{it_->Prev();skip(false);}
        leveldb::Slice key() const override{return it_->key();}
        leveldb::Slice value() const override{try{value_=owner_.decode(options_,it_->key().ToString(),it_->value().ToString());}catch(const std::exception& e){error_=leveldb::Status::Corruption(e.what());value_.clear();}return value_;}
        leveldb::Status status() const override{return error_.ok()?it_->status():error_;}
    };
public:
    SegmentDB(std::unique_ptr<leveldb::DB> db,std::string path):db_(std::move(db)),path_(std::move(path)){}
    ~SegmentDB() override{if(active_>=0)close(active_);}
    static std::unique_ptr<leveldb::DB> wrap(std::unique_ptr<leveldb::DB> db,const std::string& path,bool initialize){
        if(!initialize){std::string format;checked(db->Get(leveldb::ReadOptions{},"M/format",&format));if(format=="FFLDB001" || format=="FFLDB002")return db;need(format=="FFLDB003" || format=="FFLDB004","unsupported repository format");}
        return std::make_unique<SegmentDB>(std::move(db),path);
    }
    leveldb::Status Get(const leveldb::ReadOptions& options,const leveldb::Slice& key,std::string* value) override{return get(options,key,value);}
    leveldb::Status get(const leveldb::ReadOptions& options,const leveldb::Slice& key,std::string* value) const {
        if(key.starts_with("C/"))return leveldb::Status::NotFound("private chunk index");
        auto s=db_->Get(options,key,value);if(!s.ok())return s;
        try{*value=decode(options,key.ToString(),*value);return leveldb::Status::OK();}catch(const std::exception& e){return leveldb::Status::Corruption(e.what());}
    }
    leveldb::Status Put(const leveldb::WriteOptions& o,const leveldb::Slice& k,const leveldb::Slice& v) override{leveldb::WriteBatch batch;batch.Put(k,v);return Write(o,&batch);}
    leveldb::Status Delete(const leveldb::WriteOptions& o,const leveldb::Slice& k) override{leveldb::WriteBatch batch;batch.Delete(k);return Write(o,&batch);}
    leveldb::Status Write(const leveldb::WriteOptions& options,leveldb::WriteBatch* input) override{
        std::lock_guard lock(append_mutex_);
        try{
            replay::Capture capture;checked(input->Iterate(&capture));leveldb::WriteBatch batch;std::map<std::string,std::string> added;bool appended=false;
            for(auto op:capture.ops){
                need(!op.key.starts_with("C/"),"private chunk index mutation");if(op.erase){need(!op.key.starts_with("O/") && !op.key.starts_with("L/"),"immutable data deletion unsupported");batch.Delete(op.key);continue;}
                if(op.key=="M/format"){need(op.value=="FFLDB001" || op.value=="FFLDB002","invalid logical format");op.value=op.value=="FFLDB001"?"FFLDB003":"FFLDB004";}
                else if(op.key.starts_with("O/")){
                    need(op.key.size()==34 && op.value.size()<=256*1024 && replay::hash(op.value)==op.key.substr(2),"invalid object for segmented store");
                    if(op.value.size()<=inline_limit)op.value="I"+op.value;
                    else{
                        std::string descriptor="FFOBJ001"+number(op.value.size())+replay::le((op.value.size()+chunk_size-1)/chunk_size,4);
                        for(size_t at=0;at<op.value.size();at+=chunk_size){auto data=op.value.substr(at,chunk_size),hash=replay::hash(data);descriptor+=hash;
                            if(added.contains(hash))continue;std::string location;leveldb::ReadOptions read;read.verify_checksums=true;auto found=db_->Get(read,"C/"+hash,&location);
                            if(found.ok()){need(chunk(read,hash,&location)==data,"chunk collision/corruption");continue;}need(found.IsNotFound(),"read chunk index failed");
                            location=append(hash,data);appended=true;added.emplace(hash,location);batch.Put("C/"+hash,location);
                        }
                        op.value=std::move(descriptor);
                    }
                }else if(op.key.starts_with("L/")){
                    auto tx=replay::transaction(op.value);need(op.key==replay::logkey(tx.sequence),"invalid log key");for(auto& item:tx.ops)if(item.key.starts_with("O/"))item.value.clear();
                    op.value="FFLOGR01"+number(tx.sequence)+tx.previous+replay::encode(tx.ops);
                }
                batch.Put(op.key,op.value);
            }
            if(appended){if(injected("sync"))throw std::system_error(EIO,std::generic_category(),"injected segment sync failure");sync(active_);fault("segment_after_sync");}
            fault("segment_before_metadata");if(injected("metadata"))throw std::system_error(EIO,std::generic_category(),"injected metadata publication failure");auto result=db_->Write(options,&batch);if(result.ok())fault("segment_after_metadata");return result;
        }catch(const std::exception& e){if(active_>=0){close(active_);active_=-1;}return leveldb::Status::IOError(e.what());}
    }
    leveldb::Iterator* NewIterator(const leveldb::ReadOptions& o) override{return new Iterator(*this,o);}
    const leveldb::Snapshot* GetSnapshot() override{return db_->GetSnapshot();}
    void ReleaseSnapshot(const leveldb::Snapshot* s) override{db_->ReleaseSnapshot(s);}
    bool GetProperty(const leveldb::Slice& p,std::string* v) override{return db_->GetProperty(p,v);}
    void GetApproximateSizes(const leveldb::Range* r,int n,uint64_t* sizes) override{db_->GetApproximateSizes(r,n,sizes);}
    void CompactRange(const leveldb::Slice* a,const leveldb::Slice* b) override{db_->CompactRange(a,b);}
};
}
