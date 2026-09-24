#include "git.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

extern char **environ;
namespace wfs {
namespace {
String joinp(const char *a, const char *b) { String s(a); s.append("/"); s.append(b); return s; }
constexpr const char *marker = "gitdir: .world-git/repo.git/worktrees/active\n";

// Never use a shell, source hooks, inherited GIT_DIR/INDEX_FILE, or lazy network fetches.
int git(const char *cwd, const char *const *args, Vec<char> *output = nullptr, int *exit_code = nullptr, bool quiet_stderr = false, bool ambient_config_probe = false) {
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
             !strncmp(*p, "GIT_CONFIG_PARAMETERS=", 22)))) env.emplace_back(*p);
    }
    const char *settings[] = {"GIT_OPTIONAL_LOCKS=0", "GIT_TERMINAL_PROMPT=0", "GIT_NO_LAZY_FETCH=1"};
    for (const char *p : settings) env.emplace_back(const_cast<char *>(p));
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
    if (!rc && quiet_stderr)
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
        if (tab == i || tab == start) return WFS_E_GIT_UNSUPPORTED;
        if (tab + 1 == i) { start = i + 1; continue; }
        String name(listing.data() + start, tab - start), target(listing.data() + tab + 1, i - tab - 1);
        if (strncmp(name.c_str(), "refs/", 5) || strncmp(target.c_str(), "refs/", 5) ||
            strchr(target.c_str(), '\t') || strchr(target.c_str(), '\n')) return WFS_E_GIT_UNSUPPORTED;
        const char *sym_args[] = {"symbolic-ref", "--quiet", "--no-recurse", name.c_str(), nullptr};
        String immediate;
        if (int rc = value(root, sym_args, immediate)) return rc;
        if (strncmp(immediate.c_str(), "refs/", 5)) return WFS_E_GIT_UNSUPPORTED;
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
        close(fd); return WFS_E_GIT_UNSUPPORTED;
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

