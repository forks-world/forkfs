#include "container.h"
#include "compaction.h"

namespace forkfs {
namespace {
const std::string history("\0forkfs-history/",16);
}
void Container::schedule_compaction_locked(const std::string& world) {
    if(compaction_running_ && !compaction_stop_ && compaction_pending_.insert(world).second) {
        try {compaction_queue_.push_back(world);}
        catch(...){compaction_pending_.erase(world);throw;}
        compaction_cv_.notify_all();
    }
}
void Container::start_compaction() {
    std::lock_guard lifecycle(compaction_lifecycle_mutex_);
    std::lock_guard lock(mutex_);if(compaction_running_ || journal_->is_leveldb())return;
    std::vector<std::string> due;
    for(const auto& key:journal_->keys(history+"world/")) {
        auto name=key.substr((history+"world/").size());
        NamespaceView view(*journal_,name,false,false);
        if(view.run_count()>=NamespaceView::soft_run_limit)due.push_back(name);
    }
    compaction_stop_=false;compaction_running_=true;
    try {compaction_thread_=std::thread(&Container::compaction_loop,this);}
    catch(...){compaction_running_=false;throw;}
    for(const auto& name:due)schedule_compaction_locked(name);
}
void Container::stop_compaction() {
    std::lock_guard lifecycle(compaction_lifecycle_mutex_);
    {
        std::lock_guard lock(mutex_);if(!compaction_running_)return;
        compaction_stop_=true;compaction_cv_.notify_all();
    }
    if(compaction_thread_.joinable())compaction_thread_.join();
    std::lock_guard lock(mutex_);compaction_running_=false;compaction_active_=false;
    compaction_queue_.clear();compaction_pending_.clear();compaction_cv_.notify_all();
}
bool Container::wait_compaction(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return compaction_cv_.wait_for(lock,timeout,[&]{return !compaction_active_ && compaction_queue_.empty();});
}
std::string Container::compaction_error() const {std::lock_guard lock(mutex_);return compaction_error_;}
void Container::compaction_loop() {
    std::unique_lock lock(mutex_);
    while(true) {
        compaction_cv_.wait(lock,[&]{return compaction_stop_ || !compaction_queue_.empty();});
        if(compaction_stop_)break;
        auto world=std::move(compaction_queue_.front());compaction_queue_.pop_front();compaction_pending_.erase(world);
        compaction_active_=true;lock.unlock();bool installed=false;std::string error;
        try {
            Compaction task(*this,world);task.prepare();task.build();installed=task.install();
            if(installed && task.output_runs()>=NamespaceView::hard_run_limit)
                error="full compaction exhausted manifest run capacity";
        }
        catch(const std::exception& e){error=e.what();}
        lock.lock();compaction_active_=false;
        const bool failed=!error.empty();
        if(failed){++compaction_failures_;++compaction_world_failures_[world];compaction_error_=world+": "+error;}
        else if(installed)++compaction_completed_;
        else {++compaction_races_;schedule_compaction_locked(world);}
        compaction_cv_.notify_all();
        // Round-robin queue plus short race backoff avoids spinning on a hot World.
        if(!installed && !failed && !compaction_stop_)
            compaction_cv_.wait_for(lock,std::chrono::milliseconds(10),[&]{return compaction_stop_;});
    }
}
}
