#pragma once
// Rebuildable, private scratch facts, never a durable recovery authority.
#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/write_batch.h>
#include <filesystem>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <memory>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace forkfs {
class GcPhysicalIndex {
    std::string path_;
    std::unique_ptr<leveldb::Cache> cache_;
    std::unique_ptr<leveldb::DB> db_;
    std::filesystem::directory_iterator cleanup_,end_;
    bool closing_=false,db_removed_=false;
    static void check(const leveldb::Status& s){if(!s.ok())throw std::runtime_error("physical GC scratch index: "+s.ToString());}
    static std::string integer(uint64_t n){std::string b(8,0);for(unsigned i=0;i<8;++i)b[i]=char(n>>(8*i));return b;}
    static uint64_t number(const std::string& b){if(b.size()!=8)throw std::runtime_error("invalid physical GC scratch counter");uint64_t n=0;for(unsigned i=0;i<8;++i)n|=uint64_t(static_cast<unsigned char>(b[i]))<<(8*i);return n;}
    static leveldb::ReadOptions options(){leveldb::ReadOptions ro;ro.verify_checksums=true;ro.fill_cache=false;return ro;}
public:
    explicit GcPhysicalIndex(const std::string& repository){
        auto pattern=repository+"/.gc-physical-XXXXXX";
        auto made=mkdtemp(pattern.data());if(!made)throw std::system_error(errno,std::generic_category(),"create physical GC scratch");path_=made;
        try{
            struct stat st{};if(lstat(path_.c_str(),&st)<0 || !S_ISDIR(st.st_mode) || (st.st_mode&0777)!=0700)throw std::runtime_error("physical GC scratch is not private");
            leveldb::Options o;o.create_if_missing=true;o.error_if_exists=true;o.paranoid_checks=true;
            o.write_buffer_size=64*1024;o.max_open_files=16;o.compression=leveldb::kNoCompression;
            cache_.reset(leveldb::NewLRUCache(0));o.block_cache=cache_.get();leveldb::DB* raw=nullptr;
            check(leveldb::DB::Open(o,path_+"/db",&raw));db_.reset(raw);
        }catch(...){db_.reset();cache_.reset();std::error_code ec;std::filesystem::remove_all(path_,ec);throw;}
    }
    ~GcPhysicalIndex(){db_.reset();cache_.reset();std::error_code ec;std::filesystem::remove_all(path_,ec);}
    // Successful-round cleanup is budgeted too; cancellation/error destruction
    // still discards the private scratch synchronously.
    bool cleanup_one(){
        if(!closing_){db_.reset();cache_.reset();closing_=true;cleanup_=std::filesystem::directory_iterator(path_+"/db");return false;}
        if(cleanup_!=end_){auto entry=*cleanup_;++cleanup_;std::filesystem::remove(entry.path());return false;}
        if(!db_removed_){std::filesystem::remove(path_+"/db");db_removed_=true;return false;}
        std::filesystem::remove(path_);path_.clear();return true;
    }
    GcPhysicalIndex(const GcPhysicalIndex&)=delete;
    GcPhysicalIndex& operator=(const GcPhysicalIndex&)=delete;
    uint64_t bytes(uint64_t sid) const {
        std::string value;auto s=db_->Get(options(),"N/"+integer(sid),&value);if(s.IsNotFound())return 0;check(s);return number(value);
    }
    void reset_bytes(uint64_t sid){leveldb::WriteBatch b;b.Put("N/"+integer(sid),integer(0));check(db_->Write({},&b));}
    void add(uint64_t sid,const std::string& key,const std::string& location,uint64_t length){
        if(key.size()!=34 || !key.starts_with("C/") || location.size()!=20 || length>65536)throw std::runtime_error("invalid physical GC scratch row");
        auto count=bytes(sid);if(count>UINT64_MAX-length-44)throw std::runtime_error("physical GC scratch byte overflow");
        leveldb::WriteBatch b;b.Put("R/"+integer(sid)+key.substr(2),location);b.Put("N/"+integer(sid),integer(count+length+44));check(db_->Write({},&b));
    }
    std::optional<std::pair<std::string,std::string>> next(uint64_t sid,const std::string& after) const {
        const auto prefix="R/"+integer(sid);
        auto it=std::unique_ptr<leveldb::Iterator>(db_->NewIterator(options()));
        if(after.empty())it->Seek(prefix);
        else {if(after.size()!=34 || !after.starts_with("C/"))throw std::runtime_error("invalid physical GC scratch cursor");
            auto key=prefix+after.substr(2);it->Seek(key);if(it->Valid() && it->key().ToString()==key)it->Next();}
        check(it->status());if(!it->Valid() || !it->key().starts_with(prefix))return std::nullopt;
        auto key=it->key().ToString(),value=it->value().ToString();
        if(key.size()!=42 || value.size()!=20)throw std::runtime_error("invalid physical GC scratch reference");
        return std::pair<std::string,std::string>{"C/"+key.substr(10),value};
    }
};
}
