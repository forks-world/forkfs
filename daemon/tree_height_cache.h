#pragma once
#include <array>
#include <list>
#include <map>
#include <mutex>
#include <optional>
namespace forkfs {
// Repository-local hints for verified immutable persisted nodes. Eviction only
// costs another validation walk; no on-disk format or durability dependency.
class TreeHeightCache {
public:
    using Id=std::array<unsigned char,32>;
    explicit TreeHeightCache(size_t capacity=65536):capacity_(capacity) {}
    std::optional<unsigned> get(const Id& id) {
        std::lock_guard lock(mutex_);auto it=entries_.find(id);
        if(it==entries_.end())return std::nullopt;
        lru_.splice(lru_.begin(),lru_,it->second.position);return it->second.height;
    }
    void put(const Id& id,unsigned height) {
        std::lock_guard lock(mutex_);if(!capacity_)return;
        auto it=entries_.find(id);
        if(it!=entries_.end()){lru_.splice(lru_.begin(),lru_,it->second.position);return;}
        if(entries_.size()==capacity_){entries_.erase(lru_.back());lru_.pop_back();}
        lru_.push_front(id);entries_.emplace(id,Entry{height,lru_.begin()});
    }
private:
    struct Entry {unsigned height;std::list<Id>::iterator position;};
    const size_t capacity_;
    std::mutex mutex_;
    std::list<Id> lru_;
    std::map<Id,Entry> entries_;
};
}
