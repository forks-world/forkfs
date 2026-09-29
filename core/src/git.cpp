#include "git.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/random.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <algorithm>
#include <utility>   // std::move only

extern char **environ;
namespace wfs {
namespace {
String joinp(const char *a, const char *b) { String s(a); s.append("/"); s.append(b); return s; }
constexpr const char *marker = "gitdir: .world-git/repo.git/worktrees/active\n";

// The reason for the last Git refusal on this thread, for wfs_git_reason(). Every
// WFS_E_GIT_UNSUPPORTED / WFS_E_GIT_POLICY / WFS_E_GIT_IN_USE this file returns goes through
// refuse(); a discard refusal names paths, hence the room.
thread_local char g_reason[2048];
int refuse(int code, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_reason, sizeof g_reason, fmt, ap);
    va_end(ap);
    return code;
}
// Never use a shell, source hooks, inherited GIT_DIR/INDEX_FILE, or lazy network fetches.
// `ambient_config_probe` keeps the user's global/system configuration (and GIT_CONFIG_* command
// configuration) so a check sees files the way the user's own Git does; `stdin_fd`, when >= 0,
// becomes the child's standard input, and `stderr_fd`, when >= 0, its standard error.
int git(const char *cwd, const char *const *args, Vec<char> *output = nullptr, int *exit_code = nullptr, bool quiet_stderr = false, bool ambient_config_probe = false, int stdin_fd = -1, bool skip_lfs_smudge = false, bool no_system_attributes = false, int stderr_fd = -1) {
    if (exit_code) *exit_code = -1;
    Vec<char *> av;
    const char *prefix[] = {"git", "-c", "core.hooksPath=/dev/null", "-c", "core.fsmonitor=false",
        "-c", "gc.auto=0", "-c", "maintenance.auto=false", "-c", "submodule.recurse=false",
        "-c", "protocol.ext.allow=never", "-C", cwd};
    for (const char *p : prefix) av.emplace_back(const_cast<char *>(p));
    for (size_t i = 0; args[i]; ++i) av.emplace_back(const_cast<char *>(args[i]));
    av.emplace_back(nullptr);
    Vec<char *> env;
    for (char **p = environ; *p; ++p) {
        if (strncmp(*p, "GIT_", 4) || (ambient_config_probe &&
            (!strncmp(*p, "GIT_CONFIG_GLOBAL=", 18) || !strncmp(*p, "GIT_CONFIG_SYSTEM=", 18) ||
             !strncmp(*p, "GIT_CONFIG_NOSYSTEM=", 20) || !strncmp(*p, "GIT_CONFIG_COUNT=", 17) ||
             !strncmp(*p, "GIT_CONFIG_KEY_", 15) || !strncmp(*p, "GIT_CONFIG_VALUE_", 17) ||
             !strncmp(*p, "GIT_CONFIG_PARAMETERS=", 22))) &&
            !(no_system_attributes && !strncmp(*p, "GIT_ATTR_NOSYSTEM=", 18))) env.emplace_back(*p);
    }
    const char *settings[] = {"GIT_OPTIONAL_LOCKS=0", "GIT_TERMINAL_PROMPT=0", "GIT_NO_LAZY_FETCH=1"};
    for (const char *p : settings) env.emplace_back(const_cast<char *>(p));
    if (skip_lfs_smudge) env.emplace_back(const_cast<char *>("GIT_LFS_SKIP_SMUDGE=1"));
    if (no_system_attributes) env.emplace_back(const_cast<char *>("GIT_ATTR_NOSYSTEM=1"));
    if (!ambient_config_probe) {
        env.emplace_back(const_cast<char *>("GIT_CONFIG_NOSYSTEM=1"));
        env.emplace_back(const_cast<char *>("GIT_CONFIG_GLOBAL=/dev/null"));
    }
    env.emplace_back(nullptr);
    int pipefd[2];
    if (pipe(pipefd)) return -errno;
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC); fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc) { close(pipefd[0]); close(pipefd[1]); return -rc; }
    rc = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    if (!rc && stdin_fd >= 0) rc = posix_spawn_file_actions_adddup2(&actions, stdin_fd, STDIN_FILENO);
    if (!rc && stderr_fd >= 0) rc = posix_spawn_file_actions_adddup2(&actions, stderr_fd, STDERR_FILENO);
    else if (!rc && quiet_stderr)
        rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    if (!rc) rc = posix_spawnp(&pid, "git", &actions, nullptr, av.data(), env.data());
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (rc) { close(pipefd[0]); return rc == ENOENT ? WFS_E_GIT_FAILED : -rc; }
    if (output) output->clear();
    char buf[8192];
    int err = 0;
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { err = -errno; break; }
        if (!n) break;
        if (output && !err) {
            if (output->size() + (size_t)n > 32 * 1024 * 1024) err = -EOVERFLOW;
            else for (ssize_t i = 0; i < n; ++i) output->emplace_back(buf[i]);
        }
    }
    close(pipefd[0]);
    int status;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -errno;
    if (exit_code && WIFEXITED(status)) *exit_code = WEXITSTATUS(status);
    if (output) output->emplace_back('\0');
    if (err) return err;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : WFS_E_GIT_FAILED;
}
int value(const char *root, const char *const *args, String &out, bool missing_ok = false) {
    Vec<char> bytes; int status = -1;
    int rc = git(root, args, &bytes, &status);
    if (rc == WFS_E_GIT_FAILED && missing_ok && status == 1) { out.clear(); return 0; }
    if (rc) return rc;
    out.assign(bytes.data());
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return 0;
}
// A command whose progress chatter is noise on success (`worktree repair` reports each link it
// rewrites): its standard error is held back and shown only when it fails, so the diagnostic
// that explains a failure is never lost.
int git_diagnose_on_failure(const char *cwd, const char *const *args) {
    FILE *errors = tmpfile();
    if (!errors) return -errno;
    int rc = git(cwd, args, nullptr, nullptr, false, false, -1, false, false, fileno(errors));
    if (rc == WFS_E_GIT_FAILED) {
        rewind(errors);
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof buf, errors)) > 0) fwrite(buf, 1, n, stderr);
    }
    fclose(errors);
    return rc;
}
// Older Git LFS releases hash pointer payloads directly but do not accept
// --no-extensions. Newer releases need that flag to avoid running configured
// extensions. Probe once, fail closed on an unrecognized CLI, and select the
// raw-hash-compatible command form for every pointer verification.
int lfs_pointer_oracle(const char *root, const char *file_arg, Vec<char> *output = nullptr, int stdin_fd = -1) {
    static int no_extensions = -1;
    if (no_extensions < 0) {
        const char *help_args[] = {"lfs", "pointer", "-h", nullptr};
        Vec<char> help;
        if (int rc = git(root, help_args, &help)) return rc;
        if (!help.data() || !strstr(help.data(), "--file") || !strstr(help.data(), "--check"))
            return WFS_E_GIT_FAILED;
        no_extensions = strstr(help.data(), "--no-extensions") ? 1 : 0;
    }
    const char *modern[] = {"lfs", "pointer", "--no-extensions", file_arg, nullptr};
    const char *legacy[] = {"lfs", "pointer", file_arg, nullptr};
    return git(root, no_extensions ? modern : legacy, output, nullptr, false, false, stdin_fd);
}
int collect_symrefs(const char *root, Vec<GitSymref> &out) {
    const char *args[] = {"for-each-ref", "--format=%(refname)%09%(symref)", nullptr};
    Vec<char> listing;
    if (int rc = git(root, args, &listing)) return rc;
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i < listing.size(); ++i) {
        if (listing[i] != '\n' && listing[i] != '\0') continue;
        if (i == start) { start = i + 1; continue; }
        size_t tab = start;
        while (tab < i && listing[tab] != '\t') ++tab;
        if (tab == i || tab == start) return refuse(WFS_E_GIT_UNSUPPORTED, "a ref listing could not be parsed");
        if (tab + 1 == i) { start = i + 1; continue; }
        String name(listing.data() + start, tab - start), target(listing.data() + tab + 1, i - tab - 1);
        if (strncmp(name.c_str(), "refs/", 5) || strncmp(target.c_str(), "refs/", 5) ||
            strchr(target.c_str(), '\t') || strchr(target.c_str(), '\n'))
            return refuse(WFS_E_GIT_UNSUPPORTED, "symbolic ref %s points outside refs/", name.c_str());
        const char *sym_args[] = {"symbolic-ref", "--quiet", "--no-recurse", name.c_str(), nullptr};
        String immediate;
        if (int rc = value(root, sym_args, immediate)) return rc;
        if (strncmp(immediate.c_str(), "refs/", 5))
            return refuse(WFS_E_GIT_UNSUPPORTED, "symbolic ref %s points outside refs/", name.c_str());
        GitSymref sym{name, immediate};
        out.emplace_back(sym);
        start = i + 1;
    }
    return 0;
}
bool same_symrefs(const Vec<GitSymref> &a, const Vec<GitSymref> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].target != b[i].target) return false;
    return true;
}
bool same_bytes(const Vec<char> &a, const Vec<char> &b) {
    return a.size() == b.size() && (a.empty() || !memcmp(a.data(), b.data(), a.size()));
}
int read_bytes(const char *path, Vec<char> &out) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -errno;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 64 * 1024 * 1024) {
        close(fd); return refuse(WFS_E_GIT_UNSUPPORTED, "%s is not a regular file or is larger than 64 MiB", path);
    }
    out.clear(); char buf[8192]; int rc = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { rc = -errno; break; }
        if (!n) break;
        if (out.size() + (size_t)n > 64 * 1024 * 1024) { rc = -EFBIG; break; }
        for (ssize_t i = 0; i < n; ++i) out.emplace_back(buf[i]);
    }
    close(fd); return rc;
}
int write_bytes(const char *path, const char *data, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -errno;
    int rc = 0;
    while (size) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { rc = n < 0 ? -errno : -EIO; break; }
        data += n; size -= (size_t)n;
    }
    if (close(fd) && !rc) rc = -errno;
    return rc;
}
int write_text(const char *root, const char *rel, const char *text) {
    String path = joinp(root, rel);
    return write_bytes(path.c_str(), text, strlen(text));
}

// A gitlink (mode 160000) entry of an index or a tree: a submodule's path and recorded commit.
struct Gitlink {
    String path, oid;
};
const Gitlink *find_link(const Vec<Gitlink> &links, const char *path) {
    for (const auto &l : links) if (l.path == path) return &l;
    return nullptr;
}
int list_names(int fd, Vec<String> &out);
String absolute_lexical(const char *base, const char *rel);
// One of Git's one-line link files -- a registration's `gitdir` or `commondir`, a linked
// worktree's `.git` (`prefix` "gitdir: ") -- with a relative target resolved against `base`, the
// way Git reads it. False for anything but a regular file holding exactly one such line.
bool read_link_file(const String &path, const char *prefix, const String &base, String &out) {
    struct stat st;
    if (lstat(path.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_size > 16 * 1024) return false;
    Vec<char> bytes;
    if (read_bytes(path.c_str(), bytes)) return false;
    size_t n = bytes.size(), p = strlen(prefix);
    if (n && bytes[n - 1] == '\n') --n;
    if (n <= p || memcmp(bytes.data(), prefix, p)) return false;
    for (size_t i = p; i < n; ++i) if (bytes[i] == '\n' || bytes[i] == '\r' || !bytes[i]) return false;
    String target(bytes.data() + p, n - p);
    out = target[0] == '/' ? target : joinp(base.c_str(), target.c_str());
    return true;
}
// Whether `path` lies strictly below `root` (both spelled the same way); `rel` gets the rest.
bool under_root(const String &path, const String &root, String *rel) {
    size_t n = root.size();
    if (path.size() <= n + 1 || strncmp(path.c_str(), root.c_str(), n) || path[n] != '/') return false;
    if (rel) rel->assign(path.c_str() + n + 1);
    return true;
}
// The linked worktrees registered in `common` other than the checkout at `root` (whose own
// administration is `self`), with where each checkout lies (GitWorktree). A checkout inside the
// tree is listed only when it is exactly what its registration describes: its `.git` is a
// regular file whose gitdir resolves to that registration, the registration's gitdir resolves
// to this checkout and its commondir to `common`. Anything else there is left to nested_walk,
// which refuses it. A checkout under the reserved `.git`, `.world` or `.world-git` names is never
// admitted.
//
// An external source (`strict` false) needs only the admitted in-tree checkouts: its other
// registrations stay in the source and are not imported. A managed World (`strict`) lists every
// registration, because each one would be copied with its .world-git: one that cannot be read,
// or whose checkout is in the tree but does not link back, is refused rather than guessed at. A
// registered checkout that no longer exists is `inside` when its registered path is in the tree.
int collect_worktrees(const char *root, const char *common, const char *self, bool strict, Vec<GitWorktree> &out) {
    out.clear();
    String dir = joinp(common, "worktrees");
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        return strict ? refuse(WFS_E_GIT_UNSUPPORTED, "the World's Git worktrees directory is not a directory") : -errno;
    }
    Vec<String> names;
    int rc = list_names(fd, names);
    close(fd);
    if (rc) return rc;
    String real_root, real_common, real_self;
    if ((rc = fs_realpath(root, real_root)) || (rc = fs_realpath(common, real_common)) ||
        (rc = fs_realpath(self, real_self))) return rc;
    String lexical_root = absolute_lexical("/", root);
    for (const auto &name : names) {
        String admin = joinp(dir.c_str(), name.c_str()), real_admin, link;
        struct stat st;
        if (lstat(admin.c_str(), &st) || !S_ISDIR(st.st_mode) || fs_realpath(admin.c_str(), real_admin)) {
            if (strict) return refuse(WFS_E_GIT_UNSUPPORTED, "the World's linked worktree registration %s is not a directory", name.c_str());
            continue;
        }
        if (real_admin == real_self) continue;
        String gitdir = joinp(admin.c_str(), "gitdir");
        if (!read_link_file(gitdir, "", admin, link) || link.size() < 6 || strcmp(link.c_str() + link.size() - 5, "/.git")) {
            if (strict) return refuse(WFS_E_GIT_UNSUPPORTED, "the World's linked worktree registration %s has no readable gitdir link", name.c_str());
            continue;
        }
        GitWorktree w;
        w.id = name;
        w.path = absolute_lexical("/", String(link.c_str(), link.size() - 5).c_str());
        String real_checkout;
        if (fs_realpath(w.path.c_str(), real_checkout)) {
            // Gone (a prunable registration): nothing of it is left in the tree to leave out.
            w.inside = under_root(w.path, lexical_root, nullptr) || under_root(w.path, real_root, nullptr);
        } else if (under_root(real_checkout, real_root, &w.rel)) {
            w.inside = true;
            const char *slash = strchr(w.rel.c_str(), '/');
            String first(w.rel.c_str(), slash ? (size_t)(slash - w.rel.c_str()) : w.rel.size());
            bool reserved = first == ".git" || first == ".world" || first == ".world-git";
            String back, real_back, common_link, real_common_link;
            bool linked = !reserved &&
                read_link_file(joinp(real_checkout.c_str(), ".git"), "gitdir: ", real_checkout, back) &&
                !fs_realpath(back.c_str(), real_back) && real_back == real_admin &&
                read_link_file(joinp(admin.c_str(), "commondir"), "", admin, common_link) &&
                !fs_realpath(common_link.c_str(), real_common_link) && real_common_link == real_common;
            if (!linked) {
                if (strict) return refuse(WFS_E_GIT_UNSUPPORTED,
                    "the World's linked worktree %s at %s does not link back to its registration", name.c_str(), w.rel.c_str());
                continue;
            }
        }
        if (!strict && w.rel.empty()) continue;
        out.emplace_back(w);
    }
    return 0;
}
bool is_omitted_worktree(const Vec<GitWorktree> *worktrees, const String &rel) {
    if (worktrees) for (const auto &w : *worktrees) if (!w.rel.empty() && w.rel == rel) return true;
    return false;
}
// A nested repository cannot be made safe by fixing only the root's .git file. The only nested
// `.git` admitted is a submodule's: a directory that this repository's index records as a
// gitlink (`links`), which discover_modules then validates, captures and walks on its own.
// Everything else with a `.git` below the root -- a plain nested repository, a `.git` inside an
// uninitialized submodule's directory -- is refused. The walk's own top may hold its `.git`; only
// the top of the tree itself (`world_root`) may also hold the World's `.world-git`, never a
// submodule's top. The checkouts of the root repository's other linked worktrees (`worktrees`,
// validated by collect_worktrees) are not part of the tree and are not entered: whatever is
// below one -- an agent's own nested worktrees included -- goes with it.
int nested_walk(const String &dir_path, const String &rel, const Vec<Gitlink> *links, bool world_root,
                const Vec<GitWorktree> *worktrees) {
    bool top = rel.empty();
    DIR *dir = opendir(dir_path.c_str());
    if (!dir) return -errno;
    int rc = 0;
    for (;;) {
        errno = 0;
        dirent *e = readdir(dir);
        if (!e) { if (errno) rc = -errno; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!strcmp(e->d_name, ".git")) {
            if (!top) { rc = refuse(WFS_E_GIT_UNSUPPORTED, "nested Git repository or submodule at %s", dir_path.c_str()); break; }
            continue;
        }
        if (top && world_root && !strcmp(e->d_name, ".world-git")) continue;
        String path = joinp(dir_path.c_str(), e->d_name);
        struct stat st;
        if (lstat(path.c_str(), &st)) { rc = -errno; break; }
        if (!S_ISDIR(st.st_mode)) continue;
        String child = top ? String(e->d_name) : joinp(rel.c_str(), e->d_name);
        if (links && find_link(*links, child.c_str())) {
            String dot = joinp(path.c_str(), ".git");
            if (!lstat(dot.c_str(), &st)) continue;   // an initialized submodule
            if (errno != ENOENT) { rc = -errno; break; }
        }
        if (is_omitted_worktree(worktrees, child)) continue;
        if ((rc = nested_walk(path, child, links, world_root, worktrees))) break;
    }
    closedir(dir); return rc;
}
int nested_check(const char *root, const Vec<Gitlink> *links, bool world_root,
                 const Vec<GitWorktree> *worktrees = nullptr) {
    return nested_walk(String(root), String(), links, world_root, worktrees);
}
// A managed tree may be copied without consulting an external repository only when all
// administration stays inside it. Reject changed common-dir pointers. Additional linked
// worktrees -- an agent's, created inside the World or elsewhere -- are listed in `worktrees`
// (collect_worktrees): a copy leaves them out (omit_worktrees), so they are validated here.
int managed_check(const char *root, const char *common, const char *admin, Vec<GitWorktree> &worktrees) {
    String repo = joinp(root, ".world-git/repo.git"), active = joinp(repo.c_str(), "worktrees/active");
    String expected, actual;
    if (fs_realpath(repo.c_str(), expected) || fs_realpath(common, actual) || expected != actual)
        return refuse(WFS_E_GIT_UNSUPPORTED, "the World's Git common directory is not its own .world-git/repo.git");
    if (fs_realpath(active.c_str(), expected) || fs_realpath(admin, actual) || expected != actual)
        return refuse(WFS_E_GIT_UNSUPPORTED, "the World's worktree administration is not its own");
    for (const char *file : {"commondir", "gitdir"}) {
        Vec<char> bytes;
        String p = joinp(active.c_str(), file);
        if (int rc = read_bytes(p.c_str(), bytes)) return rc;
        const char *expected_text = !strcmp(file, "commondir") ? "../..\n" : "../../../../.git\n";
        String normalized;
        for (char c : bytes) {
            // Git 2.54 repair emits a doubled slash before .git; it is still the same
            // relative link. Compare its normalized spelling, never an external pathname.
            if (c == '/' && !normalized.empty() && normalized.back() == '/') continue;
            normalized.push_back(c);
        }
        if (normalized != expected_text)
            return refuse(WFS_E_GIT_UNSUPPORTED, "the World's worktree %s link was changed", file);
    }
    // Reject symlinks anywhere in the owned administration, including its root. Submodule
    // repositories live below active/modules (see GitModule); a `worktrees` directory in one of
    // them is a linked worktree of that submodule registered from somewhere else.
    String modules = joinp(active.c_str(), "modules/");
    Vec<String> dirs; dirs.emplace_back(joinp(root, ".world-git"));
    for (size_t i = 0; i < dirs.size(); ++i) {
        String path = dirs[i]; struct stat st;
        if (lstat(path.c_str(), &st) || !S_ISDIR(st.st_mode))
            return refuse(WFS_E_GIT_UNSUPPORTED, "the World's Git administration contains a symlink at %s", path.c_str());
        DIR *d = opendir(path.c_str()); if (!d) return -errno;
        int rc = 0;
        for (;;) {
            errno = 0; dirent *e = readdir(d);
            if (!e) { if (errno) rc = -errno; break; }
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            String child = joinp(path.c_str(), e->d_name);
            if (lstat(child.c_str(), &st)) { rc = -errno; break; }
            if (S_ISDIR(st.st_mode)) dirs.emplace_back(child);
            else if (!S_ISREG(st.st_mode)) {
                rc = refuse(WFS_E_GIT_UNSUPPORTED, "the World's Git administration contains a symlink or special file at %s", child.c_str());
                break;
            }
            if (S_ISDIR(st.st_mode) && !strcmp(e->d_name, "worktrees") && !strncmp(path.c_str(), modules.c_str(), modules.size())) {
                String head = joinp(path.c_str(), "HEAD");
                if (!lstat(head.c_str(), &st)) {
                    rc = refuse(WFS_E_GIT_UNSUPPORTED, "a submodule repository of the World has a linked worktree (%s)", child.c_str()); break;
                }
                if (errno != ENOENT) { rc = -errno; break; }
            }
        }
        closedir(d); if (rc) return rc;
    }
    return collect_worktrees(root, repo.c_str(), active.c_str(), true, worktrees);
}
int config(const char *root, const char *key, const char *val) {
    const char *args[] = {"config", "--local", key, val, nullptr};
    return git(root, args);
}
int unset_config(const char *root, const char *key) {
    const char *args[] = {"config", "--local", "--unset-all", key, nullptr};
    int status = -1, rc = git(root, args, nullptr, &status);
    if (rc == WFS_E_GIT_FAILED && status == 5) return 0;
    return rc;
}
int get_config(const char *root, const char *const *args, String &out, bool *present = nullptr) {
    Vec<char> bytes; int status = -1;
    int rc = git(root, args, &bytes, &status);
    if (rc == WFS_E_GIT_FAILED && status == 1) { out.clear(); if (present) *present = false; return 0; }
    if (rc) return rc;
    if (present) *present = true;
    out.assign(bytes.data());
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return 0;
}
int capture_settings(const char *root, Vec<GitSetting> &out) {
    out.clear();
    int rc = 0;
    const char *special[] = {"core.autocrlf", "core.safecrlf", nullptr};
    for (size_t i = 0; special[i]; ++i) {
        const char *raw[] = {"config", "--get", special[i], nullptr};
        String value; bool present;
        if ((rc = get_config(root, raw, value, &present))) return rc;
        if (!present) continue;
        bool literal = (i == 0 && !strcasecmp(value.c_str(), "input")) ||
                       (i == 1 && !strcasecmp(value.c_str(), "warn"));
        if (!literal) {
            const char *typed[] = {"config", "--get", "--type=bool", special[i], nullptr};
            if ((rc = get_config(root, typed, value, &present))) return rc;
            if (!present) return -EBUSY;
        }
        out.emplace_back(GitSetting{special[i], value});
    }
    const char *strings[] = {"core.eol", "core.checkstat", "core.checkRoundtripEncoding", nullptr};
    for (size_t i = 0; strings[i]; ++i) {
        const char *raw[] = {"config", "--get", strings[i], nullptr};
        String value; bool present;
        if ((rc = get_config(root, raw, value, &present))) return rc;
        if (present) out.emplace_back(GitSetting{strings[i], value});
    }
    // core.useReplaceRefs decides whether the mirrored refs/replace/* change what commits and
    // trees mean, so it travels with the refs rather than defaulting back to true.
    const char *bool_keys[] = {"core.filemode", "core.symlinks", "core.ignorecase", "core.precomposeunicode", "core.trustctime", "core.ignorestat",
                               "core.useReplaceRefs", nullptr};
    for (size_t i = 0; bool_keys[i]; ++i) {
        const char *typed[] = {"config", "--get", "--type=bool", bool_keys[i], nullptr};
        String value; bool present;
        if ((rc = get_config(root, typed, value, &present))) return rc;
        if (present) out.emplace_back(GitSetting{bool_keys[i], value});
    }
    return 0;
}
// Repository-local configuration that makes a World usable as a place to work, carried from
// an external source into the owned repository (a mirror clone copies refs, not config):
// remotes (URLs, refspecs, tag and prune options), branch upstreams, URL rewrites, push/fetch
// defaults, submodule settings and aliases. Aliases run only when the user types them, as they
// would in the source. Settings Git executes on its own -- hooks, remote.*.uploadpack/receivepack/vcs,
// branch.*.mergeoptions, core.sshCommand, credential helpers -- are not carried. A relative
// local remote URL is made absolute against the source (see `absolutize_remote_path`), so it
// still reaches the same repository once the source is gone; one that any url.<base>.insteadOf
// or pushInsteadOf rule matches is refused instead (make the URL absolute or remove the rule),
// since reproducing Git's rewrite of a relative path after the World moves elsewhere cannot be
// done faithfully.
const char *const kCarriedConfig =
    "^(remote\\..+\\.(url|pushurl|lfsurl|lfspushurl|lfsdefault|lfspushdefault|fetch|push|tagopt|prune|prunetags|mirror|skipdefaultupdate|skipfetchall|followremotehead)"
    "|remote\\.(lfsdefault|lfspushdefault)"
    "|lfs\\..+"
    "|remotes\\..+|remote\\.pushdefault"
    "|branch\\..+\\.(remote|merge|pushremote|rebase|description)"
    "|url\\..+\\.(insteadof|pushinsteadof)"
    "|push\\.(default|autosetupremote)|fetch\\.(prune|prunetags)"
    "|submodule\\..+\\.(url|active|branch|shallow|fetchrecursesubmodules|ignore|update)|submodule\\.active"
    "|alias\\..+)$";
