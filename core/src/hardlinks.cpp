// See hardlinks.h: collecting the hardlink groups of a tree, carrying them in the manifest, and
// replaying them onto a clone (P9, T2.5).
#include "hardlinks.h"

#include <errno.h>
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

String joinp(const char *a, const char *b) {
    String p(a);
    size_t n = p.size();
    if (n && p.c_str()[n - 1] != '/') p.append("/");
    p.append(b);
    return p;
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

struct ScanCtx {
    TreeStats *stats;
    const char *exclude;   // the one tree-relative name that is not a file of this tree
    Vec<Rec> recs;
    Vec<String> names;
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
            // What stays behind is its twin, and that twin's inode now carries a name the
            // snapshot does not have -- which is what the external counters have always been
            // for, and the grouping below reaches that verdict by itself: fewer names found
            // inside the tree than the inode's nlink.
            if (st.st_nlink > 1 && !excluded) bump(c->stats->hardlinks);
        }
    }
    if (excluded) return 0;
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
        // Fewer names here than the inode has links: the rest are outside this tree, and a
        // clone cannot be given links to files it does not contain. Count and move on.
        if (k != (size_t)c.recs[i].nlink || k < 2) {
            out.external_groups++;
            out.external_names += (uint64_t)k;
            i = j;
            continue;
        }
        Vec<const char *> ptr;
        for (size_t t = i; t < j; ++t) ptr.emplace_back(c.names[c.recs[t].idx].c_str());
        if (ptr.size() > 1) ::qsort(ptr.data(), ptr.size(), sizeof(const char *), str_cmp);
        HardlinkGroup g;
        g.nlink = c.recs[i].nlink;
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
        if (::sscanf(line + 2, " %llu %llu %n", &gid, &nlink, &consumed) != 2 || !consumed) continue;
        char *rel = line + 2 + consumed;
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

// `target`'s directory, trailing '/' included. `target` is always joinp(tree_root, rel) and
// tree_root is absolute, so there is always a '/' to find.
void dir_prefix(const String &target, String &out) {
    const char *s = target.c_str();
    const char *slash = ::strrchr(s, '/');
    if (!slash) { out.assign("./"); return; }
    out.assign(s, (size_t)(slash - s) + 1);
}

