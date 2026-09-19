// The one door into a snapshot's contents. Not part of the C ABI.
//
// Today a snapshot root is mode 0555 with UF_IMMUTABLE on every entry (P3), so reading one
// needs nothing special and snapshot_open_for_read() only checks that the root is there. The
// gate-directory work turns snapshot roots into mode 0000 directories that are opened 0500
// only for the duration of a clone; when that lands, this file is the single place that opens
// and closes that window, and every reader of snapshot bytes -- `diff`, `verify`, anything
// later -- already goes through it.
//
// Scope note: the handle deliberately covers the ROOT directory only. The entries below it stay
// immutable throughout; what the gate controls is whether anyone can traverse into the tree.
#pragma once
#include "internal.h"

namespace wfs {

struct SnapshotRead {
    String root;
    bool open = false;
    bool restore_mode = false;  // we widened the root's mode and owe it a chmod back
    bool restore_flags = false; // ... and had to clear UF_IMMUTABLE to be allowed to
    uint32_t saved_mode = 0;
    uint32_t saved_flags = 0;
};

// Grants this process read+traverse access to `snapshot_root` until snapshot_close_after_read().
// Returns 0, or a negative errno (-ENOENT when the snapshot tree is gone, -EACCES when the gate
// cannot be opened). Idempotent per handle; every successful call must be paired with a close.
int snapshot_open_for_read(const char *snapshot_root, SnapshotRead &h);

// Closes the window again, restoring whatever snapshot_open_for_read() changed. Safe on a
// handle that was never opened.
void snapshot_close_after_read(SnapshotRead &h);

// RAII wrapper, because every caller wants exactly that.
struct SnapshotReadGuard {
    SnapshotRead h;
    int rc = -EINVAL;
    explicit SnapshotReadGuard(const char *root) { rc = snapshot_open_for_read(root, h); }
    ~SnapshotReadGuard() { snapshot_close_after_read(h); }
    SnapshotReadGuard(const SnapshotReadGuard &) = delete;
    SnapshotReadGuard &operator=(const SnapshotReadGuard &) = delete;
};

} // namespace wfs