// Resolve "." and ".." lexically: the path must stay valid after the directory it was relative
// to (the source) is deleted, so it cannot be resolved through that directory.
String absolute_lexical(const char *base, const char *rel) {
    String joined = joinp(base, rel);
    Vec<String> parts;
    const char *p = joined.c_str();
    while (*p) {
        while (*p == '/') ++p;
        const char *start = p;
        while (*p && *p != '/') ++p;
        String part(start, (size_t)(p - start));
        if (part.empty() || part == ".") continue;
        if (part == "..") { if (!parts.empty()) parts.pop_back(); continue; }
        parts.emplace_back(part);
    }
    String out;
    for (const auto &part : parts) { out.push_back('/'); out.append(part.c_str()); }
    if (out.empty()) out.assign("/");
    return out;
}
// Make a relative local remote URL/pushurl absolute the way Git itself would reach it: through
// the filesystem, so a symlink component before a ".." segment (or before the part of the path
// that does not exist yet) lands where Git actually resolves it, which `absolute_lexical`'s
// lexical collapse cannot see -- e.g. `link/../up.git` with `link` a symlink to elsewhere
// resolves through that symlink, not lexically against `root`, and so does `link/new.git` when
// `new.git` does not exist yet but `link` does.
//
// If the whole joined path exists, `fs_realpath` resolves it exactly as Git would. If it does
// not (the remote was never fetched into the source, or names a path only `git push` would
// create), resolve the LONGEST EXISTING PREFIX of `rel` through the filesystem instead and join
// the missing tail onto that real path lexically: everything up to the missing tail is real, so
// any symlink in it is honored, and only the part with nothing on disk to resolve it through is
// taken literally. Refuse rather than guess when that missing tail itself contains a ".."
// component (there is no filesystem left to resolve it through) or when the first missing
// component exists per `lstat` as a symlink (a dangling one -- `realpath` fails on it, but Git
// would still follow it to wherever its target names, which cannot be told from here). If even
// `root` fails to resolve (should not happen), fall back to the old lexical join. Used only for
// a relative local remote URL (`is_relative_local_url`); other relative settings this file
// carries (e.g. hooksPath) still use `absolute_lexical` directly.
int absolutize_remote_path(const char *root, const char *rel, String &out) {
    String joined = joinp(root, rel);
    String real;
    if (fs_realpath(joined.c_str(), real) == 0) { out = real; return 0; }

    Vec<String> parts;
    for (const char *p = rel; *p;) {
        while (*p == '/') ++p;
        const char *start = p;
        while (*p && *p != '/') ++p;
        if (p != start) parts.emplace_back(start, (size_t)(p - start));
    }
    size_t n = parts.size();
    Vec<String> prefixes;                 // prefixes[i] = root joined with parts[0..i-1]
    prefixes.emplace_back(root);
    for (size_t i = 0; i < n; ++i) prefixes.emplace_back(joinp(prefixes[i].c_str(), parts[i].c_str()));

    for (size_t k = n; k-- > 0;) {
        String preal;
        if (fs_realpath(prefixes[k].c_str(), preal) != 0) continue;   // not the longest prefix
        for (size_t i = k; i < n; ++i) {
            if (parts[i] == "..")
                return refuse(WFS_E_GIT_POLICY,
                    "relative remote path %s does not exist, so how its '..' resolves cannot be "
                    "preserved; make the remote URL absolute before importing",
                    rel);
        }
        struct stat st;
        if (::lstat(prefixes[k + 1].c_str(), &st) == 0 && S_ISLNK(st.st_mode))
            return refuse(WFS_E_GIT_POLICY,
                "relative remote path %s passes through dangling symlink %s, so where it "
                "resolves cannot be preserved; make the remote URL absolute before importing",
                rel, prefixes[k + 1].c_str());
        out = preal;
        for (size_t i = k; i < n; ++i) {
            if (parts[i] == ".") continue;
            if (out.empty() || out.c_str()[out.size() - 1] != '/') out.append("/");
            out.append(parts[i].c_str());
        }
        return 0;
    }
    out = absolute_lexical(root, rel);
    return 0;
}
bool is_carried_boolean(const char *key) {
    size_t n = strlen(key);
    auto ends = [&](const char *suffix) { size_t m = strlen(suffix); return n > m && !strcmp(key + n - m, suffix); };
    if (!strncmp(key, "remote.", 7))
        return ends(".prune") || ends(".prunetags") || ends(".mirror") || ends(".skipdefaultupdate") || ends(".skipfetchall");
    if (!strncmp(key, "branch.", 7)) return ends(".rebase");
    if (!strncmp(key, "submodule.", 10)) return ends(".active") || ends(".shallow") || ends(".fetchrecursesubmodules");
    return !strcmp(key, "push.autosetupremote") || !strcmp(key, "fetch.prune") || !strcmp(key, "fetch.prunetags");
}
bool is_relative_local_url(const char *url) {
    if (!*url || url[0] == '/' || url[0] == '~' || strstr(url, "://")) return false;
    const char *colon = strchr(url, ':'), *slash = strchr(url, '/');
    if (colon && (!slash || colon < slash)) return false; // scp-like host:path
    return true;
}
// A url.<base>.insteadOf/pushInsteadOf rule, as Git applies it to a URL: `push` says which
// direction it rewrites (pushInsteadOf falls back to insteadOf only when a remote has no
// explicit pushurl); `base` and `prefix` are the rule's <base> subsection and its value, the
// literal prefix Git replaces.
struct UrlRewriteRule {
    bool push;
    String base;
    String prefix;
};
// Split a url.<base>.(insteadof|pushinsteadof) key, exactly as --get-regexp prints it (the
// variable name lowercased, the subsection kept verbatim including any dots it contains), into
// its direction and base. False for any other key.
bool parse_rewrite_key(const char *key, bool &push, String &base) {
    size_t n = strlen(key);
    if (strncmp(key, "url.", 4)) return false;
    static const char *const push_suffix = ".pushinsteadof";
    static const char *const fetch_suffix = ".insteadof";
    size_t pn = strlen(push_suffix), fn = strlen(fetch_suffix);
    if (n > 4 + pn && !strcmp(key + n - pn, push_suffix)) {
        push = true; base.assign(key + 4, n - 4 - pn); return true;
    }
    if (n > 4 + fn && !strcmp(key + n - fn, fetch_suffix)) {
        push = false; base.assign(key + 4, n - 4 - fn); return true;
    }
    return false;
}
// Parse one "--null --get-regexp ^url\..*\.(insteadof|pushinsteadof)$" listing ("<key>\n<value>\0"
// entries) into rules. A valueless rule can never match a URL and is skipped, the same as Git
// ignores it.
void parse_rewrite_listing(const Vec<char> &listing, Vec<UrlRewriteRule> &out) {
    for (size_t i = 0; i < listing.size() && listing[i];) {
        const char *entry = listing.data() + i;
        size_t len = strlen(entry);
        i += len + 1;
        const char *nl = strchr(entry, '\n');
        if (!nl || !*(nl + 1)) continue;
        String key(entry, (size_t)(nl - entry));
        bool push; String base;
        if (parse_rewrite_key(key.c_str(), push, base))
            out.emplace_back(UrlRewriteRule{push, base, String(nl + 1)});
    }
}
// The rule among `rules` -- insteadOf or pushInsteadOf, whichever has the longer matching
// prefix -- whose prefix matches `url`, or nullptr if none does. Used only to decide whether a
// relative remote URL is refused (see capture_carried_config): which single rule Git itself
// would apply, and in which direction, does not matter for that decision.
const UrlRewriteRule *matching_rewrite(const Vec<UrlRewriteRule> &rules, const char *url) {
    const UrlRewriteRule *best = nullptr;
    for (const auto &r : rules) {
        if (r.prefix.empty()) continue;
        if (strncmp(url, r.prefix.c_str(), r.prefix.size())) continue;
        if (!best || r.prefix.size() > best->prefix.size()) best = &r;
    }
    return best;
}
int scan_conditional_includes(const char *root, bool *sets_identity, Vec<UrlRewriteRule> *rewrites = nullptr);
// Every raw remote.<name>.(url|pushurl) entry capture_carried_config finds, and where each one
// lands in `out`, kept only long enough for the pinning pass below to decide whether a rewrite
// rule changes that remote's effective URL and, if so, to substitute it in. `pin_urls`, once set,
// is what gets substituted for the remote's url entries; `pin_pushurls`, when also set, is added
// as its pushurl entries -- always, when the remote had an explicit pushurl in the source, so it
// stays explicit at the World rather than falling back to the (possibly rewritten) url entries. A
// remote may have only a pushurl and no url at all (Git supports this); such a remote has
// raw_urls/url_indices empty, and, if its effective push resolves differently, ends up with
// pin_pushurls set but pin_urls left empty.
struct RemoteRewriteInfo {
    String name;
    Vec<String> raw_urls, raw_pushurls;
    Vec<size_t> url_indices, pushurl_indices;
    Vec<String> pin_urls, pin_pushurls;
};
RemoteRewriteInfo &remote_info(Vec<RemoteRewriteInfo> &remotes, const String &name) {
    for (auto &r : remotes) if (r.name == name) return r;
    return remotes.emplace_back(RemoteRewriteInfo{name, {}, {}, {}, {}, {}, {}});
}
// The newline-separated output of a command such as "git remote get-url --all <name>", with the
// trailing newline stripped and each remaining line its own entry, in order. Callers must first
// rule out any value (e.g. a raw remote URL) that could itself contain an embedded '\n' -- such a
// value would split into extra, bogus entries here, indistinguishable from genuinely separate
// lines.
int git_lines(const char *root, const char *const *args, Vec<String> &out) {
    out.clear();
    Vec<char> buf; int status = -1;
    if (int rc = git(root, args, &buf, &status, false, true)) return rc;
    size_t n = buf.empty() ? 0 : buf.size() - 1; // exclude the '\0' git() appends
    size_t start = 0;
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] != '\n') continue;
        out.emplace_back(String(buf.data() + start, i - start));
        start = i + 1;
    }
    if (start < n) out.emplace_back(String(buf.data() + start, n - start));
    return 0;
}
int capture_carried_config(const char *root, Vec<GitSetting> &out) {
    out.clear();
    // Every url.<base>.insteadOf/pushInsteadOf rule that could rewrite a URL this import
    // carries or pins: every rule the user's own Git currently applies here, from every scope
    // it reads (ambient_config_probe=true covers global/system too, not just what this
    // repository carries), plus -- below -- every rule reached only through a conditional
    // include, active here or not, since one that is inactive at the source may become active
    // once the World moves. A relative remote URL that any of these rules matches is refused
    // rather than carried (below), and so is any URL -- pinned or carried as-is -- that any of
    // them could still rewrite once the World is placed somewhere else (the chain guard,
    // further down): reproducing Git's insteadOf/pushInsteadOf resolution across a change of
    // location (which rule wins, whether a pushurl needs to be synthesized) has repeatedly
    // diverged from Git's actual behavior, and the combination is rare enough to refuse outright
    // instead.
    Vec<UrlRewriteRule> rules, conditional_rules;
    {
        const char *rw_args[] = {"config", "--includes", "--null", "--get-regexp",
            "^url\\..*\\.(insteadof|pushinsteadof)$", nullptr};
        Vec<char> listing; int status = -1;
        int rc = git(root, rw_args, &listing, &status, false, true);
        if (rc && !(rc == WFS_E_GIT_FAILED && status == 1)) return rc;
        if (!rc) parse_rewrite_listing(listing, rules);
    }
    if (int rc = scan_conditional_includes(root, nullptr, &conditional_rules)) return rc;
    for (const auto &c : conditional_rules) rules.emplace_back(c);
    const char *args[] = {"config", "--local", "--includes", "--null", "--get-regexp", kCarriedConfig, nullptr};
    Vec<char> listing; int status = -1;
    int rc = git(root, args, &listing, &status);
    if (rc == WFS_E_GIT_FAILED && status == 1) return 0;
    if (rc) return rc;
    // Every remote's raw url/pushurl entries and where they land in `out`, for the pinning pass
    // below.
    Vec<RemoteRewriteInfo> remotes;
    // Entries are "<key>\n<value>\0"; a valueless key has no newline.
    for (size_t i = 0; i < listing.size() && listing[i];) {
        const char *entry = listing.data() + i;
        size_t len = strlen(entry);
        i += len + 1;
        const char *nl = strchr(entry, '\n');
        // A valueless entry of a boolean key (`[remote "o"] prune`) is true to Git; written back
        // as an empty value it would read as false, so it is carried as "true". A valueless
        // string key (a URL, a description, an alias) stays an empty value.
        String key(entry, nl ? (size_t)(nl - entry) : len), val(nl ? nl + 1 : "");
        if (!nl && is_carried_boolean(key.c_str())) val.assign("true");
        size_t klen = key.size();
        bool is_remote = !strncmp(key.c_str(), "remote.", 7);
        bool is_url = is_remote && klen > 4 && !strcmp(key.c_str() + klen - 4, ".url");
        bool is_pushurl = is_remote && klen > 8 && !strcmp(key.c_str() + klen - 8, ".pushurl");
        bool is_submodule = !strncmp(key.c_str(), "submodule.", 10);
        bool is_submodule_url = is_submodule && klen > 14 && !strcmp(key.c_str() + klen - 4, ".url");
        // submodule.<name>.update = !<command> makes `git submodule update` run that command;
        // only the update modes Git performs itself are carried.
        if (is_submodule && klen > 17 && !strcmp(key.c_str() + klen - 7, ".update") &&
            val != "checkout" && val != "rebase" && val != "merge" && val != "none")
            return refuse(WFS_E_GIT_POLICY,
                "%s = %s is not an update mode WorldFS carries (a '!' command would be run by "
                "`git submodule update`); only checkout, rebase, merge and none are carried",
                key.c_str(), val.c_str());
        if (is_submodule_url && strpbrk(val.c_str(), "\n\r"))
            return refuse(WFS_E_GIT_POLICY,
                "%s contains a line break, which cannot be carried unambiguously; rename the path "
                "before importing", key.c_str());
        if (is_submodule_url && is_relative_local_url(val.c_str())) {
            const UrlRewriteRule *r = matching_rewrite(rules, val.c_str());
            if (r)
                return refuse(WFS_E_GIT_POLICY,
                    "%s %s is relative and url.%s.%s rewrites it; make the URL absolute or remove "
                    "the rewrite before importing",
                    key.c_str(), val.c_str(), r->base.c_str(), r->push ? "pushInsteadOf" : "insteadOf");
            if (int arc = absolutize_remote_path(root, val.c_str(), val)) return arc;
        }
        // A local-path remote URL may legally contain an embedded newline (or carriage return).
        // `git remote get-url --all`/`--push --all` below would emit it verbatim, and git_lines
        // splits on every '\n', so the effective URL list would no longer match this raw value
        // and the pinning pass could replace one working remote with several broken ones. Refuse
        // up front, before any of that runs, rather than carry it wrong.
        if ((is_url || is_pushurl) && strpbrk(val.c_str(), "\n\r")) {
            String name(key.c_str() + 7, klen - 7 - (is_url ? 4 : 8));
            return refuse(WFS_E_GIT_POLICY,
                "remote %s has a %s containing a line break, which cannot be carried unambiguously; "
                "rename the path before importing",
                name.c_str(), is_url ? "url" : "pushurl");
        }
        // The raw value, before a relative one below is made absolute: what the pinning pass
        // compares Git's effective URLs against, so a remote the source itself does not rewrite
        // is left untouched.
        if (is_url || is_pushurl) {
            String name(key.c_str() + 7, klen - 7 - (is_url ? 4 : 8));
            RemoteRewriteInfo &info = remote_info(remotes, name);
            if (is_url) { info.raw_urls.emplace_back(val); info.url_indices.emplace_back(out.size()); }
            else { info.raw_pushurls.emplace_back(val); info.pushurl_indices.emplace_back(out.size()); }
        }
        if ((is_url || is_pushurl) && is_relative_local_url(val.c_str())) {
            const UrlRewriteRule *r = matching_rewrite(rules, val.c_str());
            if (r)
                return refuse(WFS_E_GIT_POLICY,
                    "remote URL %s is relative and url.%s.%s rewrites it; make the remote URL "
                    "absolute or remove the rewrite before importing",
                    val.c_str(), r->base.c_str(), r->push ? "pushInsteadOf" : "insteadOf");
            if (int arc = absolutize_remote_path(root, val.c_str(), val)) return arc;
        }
        out.emplace_back(GitSetting{key, val});
    }
    // A relative submodule.<name>.url in the configuration is not resolved against a remote:
    // observed with Git 2.54, `git submodule update --init` hands a configured URL to clone as
    // it is, from the superproject's worktree top (from a subdirectory too, and with a remote
    // origin present), so `../lib.git` names <top>/../lib.git -- exactly what the absolutization
    // above reproduces. Only a URL that .gitmodules alone gives is resolved against the default
    // remote. Every submodule URL is classified and checked against rewrite rules in one place,
    // with the shared configuration it competes with (discover_modules).
    // A remote is one unit: if any of its repository-local settings are carried (a subsectioned
    // remote.<name>.<var> key from kCarriedConfig above, e.g. .fetch or .prune -- not a
    // section-wide key like remote.pushDefault, which has no <name>), its url/pushurl must also
    // come only from that same repository-local configuration. The World reads the same
    // global/system/command configuration the source does, so a remote.<name>.url or .pushurl
    // set there would be visible to the World too -- duplicating the URL (fetch would try both,
    // and a push URL could be contacted twice) even when no local url/pushurl exists to pin
    // against, or contacting a different, source-only rewritten endpoint (see the includeIf
    // rewrite pass above) than the one the World would reach. Refuse instead of guessing which
    // one should win.
    Vec<String> carried_remote_names;
    for (const auto &s : out) {
        const char *key = s.key.c_str();
        if (strncmp(key, "remote.", 7)) continue;
        const char *rest = key + 7;
        const char *last_dot = strrchr(rest, '.');
        if (!last_dot) continue; // section-wide key (e.g. remote.pushDefault): no <name>
        String name(rest, (size_t)(last_dot - rest));
        bool seen = false;
        for (const auto &n : carried_remote_names) if (n == name) { seen = true; break; }
        if (!seen) carried_remote_names.emplace_back(name);
    }
    {
        Vec<char> scoped; int scope_status = -1;
        const char *scope_args[] = {"config", "--includes", "--null", "--show-scope", "--get-regexp",
            "^remote\\..*\\.(url|pushurl)$", nullptr};
        int scope_rc = git(root, scope_args, &scoped, &scope_status, false, true);
        if (scope_rc && !(scope_rc == WFS_E_GIT_FAILED && scope_status == 1)) return scope_rc;
        // Entries are "<scope>\0<key>\n<value>\0".
        for (size_t i = 0; !scope_rc && i < scoped.size() && scoped[i];) {
            const char *scope = scoped.data() + i;
            i += strlen(scope) + 1;
            if (i >= scoped.size()) return WFS_E_GIT_FAILED;
            const char *entry = scoped.data() + i;
            i += strlen(entry) + 1;
            if (!strcmp(scope, "local") || !strcmp(scope, "worktree")) continue;
            const char *nl = strchr(entry, '\n');
            String key(entry, nl ? (size_t)(nl - entry) : strlen(entry));
            size_t klen = key.size();
            bool is_url = klen > 4 && !strcmp(key.c_str() + klen - 4, ".url");
            bool is_pushurl = klen > 8 && !strcmp(key.c_str() + klen - 8, ".pushurl");
            if (!is_url && !is_pushurl) continue;
            String name(key.c_str() + 7, klen - 7 - (is_url ? 4 : 8));
            bool carried = false;
            for (const auto &n : carried_remote_names) if (n == name) { carried = true; break; }
            if (!carried) continue;
            return refuse(WFS_E_GIT_POLICY,
                "remote %s has %s in %s configuration, which the World shares, while its other "
                "settings are carried from the repository; keep all of a remote's settings in "
                "one place before importing",
                name.c_str(), is_url ? "url" : "pushurl", scope);
        }
    }
    // Pin each remote whose effective URL a rewrite rule changes. An absolute remote URL is
    // carried as-is above, but a rule that lives in a conditional include active at the source
    // (the common per-account `includeIf "gitdir:..."` setup) still rewrites it there, and may or
    // may not apply once the World sits somewhere else. Rather than emulate Git's rewriting, ask
    // the source's own Git what it actually contacts, and record that instead: the World then
    // reaches the same endpoints wherever it is placed. A conditional rule that only applies at
    // the World's own location applies there, exactly as it would for any repository placed
    // there. A remote can also carry only a pushurl (no url), which Git supports; that remote has
    // no fetch side to ask about, so it is pinned separately, below, by querying only its push
    // side.
    for (auto &info : remotes) {
        if (info.raw_urls.empty() && info.raw_pushurls.empty()) continue; // nothing carried
        if (info.raw_urls.empty()) {
            // Push-only remote: only remote.<name>.pushurl is set. There is no url entry to ask
            // Git to resolve, and `remote get-url` without --push would fail outright, so only the
            // push side is queried.
            Vec<String> effective_push;
            const char *push_args[] = {"remote", "get-url", "--push", "--all", info.name.c_str(), nullptr};
            if (int grc = git_lines(root, push_args, effective_push)) return grc;
            if (effective_push == info.raw_pushurls) continue; // no rewrite applies
            for (auto &u : effective_push)
                if (is_relative_local_url(u.c_str()))
                    if (int arc = absolutize_remote_path(root, u.c_str(), u)) return arc;
            info.pin_pushurls = std::move(effective_push);
            continue;
        }
        Vec<String> effective_fetch;
        {
            const char *fetch_args[] = {"remote", "get-url", "--all", info.name.c_str(), nullptr};
            if (int grc = git_lines(root, fetch_args, effective_fetch)) return grc;
        }
        Vec<String> effective_push;
        {
            const char *push_args[] = {"remote", "get-url", "--push", "--all", info.name.c_str(), nullptr};
            if (int grc = git_lines(root, push_args, effective_push)) return grc;
        }
        bool has_explicit_pushurl = !info.raw_pushurls.empty();
        const Vec<String> &raw_push = has_explicit_pushurl ? info.raw_pushurls : info.raw_urls;
        if (effective_fetch == info.raw_urls && effective_push == raw_push)
            continue; // no rewrite applies; leave this remote as captured above
        // A rewrite applies: pin the source's result. An effective URL that is itself a relative
        // local path is only possible when no rule rewrote it; absolutize it exactly as above.
        for (auto &u : effective_fetch)
            if (is_relative_local_url(u.c_str()))
                if (int arc = absolutize_remote_path(root, u.c_str(), u)) return arc;
        bool same = effective_fetch == effective_push;
        // A pushurl is pinned whenever the effective push differs from the effective fetch, and
        // also whenever the remote had an explicit remote.<name>.pushurl in the source: Git never
        // falls back to a remote's (possibly rewritten) url entries once it has an explicit
        // pushurl, so leaving the pushurl unpinned here would make it implicit again at the World
        // and expose it to a pushInsteadOf rule active at the World's own location.
        bool pin_push = has_explicit_pushurl || !same;
        if (pin_push)
            for (auto &u : effective_push)
                if (is_relative_local_url(u.c_str()))
                    if (int arc = absolutize_remote_path(root, u.c_str(), u)) return arc;
        info.pin_urls = std::move(effective_fetch);
        if (pin_push) info.pin_pushurls = std::move(effective_push);
    }
    // Chain guard: whichever URL each remote will actually end up carrying for `url` and
    // `pushurl` -- the source's effective resolution, pinned above, or (when no rewrite applies
    // to this remote) its own raw, absolutized URL, carried as-is -- must not be matched by any
    // rule in `rules`: every insteadOf/pushInsteadOf rule the source's own Git could apply right
    // now, plus one reachable only through a conditional include that is inactive at the source
    // but could become active once the World moves. Otherwise the World would resolve a pinned
    // URL differently than the source does, or a rule inactive at the source today could start
    // rewriting an untouched, carried URL once the World sits somewhere else. A `url` is checked
    // against pushInsteadOf too, but only when the remote has no separate `pushurl` entry of its
    // own -- exactly when Git falls back to `url` for push. A `pushurl` (pinned or carried as-is)
    // is checked only against insteadOf, since Git never applies pushInsteadOf to an explicit
    // pushurl.
    for (const auto &info : remotes) {
        if (info.raw_urls.empty() && info.raw_pushurls.empty()) continue; // nothing carried
        bool url_pinned = !info.pin_urls.empty(), push_pinned = !info.pin_pushurls.empty();
        Vec<String> final_urls, final_pushurls;
        if (url_pinned) for (const auto &u : info.pin_urls) final_urls.emplace_back(u);
        else for (size_t idx : info.url_indices) final_urls.emplace_back(out[idx].value);
        if (push_pinned) for (const auto &u : info.pin_pushurls) final_pushurls.emplace_back(u);
        else for (size_t idx : info.pushurl_indices) final_pushurls.emplace_back(out[idx].value);
        bool has_pushurl = !final_pushurls.empty();
        for (const auto &u : final_urls) {
            for (const auto &r : rules) {
                if (r.prefix.empty() || strncmp(u.c_str(), r.prefix.c_str(), r.prefix.size())) continue;
                if (r.push && has_pushurl) continue;
                return refuse(WFS_E_GIT_POLICY,
                    url_pinned
                        ? "remote %s resolves to %s, which url.%s.%s would rewrite again; simplify the "
                          "URL rewrite rules before importing"
                        : "remote %s URL %s matches url.%s.%s, which a conditional include can activate "
                          "at the World's location; simplify the URL rewrite rules before importing",
                    info.name.c_str(), u.c_str(), r.base.c_str(), r.push ? "pushInsteadOf" : "insteadOf");
            }
        }
        for (const auto &u : final_pushurls) {
            for (const auto &r : rules) {
                if (r.push || r.prefix.empty() || strncmp(u.c_str(), r.prefix.c_str(), r.prefix.size())) continue;
                return refuse(WFS_E_GIT_POLICY,
                    push_pinned
                        ? "remote %s resolves to %s, which url.%s.%s would rewrite again; simplify "
                          "the URL rewrite rules before importing"
                        : "remote %s URL %s matches url.%s.%s, which a conditional include can activate "
                          "at the World's location; simplify the URL rewrite rules before importing",
                    info.name.c_str(), u.c_str(), r.base.c_str(), "insteadOf");
            }
        }
    }
    bool any_pinned = false;
    for (const auto &info : remotes)
        if (!info.pin_urls.empty() || !info.pin_pushurls.empty()) { any_pinned = true; break; }
    if (!any_pinned) return 0;
    // Replace each pinned remote's url/pushurl entries with the pinned ones, at the position of
    // its first original url entry (or, for a remote pinned on the push side only -- no url entry
    // at all -- its first original pushurl entry), and drop the rest.
    Vec<GitSetting> pinned;
    for (size_t i = 0; i < out.size(); ++i) {
        const RemoteRewriteInfo *first_owner = nullptr;
        bool drop = false;
        for (const auto &info : remotes) {
            if (info.pin_urls.empty() && info.pin_pushurls.empty()) continue;
            size_t anchor = !info.url_indices.empty() ? info.url_indices[0]
                : (info.pushurl_indices.empty() ? (size_t)-1 : info.pushurl_indices[0]);
            if (anchor == i) { first_owner = &info; break; }
            bool matched = false;
            for (size_t idx : info.url_indices) if (idx == i) matched = true;
            for (size_t idx : info.pushurl_indices) if (idx == i) matched = true;
            if (matched) { drop = true; break; }
        }
        if (first_owner) {
            if (!first_owner->pin_urls.empty()) {
                String url_key("remote."); url_key.append(first_owner->name.c_str()); url_key.append(".url");
                for (const auto &u : first_owner->pin_urls) pinned.emplace_back(GitSetting{url_key, u});
            }
            if (!first_owner->pin_pushurls.empty()) {
                String pushurl_key("remote."); pushurl_key.append(first_owner->name.c_str()); pushurl_key.append(".pushurl");
                for (const auto &u : first_owner->pin_pushurls) pinned.emplace_back(GitSetting{pushurl_key, u});
            }
        } else if (!drop) {
            pinned.emplace_back(out[i]);
        }
    }
    out.clear();
    for (auto &e : pinned) out.emplace_back(std::move(e));
    return 0;
}
// The identity later World commits are attributed with. Captured with the other settings and
// rechecked before publication, so a World never keeps an identity the source no longer has.
int capture_identity(const char *root, Vec<GitSetting> &out) {
    out.clear();
    for (const char *key : {"user.name", "user.email"}) {
        String val; bool present = false; const char *args[] = {"config", "--get", key, nullptr};
        if (int rc = get_config(root, args, val, &present)) return rc;
        if (present) out.emplace_back(GitSetting{key, val});
    }
    return 0;
}
// `git sparse-checkout` turns on extensions.worktreeConfig and keeps its switches in
// config.worktree; `disable` leaves core.sparseCheckout, core.sparseCheckoutCone and
// index.sparse there (all false). Those are captured and restored in the owned worktree, so a
// later `sparse-checkout init` behaves as it would in the source. Any other worktree-scoped
// setting is outside what the import reproduces and is refused.
int capture_worktree_config(const char *root, bool &enabled, Vec<GitSetting> &out) {
    enabled = false; out.clear();
    String val; bool present = false;
    const char *ext[] = {"config", "--local", "--type=bool", "--get", "extensions.worktreeConfig", nullptr};
    if (int rc = get_config(root, ext, val, &present)) return rc;
    if (!present || val != "true") return 0;
    enabled = true;
    const char *list[] = {"config", "--worktree", "--null", "--name-only", "--list", nullptr};
    Vec<char> names;
    int status = -1;
    int rc = git(root, list, &names, &status);
    if (rc == WFS_E_GIT_FAILED && status == 1) return 0;
    if (rc) return rc;
    for (size_t i = 0; i < names.size() && names[i];) {
        size_t start = i; while (i < names.size() && names[i]) ++i;
        String key(names.data() + start, i - start);
        ++i;
        if (key != "core.sparsecheckout" && key != "core.sparsecheckoutcone" && key != "index.sparse")
            return refuse(WFS_E_GIT_UNSUPPORTED, "worktree-scoped setting %s (only disabled sparse-checkout settings are carried)", key.c_str());
        bool seen = false;
        for (const auto &have : out) if (have.key == key) seen = true;
        if (seen) continue;
        const char *typed[] = {"config", "--worktree", "--type=bool", "--get", key.c_str(), nullptr};
        String typed_val; bool typed_present = false;
        if ((rc = get_config(root, typed, typed_val, &typed_present))) return rc;
        if (!typed_present) return -EBUSY;
        out.emplace_back(GitSetting{key, typed_val});
    }
    return 0;
}
bool same_settings(const Vec<GitSetting> &a, const Vec<GitSetting> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].key != b[i].key || a[i].value != b[i].value) return false;
    return true;
}
int capture_refs(const char *root, Vec<char> &out) {
    const char *args[] = {"for-each-ref", "--sort=refname", "--format=%(refname)%09%(objectname)", "refs/", nullptr};
    return git(root, args, &out);
}
int capture_orig(const char *root, bool &present, String &oid) {
    const char *exists[] = {"show-ref", "--exists", "ORIG_HEAD", nullptr};
    int status = -1, rc = git(root, exists, nullptr, &status, true);
    if (rc == WFS_E_GIT_FAILED && status == 2) { present = false; oid.clear(); return 0; }
    if (rc) return rc;
    const char *verify[] = {"rev-parse", "--verify", "--quiet", "ORIG_HEAD^{commit}", nullptr};
    if ((rc = value(root, verify, oid))) return rc;
    present = true; return 0;
}
// The stash stack is refs/stash plus its reflog: stash@{n} for n > 0 exists only as a reflog
// entry. Each line of a reflog is "<old> <new> <ident>\t<message>\n"; the entries' commits are
// the non-null <new> object IDs. Anything else is refused rather than guessed at.
int stash_tips(const Vec<char> &log, Vec<String> &out) {
    out.clear();
    auto hex_run = [&](size_t i, size_t end) {
        size_t j = i;
        while (j < end && ((log[j] >= '0' && log[j] <= '9') || (log[j] >= 'a' && log[j] <= 'f'))) ++j;
        return j - i;
    };
    size_t start = 0, n = log.size();
    while (start < n) {
        size_t end = start;
        while (end < n && log[end] != '\n') ++end;
        size_t len = hex_run(start, end), at = start + len + 1;
        if ((len != 40 && len != 64) || at >= end || log[at - 1] != ' ' || hex_run(at, end) != len ||
            at + len >= end || log[at + len] != ' ')
            return refuse(WFS_E_GIT_UNSUPPORTED, "the stash reflog could not be parsed");
        bool null = true;
        for (size_t i = at; i < at + len; ++i) if (log[i] != '0') { null = false; break; }
        if (!null) out.emplace_back(log.data() + at, len);
        start = end + 1;
    }
    return 0;
}
// Revisions for the objects of the stash entries that the mirror does not already hold: every
// entry, minus everything reachable from the other captured refs (`refs`, "<name>\t<oid>\n",
// which the mirror copies with their history). One per line, for --stdin / --revs.
int stash_revs(const GitRepoState &s, FILE *&out) {
    out = nullptr;
    Vec<String> tips;
    if (int rc = stash_tips(s.stash, tips)) return rc;
    if (tips.empty()) return 0;
    String text;
    for (const auto &tip : tips) { text.append(tip.c_str()); text.push_back('\n'); }
    size_t start = 0, n = s.refs.size();
    for (size_t i = 0; i <= n; ++i) {
        if (i < n && s.refs[i] != '\n' && s.refs[i] != '\0') continue;
        size_t tab = start;
        while (tab < i && s.refs[tab] != '\t') ++tab;
        if (tab + 1 < i && !(tab - start == 10 && !memcmp(s.refs.data() + start, "refs/stash", 10))) {
            text.push_back('^'); text.append(s.refs.data() + tab + 1, i - tab - 1); text.push_back('\n');
        }
        start = i + 1;
    }
    out = tmpfile();
    if (!out) return -errno;
    if (fwrite(text.c_str(), 1, text.size(), out) != text.size() || fflush(out)) {
        int rc = errno ? -errno : -EIO; fclose(out); out = nullptr; return rc;
    }
    rewind(out);
    return 0;
}
// The stash stack of the repository at `root` as the import keeps it: the reflog's bytes, Git's
// own listing of every entry (its selector, commit, tree, parents -- the index and untracked-
// files commits -- and message), and proof that every object of every entry is present.
// Recorded in `out`, whose `refs` must already be captured. A repository without a stash
// reflog (refs/stash alone, as `update-ref` leaves it, is an ordinary ref) records none.
int capture_stash(const char *root, GitRepoState &out) {
    const char *path_args[] = {"rev-parse", "--path-format=absolute", "--git-path", "logs/refs/stash", nullptr};
    if (int rc = value(root, path_args, out.stash_path)) return rc;
    int rc = read_bytes(out.stash_path.c_str(), out.stash);
    out.stash_present = rc == 0;
    out.stash_list.clear();
    if (rc == -ENOENT) { out.stash.clear(); return 0; }
    if (rc) return rc;
    const char *list[] = {"--no-replace-objects", "log", "--walk-reflogs", "--no-show-signature", "--no-decorate",
        "--no-color", "--format=%gD%x09%H%x09%T%x09%P%x09%gs", "refs/stash", "--", nullptr};
    if ((rc = git(root, list, &out.stash_list))) return rc;
    FILE *revs = nullptr;
    if ((rc = stash_revs(out, revs))) return rc;
    if (!revs) return 0;
    // Fails on any missing commit, tree or blob of an entry.
    const char *walk[] = {"--no-replace-objects", "rev-list", "--objects", "--quiet", "--stdin", nullptr};
    rc = git(root, walk, nullptr, nullptr, false, false, fileno(revs));
    fclose(revs);
    return rc;
}
bool same_stash(const GitRepoState &a, const GitRepoState &b) {
    return a.stash_present == b.stash_present && same_bytes(a.stash, b.stash) && same_bytes(a.stash_list, b.stash_list);
}
int walk_objects_fd(int fd, unsigned depth, uint64_t &total, uint64_t &entries) {
    if (depth > 256) return refuse(WFS_E_GIT_UNSUPPORTED, "the object directory is nested too deeply");
    int dupfd = dup(fd); if (dupfd < 0) return -errno;
    DIR *dir = fdopendir(dupfd);
    if (!dir) { int rc = -errno; close(dupfd); return rc; }
    int rc = 0;
    for (;;) {
        errno = 0; dirent *e = readdir(dir);
        if (!e) { if (errno) rc = -errno; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW)) { rc = -errno; break; }
        if (S_ISLNK(st.st_mode) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) {
            rc = refuse(WFS_E_GIT_UNSUPPORTED, "the object directory contains a symlink or special file (%s)", e->d_name); break;
        }
        if (UINT64_MAX - total < 1024 || (S_ISREG(st.st_mode) && (uint64_t)st.st_size > UINT64_MAX - total - 1024)) { rc = -EOVERFLOW; break; }
        if (S_ISREG(st.st_mode) && st.st_size < 0) { rc = -EOVERFLOW; break; }
        total += 1024 + (S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0);
        ++entries;
        if (S_ISDIR(st.st_mode)) {
            int child = openat(fd, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { rc = -errno; break; }
            rc = walk_objects_fd(child, depth + 1, total, entries); close(child);
            if (rc) break;
        }
    }
    closedir(dir); return rc;
}
// The logical size of the common object directory (1 KiB of metadata per entry plus every
// file's bytes) and its entry count, refusing symlinks and special files anywhere in it.
int object_import_bytes(const char *common, uint64_t &total, uint64_t *entries = nullptr) {
    String objects = joinp(common, "objects");
    int fd = open(objects.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -errno;
    uint64_t count = 0;
    total = 0; int rc = walk_objects_fd(fd, 0, total, count); close(fd);
    if (entries) *entries = count;
    return rc;
}
int validate_lfs_files(const char *root, const String &dir, const String &rel, Vec<char> &manifest, unsigned depth) {
    if (depth > 32) return refuse(WFS_E_GIT_UNSUPPORTED, "the Git LFS object cache is nested too deeply");
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ELOOP || errno == ENOTDIR
        ? refuse(WFS_E_GIT_UNSUPPORTED, "the Git LFS cache contains a symlink or special file") : -errno;
    DIR *d = fdopendir(fd);
    if (!d) { int rc = -errno; close(fd); return rc; }
    int rc = 0;
    Vec<String> names;
    for (;;) {
        errno = 0; dirent *e = readdir(d);
        if (!e) { if (errno) rc = -errno; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        names.emplace_back(e->d_name);
    }
    closedir(d);
    if (rc) return rc;
    std::sort(names.begin(), names.end(), [](const String &a, const String &b) { return a < b; });
    for (const auto &name : names) {
        struct stat st;
        int dfd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dfd < 0) return -errno;
        if (fstatat(dfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW)) { rc = -errno; close(dfd); break; }
        close(dfd);
        String child = joinp(dir.c_str(), name.c_str());
        String child_rel = rel.empty() ? String(name.c_str()) : joinp(rel.c_str(), name.c_str());
        if (S_ISDIR(st.st_mode)) { rc = validate_lfs_files(root, child, child_rel, manifest, depth + 1); if (rc) break; continue; }
        if (!S_ISREG(st.st_mode) || st.st_size < 0) {
            rc = refuse(WFS_E_GIT_UNSUPPORTED, "the Git LFS cache contains a symlink or special file (%s)", child_rel.c_str()); break;
        }
        String file_arg("--file="); file_arg.append(child.c_str());
        Vec<char> pointer;
        if ((rc = lfs_pointer_oracle(root, file_arg.c_str(), &pointer))) {
            rc = refuse(WFS_E_GIT_UNSUPPORTED, "Git LFS cache object %s is corrupt", child_rel.c_str()); break;
        }
        char oid[65] = {0}; unsigned long long size = 0;
        if (sscanf(pointer.data(), "version https://git-lfs.github.com/spec/v1\noid sha256:%64[0-9a-f]\nsize %llu", oid, &size) != 2 ||
            strlen(oid) != 64 || size != (unsigned long long)st.st_size) {
            rc = refuse(WFS_E_GIT_UNSUPPORTED, "Git LFS cache object %s is corrupt", child_rel.c_str()); break;
        }
        String expected(oid, 2); expected.push_back('/'); expected.append(oid + 2, 2); expected.push_back('/'); expected.append(oid);
        if (expected != child_rel) {
            rc = refuse(WFS_E_GIT_UNSUPPORTED, "Git LFS cache object %s does not match its SHA-256 name", child_rel.c_str()); break;
        }
        for (char c : child_rel) manifest.emplace_back(c); manifest.emplace_back('\0');
        for (size_t i = 0; i < 64; ++i) manifest.emplace_back(oid[i]); manifest.emplace_back('\0');
        char size_text[32]; int n = snprintf(size_text, sizeof size_text, "%llu", size);
        for (int i = 0; i < n; ++i) manifest.emplace_back(size_text[i]); manifest.emplace_back('\0');
    }
    return rc;
}
int lfs_import_bytes(const char *root, const char *objects, uint64_t &total, uint64_t &entries,
                     bool &present, Vec<char> &manifest) {
    struct stat st;
    if (lstat(objects, &st)) {
        if (errno == ENOENT) { total = entries = 0; present = false; manifest.clear(); return 0; }
        return -errno;
    }
    if (!S_ISDIR(st.st_mode))
        return refuse(WFS_E_GIT_UNSUPPORTED, "the Git LFS object cache is not a real directory");
    int fd = open(objects, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -errno;
    present = true;
    total = entries = 0;
    int rc = walk_objects_fd(fd, 0, total, entries);
    close(fd);
    if (!rc) { manifest.clear(); rc = validate_lfs_files(root, String(objects), String(), manifest, 0); }
    return rc;
}
int reject_configured_policy(const char *root, const char *key) {
    const char *args[] = {"config", "--get", key, nullptr};
    Vec<char> value_bytes; int status = -1;
    int rc = git(root, args, &value_bytes, &status);
    if (!rc) return refuse(WFS_E_GIT_UNSUPPORTED, "%s is set in the repository configuration", key);
    if (rc == WFS_E_GIT_FAILED && status == 1) return 0;
    return rc;
}
// Ambient configuration (global, system and GIT_CONFIG_* command configuration) is shared by the
// source and every World on this machine: a World's Git reads the same ~/.gitconfig. Settings
// that come from it unconditionally therefore mean the same thing on both sides, and the clean
// check (require_clean_tree) evaluates the source with them, the way the user's own Git does.
// What cannot be shared is refused here with WFS_E_GIT_POLICY:
//   * GIT_ATTR_SOURCE / attr.tree: attributes read from a tree-ish instead of the worktree.
//   * status settings, URL rewrites (url.<base>.insteadOf/pushInsteadOf) or per-remote/
//     per-branch settings given as command configuration: they belong to this invocation only,
//     yet the import would otherwise record what they produce (a pinned remote URL, a remote
//     or branch setting) permanently into the World.
//   * a conditional include whose target sets status or filter settings, or a per-remote or
//     per-branch setting (remote.<name>.*, branch.<name>.*): the condition (gitdir, onbranch,
//     ...) can evaluate differently at the World's location, active or not at the source, and
//     could otherwise add a URL to a carried remote or an upstream to the World's generated
//     branch once the World is in place. Includes that only set other things (identity,
//     signing, aliases, section-wide settings like remote.pushDefault) are fine; identity is
//     pinned in the World separately (pin_identity).
// Filters are refused only when tracked files actually use a defined one (reject_used_filters).
const char *const kStatusKeys = "core\\.(excludesfile|attributesfile|autocrlf|eol|safecrlf|filemode|symlinks|"
    "ignorecase|precomposeunicode|trustctime|checkstat|ignorestat|checkroundtripencoding|usereplacerefs)";
bool is_status_or_filter_key(const char *key) {
    static const char *const keys[] = {"core.excludesfile", "core.attributesfile", "core.autocrlf", "core.eol",
        "core.safecrlf", "core.filemode", "core.symlinks", "core.ignorecase", "core.precomposeunicode",
        "core.trustctime", "core.checkstat", "core.ignorestat", "core.checkroundtripencoding",
        "core.usereplacerefs", "attr.tree", nullptr};
    for (size_t i = 0; keys[i]; ++i) if (!strcasecmp(key, keys[i])) return true;
    return !strncasecmp(key, "filter.", 7) || !strncasecmp(key, "lfs.", 4) ||
        !strcasecmp(key, "remote.lfsdefault") || !strcasecmp(key, "remote.lfspushdefault");
}
// Whether `key` names a per-remote, per-branch or per-submodule setting --
// "remote.<subsection>.<var>", "branch.<subsection>.<var>" or "submodule.<subsection>.<var>" --
// as opposed to a section-wide setting with no subsection (`remote.pushDefault`,
// `branch.autoSetupMerge`, `submodule.recurse`). A subsection is present exactly when the
// remainder after the leading "remote."/"branch." contains another '.'.
bool is_remote_or_branch_subsection_key(const char *key) {
    for (const char *prefix : {"remote.", "branch.", "submodule."}) {
        size_t plen = strlen(prefix);
        if (!strncasecmp(key, prefix, plen) && strchr(key + plen, '.')) return true;
    }
    return false;
}
String dirname_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return String(".");
    if (slash == path) return String("/");
    return String(path, (size_t)(slash - path));
}
// Resolve an include path the way Git does: "~" and "~/..." are $HOME, "~user/..." is that
// user's home directory, and a relative path is relative to the directory of the file that
// contains the directive. What cannot be resolved here with certainty (an unknown user,
// Git's "%(prefix)/" install-relative form) is reported as unresolvable, never guessed.
bool resolve_include(const char *value, const char *including_file, String &out) {
    if (value[0] == '~') {
        const char *slash = strchr(value, '/');
        size_t ulen = slash ? (size_t)(slash - value - 1) : strlen(value + 1);
        String home;
        if (!ulen) {
            const char *env = getenv("HOME");
            if (!env || !*env) return false;
            home.assign(env);
        } else {
            String user(value + 1, ulen);
            struct passwd *pw = getpwnam(user.c_str());
            if (!pw || !pw->pw_dir || !*pw->pw_dir) return false;
            home.assign(pw->pw_dir);
        }
        out = slash ? joinp(home.c_str(), slash + 1) : home;
        return true;
    }
    if (!strncmp(value, "%(", 2)) return false;
    if (value[0] == '/') { out.assign(value); return true; }
    if (!including_file) return false;
    String dir = dirname_of(including_file);
    out = joinp(dir.c_str(), value);
    return true;
}
int scan_include_target(const char *root, const char *path, const char *directive, int depth,
                        bool *sets_identity = nullptr, Vec<UrlRewriteRule> *rewrites = nullptr) {
    if (depth > 10) return refuse(WFS_E_GIT_POLICY, "configuration includes are nested more than 10 deep");
    struct stat st;
    if (stat(path, &st)) return errno == ENOENT ? 0 : -errno; // Git ignores a missing include
    const char *names[] = {"config", "--file", path, "--null", "--name-only", "--list", nullptr};
    Vec<char> listing;
    if (int rc = git(root, names, &listing)) return rc;
    if (rewrites) {
        const char *rw_args[] = {"config", "--file", path, "--null", "--get-regexp",
            "^url\\..*\\.(insteadof|pushinsteadof)$", nullptr};
        Vec<char> rw_listing; int rw_status = -1;
        int rw_rc = git(root, rw_args, &rw_listing, &rw_status);
        if (rw_rc && !(rw_rc == WFS_E_GIT_FAILED && rw_status == 1)) return rw_rc;
        if (!rw_rc) parse_rewrite_listing(rw_listing, *rewrites);
    }
    bool nested = false;
    for (size_t i = 0; i < listing.size() && listing[i];) {
        const char *key = listing.data() + i;
        if (is_status_or_filter_key(key))
            return refuse(WFS_E_GIT_POLICY, "%s includes %s, which sets %s", directive, path, key);
        if (is_remote_or_branch_subsection_key(key))
            return refuse(WFS_E_GIT_POLICY,
                "%s includes %s, which sets %s; per-remote and per-branch settings (and per-submodule ones) in a "
                "conditional include depend on where the World is placed", directive, path, key);
        if (sets_identity && (!strcasecmp(key, "user.name") || !strcasecmp(key, "user.email")))
            *sets_identity = true;
        if (!strcasecmp(key, "include.path") || (!strncasecmp(key, "includeif.", 10) &&
                                                  strlen(key) > 15 && !strcasecmp(key + strlen(key) - 5, ".path")))
            nested = true;
        i += strlen(key) + 1;
    }
    if (!nested) return 0;
    const char *paths[] = {"config", "--file", path, "--null", "--get-regexp", "^(include\\.path|includeif\\..*\\.path)$", nullptr};
    Vec<char> values; int status = -1;
    int rc = git(root, paths, &values, &status);
    if (rc == WFS_E_GIT_FAILED && status == 1) return 0;
    if (rc) return rc;
    for (size_t i = 0; i < values.size() && values[i];) {
        const char *entry = values.data() + i;
        const char *nl = strchr(entry, '\n');
        String target;
        // A nested path that cannot be resolved here cannot be scanned either, and Git may still
        // load it once the outer condition is active: refuse it, exactly like a top-level one.
        if (!nl || !resolve_include(nl + 1, path, target))
            return refuse(WFS_E_GIT_POLICY, "%s includes %s, whose include %s cannot be resolved to a file",
                          directive, path, nl ? nl + 1 : entry);
        if (int nrc = scan_include_target(root, target.c_str(), directive, depth + 1, sets_identity, rewrites)) return nrc;
        i += strlen(entry) + 1;
    }
    return 0;
}
int reject_ambient_policy(const char *root) {
    if (getenv("GIT_ATTR_SOURCE")) return refuse(WFS_E_GIT_POLICY, "GIT_ATTR_SOURCE is set in the environment");
    // GIT_CONFIG_GLOBAL/GIT_CONFIG_SYSTEM are forwarded to every ambient-config probe below
    // (ambient_config_probe=true), but a relative path is resolved from each command's -C
    // directory: the source and a copy sitting elsewhere could load different files entirely.
    for (const char *name : {"GIT_CONFIG_GLOBAL", "GIT_CONFIG_SYSTEM"}) {
        const char *value = getenv(name);
        if (value && *value && (value[0] != '/'))
            return refuse(WFS_E_GIT_POLICY,
                "%s is a relative path (%s), which Git resolves per repository; use an absolute path",
                name, value);
    }
    Vec<char> listing; int status = -1;
    {
        String pattern("^(");
        pattern.append(kStatusKeys);
        pattern.append("$|attr\\.tree$)");
        const char *args[] = {"config", "--includes", "--null", "--show-scope", "--name-only",
            "--get-regexp", pattern.c_str(), nullptr};
        int rc = git(root, args, &listing, &status, false, true);
        if (rc && !(rc == WFS_E_GIT_FAILED && status == 1)) return rc;
        if (rc) listing.clear();
    }
    // Entries are "<scope>\0<key>\0".
    for (size_t i = 0; i < listing.size() && listing[i];) {
        const char *scope = listing.data() + i;
        i += strlen(scope) + 1;
        if (i >= listing.size()) return WFS_E_GIT_FAILED;
        const char *key = listing.data() + i;
        i += strlen(key) + 1;
        if (!strcasecmp(key, "attr.tree"))
            return refuse(WFS_E_GIT_POLICY, "attr.tree is set (%s configuration)", scope);
        if (!strcmp(scope, "command"))
            return refuse(WFS_E_GIT_POLICY, "%s is set as command configuration (GIT_CONFIG_* or -c)", key);
    }
    // Command configuration (GIT_CONFIG_COUNT/GIT_CONFIG_PARAMETERS, -c) belongs to this one
    // invocation, so a URL rewrite or a per-remote/per-branch setting given that way is visible
    // to the ambient probes above during import but gone once the environment is gone: baking
    // what the import would record from it (a pinned remote URL, a remote or branch setting)
    // into the World's own configuration would leave the World carrying something the source
    // never actually kept.
    {
        Vec<char> rewrite_listing; int rw_status = -1;
        const char *rw_args[] = {"config", "--includes", "--null", "--show-scope", "--name-only",
            "--get-regexp", "^(url\\..*\\.(insteadof|pushinsteadof)|remote\\..*|branch\\..*)$", nullptr};
        int rc = git(root, rw_args, &rewrite_listing, &rw_status, false, true);
        if (rc && !(rc == WFS_E_GIT_FAILED && rw_status == 1)) return rc;
        if (rc) rewrite_listing.clear();
        for (size_t i = 0; i < rewrite_listing.size() && rewrite_listing[i];) {
            const char *scope = rewrite_listing.data() + i;
            i += strlen(scope) + 1;
            if (i >= rewrite_listing.size()) return WFS_E_GIT_FAILED;
            const char *key = rewrite_listing.data() + i;
            i += strlen(key) + 1;
            if (!strcmp(scope, "command"))
                return refuse(WFS_E_GIT_POLICY, "%s is set as command configuration (GIT_CONFIG_* or -c)", key);
        }
    }
    // A relative core.excludesFile/attributesFile is resolved from each repository's location,
    // so the source and the copy (and every World) could read different files.
    {
        Vec<char> paths; int pstatus = -1;
        const char *path_args[] = {"config", "--includes", "--null", "--show-scope", "--get-regexp",
            "^core\\.(excludesfile|attributesfile)$", nullptr};
        int prc = git(root, path_args, &paths, &pstatus, false, true);
        if (prc && !(prc == WFS_E_GIT_FAILED && pstatus == 1)) return prc;
        // Entries are "<scope>\0<key>\n<value>\0".
        for (size_t i = 0; !prc && i < paths.size() && paths[i];) {
            const char *scope = paths.data() + i;
            i += strlen(scope) + 1;
            if (i >= paths.size()) return WFS_E_GIT_FAILED;
            const char *entry = paths.data() + i;
            i += strlen(entry) + 1;
            const char *nl = strchr(entry, '\n');
            const char *val = nl ? nl + 1 : "";
            if (strcmp(scope, "global") && strcmp(scope, "system")) continue;
            if (*val && val[0] != '/' && strncmp(val, "~/", 2) && strcmp(val, "~"))
                return refuse(WFS_E_GIT_POLICY, "%.*s in %s configuration is a relative path (%s), resolved differently per repository",
                              nl ? (int)(nl - entry) : (int)strlen(entry), entry, scope, val);
        }
    }
    return scan_conditional_includes(root, nullptr);
}
// Every conditional include in the ambient configuration, active or not, followed recursively:
// refused when it sets status or filter settings; `sets_identity` reports whether any sets
// user.name or user.email (so the identity can depend on where the repository is); `rewrites`,
// when given, collects every url.<base>.insteadOf/pushInsteadOf rule found in any of their
// targets (see capture_carried_config).
int scan_conditional_includes(const char *root, bool *sets_identity, Vec<UrlRewriteRule> *rewrites) {
    if (sets_identity) *sets_identity = false;
    // Entries are "<origin>\0<key>\n<value>\0".
    Vec<char> includes; int status = -1;
    const char *inc_args[] = {"config", "--includes", "--null", "--show-origin", "--get-regexp",
        "^includeif\\..*\\.path$", nullptr};
    int rc = git(root, inc_args, &includes, &status, false, true);
    if (rc == WFS_E_GIT_FAILED && status == 1) return 0;
    if (rc) return rc;
    for (size_t i = 0; i < includes.size() && includes[i];) {
        const char *origin = includes.data() + i;
        i += strlen(origin) + 1;
        if (i >= includes.size()) return WFS_E_GIT_FAILED;
        const char *entry = includes.data() + i;
        i += strlen(entry) + 1;
        const char *nl = strchr(entry, '\n');
        if (!nl) return WFS_E_GIT_FAILED;
        String directive(entry, (size_t)(nl - entry));
        const char *file = !strncmp(origin, "file:", 5) ? origin + 5 : nullptr;
        String target;
        if (!resolve_include(nl + 1, file, target))
            return refuse(WFS_E_GIT_POLICY, "%s (%s) cannot be resolved to a file", directive.c_str(), origin);
        if (int src = scan_include_target(root, target.c_str(), directive.c_str(), 0, sets_identity, rewrites)) return src;
    }
    return 0;
}
// Refuses a `check-attr filter` result that assigns any path a defined driver.
int validate_lfs_filter(const char *root, bool &defined);
int validate_lfs_storage(const char *root, bool ambient_config_probe = true, Vec<char> *snapshot = nullptr);
int reject_filter_attrs(const char *root, const Vec<char> &attrs, const Vec<char> &defined, bool *uses_lfs) {
    // Triples "<path>\0filter\0<value>\0".
    for (size_t i = 0; i < attrs.size() && attrs[i];) {
        const char *path = attrs.data() + i; i += strlen(path) + 1;
        if (i >= attrs.size()) break;
        i += strlen(attrs.data() + i) + 1;
        if (i >= attrs.size()) break;
        const char *driver = attrs.data() + i; i += strlen(driver) + 1;
        // No shortcut for "set"/"unset"/"unspecified": check-attr prints a driver literally named
        // like that the same way, and status would run it. Only a defined driver matters.
        size_t dlen = strlen(driver);
        for (size_t k = 0; k < defined.size() && defined[k];) {
            const char *key = defined.data() + k; k += strlen(key) + 1;
            // "filter.<name>.<field>": take the name segment from the key itself and compare
            // lengths first, so a long attribute value never indexes past a short key.
            const char *dot = strrchr(key, '.');
            if (!dot || dot < key + 7 || (size_t)(dot - (key + 7)) != dlen || memcmp(key + 7, driver, dlen)) continue;
            const char *field = dot + 1;
            if (!strcmp(field, "clean") || !strcmp(field, "smudge") || !strcmp(field, "process")) {
                if (dlen == 3 && !memcmp(driver, "lfs", 3)) {
                    bool lfs_defined = false;
                    if (int rc = validate_lfs_filter(root, lfs_defined)) return rc;
                    if (!lfs_defined) return -EINVAL;
                    if (int rc = validate_lfs_storage(root)) return rc;
                    if (uses_lfs) *uses_lfs = true;
                    continue;
                }
                return refuse(WFS_E_GIT_POLICY, "tracked file %s uses the '%s' filter (e.g. Git LFS), which WorldFS does not run", path, driver);
            }
        }
    }
    return 0;
}
// Admit only Git LFS's stock filters. Running an arbitrary command under the lfs name would
// turn status, checkout, or publish into arbitrary code execution; extensions can also transform
// payloads in ways the local cache copier cannot reproduce.
int validate_lfs_filter(const char *root, bool &defined) {
    defined = false;
    const char *ext[] = {"config", "--get-regexp", "^lfs\\.extension\\.", nullptr};
    Vec<char> ext_bytes; int ext_status = -1;
    if (int rc = git(root, ext, &ext_bytes, &ext_status, false, true)) {
        if (rc != WFS_E_GIT_FAILED || ext_status != 1) return rc;
    } else return refuse(WFS_E_GIT_POLICY, "Git LFS extensions are not supported");
    String clean, smudge, process, required;
    const char *clean_args[] = {"config", "--get", "filter.lfs.clean", nullptr};
    const char *smudge_args[] = {"config", "--get", "filter.lfs.smudge", nullptr};
    const char *process_args[] = {"config", "--get", "filter.lfs.process", nullptr};
    const char *required_args[] = {"config", "--get", "--type=bool", "filter.lfs.required", nullptr};
    bool have_clean = false, have_smudge = false, have_process = false, have_required = false;
    Vec<char> b; int status = -1;
#define GET_AMBIENT(args, out, flag) do { \
    status = -1; b.clear(); int grc = git(root, args, &b, &status, false, true); \
    if (grc == WFS_E_GIT_FAILED && status == 1) { out.clear(); flag = false; } \
    else if (grc) return grc; \
    else { out.assign(b.data()); if (!out.empty() && out.back() == '\n') out.pop_back(); flag = true; } \
} while (0)
    GET_AMBIENT(clean_args, clean, have_clean);
    GET_AMBIENT(smudge_args, smudge, have_smudge);
    GET_AMBIENT(process_args, process, have_process);
    GET_AMBIENT(required_args, required, have_required);
#undef GET_AMBIENT
    defined = have_clean || have_smudge || have_process;
    if (!defined) return 0;
    bool clean_ok = clean == "git-lfs clean -- %f";
    bool smudge_ok = smudge == "git-lfs smudge -- %f" || smudge == "git-lfs smudge --skip -- %f";
    bool process_ok = process == "git-lfs filter-process" || process == "git-lfs filter-process --skip";
    if (!have_clean || !have_smudge || !have_process || !clean_ok || !smudge_ok || !process_ok ||
        (have_required && required != "true"))
        return refuse(WFS_E_GIT_POLICY, "custom or incomplete filter.lfs configuration is not supported");
    return 0;
}
// A machine-wide or repository-local stock LFS filter is enough to declare that LFS is
// supported by this repository, even when the current checkout has no LFS attributes or cache.
// Unused custom/incomplete tuples remain inert under reject_used_filters.
int canonical_lfs_setup(const char *root, bool &canonical, bool &skip_smudge, bool &skip_process) {
    canonical = false;
    skip_smudge = skip_process = false;
    String clean, smudge, process, required;
    bool have_clean = false, have_smudge = false, have_process = false, have_required = false;
    auto get = [&](const char *key, String &value, bool &present) -> int {
        String arg(key);
        const char *args[] = {"config", "--get", arg.c_str(), nullptr};
        Vec<char> bytes; int status = -1;
        int rc = git(root, args, &bytes, &status, false, true);
        if (rc == WFS_E_GIT_FAILED && status == 1) { value.clear(); present = false; return 0; }
        if (rc) return rc;
        value.assign(bytes.data());
        if (!value.empty() && value.back() == '\n') value.pop_back();
        present = true;
        return 0;
    };
    if (int rc = get("filter.lfs.clean", clean, have_clean)) return rc;
    if (int rc = get("filter.lfs.smudge", smudge, have_smudge)) return rc;
    if (int rc = get("filter.lfs.process", process, have_process)) return rc;
    const char *required_args[] = {"config", "--get", "--type=bool", "filter.lfs.required", nullptr};
    Vec<char> required_bytes; int required_status = -1;
    int required_rc = git(root, required_args, &required_bytes, &required_status, false, true);
    if (required_rc == WFS_E_GIT_FAILED && required_status == 1) have_required = false;
    else if (required_rc == WFS_E_GIT_FAILED) return 0; // An invalid dormant bool is not a stock setup.
    else if (required_rc) return required_rc;
    else { required.assign(required_bytes.data()); if (!required.empty() && required.back() == '\n') required.pop_back(); have_required = true; }
    bool clean_ok = clean == "git-lfs clean -- %f";
    bool smudge_ok = smudge == "git-lfs smudge -- %f" || smudge == "git-lfs smudge --skip -- %f";
    bool process_ok = process == "git-lfs filter-process" || process == "git-lfs filter-process --skip";
    canonical = have_clean && have_smudge && have_process && clean_ok && smudge_ok && process_ok &&
        (!have_required || required == "true");
    if (canonical) {
        skip_smudge = smudge == "git-lfs smudge --skip -- %f";
        skip_process = process == "git-lfs filter-process --skip";
    }
    return 0;
}
int validate_lfs_storage(const char *root, bool ambient_config_probe, Vec<char> *snapshot) {
    String storage; bool present = false;
    const char *args[] = {"config", "--includes", "--null", "--get", "lfs.storage", nullptr};
    Vec<char> bytes; int status = -1;
    int rc = git(root, args, &bytes, &status, false, ambient_config_probe);
    if (rc == WFS_E_GIT_FAILED && status == 1) present = false;
    else if (rc) return rc;
    else { present = true; storage.assign(bytes.data()); }
    if (snapshot) {
        snapshot->emplace_back('\2'); snapshot->emplace_back(present ? '\1' : '\0');
        for (char c : bytes) snapshot->emplace_back(c);
    }
    if (!present) return 0;
    if (storage == "lfs") return 0;
    return refuse(WFS_E_GIT_UNSUPPORTED,
        "custom lfs.storage is not supported; move its objects into the repository's local lfs cache first");
}
bool lfs_endpoint_key(const char *key) {
    if (!strcmp(key, "lfs.url") || !strcmp(key, "lfs.pushurl")) return true;
    if (strncmp(key, "remote.", 7)) return false;
    size_t n = strlen(key);
    return (n > 7 && !strcmp(key + n - 7, ".lfsurl")) ||
           (n > 11 && !strcmp(key + n - 11, ".lfspushurl"));
}
bool absolute_lfs_endpoint(const char *value) {
    if (value[0] == '/') return true;
    const char *sep = strstr(value, "://");
    if (!sep || sep == value || !isalpha((unsigned char)value[0])) return false;
    for (const char *p = value + 1; p < sep; ++p)
        if (!isalnum((unsigned char)*p) && *p != '+' && *p != '-' && *p != '.') return false;
    return true;
}
int validate_lfs_endpoint_listing(const Vec<char> &listing, Vec<char> *snapshot = nullptr) {
    if (snapshot) for (char c : listing) snapshot->emplace_back(c);
    for (size_t i = 0; i < listing.size() && listing[i];) {
        const char *entry = listing.data() + i;
        size_t n = strlen(entry); i += n + 1;
        const char *nl = strchr(entry, '\n');
        String key(entry, nl ? (size_t)(nl - entry) : n);
        if (!lfs_endpoint_key(key.c_str())) continue;
        const char *value = nl ? nl + 1 : "";
        if (!*value || strpbrk(value, "\n\r") || !absolute_lfs_endpoint(value))
            return refuse(WFS_E_GIT_UNSUPPORTED,
                "%s must be an absolute URL or absolute filesystem path for Git LFS", key.c_str());
    }
    return 0;
}
int validate_lfs_tree_config(const char *root, const char *treeish, Vec<char> *snapshot = nullptr) {
    String tree_arg(treeish);
    const char *tree_args[] = {"--no-replace-objects", "ls-tree", "-z", tree_arg.c_str(), "--", ".lfsconfig", nullptr};
    Vec<char> tree;
    if (int rc = git(root, tree_args, &tree)) return rc;
    if (tree.empty() || !tree[0]) return 0;
    const char *tab = (const char *)memchr(tree.data(), '\t', tree.size());
    if (!tab) return -EIO;
    char mode[8], oid[72];
    if (sscanf(tree.data(), "%7s blob %71s", mode, oid) != 2) return -EIO;
    if (strcmp(mode, "100644") && strcmp(mode, "100755"))
        return refuse(WFS_E_GIT_UNSUPPORTED, "%s:.lfsconfig is not a regular file", treeish);
    String blobarg("--blob="); blobarg.append(oid);
    const char *blob_args[] = {"--no-replace-objects", "config", blobarg.c_str(), "--null", "--list", nullptr};
    Vec<char> listing;
    if (int rc = git(root, blob_args, &listing)) return rc;
    if (int rc = validate_lfs_endpoint_listing(listing)) return rc;
    if (snapshot) {
        snapshot->emplace_back('\1');
        for (char c : tree) snapshot->emplace_back(c);
        for (char c : listing) snapshot->emplace_back(c);
    }
    return 0;
}
int capture_lfs_endpoint_state(const char *root, Vec<char> &state, bool ambient_config_probe) {
    state.clear();
    if (int rc = validate_lfs_storage(root, ambient_config_probe, &state)) return rc;
    const char *extension_args[] = {"config", "--includes", "--null", "--get-regexp", "^lfs\\.extension\\.", nullptr};
    Vec<char> extensions; int extension_status = -1;
    int rc = git(root, extension_args, &extensions, &extension_status, false, ambient_config_probe);
    if (rc == WFS_E_GIT_FAILED && extension_status == 1) extensions.clear();
    else if (rc) return rc;
    if (!extensions.empty()) return refuse(WFS_E_GIT_UNSUPPORTED,
        "Git LFS extensions are not supported; remove lfs.extension.* configuration");
    const char *transfer_args[] = {"config", "--includes", "--null", "--get-regexp", "^lfs\\.customtransfer\\.", nullptr};
    Vec<char> transfers; int transfer_status = -1;
    rc = git(root, transfer_args, &transfers, &transfer_status, false, ambient_config_probe);
    if (rc == WFS_E_GIT_FAILED && transfer_status == 1) transfers.clear();
    else if (rc) return rc;
    if (!transfers.empty()) return refuse(WFS_E_GIT_UNSUPPORTED,
        "custom Git LFS transfer agents are not supported; remove lfs.customtransfer.* configuration");
    const char *standalone_args[] = {"config", "--includes", "--null", "--get-all",
        "lfs.standalonetransferagent", nullptr};
    Vec<char> standalone; int standalone_status = -1;
    rc = git(root, standalone_args, &standalone, &standalone_status, false, ambient_config_probe);
    if (rc == WFS_E_GIT_FAILED && standalone_status == 1) standalone.clear();
    else if (rc) return rc;
    state.emplace_back('\3');
    for (size_t i = 0; i + 1 < standalone.size();) {
        const char *agent = standalone.data() + i;
        size_t n = strlen(agent); i += n + 1;
        if (*agent && strcmp(agent, "lfs-standalone-file"))
            return refuse(WFS_E_GIT_UNSUPPORTED,
                "custom Git LFS stand-alone transfer agents are not supported; remove lfs.standalonetransferagent");
        for (size_t j = 0; j <= n; ++j) state.emplace_back(agent[j]);
    }
    const char *effective_args[] = {"config", "--includes", "--null", "--get-regexp",
        "^(lfs\\.(url|pushurl)|remote\\..+\\.(lfsurl|lfspushurl))$", nullptr};
    Vec<char> effective; int status = -1;
    rc = git(root, effective_args, &effective, &status, false, ambient_config_probe);
    if (rc == WFS_E_GIT_FAILED && status == 1) effective.clear();
    else if (rc) return rc;
    if ((rc = validate_lfs_endpoint_listing(effective, &state))) return rc;

    // .lfsconfig is separate from Git's normal config. Git LFS reads both the working-tree
    // file and the committed file on checkout, so validate each view that import/reset can use.
    String path = joinp(root, ".lfsconfig");
    struct stat st;
    if (!lstat(path.c_str(), &st)) {
        if (!S_ISREG(st.st_mode)) return refuse(WFS_E_GIT_UNSUPPORTED, ".lfsconfig is not a regular file");
        Vec<char> bytes, listing;
        if ((rc = read_bytes(path.c_str(), bytes))) return rc;
        const char *file_args[] = {"config", "--file", path.c_str(), "--null", "--list", nullptr};
        if ((rc = git(root, file_args, &listing))) return rc;
        if ((rc = validate_lfs_endpoint_listing(listing))) return rc;
        for (char c : bytes) state.emplace_back(c);
        state.emplace_back('\0');
        for (char c : listing) state.emplace_back(c);
    } else if (errno != ENOENT) return -errno;

    // The index is a third independent view: include-changes can retain a staged .lfsconfig
    // that differs from both the worktree file and HEAD. Record the full stage listing (also
    // rechecked through source_unchanged's captured index) and validate its stage-0 blob.
    const char *index_args[] = {"ls-files", "--stage", "-z", "--", ".lfsconfig", nullptr};
    Vec<char> index_listing;
    if ((rc = git(root, index_args, &index_listing))) return rc;
    for (char c : index_listing) state.emplace_back(c);
    for (size_t i = 0; i + 1 < index_listing.size();) {
        const char *record = index_listing.data() + i;
        size_t n = strlen(record); i += n + 1;
        const char *tab = strchr(record, '\t');
        if (!tab) return -EIO;
        char mode[8], oid[72]; int stage = -1;
        if (sscanf(record, "%7s %71s %d", mode, oid, &stage) != 3) return -EIO;
        if (stage != 0)
            return refuse(WFS_E_GIT_UNSUPPORTED, "the staged .lfsconfig has unresolved index stages");
        if (strcmp(mode, "100644") && strcmp(mode, "100755"))
            return refuse(WFS_E_GIT_UNSUPPORTED, "the staged .lfsconfig is not a regular file");
        String blobarg("--blob="); blobarg.append(oid);
        const char *blob_args[] = {"config", blobarg.c_str(), "--null", "--list", nullptr};
        Vec<char> listing;
        if ((rc = git(root, blob_args, &listing))) return rc;
        if ((rc = validate_lfs_endpoint_listing(listing))) return rc;
        for (char c : listing) state.emplace_back(c);
    }

    return validate_lfs_tree_config(root, "HEAD", &state);
}
// Tracked files whose `filter` attribute names a defined driver (Git LFS, git-crypt, ...) would
// have that driver executed by status in the source and in the World; WorldFS neither runs nor
// reproduces it. A driver that is defined but unused (a machine-wide `git lfs install` in a
// repository without LFS files) or used but undefined (pointer files as plain content) is fine.
// With `tree` (HEAD before --committed-only resets the copy), the paths are the tree's and each
// is checked both against the tree's own attributes -- what checking the tree out reads, once
// it is the index -- and against the worktree's, which checkout still falls back to.
int reject_used_filters(const char *root, const char *tree = nullptr, bool *uses_lfs = nullptr) {
    if (uses_lfs) *uses_lfs = false;
    const char *defined_args[] = {"config", "--null", "--name-only", "--get-regexp", "^filter\\.", nullptr};
    Vec<char> defined; int status = -1;
    int rc = git(root, defined_args, &defined, &status, false, true);
    if (rc == WFS_E_GIT_FAILED && status == 1) return 0; // no driver anywhere: nothing can run
    if (rc) return rc;
    Vec<char> paths;
    const char *ls_args[] = {"ls-files", "-z", nullptr};
    const char *tree_args[] = {"ls-tree", "-r", "-z", "--name-only", "--full-tree", tree, nullptr};
    if ((rc = git(root, tree ? tree_args : ls_args, &paths))) return rc;
    if (paths.size() <= 1) return 0;
    String source("--source=");
    if (tree) source.append(tree);
    // Check the worktree attributes, the staged/index attributes (`--cached`), and, when
    // resetting to a tree, that tree's attributes. `--include-changes` preserves staged
    // attributes independently of both the worktree and HEAD, so omitting the index view can
    // miss a filter that the imported index will activate.
    for (int pass = 0; pass < (tree ? 3 : 2); ++pass) {
        FILE *input = tmpfile();
        if (!input) return -errno;
        if (fwrite(paths.data(), 1, paths.size() - 1, input) != paths.size() - 1 || fflush(input)) {
            int err = errno ? -errno : -EIO; fclose(input); return err;
        }
        rewind(input);
        const char *attr_args[] = {"check-attr", "--stdin", "-z", "filter", nullptr};
        const char *cached_attr_args[] = {"check-attr", "--cached", "--stdin", "-z", "filter", nullptr};
        const char *tree_attr_args[] = {"check-attr", source.c_str(), "--stdin", "-z", "filter", nullptr};
        Vec<char> attrs;
        const char *const *args = pass == 0 ? attr_args : (pass == 1 ? cached_attr_args : tree_attr_args);
        rc = git(root, args, &attrs, nullptr, false, true, fileno(input));
        fclose(input);
        if (rc) return rc;
        if ((rc = reject_filter_attrs(root, attrs, defined, uses_lfs))) return rc;
    }
    return 0;
}
int reject_external_visibility_state(const char *root, bool managed) {
    if (managed) return 0;
    for (const char *key : {"transfer.hideRefs", "uploadpack.hideRefs"})
        if (int rc = reject_configured_policy(root, key)) return rc;
    String grafts; const char *graft_args[] = {"rev-parse", "--path-format=absolute", "--git-path", "info/grafts", nullptr};
    if (int graft_rc = value(root, graft_args, grafts)) return graft_rc;
    struct stat st;
    if (!lstat(grafts.c_str(), &st)) return refuse(WFS_E_GIT_UNSUPPORTED, "info/grafts is present");
    return errno == ENOENT ? 0 : -errno;
}
int reject_inprogress(const char *root) {
    String admin;
    const char *args[] = {"rev-parse", "--absolute-git-dir", nullptr};
    if (int rc = value(root, args, admin)) return rc;
    const char *inprogress[] = {"index.lock", "HEAD.lock", "MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD",
        "BISECT_START", "MERGE_AUTOSTASH", "rebase-merge", "rebase-apply", "sequencer",
        // A conflicted `git notes merge` leaves status clean; its resumable state lives only here.
        "NOTES_MERGE_PARTIAL", "NOTES_MERGE_REF"};
    struct stat st;
    for (const char *rel : inprogress) {
        String path = joinp(admin.c_str(), rel);
        if (!lstat(path.c_str(), &st)) return -EBUSY;
        if (errno != ENOENT) return -errno;
    }
    // `git notes merge --abort` and `--commit` leave NOTES_MERGE_WORKTREE behind as an empty
    // directory; Git itself treats only a non-empty one as an unconcluded merge.
    String worktree = joinp(admin.c_str(), "NOTES_MERGE_WORKTREE");
    int fd = open(worktree.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        return errno == ENOTDIR || errno == ELOOP ? -EBUSY : -errno;
    }
    DIR *d = fdopendir(fd);
    if (!d) { int rc = -errno; close(fd); return rc; }
    int rc = 0;
    for (;;) {
        errno = 0; dirent *e = readdir(d);
        if (!e) { if (errno) rc = -errno; break; }
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { rc = -EBUSY; break; }
    }
    closedir(d); return rc;
}
// Modern partial clones are marked by remote.<name>.promisor / partialclonefilter and by
// pack-*.promisor markers; extensions.partialClone is deprecated and may be absent. A local
// mirror copies such an object database without its missing blobs, so the owned repository
// would fail permanently once remote.origin is removed and the source goes away.
int reject_promisor_remotes(const char *root) {
    String listing; bool present = false;
    const char *promisor[] = {"config", "--get-regexp", "--type=bool", "^remote\\..*\\.promisor$", nullptr};
    if (int rc = get_config(root, promisor, listing, &present)) return rc;
    if (present) {
        // Each line is "<key> <bool>"; the key is a section name and may itself contain spaces.
        size_t start = 0, n = listing.size();
        for (size_t i = 0; i <= n; ++i) {
            if (i < n && listing[i] != '\n') continue;
            if (i > start) {
                size_t sp = i;
                while (sp > start && listing[sp - 1] != ' ') --sp;
                if (i - sp == 4 && !memcmp(listing.c_str() + sp, "true", 4))
                    return refuse(WFS_E_GIT_UNSUPPORTED, "partial clone (a promisor remote)");
            }
            start = i + 1;
        }
    }
    const char *filter[] = {"config", "--get-regexp", "^remote\\..*\\.partialclonefilter$", nullptr};
    if (int rc = get_config(root, filter, listing, &present)) return rc;
    return present ? refuse(WFS_E_GIT_UNSUPPORTED, "partial clone (a partialclonefilter remote)") : 0;
}
int reject_promisor_packs(const String &common) {
    String dir = joinp(common.c_str(), "objects/pack");
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT || errno == ENOTDIR ? 0 : -errno;
    DIR *d = fdopendir(fd);
    if (!d) { int rc = -errno; close(fd); return rc; }
    int rc = 0;
    for (;;) {
        errno = 0; dirent *e = readdir(d);
        if (!e) { if (errno) rc = -errno; break; }
        size_t len = strlen(e->d_name);
        if (len > 9 && !strcmp(e->d_name + len - 9, ".promisor")) { rc = refuse(WFS_E_GIT_UNSUPPORTED, "partial clone (a promisor pack)"); break; }
    }
    closedir(d); return rc;
}
// Repository extensions change what the object database or configuration means, and a mirror
// clone does not carry them (e.g. preciousObjects forbids pruning objects the source protects;
// worktreeConfig moves settings into config.worktree). Only the ones the owned repository
// reproduces are admitted: objectFormat, which the mirror keeps, the files ref backend, and
// relativeWorktrees, which the owned repository's own relative worktree registration sets.
int reject_unsupported_extensions(const char *root) {
    const char *args[] = {"config", "--local", "--null", "--get-regexp", "^extensions\\.", nullptr};
    Vec<char> listing; int status = -1;
    int rc = git(root, args, &listing, &status);
    if (rc == WFS_E_GIT_FAILED && status == 1) return 0;
    if (rc) return rc;
    // --null prints "<key>\n<value>\0" per entry.
    size_t i = 0, n = listing.size();
    while (i < n && listing[i]) {
        size_t key = i; while (i < n && listing[i] && listing[i] != '\n') ++i;
        String name(listing.data() + key, i - key);
        String val;
        if (i < n && listing[i] == '\n') { size_t v = ++i; while (i < n && listing[i]) ++i; val.assign(listing.data() + v, i - v); }
        if (i < n) ++i;
        if (name == "extensions.objectformat" || name == "extensions.relativeworktrees") continue;
        // Admitted only with the worktree settings capture_worktree_config accepts and restores.
        if (name == "extensions.worktreeconfig") continue;
        if (name == "extensions.refstorage") {
            if (!strcasecmp(val.c_str(), "files")) continue;
            return refuse(WFS_E_GIT_UNSUPPORTED, "%s ref storage (only the files backend is supported)", val.c_str());
        }
        return refuse(WFS_E_GIT_UNSUPPORTED, "repository extension %s", name.c_str());
    }
    return 0;
}
// Repeat the same eligibility checks before publication: configuration and layout
// can change while an external mirror is being copied.
int reject_import_policy(const char *root) {
    if (int rc = reject_ambient_policy(root)) return rc;
    if (int rc = reject_used_filters(root)) return rc;
    for (const char *key : {"core.excludesFile", "core.attributesFile"})
        if (int rc = reject_configured_policy(root, key)) return rc;
    for (const char *key : {"core.sparseCheckout", "core.splitIndex"}) {
        String val; bool present = false; const char *args[] = {"config", "--get", "--type=bool", key, nullptr};
        if (int rc = get_config(root, args, val, &present)) return rc;
        if (present && val != "false") return refuse(WFS_E_GIT_UNSUPPORTED, "%s is enabled", key);
    }
    String val; bool present = false; const char *partial[] = {"config", "--get", "extensions.partialClone", nullptr};
    if (int rc = get_config(root, partial, val, &present)) return rc;
    if (present) return refuse(WFS_E_GIT_UNSUPPORTED, "partial clone (extensions.partialClone)");
    if (int rc = reject_unsupported_extensions(root)) return rc;
    if (int rc = reject_promisor_remotes(root)) return rc;
    String common; const char *common_args[] = {"rev-parse", "--path-format=absolute", "--git-common-dir", nullptr};
    if (int rc = value(root, common_args, common)) return rc;
    struct stat st;
    for (const char *rel : {"objects/info/alternates", "objects/info/http-alternates", "shallow"}) {
        String path = joinp(common.c_str(), rel);
        if (!lstat(path.c_str(), &st)) return refuse(WFS_E_GIT_UNSUPPORTED, "%s is present (alternates or shallow clone)", rel);
        if (errno != ENOENT) return -errno;
    }
    if (int rc = reject_promisor_packs(common)) return rc;
    const char *shared_args[] = {"rev-parse", "--shared-index-path", nullptr};
    if (int rc = value(root, shared_args, val)) return rc;
    return val.empty() ? 0 : refuse(WFS_E_GIT_UNSUPPORTED, "split index (a shared index file)");
}
// Learned rerere resolutions live in the common directory's rr-cache, which a mirror clone
// does not copy. Git's layout is one directory per conflict holding regular files; anything
// else (a symlinked cache, nested directories, special files) is refused rather than guessed.
// Records are "<dir>/\0" for each directory, then "<dir>/<file>\0" + 8-byte length + bytes,
// both in byte order, so two captures of an unchanged cache compare equal.
void sort_names(Vec<String> &v) {
    for (size_t i = 1; i < v.size(); ++i)
        for (size_t j = i; j > 0 && strcmp(v[j - 1].c_str(), v[j].c_str()) > 0; --j) {
            String t(v[j]); v[j] = v[j - 1]; v[j - 1] = t;
        }
}
int list_names(int fd, Vec<String> &out) {
    int dup_fd = dup(fd);
    if (dup_fd < 0) return -errno;
    DIR *d = fdopendir(dup_fd);
    if (!d) { int rc = -errno; close(dup_fd); return rc; }
    rewinddir(d);
    out.clear(); int rc = 0;
    for (;;) {
        errno = 0; dirent *e = readdir(d);
        if (!e) { if (errno) rc = -errno; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        out.emplace_back(e->d_name);
    }
    closedir(d);
    if (!rc) sort_names(out);
    return rc;
}
void append_record(Vec<char> &blob, const char *a, const char *b) {
    for (const char *p = a; *p; ++p) blob.emplace_back(*p);
    blob.emplace_back('/');
    if (b) for (const char *p = b; *p; ++p) blob.emplace_back(*p);
    blob.emplace_back('\0');
}
int capture_rerere(const char *root, Vec<char> &blob, bool &present, uint64_t &bytes) {
    blob.clear(); present = false; bytes = 0;
    // rr-cache is always in the common directory. `rev-parse --git-path rr-cache` would resolve
    // a symlinked cache to its target and hide that it lives outside the repository.
    String common;
    const char *args[] = {"rev-parse", "--path-format=absolute", "--git-common-dir", nullptr};
    if (int rc = value(root, args, common)) return rc;
    String cache = joinp(common.c_str(), "rr-cache");
    int fd = open(cache.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        return errno == ENOTDIR || errno == ELOOP ? refuse(WFS_E_GIT_UNSUPPORTED, "rr-cache is a symlink or not a directory") : -errno;
    }
    present = true;
    Vec<String> dirs;
    int rc = list_names(fd, dirs);
    for (size_t i = 0; !rc && i < dirs.size(); ++i) {
        struct stat st;
        if (fstatat(fd, dirs[i].c_str(), &st, AT_SYMLINK_NOFOLLOW)) { rc = -errno; break; }
        if (!S_ISDIR(st.st_mode)) { rc = refuse(WFS_E_GIT_UNSUPPORTED, "rr-cache/%s is not a conflict directory", dirs[i].c_str()); break; }
        int sub = openat(fd, dirs[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (sub < 0) { rc = -errno; break; }
        append_record(blob, dirs[i].c_str(), nullptr);
        Vec<String> files;
        rc = list_names(sub, files);
        for (size_t j = 0; !rc && j < files.size(); ++j) {
            if (fstatat(sub, files[j].c_str(), &st, AT_SYMLINK_NOFOLLOW)) { rc = -errno; break; }
            if (!S_ISREG(st.st_mode)) { rc = refuse(WFS_E_GIT_UNSUPPORTED, "rr-cache/%s/%s is not a regular file", dirs[i].c_str(), files[j].c_str()); break; }
            String dir_path = joinp(cache.c_str(), dirs[i].c_str());
            String path = joinp(dir_path.c_str(), files[j].c_str());
            Vec<char> data;
            if ((rc = read_bytes(path.c_str(), data))) break;
            append_record(blob, dirs[i].c_str(), files[j].c_str());
            uint64_t n = data.size();
            for (int k = 0; k < 8; ++k) blob.emplace_back((char)(n >> (8 * k)));
            for (char c : data) blob.emplace_back(c);
            bytes += n + 4096;
        }
        close(sub);
        bytes += 4096;
    }
    close(fd);
    return rc;
}
int restore_rerere(const char *repo, const Vec<char> &blob) {
    String cache = joinp(repo, "rr-cache");
    if (int rc = fs_mkdir(cache.c_str(), 0700)) return rc;
    size_t i = 0, n = blob.size();
    while (i < n) {
        size_t end = i;
        while (end < n && blob[end]) ++end;
        if (end == n || end == i) return -EIO;
        String rel(blob.data() + i, end - i);
        i = end + 1;
        String path = joinp(cache.c_str(), rel.c_str());
        if (rel.back() == '/') {
            path.pop_back();
            if (int rc = fs_mkdir(path.c_str(), 0700)) return rc;
            continue;
        }
        if (n - i < 8) return -EIO;
        uint64_t len = 0;
        for (int k = 0; k < 8; ++k) len |= (uint64_t)(unsigned char)blob[i + k] << (8 * k);
        i += 8;
        if (len > n - i) return -EIO;
        if (int rc = write_bytes(path.c_str(), blob.data() + i, (size_t)len)) return rc;
        i += (size_t)len;
    }
    return 0;
}
// Project hooks, for --with-hooks. Git runs only executable files of the hooks directory, and
// the directory is the common one even for a linked worktree. A symlinked directory or hook
// would make the World run whatever the link reaches later, possibly outside it, so it is
// refused rather than followed; `.sample` files, subdirectories, special files and files Git
// would not execute are left out. A repository-local core.hooksPath travels verbatim: a
// relative one (husky's `.husky`) names a directory of the World's own tree, and an absolute
// one is what the user opted into. Only local scope counts -- global configuration is shared.
// Every hook name Git runs (githooks(5)), for a hooks path that is the worktree root itself,
// where only these files are hooks and everything else is ordinary content.
const char *const kHookNames[] = {"applypatch-msg", "pre-applypatch", "post-applypatch", "pre-commit",
    "pre-merge-commit", "prepare-commit-msg", "commit-msg", "post-commit", "pre-rebase", "post-checkout",
    "post-merge", "pre-push", "pre-receive", "update", "proc-receive", "post-receive", "post-update",
    "reference-transaction", "push-to-checkout", "pre-auto-gc", "post-rewrite", "sendemail-validate",
    "fsmonitor-watchman", "p4-changelist", "p4-prepare-changelist", "p4-post-changelist", "p4-pre-submit",
    "post-index-change", nullptr};
bool is_hook_name(const char *name) {
    for (size_t i = 0; kHookNames[i]; ++i) if (!strcmp(name, kHookNames[i])) return true;
    return false;
}
bool is_sample(const char *name) {
    size_t n = strlen(name);
    return n >= 7 && !strcmp(name + n - 7, ".sample");
}
int hooks_dir(const char *root, String &out) {
    String common;
    const char *args[] = {"rev-parse", "--path-format=absolute", "--git-common-dir", nullptr};
    if (int rc = value(root, args, common)) return rc;
    out = joinp(common.c_str(), "hooks");
    return 0;
}
int local_hooks_path(const char *root, bool &present, String &path) {
    const char *args[] = {"config", "--local", "--includes", "--get", "core.hooksPath", nullptr};
    return get_config(root, args, path, &present);
}
// Refuses any symlink in the in-tree hooks directory `dfd` (named `rel`), recursing into its
// subdirectories without following links. At the worktree root (`root_only`) only the
// hook-named entries are hook material, and they are not recursed into.
int reject_hook_links(int dfd, const String &rel, bool root_only) {
    Vec<String> entries;
    if (int rc = list_names(dfd, entries)) return rc;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (root_only && !is_hook_name(entries[i].c_str())) continue;
        struct stat st;
        if (fstatat(dfd, entries[i].c_str(), &st, AT_SYMLINK_NOFOLLOW)) return -errno;
        String name = joinp(rel.c_str(), entries[i].c_str());
        if (S_ISLNK(st.st_mode))
            return refuse(WFS_E_GIT_UNSUPPORTED, "hook %s is a symlink; --with-hooks carries only regular files", name.c_str());
        if (root_only || !S_ISDIR(st.st_mode)) continue;
        int sub = openat(dfd, entries[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (sub < 0) return -errno;
        int rc = reject_hook_links(sub, name, false);
        close(sub);
        if (rc) return rc;
    }
    return 0;
}
// A relative core.hooksPath is resolved from the worktree root. Resolved lexically against the
// real source root: one that stays inside the tree (husky's `.husky`) becomes a normalized
// relative path, so the World uses its own copy; one that leaves it (`../shared-hooks`) becomes
// the absolute directory the source used, not re-resolved beside the World. Only leading `..`
// components collapse correctly without the filesystem (the root is already real); a `..`
// after a directory name (`link/../hooks`) would be taken through `link` by Git, which may be a
// symlink to anywhere, so it is refused rather than collapsed to a different directory.
int normalize_hooks_path(const char *root, bool present, String &path) {
    if (present && !path.empty() && path[0] != '/' && path[0] != '~') {
        bool named = false;
        for (const char *p = path.c_str(); *p;) {
            while (*p == '/') ++p;
            const char *start = p;
            while (*p && *p != '/') ++p;
            size_t n = (size_t)(p - start);
            if (n == 0 || (n == 1 && *start == '.')) continue;
            if (n == 2 && start[0] == '.' && start[1] == '.') {
                if (named)
                    return refuse(WFS_E_GIT_UNSUPPORTED, "core.hooksPath %s has a '..' after a directory name, which Git resolves through that directory (possibly a symlink); simplify it", path.c_str());
                continue;
            }
            named = true;
        }
        String real_root;
        if (int rc = fs_realpath(root, real_root)) return rc;
        String resolved = absolute_lexical(real_root.c_str(), path.c_str());
        size_t n = real_root.size();
        if (resolved == real_root) path.assign(".");
        else if (resolved.size() > n && !strncmp(resolved.c_str(), real_root.c_str(), n) && resolved[n] == '/')
            path.assign(resolved.c_str() + n + 1);
        else path = resolved;
    }
    return 0;
}
int capture_hooks(const char *root, Vec<GitHook> &out, bool &path_present, String &path) {
    out.clear();
    if (int rc = local_hooks_path(root, path_present, path)) return rc;
    if (int rc = normalize_hooks_path(root, path_present, path)) return rc;
    if (path_present) {
        // With core.hooksPath set, Git runs hooks from there and never from the default
        // directory, so that is the only one that matters. An in-tree directory travels with the
        // tree, so nothing is copied, but every component and every entry in it is checked
        // without following links: a symlinked `.husky` or hook would make the World execute a
        // mutable target outside it. An absolute or `~` directory is outside the tree and is used
        // as the source used it, which is what --with-hooks opts into.
        if (path.empty() || path[0] == '/' || path[0] == '~') return 0;
        // Administration the import replaces (.git becomes the WorldFS marker) or owns cannot
        // hold hooks that travel with the tree.
        for (const char *reserved : {".git", ".world-git", ".world"}) {
            size_t n = strlen(reserved);
            if (!strncmp(path.c_str(), reserved, n) && (path[n] == '\0' || path[n] == '/'))
                return refuse(WFS_E_GIT_UNSUPPORTED, "core.hooksPath %s is inside %s, which the import replaces; move the hooks into the tree or use an absolute path", path.c_str(), reserved);
        }
        String real_root;
        if (int rc = fs_realpath(root, real_root)) return rc;
        String walk(real_root);
        const char *p = path.c_str();
        while (*p) {
            while (*p == '/') ++p;
            const char *start = p;
            while (*p && *p != '/') ++p;
            if (p == start) break;
            String part(start, (size_t)(p - start));
            if (part == ".") continue;
            walk = joinp(walk.c_str(), part.c_str());
            struct stat st;
            if (lstat(walk.c_str(), &st)) return errno == ENOENT ? 0 : -errno;
            if (S_ISLNK(st.st_mode))
                return refuse(WFS_E_GIT_UNSUPPORTED, "core.hooksPath %s goes through a symlink (%s)", path.c_str(), walk.c_str());
            if (!S_ISDIR(st.st_mode)) return 0;
        }
        int dfd = open(walk.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dfd < 0) return errno == ENOENT ? 0 : -errno;
        // At the worktree root only the hook-named files are hooks; elsewhere the whole
        // directory, subdirectories included, is hook material (hooks commonly source their
        // neighbours and nested helpers such as husky's `_/`).
        int lrc = reject_hook_links(dfd, path, path == ".");
        close(dfd);
        return lrc;
    }
    String dir;
    if (int rc = hooks_dir(root, dir)) return rc;
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        return errno == ENOTDIR || errno == ELOOP
            ? refuse(WFS_E_GIT_UNSUPPORTED, "the hooks directory %s is a symlink or not a directory", dir.c_str()) : -errno;
    }
    Vec<String> names;
    int rc = list_names(fd, names);
    for (size_t i = 0; !rc && i < names.size(); ++i) {
        const char *name = names[i].c_str();
        if (is_sample(name)) continue;
        struct stat st;
        if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) { rc = -errno; break; }
        if (S_ISLNK(st.st_mode)) { rc = refuse(WFS_E_GIT_UNSUPPORTED, "hook %s is a symlink; --with-hooks copies only regular files", name); break; }
        if (!S_ISREG(st.st_mode) || !(st.st_mode & 0111)) continue;
        GitHook hook;
        hook.name.assign(name);
        hook.mode = (uint32_t)(st.st_mode & 0777);
        String p = joinp(dir.c_str(), name);
        if ((rc = read_bytes(p.c_str(), hook.bytes))) break;
        out.emplace_back(hook);
    }
    close(fd);
    return rc;
}
bool same_hooks(const Vec<GitHook> &a, const Vec<GitHook> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].mode != b[i].mode || !same_bytes(a[i].bytes, b[i].bytes)) return false;
    return true;
}
const char *lfs_prepush_script() {
    return "#!/bin/sh\ncommand -v git-lfs >/dev/null 2>&1 || { printf >&2 \"\\n%s\\n\\n\" \"This repository is configured for Git LFS but 'git-lfs' was not found on your path. If you no longer wish to use Git LFS, remove this hook by deleting the 'pre-push' file in the hooks directory (set by 'core.hookspath'; usually '.git/hooks').\"; exit 2; }\ngit lfs pre-push \"$@\"\n";
}
// Exact Git LFS v3.4.1 hookBaseContent with Command=pre-push and Hook.write's final LF.
const char *legacy_lfs_prepush_script() {
    return "#!/bin/sh\ncommand -v git-lfs >/dev/null 2>&1 || { echo >&2 \"\\nThis repository is configured for Git LFS but 'git-lfs' was not found on your path. If you no longer wish to use Git LFS, remove this hook by deleting the 'pre-push' file in the hooks directory (set by 'core.hookspath'; usually '.git/hooks').\\n\"; exit 2; }\ngit lfs pre-push \"$@\"\n";
}
bool canonical_lfs_prepush(const Vec<char> &bytes) {
    const char *script = lfs_prepush_script();
    if (bytes.size() == strlen(script) && !memcmp(bytes.data(), script, bytes.size())) return true;
    const char *legacy = legacy_lfs_prepush_script();
    return bytes.size() == strlen(legacy) && !memcmp(bytes.data(), legacy, bytes.size());
}
int read_lfs_prepush(const String &dir, bool allow_create, String &path) {
    path = joinp(dir.c_str(), "pre-push");
    struct stat st;
    if (lstat(path.c_str(), &st)) {
        if (errno != ENOENT) return -errno;
        return allow_create ? 0 : refuse(WFS_E_GIT_UNSUPPORTED,
            "core.hooksPath has no canonical executable Git LFS pre-push hook; add it before importing with --with-hooks");
    }
    if (!S_ISREG(st.st_mode)) return refuse(WFS_E_GIT_UNSUPPORTED, "the pre-push hook is not a regular file");
    if (!(st.st_mode & 0111)) return refuse(WFS_E_GIT_UNSUPPORTED,
        "the canonical Git LFS pre-push hook is not executable");
    Vec<char> bytes;
    if (int rc = read_bytes(path.c_str(), bytes)) return rc;
    if (!canonical_lfs_prepush(bytes))
        return refuse(WFS_E_GIT_UNSUPPORTED,
            "a custom pre-push hook conflicts with the generated Git LFS hook; keep only the canonical Git LFS hook or remove pre-push");
    return 1;
}
String relative_hooks_path(const char *root, const char *dir) {
    String from, to;
    if (fs_realpath(root, from) || fs_realpath(dir, to)) return String();
    Vec<String> a, b;
    auto split = [](const String &path, Vec<String> &parts) {
        size_t i = 0;
        while (i < path.size()) {
            while (i < path.size() && path[i] == '/') ++i;
            size_t start = i;
            while (i < path.size() && path[i] != '/') ++i;
            if (i > start) parts.emplace_back(path.c_str() + start, i - start);
        }
    };
    split(from, a); split(to, b);
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    String result;
    for (size_t j = i; j < a.size(); ++j) { if (!result.empty()) result.push_back('/'); result.append(".."); }
    for (size_t j = i; j < b.size(); ++j) { if (!result.empty()) result.push_back('/'); result.append(b[j].c_str()); }
    return result.empty() ? String(".") : result;
}
// --with-hooks may carry the LFS hook only when no pre-push hook exists or the existing file is
// exactly Git LFS's generated hook. External hook paths cannot be modified, so they must already
// contain the canonical script.
int validate_lfs_prepush_source(const char *root, const GitRepoState &s) {
    if (!s.lfs_active || !s.with_hooks) return 0;
    String dir;
    if (!s.hooks_path_present) {
        if (int rc = hooks_dir(root, dir)) return rc;
    } else if (s.hooks_path.empty() || s.hooks_path == ".") {
        if (s.hooks_path == ".") dir = root;
        else return refuse(WFS_E_GIT_UNSUPPORTED, "an empty core.hooksPath cannot host the Git LFS pre-push hook");
    } else if (s.hooks_path[0] == '/' || s.hooks_path[0] == '~') {
        return refuse(WFS_E_GIT_UNSUPPORTED,
            "external core.hooksPath %s cannot be carried with Git LFS; use a path inside the repository", s.hooks_path.c_str());
    } else {
        dir = joinp(root, s.hooks_path.c_str());
    }
    String path;
    // Explicit in-tree hooks paths are part of the user's worktree. Never create the
    // generated hook there: require the canonical executable hook to already exist.
    int rc = read_lfs_prepush(dir, !s.hooks_path_present, path);
    if (rc < 0) return rc;
    return 0;
}
int install_lfs_prepush(const GitRepoState &s, const char *worktree, const char *repo) {
    if (!s.lfs_active) return 0;
    if (s.with_hooks && s.hooks_path_present && (s.hooks_path.empty() || s.hooks_path[0] == '/' || s.hooks_path[0] == '~')) return 0;
    if (s.with_hooks && s.hooks_path_present && s.hooks_path[0] != '/' && s.hooks_path[0] != '~') {
        String path;
        String dir = s.hooks_path == "." ? String(worktree) : joinp(worktree, s.hooks_path.c_str());
        int exists = read_lfs_prepush(dir, false, path);
        return exists < 0 ? exists : 0;
    }
    String dir;
    if (s.with_hooks && s.hooks_path_present) dir = joinp(worktree, s.hooks_path.c_str());
    else dir = joinp(repo, "hooks");
    if (int rc = fs_mkdir(dir.c_str(), 0755)) { if (rc != -EEXIST) return rc; }
    String path;
    int exists = read_lfs_prepush(dir, true, path);
    if (exists < 0) return exists;
    if (!exists) {
        const char *script = lfs_prepush_script();
        if (int rc = write_bytes(path.c_str(), script, strlen(script))) return rc;
        if (chmod(path.c_str(), 0755)) return -errno;
    }
    if (!s.with_hooks || !s.hooks_path_present) {
        String relative = relative_hooks_path(worktree, dir.c_str());
        if (relative.empty()) return -EINVAL;
        if (int rc = config(worktree, "core.hooksPath", relative.c_str())) return rc;
    }
    return 0;
}
int install_hooks(const GitRepoState &s, const char *clone, const char *repo) {
    if (!s.hooks.empty()) {
        String dir = joinp(repo, "hooks");
        if (int rc = fs_mkdir(dir.c_str(), 0755)) { if (rc != -EEXIST) return rc; }
        for (const auto &hook : s.hooks) {
            String p = joinp(dir.c_str(), hook.name.c_str());
            if (int rc = write_bytes(p.c_str(), hook.bytes.data(), hook.bytes.size())) return rc;
            if (chmod(p.c_str(), (mode_t)hook.mode)) return -errno;
        }
    }
    int rc = s.hooks_path_present ? config(clone, "core.hooksPath", s.hooks_path.c_str()) : 0;
    if (!rc) rc = install_lfs_prepush(s, clone, repo);
    return rc;
}
int configure_lfs_filter(const GitRepoState &s, const char *cwd, const char *repo) {
    const char *storage[] = {"--git-dir", repo, "config", "lfs.storage", "lfs", nullptr};
    const char *clean[] = {"--git-dir", repo, "config", "filter.lfs.clean", "git-lfs clean -- %f", nullptr};
    const char *smudge[] = {"--git-dir", repo, "config", "filter.lfs.smudge",
        s.lfs_skip_smudge ? "git-lfs smudge --skip -- %f" : "git-lfs smudge -- %f", nullptr};
    const char *process[] = {"--git-dir", repo, "config", "filter.lfs.process",
        s.lfs_skip_process ? "git-lfs filter-process --skip" : "git-lfs filter-process", nullptr};
    const char *required[] = {"--git-dir", repo, "config", "filter.lfs.required", "true", nullptr};
    if (int rc = git(cwd, storage)) return rc;
    if (int rc = git(cwd, clean)) return rc;
    if (int rc = git(cwd, smudge)) return rc;
    if (int rc = git(cwd, process)) return rc;
    return git(cwd, required);
}
int activate_managed_target_lfs(const GitRepoState &s, const char *worktree, const char *repo) {
    bool present = false; String hooks;
    if (int rc = local_hooks_path(worktree, present, hooks)) return rc;
    bool use_admin = !present;
    if (present) {
        if (hooks.empty() || hooks[0] == '~')
            return refuse(WFS_E_GIT_UNSUPPORTED, "managed Git LFS cannot safely install a pre-push hook for core.hooksPath %s", hooks.c_str());
        String expected;
        if (int rc = hooks_dir(worktree, expected)) return rc;
        String expected_relative = relative_hooks_path(worktree, expected.c_str());
        if (hooks == expected || (!expected_relative.empty() && hooks == expected_relative)) {
            use_admin = true;
        } else {
            String normalized(hooks);
            if (int rc = normalize_hooks_path(worktree, present, normalized)) return rc;
            String dir = normalized[0] == '/' ? normalized :
                (normalized == "." ? String(worktree) : joinp(worktree, normalized.c_str()));
            String path;
            int hook = read_lfs_prepush(dir, false, path);
            if (hook < 0) return hook;
        }
    }
    if (int rc = configure_lfs_filter(s, worktree, repo)) return rc;
    if (use_admin) {
        GitRepoState generated = s;
        generated.lfs_active = true;
        generated.with_hooks = false;
        generated.hooks_path_present = false;
        generated.hooks_path.clear();
        return install_lfs_prepush(generated, worktree, repo);
    }
    return 0;
}
// GIT_OPTIONAL_LOCKS=0 (set by git()) keeps status from refreshing the index it inspects.
// --ignore-submodules=dirty: a submodule counts as changed when its HEAD differs from the gitlink
// the index records (read in-process from its refs), but status does not run a child `git
// status` inside it -- that child would read the submodule's own configuration, filters
// included, before anything checked them. Each submodule's own worktree is checked by a direct
// call on it instead (require_clean_copy, git_source). The command-line value also overrides
// submodule.<name>.ignore and diff.ignoreSubmodules, so neither can hide a moved submodule.
// The checkouts of the repository's other linked worktrees inside the tree (`worktrees`) are not
// part of it -- status in the main checkout lists one that is not ignored as an untracked
// directory -- so exactly those paths are excluded.
int require_clean_tree(const char *root, const Vec<GitWorktree> *worktrees = nullptr) {
    Vec<String> excludes;
    if (worktrees) for (const auto &w : *worktrees) {
        if (w.rel.empty()) continue;
        String exclude(":(exclude,top,literal)");
        exclude.append(w.rel.c_str());
        excludes.emplace_back(exclude);
    }
    Vec<const char *> status_args;
    for (const char *a : {"status", "--porcelain=v1", "-z", "--untracked-files=normal", "--ignore-submodules=dirty",
                          "--", ".", ":(exclude).world", ":(exclude).world-git"})
        status_args.emplace_back(a);
    for (const auto &e : excludes) status_args.emplace_back(e.c_str());
    status_args.emplace_back(nullptr);
    Vec<char> dirty;
    // With the user's own global/system configuration (global ignores, autocrlf, ...), so
    // "clean" means what `git status` in the source and in the World both say.
    bool uses_lfs = false;
    if (int rc = reject_used_filters(root, nullptr, &uses_lfs)) return rc;
    int rc = 0;
    if (uses_lfs) {
        char scratch[] = "/tmp/worldfs-lfs-status-XXXXXX";
        if (!mkdtemp(scratch)) return -errno;
        String storage(scratch); storage.append("/storage");
        String storage_config("lfs.storage="); storage_config.append(storage.c_str());
        Vec<const char *> args;
        for (const char *a : {"-c", "filter.lfs.process=git-lfs filter-process --skip", "-c",
                              "filter.lfs.smudge=git-lfs smudge --skip -- %f", "-c"})
            args.emplace_back(a);
        args.emplace_back(storage_config.c_str());
        for (const char *const *a = status_args.data(); *a; ++a) args.emplace_back(*a);
        args.emplace_back(nullptr);
        rc = git(root, args.data(), &dirty, nullptr, false, true);
        int cleanup = fs_remove_tree(scratch);
        if (!rc && cleanup) rc = cleanup;
    } else {
        rc = git(root, status_args.data(), &dirty, nullptr, false, true);
    }
    if (rc) return rc;
    return dirty.size() > 1 ? WFS_E_GIT_DIRTY : 0;
}
// --committed-only: make the copy's Git-visible content exactly HEAD. Only the copy is touched;
// the source was captured read-only and is rechecked unchanged before this runs. The index is
// refreshed first (the copy's inodes and ctimes differ from the ones it records), so the reset
// rewrites only files whose content differs from HEAD and the rest stay clones of the source's
// blocks. `clean` without -x removes untracked, non-ignored files and keeps ignored build/data
// artifacts and the reserved administration; `read-tree --reset -u` then puts back modified and
// deleted tracked files and drops files that were only staged. They run with the user's ambient
// configuration, like the clean check, so "ignored" and "clean" mean what the user's Git says.
// No filter can run, and hooks are off. The source-side check saw only the source's worktree and
// index attributes, which a dirty `.gitattributes` can differ from HEAD's, so filter use is
// checked here first, before the refresh (which cleans stat-dirty files) or the reset (which
// checks HEAD out), against the copy's index and worktree and against HEAD.
// SQUASH_MSG describes staged content that no longer exists, so it goes too.
// A submodule is reset to `target`, the commit its committed superproject records: HEAD is
// detached there first when it is anywhere else, and the filters are checked against it.
int reset_to_head(const char *clone, const char *target = nullptr) {
    const char *tree = target ? target : "HEAD";
    bool lfs_index = false, lfs_tree = false;
    if (int rc = reject_used_filters(clone, nullptr, &lfs_index)) return rc;
    if (int rc = reject_used_filters(clone, tree, &lfs_tree)) return rc;
    // skip-worktree and assume-unchanged entries are invisible to status and left alone by
    // read-tree, so their worktree bytes would survive the reset. Clear both marks in the
    // copy's index first (`ls-files -v`: 'S'/'s' is skip-worktree, a lower-case tag is
    // assume-unchanged); the index is about to become exactly HEAD anyway. Each mark is cleared
    // by its own update-index call with the paths as arguments: the per-path options neither
    // apply to --stdin paths nor combine in one call.
    int rc = 0;
    {
        Vec<char> listing;
        const char *ls[] = {"ls-files", "-v", "-z", nullptr};
        if ((rc = git(clone, ls, &listing))) return rc;
        Vec<String> skip, assume;
        for (size_t i = 0; i + 1 < listing.size();) {
            const char *entry = listing.data() + i;
            size_t len = strlen(entry);
            i += len + 1;
            if (len < 3 || entry[1] != ' ') continue;
            char tag = entry[0];
            if (tag == 'S' || tag == 's') skip.emplace_back(entry + 2);
            if (tag >= 'a' && tag <= 'z') assume.emplace_back(entry + 2);
        }
        struct Pass { const char *flag; Vec<String> *paths; } passes[] = {
            {"--no-skip-worktree", &skip}, {"--no-assume-unchanged", &assume}};
        for (auto &pass : passes) {
            for (size_t at = 0; at < pass.paths->size();) {
                Vec<const char *> args;
                args.emplace_back("update-index");
                args.emplace_back(pass.flag);
                args.emplace_back("--");
                for (size_t n = 0; n < 256 && at < pass.paths->size(); ++n, ++at)
                    args.emplace_back((*pass.paths)[at].c_str());
                args.emplace_back(nullptr);
                if ((rc = git(clone, args.data()))) return rc;
            }
        }
    }
    const char *refresh[] = {"update-index", "-q", "--refresh", nullptr};
    int status = -1;
    rc = git(clone, refresh, nullptr, &status, true, true);
    if (rc && !(rc == WFS_E_GIT_FAILED && status == 1)) return rc;
    const char *clean[] = {"clean", "-f", "-d", "-q", "--", ".", ":(exclude,top,literal).world",
                           ":(exclude,top,literal).world-git", nullptr};
    if ((rc = git(clone, clean, nullptr, nullptr, false, true))) return rc;
    if (target) {
        String head;
        const char *head_args[] = {"rev-parse", "--verify", "HEAD^{commit}", nullptr};
        if ((rc = value(clone, head_args, head))) return rc;
        if (head != target) {
            const char *detach[] = {"update-ref", "--no-deref", "-m", "world: --committed-only", "HEAD", target, nullptr};
            if ((rc = git(clone, detach))) return rc;
        }
    }
    // Git LFS's process filter can download during checkout. Ask its canonical filter to leave
    // pointers in place, then hydrate only from the copied local cache with `lfs checkout`.
    const char *reset[] = {"read-tree", "--reset", "-u", "HEAD", nullptr};
    if ((rc = git(clone, reset, nullptr, nullptr, false, true, -1, true))) return rc;
    if (lfs_index || lfs_tree) {
        const char *checkout[] = {"lfs", "checkout", nullptr};
        if ((rc = git(clone, checkout, nullptr, nullptr, false, true))) return rc;
    }
    // A directory the reset emptied of staged additions, or anything else left untracked.
    if ((rc = git(clone, clean, nullptr, nullptr, false, true))) return rc;
    String squash;
    const char *squash_args[] = {"rev-parse", "--path-format=absolute", "--git-path", "SQUASH_MSG", nullptr};
    if ((rc = value(clone, squash_args, squash))) return rc;
    if (unlink(squash.c_str()) && errno != ENOENT) return -errno;
    return 0;
}
// `for-each-ref` (and therefore the mirror) silently omits a symbolic ref whose target does
// not exist, e.g. after `git symbolic-ref refs/heads/alias refs/heads/future`, so the captured
// map cannot be treated as complete on its own. Symbolic refs are only ever loose files in the
// files backend, so every `ref: ` file under refs/ must be one the enumeration returned.
// Reftable offers no read-only way to list them, so that backend is refused.
int scan_loose_symrefs(int dirfd, const String &prefix, const Vec<GitSymref> &known, int depth) {
    if (depth > 64) return refuse(WFS_E_GIT_UNSUPPORTED, "the refs directory is nested too deeply");
    int dup_fd = dup(dirfd);
    if (dup_fd < 0) return -errno;
    DIR *d = fdopendir(dup_fd);
    if (!d) { int rc = -errno; close(dup_fd); return rc; }
    rewinddir(d);
    int rc = 0;
    for (;;) {
        errno = 0; dirent *e = readdir(d);
        if (!e) { if (errno) rc = -errno; break; }
        const char *name = e->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        String ref = prefix; ref.push_back('/');
        for (const char *p = name; *p; ++p) ref.push_back(*p);
        struct stat st;
        if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW)) { rc = -errno; break; }
        if (S_ISDIR(st.st_mode)) {
            int sub = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (sub < 0) { rc = -errno; break; }
            rc = scan_loose_symrefs(sub, ref, known, depth + 1);
            close(sub);
            if (rc) break;
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        int fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) { rc = -errno; break; }
        char head[5]; ssize_t n = read(fd, head, sizeof head);
        close(fd);
        if (n < 0) { rc = -errno; break; }
        if (n < 5 || memcmp(head, "ref: ", 5)) continue;
        bool listed = false;
        for (const auto &sym : known) if (sym.name == ref) { listed = true; break; }
        if (!listed) { rc = refuse(WFS_E_GIT_UNSUPPORTED, "symbolic ref %s points at a ref that does not exist", ref.c_str()); break; }
    }
    closedir(d);
    return rc;
}
int reject_unlisted_symrefs(const char *root, const Vec<GitSymref> &known) {
    String format;
    const char *format_args[] = {"rev-parse", "--show-ref-format", nullptr};
    if (int rc = value(root, format_args, format)) return rc;
    if (format != "files") return refuse(WFS_E_GIT_UNSUPPORTED, "%s ref storage (only the files backend is supported)", format.c_str());
    String common, admin;
    const char *common_args[] = {"rev-parse", "--path-format=absolute", "--git-common-dir", nullptr};
    const char *admin_args[] = {"rev-parse", "--absolute-git-dir", nullptr};
    if (int rc = value(root, common_args, common)) return rc;
    if (int rc = value(root, admin_args, admin)) return rc;
    for (const String *dir : {&common, &admin}) {
        if (dir == &admin && admin == common) break;
        String refs = joinp(dir->c_str(), "refs");
        int fd = open(refs.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) { if (errno == ENOENT) continue; return -errno; }
        String prefix("refs");
        int rc = scan_loose_symrefs(fd, prefix, known, 0);
        close(fd);
        if (rc) return rc;
    }
    return 0;
}
int source_unchanged(const GitRepoState &s) {
    if (int rc = reject_inprogress(s.root.c_str())) return rc;
    if (int rc = reject_import_policy(s.root.c_str())) return rc;
    if (int rc = reject_external_visibility_state(s.root.c_str(), s.managed)) return rc;
    String head; const char *args[] = {"rev-parse", "--verify", "HEAD^{commit}", nullptr};
    if (int rc = value(s.root.c_str(), args, head)) return rc;
    Vec<char> index;
    int rc = read_bytes(s.index_path.c_str(), index);
    if (rc == -ENOENT && s.index.empty()) rc = 0;
    if (rc) return rc;
    if (head != s.head || index.size() != s.index.size() ||
        (!index.empty() && memcmp(index.data(), s.index.data(), index.size()))) return -EBUSY;
    String head_ref, admin;
    const char *ref_args[] = {"symbolic-ref", "--quiet", "HEAD", nullptr};
    const char *admin_args[] = {"rev-parse", "--absolute-git-dir", nullptr};
    if (int ref_rc = value(s.root.c_str(), ref_args, head_ref, true)) return ref_rc;
    if (int admin_rc = value(s.root.c_str(), admin_args, admin)) return admin_rc;
    if (head_ref != s.head_ref || admin != s.admin) return -EBUSY;
    Vec<char> exclude;
    rc = read_bytes(s.exclude_path.c_str(), exclude);
    if (rc == -ENOENT && s.exclude.empty()) rc = 0;
    if (rc) return rc;
    if (!same_bytes(exclude, s.exclude)) return -EBUSY;
    Vec<char> attributes;
    rc = read_bytes(s.attributes_path.c_str(), attributes);
    if (rc == -ENOENT && s.attributes.empty()) rc = 0;
    if (rc) return rc;
    if (!same_bytes(attributes, s.attributes)) return -EBUSY;
    Vec<char> sparse; int sparse_rc = read_bytes(s.sparse_path.c_str(), sparse);
    if (sparse_rc != 0 && sparse_rc != -ENOENT) return sparse_rc;
    if ((sparse_rc == 0) != s.sparse_present || !same_bytes(sparse, s.sparse)) return -EBUSY;
    Vec<char> squash; int squash_rc = read_bytes(s.squash_path.c_str(), squash);
    bool squash_present = squash_rc == 0;
    if (squash_rc != 0 && squash_rc != -ENOENT) return squash_rc;
    if (squash_present != s.squash_present || !same_bytes(squash, s.squash)) return -EBUSY;
    Vec<char> fetch;
    int fetch_rc = read_bytes(s.fetch_path.c_str(), fetch);
    if (fetch_rc != 0 && fetch_rc != -ENOENT) return fetch_rc;
    if ((fetch_rc == 0) != s.fetch_present || !same_bytes(fetch, s.fetch)) return -EBUSY;
    Vec<GitSetting> settings;
    if (int setting_rc = capture_settings(s.root.c_str(), settings)) return setting_rc;
    if (!same_settings(settings, s.settings)) return -EBUSY;
    Vec<GitSetting> identity;
    if (int identity_rc = capture_identity(s.root.c_str(), identity)) return identity_rc;
    if (!same_settings(identity, s.identity)) return -EBUSY;
    bool worktree_config = false; Vec<GitSetting> worktree_settings;
    if (int wt_rc = capture_worktree_config(s.root.c_str(), worktree_config, worktree_settings)) return wt_rc;
    if (worktree_config != s.worktree_config || !same_settings(worktree_settings, s.worktree_settings)) return -EBUSY;
    if (!s.managed) {
        Vec<GitSetting> carried;
        if (int carry_rc = capture_carried_config(s.root.c_str(), carried)) return carry_rc;
        if (!same_settings(carried, s.carried)) return -EBUSY;
    }
    if (s.with_hooks && !s.managed) {
        Vec<GitHook> hooks; bool path_present = false; String path;
        if (int hook_rc = capture_hooks(s.root.c_str(), hooks, path_present, path)) return hook_rc;
        if (!same_hooks(hooks, s.hooks) || path_present != s.hooks_path_present || path != s.hooks_path) return -EBUSY;
    }
    Vec<char> direct_refs; if (int ref_rc = capture_refs(s.root.c_str(), direct_refs)) return ref_rc;
    if (!same_bytes(direct_refs, s.refs)) return -EBUSY;
    // The stash stack as a whole: refs/stash is in the refs above, the entries below it only here.
    Vec<char> stash; int stash_rc = read_bytes(s.stash_path.c_str(), stash);
    if (stash_rc != 0 && stash_rc != -ENOENT) return stash_rc;
    if ((stash_rc == 0) != s.stash_present || !same_bytes(stash, s.stash)) return -EBUSY;
    bool orig_present; String orig; if (int orig_rc = capture_orig(s.root.c_str(), orig_present, orig)) return orig_rc;
    if (orig_present != s.orig_present || orig != s.orig_head) return -EBUSY;
    if (!s.managed) {
        String common;
        const char *a[] = {"rev-parse", "--path-format=absolute", "--git-common-dir", nullptr};
        if (int rc = value(s.root.c_str(), a, common)) return rc;
        uint64_t bytes = 0;
        if (int rc = object_import_bytes(common.c_str(), bytes)) return rc;
        uint64_t lfs_bytes = 0, lfs_entries = 0; bool lfs_present = false; Vec<char> lfs_manifest;
        if (!s.lfs_objects.empty())
            if (int rc = lfs_import_bytes(s.root.c_str(), s.lfs_objects.c_str(), lfs_bytes, lfs_entries,
                                          lfs_present, lfs_manifest)) return rc;
        if (lfs_present != s.lfs_present || lfs_bytes != s.lfs_bytes || lfs_entries != s.lfs_entries ||
            !same_bytes(lfs_manifest, s.lfs_manifest) || bytes + s.rerere_bytes + lfs_bytes != s.import_bytes) return -EBUSY;
        Vec<char> rerere; bool rerere_present = false; uint64_t rerere_bytes = 0;
        if (int rc = capture_rerere(s.root.c_str(), rerere, rerere_present, rerere_bytes)) return rc;
        if (rerere_present != s.rerere_present || !same_bytes(rerere, s.rerere)) return -EBUSY;
    }
    Vec<char> endpoint_state;
    if (int rc = capture_lfs_endpoint_state(s.root.c_str(), endpoint_state, s.lfs_active)) return rc;
    if (!same_bytes(endpoint_state, s.lfs_endpoint_state)) return -EBUSY;
    bool lfs_setup = false, skip_smudge = false, skip_process = false;
    if (int rc = canonical_lfs_setup(s.root.c_str(), lfs_setup, skip_smudge, skip_process)) return rc;
    if (lfs_setup != s.lfs_filter_setup || skip_smudge != s.lfs_skip_smudge ||
        skip_process != s.lfs_skip_process) return -EBUSY;
    Vec<GitSymref> refs;
    if (int symrc = collect_symrefs(s.root.c_str(), refs)) return symrc;
    if (!same_symrefs(refs, s.symrefs)) return -EBUSY;
    return reject_unlisted_symrefs(s.root.c_str(), refs);
}
}