// Hardlink `canon` into the directory of `target` under a name of ours. On success `tmp` is
// that name and the link exists; on failure `errno` says why and nothing was created.
bool link_under_fresh_name(const char *canon, const String &target, String &tmp) {
    String dir;
    dir_prefix(target, dir);
    for (int t = 0; t < kTmpTries; ++t) {
        char leaf[64];
        hl_tmp_leaf(leaf, sizeof leaf);
        tmp.assign(dir);
        tmp.append(leaf);
        if (::link(canon, tmp.c_str()) == 0) return true;
        if (errno != EEXIST) return false;
    }
    errno = EEXIST;
    return false;
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
struct DirLend {
    String path;
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
        // path, so a directory is never left more permissive than we found it.
        for (size_t i = v_.size(); i-- > 0;) {
            ::chmod(v_[i].path.c_str(), v_[i].mode);
#ifdef __APPLE__
            if (v_[i].had_flags) ::lchflags(v_[i].path.c_str(), (u_int32_t)v_[i].flags);
#endif
        }
    }
    // Gives `dir` owner write + search. False means the directory could not be read or could not
    // be changed at all, and the caller fails exactly as it did before.
    bool lend(const char *dir) {
        struct stat st;
        if (::lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
        DirLend u;
        u.path.assign(dir);
        u.mode = (mode_t)(st.st_mode & 07777);
#ifdef __APPLE__
        u.flags = (uint32_t)st.st_flags;
        u.had_flags = (st.st_flags & (UF_IMMUTABLE | UF_APPEND)) != 0;
        if (u.had_flags &&
            ::lchflags(dir, (u_int32_t)(st.st_flags & ~(uint32_t)(UF_IMMUTABLE | UF_APPEND))) != 0)
            return false;
#endif
        mode_t want = (mode_t)(u.mode | S_IWUSR | S_IXUSR);
        if (want != u.mode && ::chmod(dir, want) != 0) {
#ifdef __APPLE__
            if (u.had_flags) ::lchflags(dir, (u_int32_t)u.flags);
#endif
            return false;
        }
        v_.emplace_back(std::move(u));   // recorded only once it really changed
        return true;
    }

  private:
    Vec<DirLend> v_;
};

// The serialization point for every lend in one replay. One per hardlinks_restore() call, on
// its stack: nothing here is global state.
struct Relender {
    Mutex mu;
};

// link(2) + rename(2) again, with the target's directory lent owner write for their duration.
// `*err` is the negative errno of whichever call failed when this returns false.
bool relink_under_lend(Relender &rl, const char *canon, const String &target, int *err) {
    Guard lk(rl.mu);
    String dir;
    dir_prefix(target, dir);
    if (dir.size() > 1) dir.resize(dir.size() - 1);   // dir_prefix keeps the trailing '/'
    LendStack lend;
    if (!lend.lend(dir.c_str())) return false;
    String tmp;
    if (!link_under_fresh_name(canon, target, tmp)) { *err = -errno; return false; }
    if (::rename(tmp.c_str(), target.c_str()) != 0) {
        *err = -errno;
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

// Every name of the group still exists in the live tree and still shares one inode. Used when
// the groups come from a snapshot's manifest but the tree being cloned is a world that has been
// written to since -- the group may have been broken there long ago.
bool group_still_linked(const char *verify_root, const HardlinkGroup &g) {
    struct stat first;
    String p = joinp(verify_root, g.paths[0].c_str());
    if (::lstat(p.c_str(), &first) != 0 || !S_ISREG(first.st_mode)) return false;
    if ((uint64_t)first.st_nlink < (uint64_t)g.paths.size()) return false;
    for (size_t i = 1; i < g.paths.size(); ++i) {
        struct stat st;
        String q = joinp(verify_root, g.paths[i].c_str());
        if (::lstat(q.c_str(), &st) != 0) return false;
        if (st.st_dev != first.st_dev || st.st_ino != first.st_ino) return false;
    }
    return true;
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
        String c = joinp(tree_root, g.paths[i].c_str());
        if (::lstat(c.c_str(), &bst) == 0 && S_ISREG(bst.st_mode)) { base = i; break; }
        r.missing++;
        whole = false;
    }
    if (base == g.paths.size()) { r.broken.emplace_back((uint64_t)index); return; }
    {
        String canon = joinp(tree_root, g.paths[base].c_str());
        bool linked = false;
        for (size_t i = base + 1; i < g.paths.size(); ++i) {
            String p = joinp(tree_root, g.paths[i].c_str());
            struct stat st;
            if (::lstat(p.c_str(), &st) != 0) { r.missing++; whole = false; continue; }
            if (st.st_dev == bst.st_dev && st.st_ino == bst.st_ino) { linked = true; continue; }
            // Not the file the scan saw any more: leave it alone. Unlinking it would throw away
            // somebody's data to save a few blocks.
            if (!S_ISREG(st.st_mode) || st.st_size != bst.st_size || mtime_ns(st) != mtime_ns(bst)) {
                r.skipped++;
                whole = false;
                continue;
            }
            String tmp;
            int e = 0;
            bool ok = link_under_fresh_name(canon.c_str(), p, tmp);
            if (!ok) e = -errno;
            // rename(2), not unlink+link: the name never stops existing, so a crash or an
            // error here cannot lose the file.
            else if (::rename(tmp.c_str(), p.c_str()) != 0) {
                e = -errno;
                ::unlink(tmp.c_str());
                ok = false;
            }
            // The directory refused us, not the file: it is a read-only directory of the
            // source's own making. Lend it owner write for the two calls and put it back.
            if (!ok && (e == -EACCES || e == -EPERM))
                ok = relink_under_lend(rl, canon.c_str(), p, &e);
            if (!ok) {
                if (!r.first_err) r.first_err = e;
                r.skipped++;
                whole = false;
                continue;
            }
            r.links++;
            linked = true;
        }
        if (linked) r.groups++;
    }
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
    for (size_t i = 0; i < set.groups.size(); ++i) {
        const HardlinkGroup &g = set.groups[i];
        // Both of these are already refusals in hardlinks_manifest_read; repeated because this
        // function's promise is about the set it was handed, not about where it came from.
        if (g.paths.size() < 2 || (uint64_t)g.paths.size() != g.nlink) return -EINVAL;
        struct stat first;
        String p = joinp(tree_root, g.paths[0].c_str());
        if (::lstat(p.c_str(), &first) != 0 || !S_ISREG(first.st_mode)) return -EINVAL;
        // Exactly, not "at least": a snapshot tree is published with the group's names -- and
        // only those -- on that inode, so a bigger nlink means the tree is not the tree this
        // manifest describes any more. (The live-source check in group_still_linked() is the
        // one that settles for >=, because a live tree may have grown a link of its own.)
        if ((uint64_t)first.st_nlink != (uint64_t)g.paths.size()) return -EINVAL;
        for (size_t k = 1; k < g.paths.size(); ++k) {
            struct stat st;
            String q = joinp(tree_root, g.paths[k].c_str());
            if (::lstat(q.c_str(), &st) != 0) return -EINVAL;
            if (st.st_dev != first.st_dev || st.st_ino != first.st_ino) return -EINVAL;
        }
    }
    return 0;
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