// A nested worktree/submodule cannot be made safe by fixing only the root's .git file.
// Until recursive Git imports exist, reject nested repositories, including plain nested ones.
int nested_check(const char *root, bool top = true) {
    DIR *dir = opendir(root);
    if (!dir) return -errno;
    int rc = 0;
    for (;;) {
        errno = 0;
        dirent *e = readdir(dir);
        if (!e) { if (errno) rc = -errno; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!strcmp(e->d_name, ".git")) {
            if (!top) { rc = WFS_E_GIT_UNSUPPORTED; break; }
            continue;
        }
        if (top && !strcmp(e->d_name, ".world-git")) continue;
        String path = joinp(root, e->d_name);
        struct stat st;
        if (lstat(path.c_str(), &st)) { rc = -errno; break; }
        if (S_ISDIR(st.st_mode) && (rc = nested_check(path.c_str(), false))) break;
    }
    closedir(dir); return rc;
}
// A managed tree may be copied without consulting an external repository only when all
// administration stays inside it. Reject changed common-dir pointers and additional worktrees.
int managed_check(const char *root, const char *common, const char *admin) {
    String repo = joinp(root, ".world-git/repo.git"), active = joinp(repo.c_str(), "worktrees/active");
    String expected, actual;
    if (fs_realpath(repo.c_str(), expected) || fs_realpath(common, actual) || expected != actual)
        return WFS_E_GIT_UNSUPPORTED;
    if (fs_realpath(active.c_str(), expected) || fs_realpath(admin, actual) || expected != actual)
        return WFS_E_GIT_UNSUPPORTED;
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
        if (normalized != expected_text) return WFS_E_GIT_UNSUPPORTED;
    }
    // Reject symlinks anywhere in the owned administration, including its root.
    Vec<String> dirs; dirs.emplace_back(joinp(root, ".world-git"));
    for (size_t i = 0; i < dirs.size(); ++i) {
        String path = dirs[i]; struct stat st;
        if (lstat(path.c_str(), &st) || !S_ISDIR(st.st_mode)) return WFS_E_GIT_UNSUPPORTED;
        DIR *d = opendir(path.c_str()); if (!d) return -errno;
        int rc = 0;
        for (;;) {
            errno = 0; dirent *e = readdir(d);
            if (!e) { if (errno) rc = -errno; break; }
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            String child = joinp(path.c_str(), e->d_name);
            if (lstat(child.c_str(), &st)) { rc = -errno; break; }
            if (S_ISDIR(st.st_mode)) dirs.emplace_back(child);
            else if (!S_ISREG(st.st_mode)) { rc = WFS_E_GIT_UNSUPPORTED; break; }
            if (path == joinp(repo.c_str(), "worktrees") && strcmp(e->d_name, "active")) {
                rc = WFS_E_GIT_UNSUPPORTED; break;
            }
        }
        closedir(d); if (rc) return rc;
    }
    return 0;
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
    const char *filters[] = {"config", "--get-regexp", "^filter\\.", nullptr};
    Vec<char> filter_bytes; int filter_status = -1;
    int rc = git(root, filters, &filter_bytes, &filter_status);
    if (rc != WFS_E_GIT_FAILED || filter_status != 1) {
        if (rc) return rc;
        return WFS_E_GIT_UNSUPPORTED;
    }
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
int walk_objects_fd(int fd, unsigned depth, uint64_t &total) {
    if (depth > 256) return WFS_E_GIT_UNSUPPORTED;
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
        if (S_ISLNK(st.st_mode) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) { rc = WFS_E_GIT_UNSUPPORTED; break; }
        if (UINT64_MAX - total < 1024 || (S_ISREG(st.st_mode) && (uint64_t)st.st_size > UINT64_MAX - total - 1024)) { rc = -EOVERFLOW; break; }
        if (S_ISREG(st.st_mode) && st.st_size < 0) { rc = -EOVERFLOW; break; }
        total += 1024 + (S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0);
        if (S_ISDIR(st.st_mode)) {
            int child = openat(fd, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { rc = -errno; break; }
            rc = walk_objects_fd(child, depth + 1, total); close(child);
            if (rc) break;
        }
    }
    closedir(dir); return rc;
}
int object_import_bytes(const char *common, uint64_t &total) {
    String objects = joinp(common, "objects");
    int fd = open(objects.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -errno;
    total = 0; int rc = walk_objects_fd(fd, 0, total); close(fd); return rc;
}
int reject_configured_policy(const char *root, const char *key) {
    const char *args[] = {"config", "--get", key, nullptr};
    Vec<char> value_bytes; int status = -1;
    int rc = git(root, args, &value_bytes, &status);
    if (!rc) return WFS_E_GIT_UNSUPPORTED;
    if (rc == WFS_E_GIT_FAILED && status == 1) return 0;
    return rc;
}
// This fixed, read-only query is the sole caller allowed to load ambient config.
// Git parses the files, but returns only matching key names, never their values.
int reject_ambient_policy(const char *root) {
    const char *args[] = {"config", "--includes", "--null", "--show-scope", "--name-only",
        "--get-regexp", "^(core\\.(excludesfile|attributesfile|autocrlf|eol|safecrlf|filemode|symlinks|ignorecase|precomposeunicode|trustctime|checkstat|ignorestat|checkroundtripencoding|usereplacerefs)$|filter\\..*|includeif\\..*\\.path$)", nullptr};
    Vec<char> listing; int status = -1;
    int rc = git(root, args, &listing, &status, false, true);
    if (rc == WFS_E_GIT_FAILED && status == 1) return 0;
    if (rc) return rc;
    size_t i = 0;
    while (i < listing.size() && listing[i]) {
        size_t scope = i; while (i < listing.size() && listing[i]) ++i;
        size_t scope_len = i - scope;
        if (i >= listing.size()) return WFS_E_GIT_FAILED;
        ++i;
        size_t key = i; while (i < listing.size() && listing[i]) ++i;
        if (i == key || i + 1 >= listing.size()) return WFS_E_GIT_FAILED;
        size_t key_len = i - key;
        if (key_len >= 10 && !memcmp(listing.data() + key, "includeif.", 9) &&
            !memcmp(listing.data() + i - 5, ".path", 5)) return WFS_E_GIT_UNSUPPORTED;
        if ((scope_len == 6 && !memcmp(listing.data() + scope, "global", 6)) ||
            (scope_len == 6 && !memcmp(listing.data() + scope, "system", 6)) ||
            (scope_len == 7 && !memcmp(listing.data() + scope, "command", 7))) return WFS_E_GIT_UNSUPPORTED;
        ++i;
    }
    return 0;
}
int reject_external_visibility_state(const char *root, bool managed) {
    if (managed) return 0;
    for (const char *key : {"transfer.hideRefs", "uploadpack.hideRefs"})
        if (int rc = reject_configured_policy(root, key)) return rc;
    const char *args[] = {"reflog", "exists", "refs/stash", nullptr};
    int status = -1, rc = git(root, args, nullptr, &status);
    if (!rc) return WFS_E_GIT_UNSUPPORTED;
    if (rc != WFS_E_GIT_FAILED || status != 1) return rc;
    String grafts; const char *graft_args[] = {"rev-parse", "--path-format=absolute", "--git-path", "info/grafts", nullptr};
    if (int graft_rc = value(root, graft_args, grafts)) return graft_rc;
    struct stat st;
    if (!lstat(grafts.c_str(), &st)) return WFS_E_GIT_UNSUPPORTED;
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
                if (i - sp == 4 && !memcmp(listing.c_str() + sp, "true", 4)) return WFS_E_GIT_UNSUPPORTED;
            }
            start = i + 1;
        }
    }
    const char *filter[] = {"config", "--get-regexp", "^remote\\..*\\.partialclonefilter$", nullptr};
    if (int rc = get_config(root, filter, listing, &present)) return rc;
    return present ? WFS_E_GIT_UNSUPPORTED : 0;
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
        if (len > 9 && !strcmp(e->d_name + len - 9, ".promisor")) { rc = WFS_E_GIT_UNSUPPORTED; break; }
    }
    closedir(d); return rc;
}
// Repeat the same eligibility checks before publication: configuration and layout
// can change while an external mirror is being copied.
int reject_import_policy(const char *root) {
    if (int rc = reject_ambient_policy(root)) return rc;
    for (const char *key : {"core.excludesFile", "core.attributesFile"})
        if (int rc = reject_configured_policy(root, key)) return rc;
    for (const char *key : {"core.sparseCheckout", "core.splitIndex"}) {
        String val; bool present = false; const char *args[] = {"config", "--get", "--type=bool", key, nullptr};
        if (int rc = get_config(root, args, val, &present)) return rc;
        if (present && val != "false") return WFS_E_GIT_UNSUPPORTED;
    }
    String val; bool present = false; const char *partial[] = {"config", "--get", "extensions.partialClone", nullptr};
    if (int rc = get_config(root, partial, val, &present)) return rc;
    if (present) return WFS_E_GIT_UNSUPPORTED;
    if (int rc = reject_promisor_remotes(root)) return rc;
    String common; const char *common_args[] = {"rev-parse", "--path-format=absolute", "--git-common-dir", nullptr};
    if (int rc = value(root, common_args, common)) return rc;
    struct stat st;
    for (const char *rel : {"objects/info/alternates", "objects/info/http-alternates", "shallow"}) {
        String path = joinp(common.c_str(), rel);
        if (!lstat(path.c_str(), &st)) return WFS_E_GIT_UNSUPPORTED;
        if (errno != ENOENT) return -errno;
    }
    if (int rc = reject_promisor_packs(common)) return rc;
    const char *shared_args[] = {"rev-parse", "--shared-index-path", nullptr};
    if (int rc = value(root, shared_args, val)) return rc;
    return val.empty() ? 0 : WFS_E_GIT_UNSUPPORTED;
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
        return errno == ENOTDIR || errno == ELOOP ? WFS_E_GIT_UNSUPPORTED : -errno;
    }
    present = true;
    Vec<String> dirs;
    int rc = list_names(fd, dirs);
    for (size_t i = 0; !rc && i < dirs.size(); ++i) {
        struct stat st;
        if (fstatat(fd, dirs[i].c_str(), &st, AT_SYMLINK_NOFOLLOW)) { rc = -errno; break; }
        if (!S_ISDIR(st.st_mode)) { rc = WFS_E_GIT_UNSUPPORTED; break; }
        int sub = openat(fd, dirs[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (sub < 0) { rc = -errno; break; }
        append_record(blob, dirs[i].c_str(), nullptr);
        Vec<String> files;
        rc = list_names(sub, files);
        for (size_t j = 0; !rc && j < files.size(); ++j) {
            if (fstatat(sub, files[j].c_str(), &st, AT_SYMLINK_NOFOLLOW)) { rc = -errno; break; }
            if (!S_ISREG(st.st_mode)) { rc = WFS_E_GIT_UNSUPPORTED; break; }
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
// GIT_OPTIONAL_LOCKS=0 (set by git()) keeps status from refreshing the index it inspects.
int require_clean_tree(const char *root) {
    const char *args[] = {"status", "--porcelain=v1", "-z", "--untracked-files=normal",
        "--", ".", ":(exclude).world", ":(exclude).world-git", nullptr};
    Vec<char> dirty;
    if (int rc = git(root, args, &dirty)) return rc;
    return dirty.size() > 1 ? WFS_E_GIT_DIRTY : 0;
}
int source_unchanged(const GitSource &s) {
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
    Vec<char> direct_refs; if (int ref_rc = capture_refs(s.root.c_str(), direct_refs)) return ref_rc;
    if (!same_bytes(direct_refs, s.refs)) return -EBUSY;
    bool orig_present; String orig; if (int orig_rc = capture_orig(s.root.c_str(), orig_present, orig)) return orig_rc;
    if (orig_present != s.orig_present || orig != s.orig_head) return -EBUSY;
    if (!s.managed) {
        String common;
        const char *a[] = {"rev-parse", "--path-format=absolute", "--git-common-dir", nullptr};
        if (int rc = value(s.root.c_str(), a, common)) return rc;
        uint64_t bytes = 0;
        if (int rc = object_import_bytes(common.c_str(), bytes)) return rc;
        if (bytes + s.rerere_bytes != s.import_bytes) return -EBUSY;
        Vec<char> rerere; bool rerere_present = false; uint64_t rerere_bytes = 0;
        if (int rc = capture_rerere(s.root.c_str(), rerere, rerere_present, rerere_bytes)) return rc;
        if (rerere_present != s.rerere_present || !same_bytes(rerere, s.rerere)) return -EBUSY;
    }
    Vec<GitSymref> refs;
    if (int symrc = collect_symrefs(s.root.c_str(), refs)) return symrc;
    if (!same_symrefs(refs, s.symrefs)) return -EBUSY;
    return 0;
}
}

// WorldFS owns the root `.world` file and `.world-git` directory: snapshots drop the copied
// `.world`, forks write metadata there, and info/exclude cannot hide a tracked path. A path
// tracked in the index would fork dirty or commit metadata; one anywhere in preserved history
// would overwrite the World marker on an ordinary checkout (ignored files are overwritten by
// default). So the index and every commit reachable from what the import keeps -- all refs,
// HEAD, ORIG_HEAD and FETCH_HEAD tips -- must never contain either path. --full-history keeps
// a side branch that added the path and was merged away from being simplified out.
// --no-replace-objects makes the walk read the commits and trees the import actually preserves:
// a refs/replace/* entry could otherwise present a safe tree for a commit whose real tree has
// the path, and deleting that replacement in the World would bring it back. The replacement
// commits are still scanned, as ordinary tips under --all.
int reject_reserved_paths(const char *root, const GitSource &s) {
    const char *tracked_args[] = {"ls-files", "-z", "--", ":(top,literal).world", ":(top,literal).world-git", nullptr};
    Vec<char> tracked;
    if (int rc = git(root, tracked_args, &tracked)) return rc;
    if (tracked.size() > 1) return WFS_E_GIT_UNSUPPORTED;
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
            else if (i > start) return WFS_E_GIT_UNSUPPORTED;
            start = i + 1;
        }
    }
    Vec<const char *> args;
    for (const char *a : {"--no-replace-objects", "rev-list", "-n", "1", "--full-history", "--all"}) args.emplace_back(a);
    for (const auto &tip : tips) args.emplace_back(tip.c_str());
    for (const char *a : {"--", ":(top,literal).world", ":(top,literal).world-git"}) args.emplace_back(a);
    args.emplace_back(nullptr);
    Vec<char> hit;
    if (int rc = git(root, args.data(), &hit)) return rc;
    return hit.size() > 1 ? WFS_E_GIT_UNSUPPORTED : 0;
}
int git_source(const char *root, bool include_changes, GitSource &out) {
    if (int rc = nested_check(root)) return rc;
    String dot = joinp(root, ".git"), managed = joinp(root, ".world-git");
    struct stat st;
    bool has_managed = lstat(managed.c_str(), &st) == 0;
    if (!has_managed && errno != ENOENT) return -errno;
    if (lstat(dot.c_str(), &st)) return errno == ENOENT && !has_managed ? 0 : WFS_E_GIT_UNSUPPORTED;
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) return WFS_E_GIT_UNSUPPORTED;
    out.present = true; out.root = root;
    if (has_managed) {
        Vec<char> contents;
        if (int rc = read_bytes(dot.c_str(), contents)) return rc;
        if (contents.size() != strlen(marker) || memcmp(contents.data(), marker, contents.size()))
            return WFS_E_GIT_UNSUPPORTED;
        out.managed = true;
    }
    String top;
    const char *top_args[] = {"rev-parse", "--show-toplevel", nullptr};
    if (int rc = value(root, top_args, top)) return rc;
    String real_top, real_root;
    if (fs_realpath(root, real_root) || fs_realpath(top.c_str(), real_top) || real_root != real_top)
        return WFS_E_GIT_UNSUPPORTED;
    if (int policy_rc = reject_import_policy(root)) return policy_rc;
    if (int policy_rc = capture_settings(root, out.settings)) return policy_rc;
    if (int identity_rc = capture_identity(root, out.identity)) return identity_rc;
    const char *head_args[] = {"rev-parse", "--verify", "HEAD^{commit}", nullptr};
    if (value(root, head_args, out.head)) return WFS_E_GIT_UNSUPPORTED;
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
    String common, admin;
    const char *common_args[] = {"rev-parse", "--path-format=absolute", "--git-common-dir", nullptr};
    const char *admin_args[] = {"rev-parse", "--absolute-git-dir", nullptr};
    if ((rc = value(root, common_args, common)) || (rc = value(root, admin_args, admin))) return rc;
    if (!out.managed) { if ((rc = object_import_bytes(common.c_str(), out.import_bytes))) return rc; }
    if (out.managed && (rc = managed_check(root, common.c_str(), admin.c_str()))) return rc;
    if ((rc = reject_external_visibility_state(root, out.managed))) return rc;
    if ((rc = capture_refs(root, out.refs))) return rc;
    if ((rc = capture_orig(root, out.orig_present, out.orig_head))) return rc;
    if ((rc = collect_symrefs(root, out.symrefs))) return rc;
    if ((rc = reject_inprogress(root))) return rc;
    if (!out.managed) {
        if ((rc = capture_rerere(root, out.rerere, out.rerere_present, out.rerere_bytes))) return rc;
        out.import_bytes += out.rerere_bytes;
    }
    // Sparse/split indexes and gitlinks require a separate import contract.
    const char *ls_args[] = {"ls-files", "--stage", "-z", nullptr};
    Vec<char> listing;
    if ((rc = git(root, ls_args, &listing))) return rc;
    for (size_t i = 0; i + 1 < listing.size();) {
        const char *entry = listing.data() + i;
        if (!strncmp(entry, "160000 ", 7)) return WFS_E_GIT_UNSUPPORTED;
        size_t end = i;
        while (end < listing.size() && listing[end]) ++end;
        size_t tab = i;
        while (tab < end && listing[tab] != '\t') ++tab;
        if (tab < end && tab - i >= 2 && listing[tab - 1] >= '1' && listing[tab - 1] <= '3' && listing[tab - 2] == ' ')
            return -EBUSY;
        i += strlen(listing.data() + i) + 1;
    }
    if ((rc = reject_reserved_paths(root, out))) return rc;
    out.require_clean = !include_changes;
    if (!include_changes) {
        if ((rc = require_clean_tree(root))) return rc;
    }
    return 0;
}

