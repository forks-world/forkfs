// See snapshot_access.h: the gate on a snapshot root, and the single place that opens and
// closes a snapshot's read window.
#include "snapshot_access.h"

#include <errno.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wfs {

namespace {

// <store>/snapshots/S<n>/root -> <store>/snapshots/S<n>/manifest. The manifest is the lock
// file: it is a sibling of the root, so it is never behind the gate.
String manifest_of(const char *snap_root) {
    String p(snap_root);
    const char *slash = ::strrchr(snap_root, '/');
    if (!slash) { p.assign("manifest"); return p; }
    p.resize((size_t)(slash - snap_root) + 1);
    p.append("manifest");
    return p;
}

} // namespace

int SnapGate::open(const char *snap_root, bool hard) {
    if (hard) return 0;   // --hard snapshots are protected per entry, not by a gate
    if (!snap_root || !*snap_root) return -EINVAL;
    String lock = manifest_of(snap_root);
    fd = ::open(lock.c_str(), O_RDONLY);
    if (fd < 0) return -errno;
    // Blocking: the window is one clonefile (or one comparison) long, and waiting is better
    // than failing.
    if (::flock(fd, LOCK_EX) != 0) { int e = errno; ::close(fd); fd = -1; return -e; }
    if (::chmod(snap_root, WFS_GATE_OPEN) != 0) {
        int e = errno;
        ::flock(fd, LOCK_UN);
        ::close(fd);
        fd = -1;
        return -e;
    }
    root.assign(snap_root);
    restore = WFS_GATE_CLOSED + 1;   // non-zero marker; the mode itself is a constant
    return 0;
}

void SnapGate::close() {
    if (restore) { ::chmod(root.c_str(), WFS_GATE_CLOSED); restore = 0; }
    if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); fd = -1; }
}

int snapshot_open_for_read(const char *snapshot_root, SnapshotRead &h) {
    snapshot_close_after_read(h);
    if (!snapshot_root || !*snapshot_root) return -EINVAL;
    struct stat st;
    if (::lstat(snapshot_root, &st) != 0) return -errno;
    if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
    h.open = true;

    // A --hard snapshot keeps a 0555 root (the protection is UF_IMMUTABLE on the entries), and
    // so does a source that is not a snapshot at all: nothing to open, nothing to serialise on.
    if (::access(snapshot_root, R_OK | X_OK) == 0) return 0;

    // Gated. Only the owner can lift it; anything else is a genuine refusal rather than
    // something to work around.
    if (st.st_uid != ::geteuid()) {
        snapshot_close_after_read(h);
        return -EACCES;
    }
    if (int rc = h.gate.open(snapshot_root, false)) {
        snapshot_close_after_read(h);
        return rc;
    }
    return 0;
}

void snapshot_close_after_read(SnapshotRead &h) {
    h.gate.close();
    h.open = false;
}

} // namespace wfs
