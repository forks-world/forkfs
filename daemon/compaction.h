#pragma once
#include "container.h"
#include "metadata_view.h"

namespace forkfs {
// A job fixes its input under the container lock, computes using owned immutable
// bytes without the lock, and publishes only if the selected head still matches.
class Compaction {
public:
    Compaction(Container& container,std::string world):container_(container),world_(std::move(world)) {}
    void prepare();
    void build();
    enum class StageResult {Staged,Ready,Stale};
    StageResult stage_next();
    bool install(); // Stale candidates never switch head; staged objects may remain.
    static constexpr size_t stage_bytes=256*1024;
    size_t output_runs() const {return plan_.run_count;}
private:
    Container& container_;
    std::string world_;
    std::optional<NamespaceView::ObjectId> head_;
    uint64_t sequence_=0;
    NamespaceView::Frozen input_;
    NamespaceView::Plan plan_;
    std::vector<std::pair<std::string,NamespaceView::Bytes>> output_;
    std::vector<NamespaceView::ObjectId> output_ids_;
    NamespaceView::ObjectId output_head_{};
    size_t stage_cursor_=0;
    bool staging_started_=false;
    bool matches_head_locked() const;
    bool prepared_=false,built_=false,installed_=false;
};
}
