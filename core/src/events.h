// Change-candidate collection for `world fs diff` (T1.3). Not part of the C ABI.
//
// On Darwin the candidates come from an FSEvents historical replay starting at the event id
// recorded when the world was forked (docs/CLONE_MODEL_MACOS27.md §6.2). Everywhere else there
// is no such source and wfs_world_diff falls back to the full two-tree walk.
//
// The candidates are never the answer (P10): every one of them is verified against the source
// snapshot by diff.cpp. This layer's only job is to be honest about when it cannot enumerate
// them, which it reports as one of the non-OK statuses below.
#pragma once
#include "internal.h"

#include <stdlib.h>
#include <string.h>

namespace wfs {

enum fs_events_status {
    FS_EV_OK = 0,      // every change since the cursor is in the bag
    FS_EV_UNSUPPORTED, // no FSEvents here, or the stream could not be created
    FS_EV_STALE,       // the cursor is older than the volume's event journal
    FS_EV_MUST_SCAN,   // kFSEventStreamEventFlagMustScanSubDirs
    FS_EV_DROPPED,     // kFSEventStreamEventFlagUserDropped / KernelDropped
    FS_EV_WRAPPED,     // kFSEventStreamEventFlagEventIdsWrapped, or the counter went backwards
    FS_EV_TIMEOUT      // HistoryDone never arrived
};

// A pile of relative paths: one character buffer plus offsets, so nothing has to be moved when
// it grows and the sort at the end is a sort of pointers. (arch.md §39: no std::string, no
// std::unordered_map; a bag of C strings and qsort do the whole job.)
struct PathBag {
    Vec<char> buf;
    Vec<uint32_t> off;
    Vec<const char *> ptr; // valid after finish()

    void add(const char *s, size_t n) {
        size_t at = buf.size();
        if (at > 0xffffffffu) return; // 4 GB of candidate paths: something is very wrong
        buf.resize(at + n + 1);
        if (n) memcpy(buf.data() + at, s, n);
        buf.data()[at + n] = 0;
        off.emplace_back((uint32_t)at);
    }
    void add(const char *s) { add(s, strlen(s)); }

    static int cmp(const void *a, const void *b) {
        return strcmp(*(const char *const *)a, *(const char *const *)b);
    }
    // Sort and drop duplicates. One path can be named by several events (created, then written,
    // then chmod'ed) and the verification only wants to look at it once.
    void finish() {
        ptr.clear();
        ptr.reserve(off.size());
        for (size_t i = 0; i < off.size(); ++i) ptr.emplace_back(buf.data() + off[i]);
        if (ptr.size() > 1) {
            qsort(ptr.data(), ptr.size(), sizeof(const char *), cmp);
            size_t w = 1;
            for (size_t i = 1; i < ptr.size(); ++i)
                if (strcmp(ptr[i], ptr[w - 1]) != 0) ptr[w++] = ptr[i];
            ptr.resize(w);
        }
    }
    size_t size() const { return ptr.size(); }
    const char *at(size_t i) const { return ptr[i]; }
};

// Replays everything recorded for `root` since event id `since` and appends the changed paths,
// relative to `root`, to `out`. The delivery thread is the stream's own and does nothing but
// append, because a blocked consumer is the one thing that makes FSEvents drop events
// (CLONE_MODEL_MACOS27 §6.2). `dev` is the st_dev of the root and `since_time` the unix second
// the cursor was taken; together they answer "is this cursor still inside the journal".
// `sentinel_dir` is a directory this process may write to -- the store -- where a watermark file
// is created and watched alongside `root`, so that "the replay is complete" is observed rather
// than assumed; NULL falls back to a quiet-period heuristic.
// Returns an fs_events_status. On anything but FS_EV_OK the caller must do a full scan.
int fs_events_replay(const char *root, uint64_t since, uint64_t dev, int64_t since_time,
                     const char *sentinel_dir, PathBag &out);

} // namespace wfs