// WorldFS owns the root `.world` file and `.world-git` directory: snapshots drop the copied
// `.world`, forks write metadata there, and info/exclude cannot hide a tracked path. A path
// tracked in the index would fork dirty or commit metadata; one anywhere in preserved history
// would overwrite the World marker on an ordinary checkout (ignored files are overwritten by
// default). So the index and every commit reachable from what the import keeps -- all refs,
// HEAD, ORIG_HEAD and FETCH_HEAD tips and every stash entry -- must never contain either path. --full-history keeps
// a side branch that added the path and was merged away from being simplified out.
// --no-replace-objects makes the walk read the commits and trees the import actually preserves:
// a refs/replace/* entry could otherwise present a safe tree for a commit whose real tree has
// the path, and deleting that replacement in the World would bring it back. The replacement
// commits are still scanned, as ordinary tips under --all.
int reject_reserved_paths(const char *root, const GitRepoState &s) {
    const char *tracked_args[] = {"ls-files", "-z", "--", ":(top,literal).world", ":(top,literal).world-git", nullptr};
    Vec<char> tracked;
    if (int rc = git(root, tracked_args, &tracked)) return rc;
    if (tracked.size() > 1) return refuse(WFS_E_GIT_UNSUPPORTED, "the index tracks the reserved path .world or .world-git");
    Vec<String> tips;
    tips.emplace_back(s.head.c_str());
    if (s.orig_present) tips.emplace_back(s.orig_head.c_str());
    if (s.fetch_present) {
        // FETCH_HEAD lines start with an object ID followed by a tab.
        size_t start = 0, n = s.fetch.size();
        for (size_t i = 0; i <= n; ++i) {
            if (i < n && s.fetch[i] != '\n') continue;
            size_t hex = start;
            while (hex < i && ((s.fetch[hex] >= '0' && s.fetch[hex] <= '9') || (s.fetch[hex] >= 'a' && s.fetch[hex] <= 'f'))) ++hex;
            if (hex < i && s.fetch[hex] == '\t' && (hex - start == 40 || hex - start == 64))
                tips.emplace_back(s.fetch.data() + start, hex - start);
            else if (i > start) return refuse(WFS_E_GIT_UNSUPPORTED, "FETCH_HEAD could not be parsed");
            start = i + 1;
        }
    }
    // Every stash entry, not only refs/stash: `git stash apply stash@{n}` writes its worktree,
    // index and untracked-files trees (the entry commit and its second and third parents).
    Vec<String> stash;
    if (int rc = stash_tips(s.stash, stash)) return rc;
    for (const auto &tip : stash) tips.emplace_back(tip.c_str());
    Vec<const char *> args;
    for (const char *a : {"--no-replace-objects", "rev-list", "-n", "1", "--full-history", "--all"}) args.emplace_back(a);
    for (const auto &tip : tips) args.emplace_back(tip.c_str());
    for (const char *a : {"--", ":(top,literal).world", ":(top,literal).world-git"}) args.emplace_back(a);
    args.emplace_back(nullptr);
    Vec<char> hit;
    if (int rc = git(root, args.data(), &hit)) return rc;
    return hit.size() > 1 ? refuse(WFS_E_GIT_UNSUPPORTED, "a preserved commit tracks the reserved path .world or .world-git") : 0;
}
// --committed-only resets the copy to HEAD, which removes an in-tree hooks directory that is
// only staged or untracked; the World would then point core.hooksPath at nothing and silently
// skip its hooks. Require a relative (normalized) hooks path to be in HEAD with nothing pending.
int require_committed_hooks_path(const char *root, bool present, const String &hp) {
    if (!present || hp.empty() || hp[0] == '/' || hp[0] == '~') return 0;
    bool at_root = hp == ".";
    if (!at_root) {
        // `HEAD:<path>` takes everything after the colon as the path, so the type is read
        // with cat-file rather than peeled with ^{tree}.
        String spec("HEAD:"); spec.append(hp.c_str());
        const char *type_args[] = {"cat-file", "-t", spec.c_str(), nullptr};
        Vec<char> type; int tstatus = -1;
        int trc = git(root, type_args, &type, &tstatus, true);
        if (trc || strcmp(type.data(), "tree\n"))
            return refuse(WFS_E_GIT_UNSUPPORTED, "core.hooksPath %s is not committed, so --committed-only would leave the hooks out; commit it or drop --committed-only", hp.c_str());
    }
    // The directory being in HEAD is not enough: a hook inside it that is staged,
    // untracked or modified would be reset away just the same. At the worktree root only
    // the hook-named files are hooks, so only those are checked there.
    Vec<String> scopes;
    if (at_root) {
        for (size_t k = 0; kHookNames[k]; ++k) {
            String one(":(top,literal)"); one.append(kHookNames[k]); scopes.emplace_back(one);
        }
    } else {
        String one(":(top,literal)"); one.append(hp.c_str()); scopes.emplace_back(one);
    }
    Vec<const char *> st_args;
    for (const char *a : {"status", "--porcelain=v1", "-z", "--untracked-files=all", "--ignored=no", "--ignore-submodules=dirty", "--"})
        st_args.emplace_back(a);
    for (const auto &scope : scopes) st_args.emplace_back(scope.c_str());
    st_args.emplace_back(nullptr);
    Vec<char> pending;
    bool uses_lfs = false;
    if (int src = reject_used_filters(root, nullptr, &uses_lfs)) return src;
    int status_rc = 0;
    if (uses_lfs) {
        // This source-side status probe must not let Git LFS populate the source cache (or
        // install/update hooks) while checking only whether the copied hooks path is clean.
        char scratch[] = "/tmp/worldfs-lfs-hooks-XXXXXX";
        if (!mkdtemp(scratch)) return -errno;
        String storage(scratch); storage.append("/storage");
        String storage_config("lfs.storage="); storage_config.append(storage.c_str());
        Vec<const char *> safe_args;
        for (const char *a : {"-c", "filter.lfs.process=git-lfs filter-process --skip", "-c",
                              "filter.lfs.smudge=git-lfs smudge --skip -- %f", "-c", storage_config.c_str()})
            safe_args.emplace_back(a);
        for (size_t i = 0; st_args[i]; ++i) safe_args.emplace_back(st_args[i]);
        safe_args.emplace_back(nullptr);
        status_rc = git(root, safe_args.data(), &pending, nullptr, false, true);
        int cleanup = fs_remove_tree(scratch);
        if (!status_rc && cleanup) status_rc = cleanup;
    } else {
        status_rc = git(root, st_args.data(), &pending, nullptr, false, true);
    }
    if (status_rc) return status_rc;
    if (pending.size() > 1)
        return refuse(WFS_E_GIT_UNSUPPORTED, "core.hooksPath %s has uncommitted changes, which --committed-only would drop; commit them or drop --committed-only", hp.c_str());
    // Status cannot see edits to a hook marked skip-worktree or assume-unchanged, and the reset
    // clears those marks and restores the committed bytes (reset_to_head), so any such hook is
    // refused: `ls-files -v` tags skip-worktree with 'S'/'s', assume-unchanged in lower case.
    st_args.clear();
    for (const char *a : {"ls-files", "-v", "-z", "--"}) st_args.emplace_back(a);
    for (const auto &scope : scopes) st_args.emplace_back(scope.c_str());
    st_args.emplace_back(nullptr);
    Vec<char> listing;
    if (int lrc = git(root, st_args.data(), &listing)) return lrc;
    for (size_t i = 0; i + 1 < listing.size();) {
        const char *entry = listing.data() + i;
        size_t len = strlen(entry);
        i += len + 1;
        if (len < 3 || entry[1] != ' ') continue;
        if (entry[0] == 'S' || (entry[0] >= 'a' && entry[0] <= 'z'))
            return refuse(WFS_E_GIT_UNSUPPORTED, "hook %s is marked skip-worktree or assume-unchanged, so --committed-only would reset it to its committed version unseen; clear the mark or drop --committed-only", entry + 2);
    }
    return 0;
}
// Parses the gitlinks out of `ls-files --stage -z` ("<mode> <oid> <stage>\t<path>") or
// `ls-tree -r -z` ("<mode> <type> <oid>\t<path>") output.
void parse_gitlinks(const Vec<char> &listing, Vec<Gitlink> &out) {
    out.clear();
    for (size_t i = 0; i + 1 < listing.size();) {
        const char *entry = listing.data() + i;
        i += strlen(entry) + 1;
        if (strncmp(entry, "160000 ", 7)) continue;
        const char *tab = strchr(entry, '\t');
        if (!tab) continue;
        const char *oid = entry + 7;
        if (!strncmp(oid, "commit ", 7)) oid += 7;
        const char *end = strchr(oid, ' ');
        if (!end || end > tab) end = tab;
        out.emplace_back(Gitlink{String(tab + 1), String(oid, (size_t)(end - oid))});
    }
}
int tree_gitlinks(const char *root, const char *tree, Vec<Gitlink> &out) {
    const char *args[] = {"ls-tree", "-r", "-z", "--full-tree", tree, nullptr};
    Vec<char> listing;
    if (int rc = git(root, args, &listing)) return rc;
    parse_gitlinks(listing, out);
    return 0;
}
// Every setting of the .gitmodules that is published: the worktree's file, or with `tree`
// (--committed-only) that commit's. The listing is `config --null --list` output
// ("<key>\n<value>\0" entries). A missing file is an empty mapping -- never the index's or
// HEAD's copy, which Git itself may fall back to but which the published tree does not hold.
int gitmodules_listing(const char *root, const char *tree, Vec<char> &listing) {
    listing.clear();
    String file = joinp(root, ".gitmodules"), blob;
    const char *source_flag = "--blob";
    if (tree) {
        blob.assign(tree); blob.append(":.gitmodules");
        const char *exists[] = {"cat-file", "-e", blob.c_str(), nullptr};
        if (git(root, exists, nullptr, nullptr, true)) return 0;
    } else {
        struct stat st;
        if (!lstat(file.c_str(), &st)) {
            if (!S_ISREG(st.st_mode)) return refuse(WFS_E_GIT_UNSUPPORTED, ".gitmodules is not a regular file");
            source_flag = "--file";
            blob = file;
        } else {
            return errno == ENOENT ? 0 : -errno;
        }
    }
    const char *args[] = {"config", source_flag, blob.c_str(), "--null", "--list", nullptr};
    return git(root, args, &listing);
}
// The value of `submodule.<name>.<var>` in a .gitmodules listing; the last one wins, like Git.
const char *gitmodules_value(const Vec<char> &listing, const String &name, const char *var) {
    String key("submodule."); key.append(name.c_str()); key.push_back('.'); key.append(var); key.push_back('\n');
    const char *found = nullptr;
    for (size_t i = 0; i < listing.size() && listing[i];) {
        const char *entry = listing.data() + i;
        i += strlen(entry) + 1;
        if (!strncmp(entry, key.c_str(), key.size())) found = entry + key.size();
    }
    return found;
}
// The submodule name .gitmodules gives each path, from a listing: pairs are {path, name}.
void module_names(const Vec<char> &listing, Vec<GitSetting> &out) {
    out.clear();
    for (size_t i = 0; i < listing.size() && listing[i];) {
        const char *entry = listing.data() + i;
        size_t len = strlen(entry);
        i += len + 1;
        const char *nl = strchr(entry, '\n');
        if (!nl || nl - entry < 16 || strncmp(entry, "submodule.", 10) || strncmp(nl - 5, ".path", 5)) continue;
        String name(entry + 10, (size_t)(nl - entry) - 15);
        out.emplace_back(GitSetting{String(nl + 1), name});
    }
}
const String *module_name(const Vec<GitSetting> &names, const String &path) {
    const String *found = nullptr;
    for (const auto &n : names) if (n.key == path) found = &n.value;   // the last one wins, like Git
    return found;
}
// `git submodule init`'s resolution of a "./" or "../" .gitmodules URL against a remote's URL,
// observed with Git 2.54 across local, file://, ssh://, https:// and scp-like bases: trailing
// slashes of the base are dropped; each leading "../" removes the base's last "/"-component and
// each leading "./" is skipped; the rest is joined with "/", and one trailing "/" of the result
// is dropped. Git also chops at a ':' of an scp-like base, falls back to "." and yields
// relative or malformed URLs (ssh:/x.git) once the components run out: those cases, an empty
// rest, and a relative base are not reproduced -- false, and the caller refuses.
bool resolve_submodule_url(const String &base_url, const char *url, String &out) {
    String base(base_url);
    while (!base.empty() && base.back() == '/') base.pop_back();
    const char *b = base.c_str();
    const char *scheme = strstr(b, "://");
    size_t floor;   // no "../" may cut below this: the root, the host, or the scp host
    if (b[0] == '/') {
        floor = 0;
    } else if (scheme) {
        const char *slash = strchr(scheme + 3, '/');
        if (!slash) return false;
        floor = (size_t)(slash - b);
    } else {
        const char *colon = strchr(b, ':'), *slash = strchr(b, '/');
        if (!colon || (slash && slash < colon)) return false;
        floor = (size_t)(colon - b) + 1;
    }
    const char *p = url;
    for (;;) {
        if (!strncmp(p, "../", 3)) {
            p += 3;
            const char *last = strrchr(base.c_str(), '/');
            if (!last || (size_t)(last - base.c_str()) < floor) return false;
            base.resize((size_t)(last - base.c_str()));
        } else if (!strncmp(p, "./", 2)) {
            p += 2;
        } else {
            break;
        }
    }
    if (!*p) return false;
    out = base; out.push_back('/'); out.append(p);
    if (out.back() == '/') out.pop_back();
    return true;
}
// Whether the first `n` bytes of `a` and `b` are equal under ASCII case folding (other bytes,
// UTF-8 included, compared exactly): submodule names are directories below modules/, and a
// World may live on, or be forked onto, a case-insensitive volume where `Lib` and `lib` are one
// directory. Independent of the locale.
bool ascii_caseeq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}
// A name is a directory below modules/: no empty, "." or ".." component, nothing absolute.
bool valid_module_name(const String &name) {
    if (name.empty() || name[0] == '/' || strpbrk(name.c_str(), "\\\n\r")) return false;
    for (const char *p = name.c_str();;) {
        const char *start = p;
        while (*p && *p != '/') ++p;
        size_t n = (size_t)(p - start);
        if (n == 0 || (n == 1 && *start == '.') || (n == 2 && start[0] == '.' && start[1] == '.')) return false;
        if (!*p) return true;
        ++p;
    }
}
// `to` relative to the directory `from`, both relative to the same root and normalized, the
// way Git spells its relative submodule links: the common leading components are dropped.
String rel_link(const String &from, const String &to) {
    Vec<String> a, b;
    for (const String *src : {&from, &to}) {
        Vec<String> &parts = src == &from ? a : b;
        for (const char *p = src->c_str(); *p;) {
            while (*p == '/') ++p;
            const char *start = p;
            while (*p && *p != '/') ++p;
            if (p != start) parts.emplace_back(start, (size_t)(p - start));
        }
    }
    size_t common = 0;
    while (common < a.size() && common < b.size() && a[common] == b[common]) ++common;
    String out;
    for (size_t i = common; i < a.size(); ++i) out.append("../");
    for (size_t i = common; i < b.size(); ++i) { out.append(b[i].c_str()); if (i + 1 < b.size()) out.push_back('/'); }
    if (out.empty()) out.assign(".");
    else if (out.back() == '/') out.pop_back();
    return out;
}
constexpr const char *kActive = ".world-git/repo.git/worktrees/active";
// The `.git` file of a submodule at `path` whose repository is `gitdir` (see GitModule).
String module_gitfile(const String &path, const String &gitdir) {
    String admin(kActive); admin.push_back('/'); admin.append(gitdir.c_str());
    String text("gitdir: "); text.append(rel_link(path, admin).c_str()); text.push_back('\n');
    return text;
}
String module_worktree(const String &path, const String &gitdir) {
    String admin(kActive); admin.push_back('/'); admin.append(gitdir.c_str());
    return rel_link(admin, path);
}
// Git 2.54 can spell a relative link with a doubled slash; compare the normalized spelling.
String squeeze_slashes(const char *text, size_t n) {
    String out;
    for (size_t i = 0; i < n; ++i) {
        if (text[i] == '/' && !out.empty() && out.back() == '/') continue;
        out.push_back(text[i]);
    }
    return out;
}
// A managed World's submodule at `path` (relative to the World's root `world`) is trusted only
// in the World's own layout, exactly: every component of its path a real directory, its `.git`
// a regular file with the relative link to the owned repository `gitdir` (see GitModule), and
// that repository's core.worktree the relative link back. Anything else -- a symlinked
// directory, a gitfile repointed at some other repository -- would have Git read or write a
// repository that is not the World's.
int check_owned_module(const char *world, const String &path, const String &gitdir) {
    String walk(world);
    for (const char *p = path.c_str(); *p;) {
        const char *start = p;
        while (*p && *p != '/') ++p;
        walk = joinp(walk.c_str(), String(start, (size_t)(p - start)).c_str());
        if (*p) ++p;
        struct stat st;
        if (lstat(walk.c_str(), &st) || !S_ISDIR(st.st_mode))
            return refuse(WFS_E_GIT_UNSUPPORTED, "submodule path %s is not a directory of the World", path.c_str());
    }
    String dot = joinp(walk.c_str(), ".git");
    struct stat st;
    Vec<char> bytes;
    if (lstat(dot.c_str(), &st) || !S_ISREG(st.st_mode) || read_bytes(dot.c_str(), bytes) ||
        squeeze_slashes(bytes.data(), bytes.size()) != module_gitfile(path, gitdir))
        return refuse(WFS_E_GIT_UNSUPPORTED, "the .git link of submodule %s was changed", path.c_str());
    String admin(kActive); admin.push_back('/'); admin.append(gitdir.c_str()); admin.append("/config");
    String config = joinp(world, admin.c_str()), worktree;
    const char *wt_args[] = {"config", "--file", config.c_str(), "--get", "core.worktree", nullptr};
    if (int rc = get_config(world, wt_args, worktree)) return rc;
    if (squeeze_slashes(worktree.c_str(), worktree.size()) != module_worktree(path, gitdir))
        return refuse(WFS_E_GIT_UNSUPPORTED, "the core.worktree link of submodule %s was changed", path.c_str());
    return 0;
}
// Prefix the reason of a refusal that came from inside a submodule with its path.
int in_module(int rc, const String &path) {
    if (rc != WFS_E_GIT_UNSUPPORTED && rc != WFS_E_GIT_POLICY) return rc;
    char inner[sizeof g_reason];
    snprintf(inner, sizeof inner, "%s", g_reason);
    if (!strncmp(inner, "submodule ", 10)) return rc;   // a nested one already named itself
    return refuse(rc, "submodule %s: %s", path.c_str(), inner);
}
// A managed World is copied with byte-identical configuration (the effective-configuration
// comparison relies on it), so nothing in it is absolutized the way an external import makes a
// relative remote or submodule URL absolute or pins a relative core.hooksPath that leaves the
// tree. A value set by hand inside the World that Git resolves from the repository's location
// would name a different place in the copy: refused. `git submodule init`, `git remote add`
// with an absolute path and the import itself write absolute values; an in-tree relative
// core.hooksPath travels with the tree and is fine.
int reject_relative_managed_paths(const char *root, bool allow_missing_lfs_hook = false) {
    const char *args[] = {"config", "--local", "--includes", "--null", "--get-regexp",
        "^remote\\..*\\.(url|pushurl)$", nullptr};
    Vec<char> listing; int status = -1;
    int rc = git(root, args, &listing, &status);
    if (rc && !(rc == WFS_E_GIT_FAILED && status == 1)) return rc;
    for (size_t i = 0; !rc && i < listing.size() && listing[i];) {
        const char *entry = listing.data() + i;
        i += strlen(entry) + 1;
        const char *nl = strchr(entry, '\n');
        if (!nl || !is_relative_local_url(nl + 1)) continue;
        return refuse(WFS_E_GIT_POLICY,
            "the World's %.*s is the relative path %s, which a copy of the World would resolve from its "
            "own location; make it absolute (git submodule init and the import write absolute URLs)",
            (int)(nl - entry), entry, nl + 1);
    }
    bool present = false; String hooks;
    if ((rc = local_hooks_path(root, present, hooks))) return rc;
    if (present && !hooks.empty() && hooks[0] != '/' && hooks[0] != '~') {
        String normalized(hooks);
        if ((rc = normalize_hooks_path(root, present, normalized))) return rc;
        if (normalized[0] == '/') {
            // The generated LFS hook for an owned submodule lives in that submodule's common
            // Git directory, which is deliberately under the World's root .world-git rather
            // than under the submodule worktree. Admit only this exact managed admin/hooks
            // destination with the canonical executable pre-push hook; arbitrary paths that
            // escape the submodule still resolve differently after a World copy and are refused.
            String expected, expected_real, resolved;
            bool owned_lfs_hook = false;
            if (!hooks_dir(root, expected) && !fs_realpath(expected.c_str(), expected_real)) {
                String expected_relative = relative_hooks_path(root, expected.c_str());
                if (!expected_relative.empty() && hooks == expected_relative &&
                    !fs_realpath(normalized.c_str(), resolved) && expected_real == resolved) {
                    String prepush;
                    int hook_rc = read_lfs_prepush(expected, false, prepush);
                    if (hook_rc == 1) owned_lfs_hook = true;
                    else if (allow_missing_lfs_hook) {
                        struct stat st;
                        owned_lfs_hook = lstat(prepush.c_str(), &st) && errno == ENOENT;
                    }
                }
            }
            if (!owned_lfs_hook)
                return refuse(WFS_E_GIT_POLICY,
                    "the World's core.hooksPath %s leaves its tree, so a copy of the World would resolve it "
                    "from its own location; make it absolute", hooks.c_str());
        }
    }
    return 0;
}
// Everything the import reproduces of one repository -- the root, or a submodule's -- and every
// eligibility check on it. `gitlinks` receives the index's gitlinks.
int capture_repo(const char *root, GitRepoState &out, bool with_hooks, bool module, Vec<Gitlink> &gitlinks,
                 const char *target = nullptr) {
    out.root = root;
    String top;
    const char *top_args[] = {"rev-parse", "--show-toplevel", nullptr};
    if (int rc = value(root, top_args, top)) return rc;
    String real_top, real_root;
    if (fs_realpath(root, real_root) || fs_realpath(top.c_str(), real_top) || real_root != real_top)
        return refuse(WFS_E_GIT_UNSUPPORTED, "the directory is not the top level of its Git repository");
    if (int policy_rc = reject_import_policy(root)) return policy_rc;
    if (int policy_rc = capture_settings(root, out.settings)) return policy_rc;
    if (int identity_rc = capture_identity(root, out.identity)) return identity_rc;
    if (int wt_rc = capture_worktree_config(root, out.worktree_config, out.worktree_settings)) return wt_rc;
    if (!out.managed) {
        if (int carry_rc = capture_carried_config(root, out.carried)) return carry_rc;
    }
    // A managed World's hooks and core.hooksPath are already part of the .world-git it clones.
    out.with_hooks = with_hooks;
    if (with_hooks && !out.managed) {
        if (int hook_rc = capture_hooks(root, out.hooks, out.hooks_path_present, out.hooks_path)) return hook_rc;
    }
    if (out.managed) {
        // A managed copy has byte-identical configuration files and import commands ignore
        // global/system scope, so the effective list can only differ through an include that
        // resolves differently from the copy's location (hooksPath, identity, anything).
        const char *list[] = {"config", "--list", "--includes", "--null", nullptr};
        if (int list_rc = git(root, list, &out.effective_config)) return list_rc;
    }
    const char *head_args[] = {"rev-parse", "--verify", "HEAD^{commit}", nullptr};
    if (value(root, head_args, out.head)) return refuse(WFS_E_GIT_UNSUPPORTED, "the repository has no commit yet");
    bool lfs_worktree = false, lfs_head = false, lfs_target = false, lfs_setup = false;
    bool lfs_skip_smudge = false, lfs_skip_process = false;
    int lfs_rc = reject_used_filters(root, nullptr, &lfs_worktree);
    if (!lfs_rc) lfs_rc = reject_used_filters(root, out.head.c_str(), &lfs_head);
    if (!lfs_rc && target && strcmp(target, out.head.c_str()))
        lfs_rc = reject_used_filters(root, target, &lfs_target);
    if (!lfs_rc) lfs_rc = canonical_lfs_setup(root, lfs_setup, lfs_skip_smudge, lfs_skip_process);
    if (lfs_rc) return lfs_rc;
    out.lfs_filter_setup = lfs_setup;
    out.lfs_skip_smudge = lfs_skip_smudge;
    out.lfs_skip_process = lfs_skip_process;
    out.lfs_active = lfs_worktree || lfs_head || lfs_target || lfs_setup;
    const char *head_ref_args[] = {"symbolic-ref", "--quiet", "HEAD", nullptr};
    if (int rc = value(root, head_ref_args, out.head_ref, true)) return rc;
    const char *index_args[] = {"rev-parse", "--path-format=absolute", "--git-path", "index", nullptr};
    if (int rc = value(root, index_args, out.index_path)) return rc;
    int rc = read_bytes(out.index_path.c_str(), out.index);
    if (rc && rc != -ENOENT) return rc;
    const char *exclude_args[] = {"rev-parse", "--path-format=absolute", "--git-path", "info/exclude", nullptr};
    if ((rc = value(root, exclude_args, out.exclude_path))) return rc;
    rc = read_bytes(out.exclude_path.c_str(), out.exclude);
    if (rc && rc != -ENOENT) return rc;
    const char *attributes_args[] = {"rev-parse", "--path-format=absolute", "--git-path", "info/attributes", nullptr};
    if ((rc = value(root, attributes_args, out.attributes_path))) return rc;
    rc = read_bytes(out.attributes_path.c_str(), out.attributes);
    if (rc && rc != -ENOENT) return rc;
    // core.sparseCheckout=true is refused above; a disabled one may still keep its patterns.
    const char *sparse_args[] = {"rev-parse", "--path-format=absolute", "--git-path", "info/sparse-checkout", nullptr};
    if ((rc = value(root, sparse_args, out.sparse_path))) return rc;
    rc = read_bytes(out.sparse_path.c_str(), out.sparse);
    if (!rc) out.sparse_present = true;
    else if (rc != -ENOENT) return rc;
    const char *squash_args[] = {"rev-parse", "--path-format=absolute", "--git-path", "SQUASH_MSG", nullptr};
    if ((rc = value(root, squash_args, out.squash_path))) return rc;
    rc = read_bytes(out.squash_path.c_str(), out.squash);
    if (!rc) out.squash_present = true;
    else if (rc != -ENOENT) return rc;
    const char *fetch_args[] = {"rev-parse", "--path-format=absolute", "--git-path", "FETCH_HEAD", nullptr};
    if ((rc = value(root, fetch_args, out.fetch_path))) return rc;
    rc = read_bytes(out.fetch_path.c_str(), out.fetch);
    out.fetch_present = rc == 0;
    if (rc && rc != -ENOENT) return rc;
    String common;
    const char *common_args[] = {"rev-parse", "--path-format=absolute", "--git-common-dir", nullptr};
    const char *admin_args[] = {"rev-parse", "--absolute-git-dir", nullptr};
    if ((rc = value(root, common_args, common)) || (rc = value(root, admin_args, out.admin))) return rc;
    if (!out.managed) {
        if ((rc = object_import_bytes(common.c_str(), out.import_bytes, &out.object_entries))) return rc;
        out.objects = joinp(common.c_str(), "objects");
    }
    out.lfs_objects = joinp(common.c_str(), "lfs/objects");
    if ((rc = lfs_import_bytes(root, out.lfs_objects.c_str(), out.lfs_bytes, out.lfs_entries,
                               out.lfs_present, out.lfs_manifest))) return rc;
    // Preserve an existing local cache even when current HEAD no longer tracks LFS paths: an
    // older branch, stash, or later checkout may still need its payloads.
    if (out.lfs_present) {
        out.lfs_active = true;
    }
    out.lfs_target_only = lfs_target && !(lfs_worktree || lfs_head || out.lfs_present);
    if ((rc = capture_lfs_endpoint_state(root, out.lfs_endpoint_state, out.lfs_active))) return rc;
    if (target && strcmp(target, out.head.c_str()))
        if ((rc = validate_lfs_tree_config(root, target))) return rc;
    if (out.managed) {
        if (int relative_rc = reject_relative_managed_paths(root, out.lfs_target_only)) return relative_rc;
    }
    if ((rc = validate_lfs_prepush_source(root, out))) return rc;
    if (!out.managed) {
        if (UINT64_MAX - out.import_bytes < out.lfs_bytes) return -EOVERFLOW;
        out.import_bytes += out.lfs_bytes;
    }
    if (module) {
        // A submodule's repository is copied as one directory; a linked worktree of some other
        // repository standing in for it is not what Git itself would create there.
        String real_common, real_admin;
        if (fs_realpath(common.c_str(), real_common) || fs_realpath(out.admin.c_str(), real_admin) || real_common != real_admin)
            return refuse(WFS_E_GIT_UNSUPPORTED, "its repository is a linked worktree of another repository");
    } else {
        out.common = common;
        if (out.managed) rc = managed_check(root, common.c_str(), out.admin.c_str(), out.worktrees);
        else rc = collect_worktrees(root, common.c_str(), out.admin.c_str(), false, out.worktrees);
        if (rc) return rc;
    }
    if ((rc = reject_external_visibility_state(root, out.managed))) return rc;
    if ((rc = capture_refs(root, out.refs))) return rc;
    if ((rc = capture_stash(root, out))) return rc;
    if ((rc = capture_orig(root, out.orig_present, out.orig_head))) return rc;
    if ((rc = collect_symrefs(root, out.symrefs))) return rc;
    if ((rc = reject_unlisted_symrefs(root, out.symrefs))) return rc;
    if ((rc = reject_inprogress(root))) return rc;
    if (!out.managed) {
        if ((rc = capture_rerere(root, out.rerere, out.rerere_present, out.rerere_bytes))) return rc;
        out.import_bytes += out.rerere_bytes;
    }
    // Unmerged entries are not a baseline; gitlinks are the submodules discover_modules takes.
    const char *ls_args[] = {"ls-files", "--stage", "-z", nullptr};
    Vec<char> listing;
    if ((rc = git(root, ls_args, &listing))) return rc;
    for (size_t i = 0; i + 1 < listing.size();) {
        size_t end = i;
        while (end < listing.size() && listing[end]) ++end;
        size_t tab = i;
        while (tab < end && listing[tab] != '\t') ++tab;
        if (tab < end && tab - i >= 2 && listing[tab - 1] >= '1' && listing[tab - 1] <= '3' && listing[tab - 2] == ' ')
            return -EBUSY;
        i += strlen(listing.data() + i) + 1;
    }
    parse_gitlinks(listing, gitlinks);
    return reject_reserved_paths(root, out);
}
// The committed-only hooks check for one repository whose copy is reset to `target`: a relative
// core.hooksPath (the carried one of an external source, or the managed World's own) must be
// committed there with nothing pending. require_committed_hooks_path reads HEAD and the
// worktree, so a submodule reset to a different commit than its HEAD cannot keep one at all.
int committed_hooks_check(const char *root, const GitRepoState &s, const char *target) {
    bool present = s.hooks_path_present; String hp = s.hooks_path;
    if (s.managed) {
        if (int rc = local_hooks_path(root, present, hp)) return rc;
        // A target-only LFS module gets its generated pre-push hook in its owned Git
        // administration after the copy is verified. Its exact generated relative path is
        // outside the module worktree, so it need not be committed in the target tree.
        if (s.lfs_target_only && target && s.head != target && present) {
            String expected;
            if (int rc = hooks_dir(root, expected)) return rc;
            String expected_relative = relative_hooks_path(root, expected.c_str());
            if (hp == expected || (!expected_relative.empty() && hp == expected_relative)) {
                String prepush;
                int hook = read_lfs_prepush(expected, false, prepush);
                if (hook == 1) return 0;
                if (hook < 0) {
                    struct stat st;
                    if (lstat(prepush.c_str(), &st) && errno == ENOENT) return 0;
                    return hook;
                }
            }
        }
        if (int rc = normalize_hooks_path(root, present, hp)) return rc;
    } else if (!s.with_hooks) {
        return 0;
    }
    if (target && s.head != target && present && !hp.empty() && hp[0] != '/' && hp[0] != '~')
        return refuse(WFS_E_GIT_UNSUPPORTED, "core.hooksPath %s is inside the submodule, whose HEAD is not the commit --committed-only resets it to; drop --committed-only", hp.c_str());
    return require_committed_hooks_path(root, present, hp);
}
// The configuration a repository will have in the World, as Git there reads it. An external
// source's own is what the import carries; a managed World's travels whole with its cloned
// administration, so it is its local configuration with its includes. The World's Git also
// reads the shared global and system configuration (command configuration belongs to one
// invocation, and remote/branch settings given that way are refused by reject_ambient_policy).
// Git reads system, then global, then the repository's own, and the last value wins, so the
// shared entries come first; `shared` counts them.
int world_config(const char *repo_root, const GitRepoState &state, Vec<GitSetting> &carried, size_t &shared) {
    carried.clear();
    shared = 0;
    {
        const char *list[] = {"config", "--includes", "--null", "--show-scope", "--list", nullptr};
        Vec<char> all; int status = -1;
        int rc = git(repo_root, list, &all, &status, false, true);
        if (rc && !(rc == WFS_E_GIT_FAILED && status == 1)) return rc;
        // Entries are "<scope>\0<key>\n<value>\0".
        for (size_t i = 0; !rc && i < all.size() && all[i];) {
            const char *scope = all.data() + i;
            i += strlen(scope) + 1;
            if (i >= all.size()) return WFS_E_GIT_FAILED;
            const char *entry = all.data() + i;
            i += strlen(entry) + 1;
            if (strcmp(scope, "system") && strcmp(scope, "global")) continue;
            const char *nl = strchr(entry, '\n');
            carried.emplace_back(GitSetting{String(entry, nl ? (size_t)(nl - entry) : strlen(entry)), String(nl ? nl + 1 : "")});
            ++shared;
        }
    }
    if (!state.managed) {
        for (const auto &c : state.carried) carried.emplace_back(c);
        return 0;
    }
    const char *list[] = {"config", "--local", "--includes", "--null", "--list", nullptr};
    Vec<char> local;
    if (int rc = git(repo_root, list, &local)) return rc;
    for (size_t i = 0; i < local.size() && local[i];) {
        const char *entry = local.data() + i;
        i += strlen(entry) + 1;
        const char *nl = strchr(entry, '\n');
        carried.emplace_back(GitSetting{String(entry, nl ? (size_t)(nl - entry) : strlen(entry)), String(nl ? nl + 1 : "")});
    }
    return 0;
}
// Every submodule.<name>.url the World's configuration holds -- for a current gitlink or a
// dormant one another branch uses -- classified as Git in the World sees it: the winning value
// (the repository's own after the shared ones). A carried value was made absolute by the import
// (capture_carried_config); a managed World's is copied byte for byte and a shared one is used
// as it is, so a relative one of either is refused. Every URL then goes through the same
// conditional-include rewrite guard as carried remote URLs.
int check_configured_submodule_urls(const char *repo_root, const Vec<GitSetting> &carried, size_t shared) {
    Vec<UrlRewriteRule> conditional_rules;
    bool rules_loaded = false;
    for (size_t i = 0; i < carried.size(); ++i) {
        const String &key = carried[i].key;
        size_t n = key.size();
        if (n < 15 || strncmp(key.c_str(), "submodule.", 10) || strcmp(key.c_str() + n - 4, ".url")) continue;
        bool later = false;
        for (size_t j = i + 1; j < carried.size(); ++j) if (carried[j].key == key) later = true;
        if (later) continue;   // not the winning value
        const String &url = carried[i].value;
        if (is_relative_local_url(url.c_str())) {
            if (i < shared)
                return refuse(WFS_E_GIT_POLICY,
                    "%s is the relative path %s in global or system configuration, which Git resolves "
                    "from the worktree and so differently in the World; make it absolute", key.c_str(), url.c_str());
            return refuse(WFS_E_GIT_POLICY,
                "the World's %s is the relative path %s, which a copy of the World would resolve from its "
                "own location; make it absolute (git submodule init and the import write absolute URLs)",
                key.c_str(), url.c_str());
        }
        if (!rules_loaded) {
            if (int rc = scan_conditional_includes(repo_root, nullptr, &conditional_rules)) return rc;
            rules_loaded = true;
        }
        const UrlRewriteRule *r = matching_rewrite(conditional_rules, url.c_str());
        if (r)
            return refuse(WFS_E_GIT_POLICY,
                "%s %s matches url.%s.%s, which a conditional include can change at the World's location; "
                "simplify the URL rewrite rules before importing",
                key.c_str(), url.c_str(), r->base.c_str(), r->push ? "pushInsteadOf" : "insteadOf");
    }
    return 0;
}
// Submodules are at most this many levels deep below the root.
constexpr int kMaxModuleDepth = 8;
// Every initialized submodule of the repository at `repo_root` (the root, or a submodule found
// before it), recursively, appended to `top.modules` parents first. `prefix` is repo_root's path
// in the tree and `gitdir_prefix` its repository below the root's administration ("" for the
// root). An uninitialized submodule -- a gitlink whose directory has no `.git` -- stays as the
// source has it: its gitlink and directory, and the superproject's submodule.<name>.* settings.
// `tree`, for --committed-only, is the commit this repository's copy is reset to: each
// submodule is then reset to the commit that tree records for it.
int discover_modules(GitSource &top, const char *repo_root, const String &prefix, const String &gitdir_prefix,
                     const Vec<Gitlink> &gitlinks, const char *tree, int depth, size_t self) {
    if (gitlinks.empty()) return 0;
    GitRepoState &state = self == (size_t)-1 ? top : top.modules[self].repo;
    // Copied: `state` may move once submodules are appended below.
    Vec<GitSetting> carried;
    size_t shared = 0;
    if (int rc = world_config(repo_root, state, carried, shared)) return rc;
    Vec<char> listing, tree_listing;
    if (int rc = gitmodules_listing(repo_root, nullptr, listing)) return rc;
    Vec<GitSetting> names, tree_names;
    module_names(listing, names);
    Vec<Gitlink> tree_links;
    if (tree) {
        // Resetting the copy to `tree` would turn a submodule the index adds into a nested
        // repository, and leave a removed one's gitlink behind: the submodule set must be
        // committed as it is.
        if (int rc = tree_gitlinks(repo_root, tree, tree_links)) return rc;
        bool same = tree_links.size() == gitlinks.size();
        for (size_t i = 0; same && i < gitlinks.size(); ++i) same = find_link(tree_links, gitlinks[i].path.c_str()) != nullptr;
        if (!same)
            return refuse(WFS_E_GIT_UNSUPPORTED, "a submodule is added or removed without being committed, which --committed-only cannot reset; commit it or drop --committed-only");
        if (int rc = gitmodules_listing(repo_root, tree, tree_listing)) return rc;
        module_names(tree_listing, tree_names);
    }
    // The .gitmodules the copy will publish -- the worktree's, or with --committed-only the
    // committed one -- is the one source of truth for every check below (names, collisions,
    // relative URLs) and for the owned repositories' names, and is rechecked as a whole against
    // the copy before publication (same_gitmodules). The worktree's is consulted only to
    // require that an initialized submodule's source repository sits under that same name.
    const Vec<char> &published = tree ? tree_listing : listing;
    const Vec<GitSetting> &published_names = tree ? tree_names : names;
    state.gitmodules = published;
    state.gitmodules_checked = true;
    // A "./" or "../" URL that only .gitmodules gives (no submodule.<name>.url in the
    // configuration: an uninitialized submodule, typically) is resolved by `git submodule init`
    // against the URL of the repository's default remote, and against the repository's own
    // directory when that remote has no URL. Observed with Git 2.54: the default remote is
    // branch.<current>.remote when HEAD is on a branch that has one (even when origin also
    // exists, and even when that remote is not configured -- then the directory is the base);
    // otherwise, detached or on a branch without one, it is the only remote when exactly one is
    // configured, and `origin` otherwise. It is decided here for HEAD as it will be in the
    // World: the root's generated world/W<n> branch has no upstream -- also when the source is
    // itself a World, whose own branch every fork (and every fork of its checkpoints, pooled or
    // not) replaces in git_branch -- and a submodule keeps its source branch unless
    // --committed-only detaches it. Accepted only when that remote's URL travels with the World;
    // the World's own directory is never the source's.
    {
        String branch;
        if (self != (size_t)-1) {
            const GitModule &me = top.modules[self];
            bool detached = me.repo.head_ref.empty() || (tree && me.target != me.repo.head);
            if (!detached && !strncmp(me.repo.head_ref.c_str(), "refs/heads/", 11)) branch.assign(me.repo.head_ref.c_str() + 11);
        }
        // branch.<b>.remote is the selected remote as soon as it is set, even to an empty value
        // (Git then finds no remote..url and uses the repository's directory); the last value
        // wins. pushRemote and remote.pushDefault play no part (observed).
        String remote;
        bool branch_remote = false;
        if (!branch.empty()) {
            String key("branch."); key.append(branch.c_str()); key.append(".remote");
            for (const auto &c : carried) if (c.key == key) { remote = c.value; branch_remote = true; }
        }
        if (!branch_remote) {
            Vec<String> remotes;
            for (const auto &c : carried) {
                if (strncmp(c.key.c_str(), "remote.", 7)) continue;
                const char *dot = strrchr(c.key.c_str() + 7, '.');
                if (!dot) continue;   // remote.pushDefault
                String name(c.key.c_str() + 7, (size_t)(dot - c.key.c_str() - 7));
                bool seen = false;
                for (const auto &r : remotes) if (r == name) seen = true;
                if (!seen) remotes.emplace_back(name);
            }
            remote.assign(remotes.size() == 1 ? remotes[0].c_str() : "origin");
        }
        // The base is the remote's last `url` (observed with several); a remote with only a
        // pushurl has none.
        String remote_url("remote."); remote_url.append(remote.c_str()); remote_url.append(".url");
        const String *base = nullptr;
        bool shared_base = false;
        for (size_t i = 0; i < carried.size(); ++i)
            if (carried[i].key == remote_url) { base = &carried[i].value; shared_base = i < shared; }
        // Rewrite rules from conditional includes, active here or not: loaded only when a URL
        // that only .gitmodules gives is checked against them.
        Vec<UrlRewriteRule> conditional_rules;
        bool rules_loaded = false;
        // Each submodule's effective URL, classified once, as Git in the World will see it: the
        // winning submodule.<name>.url of the shared and the carried configuration (last wins, the
        // repository's own after the shared), else the .gitmodules URL by the default-remote rule.
        for (const auto &link : gitlinks) {
            const String *name = module_name(published_names, link.path);
            if (!name) continue;
            String path(prefix); path.append(link.path.c_str());
            String key("submodule."); key.append(name->c_str()); key.append(".url");
            const GitSetting *configured = nullptr;
            for (size_t i = 0; i < carried.size(); ++i)
                if (carried[i].key == key) configured = &carried[i];
            const char *url = configured ? nullptr : gitmodules_value(published, *name, "url");
            String effective;
            if (configured) {
                continue;   // classified with the configuration (check_configured_submodule_urls)
            } else if (!url) {
                continue;
            } else if (!strncmp(url, "./", 2) || !strncmp(url, "../", 3)) {
                if (!base)
                    return refuse(WFS_E_GIT_POLICY,
                        "submodule %s: its .gitmodules url %s is relative and the repository's default remote "
                        "(%s%s) has no URL the World carries, so it would resolve against the World's location; "
                        "set submodule.%s.url or add the remote before importing", path.c_str(), url,
                        remote.empty() ? "an empty branch." : remote.c_str(), remote.empty() ? "<name>.remote" : "",
                        name->c_str());
                // A shared URL is the same everywhere, but a relative one is resolved from
                // wherever the repository is.
                if (shared_base && is_relative_local_url(base->c_str()))
                    return refuse(WFS_E_GIT_POLICY,
                        "submodule %s: its .gitmodules url %s resolves against %s in global or system "
                        "configuration, which is the relative path %s and resolves differently in the World; "
                        "make it absolute", path.c_str(), url, remote_url.c_str(), base->c_str());
                if (!resolve_submodule_url(*base, url, effective))
                    return refuse(WFS_E_GIT_POLICY,
                        "submodule %s: its .gitmodules url %s cannot be resolved against %s the way Git would "
                        "be reproduced faithfully; set submodule.%s.url before importing", path.c_str(), url,
                        base->c_str(), name->c_str());
            } else if (url[0] == '/' || (url[0] != '~' && !is_relative_local_url(url))) {
                effective.assign(url);
            } else {
                // Any other path (`sub.git`, `..`, `~/x.git`) is cloned as it is, from the worktree
                // top (observed): a different place in the World.
                return refuse(WFS_E_GIT_POLICY,
                    "submodule %s: its .gitmodules url %s is a path Git takes relative to the worktree, "
                    "which differs in the World; set submodule.%s.url before importing", path.c_str(), url, name->c_str());
            }
            // The same guard as for carried remote URLs (capture_carried_config): a rule a
            // conditional include holds could rewrite the URL differently at the World.
            if (!rules_loaded) {
                if (int rc = scan_conditional_includes(repo_root, nullptr, &conditional_rules)) return rc;
                rules_loaded = true;
            }
            const UrlRewriteRule *r = matching_rewrite(conditional_rules, effective.c_str());
            if (r)
                return refuse(WFS_E_GIT_POLICY,
                    "submodule %s: its url resolves to %s, which url.%s.%s from a conditional "
                    "include can change at the World's location; simplify the URL rewrite rules before importing",
                    path.c_str(), effective.c_str(), r->base.c_str(), r->push ? "pushInsteadOf" : "insteadOf");
        }
    }
    String modules_dir;
    const char *modules_args[] = {"rev-parse", "--path-format=absolute", "--git-path", "modules", nullptr};
    if (int rc = value(repo_root, modules_args, modules_dir)) return rc;
    // Every gitlink's name, initialized or not, is a directory below this repository's modules/:
    // initializing one later puts its repository there, so a name that escapes it or lands in
    // (or over) another's is refused now. A gitlink with no .gitmodules entry at all is Git's
    // ordinary "embedded" gitlink; left uninitialized, there is nothing to import for it.
    Vec<String> sibling_names;
    for (const auto &link : gitlinks) {
        const String *name = module_name(published_names, link.path);
        if (!name) continue;
        String path(prefix); path.append(link.path.c_str());
        if (!valid_module_name(*name)) return refuse(WFS_E_GIT_UNSUPPORTED, "submodule %s has an unsafe name (%s)", path.c_str(), name->c_str());
        for (const auto &other : sibling_names) {
            size_t n = other.size() < name->size() ? other.size() : name->size();
            if (ascii_caseeq(other.c_str(), name->c_str(), n) &&
                (other.size() == name->size() || (other.size() > n ? other[n] : (*name)[n]) == '/'))
                return refuse(WFS_E_GIT_UNSUPPORTED, "submodule names %s and %s share a repository directory", other.c_str(), name->c_str());
        }
        sibling_names.emplace_back(*name);
    }
    for (const auto &link : gitlinks) {
        String path(prefix); path.append(link.path.c_str());
        String full = joinp(repo_root, link.path.c_str());
        // Git never checks a submodule out through a symlink; neither is anything else here.
        bool present = true;
        {
            String walk(repo_root);
            for (const char *p = link.path.c_str(); *p && present;) {
                const char *start = p;
                while (*p && *p != '/') ++p;
                walk = joinp(walk.c_str(), String(start, (size_t)(p - start)).c_str());
                if (*p) ++p;
                struct stat st;
                if (lstat(walk.c_str(), &st)) {
                    if (errno != ENOENT) return -errno;
                    present = false;
                } else if (!S_ISDIR(st.st_mode)) {
                    return refuse(WFS_E_GIT_UNSUPPORTED, "submodule path %s is not a directory (%s)", path.c_str(), walk.c_str());
                }
            }
        }
        if (!present) continue;   // a deleted submodule directory: status reports it like any deletion
        String dot = joinp(full.c_str(), ".git");
        struct stat st;
        if (lstat(dot.c_str(), &st)) {
            if (errno != ENOENT) return -errno;
            continue;   // uninitialized
        }
        if (depth >= kMaxModuleDepth)
            return refuse(WFS_E_GIT_UNSUPPORTED, "submodule %s is nested more than %d levels deep", path.c_str(), kMaxModuleDepth);
        const String *name = module_name(published_names, link.path);
        if (!name) return refuse(WFS_E_GIT_UNSUPPORTED, "submodule %s has no entry in the .gitmodules that would be published (the file is missing or does not name it)", path.c_str());
        if (tree) {
            const String *current = module_name(names, link.path);
            if (!current || *current != *name)
                return refuse(WFS_E_GIT_UNSUPPORTED, "submodule %s is named differently in the uncommitted .gitmodules, which --committed-only would reset; commit it or drop --committed-only", path.c_str());
        }
        bool gitfile = S_ISREG(st.st_mode);
        if (!gitfile && !S_ISDIR(st.st_mode))
            return refuse(WFS_E_GIT_UNSUPPORTED, "submodule %s has a .git that is neither a directory nor a file", path.c_str());
        if (!gitfile && top.managed)
            return refuse(WFS_E_GIT_UNSUPPORTED, "submodule %s keeps its repository in its own .git directory instead of the World's administration", path.c_str());
        // Resolve the repository with Git, never by reading the pointer: it must be exactly the
        // submodule's own -- its .git directory, or the superproject's modules/<name> -- and
        // its worktree must be this directory.
        String sub_top, admin, real_full, real_top, real_admin, expected, real_expected;
        const char *top_args[] = {"rev-parse", "--show-toplevel", nullptr};
        const char *admin_args[] = {"rev-parse", "--absolute-git-dir", nullptr};
        if (value(full.c_str(), top_args, sub_top) || fs_realpath(full.c_str(), real_full) ||
            fs_realpath(sub_top.c_str(), real_top) || real_full != real_top)
            return refuse(WFS_E_GIT_UNSUPPORTED, "submodule %s does not hold its own repository's worktree", path.c_str());
        if (int rc = value(full.c_str(), admin_args, admin)) return rc;
        expected = gitfile ? joinp(modules_dir.c_str(), name->c_str()) : dot;
        if (fs_realpath(admin.c_str(), real_admin) || fs_realpath(expected.c_str(), real_expected) || real_admin != real_expected)
            return refuse(WFS_E_GIT_UNSUPPORTED, "the .git of submodule %s points outside its superproject's modules/%s", path.c_str(), name->c_str());
        GitModule m;
        m.path = path;
        m.name = *name;
        m.gitdir = gitdir_prefix; m.gitdir.append("modules/"); m.gitdir.append(name->c_str());
        if (top.managed) {
            if (int rc = check_owned_module(top.root.c_str(), m.path, m.gitdir)) return rc;
        }
        if (tree) {
            const Gitlink *recorded = find_link(tree_links, link.path.c_str());
            m.target = recorded->oid;
            String spec(m.target); spec.append("^{commit}");
            const char *exists[] = {"cat-file", "-e", spec.c_str(), nullptr};
            if (git(full.c_str(), exists, nullptr, nullptr, true))
                return refuse(WFS_E_GIT_UNSUPPORTED, "submodule %s: commit %s, which the committed superproject records, is not in its repository", path.c_str(), m.target.c_str());
        }
        m.repo.managed = top.managed;
        Vec<Gitlink> sub_links;
        if (int rc = capture_repo(full.c_str(), m.repo, top.with_hooks, true, sub_links,
                                  tree ? m.target.c_str() : nullptr)) return in_module(rc, path);
        {
            Vec<GitSetting> config; size_t shared = 0;
            if (int rc = world_config(full.c_str(), m.repo, config, shared)) return rc;
            if (int rc = check_configured_submodule_urls(full.c_str(), config, shared)) return in_module(rc, path);
        }
        if (int rc = nested_check(full.c_str(), &sub_links, false)) return in_module(rc, path);
        if (tree) {
            if (int rc = committed_hooks_check(full.c_str(), m.repo, m.target.c_str())) return in_module(rc, path);
        } else if (top.require_clean && m.repo.head != link.oid) {
            return WFS_E_GIT_DIRTY;   // HEAD moved away from the recorded commit
        }
        top.modules.emplace_back(m);
        String child_prefix(path); child_prefix.push_back('/');
        String child_gitdir(m.gitdir); child_gitdir.push_back('/');
        if (int rc = discover_modules(top, full.c_str(), child_prefix, child_gitdir, sub_links,
                                      tree ? m.target.c_str() : nullptr, depth + 1, top.modules.size() - 1)) return rc;
    }
    return 0;
}
int git_source(const char *root, bool include_changes, GitSource &out, bool committed_only, bool with_hooks) {
    g_reason[0] = '\0';
    if (include_changes && committed_only) return -EINVAL;
    String dot = joinp(root, ".git"), managed = joinp(root, ".world-git");
    struct stat st;
    bool has_managed = lstat(managed.c_str(), &st) == 0;
    if (!has_managed && errno != ENOENT) return -errno;
    if (lstat(dot.c_str(), &st)) {
        if (errno == ENOENT && !has_managed) {
            if (int rc = nested_check(root, nullptr, true)) return rc;
            if (committed_only) return refuse(WFS_E_GIT_UNSUPPORTED, "--committed-only needs a Git repository at the source root");
            if (with_hooks) return refuse(WFS_E_GIT_UNSUPPORTED, "--with-hooks needs a Git repository at the source root");
            return 0;
        }
        return refuse(WFS_E_GIT_UNSUPPORTED, ".world-git exists without its .git marker");
    }
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) return refuse(WFS_E_GIT_UNSUPPORTED, ".git is neither a directory nor a file");
    out.present = true;
    if (has_managed) {
        Vec<char> contents;
        if (int rc = read_bytes(dot.c_str(), contents)) return rc;
        if (contents.size() != strlen(marker) || memcmp(contents.data(), marker, contents.size()))
            return refuse(WFS_E_GIT_UNSUPPORTED, "the .git file is not the WorldFS marker");
        out.managed = true;
    }
    Vec<Gitlink> gitlinks;
    if (int rc = capture_repo(root, out, with_hooks, false, gitlinks)) return rc;
    {
        Vec<GitSetting> config; size_t shared = 0;
        if (int rc = world_config(root, out, config, shared)) return rc;
        if (int rc = check_configured_submodule_urls(root, config, shared)) return rc;
    }
    // A managed World carries its hooks and core.hooksPath with its .world-git, so the same
    // committed-path check applies to it whether or not --with-hooks was given.
    if (committed_only) {
        if (int rc = committed_hooks_check(root, out, nullptr)) return rc;
    }
    // Only the root repository's own linked worktrees are left out; one checked out at or inside
    // a submodule's path is part of that submodule's tree, which is not something to remove.
    for (const auto &w : out.worktrees) for (const auto &l : gitlinks) {
        String under;
        if (!w.rel.empty() && (w.rel == l.path || under_root(w.rel, l.path, &under)))
            return refuse(WFS_E_GIT_UNSUPPORTED, "a linked worktree of the repository is checked out in submodule path %s", l.path.c_str());
    }
    if (int rc = nested_check(root, &gitlinks, true, &out.worktrees)) return rc;
    out.has_gitlinks = !gitlinks.empty();
    // With --committed-only the copy is reset to HEAD and must then be clean; the source's own
    // uncommitted state is simply not carried, so it is not a reason to refuse.
    out.require_clean = !include_changes;
    out.committed_only = committed_only;
    if (int rc = discover_modules(out, root, String(), String(), gitlinks, committed_only ? out.head.c_str() : nullptr, 0, (size_t)-1))
        return rc;
    if (!include_changes && !committed_only) {
        // Every repository's policy checks have run by now, so no status below runs a filter.
        if (int rc = require_clean_tree(root, &out.worktrees)) return rc;
        for (const auto &m : out.modules)
            if (int rc = require_clean_tree(m.repo.root.c_str())) return rc;
    }
    return 0;
}