// A managed World is copied with its whole administration, so a live or stale Git lock
// (packed-refs.lock, refs/**/x.lock, config.lock, commit-graph or midx locks, ...) would be
// published in the child and make its later ref or maintenance updates fail. Scan the copy:
// it is exactly what gets published. Loose-object fan-out directories hold no locks and can
// be large, so they are skipped.
int reject_locks_fd(int dirfd, bool objects_level, int depth) {
    if (depth > 64) return WFS_E_GIT_UNSUPPORTED;
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
int git_import(const GitSource &s, const char *clone) {
    if (!s.present) return 0;
    if (int rc = source_unchanged(s)) return rc;
    if (s.managed) {
        // Detect a copied HEAD/index from a different moment, even when the source looks
        // unchanged again by the time cloning finishes.
        GitSource copy;
        if (int rc = git_source(clone, true, copy)) return rc;
        if (copy.head != s.head || copy.index.size() != s.index.size() ||
            (!copy.index.empty() && memcmp(copy.index.data(), s.index.data(), s.index.size())) ||
            !same_bytes(copy.exclude, s.exclude) ||
            !same_bytes(copy.attributes, s.attributes) ||
            copy.fetch_present != s.fetch_present || !same_bytes(copy.fetch, s.fetch) ||
            copy.squash_present != s.squash_present || !same_bytes(copy.squash, s.squash) ||
            copy.sparse_present != s.sparse_present || !same_bytes(copy.sparse, s.sparse) ||
            !same_symrefs(copy.symrefs, s.symrefs) || !same_settings(copy.settings, s.settings)) return -EBUSY;
        if (!same_bytes(copy.refs, s.refs) ||
            copy.orig_present != s.orig_present || copy.orig_head != s.orig_head) return -EBUSY;
        if (int rc = reject_copied_locks(clone)) return rc;
        // HEAD and the index do not change when a tracked file is edited after the source's
        // clean check, so check the copy that will actually be published.
        return s.require_clean ? require_clean_tree(clone) : 0;
    }
    String owned = joinp(clone, ".world-git");
    if (int rc = fs_mkdir(owned.c_str(), 0700)) return rc;
    String repo = joinp(owned.c_str(), "repo.git");
    // Mirror all resolvable refs, including stash, notes, remote-tracking and custom refs; a
    // bare clone omits other ref namespaces and would lose them when the source is removed.
    const char *copy[] = {"clone", "--mirror", "--no-hardlinks", "--template=", "--quiet", "--", s.root.c_str(), repo.c_str(), nullptr};
    if (int rc = git(clone, copy)) return rc;
    for (const auto &ref : s.symrefs) {
        const char *sym_args[] = {"--git-dir", repo.c_str(), "symbolic-ref", ref.name.c_str(),
                                  ref.target.c_str(), nullptr};
        if (int rc = git(clone, sym_args)) return rc;
    }
    // clone --local copies loose objects too, including blobs referenced only by the index;
    // --no-hardlinks prevents subsequent Git operations changing the source's object files.
    String active = joinp(owned.c_str(), "active");
    const char *add[] = {"--git-dir", repo.c_str(), "worktree", "add", "--relative-paths", "--no-checkout",
        "--detach", "--quiet", "--", active.c_str(), s.head.c_str(), nullptr};
    if (int rc = git(clone, add)) return rc;
    String dot = joinp(clone, ".git");
    if (int rc = fs_remove_tree(dot.c_str())) return rc;
    if (int rc = write_text(clone, ".git", marker)) return rc;
    const char *repair[] = {"worktree", "repair", "--relative-paths", nullptr};
    if (int rc = git(clone, repair)) return rc;
    // The only entry in the disposable no-checkout directory is its gitfile.
    String old_dot = joinp(active.c_str(), ".git");
    if (unlink(old_dot.c_str()) || rmdir(active.c_str())) return -errno;
    String index = joinp(repo.c_str(), "worktrees/active/index");
    if (!s.index.empty()) {
        if (int rc = write_bytes(index.c_str(), s.index.data(), s.index.size())) return rc;
    } else {
        // No index means all tracked files were removed from it; do not recreate HEAD's index.
        const char *empty[] = {"read-tree", "--empty", nullptr};
        if (int rc = git(clone, empty)) return rc;
    }
    Vec<char> excludes;
    for (char c : s.exclude) excludes.emplace_back(c);
    if (!excludes.empty() && excludes.back() != '\n') excludes.emplace_back('\n');
    for (const char *reserved : {"/.world\n", "/.world-git/\n"})
        for (const char *p = reserved; *p; ++p) excludes.emplace_back(*p);
    // Empty templates omit info/. Create only the directory needed for owned rules.
    String info = joinp(repo.c_str(), "info");
    if (int rc = fs_mkdir(info.c_str(), 0700)) { if (rc != -EEXIST) return rc; }
    String exclude_path = joinp(repo.c_str(), "info/exclude");
    if (int rc = write_bytes(exclude_path.c_str(), excludes.data(), excludes.size())) return rc;
    if (!s.attributes.empty()) {
        String attributes_path = joinp(repo.c_str(), "info/attributes");
        if (int rc = write_bytes(attributes_path.c_str(), s.attributes.data(), s.attributes.size())) return rc;
    }
    if (s.rerere_present) {
        if (int rc = restore_rerere(repo.c_str(), s.rerere)) return rc;
    }
    if (int rc = config(clone, "worldfs.baseline", s.head.c_str())) return rc;
    if (int rc = config(clone, "worldfs.formatVersion", "1")) return rc;
    for (const auto &id : s.identity)
        if (int rc = config(clone, id.key.c_str(), id.value.c_str())) return rc;
    for (const char *key : {"core.autocrlf", "core.safecrlf", "core.eol", "core.checkstat", "core.checkRoundtripEncoding",
                            "core.filemode", "core.symlinks", "core.ignorecase", "core.precomposeunicode", "core.trustctime", "core.ignorestat",
                            "core.useReplaceRefs"})
        if (int rc = unset_config(clone, key)) return rc;
    for (const auto &setting : s.settings)
        if (int rc = config(clone, setting.key.c_str(), setting.value.c_str())) return rc;
    if (s.sparse_present) {
        const char *args[] = {"rev-parse", "--path-format=absolute", "--git-path", "info/sparse-checkout", nullptr};
        String dest_sparse;
        if (int rc = value(clone, args, dest_sparse)) return rc;
        String dest_info(dest_sparse.c_str(), dest_sparse.size() - strlen("/sparse-checkout"));
        if (int rc = fs_mkdir(dest_info.c_str(), 0700)) { if (rc != -EEXIST) return rc; }
        if (int rc = write_bytes(dest_sparse.c_str(), s.sparse.data(), s.sparse.size())) return rc;
    }
    if (s.squash_present) {
        const char *args[] = {"rev-parse", "--path-format=absolute", "--git-path", "SQUASH_MSG", nullptr};
        String dest_squash;
        if (int rc = value(clone, args, dest_squash)) return rc;
        if (int rc = write_bytes(dest_squash.c_str(), s.squash.data(), s.squash.size())) return rc;
    }
    if (s.fetch_present) {
        const char *args[] = {"rev-parse", "--path-format=absolute", "--git-path", "FETCH_HEAD", nullptr};
        String dest_fetch;
        if (int rc = value(clone, args, dest_fetch)) return rc;
        if (int rc = write_bytes(dest_fetch.c_str(), s.fetch.data(), s.fetch.size())) return rc;
    }
    if (s.orig_present) {
        const char *orig_args[] = {"update-ref", "ORIG_HEAD", s.orig_head.c_str(), nullptr};
        if (int rc = git(clone, orig_args)) return rc;
    }
    // A clone is local and self-contained; its remote is not an implicit write-back channel.
    const char *remote[] = {"config", "--local", "--remove-section", "remote.origin", nullptr};
    if (int rc = git(clone, remote)) return rc;
    Vec<char> imported_refs;
    if (int rc = capture_refs(clone, imported_refs)) return rc;
    if (!same_bytes(imported_refs, s.refs)) return -EBUSY;
    if (int rc = source_unchanged(s)) return rc;
    // The owned repository now carries the source's index and status settings, so this sees the
    // bytes that will be published, including edits made after the source's own clean check.
    return s.require_clean ? require_clean_tree(clone) : 0;
}

int git_discard_check(const char *root) {
    String owned = joinp(root, ".world-git"); struct stat st;
    bool managed = lstat(owned.c_str(), &st) == 0;
    if (!managed && errno != ENOENT) return -errno;
    String dir = joinp(root, managed ? ".world-git/repo.git/worktrees" : ".git/worktrees");
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT || errno == ENOTDIR ? 0 : -errno;
    DIR *d = fdopendir(fd);
    if (!d) { int rc = -errno; close(fd); return rc; }
    int rc = 0;
    for (;;) {
        errno = 0; dirent *e = readdir(d);
        if (!e) { if (errno) rc = -errno; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (managed && !strcmp(e->d_name, "active")) continue;
        rc = WFS_E_GIT_IN_USE; break;
    }
    closedir(d); return rc;
}

int git_branch(const char *clone, wfs_id world) {
    String dot = joinp(clone, ".git"); struct stat st;
    if (lstat(dot.c_str(), &st)) return errno == ENOENT ? 0 : -errno;
    GitSource s;
    if (int rc = git_source(clone, true, s)) return rc;
    if (!s.managed) return WFS_E_GIT_UNSUPPORTED;
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
    // so names.size() + 1 candidates always include a free one.
    for (size_t suffix = 0; suffix <= names.size(); ++suffix) {
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
        if (available) break;
    }
    if (!available) return -EEXIST;
    const char *create[] = {"update-ref", branch, s.head.c_str(), "", nullptr};
    if (int rc = git(clone, create)) return rc;
    const char *checkout[] = {"symbolic-ref", "HEAD", branch, nullptr};
    if (int rc = git(clone, checkout)) return rc;
    if (int rc = config(clone, "worldfs.baseline", s.head.c_str())) return rc;
    return 0;
}
} // namespace wfs

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
        return WFS_E_GIT_UNSUPPORTED;
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
