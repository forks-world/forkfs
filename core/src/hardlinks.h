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
// Every restore happens before the publish rename (P8), so a crash leaves the half-built tree and
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
    // PR #1 review (8th round): what the manifest's own `#hl` header said, when this set was
    // read back from one. The header is written before the lines it describes, so it is the only
    // thing that can tell a manifest that ends where it was meant to from one that was cut
    // short: a truncated last member leaves the group count intact and one group a name short,
    // and a group with a single name is silently skipped by the replay. Filled by
    // hardlinks_manifest_read, which refuses a set that does not match them; zero after a scan.
    uint64_t header_groups = 0;
    uint64_t header_names = 0;
    bool header_seen = false;
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
    // PR #1 review (5th round): the indices, in `set.groups`, of the groups this replay did NOT
    // leave whole -- one the verify root no longer agrees with, or one with a name that had
    // diverged from the canonical file and was therefore left alone. wfs_snapshot_create drops
    // them from the manifest it writes and from the row's hl_groups, so a snapshot never
    // advertises a group its own tree does not have: a fork from it replays the manifest without
    // a verify root (the snapshot is immutable) and would otherwise link one name over another
    // name's contents, which is the very thing this replay just refused to do.
    Vec<uint64_t> broken;
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
//
// PR #1 review (8th round): and a section that does not agree with its own header is -EINVAL,
// which every caller maps to WFS_E_SNAPSHOT_DIRTY. The header's counts used to be parsed and
// thrown away except for the external ones, so a manifest whose last member never reached the
// disk read back with the full group count and one group holding a single name -- which
// restore_group() skips without a word. The fork was published with two independent files where
// the snapshot records one inode under two names, and nothing downstream ever reads the manifest
// again to notice. So: there must be a header when there are `hl` lines, the groups and the
// names must be exactly what it says, and no group may hold fewer than two names (the scan never
// writes one -- a group whose names are not all inside the tree is counted as external instead).
//
// PR #1 review (9th round): and every group must hold exactly `nlink` names. "At least two" was
// too weak by one damage shape -- a member re-tagged into the neighbouring group leaves both the
// group count and the name count intact, so the header agrees, and the replay then links a name
// belonging to one inode onto another group's canonical file. The manifest needs no new field
// for this: a group is written only when the scan found every one of the inode's links inside
// the tree (paths.size() == nlink by construction), and a group that reaches outside is counted
// in the header's external totals and never written as `hl` lines. So `nlink` already IS the
// declared in-tree member count, the format is unchanged, and every manifest an older build of
// this library wrote still reads.
int hardlinks_manifest_read(const char *manifest_path, HardlinkSet &out);

// <store>/snapshots/S<n>/root -> <store>/snapshots/S<n>/manifest.
String hardlinks_manifest_path(const char *snapshot_root);

// Replays `set` inside `tree_root`, which must be a freshly cloned tree that nobody can see yet
// (a fork's recorded temporary or an unclaimed pool entry). For each group the first name that exists
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
// name, not a walk. A snapshot or checkpoint passes its own live source here for the same
// reason (PR #1 review, 5th round): the scan and the clone are two separate walks of a tree
// that nothing stops the user from writing to in between, and a member replaced in that window
// with a file of the same size and mtime is otherwise linked over.
int hardlinks_restore(const char *tree_root, const HardlinkSet &set, const char *verify_root,
                      HardlinkRestore *out);

// Checks `set` against the tree it was written from, before any of it is replayed anywhere.
// Returns 0, or -EINVAL for the first group `tree_root` does not agree with. Callers map that
// to WFS_E_SNAPSHOT_DIRTY.
//
// PR #1 review (13th round, P2): every check hardlinks_manifest_read makes is structural -- the
// header's totals, each group's size against its declared nlink, no name twice, no path that
// leaves the tree -- and a manifest whose members were exchanged between two groups passes all
// of them. (a,b),(c,d) written as (a,c),(b,d) keeps the group count, the name count, both
// nlinks, the uniqueness and the path sanity, and the replay then links `c` onto `a`: when the
// four files share a size and an mtime, which clones of one snapshot walk do, restore_group's
// "is this still the file the scan saw" guard cannot tell either, so the fork is published with
// two names welded and one file's content gone.
//
// No check on the manifest can catch that, because the manifest is the thing that is wrong. The
// snapshot's TREE is the immutable original, so that is what the groups are asked about: every
// member must lstat to one and the same inode, and that inode's st_nlink must be exactly the
// group's member count (the scan writes a group only when every one of the inode's links was
// found inside the tree, so in a snapshot this library published nlink IS the member count).
//
// Not the clone: clonefile breaks every hardlink, so a fresh clone has nothing to say about
// which of its names shared an inode. And not instead of hardlinks_restore's `verify_root`,
// which is the live-source check of the 5th round and stays exactly as it was -- a live tree is
// allowed to have moved on, so a group it no longer agrees with is skipped and reported, while
// a snapshot that no longer agrees with its own manifest is damage and a refusal.
//
// Cost: one lstat(2) per hardlinked name -- what hardlinks.h already prices a verify at -- and
// zero syscalls for the overwhelmingly common snapshot that has no groups at all. It must run
// inside the snapshot's read window (SnapGate): a gated root is mode 0000 and no name below it
// resolves until the gate is open.
int hardlinks_verify_groups(const char *tree_root, const HardlinkSet &set);

} // namespace wfs