// A managed World is copied with its whole administration, so a live or stale Git lock
// (packed-refs.lock, refs/**/x.lock, config.lock, commit-graph or midx locks, ...) would be
// published in the child and make its later ref or maintenance updates fail. Scan the copy:
// it is exactly what gets published. Loose-object fan-out directories hold no locks and can
// be large, so they are skipped.
int reject_locks_fd(int dirfd, bool objects_level, int depth) {
    if (depth > 64) return refuse(WFS_E_GIT_UNSUPPORTED, "the World's Git administration is nested too deeply");
    int dup_fd = dup(dirfd);
    if (dup_fd < 0) return -errno;
    DIR *d = fdopendir(dup_fd);
    if (!d) { int rc = -errno; close(dup_fd); return rc; }
    rewinddir(d);
    int rc = 0;
    for (;;) {
        errno = 0; dirent *e = readdir(d);
        if (!e) { if (errno) rc = -errno; break; }
        const char *name = e->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        size_t len = strlen(name);
        if (len >= 5 && !strcmp(name + len - 5, ".lock")) { rc = -EBUSY; break; }
        struct stat st;
        if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW)) { rc = -errno; break; }
        if (!S_ISDIR(st.st_mode)) continue;
        bool fanout = objects_level && len == 2 && isxdigit((unsigned char)name[0]) && isxdigit((unsigned char)name[1]);
        if (fanout) continue;
        int sub = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (sub < 0) { rc = -errno; break; }
        rc = reject_locks_fd(sub, depth == 0 && !strcmp(name, "objects"), depth + 1);
        close(sub);
        if (rc) break;
    }
    closedir(d);
    return rc;
}
int reject_copied_locks(const char *clone) {
    String repo = joinp(clone, ".world-git/repo.git");
    int fd = open(repo.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -errno;
    int rc = reject_locks_fd(fd, false, 0);
    close(fd);
    return rc;
}
// The identity the source's commits carry, kept independent of where the World ends up. When
// any conditional include (`includeIf "gitdir:~/work/"`) sets user.name or user.email, the
// identity can depend on the repository's location -- and the World's final location is not
// known here (it is copied first and moved into place later). So in that case the source's
// effective identity is written into the World's own configuration, and an identity the
// source does not have is written as an explicit empty value. Without such includes the
// identity comes from unconditional configuration, which is the same everywhere, and nothing
// is written (a global identity configured later still applies).
int pin_identity(const char *source, const char *clone) {
    bool conditional = false;
    if (int rc = scan_conditional_includes(source, &conditional)) return rc;
    if (!conditional) return 0;
    for (const char *key : {"user.name", "user.email"}) {
        const char *args[] = {"config", "--get", key, nullptr};
        Vec<char> sv; int ss = -1;
        int rc = git(source, args, &sv, &ss, false, true);
        if (rc && !(rc == WFS_E_GIT_FAILED && ss == 1)) return rc;
        String want(rc ? "" : sv.data());
        if (!want.empty() && want.back() == '\n') want.pop_back();
        if (int wrc = config(clone, key, want.c_str())) return wrc;
    }
    return 0;
}
// What a managed copy must reproduce of its source exactly (git_import).
bool same_capture(const GitRepoState &copy, const GitRepoState &s) {
    return copy.head == s.head && copy.head_ref == s.head_ref && same_bytes(copy.index, s.index) &&
        same_bytes(copy.exclude, s.exclude) && same_bytes(copy.attributes, s.attributes) &&
        copy.fetch_present == s.fetch_present && same_bytes(copy.fetch, s.fetch) &&
        copy.squash_present == s.squash_present && same_bytes(copy.squash, s.squash) &&
        copy.sparse_present == s.sparse_present && same_bytes(copy.sparse, s.sparse) &&
        same_symrefs(copy.symrefs, s.symrefs) && same_settings(copy.settings, s.settings) &&
        // A relative include can resolve differently from the copy's location.
        same_settings(copy.identity, s.identity) && same_bytes(copy.effective_config, s.effective_config) &&
        copy.worktree_config == s.worktree_config && same_settings(copy.worktree_settings, s.worktree_settings) &&
        same_bytes(copy.refs, s.refs) && same_stash(copy, s) && copy.orig_present == s.orig_present &&
        copy.lfs_active == s.lfs_active && copy.lfs_present == s.lfs_present &&
        copy.lfs_filter_setup == s.lfs_filter_setup && copy.lfs_skip_smudge == s.lfs_skip_smudge &&
        copy.lfs_skip_process == s.lfs_skip_process &&
        copy.lfs_target_only == s.lfs_target_only &&
        copy.lfs_bytes == s.lfs_bytes && copy.lfs_entries == s.lfs_entries &&
        same_bytes(copy.lfs_manifest, s.lfs_manifest) &&
        same_bytes(copy.lfs_endpoint_state, s.lfs_endpoint_state) &&
        copy.orig_head == s.orig_head;
}
// A managed copy's submodules must be exactly its source's, in the same place, with the same state.
bool same_modules(const GitSource &copy, const GitSource &s) {
    if (copy.modules.size() != s.modules.size()) return false;
    for (size_t i = 0; i < s.modules.size(); ++i) {
        const GitModule &a = copy.modules[i], &b = s.modules[i];
        if (a.path != b.path || a.name != b.name || a.gitdir != b.gitdir || a.target != b.target ||
            !same_capture(a.repo, b.repo)) return false;
    }
    return true;
}
// After an external import: the copy's repositories -- the root and each imported submodule --
// hold a `.git` below their top exactly at the submodules that were imported, and nowhere else.
// A submodule initialized in the source after it was captured was cloned with a `.git` that
// still leads back into the source's administration; it is caught here instead of published.
int copy_layout_check(const GitSource &s, const char *clone) {
    for (size_t i = 0; i <= s.modules.size(); ++i) {
        String prefix = i ? s.modules[i - 1].path : String();
        String dir = i ? joinp(clone, prefix.c_str()) : String(clone);
        if (i) prefix.push_back('/');
        const char *ls_args[] = {"ls-files", "--stage", "-z", nullptr};
        Vec<char> listing;
        if (int rc = git(dir.c_str(), ls_args, &listing)) return rc;
        Vec<Gitlink> links;
        parse_gitlinks(listing, links);
        for (const auto &link : links) {
            String path(prefix); path.append(link.path.c_str());
            String dot = joinp(clone, path.c_str()); dot = joinp(dot.c_str(), ".git");
            struct stat st;
            bool initialized = !lstat(dot.c_str(), &st);
            if (!initialized && errno != ENOENT) return -errno;
            bool imported = false;
            for (const auto &m : s.modules) if (m.path == path) { imported = true; break; }
            if (initialized != imported) return -EBUSY;
        }
        if (int rc = nested_check(dir.c_str(), &links, i == 0)) return rc;
    }
    return 0;
}
// The copy's .gitmodules -- the bytes that will be published, after any --committed-only
// reset -- must give every repository with gitlinks exactly the settings the import was built
// from: an owned repository sits under the name its .gitmodules had at capture time.
int same_gitmodules(const GitSource &s, const char *clone) {
    for (size_t i = 0; i <= s.modules.size(); ++i) {
        const GitRepoState &state = i ? s.modules[i - 1].repo : s;
        if (!state.gitmodules_checked) continue;
        String dir = i ? joinp(clone, s.modules[i - 1].path.c_str()) : String(clone);
        Vec<char> listing;
        if (int rc = gitmodules_listing(dir.c_str(), nullptr, listing)) return rc;
        if (!same_bytes(listing, state.gitmodules)) return -EBUSY;
    }
    return 0;
}
int sources_unchanged(const GitSource &s) {
    if (int rc = source_unchanged(s)) return rc;
    for (const auto &m : s.modules)
        if (int rc = source_unchanged(m.repo)) return in_module(rc, m.path);
    return 0;
}
// The stash stack of an external source (see GitRepoState::stash). Object stores are cloned in
// full, including unreachable stash history; restore only the reflog, byte for byte, where the
// owned repository's Git reads it (its common directory, shared by the World's worktree).
// require_owned_objects checks that every reflog entry is reachable in the cloned object store.
int restore_stash(const GitRepoState &s, const char *worktree) {
    if (!s.stash_present) return 0;
    String log;
    const char *log_args[] = {"rev-parse", "--path-format=absolute", "--git-path", "logs/refs/stash", nullptr};
    if (int rc = value(worktree, log_args, log)) return rc;
    String refs_dir(log.c_str(), log.size() - strlen("/stash"));
    String logs_dir(refs_dir.c_str(), refs_dir.size() - strlen("/refs"));
    for (const String *dir : {&logs_dir, &refs_dir})
        if (int rc = fs_mkdir(dir->c_str(), 0700)) { if (rc != -EEXIST) return rc; }
    return write_bytes(log.c_str(), s.stash.data(), s.stash.size());
}
// The state every owned repository -- the root's or a submodule's -- carries over from its
// source once its mirror exists: index, local rules, rerere cache, status settings and identity,
// worktree-scoped settings, pending SQUASH_MSG, FETCH_HEAD, ORIG_HEAD, carried configuration,
// hooks and the stash stack. `worktree` is the repository's worktree in the copy, whose Git commands reach `repo`.
int restore_state(const GitRepoState &s, const char *worktree, const char *repo, const char *index, bool root) {
    if (!s.index.empty()) {
        if (int rc = write_bytes(index, s.index.data(), s.index.size())) return rc;
    } else {
        // No index means all tracked files were removed from it; do not recreate HEAD's index.
        const char *empty[] = {"read-tree", "--empty", nullptr};
        if (int rc = git(worktree, empty)) return rc;
    }
    Vec<char> excludes;
    for (char c : s.exclude) excludes.emplace_back(c);
    if (root) {
        if (!excludes.empty() && excludes.back() != '\n') excludes.emplace_back('\n');
        for (const char *reserved : {"/.world\n", "/.world-git/\n"})
            for (const char *p = reserved; *p; ++p) excludes.emplace_back(*p);
    }
    // Empty templates omit info/. Create only the directory needed for owned rules.
    String info = joinp(repo, "info");
    if (int rc = fs_mkdir(info.c_str(), 0700)) { if (rc != -EEXIST) return rc; }
    if (!excludes.empty()) {
        String exclude_path = joinp(repo, "info/exclude");
        if (int rc = write_bytes(exclude_path.c_str(), excludes.data(), excludes.size())) return rc;
    }
    if (!s.attributes.empty()) {
        String attributes_path = joinp(repo, "info/attributes");
        if (int rc = write_bytes(attributes_path.c_str(), s.attributes.data(), s.attributes.size())) return rc;
    }
    if (s.rerere_present) {
        if (int rc = restore_rerere(repo, s.rerere)) return rc;
    }
    if (root) {
        if (int rc = config(worktree, "worldfs.baseline", s.head.c_str())) return rc;
        if (int rc = config(worktree, "worldfs.formatVersion", "1")) return rc;
    }
    for (const auto &id : s.identity)
        if (int rc = config(worktree, id.key.c_str(), id.value.c_str())) return rc;
    for (const char *key : {"core.autocrlf", "core.safecrlf", "core.eol", "core.checkstat", "core.checkRoundtripEncoding",
                            "core.filemode", "core.symlinks", "core.ignorecase", "core.precomposeunicode", "core.trustctime", "core.ignorestat",
                            "core.useReplaceRefs"})
        if (int rc = unset_config(worktree, key)) return rc;
    for (const auto &setting : s.settings)
        if (int rc = config(worktree, setting.key.c_str(), setting.value.c_str())) return rc;
    if (s.worktree_config) {
        if (int rc = config(worktree, "extensions.worktreeConfig", "true")) return rc;
        if (root) {
            // With worktreeConfig on, core.bare must live in the main worktree's config.worktree
            // (git-worktree(1)); left in the common config it would make the owned linked
            // worktree bare too. The owned repository's main worktree is the bare mirror itself.
            const char *bare[] = {"--git-dir", repo, "config", "--worktree", "core.bare", "true", nullptr};
            if (int rc = git(worktree, bare)) return rc;
            if (int rc = unset_config(worktree, "core.bare")) return rc;
        }
        for (const auto &setting : s.worktree_settings) {
            const char *args[] = {"config", "--worktree", setting.key.c_str(), setting.value.c_str(), nullptr};
            if (int rc = git(worktree, args)) return rc;
        }
    }
    if (s.sparse_present) {
        const char *args[] = {"rev-parse", "--path-format=absolute", "--git-path", "info/sparse-checkout", nullptr};
        String dest_sparse;
        if (int rc = value(worktree, args, dest_sparse)) return rc;
        String dest_info(dest_sparse.c_str(), dest_sparse.size() - strlen("/sparse-checkout"));
        if (int rc = fs_mkdir(dest_info.c_str(), 0700)) { if (rc != -EEXIST) return rc; }
        if (int rc = write_bytes(dest_sparse.c_str(), s.sparse.data(), s.sparse.size())) return rc;
    }
    if (s.squash_present) {
        const char *args[] = {"rev-parse", "--path-format=absolute", "--git-path", "SQUASH_MSG", nullptr};
        String dest_squash;
        if (int rc = value(worktree, args, dest_squash)) return rc;
        if (int rc = write_bytes(dest_squash.c_str(), s.squash.data(), s.squash.size())) return rc;
    }
    if (s.fetch_present) {
        const char *args[] = {"rev-parse", "--path-format=absolute", "--git-path", "FETCH_HEAD", nullptr};
        String dest_fetch;
        if (int rc = value(worktree, args, dest_fetch)) return rc;
        if (int rc = write_bytes(dest_fetch.c_str(), s.fetch.data(), s.fetch.size())) return rc;
    }
    if (s.orig_present) {
        const char *orig_args[] = {"update-ref", "ORIG_HEAD", s.orig_head.c_str(), nullptr};
        if (int rc = git(worktree, orig_args)) return rc;
    }
    // The mirror's own remote points at the source: it is not an implicit write-back channel and
    // is removed. The source's own remotes, upstreams and aliases are carried instead.
    const char *remote[] = {"config", "--local", "--remove-section", "remote.origin", nullptr};
    if (int rc = git(worktree, remote)) return rc;
    for (const auto &setting : s.carried) {
        const char *args[] = {"config", "--local", "--add", setting.key.c_str(), setting.value.c_str(), nullptr};
        if (int rc = git(worktree, args)) return rc;
    }
    // Installed, never run: every command here passes core.hooksPath=/dev/null.
    if (s.with_hooks || s.lfs_active) {
        if (int rc = install_hooks(s, worktree, repo)) return rc;
    }
    if (int rc = restore_stash(s, worktree)) return rc;
    // The copy as it will be published: its refs, and its stash stack read back by Git itself --
    // the same reflog bytes, the same entries, and every object of every entry present.
    GitRepoState imported;
    if (int rc = capture_refs(worktree, imported.refs)) return rc;
    if (!same_bytes(imported.refs, s.refs)) return -EBUSY;
    if (int rc = capture_stash(worktree, imported)) return rc;
    return same_stash(imported, s) ? 0 : -EBUSY;
}
bool lower_hex(const char *p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return false;
    return n > 0;
}
// `<prefix><object ID><suffix>` for one of `suffixes` (null-terminated), SHA-1 or SHA-256.
bool hashed_name(const char *name, const char *prefix, const char *const *suffixes) {
    size_t pl = strlen(prefix);
    if (strncmp(name, prefix, pl)) return false;
    const char *hash = name + pl, *dot = strchr(hash, '.');
    size_t n = dot ? (size_t)(dot - hash) : strlen(hash);
    if ((n != 40 && n != 64) || !lower_hex(hash, n) || !dot) return false;
    for (size_t i = 0; suffixes[i]; ++i) if (!strcmp(dot, suffixes[i])) return true;
    return false;
}
// What an object store holds, by directory relative to objects/: loose objects in the
// two-hex-digit fan-out directories; packs with their .idx, .rev, .bitmap, .keep and .mtimes
// files; multi-pack indexes, single or chained; info/packs and commit-graphs, single or chained.
bool object_store_entry(const char *rel, const char *name, bool dir) {
    static const char *const pack_ext[] = {".pack", ".idx", ".rev", ".bitmap", ".keep", ".mtimes", nullptr};
    static const char *const midx_ext[] = {".bitmap", ".rev", nullptr};
    static const char *const chained_midx_ext[] = {".midx", ".bitmap", ".rev", nullptr};
    static const char *const graph_ext[] = {".graph", nullptr};
    if (!*rel) return dir && ((strlen(name) == 2 && lower_hex(name, 2)) || !strcmp(name, "pack") || !strcmp(name, "info"));
    if (strlen(rel) == 2) return !dir && (strlen(name) == 38 || strlen(name) == 62) && lower_hex(name, strlen(name));
    if (!strcmp(rel, "pack")) {
        if (dir) return !strcmp(name, "multi-pack-index.d");
        return hashed_name(name, "pack-", pack_ext) || !strcmp(name, "multi-pack-index") ||
               hashed_name(name, "multi-pack-index-", midx_ext);
    }
    if (!strcmp(rel, "pack/multi-pack-index.d"))
        return !dir && (!strcmp(name, "multi-pack-index-chain") || hashed_name(name, "multi-pack-index-", chained_midx_ext));
    if (!strcmp(rel, "info")) {
        if (dir) return !strcmp(name, "commit-graphs");
        return !strcmp(name, "packs") || !strcmp(name, "commit-graph");
    }
    if (!strcmp(rel, "info/commit-graphs"))
        return !dir && (!strcmp(name, "commit-graph-chain") || hashed_name(name, "graph-", graph_ext));
    return false;
}
// Reduce a cloned object directory to object_store_entry's allowlist. Anything else Git may
// have had there mid-write -- tmp_obj_*/tmp_pack_*/.tmp-* files, incoming-* quarantine
// directories of a push not yet accepted -- is not part of the repository and is dropped;
// objects nothing reaches are harmless and anything reachable that went missing is caught by
// require_owned_objects. What eligibility refuses in the source (a symlink or special file,
// alternates, promisor packs) is refused again here, since the clone reflects the source a
// moment after it was checked.
int prune_object_clone(const String &dir, const String &rel, unsigned depth) {
    if (depth > 3) return refuse(WFS_E_GIT_UNSUPPORTED, "the object directory is nested too deeply");
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ELOOP || errno == ENOTDIR
        ? refuse(WFS_E_GIT_UNSUPPORTED, "the object directory contains a symlink or special file (%s)", rel.empty() ? "objects" : rel.c_str()) : -errno;
    Vec<String> names;
    int rc = list_names(fd, names);
    for (size_t i = 0; !rc && i < names.size(); ++i) {
        const char *name = names[i].c_str();
        struct stat st;
        if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) { rc = -errno; break; }
        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
            rc = refuse(WFS_E_GIT_UNSUPPORTED, "the object directory contains a symlink or special file (%s)", name); break;
        }
        String child_rel = rel.empty() ? String(name) : joinp(rel.c_str(), name);
        static const char *const promisor_ext[] = {".promisor", nullptr};
        if (child_rel == "info/alternates" || child_rel == "info/http-alternates") {
            rc = refuse(WFS_E_GIT_UNSUPPORTED, "objects/%s is present (alternates or shallow clone)", child_rel.c_str()); break;
        }
        if (rel == "pack" && hashed_name(name, "pack-", promisor_ext)) { rc = refuse(WFS_E_GIT_UNSUPPORTED, "partial clone (a promisor pack)"); break; }
        String path = joinp(dir.c_str(), name);
        if (!object_store_entry(rel.c_str(), name, S_ISDIR(st.st_mode))) rc = fs_remove_tree(path.c_str());
        else if (S_ISDIR(st.st_mode)) rc = prune_object_clone(path, child_rel, depth + 1);
    }
    close(fd);
    return rc;
}
// The owned repository is created the way `git clone --mirror` made it -- the same bare layout,
// packed refs, HEAD, remote.origin mirror configuration (removed again by restore_state) and
// symbolic refs -- but its objects are not transferred through Git: the source's common object
// directory is cloned with fs_clone_tree (clonefile on APFS, reflinks on XFS/Btrfs, a copy on
// ext4 or across volumes, never following symlinks), so an import costs metadata rather than a
// second copy of every pack. That also takes loose objects, including blobs only the index
// refers to. Refs are then written from the captured snapshot with one update-ref transaction,
// which only checks that each object exists; nothing is fetched. The clone shares no inode
// with the source, so later Git operations in either never change the other's object files.
int own_repository(const GitRepoState &s, const char *cwd, const char *from, const char *repo) {
    String format;
    const char *format_args[] = {"rev-parse", "--show-object-format", nullptr};
    if (int rc = value(s.root.c_str(), format_args, format)) return rc;
    String format_arg("--object-format="); format_arg.append(format.c_str());
    const char *init[] = {"init", "--bare", "--template=", "--quiet", format_arg.c_str(), "--", repo, nullptr};
    if (int rc = git(cwd, init)) return rc;
    String objects = joinp(repo, "objects");
    if (int rc = fs_remove_tree(objects.c_str())) return rc;
    int rc = fs_clone_tree(s.objects.c_str(), objects.c_str(), true);
    // A pack or loose object that vanished mid-clone: gc/repack in the source. Retryable.
    if (rc == -ENOENT || rc == -ESTALE) return -EBUSY;
    if (rc) return rc;
    if (s.lfs_active) {
        String lfs = joinp(repo, "lfs");
        String media = joinp(lfs.c_str(), "objects");
        if (s.lfs_present) {
            if (int mrc = fs_mkdir(lfs.c_str(), 0700)) { if (mrc != -EEXIST) return mrc; }
            rc = fs_clone_tree(s.lfs_objects.c_str(), media.c_str(), true);
            if (rc == -ENOENT || rc == -ESTALE) return -EBUSY;
            if (rc) return rc;
            uint64_t bytes = 0, entries = 0; bool present = false; Vec<char> manifest;
            if ((rc = lfs_import_bytes(s.root.c_str(), media.c_str(), bytes, entries, present, manifest))) return rc;
            if (!present || bytes != s.lfs_bytes || entries != s.lfs_entries || !same_bytes(manifest, s.lfs_manifest)) return -EBUSY;
        }
        if ((rc = configure_lfs_filter(s, cwd, repo))) return rc;
    }
    if ((rc = prune_object_clone(objects, String(), 0))) return rc;
    String script;
    for (size_t i = 0; i < s.refs.size() && s.refs[i];) {
        size_t end = i, tab = i;
        while (end < s.refs.size() && s.refs[end] && s.refs[end] != '\n') ++end;
        while (tab < end && s.refs[tab] != '\t') ++tab;
        if (tab == end) return -EINVAL;
        script.append("create ");
        script.append(s.refs.data() + i, tab - i);
        script.push_back(' ');
        script.append(s.refs.data() + tab + 1, end - tab - 1);
        script.push_back('\n');
        i = end < s.refs.size() && s.refs[end] == '\n' ? end + 1 : end;
    }
    FILE *input = tmpfile();
    if (!input) return -errno;
    if (fwrite(script.c_str(), 1, script.size(), input) != script.size() || fflush(input)) {
        int err = errno ? -errno : -EIO; fclose(input); return err;
    }
    rewind(input);
    const char *txn[] = {"--git-dir", repo, "update-ref", "--no-deref", "--stdin", nullptr};
    rc = git(cwd, txn, nullptr, nullptr, false, false, fileno(input));
    fclose(input);
    // An object the captured refs name is missing from the clone: repacked away mid-clone.
    if (rc == WFS_E_GIT_FAILED) return -EBUSY;
    if (rc) return rc;
    const char *pack[] = {"--git-dir", repo, "pack-refs", "--all", nullptr};
    if ((rc = git(cwd, pack))) return rc;
    const char *on_branch[] = {"--git-dir", repo, "symbolic-ref", "HEAD", s.head_ref.c_str(), nullptr};
    const char *detached[] = {"--git-dir", repo, "update-ref", "--no-deref", "HEAD", s.head.c_str(), nullptr};
    if ((rc = git(cwd, s.head_ref.empty() ? detached : on_branch))) return rc;
    const char *const origin[][2] = {{"remote.origin.url", from}, {"remote.origin.tagOpt", "--no-tags"},
        {"remote.origin.fetch", "+refs/*:refs/*"}, {"remote.origin.mirror", "true"}};
    for (const auto &kv : origin) {
        const char *args[] = {"--git-dir", repo, "config", kv[0], kv[1], nullptr};
        if ((rc = git(cwd, args))) return rc;
    }
    for (const auto &ref : s.symrefs) {
        const char *sym_args[] = {"--git-dir", repo, "symbolic-ref", ref.name.c_str(), ref.target.c_str(), nullptr};
        if (int rc = git(cwd, sym_args)) return rc;
    }
    return 0;
}
int import_root(const GitSource &s, const char *clone) {
    String owned = joinp(clone, ".world-git");
    if (int rc = fs_mkdir(owned.c_str(), 0700)) return rc;
    String repo = joinp(owned.c_str(), "repo.git");
    if (int rc = own_repository(s, clone, s.root.c_str(), repo.c_str())) return rc;
    String active = joinp(owned.c_str(), "active");
    const char *add[] = {"--git-dir", repo.c_str(), "worktree", "add", "--relative-paths", "--no-checkout",
        "--detach", "--quiet", "--", active.c_str(), s.head.c_str(), nullptr};
    if (int rc = git(clone, add)) return rc;
    String dot = joinp(clone, ".git");
    if (int rc = fs_remove_tree(dot.c_str())) return rc;
    if (int rc = write_text(clone, ".git", marker)) return rc;
    const char *repair[] = {"worktree", "repair", "--relative-paths", nullptr};
    if (int rc = git_diagnose_on_failure(clone, repair)) return rc;
    // Linked worktrees created in the World later -- an AI agent's `.claude/worktrees/<name>`,
    // say -- get relative links too, so one inside the tree keeps working when the World is
    // moved, trashed and restored. Any copy leaves such worktrees out (omit_worktrees).
    const char *relative[] = {"--git-dir", repo.c_str(), "config", "worktree.useRelativePaths", "true", nullptr};
    if (int rc = git(clone, relative)) return rc;
    // The only entry in the disposable no-checkout directory is its gitfile.
    String old_dot = joinp(active.c_str(), ".git");
    if (unlink(old_dot.c_str()) || rmdir(active.c_str())) return -errno;
    String index = joinp(repo.c_str(), "worktrees/active/index");
    return restore_state(s, clone, repo.c_str(), index.c_str(), true);
}
// A submodule's repository goes where Git itself looks for it -- modules/<name> of its
// superproject's Git directory, below the root's per-worktree one -- as a non-bare repository
// whose core.worktree and the worktree's `.git` file point at each other relatively. The copied
// `.git` (an old-style repository, or a gitfile into the source's administration) is removed
// first, so no command below can reach the source through it.
int import_module(const GitModule &m, const char *clone) {
    const GitRepoState &s = m.repo;
    String worktree = joinp(clone, m.path.c_str());
    String admin(kActive); admin.push_back('/'); admin.append(m.gitdir.c_str());
    String repo = joinp(clone, admin.c_str());
    String dot = joinp(worktree.c_str(), ".git");
    if (int rc = fs_remove_tree(dot.c_str())) return rc;
    if (int rc = own_repository(s, clone, s.admin.c_str(), repo.c_str())) return rc;
    String link = module_worktree(m.path, m.gitdir);
    const char *bare[] = {"--git-dir", repo.c_str(), "config", "core.bare", "false", nullptr};
    const char *wt[] = {"--git-dir", repo.c_str(), "config", "core.worktree", link.c_str(), nullptr};
    if (int rc = git(clone, bare)) return rc;
    if (int rc = git(clone, wt)) return rc;
    // HEAD as the source had it: on its branch, or detached.
    const char *on_branch[] = {"--git-dir", repo.c_str(), "symbolic-ref", "HEAD", s.head_ref.c_str(), nullptr};
    const char *detached[] = {"--git-dir", repo.c_str(), "update-ref", "--no-deref", "HEAD", s.head.c_str(), nullptr};
    if (int rc = git(clone, s.head_ref.empty() ? detached : on_branch)) return rc;
    String gitfile = module_gitfile(m.path, m.gitdir);
    if (int rc = write_bytes(dot.c_str(), gitfile.c_str(), gitfile.size())) return rc;
    String top, real_top, real_worktree;
    const char *top_args[] = {"rev-parse", "--show-toplevel", nullptr};
    if (int rc = value(worktree.c_str(), top_args, top)) return rc;
    if (fs_realpath(top.c_str(), real_top) || fs_realpath(worktree.c_str(), real_worktree) || real_top != real_worktree)
        return refuse(WFS_E_GIT_UNSUPPORTED, "its owned repository does not resolve to its worktree");
    String index = joinp(repo.c_str(), "index");
    return restore_state(s, worktree.c_str(), repo.c_str(), index.c_str(), false);
}
// --committed-only: the root to HEAD, then each submodule, parents first, to the commit its
// superproject's committed tree records.
int reset_copy(const GitSource &s, const char *clone) {
    if (int rc = reset_to_head(clone)) return rc;
    for (const auto &m : s.modules) {
        String path = joinp(clone, m.path.c_str());
        if (int rc = reset_to_head(path.c_str(), m.target.c_str())) return in_module(rc, m.path);
    }
    return 0;
}
// HEAD and the index do not change when a tracked file is edited after the source's clean check,
// so check the copy that will actually be published, every submodule included. Re-probe each
// repository's ambient policy and filters first -- GIT_CONFIG_GLOBAL/SYSTEM and other
// per-directory configuration can resolve differently beside the copy than beside the source --
// so no status below runs a filter the source-side probe never saw.
int require_clean_copy(const GitSource &s, const char *clone) {
    if (int rc = reject_ambient_policy(clone)) return rc;
    if (int rc = reject_used_filters(clone)) return rc;
    for (const auto &m : s.modules) {
        String path = joinp(clone, m.path.c_str());
        if (int rc = reject_ambient_policy(path.c_str())) return in_module(rc, m.path);
        if (int rc = reject_used_filters(path.c_str())) return in_module(rc, m.path);
    }
    if (int rc = require_clean_tree(clone)) return rc;
    for (const auto &m : s.modules) {
        String path = joinp(clone, m.path.c_str());
        if (int rc = require_clean_tree(path.c_str())) return rc;
    }
    return 0;
}
// A source that repacks or collects garbage while its object directory is cloned can leave the
// clone without some objects (a pack deleted before the clone reached it, its replacement
// created after). Before publication, walk everything the owned repository preserves -- all refs
// and HEADs, reflogs, the index, ORIG_HEAD and FETCH_HEAD tips -- down to every tree and blob, as
// Git's own connectivity check after a fetch does. --no-replace-objects walks the real graph, not
// what refs/replace/* substitutes. A hole means the source changed mid-import: -EBUSY, retryable.
int require_owned_objects(const GitRepoState &s, const char *worktree) {
    Vec<String> tips;
    if (s.orig_present) tips.emplace_back(s.orig_head.c_str());
    if (s.fetch_present) {
        // FETCH_HEAD lines start with an object ID followed by a tab (reject_reserved_paths
        // has already refused anything else).
        for (size_t start = 0, i = 0, n = s.fetch.size(); i <= n; ++i) {
            if (i < n && s.fetch[i] != '\n') continue;
            size_t hex = start;
            while (hex < i && lower_hex(s.fetch.data() + hex, 1)) ++hex;
            if (hex < i && s.fetch[hex] == '\t' && (hex - start == 40 || hex - start == 64))
                tips.emplace_back(s.fetch.data() + start, hex - start);
            start = i + 1;
        }
    }
    Vec<const char *> args;
    for (const char *a : {"--no-replace-objects", "rev-list", "--objects", "--quiet", "--all", "--reflog", "--indexed-objects"})
        args.emplace_back(a);
    for (const auto &tip : tips) args.emplace_back(tip.c_str());
    args.emplace_back("--");
    args.emplace_back(nullptr);
    int rc = git(worktree, args.data(), nullptr, nullptr, true);
    return rc == WFS_E_GIT_FAILED ? -EBUSY : rc;
}
int require_owned_lfs(const GitRepoState &s, const char *worktree) {
    if (!s.lfs_active) return 0;
    String common;
    const char *args[] = {"rev-parse", "--path-format=absolute", "--git-common-dir", nullptr};
    if (int rc = value(worktree, args, common)) return rc;
    String objects = joinp(common.c_str(), "lfs/objects");
    uint64_t bytes = 0, entries = 0; bool present = false; Vec<char> manifest;
    if (int rc = lfs_import_bytes(worktree, objects.c_str(), bytes, entries, present, manifest)) return rc;
    if (present != s.lfs_present || bytes != s.lfs_bytes || entries != s.lfs_entries ||
        !same_bytes(manifest, s.lfs_manifest))
        return refuse(WFS_E_GIT_UNSUPPORTED, "the World's Git LFS cache changed or contains corrupt objects");
    return 0;
}
uint64_t git_import_budget(const GitSource &s, const char *near) {
    uint64_t total = 0;
    for (size_t i = 0; i <= s.modules.size(); ++i) {
        const GitRepoState &state = i ? s.modules[i - 1].repo : s;
        if (state.managed || state.objects.empty()) continue;
        uint64_t objects = state.import_bytes - state.rerere_bytes - state.lfs_bytes;
        uint64_t metadata = state.object_entries > objects / 1024 ? objects : state.object_entries * 1024;
        uint64_t need = state.rerere_bytes + (fs_clone_shares(near, state.objects.c_str()) ? metadata : objects);
        if (state.lfs_present) {
            uint64_t lfs_metadata = state.lfs_entries > state.lfs_bytes / 1024 ? state.lfs_bytes : state.lfs_entries * 1024;
            uint64_t lfs_need = fs_clone_shares(near, state.lfs_objects.c_str()) ? lfs_metadata : state.lfs_bytes;
            need = lfs_need > UINT64_MAX - need ? UINT64_MAX : need + lfs_need;
        }
        total = need > UINT64_MAX - total ? UINT64_MAX : total + need;
    }
    return total;
}
// wfs_git_omitted_worktrees(): what the last import on this thread left out, one per line.
thread_local String g_omitted;
// Removes `rel` below `clone` -- a copy this import owns and nothing else writes -- without
// following a symlink on the way: a component that changed in the source after it was captured
// cannot turn the removal into one outside the copy. Already gone is fine (a worktree nested in
// one removed before it).
int remove_below(const char *clone, const String &rel) {
    String path(clone);
    size_t start = 0;
    for (size_t i = 0; i <= rel.size(); ++i) {
        if (i < rel.size() && rel[i] != '/') continue;
        path.push_back('/');
        path.append(rel.c_str() + start, i - start);
        start = i + 1;
        struct stat st;
        if (lstat(path.c_str(), &st)) return errno == ENOENT ? 0 : -errno;
        if (!S_ISDIR(st.st_mode)) return -EBUSY;
    }
    return fs_remove_tree(path.c_str());
}
// A managed World's registration of the linked worktree `w`, relative to the World's root.
String worktree_registration(const GitWorktree &w) {
    String admin(".world-git/repo.git/worktrees/");
    admin.append(w.id.c_str());
    return admin;
}
// The source's or parent World's other linked worktrees (GitWorktree) are separate checkouts
// of the repository, not part of the tree: the copy gets the repository's refs and objects --
// so their branches and commits -- but not their checkouts or their uncommitted state. Their
// checkouts inside the tree are removed from the copy; a managed World's copy also drops their
// registrations, which would otherwise make the child's Git believe it had checkouts that
// belong to the parent (a branch checked out there could not be checked out in the child). The
// source, the parent and anything outside the tree are never touched.
// The copy may not have them at all: a walked copy (fs_clone_tree's `omit`) never made them,
// and remove_below takes that as done. A whole-root clonefile(2) copied them and they go here.
int omit_worktrees(const GitSource &s, const char *clone) {
    for (const auto &w : s.worktrees) {
        if (s.managed)
            if (int rc = remove_below(clone, worktree_registration(w))) return rc;
        if (!w.rel.empty())
            if (int rc = remove_below(clone, w.rel)) return rc;
        g_omitted.append(w.rel.empty() ? w.path.c_str() : w.rel.c_str());
        g_omitted.push_back('\n');
    }
    return 0;
}
void git_omitted_names(const GitSource &s, Vec<String> &out) {
    out.clear();
    if (!s.present) return;
    for (const auto &w : s.worktrees) {
        if (s.managed) out.emplace_back(worktree_registration(w));
        if (!w.rel.empty()) out.emplace_back(w.rel);
    }
}
int git_import(const GitSource &s, const char *clone) {
    g_omitted.clear();
    if (!s.present) return 0;
    if (int rc = sources_unchanged(s)) return rc;
    if (int rc = omit_worktrees(s, clone)) return rc;
    if (!s.managed) {
        // A linked worktree added inside the source after the capture may have been cloned
        // with a `.git` leading back into the source's administration; Git registers a worktree
        // before it checks one out, so its registration shows it. One removed meanwhile (an
        // agent's cleanup) is fine: its checkout was left out of the copy either way. (A managed
        // copy is recaptured below instead.)
        Vec<GitWorktree> now;
        if (int rc = collect_worktrees(s.root.c_str(), s.common.c_str(), s.admin.c_str(), false, now)) return rc;
        for (const auto &w : now) {
            bool known = false;
            for (const auto &c : s.worktrees) if (c.id == w.id && c.rel == w.rel) known = true;
            if (!known) return -EBUSY;
        }
    }
    if (!s.managed) {
        if (int rc = import_root(s, clone)) return rc;
        for (const auto &m : s.modules)
            if (int rc = import_module(m, clone)) return in_module(rc, m.path);
        if (int rc = require_owned_objects(s, clone)) return rc;
        if (int rc = require_owned_lfs(s, clone)) return rc;
        for (const auto &m : s.modules) {
            String path = joinp(clone, m.path.c_str());
            if (int rc = require_owned_objects(m.repo, path.c_str())) return in_module(rc, m.path);
            if (int rc = require_owned_lfs(m.repo, path.c_str())) return in_module(rc, m.path);
        }
        if (int rc = sources_unchanged(s)) return rc;
    }
    if (s.managed) {
        // Capture the copy as the managed World it is, submodules included: a copied HEAD/index
        // from a different moment, or a link that resolves differently at the copy's location,
        // is caught here even when the source looks unchanged again by the time cloning ends.
        GitSource copy;
        if (int rc = git_source(clone, !s.committed_only, copy, s.committed_only)) return rc;
        // A worktree registered after the capture was copied but not omitted.
        if (!same_capture(copy, s) || !same_modules(copy, s) || !copy.worktrees.empty()) return -EBUSY;
        if (int rc = reject_copied_locks(clone)) return rc;
        if (s.lfs_target_only) {
            String repo = joinp(clone, ".world-git/repo.git");
            if (int rc = activate_managed_target_lfs(s, clone, repo.c_str())) return rc;
        }
        for (const auto &m : s.modules) if (m.repo.lfs_target_only) {
            String path = joinp(clone, m.path.c_str());
            String admin(".world-git/repo.git/worktrees/active/"); admin.append(m.gitdir.c_str());
            String repo = joinp(clone, admin.c_str());
            if (int rc = activate_managed_target_lfs(m.repo, path.c_str(), repo.c_str())) return in_module(rc, m.path);
        }
    } else if (s.has_gitlinks) {
        if (int rc = copy_layout_check(s, clone)) return rc;
    }
    if (int rc = pin_identity(s.root.c_str(), clone)) return rc;
    for (const auto &m : s.modules) {
        String path = joinp(clone, m.path.c_str());
        if (int rc = pin_identity(m.repo.root.c_str(), path.c_str())) return in_module(rc, m.path);
    }
    if (s.committed_only) {
        if (int rc = reset_copy(s, clone)) return rc;
    }
    if (int rc = same_gitmodules(s, clone)) return rc;
    if (!s.require_clean) return 0;
    return require_clean_copy(s, clone);
}

