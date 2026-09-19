// The one door into a snapshot's contents, and the gate that door opens. Not part of the C ABI.
//
// P3 (T1.1b): a snapshot root is mode WFS_GATE_CLOSED (0000) and the entries below it are left
// exactly as the clone made them. The kernel refuses to resolve any name below an untraversable
// directory, so nothing inside can be listed, read, written, created or deleted, and a fork
// costs one clonefile with nothing to undo. `--hard` snapshots (snapshots.hard = 1) instead
// carry UF_IMMUTABLE on every entry and keep a readable root; they need no gate.
//
// SnapGate is the only thing that opens a gated root, and it is shared: `fork`, `checkpoint`,
// `verify` (core/src/world.cpp) and `diff` (through SnapshotRead, below) all serialise on the
// same exclusive flock, taken on the snapshot's `manifest` file -- which lives next to the root
// and is never gated. Two holders therefore do not share a window: the second waits for the
// first to close the gate and then opens it again.
//
// Cost note for callers: the flock is held for as long as the gate is open, so a long reader
// (a diff's comparison phase is 0.2-1.4 s on 50k entries) delays every fork from that same
// snapshot for that long. Open the gate around the phase that actually reads snapshot bytes,
// not around the whole command.
#pragma once
#include "internal.h"

#include <errno.h>
#include <fcntl.h>

namespace wfs {

// The gate itself. `hard` snapshots are a no-op: open() returns 0 and close() does nothing.
struct SnapGate {
    String root;
    uint32_t restore = 0;   // 0 = nothing to do (a --hard snapshot, or never opened)
    int fd = -1;

    SnapGate() = default;
    SnapGate(const SnapGate &) = delete;
    SnapGate &operator=(const SnapGate &) = delete;
    ~SnapGate() { close(); }

    // snap_root is <store>/snapshots/S<n>/root; the lock file is its sibling `manifest`.
    // Blocks until the flock is free. Returns 0 or a negative errno.
    int open(const char *snap_root, bool hard);
    void close();
};

// A snapshot opened for reading: the gate plus the bookkeeping that tells close() whether there
// is anything to undo.
struct SnapshotRead {
    SnapGate gate;
    bool open = false;
};

// Grants this process read+traverse access to `snapshot_root` until snapshot_close_after_read().
// A root that is already traversable (a --hard snapshot, mode 0555) is accepted as is; a gated
// root goes through SnapGate, which means this call blocks while another fork or verify holds
// the window. Returns 0, or a negative errno (-ENOENT when the snapshot tree is gone, -ENOTDIR
// when it is not a directory, -EACCES when the gate cannot be opened). Every successful call
// must be paired with a close.
int snapshot_open_for_read(const char *snapshot_root, SnapshotRead &h);

// Closes the window again, restoring whatever snapshot_open_for_read() changed -- a gated root
// goes back to WFS_GATE_CLOSED. Safe on a handle that was never opened, and on every error path
// (the guard below is how callers get that for free).
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
