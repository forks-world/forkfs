// See snapshot_access.h: the single place that opens and closes a snapshot's read window.
#include "snapshot_access.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wfs {

int snapshot_open_for_read(const char *snapshot_root, SnapshotRead &h) {
    h = SnapshotRead();
    if (!snapshot_root || !*snapshot_root) return -EINVAL;
    struct stat st;
    if (::lstat(snapshot_root, &st) != 0) return -errno;
    if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
    h.root.assign(snapshot_root);
    h.saved_mode = (uint32_t)(st.st_mode & 07777);
#ifdef __APPLE__
    h.saved_flags = (uint32_t)st.st_flags;
#endif
    h.open = true;

    // The common case today: 0555, readable and traversable, nothing to do.
    if (::access(snapshot_root, R_OK | X_OK) == 0) return 0;

    // The gate-directory case: widen the root to 0500 for the duration. Only the owner can do
    // this, and only after UF_IMMUTABLE is out of the way; anything else is a genuine refusal
    // rather than something to work around.
    if (st.st_uid != ::geteuid()) {
        snapshot_close_after_read(h);
        return -EACCES;
    }
    mode_t want = (mode_t)(h.saved_mode | 0500);
    if (::chmod(snapshot_root, want) != 0) {
#ifdef __APPLE__
        if (errno == EPERM && (h.saved_flags & (UF_IMMUTABLE | SF_IMMUTABLE)) != 0) {
            if (::lchflags(snapshot_root, h.saved_flags & ~(uint32_t)(UF_IMMUTABLE | SF_IMMUTABLE)) != 0) {
                int e = errno;
                snapshot_close_after_read(h);
                return -e;
            }
            h.restore_flags = true;
            if (::chmod(snapshot_root, want) != 0) {
                int e = errno;
                snapshot_close_after_read(h);
                return -e;
            }
            h.restore_mode = true;
            return 0;
        }
#endif
        int e = errno;
        snapshot_close_after_read(h);
        return -e;
    }
    h.restore_mode = true;
    return 0;
}

void snapshot_close_after_read(SnapshotRead &h) {
    if (!h.open) return;
    if (h.restore_mode) ::chmod(h.root.c_str(), (mode_t)h.saved_mode);
#ifdef __APPLE__
    if (h.restore_flags) ::lchflags(h.root.c_str(), h.saved_flags);
#endif
    h.restore_mode = false;
    h.restore_flags = false;
    h.open = false;
}

} // namespace wfs