// Whether any submodule repository below `dir` (the World's active/modules) has a linked
// worktree registered: a directory holding HEAD whose `worktrees` directory is not empty.
int modules_in_use(const String &dir, int depth) {
    if (depth > 64) return refuse(WFS_E_GIT_UNSUPPORTED, "the World's submodule administration is nested too deeply");
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT || errno == ENOTDIR ? 0 : -errno;
    Vec<String> names;
    int rc = list_names(fd, names);
    struct stat st;
    bool repository = !rc && !fstatat(fd, "HEAD", &st, AT_SYMLINK_NOFOLLOW);
    close(fd);
    if (rc) return rc;
    for (const auto &name : names) {
        String child = joinp(dir.c_str(), name.c_str());
        if (lstat(child.c_str(), &st) || !S_ISDIR(st.st_mode)) continue;
        if (repository && name == "worktrees") {
            int wfd = open(child.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (wfd < 0) return -errno;
            Vec<String> entries;
            rc = list_names(wfd, entries);
            close(wfd);
            if (rc) return rc;
            if (!entries.empty())
                return refuse(WFS_E_GIT_IN_USE, "a submodule repository of the World has a linked worktree (%s)", child.c_str());
            continue;
        }
        if ((rc = modules_in_use(child, depth + 1))) return rc;
    }
    return 0;
}
// A linked worktree whose checkout is inside the World goes to the trash with it, registration
// and all, and comes back with it on restore. One outside it would be left with a `.git` pointing
// into a discarded tree, so it blocks the discard until it is removed with Git; the refusal names
// each such checkout and the command.
int git_discard_check(const char *root) {
    g_reason[0] = '\0';
    String owned = joinp(root, ".world-git"); struct stat st;
    bool managed = lstat(owned.c_str(), &st) == 0;
    if (!managed && errno != ENOENT) return -errno;
    String repo = joinp(root, managed ? ".world-git/repo.git" : ".git"), dir = joinp(repo.c_str(), "worktrees");
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT || errno == ENOTDIR ? 0 : -errno;
    Vec<String> names;
    int rc = list_names(fd, names);
    close(fd);
    if (rc) return rc;
    bool others = false;
    for (const auto &name : names) if (!managed || name != "active") others = true;
    if (others && !managed)
        return refuse(WFS_E_GIT_IN_USE, "the tree's repository has linked worktrees; remove them with "
                      "`git -C %s worktree remove <path>`", root);
    if (others) {
        String active = joinp(dir.c_str(), "active");
        Vec<GitWorktree> worktrees;
        // A registration whose checkout cannot be placed is not known to be inside the World.
        if (collect_worktrees(root, repo.c_str(), active.c_str(), true, worktrees))
            return refuse(WFS_E_GIT_IN_USE, "the World's linked worktree registrations cannot be read; remove its "
                          "other worktrees with `git -C %s worktree remove <path>` or `git -C %s worktree prune`", root, root);
        String outside;
        for (const auto &w : worktrees) {
            if (w.inside) continue;
            if (!outside.empty()) outside.append(", ");
            outside.append(w.path.c_str());
        }
        if (!outside.empty())
            return refuse(WFS_E_GIT_IN_USE, "linked worktrees outside the World depend on it: %s; remove each with "
                          "`git -C %s worktree remove <path>` (`git -C %s worktree prune` for one that no longer exists)",
                          outside.c_str(), root, root);
    }
    // A linked worktree of one of the World's submodules would be left pointing at nothing too.
    return modules_in_use(joinp(root, ".world-git/repo.git/worktrees/active/modules"), 0);
}

// A regex matching every "branch.<short_name>.*" key, with short_name's regex
// metacharacters escaped, in the style `git config --get-regexp` expects.
String branch_section_pattern(const char *short_name) {
    String pattern("^branch\\.");
    for (const char *p = short_name; *p; ++p) {
        if (strchr(R"(.+*?[](){}^$|\)", *p)) pattern.push_back('\\');
        pattern.push_back(*p);
    }
    pattern.append("\\.");
    return pattern;
}
// Whether the user's ambient configuration (global, system, or GIT_CONFIG_* command
// configuration -- any scope other than local/worktree) has any branch.<short_name>.* key.
// Ordinary Git in the World reads this configuration the same way it reads the source's, but
// this import can only remove repository-local configuration for a branch (see the
// --local --remove-section below), so a name still carrying ambient branch settings -- for
// example branch.world/W1.remote from a global config entry -- must not be chosen at all.
int branch_has_ambient_config(const char *clone, const char *short_name, bool &out) {
    out = false;
    String pattern = branch_section_pattern(short_name);
    Vec<char> listing; int status = -1;
    const char *args[] = {"config", "--includes", "--null", "--show-scope", "--name-only",
        "--get-regexp", pattern.c_str(), nullptr};
    int rc = git(clone, args, &listing, &status, false, true);
    if (rc == WFS_E_GIT_FAILED && status == 1) return 0; // no matching key in any scope
    if (rc) return rc;
    // Entries are "<scope>\0<key>\0".
    for (size_t i = 0; i < listing.size() && listing[i];) {
        const char *scope = listing.data() + i;
        i += strlen(scope) + 1;
        if (i >= listing.size()) return WFS_E_GIT_FAILED;
        const char *key = listing.data() + i;
        i += strlen(key) + 1;
        (void)key;
        if (strcmp(scope, "local") && strcmp(scope, "worktree")) { out = true; return 0; }
    }
    return 0;
}
int git_branch(const char *clone, wfs_id world) {
    String dot = joinp(clone, ".git"); struct stat st;
    if (lstat(dot.c_str(), &st)) return errno == ENOENT ? 0 : -errno;
    GitSource s;
    if (int rc = git_source(clone, true, s)) return rc;
    if (!s.managed) return refuse(WFS_E_GIT_UNSUPPORTED, "not a WorldFS-managed Git World");
    // Every copy leaves the other linked worktrees out (omit_worktrees), and snapshots are copies,
    // so a new World that still has one was not made by this code.
    if (!s.worktrees.empty())
        return refuse(WFS_E_GIT_UNSUPPORTED, "the World has an additional linked worktree (%s)", s.worktrees[0].id.c_str());
    Vec<char> refs;
    const char *list_refs[] = {"for-each-ref", "--format=%(refname)", "refs/heads/", nullptr};
    if (int rc = git(clone, list_refs, &refs)) return rc;
    Vec<String> names;
    size_t start = 0;
    for (size_t i = 0; i + 1 < refs.size(); ++i) if (refs[i] == '\n') {
        names.emplace_back(refs.data() + start, i - start); start = i + 1;
    }
    bool root_taken = false;
    for (const auto &name : names) if (name == "refs/heads/world") root_taken = true;
    char branch[96];
    bool available = false;
    // Each existing branch blocks at most one candidate (by equality or as a directory prefix),
    // so names.size() + 1 candidates always include one free of ref collisions. A candidate can
    // also be blocked by ambient (non-local) branch.<name>.* configuration this import cannot
    // remove; each such block extends the bound by one more candidate, so the loop always still
    // reaches a name free of both. `ambient_blocked` is capped so a pathological ambient
    // configuration (thousands of matching sections) cannot loop unboundedly.
    size_t ambient_blocked = 0;
    constexpr size_t kMaxAmbientBlocked = 4096;
    for (size_t suffix = 0; suffix <= names.size() + ambient_blocked; ++suffix) {
        const char *sep = root_taken ? "-" : "/";
        if (!suffix) snprintf(branch, sizeof branch, "refs/heads/world%sW%llu", sep, (unsigned long long)world);
        else snprintf(branch, sizeof branch, "refs/heads/world%sW%llu-%llu", sep, (unsigned long long)world,
                      (unsigned long long)suffix);
        available = true;
        size_t len = strlen(branch);
        for (const auto &name : names) {
            size_t n = name.size(), shorter = n < len ? n : len;
            if (!memcmp(branch, name.c_str(), shorter) &&
                (n == len || (n < len ? branch[n] == '/' : name[len] == '/'))) {
                available = false; break;
            }
        }
        if (available) {
            bool ambient_configured = false;
            if (int rc = branch_has_ambient_config(clone, branch + 11, ambient_configured)) return rc;
            if (ambient_configured) {
                available = false;
                if (++ambient_blocked > kMaxAmbientBlocked)
                    return refuse(WFS_E_GIT_POLICY,
                        "no free World branch name could be found: too many candidates have "
                        "branch.<name> settings in global or system configuration");
            }
        }
        if (available) break;
    }
    if (!available) return -EEXIST;
    const char *create[] = {"update-ref", branch, s.head.c_str(), "", nullptr};
    if (int rc = git(clone, create)) return rc;
    // A generated World branch must start without an upstream, even when stale
    // branch.<name>.* configuration for this exact short name was carried in --
    // e.g. a leftover branch.world/W1.remote/.merge left behind by a branch this
    // World once had, or, for a World forked from a World, configuration copied
    // wholesale from the parent. Remove any section for the new branch's short
    // name before HEAD is pointed at it.
    const char *short_name = branch + 11; // strip the "refs/heads/" prefix
    {
        String section("branch."); section.append(short_name);
        const char *remove[] = {"config", "--local", "--remove-section", section.c_str(), nullptr};
        int status = -1;
        // A missing section exits 128 ("no such section"); that is the common case
        // (nothing was carried) and is not an error here.
        int rc = git(clone, remove, nullptr, &status, true);
        if (rc && !(rc == WFS_E_GIT_FAILED && status == 128)) return rc;
        // Verify by name rather than trusting --remove-section's exit status alone:
        // build a regex that matches this exact short name (escaping regex metacharacters
        // it may contain) and confirm no branch.<name>.* key remains.
        String pattern = branch_section_pattern(short_name);
        // With --includes: a key an included file sets survives --remove-section.
        const char *check[] = {"config", "--local", "--includes", "--name-only", "--get-regexp", pattern.c_str(), nullptr};
        int check_status = -1;
        rc = git(clone, check, nullptr, &check_status, true);
        if (rc == 0)
            return refuse(WFS_E_GIT_POLICY, "branch.%s settings come from a file the World's configuration "
                          "includes, which WorldFS cannot remove; the new World's branch would not start "
                          "without an upstream", short_name);
        if (!(rc == WFS_E_GIT_FAILED && check_status == 1)) return rc; // unexpected failure
    }
    const char *checkout[] = {"symbolic-ref", "HEAD", branch, nullptr};
    if (int rc = git(clone, checkout)) return rc;
    if (int rc = config(clone, "worldfs.baseline", s.head.c_str())) return rc;
    return 0;
}
} // namespace wfs

