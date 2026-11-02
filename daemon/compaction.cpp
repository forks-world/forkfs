#include "compaction.h"
#include <stdexcept>
#ifdef FORKFS_TEST_FAULTS
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#endif

namespace forkfs {
namespace {
const std::string history("\0forkfs-history/",16);
std::string hex(const NamespaceView::ObjectId& id) {
    std::string result;const char* chars="0123456789abcdef";
    for(auto c:id){result+=chars[c>>4];result+=chars[c&15];}return result;
}
void need(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
void fault(const char* stage) {
#ifdef FORKFS_TEST_FAULTS
    const char* value=std::getenv("FORKFS_TEST_CRASH");
    if(value && !strcmp(value,stage))_exit(86);
#else
    (void)stage;
#endif
}
}
void Compaction::prepare() {
    need(!prepared_,"compaction task already prepared");NamespaceView::valid_name(world_);
    std::lock_guard lock(container_.mutex_);const auto& journal=*container_.journal_;
    auto key=history+"world/"+world_;
    if(journal.contains(key))head_=journal.root_object(key);
    sequence_=journal.sequence();
    NamespaceView view(journal,world_,false,false);input_=view.freeze();prepared_=true;
}
void Compaction::build() {
    need(prepared_ && !built_,"compaction task cannot build");
    // No Journal access in this phase: foreground mutations can continue safely.
    plan_=NamespaceView::compact_frozen(input_);
    for(auto& payload:plan_.objects) {
        auto id=Journal::object_id(payload);output_ids_.push_back(id);output_.emplace_back(history+"object/"+hex(id),std::move(payload));
    }
    output_head_=Journal::object_id(plan_.manifest);built_=true;
}
bool Compaction::matches_head_locked() const {
    const auto& journal=*container_.journal_;auto key=history+"world/"+world_;
    std::optional<NamespaceView::ObjectId> current;
    if(journal.contains(key))current=journal.root_object(key);
    return current==head_ && (head_ || journal.sequence()==sequence_);
}
Compaction::StageResult Compaction::stage_next() {
    need(built_ && !installed_,"compaction task cannot stage");staging_started_=true;
    std::lock_guard lock(container_.mutex_);auto& journal=*container_.journal_;
    if(!matches_head_locked())return StageResult::Stale;
    size_t cursor=stage_cursor_,bytes=0;std::vector<Mutation> batch;
    while(cursor<output_.size()) {
        const auto& [key,payload]=output_[cursor];
        need(payload.size()<=stage_bytes,"compaction object exceeds staging budget");
        if(journal.contains(key)) {
            need(journal.root_object(key)==output_ids_[cursor],"staged object identity mismatch");
            need(journal.get(key)==payload,"staged object content mismatch");
            ++cursor;continue;
        }
        if(!batch.empty() && (bytes+payload.size()>stage_bytes || batch.size()==64))break;
        batch.push_back({key,payload});bytes+=payload.size();++cursor;
    }
    if(!batch.empty()) {
        journal.transact(journal.sequence(),batch);
        // Only our own staging advances the legacy fence; unrelated writes still invalidate it.
        if(!head_)sequence_=journal.sequence();
        stage_cursor_=cursor;fault("compaction_stage_committed");return StageResult::Staged;
    }
    stage_cursor_=cursor;return StageResult::Ready;
}
bool Compaction::install() {
    need(built_ && !installed_,"compaction task cannot install");
    {
        std::lock_guard lock(container_.mutex_);
        if(!matches_head_locked())return false;
        if(head_ && *head_==output_head_){installed_=true;return true;}
    }
    size_t total=plan_.manifest.size();for(const auto& entry:output_)total+=entry.second.size();
    if(staging_started_ || total>stage_bytes) {
        for(;;) {
            auto result=stage_next();if(result==StageResult::Stale)return false;
            if(result==StageResult::Ready)break;
            // Each call releases the container lock before the next durable batch.
        }
    }
    std::lock_guard lock(container_.mutex_);auto& journal=*container_.journal_;
    if(!matches_head_locked())return false;
    if(head_ && *head_==output_head_){installed_=true;return true;}
    std::vector<Mutation> batch;
    if(!staging_started_) {
        for(size_t i=0;i<output_.size();++i) {
            const auto& [object,payload]=output_[i];
            if(journal.contains(object)) {
                need(journal.root_object(object)==output_ids_[i],"compaction object identity mismatch");
                need(journal.get(object)==payload,"compaction object content mismatch");
            } else batch.push_back({object,payload});
        }
    } else {
        for(size_t i=0;i<output_.size();++i)
            need(journal.contains(output_[i].first) && journal.root_object(output_[i].first)==output_ids_[i],
                 "compaction output missing before publication");
    }
    // This is the sole visibility point. Staging never changes a World head.
    batch.push_back({history+"world/"+world_,plan_.manifest});fault("compaction_before_publish");
    journal.transact(journal.sequence(),batch);installed_=true;fault("compaction_published");
    container_.compaction_cv_.notify_all();return true;
}
}
