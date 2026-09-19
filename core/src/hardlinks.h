// P9 (T2.5): hardlinks inside a tree, and how a clone gets them back.
//
// clonefile(dir) -- and every alternative measured in docs/CLONE_MODEL_MACOS27.md §11 -- breaks
// intra-tree hardlinks: each name becomes an independent inode with nlink 1 that happens to
// share extents. Tools that rely on link identity (pnpm/uv stores, git object packs, cargo
// target caches) then see two files where they put one, and the first write through either name
// silently doubles the storage instead of being visible through both.
//
// The fix has two halves:
//
//   scan      the walk that a snapshot already does over its SOURCE records, for every backing
//             (dev, ino) with st_nlink > 1, the relative names that carry it. A group whose
//             names are all inside the tree can be rebuilt; one that reaches outside (the names
//             found are fewer than st_nlink) cannot, and is only counted.
//   restore   after a clone -- of a snapshot, of a world, of a pool entry -- the groups are
//             replayed: the first name is the canonical file, the others are unlinked and
//             linked to it. That is O(hardlinked names), not O(tree).
//
// Every restore happens before the publish rename (P8), so a crash leaves a `.wfs-tmp` tree and
// nothing else. The groups travel in the snapshot's manifest, as an additive section that the
// existing manifest reader skips (see hardlinks_manifest_write).
#pragma once
#include "internal.h"

#include <stdio.h>

namespace wfs {

// One backing inode and every name inside the tree that carries it. `paths` is sorted, so the
// canonical name (paths[0]) is the same on every machine and in every re-scan.
struct HardlinkGroup {
    uint64_t nlink = 0;   // st_nlink as the source reported it; == paths.size() for a full group
    Vec<String> paths;    // relative to the tree root, never a leading '/'
};

// The restorable groups of one tree, plus what had to be given up on.
struct HardlinkSet {
    Vec<HardlinkGroup> groups;    // fully inside the tree; these are the ones that get replayed
    uint64_t names = 0;           // total names in `groups`
    uint64_t external_groups = 0; // groups with at least one name outside the tree
    uint64_t external_names = 0;  // the names of those groups that are inside it
};

// What one replay did. Nothing here is fatal: a fork whose hardlinks could not all be rebuilt
// is still a correct fork, only a less thrifty one, so the counters are reported, not returned.
struct HardlinkRestore {
    uint64_t groups = 0;   // groups with at least one link rebuilt
    uint64_t links = 0;    // names relinked to their canonical file
    uint64_t missing = 0;  // names that were not in the clone at all (tolerated, see below)
    uint64_t skipped = 0;  // names that had diverged from the canonical file, or groups the
                           // verify root no longer agrees with
    int first_err = 0;     // first negative errno from link(2)/rename(2), 0 when there was none
};

// One parallel walk of `root` that both counts (exactly as fs_count_entries does, so callers
// that need TreeStats pay for nothing extra) and collects the hardlink groups. Regular files
// only: a directory's nlink is its subdirectory count, and a symlink is never a link target
// here because the walk lstat()s.
int hardlinks_scan(const char *root, TreeStats *stats, HardlinkSet &out);

// Appends the set to an open manifest, as
//
//     #hl 1 <groups> <names> <external groups> <external names>
//     hl <group> <nlink> <relative path>
//
// Additive by construction: wfs_snapshot_verify's reader requires a space at line[1] before it
// parses anything, and neither "#hl" nor "hl " has one, so both line kinds are skipped by every
// manifest reader that predates them. Escaping is the manifest's own ('\\' and '\n').
void hardlinks_manifest_write(FILE *f, const HardlinkSet &set);

// Reads that section back. A manifest without one (an older store, or a tree that had no
// hardlinks) yields an empty set and rc 0; a missing manifest yields -ENOENT. The entry lines
// are skipped without being parsed, so the cost is one pass over the file -- which is why
// callers gate this on the snapshot row's hl_groups being non-zero.
int hardlinks_manifest_read(const char *manifest_path, HardlinkSet &out);

// <store>/snapshots/S<n>/root -> <store>/snapshots/S<n>/manifest.
String hardlinks_manifest_path(const char *snapshot_root);

// Replays `set` inside `tree_root`, which must be a freshly cloned tree that nobody can see yet
// (a `.wfs-tmp` tree or an unclaimed pool entry). For each group the first name that exists
// becomes the canonical file and the others are relinked to it: link(2) to a temporary name in
// the same directory, then rename(2) over the target, so a failure half way leaves the original
// file in place rather than a hole.
//
// A name is left alone -- never unlinked -- when it is gone (ENOENT: possible when the source
// was a live world, whose tree can change between the scan and the clone), when it is not a
// regular file, or when its size or mtime differ from the canonical file's, which is what a
// name replaced between the scan and the clone looks like.
//
// `verify_root`, when non-null, is a live tree the group is checked against first: every name
// of the group must still exist there and still share one inode. That is how a fork from a live
// world uses its origin snapshot's groups without trusting them -- the cost is one lstat per
// name, not a walk.
int hardlinks_restore(const char *tree_root, const HardlinkSet &set, const char *verify_root,
                      HardlinkRestore *out);

} // namespace wfs