namespace wfs {
// Whether `ref` is checked out in any worktree of `repo`. Records of `worktree list
// --porcelain -z` are NUL-terminated fields with an empty field between worktrees: the whole
// buffer is walked (git() appends one final NUL), not just the first record.
int branch_checked_out(const char *repo, const String &ref, bool &out) {
    out = false;
    Vec<char> worktrees;
    const char *wt_args[] = {"worktree", "list", "--porcelain", "-z", nullptr};
    if (int rc = git(repo, wt_args, &worktrees)) return rc;
    String checked("branch "); checked.append(ref.c_str());
    for (size_t i = 0; i + 1 < worktrees.size();) {
        const char *line = worktrees.data() + i;
        if (!strcmp(line, checked.c_str())) { out = true; return 0; }
        i += strlen(line) + 1;
    }
    return 0;
}
}

namespace wfs {
// The initialized submodules of the repository at `root`, recursively, as paths below it: the
// directories its index records as gitlinks that hold a `.git`.
// Each one is checked to be in the World's own layout (check_owned_module) before anything is
// read from it or below it.
int checked_out_modules(const char *world, const char *root, const String &prefix, const String &gitdir_prefix,
                        Vec<String> &out, int depth) {
    const char *ls_args[] = {"ls-files", "--stage", "-z", nullptr};
    Vec<char> listing, gitmodules;
    if (int rc = git(root, ls_args, &listing)) return rc;
    Vec<Gitlink> links;
    parse_gitlinks(listing, links);
    if (links.empty()) return 0;
    if (int rc = gitmodules_listing(root, nullptr, gitmodules)) return rc;
    Vec<GitSetting> names;
    module_names(gitmodules, names);
    for (const auto &l : links) {
        String full = joinp(root, l.path.c_str()), dot = joinp(full.c_str(), ".git");
        String path(prefix); path.append(l.path.c_str());
        struct stat st;
        if (lstat(dot.c_str(), &st)) {
            if (errno == ENOENT || errno == ENOTDIR) continue;   // uninitialized or deleted
            return -errno;
        }
        if (depth >= kMaxModuleDepth)
            return refuse(WFS_E_GIT_UNSUPPORTED, "submodule %s is nested more than %d levels deep", path.c_str(), kMaxModuleDepth);
        const String *name = module_name(names, l.path);
        if (!name || !valid_module_name(*name))
            return refuse(WFS_E_GIT_UNSUPPORTED, "submodule %s has no usable .gitmodules entry", path.c_str());
        String gitdir(gitdir_prefix); gitdir.append("modules/"); gitdir.append(name->c_str());
        if (int rc = check_owned_module(world, path, gitdir)) return rc;
        out.emplace_back(path);
        String child(path); child.push_back('/');
        String child_gitdir(gitdir); child_gitdir.push_back('/');
        if (int rc = checked_out_modules(world, full.c_str(), child, child_gitdir, out, depth + 1)) return rc;
    }
    return 0;
}
// Publish copies only the root's commits. A gitlink commit the published range introduces -- one
// that a commit reachable from `now` but from no other ref of the target records, in any diff
// against a parent (merges against each) -- must already be in the target's own checked-out
// submodule at that path, or the target could not check the published commits out. Nothing is
// fetched into or pushed from any submodule. `worktree` is the target's top level, or null for a
// bare target, which has no submodule checked out.
int check_published_gitlinks(const char *repo, const char *worktree, const char *staging, const String &now) {
    String exclude("--exclude="); exclude.append(staging);
    const char *range[] = {"--no-replace-objects", "rev-list", now.c_str(), exclude.c_str(), "--not", "--all", nullptr};
    Vec<char> commits;
    if (int rc = git(repo, range, &commits)) return rc;
    if (commits.size() <= 1) return 0;
    FILE *input = tmpfile();
    if (!input) return -errno;
    if (fwrite(commits.data(), 1, commits.size() - 1, input) != commits.size() - 1 || fflush(input)) {
        int err = errno ? -errno : -EIO; fclose(input); return err;
    }
    rewind(input);
    const char *diff[] = {"--no-replace-objects", "diff-tree", "--stdin", "-r", "-m", "--root", "-z", "--no-renames",
                          "--no-commit-id", nullptr};
    Vec<char> raw;
    int rc = git(repo, diff, &raw, nullptr, false, false, fileno(input));
    fclose(input);
    if (rc) return rc;
    // Records are ":<old mode> <new mode> <old oid> <new oid> <status>\0<path>\0".
    Vec<GitSetting> links;   // {path, commit}
    for (size_t i = 0; i < raw.size() && raw[i];) {
        const char *header = raw.data() + i;
        i += strlen(header) + 1;
        if (header[0] != ':' || i >= raw.size()) continue;
        const char *path = raw.data() + i;
        i += strlen(path) + 1;
        char old_mode[8], new_mode[8], old_oid[72], new_oid[72], status[8];
        if (sscanf(header, ":%7s %7s %71s %71s %7s", old_mode, new_mode, old_oid, new_oid, status) != 5) continue;
        if (strcmp(new_mode, "160000") || status[0] == 'D') continue;
        bool seen = false;
        for (const auto &l : links) if (l.key == path && l.value == new_oid) { seen = true; break; }
        if (!seen) links.emplace_back(GitSetting{String(path), String(new_oid)});
    }
    for (const auto &l : links) {
        bool checked_out = false;
        String full;
        if (worktree) {
            full = joinp(worktree, l.key.c_str());
            String dot = joinp(full.c_str(), ".git"), top, real_full, real_top;
            const char *top_args[] = {"rev-parse", "--show-toplevel", nullptr};
            struct stat st;
            checked_out = !lstat(dot.c_str(), &st) && !value(full.c_str(), top_args, top) &&
                          !fs_realpath(full.c_str(), real_full) && !fs_realpath(top.c_str(), real_top) && real_full == real_top;
        }
        if (!checked_out)
            return refuse(WFS_E_GIT_TARGET, "the published commits record submodule %s at %s, but %s does not have that submodule initialized, so it could not check them out; initialize it there first",
                          l.key.c_str(), l.value.c_str(), repo);
        String spec(l.value); spec.append("^{commit}");
        const char *exists[] = {"cat-file", "-e", spec.c_str(), nullptr};
        if (git(full.c_str(), exists, nullptr, nullptr, true))
            return refuse(WFS_E_GIT_TARGET, "the published commits record submodule %s at %s, which the submodule in %s does not have; get that commit there first (publish copies only the root's commits)",
                          l.key.c_str(), l.value.c_str(), repo);
    }
    return 0;
}

struct LfsPointerRecord { String oid; uint64_t size; };

bool parse_lfs_pointer(const char *data, size_t length, LfsPointerRecord &out, bool &looks_like_pointer) {
    static const char version[] = "version https://git-lfs.github.com/spec/v1\n";
    looks_like_pointer = length >= sizeof(version) - 1 && !memcmp(data, version, sizeof(version) - 1);
    if (!looks_like_pointer) return false;
    // Only the canonical, extension-free v1 form is supported. Do not silently publish
    // history whose LFS pointer Git LFS would interpret differently.
    if (length < sizeof(version) - 1 + 6 + 64 + 6) return false;
    size_t pos = sizeof(version) - 1;
    if (memcmp(data + pos, "oid sha256:", 11)) return false;
    pos += 11;
    String oid(data + pos, 64); pos += 64;
    if (oid.size() != 64) return false;
    for (char c : oid) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    if (pos >= length || data[pos++] != '\n' || length - pos < 5 || memcmp(data + pos, "size ", 5)) return false;
    pos += 5;
    if (pos >= length || data[pos] < '0' || data[pos] > '9') return false;
    size_t size_start = pos;
    uint64_t size = 0;
    for (; pos < length && data[pos] >= '0' && data[pos] <= '9'; ++pos) {
        unsigned digit = (unsigned)(data[pos] - '0');
        if (size > (UINT64_MAX - digit) / 10) return false;
        size = size * 10 + digit;
    }
    if (pos - size_start > 1 && data[size_start] == '0') return false;
    if (pos >= length || data[pos++] != '\n' || pos != length) return false;
    out = LfsPointerRecord{oid, size};
    return true;
}

bool lfs_manifest_has(const Vec<char> &manifest, const String &oid, uint64_t size) {
    for (size_t i = 0; i < manifest.size();) {
        String rel(manifest.data() + i); i += rel.size() + 1;
        String listed_oid(manifest.data() + i); i += listed_oid.size() + 1;
        String listed_size(manifest.data() + i); i += listed_size.size() + 1;
        if (listed_oid == oid.c_str() && strtoull(listed_size.c_str(), nullptr, 10) == size) return true;
    }
    return false;
}

// Open/create one path component relative to an already-validated directory descriptor.
int open_lfs_dir(int parent, const char *name, bool create) {
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0 || !create || errno != ENOENT) return fd;
    if (mkdirat(parent, name, 0700) && errno != EEXIST) return -1;
    if (errno != EEXIST && fsync(parent)) return -1;
    return openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

int verify_named_lfs_payload(const char *world, int dirfd, const LfsPointerRecord &record) {
    struct stat named_before;
    if (fstatat(dirfd, record.oid.c_str(), &named_before, AT_SYMLINK_NOFOLLOW) ||
        !S_ISREG(named_before.st_mode) || named_before.st_size < 0 ||
        (uint64_t)named_before.st_size != record.size)
        return refuse(WFS_E_GIT_TARGET, "the target Git LFS cache has a corrupt object %s", record.oid.c_str());

    int fd = openat(dirfd, record.oid.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat before;
    if (fd < 0 || fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        (uint64_t)before.st_size != record.size || before.st_dev != named_before.st_dev ||
        before.st_ino != named_before.st_ino) {
        if (fd >= 0) close(fd);
        return refuse(WFS_E_GIT_TARGET, "the target Git LFS cache object %s changed or is corrupt", record.oid.c_str());
    }

    const char *file_arg = "--file=/dev/stdin";
    Vec<char> first_hash, second_hash;
    int rc = lseek(fd, 0, SEEK_SET) < 0 ? -errno : lfs_pointer_oracle(world, file_arg, &first_hash, fd);
    if (!rc && lseek(fd, 0, SEEK_SET) < 0) rc = -errno;
    if (!rc) rc = lfs_pointer_oracle(world, file_arg, &second_hash, fd);
    bool valid_hash = false;
    if (!rc) {
        LfsPointerRecord got; bool looks = false;
        valid_hash = parse_lfs_pointer(first_hash.data(), first_hash.size() - 1, got, looks) &&
            got.oid == record.oid && got.size == record.size && same_bytes(first_hash, second_hash);
        if (!valid_hash) rc = refuse(WFS_E_GIT_TARGET,
            "the target Git LFS cache object %s changed or is corrupt", record.oid.c_str());
    }

    struct stat after, named_after;
    bool after_ok = !fstat(fd, &after);
    bool named_ok = !fstatat(dirfd, record.oid.c_str(), &named_after, AT_SYMLINK_NOFOLLOW);
    bool same_times = false;
    bool same_named_times = false;
#if defined(__APPLE__)
    same_times = after_ok && before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
        before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
        before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
        before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
    same_named_times = after_ok && named_ok && after.st_mtimespec.tv_sec == named_after.st_mtimespec.tv_sec &&
        after.st_mtimespec.tv_nsec == named_after.st_mtimespec.tv_nsec &&
        after.st_ctimespec.tv_sec == named_after.st_ctimespec.tv_sec &&
        after.st_ctimespec.tv_nsec == named_after.st_ctimespec.tv_nsec;
#else
    same_times = after_ok && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
        before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
        before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
    same_named_times = after_ok && named_ok && after.st_mtim.tv_sec == named_after.st_mtim.tv_sec &&
        after.st_mtim.tv_nsec == named_after.st_mtim.tv_nsec &&
        after.st_ctim.tv_sec == named_after.st_ctim.tv_sec &&
        after.st_ctim.tv_nsec == named_after.st_ctim.tv_nsec;
#endif
    bool stable = after_ok && named_ok &&
        S_ISREG(named_after.st_mode) && before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
        before.st_size == after.st_size && same_times && after.st_dev == named_after.st_dev &&
        after.st_ino == named_after.st_ino && after.st_size == named_after.st_size && same_named_times;
    close(fd);
    if (rc) return rc;
    if (!stable) return refuse(WFS_E_GIT_TARGET,
        "the target Git LFS cache object %s changed or is corrupt", record.oid.c_str());
    return 0;
}

int install_lfs_payload(const char *world, const char *source_common, const char *target_common,
                        const LfsPointerRecord &record) {
    char first[3] = {record.oid[0], record.oid[1], 0};
    char second[3] = {record.oid[2], record.oid[3], 0};
    int root = open(target_common, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) return -errno;
    int lfs = open_lfs_dir(root, "lfs", true); close(root);
    if (lfs < 0) return -errno;
    int objects = open_lfs_dir(lfs, "objects", true); close(lfs);
    if (objects < 0) return -errno;
    int a = open_lfs_dir(objects, first, true); close(objects);
    if (a < 0) return -errno;
    int b = open_lfs_dir(a, second, true); close(a);
    if (b < 0) return -errno;
    struct stat st;
    if (!fstatat(b, record.oid.c_str(), &st, AT_SYMLINK_NOFOLLOW)) {
        int verify_rc = verify_named_lfs_payload(world, b, record);
        close(b);
        return verify_rc;
    }
    if (errno != ENOENT) { int err = -errno; close(b); return err; }

    int source_root = open(source_common, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (source_root < 0) { close(b); return -errno; }
    int source_lfs = open_lfs_dir(source_root, "lfs", false); close(source_root);
    if (source_lfs < 0) { close(b); return refuse(WFS_E_GIT_TARGET, "the World is missing Git LFS object %s", record.oid.c_str()); }
    int source_objects = open_lfs_dir(source_lfs, "objects", false); close(source_lfs);
    if (source_objects < 0) { close(b); return refuse(WFS_E_GIT_TARGET, "the World is missing Git LFS object %s", record.oid.c_str()); }
    int source_a = open_lfs_dir(source_objects, first, false); close(source_objects);
    if (source_a < 0) { close(b); return refuse(WFS_E_GIT_TARGET, "the World is missing Git LFS object %s", record.oid.c_str()); }
    int source_b = open_lfs_dir(source_a, second, false); close(source_a);
    if (source_b < 0) { close(b); return refuse(WFS_E_GIT_TARGET, "the World is missing Git LFS object %s", record.oid.c_str()); }
    int source = openat(source_b, record.oid.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC); close(source_b);
    if (source < 0) { close(b); return refuse(WFS_E_GIT_TARGET, "the World is missing Git LFS object %s", record.oid.c_str()); }
    struct stat source_st;
    if (fstat(source, &source_st) || !S_ISREG(source_st.st_mode) || source_st.st_size < 0 ||
        (uint64_t)source_st.st_size != record.size) {
        close(source); close(b); return refuse(WFS_E_GIT_TARGET, "the World Git LFS object %s changed or is corrupt", record.oid.c_str());
    }
    unsigned char nonce[8];
    if (getentropy(nonce, sizeof nonce)) { int err = -errno; close(source); close(b); return err; }
    char temp[64]; snprintf(temp, sizeof temp, ".wfs-%02x%02x%02x%02x%02x%02x%02x%02x",
        nonce[0], nonce[1], nonce[2], nonce[3], nonce[4], nonce[5], nonce[6], nonce[7]);
    int output = openat(b, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (output < 0) { int err = -errno; close(source); close(b); return err; }
    int rc = 0; char buf[65536]; uint64_t copied = 0;
    for (;;) {
        ssize_t n = read(source, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { rc = -errno; break; }
        if (!n) break;
        size_t left = (size_t)n; const char *p = buf;
        while (left) {
            ssize_t w = write(output, p, left);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { rc = w < 0 ? -errno : -EIO; break; }
            p += w; left -= (size_t)w; copied += (uint64_t)w;
        }
        if (rc) break;
    }
    if (!rc && copied != record.size) rc = -EIO;
    if (!rc && fsync(output)) rc = -errno;
    close(source);
    if (close(output) && !rc) rc = -errno;
    if (!rc) {
        String temp_path = joinp(target_common, "lfs/objects");
        temp_path = joinp(temp_path.c_str(), first); temp_path = joinp(temp_path.c_str(), second); temp_path = joinp(temp_path.c_str(), temp);
        String file_arg("--file="); file_arg.append(temp_path.c_str());
        Vec<char> parsed;
        rc = lfs_pointer_oracle(world, file_arg.c_str(), &parsed);
        if (!rc) {
            LfsPointerRecord got; bool looks = false;
            if (!parse_lfs_pointer(parsed.data(), parsed.size() - 1, got, looks) || got.oid != record.oid || got.size != record.size)
                rc = refuse(WFS_E_GIT_TARGET, "the World Git LFS object %s changed or is corrupt", record.oid.c_str());
        }
    }
    if (!rc && linkat(b, temp, b, record.oid.c_str(), 0)) {
        if (errno == EEXIST) {
            rc = verify_named_lfs_payload(world, b, record);
        } else rc = -errno;
    }
    if (!rc && fsync(b)) rc = -errno;
    (void)unlinkat(b, temp, 0);
    close(b);
    return rc;
}

int publish_lfs(const char *world, const char *target, const String &now) {
    String source_common, target_common;
    const char *common_args[] = {"rev-parse", "--path-format=absolute", "--git-common-dir", nullptr};
    if (int rc = value(world, common_args, source_common)) return rc;
    if (int rc = value(target, common_args, target_common)) return rc;
    String source_cache = joinp(source_common.c_str(), "lfs/objects");
    String target_cache = joinp(target_common.c_str(), "lfs/objects");
    uint64_t total = 0, entries = 0; bool present = false; Vec<char> source_manifest, target_manifest;

    const char *commits_args[] = {"--no-replace-objects", "rev-list", now.c_str(), nullptr};
    Vec<char> commits;
    if (int rc = git(target, commits_args, &commits)) return rc;
    if (commits.empty() || !commits[0]) return 0;
    struct AttrRepoCleanup {
        String root, objects;
        ~AttrRepoCleanup() {
            if (!objects.empty()) (void)unlink(objects.c_str());
            if (!root.empty()) (void)fs_remove_tree(root.c_str());
        }
    } attr_repo;
    char scratch[] = "/tmp/worldfs-lfs-attrs-XXXXXX";
    if (!mkdtemp(scratch)) return -errno;
    attr_repo.root = scratch;
    String scratch_repo = joinp(scratch, "repo.git");
    String object_format;
    const char *format_args[] = {"rev-parse", "--show-object-format", nullptr};
    if (int rc = value(target, format_args, object_format)) return rc;
    String format_arg("--object-format="); format_arg.append(object_format.c_str());
    const char *init_args[] = {"init", "--bare", "--template=", "--quiet", format_arg.c_str(), "--", scratch_repo.c_str(), nullptr};
    if (int rc = git(target, init_args)) return rc;
    attr_repo.objects = joinp(scratch_repo.c_str(), "objects");
    if (int rc = fs_remove_tree(attr_repo.objects.c_str())) return rc;
    String target_objects = joinp(target_common.c_str(), "objects");
    if (symlink(target_objects.c_str(), attr_repo.objects.c_str())) return -errno;
    Vec<LfsPointerRecord> pointers;
    auto add_pointer_blob = [&](const String &blob_oid) -> int {
        const char *size_args[] = {"--no-replace-objects", "cat-file", "-s", blob_oid.c_str(), nullptr};
        Vec<char> size_text;
        if (int rc = git(target, size_args, &size_text)) return rc;
        char *end = nullptr;
        unsigned long long blob_size = strtoull(size_text.data(), &end, 10);
        if (!end || end == size_text.data() || blob_size > 1024) return 0;
        const char *blob_args[] = {"--no-replace-objects", "cat-file", "blob", blob_oid.c_str(), nullptr};
        Vec<char> blob;
        if (int rc = git(target, blob_args, &blob)) return rc;
        LfsPointerRecord record; bool looks = false;
        bool valid = parse_lfs_pointer(blob.data(), blob.size() - 1, record, looks);
        if (looks && !valid)
            return refuse(WFS_E_GIT_TARGET, "published history contains a noncanonical or unsupported Git LFS pointer");
        if (!valid) return 0;
        for (const auto &p : pointers) if (p.oid == record.oid) {
            if (p.size != record.size) return refuse(WFS_E_GIT_TARGET, "published history has conflicting Git LFS sizes for %s", record.oid.c_str());
            return 0;
        }
        pointers.emplace_back(record);
        return 0;
    };
    auto regular_mode = [](const char *mode) { return !strcmp(mode, "100644") || !strcmp(mode, "100755"); };
    for (size_t ci = 0; ci + 1 < commits.size();) {
        size_t end = ci; while (end + 1 < commits.size() && commits[end] != '\n') ++end;
        if (end == ci) { ci = end + 1; continue; }
        String commit(commits.data() + ci, end - ci); ci = end + 1;
        if (int rc = validate_lfs_tree_config(target, commit.c_str())) return rc;
        const char *diff_args[] = {"--no-replace-objects", "diff-tree", "--no-commit-id", "--root", "-r", "-m", "-z", "--raw", "--no-renames", commit.c_str(), nullptr};
        Vec<char> diff;
        if (int rc = git(target, diff_args, &diff)) return rc;
        Vec<GitSetting> files;
        bool attributes_changed = false;
        for (size_t i = 0; i + 1 < diff.size();) {
            const char *header = diff.data() + i; i += strlen(header) + 1;
            if (header[0] != ':' || i >= diff.size()) continue;
            const char *path = diff.data() + i; i += strlen(path) + 1;
            char old_mode[8], new_mode[8], old_oid[72], new_oid[72], status[8];
            if (sscanf(header, ":%7s %7s %71s %71s %7s", old_mode, new_mode, old_oid, new_oid, status) != 5) continue;
            size_t plen = strlen(path);
            static const char root_attr[] = ".gitattributes";
            static const char nested_attr[] = "/.gitattributes";
            if ((plen == sizeof(root_attr) - 1 && !strcmp(path, root_attr)) ||
                (plen >= sizeof(nested_attr) - 1 && !strcmp(path + plen - (sizeof(nested_attr) - 1), nested_attr)))
                attributes_changed = true;
            if (regular_mode(new_mode) && status[0] != 'D') files.emplace_back(GitSetting{String(path), String(new_oid)});
        }
        if (attributes_changed) {
            const char *tree_args[] = {"--no-replace-objects", "ls-tree", "-r", "-z", "--full-tree", commit.c_str(), nullptr};
            Vec<char> tree;
            if (int rc = git(target, tree_args, &tree)) return rc;
            for (size_t i = 0; i + 1 < tree.size();) {
                const char *record = tree.data() + i; i += strlen(record) + 1;
                const char *tab = strchr(record, '\t');
                if (!tab) continue;
                char mode[8], type[8], oid[72];
                if (sscanf(record, "%7s %7s %71s", mode, type, oid) != 3 || strcmp(type, "blob") || !regular_mode(mode)) continue;
                const char *path = tab + 1;
                size_t plen = strlen(path);
                bool found = false; for (const auto &f : files) if (f.key == path) { found = true; break; }
                if (!found) files.emplace_back(GitSetting{String(path, plen), String(oid)});
            }
        }
        if (files.empty()) continue;
        FILE *paths = tmpfile();
        if (!paths) return -errno;
        for (const auto &file : files) {
            if (fwrite(file.key.c_str(), 1, file.key.size() + 1, paths) != file.key.size() + 1) {
                int err = errno ? -errno : -EIO; fclose(paths); return err;
            }
        }
        if (fflush(paths)) { int err = -errno; fclose(paths); return err; }
        rewind(paths);
        String source("--source="); source.append(commit.c_str());
        const char *attr_args[] = {"--no-replace-objects", "-c", "core.attributesFile=/dev/null",
            "check-attr", "--stdin", "-z", source.c_str(), "filter", nullptr};
        Vec<char> attrs;
        int attr_rc = git(scratch_repo.c_str(), attr_args, &attrs, nullptr, false, false, fileno(paths), false, true);
        fclose(paths);
        if (attr_rc) return attr_rc;
        size_t ai = 0;
        for (const auto &file : files) {
            if (ai + 1 >= attrs.size()) return refuse(WFS_E_GIT_TARGET, "Git returned an incomplete attribute listing for published history");
            const char *path = attrs.data() + ai; ai += strlen(path) + 1;
            const char *attribute = attrs.data() + ai; ai += strlen(attribute) + 1;
            const char *value_text = attrs.data() + ai; ai += strlen(value_text) + 1;
            if (file.key != path || strcmp(attribute, "filter"))
                return refuse(WFS_E_GIT_TARGET, "Git returned an unexpected attribute listing for published history");
            if (!strcmp(value_text, "lfs"))
                if (int rc = add_pointer_blob(file.value)) return rc;
        }
    }
    if (pointers.empty()) return 0;
    if (int rc = validate_lfs_storage(target)) return rc;
    if (int rc = lfs_import_bytes(target, target_cache.c_str(), total, entries, present, target_manifest)) return rc;
    bool needs_source = false;
    for (const auto &p : pointers) if (!lfs_manifest_has(target_manifest, p.oid, p.size)) needs_source = true;
    if (needs_source) {
        if (int rc = lfs_import_bytes(world, source_cache.c_str(), total, entries, present, source_manifest)) return rc;
    }
    for (const auto &p : pointers) {
        if (lfs_manifest_has(target_manifest, p.oid, p.size)) continue;
        if (!lfs_manifest_has(source_manifest, p.oid, p.size))
            return refuse(WFS_E_GIT_TARGET, "the World is missing Git LFS object %s", p.oid.c_str());
    }
    for (const auto &p : pointers) {
        if (int rc = install_lfs_payload(world, source_common.c_str(), target_common.c_str(), p)) return rc;
    }
    return 0;
}
}

extern "C" const char *wfs_git_reason(void) { return wfs::g_reason; }
namespace wfs {
void git_clear_omitted() { g_omitted.clear(); }
}
extern "C" const char *wfs_git_omitted_worktrees(void) { return wfs::g_omitted.c_str(); }

extern "C" int wfs_git_uncarried_hooks(const char *root) {
    if (!root) return -EINVAL;
    wfs::String dot = wfs::joinp(root, ".git"), owned = wfs::joinp(root, ".world-git");
    struct stat st;
    if (lstat(dot.c_str(), &st)) return errno == ENOENT ? 0 : -errno;
    if (!lstat(owned.c_str(), &st)) return 0;   // a managed World's hooks travel with it
    if (errno != ENOENT) return -errno;
    bool present = false; wfs::String path;
    if (int rc = wfs::local_hooks_path(root, present, path)) return rc;
    if (present) return 1;
    wfs::String dir;
    if (int rc = wfs::hooks_dir(root, dir)) return rc;
    // Anything --with-hooks would carry or refuse to follow is worth the note.
    DIR *d = opendir(dir.c_str());
    if (!d) return errno == ENOENT || errno == ENOTDIR ? 0 : -errno;
    int found = 0;
    for (;;) {
        errno = 0; dirent *e = readdir(d);
        if (!e) { if (errno) found = -errno; break; }
        if (e->d_name[0] == '.' || wfs::is_sample(e->d_name)) continue;
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
        if (S_ISLNK(st.st_mode) || (S_ISREG(st.st_mode) && (st.st_mode & 0111))) { found = 1; break; }
    }
    closedir(d);
    return found;
}

extern "C" int wfs_git_inspect(const char *root, wfs_git_info *out) {
    if (!root || !out) return -EINVAL;
    memset(out, 0, sizeof *out);
    wfs::String dot = wfs::joinp(root, ".git"); struct stat st;
    if (lstat(dot.c_str(), &st)) return errno == ENOENT ? 0 : -errno;
    wfs::String owned = wfs::joinp(root, ".world-git");
    if (lstat(owned.c_str(), &st)) return errno == ENOENT ? 0 : -errno;
    wfs::Vec<char> contents;
    if (int rc = wfs::read_bytes(dot.c_str(), contents)) return rc;
    if (contents.size() != strlen(wfs::marker) || memcmp(contents.data(), wfs::marker, contents.size()))
        return wfs::refuse(WFS_E_GIT_UNSUPPORTED, "the .git file is not the WorldFS marker");
    wfs::String branch, base, common, head;
    const char *head_args[] = {"rev-parse", "--verify", "HEAD^{commit}", nullptr};
    if (int rc = wfs::value(root, head_args, head)) return rc;
    const char *branch_args[] = {"symbolic-ref", "--quiet", "HEAD", nullptr};
    const char *base_args[] = {"config", "--local", "--get", "worldfs.baseline", nullptr};
    const char *common_args[] = {"rev-parse", "--path-format=absolute", "--git-common-dir", nullptr};
    // Detached HEAD after an explicit user checkout is valid; an empty branch reports it
    // (symbolic-ref exits 1 without output). The full symbolic target is read rather than
    // rev-parse --abbrev-ref, which reports "heads/<name>" when a tag shares the branch name.
    // Like `git branch --show-current`, a symbolic HEAD outside refs/heads/ is not a branch.
    if (int rc = wfs::value(root, branch_args, branch, true)) return rc;
    if (!strncmp(branch.c_str(), "refs/heads/", 11) && branch.size() > 11) {
        wfs::String name(branch.c_str() + 11, branch.size() - 11);
        branch = name;
    } else branch.clear();
    if (int rc = wfs::value(root, base_args, base)) return rc;
    if (int rc = wfs::value(root, common_args, common)) return rc;
    if (head.size() >= sizeof out->head || branch.size() >= sizeof out->branch ||
        base.size() >= sizeof out->baseline || common.size() >= sizeof out->git_dir)
        return -EOVERFLOW;
    out->present = 1;
    snprintf(out->head, sizeof out->head, "%s", head.c_str());
    snprintf(out->branch, sizeof out->branch, "%s", branch.c_str());
    snprintf(out->baseline, sizeof out->baseline, "%s", base.c_str());
    snprintf(out->git_dir, sizeof out->git_dir, "%s", common.c_str());
    return 0;
}

extern "C" int wfs_git_publish(const char *world_root, const char *repo, const char *branch, int force,
                               wfs_git_publish_result *out) {
    using namespace wfs;
    if (!world_root || !repo || !*repo || !out) return -EINVAL;
    memset(out, 0, sizeof *out);
    g_reason[0] = '\0';
    wfs_git_info info;
    if (int rc = wfs_git_inspect(world_root, &info)) return rc;
    if (!info.present) return refuse(WFS_E_GIT_UNSUPPORTED, "the World has no WorldFS-managed Git repository");
    // Same protection as fork/checkpoint: a symlinked or foreign administration must not be
    // published as this World. wfs_git_inspect only follows the fixed .git marker; it does not
    // validate that .world-git and everything beneath it are still the World's own. The World's
    // other linked worktrees are separate checkouts; publishing sends only its branch.
    Vec<GitWorktree> worktrees;
    {
        String common, admin;
        const char *common_args[] = {"rev-parse", "--path-format=absolute", "--git-common-dir", nullptr};
        const char *admin_args[] = {"rev-parse", "--absolute-git-dir", nullptr};
        int rc;
        if ((rc = value(world_root, common_args, common)) || (rc = value(world_root, admin_args, admin))) return rc;
        if ((rc = managed_check(world_root, common.c_str(), admin.c_str(), worktrees))) return rc;
    }
    // Import guarantees the preserved history is clean (reject_reserved_paths), so a commit that
    // tracks .world or .world-git can only have been made afterwards, by force-adding a reserved
    // path and committing it. info.head is the commit wfs_git_inspect just inspected; the later
    // `now == info.head` check refuses if the fetch's live branch moved to anything else, so
    // checking info.head here (rather than the live branch, which could move again before that
    // check runs) is enough to cover whatever is actually published. --full-history walks every
    // commit reachable from info.head, not only ones added since import, because the target must
    // not receive these paths through any commit being published -- a later clean commit on top
    // does not clear an earlier one out of history. Same pathspecs/flags as reject_reserved_paths.
    // This is scanned twice: once with --no-replace-objects, which sees the real commits and
    // trees the import preserved (a replacement could otherwise present a safe tree for a
    // commit whose real tree is reserved -- same reasoning as reject_reserved_paths), and once
    // with replacement refs explicitly honored (-c core.useReplaceRefs=true, without
    // --no-replace-objects -- default git() calls neither disable nor redirect replacements, so
    // this only needs to override a local core.useReplaceRefs=false that might otherwise turn
    // them off), since an active refs/replace/* the target carries identically (required by the
    // symmetric replacement-ref check below) would otherwise let a reserved path reach the
    // target through the replaced view alone. Either scan finding it is enough to refuse; run
    // both before the staging ref is created, so a refusal here needs no cleanup.
    {
        const char *reserved_args[] = {"--no-replace-objects", "rev-list", "-n", "1", "--full-history",
            info.head, "--", ":(top,literal).world", ":(top,literal).world-git", nullptr};
        Vec<char> hit;
        if (int rc = git(world_root, reserved_args, &hit)) return rc;
        const char *reserved_args_replaced[] = {"-c", "core.useReplaceRefs=true", "rev-list", "-n", "1",
            "--full-history", info.head, "--", ":(top,literal).world", ":(top,literal).world-git", nullptr};
        Vec<char> hit_replaced;
        if (int rc = git(world_root, reserved_args_replaced, &hit_replaced)) return rc;
        if (hit.size() > 1 || hit_replaced.size() > 1)
            return refuse(WFS_E_GIT_TARGET,
                "the World's commit %s or its history tracks the reserved path .world or .world-git; remove it from history before publishing",
                info.head);
    }
    if (!branch || !*branch) {
        if (!info.branch[0])
            return refuse(WFS_E_GIT_TARGET, "the World's HEAD is detached; name the branch to create with --branch");
        branch = info.branch;
    }
    String ref("refs/heads/");
    ref.append(branch);
    if (ref.size() >= sizeof out->ref) return -ENAMETOOLONG;
    const char *check_ref[] = {"check-ref-format", ref.c_str(), nullptr};
    if (git(world_root, check_ref, nullptr, nullptr, true))
        return refuse(WFS_E_GIT_TARGET, "%s is not a valid branch name", branch);
    // The target must be a repository of its own -- its top level, or a bare repository itself,
    // not a directory that merely sits inside some other repository -- and not this World.
    String world_real, repo_real, top, git_dir, bare;
    if (fs_realpath(repo, repo_real)) return refuse(WFS_E_GIT_TARGET, "%s does not exist", repo);
    const char *bare_args[] = {"rev-parse", "--is-bare-repository", nullptr};
    if (value(repo, bare_args, bare)) return refuse(WFS_E_GIT_TARGET, "%s is not a Git repository", repo);
    const char *where[] = {"rev-parse", bare == "true" ? "--absolute-git-dir" : "--show-toplevel", nullptr};
    String real_where;
    if (value(repo, where, top) || fs_realpath(top.c_str(), real_where) || real_where != repo_real)
        return refuse(WFS_E_GIT_TARGET, "%s is not a Git repository (it is not the top of one)", repo);
    if (fs_realpath(world_root, world_real)) return refuse(WFS_E_GIT_TARGET, "%s does not exist", world_root);
    if (world_real == repo_real)
        return refuse(WFS_E_GIT_TARGET, "the target repository is the World itself");
    // The target may be a distinct worktree of, or a bare path naming, the World's own private
    // repository (for instance <world>/.world-git/repo.git) rather than the World's own working
    // tree -- same common Git directory, different top-level path -- which the check above does
    // not catch. Compare canonical common directories instead to refuse that too.
    {
        String target_common;
        const char *common_args[] = {"rev-parse", "--path-format=absolute", "--git-common-dir", nullptr};
        if (value(repo, common_args, target_common))
            return refuse(WFS_E_GIT_TARGET, "%s is not a Git repository", repo);
        String target_common_real;
        if (fs_realpath(target_common.c_str(), target_common_real))
            return refuse(WFS_E_GIT_TARGET, "%s does not exist", target_common.c_str());
        String world_common_real;
        if (int wrc = fs_realpath(info.git_dir, world_common_real)) return wrc;
        if (target_common_real == world_common_real)
            return refuse(WFS_E_GIT_TARGET, "the target repository is the World's own repository (%s)",
                target_common_real.c_str());
    }
    // A symbolic destination would be dereferenced by the update and move whatever it points
    // at (a checked-out main, a ref outside refs/heads); publish writes plain branches only.
    {
        const char *sym[] = {"symbolic-ref", "--quiet", ref.c_str(), nullptr};
        int status = -1;
        int src = git(repo, sym, nullptr, &status, true);
        if (!src) return refuse(WFS_E_GIT_TARGET, "%s is a symbolic ref in the target repository; publish only writes plain branches", branch);
        if (!(src == WFS_E_GIT_FAILED && status == 1)) return src;
    }
    // A branch checked out in any worktree of the target would be moved under its user.
    bool checked_out = false;
    if (int rc = branch_checked_out(repo, ref, checked_out)) return rc;
    if (checked_out)
        return refuse(WFS_E_GIT_TARGET, "%s is checked out in the target repository; choose another name with --branch", branch);
    // The World's own status is a diagnostic for the caller; take it before anything changes, so
    // an error here can never be reported for a publication that already happened.
    // Status must not execute a filter that was installed or attached after the import.
    if (int rc = reject_ambient_policy(world_root)) return rc;
    if (int rc = reject_used_filters(world_root)) return rc;
    Vec<String> modules;
    if (int rc = checked_out_modules(world_root, world_root, String(), String(), modules, 0)) return rc;
    for (const auto &m : modules) {
        String path = joinp(world_root, m.c_str());
        if (int rc = reject_used_filters(path.c_str())) return in_module(rc, m);
    }
    int dirty = require_clean_tree(world_root, &worktrees);
    for (size_t i = 0; !dirty && i < modules.size(); ++i) {
        String path = joinp(world_root, modules[i].c_str());
        dirty = require_clean_tree(path.c_str());
    }
    if (dirty && dirty != WFS_E_GIT_DIRTY) return dirty;
    String old;
    const char *old_args[] = {"rev-parse", "--verify", "--quiet", ref.c_str(), nullptr};
    if (int rc = value(repo, old_args, old, true)) return rc;
    // Bring the World's commits over under a private name first, so nothing the user sees
    // moves until every check has passed. The name carries 128 random bits, so concurrent
    // publishes (in this process or others) never share it, and it must not exist yet.
    unsigned char nonce[16];
    if (getentropy(nonce, sizeof nonce)) return -errno;
    char staging[80];
    int off = snprintf(staging, sizeof staging, "refs/worldfs/publish-");
    for (unsigned char b : nonce) off += snprintf(staging + off, sizeof staging - (size_t)off, "%02x", b);
    {
        String existing;
        const char *probe[] = {"rev-parse", "--verify", "--quiet", staging, nullptr};
        if (int prc = value(repo, probe, existing, true)) return prc;
        if (!existing.empty()) return -EEXIST;
    }
    // The target's url.<base>.insteadOf rules apply to the fetch's repository operand: one that
    // matches the World's path would fetch the same-named branch from somewhere else. Ask Git
    // what it would actually contact, and refuse unless that is the World itself. The canonical
    // (realpath'd) form is used here and for the fetch operand below, since a relative
    // world_root is resolved by Git from the target's -C directory, not the caller's -- a
    // different place than fs_realpath resolved it from above.
    {
        String effective;
        const char *get_url[] = {"ls-remote", "--get-url", world_real.c_str(), nullptr};
        if (int urc = value(repo, get_url, effective)) return urc;
        if (effective != world_real)
            return refuse(WFS_E_GIT_TARGET, "the target repository's url.*.insteadOf rewrites the World's path to %s", effective.c_str());
    }
    // Git's refs/replace/* rewrite what a commit's history and trees mean, and the fetch below
    // transfers only the branch tip -- so a replacement active in the World that the target
    // repository does not carry identically would let the very same commit id mean something
    // different once published. Check this before anything is staged, so a refusal here leaves
    // the target untouched (there is no staging ref yet to drop).
    {
        // The user's own Git honors a global or system core.useReplaceRefs, so this reads the
        // effective value with the ambient configuration Git itself would see (get_config's
        // git() call disables it), rather than only the repository-local setting.
        auto active_replacements = [](const char *root, bool &active, Vec<char> &listing) -> int {
            Vec<char> buf; int status = -1;
            const char *cfg[] = {"config", "--type=bool", "--get", "core.useReplaceRefs", nullptr};
            int rc = git(root, cfg, &buf, &status, false, true);
            if (rc == WFS_E_GIT_FAILED && status == 1) active = true;
            else if (rc) return rc;
            else active = strcmp(buf.data(), "false\n") != 0;
            listing.clear();
            if (!active) return 0;
            const char *list_args[] = {"for-each-ref", "--format=%(refname) %(objectname)", "refs/replace/", nullptr};
            return git(root, list_args, &listing);
        };
        bool world_active = false, target_active = false;
        Vec<char> world_listing, target_listing;
        if (int rc = active_replacements(world_real.c_str(), world_active, world_listing)) return rc;
        if (int rc = active_replacements(repo, target_active, target_listing)) return rc;
        // Symmetric: either side having active, non-empty replacements is enough to require the
        // other side to match exactly (same active state and byte-identical listing) -- a
        // replacement active only in the target would show the published branch through it too.
        bool world_has = world_active && world_listing.size() > 1; // more than just the trailing NUL
        bool target_has = target_active && target_listing.size() > 1;
        if ((world_has || target_has) &&
            (world_active != target_active || !same_bytes(world_listing, target_listing)))
            return refuse(WFS_E_GIT_TARGET, "%s and the World do not have identical replacement refs (refs/replace/); the published history would mean something different there", repo);
    }
    // Legacy info/grafts rewrite a commit's parents and, unlike refs/replace/*, are not disabled
    // by --no-replace-objects -- so a graft in either repository could make a diverged branch
    // look like a fast-forward or shared history to the checks below. Refuse before they run.
    {
        const char *dirs[] = {world_real.c_str(), repo};
        const char *labels[] = {"the World", repo};
        for (size_t i = 0; i < 2; ++i) {
            String grafts;
            const char *graft_args[] = {"rev-parse", "--path-format=absolute", "--git-path", "info/grafts", nullptr};
            if (int rc = value(dirs[i], graft_args, grafts)) return rc;
            struct stat st;
            if (!lstat(grafts.c_str(), &st))
                return refuse(WFS_E_GIT_TARGET, "%s has info/grafts, which changes history; remove it before publishing", labels[i]);
            if (errno != ENOENT) return -errno;
        }
    }
    String source_ref;
    if (info.branch[0]) { source_ref.assign("refs/heads/"); source_ref.append(info.branch); }
    else source_ref.assign("HEAD");
    String spec;
    spec.append(source_ref.c_str()); spec.push_back(':'); spec.append(staging);
    const char *fetch[] = {"fetch", "--quiet", "--no-tags", "--no-write-fetch-head", "--no-recurse-submodules",
                           "--", world_real.c_str(), spec.c_str(), nullptr};
    int rc = git(repo, fetch);
    // Read the staging ref whatever the fetch returned: Git can write it and still exit
    // non-zero afterwards (a commit-graph or maintenance step failing), and a ref this call
    // wrote must be taken back out below either way.
    String now;
    {
        const char *now_args[] = {"rev-parse", "--verify", "--quiet", staging, nullptr};
        int nrc = value(repo, now_args, now, true);
        if (!rc) rc = nrc;
        if (!rc && now.empty()) rc = WFS_E_GIT_FAILED;
    }
    // info.head is the commit wfs_git_inspect actually inspected and whose dirty state is
    // reported; the fetch above followed the live branch, so if the World's branch moved
    // between inspection and fetch, `now` would be a newer, uninspected commit. Publish only
    // the commit that was inspected.
    if (!rc && now != info.head)
        rc = refuse(WFS_E_GIT_TARGET, "the World's %s moved from %.12s to %.12s while publishing; nothing was changed",
                    info.branch[0] ? "branch" : "HEAD", info.head, now.c_str());
    if (!rc && !force) {
        // Shared history: at least one of the World's commits is already in the repository.
        // An unrelated repository would otherwise gain a branch with a foreign root.
        // --no-replace-objects: this judges the target's real history, ignoring any
        // replacement refs of its own (the check above already handled the World's).
        String exclude("--exclude="); exclude.append(staging);
        const char *all_args[] = {"--no-replace-objects", "rev-list", "--count", now.c_str(), nullptr};
        const char *new_args[] = {"--no-replace-objects", "rev-list", "--count", now.c_str(), exclude.c_str(), "--not", "--all", nullptr};
        String all, fresh;
        if (!(rc = value(repo, all_args, all)) && !(rc = value(repo, new_args, fresh)) && all == fresh)
            rc = refuse(WFS_E_GIT_TARGET, "%s shares no history with the World; is it the repository the World came from? (--force skips this check)", repo);
    }
    if (!rc && !force && !old.empty() && old != now) {
        const char *ff[] = {"--no-replace-objects", "merge-base", "--is-ancestor", old.c_str(), now.c_str(), nullptr};
        int status = -1;
        int ff_rc = git(repo, ff, nullptr, &status, true);
        if (ff_rc == WFS_E_GIT_FAILED && status == 1)
            rc = refuse(WFS_E_GIT_TARGET, "%s in the target repository has commits the World's branch does not; this is not a fast-forward (--force overwrites it)", branch);
        else rc = ff_rc;
    }
    if (!rc) rc = check_published_gitlinks(repo, bare == "true" ? nullptr : repo_real.c_str(), staging, now);
    // One ref transaction: the branch moves (compare-and-swap against the value checked above)
    // and the staging ref disappears together, or neither happens. On an earlier refusal only
    // the staging ref is dropped, and a failure to drop it does not replace that answer.
    const char *zero = strlen(now.c_str()) == 64
        ? "0000000000000000000000000000000000000000000000000000000000000000"
        : "0000000000000000000000000000000000000000";
    if (now.empty()) return rc ? rc : WFS_E_GIT_FAILED;
    String script;
    if (!rc && old != now) {
        // Git offers no way to make "not checked out in any worktree" part of a ref
        // transaction; its own refusal to fetch into a checked-out branch is the same kind of
        // check. Repeat it immediately before the transaction, so the window is only the
        // transaction itself rather than the whole fetch and history checks.
        bool checked_now = false;
        rc = branch_checked_out(repo, ref, checked_now);
        if (!rc && checked_now)
            rc = refuse(WFS_E_GIT_TARGET, "%s was checked out in the target repository while publishing; nothing was changed", branch);
    }
    if (!rc) rc = publish_lfs(world_root, repo, now);
    if (!rc && old != now) {
        script.append("update "); script.append(ref.c_str()); script.push_back(' ');
        script.append(now.c_str()); script.push_back(' ');
        script.append(old.empty() ? zero : old.c_str()); script.push_back('\n');
    } else if (!rc) {
        // Already there: still prove, in the same transaction, that nobody moved it meanwhile.
        script.append("verify "); script.append(ref.c_str()); script.push_back(' ');
        script.append(now.c_str()); script.push_back('\n');
    }
    script.append("delete "); script.append(staging); script.push_back(' ');
    script.append(now.c_str()); script.push_back('\n');
    // If the transaction cannot even be fed, still take the staging ref back out (only with
    // the value this call fetched), so a failure never leaves the target modified.
    auto drop_staging = [&]() {
        const char *drop[] = {"update-ref", "--no-deref", "-d", staging, now.c_str(), nullptr};
        (void)git(repo, drop, nullptr, nullptr, true);
    };
    FILE *input = tmpfile();
    if (!input) { int err = -errno; drop_staging(); return rc ? rc : err; }
    if (fwrite(script.c_str(), 1, script.size(), input) != script.size() || fflush(input)) {
        int err = errno ? -errno : -EIO; fclose(input); drop_staging(); return rc ? rc : err;
    }
    rewind(input);
    // --no-deref: the transaction names the branch itself, never a ref it might point to.
    const char *txn[] = {"update-ref", "-m", "world fs publish", "--no-deref", "--stdin", nullptr};
    int txn_rc = git(repo, txn, nullptr, nullptr, false, false, fileno(input));
    fclose(input);
    // A failed transaction (for example a lost compare-and-swap: another process moved the
    // branch between the checks above and here) leaves the branch untouched, but the staging ref
    // this call fetched into the target is still sitting there unless it is taken back out too.
    if (txn_rc) { drop_staging(); return rc ? rc : txn_rc; }
    if (rc) return rc;
    snprintf(out->ref, sizeof out->ref, "%s", ref.c_str());
    snprintf(out->old_oid, sizeof out->old_oid, "%s", old.c_str());
    snprintf(out->new_oid, sizeof out->new_oid, "%s", now.c_str());
    out->dirty = dirty == WFS_E_GIT_DIRTY;
    return 0;
}
