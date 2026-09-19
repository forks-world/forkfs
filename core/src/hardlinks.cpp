// See hardlinks.h: collecting the hardlink groups of a tree, carrying them in the manifest, and
// replaying them onto a clone (P9, T2.5).
#include "hardlinks.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>   // std::move only

// The test seam for "the replay failed" (PR #1 review, 4th round). A link(2) that the file
// system refuses is not something a test can produce on a developer's machine, and what has to
// be pinned down is the unwinding: no S<n> tree, no row, no half-built world. Nothing in the
// library ever assigns this; it is 0 in every run that is not core_test.
extern "C" int wfs_test_hardlink_restore_err = 0;

namespace wfs {

namespace {

// ---- the member path, walked the way it is meant to be read ------------------------------
//
// PR #1 review (17th round, P1): a member path is a chain of real NAMES, and a name is not a
// symlink either. manifest_path_sane() below is lexical -- no leading '/', no `..`, no empty
// and no `.` component -- and `s/a` passes every one of those when `s` is a symlink to a
// directory. The syscalls a group is made of then follow it: lstat(2) does not follow the LAST
// component and follows every one before it, link(2) and rename(2) follow all of them. So
// hardlinks_verify_groups approved whatever the symlink led to beside the SNAPSHOT and the
// replay linked and renamed over whatever the same symlink leads to beside the CLONE -- and a
// relative symlink lands somewhere else in every copy of the tree, because a snapshot's root
// and a fork's temporary sit at different depths under different parents. A manifest naming
// `s/x`, `s/y` could therefore be verified against a hardlinked pair planted next to the
// snapshot and replayed over two files that are not in the clone at all.
//
// So the path is walked from the tree root, one component at a time, each opened with
// O_DIRECTORY|O_NOFOLLOW, and every operation on the leaf is an *at(2) call relative to the
// parent directory's descriptor. The check and the operation are then the same syscall path --
// nothing is resolved twice and there is no window in between -- and a symlink component is
// ELOOP before anything has been created anywhere.
//
// O_RDONLY rather than O_SEARCH: the lend in the replay needs fchmod(2) on the descriptor, and
// a directory that can hold a member was enumerated by readdir(3) in the scan that produced
// that member's name, so it is readable by construction.
const int kDirFlags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;

// The tree root itself. Its last component is the caller's own path, never a manifest's: a
// store-internal directory of ours, or a source the caller has already put through realpath(3)
// (check_path, PATH_SOURCE). Returns the fd or -errno.
int open_tree_root(const char *root) {
    if (!root || !*root) return -EINVAL;
    int fd = ::open(root, kDirFlags);
    return fd < 0 ? -errno : fd;
}

// The directory holding `rel`'s last component, descended from `rootfd`; `leaf` gets that last
// component. Always a fresh descriptor, so every caller closes what it gets. -ELOOP means a
// component was a symlink, -ENOTDIR that it was not a directory at all.
int open_parent(int rootfd, const char *rel, String &leaf) {
    if (!rel || !*rel) return -EINVAL;
    int cur = ::dup(rootfd);
    if (cur < 0) return -errno;
    for (const char *s = rel;;) {
        const char *n = ::strchr(s, '/');
        if (!n) { leaf.assign(s); return cur; }
        String comp;
        comp.assign(s, (size_t)(n - s));
        int nx = ::openat(cur, comp.c_str(), kDirFlags);
        int e = nx < 0 ? -errno : 0;
        ::close(cur);
        if (nx < 0) return e;
        cur = nx;
        s = n + 1;
    }
}

// lstat(2) of a member, asked of its parent's descriptor: every component above the leaf has
// been proven a real directory by the descent, and the leaf itself is not followed.
int member_lstat(int rootfd, const char *rel, struct stat &st) {
    String leaf;
    int fd = open_parent(rootfd, rel, leaf);
    if (fd < 0) return fd;
    int rc = ::fstatat(fd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 ? 0 : -errno;
    ::close(fd);
    return rc;
}

int64_t mtime_ns(const struct stat &st) {
#ifdef __APPLE__
    return (int64_t)st.st_mtimespec.tv_sec * 1000000000 + (int64_t)st.st_mtimespec.tv_nsec;
#else
    return (int64_t)st.st_mtim.tv_sec * 1000000000 + (int64_t)st.st_mtim.tv_nsec;
#endif
}

// ---- the scan --------------------------------------------------------------------------------

// One name with more than one link. POD on purpose: the grouping is a qsort of these, and the
// name itself stays put in a vector that is never reordered, so `idx` keeps pointing at it.
struct Rec {
    uint64_t dev;
    uint64_t ino;
    uint64_t nlink;
    uint32_t idx;
};

int rec_cmp(const void *a, const void *b) {
    const Rec *x = (const Rec *)a, *y = (const Rec *)b;
    if (x->dev != y->dev) return x->dev < y->dev ? -1 : 1;
    if (x->ino != y->ino) return x->ino < y->ino ? -1 : 1;
    return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}

int str_cmp(const void *a, const void *b) {
    return ::strcmp(*(const char *const *)a, *(const char *const *)b);
}

// PR #1 review (22nd round, P2): the inode behind the excluded name, and how many of that
// inode's links wear a name this tree is not publishing. `n` is 1 in every run that has one at
// all -- `exclude` is a single name, matched whole -- but the subtraction below is written as a
// count so that it stays the subtraction it means.
struct ExclIno {
    uint64_t dev;
    uint64_t ino;
    uint64_t n;
};

struct ScanCtx {
    TreeStats *stats;
    const char *exclude;   // the one tree-relative name that is not a file of this tree
    Vec<Rec> recs;
    Vec<String> names;
    Vec<ExclIno> excl;     // (dev, ino) of the excluded name, when it has links to spare
    Mutex mu;
};

void bump(uint64_t &v) { __atomic_fetch_add(&v, 1, __ATOMIC_RELAXED); }

// Runs on four walker threads. The lock is only taken for entries that actually have more than
// one link, which in the trees this exists for (a pnpm store, a git pack) is a small minority
// and in every other tree is nobody.
int scan_entry(void *ctx, const char *, const char *rel, const struct stat &st, bool is_dir) {
    ScanCtx *c = (ScanCtx *)ctx;
    if (!*rel) return 0;   // the root itself is not an entry of the tree
    // PR #1 review (14th round, P2): the one name the caller takes back out of the clone. A
    // checkpoint unlinks the source world's `.world` marker from the tree it is about to
    // publish -- the world's identity is not the snapshot's -- so the marker is not a file of
    // that tree. A marker somebody's tool had hardlinked (a backup copy, a content-addressed
    // store, a `cp -al` of the tree next door) otherwise went into a group whose member the
    // snapshot did not have, and every verify and every fork of it refused the snapshot from
    // the moment it was published. Exactly one name, matched whole: a `.world` inside a
    // sub-world is left alone here because snapshot creation leaves it alone too.
    const bool excluded = c->exclude && !::strcmp(rel, c->exclude);
    if (c->stats) {
        bump(c->stats->entries);
        if (is_dir) bump(c->stats->dirs);
        else {
            bump(c->stats->files);
            // Nor is it one of the tree's hardlinked files, however many links its inode has.
            // What stays behind is its twin -- or its twins: whether that inode is still
            // hardlinked at all, once this name is gone, is a question about the whole inode and
            // is settled in the grouping pass below (22nd round), which also takes this bump
            // back for the twin that turns out to be alone.
            if (st.st_nlink > 1 && !excluded) bump(c->stats->hardlinks);
        }
    }
    if (excluded) {
        // PR #1 review (22nd round, P2): and its inode is remembered, because the names that
        // stay behind still count it in their own st_nlink. See the grouping pass.
        if (!is_dir && S_ISREG(st.st_mode) && st.st_nlink > 1) {
            Guard g(c->mu);
            bool found = false;
            for (size_t i = 0; i < c->excl.size(); ++i)
                if (c->excl[i].dev == (uint64_t)st.st_dev && c->excl[i].ino == (uint64_t)st.st_ino) {
                    c->excl[i].n++;
                    found = true;
                    break;
                }
            if (!found) {
                ExclIno e;
                e.dev = (uint64_t)st.st_dev;
                e.ino = (uint64_t)st.st_ino;
                e.n = 1;
                c->excl.emplace_back(e);
            }
        }
        return 0;
    }
    // Directories carry nlink > 1 by construction (one link per subdirectory) and cannot be
    // hardlinked; symlinks are lstat'ed here, and link(2) on one is not what any of this means.
    if (is_dir || !S_ISREG(st.st_mode) || st.st_nlink <= 1) return 0;
    Guard g(c->mu);
    Rec r;
    r.dev = (uint64_t)st.st_dev;
    r.ino = (uint64_t)st.st_ino;
    r.nlink = (uint64_t)st.st_nlink;
    r.idx = (uint32_t)c->names.size();
    c->names.emplace_back(rel);
    c->recs.emplace_back(r);
    return 0;
}

} // namespace

int hardlinks_scan(const char *root, const char *exclude_rel, TreeStats *stats,
                   HardlinkSet &out) {
    if (stats) *stats = TreeStats();
    out = HardlinkSet();
    ScanCtx c;
    c.stats = stats;
    c.exclude = (exclude_rel && *exclude_rel) ? exclude_rel : nullptr;
    if (int rc = fs_walk_tree(root, 4, FS_DIRS_PRE, &c, scan_entry)) return rc;
    if (c.recs.empty()) return 0;

    // Group by backing inode. The walk order is four threads deep, so the names of a group are
    // sorted afterwards: the canonical name must not depend on which thread got there first.
    ::qsort(c.recs.data(), c.recs.size(), sizeof(Rec), rec_cmp);
    size_t i = 0;
    while (i < c.recs.size()) {
        size_t j = i + 1;
        while (j < c.recs.size() && c.recs[j].dev == c.recs[i].dev && c.recs[j].ino == c.recs[i].ino) ++j;
        size_t k = j - i;
        // PR #1 review (22nd round, P2): the inode's links, MINUS the ones the caller is taking
        // back out of the tree. The 14th round dropped the excluded name's own record and
        // nothing else, so the names that stayed behind kept an st_nlink that still counts it:
        // `.world` hardlinked with `m1` and `m2` is one inode with nlink 3, of which the scan
        // finds 2, and the test below -- k != nlink -- called that a group reaching outside the
        // tree. It does not reach outside. The third name is ours and it is about to be
        // unlinked, so the truth about the published tree is that `m1` and `m2` are one inode
        // under two names -- and writing nothing meant the clone's broken links stayed broken
        // and the snapshot went out with two independent files. Only a name that is really
        // outside this tree may make a group external.
        uint64_t nl = c.recs[i].nlink;
        for (size_t e = 0; e < c.excl.size(); ++e)
            if (c.excl[e].dev == c.recs[i].dev && c.excl[e].ino == c.recs[i].ino) {
                nl = c.excl[e].n < nl ? nl - c.excl[e].n : 0;
                break;
            }
        // One name left after the exclusion, and no link of that inode anywhere else: not a
        // group (a group is two names or it is nothing) and not external either. It is a plain
        // file in the published tree, so it is also not one of the tree's hardlinked files --
        // the `hardlinks` bump the walk made for it above is taken back here, where the whole
        // inode is finally in view. (`nl` is the source's count minus OUR names: k == nl == 1
        // is only reachable through an exclusion, because a record exists only for nlink > 1.)
        if (k == 1 && nl == 1) {
            if (stats && stats->hardlinks) stats->hardlinks--;
            i = j;
            continue;
        }
        // Fewer names here than the inode has links: the rest are outside this tree, and a
        // clone cannot be given links to files it does not contain. Count and move on.
        if (k != (size_t)nl || k < 2) {
            out.external_groups++;
            out.external_names += (uint64_t)k;
            i = j;
            continue;
        }
        Vec<const char *> ptr;
        for (size_t t = i; t < j; ++t) ptr.emplace_back(c.names[c.recs[t].idx].c_str());
        if (ptr.size() > 1) ::qsort(ptr.data(), ptr.size(), sizeof(const char *), str_cmp);
        HardlinkGroup g;
        // The nlink the PUBLISHED tree will have, which is the number of names in the group --
        // the same value as before for every group with nothing excluded from it, and the only
        // correct one for a group the marker was on: hardlinks_verify_groups and the manifest
        // reader both hold a group to `nlink == paths.size()` against the tree that is there.
        g.nlink = (uint64_t)k;
        for (size_t t = 0; t < ptr.size(); ++t) g.paths.emplace_back(ptr[t]);
        out.names += (uint64_t)g.paths.size();
        out.groups.emplace_back(std::move(g));
        i = j;
    }
    return 0;
}

// ---- the manifest section ----------------------------------------------------------------------

String hardlinks_manifest_path(const char *snapshot_root) {
    String p(snapshot_root ? snapshot_root : "");
    const char *slash = snapshot_root ? ::strrchr(snapshot_root, '/') : nullptr;
    if (!slash) { p.assign("manifest"); return p; }
    p.resize((size_t)(slash - snapshot_root) + 1);
    p.append("manifest");
    return p;
}

namespace {

// PR #1 review (12th round): and CR. A file name may hold one -- it is an ordinary byte to
// every file system this runs on -- and the reader below used to strip a trailing one as if the
// file this very function writes might have CRLF line endings. So `a\r` was written raw and read
// back as `a`, and the replay then acted on a name that is somebody else's file. The three bytes
// that cannot survive a line-oriented format unescaped are backslash, LF and CR; nothing else in
// a path needs quoting here, because the terminator is the only structure a line has left.
void put_escaped(FILE *f, const char *s) {
    for (const char *p = s; *p; ++p) {
        if (*p == '\\') ::fputs("\\\\", f);
        else if (*p == '\n') ::fputs("\\n", f);
        else if (*p == '\r') ::fputs("\\r", f);
        else ::fputc(*p, f);
    }
}

// PR #1 review (11th round): what a member path is allowed to be. Every path this library
// writes is a tree-relative path produced by the walker -- `rel` from fs_walk_tree, which never
// begins with '/' and never contains a `..` component. A manifest that says otherwise is not one
// we wrote, and the replay resolves these names against the clone's root: `../../x` is a name
// outside the tree that link(2) and rename(2) would then act on, overwriting somebody else's
// file with the group's canonical inode. Nothing checked this before.
//
// PR #1 review (14th round): and every component must be a NAME. Refusing `..` and a leading
// '/' still left a file two ways to be spelled -- `./d/a` and `d//a` are the same file as `d/a`
// to every syscall and three different strings to every check the reader makes. That is the
// 11th round's repeat in a spelling its uniqueness pass cannot see: (d/a, ./d/a) is two
// members, both unique, of a group whose nlink really is 2, and the 13th round's tree check
// lstats both of them onto one inode with nlink 2 -- because they ARE one name. The replay then
// finds the second member already on the canonical inode, counts it linked and stops, and the
// real `d/b` is left an independent file in a fork that reported success.
//
// So: no empty component (`a//b`, a trailing '/', and the leading '/' that was already
// refused), and no component that is `.` or `..`. The writer cannot produce one -- `rel` is
// built by joining readdir names, and neither readdir(3) (which skips them explicitly) nor
// getattrlistbulk(2) (which never returns them) yields `.`, `..` or an empty name -- so no
// manifest this library has ever written becomes unreadable by this.
bool manifest_path_sane(const char *p) {
    if (!p || !*p) return false;
    for (const char *s = p;;) {
        const char *n = ::strchr(s, '/');
        size_t len = n ? (size_t)(n - s) : ::strlen(s);
        if (len == 0) return false;                                    // "", "a//b", "a/", "/a"
        if (s[0] == '.' && (len == 1 || (len == 2 && s[1] == '.'))) return false;   // "." / ".."
        if (!n) return true;
        s = n + 1;
    }
}

// The manifest's own escaping, in reverse (world.cpp's reader does the same thing). `\\r` is new
// in the 12th round and costs nothing in compatibility: an older manifest cannot contain the
// two-byte sequence backslash-r, because a literal backslash was always written `\\\\` and is
// consumed as a pair here before its `r` is ever looked at.
void unescape(char *s) {
    char *w = s;
    for (char *r = s; *r; ++r) {
        if (*r == '\\' && r[1]) {
            ++r;
            *w++ = (*r == 'n') ? '\n' : (*r == 'r') ? '\r' : *r;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

} // namespace

void hardlinks_manifest_write(FILE *f, const HardlinkSet &set) {
    if (!f) return;
    if (set.groups.empty() && !set.external_groups) return;
    ::fprintf(f, "#hl 1 %llu %llu %llu %llu\n", (unsigned long long)set.groups.size(),
              (unsigned long long)set.names, (unsigned long long)set.external_groups,
              (unsigned long long)set.external_names);
    for (size_t g = 0; g < set.groups.size(); ++g) {
        const HardlinkGroup &grp = set.groups[g];
        for (size_t i = 0; i < grp.paths.size(); ++i) {
            ::fprintf(f, "hl %llu %llu ", (unsigned long long)g, (unsigned long long)grp.nlink);
            put_escaped(f, grp.paths[i].c_str());
            ::fputc('\n', f);
        }
    }
}

int hardlinks_manifest_read(const char *manifest_path, HardlinkSet &out) {
    out = HardlinkSet();
    FILE *f = ::fopen(manifest_path, "r");
    if (!f) return -errno;
    char line[8192];
    uint64_t cur = 0;
    bool have_cur = false;
    bool bad_path = false;
    while (::fgets(line, sizeof line, f)) {
        // Entry lines are the overwhelming majority and are of no interest here; rejecting them
        // on three bytes keeps this a memcpy-speed pass over the file.
        if (line[0] != 'h' || line[1] != 'l' || line[2] != ' ') {
            if (line[0] == '#' && line[1] == 'h' && line[2] == 'l') {
                unsigned long long gs = 0, ns = 0, eg = 0, en = 0, ver = 0;
                if (::sscanf(line + 3, " %llu %llu %llu %llu %llu", &ver, &gs, &ns, &eg, &en) == 5) {
                    out.external_groups = eg;
                    out.external_names = en;
                    // PR #1 review (8th round): the two counts that say how much of this section
                    // there is supposed to be. They used to be parsed and dropped.
                    out.header_groups = gs;
                    out.header_names = ns;
                    out.header_seen = true;
                }
            }
            continue;
        }
        // PR #1 review (12th round): the terminator, and only the terminator. This used to eat a
        // trailing CR as well, so a name that ends in one came back a byte short -- `a\r` read as
        // `a` -- and the replay linked whatever `a` happened to be. (A manifest written before
        // this round with a CR in a name was already unreadable in exactly that way; there is no
        // compatibility to keep, because there was never a correct reading of it.)
        size_t n = ::strlen(line);
        if (n && line[n - 1] == '\n') line[--n] = 0;
        unsigned long long gid = 0, nlink = 0;
        int consumed = 0;
        // PR #1 review (19th round): `%llu %n`, not `%llu %n` with a space. A whitespace
        // directive in scanf(3) consumes every space, tab, CR and LF it can reach, and the byte
        // after the nlink is the ONE separator the writer put there -- `fprintf(f, "hl %llu
        // %llu ", ...)` above, one space, and then the name verbatim through put_escaped. So a
        // member whose name begins with a space or a tab (an ordinary file name, and one a
        // download or an editor produces) had that byte -- and every further one -- swallowed
        // by the separator: ` a` came back as `a`, somebody else's file or nobody's. The group
        // check then lstats the misread names, finds other inodes (or none), and the snapshot
        // is WFS_E_SNAPSHOT_DIRTY from the moment it is published: no fork of it, and no pool
        // fill of it, ever again. Read the separator as what it is -- exactly one ' ' -- and
        // take the rest of the line as the name. Nothing about the format changes, so every
        // manifest already on disk reads exactly as it did.
        if (::sscanf(line + 2, " %llu %llu%n", &gid, &nlink, &consumed) != 2 || !consumed) continue;
        char *sep = line + 2 + consumed;
        if (*sep != ' ') continue;
        char *rel = sep + 1;
        if (!*rel) continue;
        unescape(rel);
        if (!manifest_path_sane(rel)) { bad_path = true; break; }
        if (!have_cur || gid != cur || out.groups.empty()) {
            HardlinkGroup g;
            g.nlink = nlink;
            out.groups.emplace_back(std::move(g));
            cur = gid;
            have_cur = true;
        }
        out.groups[out.groups.size() - 1].paths.emplace_back(rel);
        out.names++;
    }
    ::fclose(f);
    if (bad_path) return -EINVAL;
    // PR #1 review (8th round): does the section match the header that describes it? A manifest
    // cut short -- a truncated write, a full disk, a store somebody has been editing -- keeps
    // the group count and loses a name, and a group of one name is replayed as nothing at all.
    // Everything a caller does with this set assumes it is the set the snapshot was published
    // with, so anything else is a refusal here rather than a quiet half-replay there.
    if (!out.header_seen) return (out.groups.size() || out.names) ? -EINVAL : 0;
    if ((uint64_t)out.groups.size() != out.header_groups || out.names != out.header_names)
        return -EINVAL;
    // PR #1 review (9th round): and every group must be as big as it says it is. "At least two
    // names" was too weak by exactly one damage shape: a member re-tagged into the neighbouring
    // group keeps the group count and the name count intact, so the header agrees, and the
    // replay then links a name from one inode's group onto another group's canonical file --
    // cloned content overwritten, in a fork that reported success. `nlink` is the authority: the
    // scan puts a group in `groups` only when every one of the inode's links was found inside
    // the tree (k == nlink), and a group that reaches outside is counted in the header's
    // external totals and never written as `hl` lines at all. So in a manifest this library
    // wrote, a group's member count IS its nlink, and anything else is damage.
    for (size_t i = 0; i < out.groups.size(); ++i) {
        const HardlinkGroup &g = out.groups[i];
        if (g.paths.size() < 2) return -EINVAL;
        if ((uint64_t)g.paths.size() != g.nlink) return -EINVAL;
    }
    // PR #1 review (11th round): and no name may appear twice in the whole section. This is the
    // last damage shape that keeps every count the checks above look at: repeat a member inside
    // its own group, or paste one group's member over another group's, and the group count, the
    // name count and every group's size are all still exactly what the header says. What the
    // replay then does with the repeat is nothing -- the name is already on the canonical inode,
    // so it is counted as linked -- and the member that was displaced to make room is simply not
    // in the manifest any more: the fork comes back rc 0 with an independent inode where the
    // snapshot has a link. Across two groups it is worse: the member pasted in is replayed
    // against the *other* group's canonical file, and a clone's content is overwritten.
    // The scan can never write a repeat (a name is one dirent and is recorded once), so this is
    // damage by construction. Sort the names and look at the neighbours -- one qsort of
    // `out.names` pointers, on a set that is empty in almost every tree.
    {
        Vec<const char *> all;
        for (size_t i = 0; i < out.groups.size(); ++i)
            for (size_t k = 0; k < out.groups[i].paths.size(); ++k)
                all.emplace_back(out.groups[i].paths[k].c_str());
        if (all.size() > 1) {
            ::qsort(all.data(), all.size(), sizeof(const char *), str_cmp);
            for (size_t i = 1; i < all.size(); ++i)
                if (!::strcmp(all[i - 1], all[i])) return -EINVAL;
        }
    }
    return 0;
}

// ---- the replay ----------------------------------------------------------------------------

namespace {

// ---- the temporary name -----------------------------------------------------------------------
//
// PR #1 review (P1): the temporary link used to be `<target>.wfs-tmp`, on the assumption that no
// user tree contains that name -- and the EEXIST branch then *unlinked* whatever was there. A
// snapshot is somebody's workspace: a file called `b.wfs-tmp` next to a hardlinked `b` is an
// ordinary file, and the replay threw it away. Nothing outside the store may be assumed to be
// ours, so the temporary is drawn instead from a name nothing else can be holding:
//
//     .wfs-hl-<pid>-<counter>-<16 hex from getentropy(2)>
//
// in the same directory as the target (rename(2) is only atomic within one directory). link(2)
// is an exclusive create -- it returns EEXIST rather than overwriting -- so the successful link
// *is* the claim, exactly as O_EXCL is for open(2). On EEXIST a fresh name is drawn and the
// claim tried again; nothing that is not ours is ever unlinked, at any point.
const int kTmpTries = 8;

void hl_tmp_leaf(char *out, size_t cap) {
    static uint64_t seq = 0;
    uint64_t n = __atomic_fetch_add(&seq, 1, __ATOMIC_RELAXED);
    unsigned char raw[8];
    if (::getentropy(raw, sizeof raw) != 0)
        for (size_t i = 0; i < sizeof raw; ++i)
            raw[i] = (unsigned char)(::getpid() + i * 31 + (int)n);
    uint64_t r = 0;
    for (size_t i = 0; i < sizeof raw; ++i) r = (r << 8) | raw[i];
    ::snprintf(out, cap, ".wfs-hl-%d-%llu-%016llx", (int)::getpid(), (unsigned long long)n,
               (unsigned long long)r);
}

// Hardlink the canonical file into `dirfd` -- the target's own directory, because rename(2) is
// only atomic within one -- under a name of ours. On success `tmp` is that name and the link
// exists; on failure `errno` says why and nothing was created.
bool link_at_fresh_name(int canon_fd, const char *canon_leaf, int dirfd, String &tmp) {
    for (int t = 0; t < kTmpTries; ++t) {
        char leaf[64];
        hl_tmp_leaf(leaf, sizeof leaf);
        tmp.assign(leaf);
        // Flag 0, not AT_SYMLINK_FOLLOW: the canonical name has been fstatat'ed as a regular
        // file through this very descriptor and is linked as itself.
        if (::linkat(canon_fd, canon_leaf, dirfd, tmp.c_str(), 0) == 0) return true;
        if (errno != EEXIST) return false;
    }
    errno = EEXIST;
    return false;
}

// link(2) + rename(2), both relative to descriptors the descent opened. `*err` is the negative
// errno of whichever call failed when this returns false; nothing of ours is left behind.
bool relink_at(int canon_fd, const char *canon_leaf, int dirfd, const char *leaf, int *err) {
    String tmp;
    if (!link_at_fresh_name(canon_fd, canon_leaf, dirfd, tmp)) { *err = -errno; return false; }
    // rename(2), not unlink+link: the name never stops existing, so a crash or an error here
    // cannot lose the file.
    if (::renameat(dirfd, tmp.c_str(), dirfd, leaf) != 0) {
        *err = -errno;
        ::unlinkat(dirfd, tmp.c_str(), 0);
        return false;
    }
    return true;
}

// ---- directories the source made read-only ----------------------------------------------------
//
// PR #1 review (4th round, P2): a hardlink group can live under a directory whose owner-write
// bit the source does not set -- 0555 is an ordinary mode for a vendored tree, a generated
// fixture, a `chmod -R a-w` release directory. The clone wears that mode too, so link(2) and
// rename(2) inside it come back EACCES, the group could not be rebuilt, and the snapshot was
// published anyway: a tree whose manifest and database row advertise a group it does not have,
// inherited by every fork and every pool entry made from it.
//
// So the replay lends the directory owner write (and search) for exactly the two syscalls and
// puts it back the way it was -- the exact mode, and the UF_IMMUTABLE/UF_APPEND flags if it had
// any. The lends are recorded on a stack and undone in reverse on every way out, errors
// included, by the destructor. Two groups can live in the same directory and the replay is four
// threads wide, so the whole lend/link/rename/restore sequence is serialized on one mutex: it
// costs nothing in the common case, where it never runs at all.
//
// PR #1 review (18th round, P2): and the same question about the FILE. UF_IMMUTABLE and
// UF_APPEND are USER flags -- `chflags uchg` on a vendored tree, a release directory, a fixture
// somebody froze -- and clonefile(2) copies them onto every name of the clone. link(2) refuses
// an immutable or append-only SOURCE with EPERM and rename(2) refuses to replace an immutable
// TARGET with EPERM, so neither half of relink_at() could run and the lend above, which unlocks
// the directory and nothing else, did not help: a valid source tree came back -EPERM out of
// `snapshot create`, `pool fill` and every fork, because a replay error is fatal (4th round).
// So the two inodes those two calls touch are lent as well -- the canonical file, and the name
// the rename replaces -- through descriptors opened from the descent's own directory fd with
// O_NOFOLLOW, which is the 17th round's rule about where a member's name may lead. Opening an
// immutable file O_RDONLY is allowed, and clearing a UF_ flag on it is the owner's to do.
// (`--hard` snapshots are a different matter: a fork unprotects the whole clone before the
// replay runs. This is about flags the SOURCE tree carries.)
struct Lend {
    int fd = -1;        // a directory's is borrowed; a file's is opened by lend_file and owned
    bool owned = false;
    bool had_mode = false;
    mode_t mode = 0;
    uint32_t flags = 0;
    bool had_flags = false;
};

class LendStack {
  public:
    LendStack() = default;
    LendStack(const LendStack &) = delete;
    LendStack &operator=(const LendStack &) = delete;
    ~LendStack() {
        // In reverse, and unconditionally: this runs on the success path and on every error
        // path, so nothing is ever left more permissive than we found it.
        for (size_t i = v_.size(); i-- > 0;) {
            if (v_[i].had_mode) ::fchmod(v_[i].fd, v_[i].mode);
#ifdef __APPLE__
            if (v_[i].had_flags) ::fchflags(v_[i].fd, (u_int32_t)v_[i].flags);
#endif
            if (v_[i].owned) ::close(v_[i].fd);
        }
    }
    // Gives the directory behind `dirfd` owner write + search. False means it could not be
    // asked or could not be changed at all, and the caller fails exactly as it did before.
    // The descriptor is the one the descent opened (17th round), so the directory being lent is
    // the very directory the link and the rename below will go into -- not a path resolved a
    // second time.
    bool lend(int dirfd) {
        struct stat st;
        if (::fstat(dirfd, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
        Lend u;
        u.fd = dirfd;
        u.had_mode = true;
        u.mode = (mode_t)(st.st_mode & 07777);
#ifdef __APPLE__
        u.flags = (uint32_t)st.st_flags;
        u.had_flags = (st.st_flags & (UF_IMMUTABLE | UF_APPEND)) != 0;
        if (u.had_flags &&
            ::fchflags(dirfd, (u_int32_t)(st.st_flags & ~(uint32_t)(UF_IMMUTABLE | UF_APPEND))) != 0)
            return false;
#endif
        mode_t want = (mode_t)(u.mode | S_IWUSR | S_IXUSR);
        if (want != u.mode && ::fchmod(dirfd, want) != 0) {
#ifdef __APPLE__
            if (u.had_flags) ::fchflags(dirfd, (u_int32_t)u.flags);
#endif
            return false;
        }
        v_.emplace_back(std::move(u));   // recorded only once it really changed
        return true;
    }

    // Takes UF_IMMUTABLE/UF_APPEND off the file `leaf` names inside `dirfd`. False means the
    // file could not be opened or its flags could not be changed, and the caller fails exactly
    // as it did before; a file that carries neither flag is true and is not recorded at all,
    // because there is nothing to put back. `at` receives the entry's index, for forget().
    bool lend_file(int dirfd, const char *leaf, size_t *at = nullptr) {
        if (at) *at = (size_t)-1;
#ifdef __APPLE__
        // O_NOFOLLOW: the leaf of a member path is not a symlink either (17th round), and this
        // is the same directory descriptor the link and the rename below use.
        int fd = ::openat(dirfd, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return false;
        struct stat st;
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { ::close(fd); return false; }
        if ((st.st_flags & (UF_IMMUTABLE | UF_APPEND)) == 0) { ::close(fd); return true; }
        if (::fchflags(fd, (u_int32_t)(st.st_flags & ~(uint32_t)(UF_IMMUTABLE | UF_APPEND))) != 0) {
            ::close(fd);
            return false;
        }
        Lend u;
        u.fd = fd;
        u.owned = true;
        u.flags = (uint32_t)st.st_flags;
        u.had_flags = true;
        if (at) *at = v_.size();
        v_.emplace_back(std::move(u));
        return true;
#else
        (void)dirfd;
        (void)leaf;
        return true;
#endif
    }

    // "That inode has no name any more": the descriptor is still closed, but its flags are not
    // put back, because there is nothing left to wear them.
    void forget(size_t at) {
        if (at < v_.size()) v_[at].had_flags = false;
    }

  private:
    Vec<Lend> v_;
};

// The serialization point for every lend in one replay. One per hardlinks_restore() call, on
// its stack: nothing here is global state.
struct Relender {
    Mutex mu;
};

// relink_at() again, with the target's own directory lent owner write for its duration -- and
// (18th round) with the two files it touches lent the flags they carry. The canonical file is
// lent once per name linked rather than once per group, which ends in the same place: after the
// link the new name IS the canonical inode, so restoring that inode's flags restores the whole
// group's, and the next name starts from the file exactly as the source left it.
bool relink_under_lend(Relender &rl, int canon_fd, const char *canon_leaf, int dirfd,
                       const char *leaf, int *err) {
    Guard lk(rl.mu);
    LendStack lend;
    if (!lend.lend(dirfd)) return false;
    // Neither of these is a reason to give up on its own: a file with no flags on it says true
    // without being recorded, and one that will not open or will not change simply leaves
    // relink_at to fail the way it already did, with the file system's own errno.
    lend.lend_file(canon_fd, canon_leaf);
    size_t victim = (size_t)-1;
    lend.lend_file(dirfd, leaf, &victim);
    bool ok = relink_at(canon_fd, canon_leaf, dirfd, leaf, err);
    // The rename unlinked the file `leaf` used to name; only its inode is left, held open by
    // this lend alone, and an unlinked inode has no flags worth restoring.
    if (ok) lend.forget(victim);
    return ok;
}

// Every name of the group still exists in the live tree and still shares one inode. Used when
// the groups come from a snapshot's manifest but the tree being cloned is a world that has been
// written to since -- the group may have been broken there long ago.
bool group_still_linked(const char *verify_root, const HardlinkGroup &g) {
    int rootfd = open_tree_root(verify_root);
    if (rootfd < 0) return false;
    bool ok = true;
    struct stat first;
    // PR #1 review (17th round, P1): the descent, here too. This is the only check a fork from
    // a live world makes -- hardlinks_verify_groups does not run on that path, because a live
    // tree is allowed to have moved on -- so a member behind a symlink was approved against
    // whatever the symlink leads to in the WORLD and then replayed against whatever it leads to
    // beside the fork's temporary, which is a different directory entirely.
    if (member_lstat(rootfd, g.paths[0].c_str(), first) != 0 || !S_ISREG(first.st_mode)) ok = false;
    else if ((uint64_t)first.st_nlink < (uint64_t)g.paths.size()) ok = false;
    for (size_t i = 1; ok && i < g.paths.size(); ++i) {
        struct stat st;
        if (member_lstat(rootfd, g.paths[i].c_str(), st) != 0) ok = false;
        else if (st.st_dev != first.st_dev || st.st_ino != first.st_ino) ok = false;
    }
    ::close(rootfd);
    return ok;
}

// One group. Everything here is the same handful of syscalls whichever thread runs it, and no
// two groups ever touch the same name, so the workers below need no coordination at all.
void restore_group(const char *tree_root, const char *verify_root, const HardlinkGroup &g,
                   size_t index, HardlinkRestore &r, Relender &rl) {
    if (g.paths.size() < 2) return;
    if (verify_root && !group_still_linked(verify_root, g)) {
        r.skipped++;
        r.broken.emplace_back((uint64_t)index);
        return;
    }
    // Every name that is in the clone has to end up on the canonical inode, or the group is not
    // the group any more and whoever writes it down has to know (`broken` above).
    bool whole = true;

    // PR #1 review (17th round, P1): every name below is reached by descending from this
    // descriptor, one component at a time and never through a symlink (open_parent). A tree
    // root that cannot be opened at all is what an absent tree has always been here: every name
    // is missing and the group is not the group the set describes.
    int rootfd = open_tree_root(tree_root);
    if (rootfd < 0) {
        r.missing += (uint64_t)g.paths.size();
        r.broken.emplace_back((uint64_t)index);
        return;
    }

    // The canonical file is the first name that is actually there. Promoting the next one
    // matters for a checkpoint of a live world: the first name may have been deleted between
    // the scan and the clone, and the rest of the group is still perfectly restorable -- but
    // the group the manifest describes is not the group the tree then has.
    //
    // PR #1 review (14th round, P2): which is why a missing member is `whole = false`, here and
    // in the loop below. It used to be counted and forgiven, so wfs_snapshot_create kept a
    // group in the manifest and in hl_groups whose member its own tree does not contain, and
    // every later verify and every fork lstat'ed that member and refused the snapshot as dirty
    // -- for ever, on a snapshot that was never damaged. The round-5 rule is that a snapshot
    // describes the tree it actually has, so the group is dropped instead. (The 14th round's
    // other half keeps the marker out of a group in the first place; this half is what covers
    // anything else snapshot creation takes out of the clone, and any member that vanishes in
    // the window between the scan and the clone of a live source. A fork and a pool fill do not
    // read `broken` at all -- their clone is published as it is -- so nothing else changes.)
    size_t base = g.paths.size();
    struct stat bst;
    for (size_t i = 0; i < g.paths.size(); ++i) {
        int e = member_lstat(rootfd, g.paths[i].c_str(), bst);
        if (e == 0 && S_ISREG(bst.st_mode)) { base = i; break; }
        // A name that is not there is `missing`, as it always was; a name that is not a chain
        // of real directory names is one this replay declines to touch, which is `skipped`.
        if (e == 0 || e == -ENOENT) r.missing++;
        else r.skipped++;
        whole = false;
    }
    if (base == g.paths.size()) {
        ::close(rootfd);
        r.broken.emplace_back((uint64_t)index);
        return;
    }
    {
        String canon_leaf;
        int canon_fd = open_parent(rootfd, g.paths[base].c_str(), canon_leaf);
        if (canon_fd < 0) {
            ::close(rootfd);
            r.skipped++;
            r.broken.emplace_back((uint64_t)index);
            return;
        }
        bool linked = false;
        for (size_t i = base + 1; i < g.paths.size(); ++i) {
            String leaf;
            int pfd = open_parent(rootfd, g.paths[i].c_str(), leaf);
            if (pfd < 0) {
                if (pfd == -ENOENT) r.missing++;
                else r.skipped++;
                whole = false;
                continue;
            }
            struct stat st;
            if (::fstatat(pfd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
                r.missing++;
                whole = false;
                ::close(pfd);
                continue;
            }
            if (st.st_dev == bst.st_dev && st.st_ino == bst.st_ino) {
                linked = true;
                ::close(pfd);
                continue;
            }
            // Not the file the scan saw any more: leave it alone. Unlinking it would throw away
            // somebody's data to save a few blocks.
            if (!S_ISREG(st.st_mode) || st.st_size != bst.st_size || mtime_ns(st) != mtime_ns(bst)) {
                r.skipped++;
                whole = false;
                ::close(pfd);
                continue;
            }
            int e = 0;
            bool ok = relink_at(canon_fd, canon_leaf.c_str(), pfd, leaf.c_str(), &e);
            // The directory refused us, not the file: it is a read-only directory of the
            // source's own making. Lend it owner write for the two calls and put it back.
            if (!ok && (e == -EACCES || e == -EPERM))
                ok = relink_under_lend(rl, canon_fd, canon_leaf.c_str(), pfd, leaf.c_str(), &e);
            ::close(pfd);
            if (!ok) {
                if (!r.first_err) r.first_err = e;
                r.skipped++;
                whole = false;
                continue;
            }
            r.links++;
            linked = true;
        }
        ::close(canon_fd);
        if (linked) r.groups++;
    }
    ::close(rootfd);
    if (!whole) r.broken.emplace_back((uint64_t)index);
}

// The whole job is metadata transactions (link + rename measure 0.6 ms a pair on 27.0), and
// APFS takes those about 2.5x faster from four threads -- the same sweet spot the walkers and
// the collector use (docs/CLONE_MODEL_MACOS27.md §9, P16). Below a handful of groups the
// threads cost more than they save.
const size_t kParallelFrom = 8;
const int kThreads = 4;

struct RestoreJob {
    const HardlinkSet *set;
    const char *tree_root;
    const char *verify_root;
    uint64_t next;      // the next group to take, bumped atomically
    Mutex mu;           // the merge of a worker's counters into `agg`
    HardlinkRestore agg;
    Relender rl;        // the read-only-directory path, shared by all four workers
};

void *restore_worker(void *arg) {
    RestoreJob *j = (RestoreJob *)arg;
    HardlinkRestore local;
    size_t n = j->set->groups.size();
    for (;;) {
        uint64_t i = __atomic_fetch_add(&j->next, 1, __ATOMIC_RELAXED);
        if (i >= n) break;
        restore_group(j->tree_root, j->verify_root, j->set->groups[(size_t)i], (size_t)i, local, j->rl);
    }
    Guard g(j->mu);
    j->agg.groups += local.groups;
    j->agg.links += local.links;
    j->agg.missing += local.missing;
    j->agg.skipped += local.skipped;
    for (size_t k = 0; k < local.broken.size(); ++k) j->agg.broken.emplace_back(local.broken[k]);
    if (!j->agg.first_err) j->agg.first_err = local.first_err;
    return nullptr;
}

// PR #1 review (4th round, P2): what a replay failure is worth. `missing` and `skipped` stay
// tolerated -- they are a live source changing under the clone, and the header has always said
// so. `first_err` is different: it is a link(2) or rename(2) that the file system refused, and
// the tree the caller is about to publish would then disagree with the manifest and the database
// row that describe it. That is not a thriftiness question, so it is returned, and snapshot
// creation / a fork / a pool fill unwind on it.
int replay_verdict(const HardlinkRestore &r) {
    if (wfs_test_hardlink_restore_err) return wfs_test_hardlink_restore_err;
    return r.first_err;
}

} // namespace

// See hardlinks.h. The manifest checked against the tree it was written from, which is the only
// thing that can catch a manifest whose members were exchanged between two groups: every count
// the reader looks at survives that, and so does the replay's "same size, same mtime" guard.
int hardlinks_verify_groups(const char *tree_root, const HardlinkSet &set) {
    if (!tree_root || !*tree_root) return -EINVAL;
    if (set.groups.empty()) return 0;
    // PR #1 review (17th round, P1): the lstats are asked of descriptors this function opens
    // itself, one path component at a time and never through a symlink. lstat(2) leaves the
    // last component alone and follows every one before it, so `s/x` with `s` a symlink was
    // answered by whatever the symlink leads to NEXT TO THE SNAPSHOT -- a hardlinked pair a
    // manifest's author can plant there -- while the replay, resolving the same relative
    // symlink from a clone that sits somewhere else entirely, linked and renamed over two files
    // outside the clone. A member whose parent is not a real directory is now -EINVAL here,
    // which is WFS_E_SNAPSHOT_DIRTY to every caller, before anything is cloned or linked.
    int rootfd = open_tree_root(tree_root);
    if (rootfd < 0) return -EINVAL;
    int rc = 0;
    for (size_t i = 0; i < set.groups.size() && !rc; ++i) {
        const HardlinkGroup &g = set.groups[i];
        // Both of these are already refusals in hardlinks_manifest_read; repeated because this
        // function's promise is about the set it was handed, not about where it came from.
        if (g.paths.size() < 2 || (uint64_t)g.paths.size() != g.nlink) { rc = -EINVAL; break; }
        struct stat first;
        if (member_lstat(rootfd, g.paths[0].c_str(), first) != 0 || !S_ISREG(first.st_mode)) {
            rc = -EINVAL;
            break;
        }
        // Exactly, not "at least": a snapshot tree is published with the group's names -- and
        // only those -- on that inode, so a bigger nlink means the tree is not the tree this
        // manifest describes any more. (The live-source check in group_still_linked() is the
        // one that settles for >=, because a live tree may have grown a link of its own.)
        if ((uint64_t)first.st_nlink != (uint64_t)g.paths.size()) { rc = -EINVAL; break; }
        for (size_t k = 1; k < g.paths.size(); ++k) {
            struct stat st;
            if (member_lstat(rootfd, g.paths[k].c_str(), st) != 0) { rc = -EINVAL; break; }
            if (st.st_dev != first.st_dev || st.st_ino != first.st_ino) { rc = -EINVAL; break; }
        }
    }
    ::close(rootfd);
    return rc;
}

int hardlinks_restore(const char *tree_root, const HardlinkSet &set, const char *verify_root,
                      HardlinkRestore *out) {
    if (!tree_root || !*tree_root) return -EINVAL;
    if (set.groups.size() < kParallelFrom) {
        HardlinkRestore r;
        Relender rl;
        for (size_t i = 0; i < set.groups.size(); ++i)
            restore_group(tree_root, verify_root, set.groups[i], i, r, rl);
        if (out) *out = r;
        return replay_verdict(r);
    }
    RestoreJob j;
    j.set = &set;
    j.tree_root = tree_root;
    j.verify_root = verify_root;
    j.next = 0;
    pthread_t th[kThreads];
    int started = threads_start(th, kThreads, restore_worker, &j);
    if (started == 0) restore_worker(&j);   // no threads to be had: do it here
    for (int i = 0; i < started; ++i) ::pthread_join(th[i], nullptr);
    if (out) *out = j.agg;
    return replay_verdict(j.agg);
}

} // namespace wfs
