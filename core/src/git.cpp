#include "git.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/random.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

extern char **environ;
namespace wfs {
namespace {
String joinp(const char *a, const char *b) { String s(a); s.append("/"); s.append(b); return s; }
constexpr const char *marker = "gitdir: .world-git/repo.git/worktrees/active\n";

// The reason for the last Git refusal on this thread, for wfs_git_reason(). Every
// WFS_E_GIT_UNSUPPORTED / WFS_E_GIT_POLICY this file returns goes through refuse().
thread_local char g_reason[512];
int refuse(int code, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_reason, sizeof g_reason, fmt, ap);
    va_end(ap);
    return code;
}
// Never use a shell, source hooks, inherited GIT_DIR/INDEX_FILE, or lazy network fetches.
// `ambient_config_probe` keeps the user's global/system configuration (and GIT_CONFIG_* command
// configuration) so a check sees files the way the user's own Git does; `stdin_fd`, when >= 0,
// becomes the child's standard input.
int git(const char *cwd, const char *const *args, Vec<char> *output = nullptr, int *exit_code = nullptr, bool quiet_stderr = false, bool ambient_config_probe = false, int stdin_fd = -1) {
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
    if (!rc && stdin_fd >= 0) rc = posix_spawn_file_actions_adddup2(&actions, stdin_fd, STDIN_FILENO);
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
            if (!top) { rc = refuse(WFS_E_GIT_UNSUPPORTED, "nested Git repository or submodule at %s", root); break; }
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
    // Reject symlinks anywhere in the owned administration, including its root.
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
            if (path == joinp(repo.c_str(), "worktrees") && strcmp(e->d_name, "active")) {
                rc = refuse(WFS_E_GIT_UNSUPPORTED, "the World has an additional linked worktree (%s)", e->d_name); break;
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
// defaults and aliases. Aliases run only when the user types them, as they would in the
// source. Settings Git executes on its own -- hooks, remote.*.uploadpack/receivepack/vcs,
// branch.*.mergeoptions, core.sshCommand, credential helpers -- are not carried. A relative
// local remote path is made absolute against the source, so it keeps pointing at the same
// repository after the source is gone.
const char *const kCarriedConfig =
    "^(remote\\..+\\.(url|pushurl|fetch|push|tagopt|prune|prunetags|mirror|skipdefaultupdate|skipfetchall|followremotehead)"
    "|remotes\\..+|remote\\.pushdefault"
    "|branch\\..+\\.(remote|merge|pushremote|rebase|description)"
    "|url\\..+\\.(insteadof|pushinsteadof)"
    "|push\\.(default|autosetupremote)|fetch\\.(prune|prunetags)"
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
bool is_relative_local_url(const char *url) {
    if (!*url || url[0] == '/' || url[0] == '~' || strstr(url, "://")) return false;
    const char *colon = strchr(url, ':'), *slash = strchr(url, '/');
    if (colon && (!slash || colon < slash)) return false; // scp-like host:path
    return true;
}
int capture_carried_config(const char *root, Vec<GitSetting> &out) {
    out.clear();
    const char *args[] = {"config", "--local", "--includes", "--null", "--get-regexp", kCarriedConfig, nullptr};
    Vec<char> listing; int status = -1;
    int rc = git(root, args, &listing, &status);
    if (rc == WFS_E_GIT_FAILED && status == 1) return 0;
    if (rc) return rc;
    // Entries are "<key>\n<value>\0"; a valueless key has no newline.
    for (size_t i = 0; i < listing.size() && listing[i];) {
        const char *entry = listing.data() + i;
        size_t len = strlen(entry);
        i += len + 1;
        const char *nl = strchr(entry, '\n');
        String key(entry, nl ? (size_t)(nl - entry) : len), val(nl ? nl + 1 : "");
        size_t klen = key.size();
        bool url = !strncmp(key.c_str(), "remote.", 7) &&
                   ((klen > 4 && !strcmp(key.c_str() + klen - 4, ".url")) ||
                    (klen > 8 && !strcmp(key.c_str() + klen - 8, ".pushurl")));
        if (url && is_relative_local_url(val.c_str())) val = absolute_lexical(root, val.c_str());
        out.emplace_back(GitSetting{key, val});
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
int walk_objects_fd(int fd, unsigned depth, uint64_t &total) {
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
//   * status settings given as command configuration: they belong to this invocation only.
//   * a conditional include whose target sets status or filter settings: the condition
//     (gitdir, onbranch, ...) can evaluate differently at the World's location, active or not
//     at the source. Includes that only set other things (identity, signing, aliases) are fine;
//     identity is pinned in the World separately (pin_identity).
// Filters are refused only when tracked files actually use a defined one (reject_used_filters).
const char *const kStatusKeys = "core\\.(excludesfile|attributesfile|autocrlf|eol|safecrlf|filemode|symlinks|"
    "ignorecase|precomposeunicode|trustctime|checkstat|ignorestat|checkroundtripencoding|usereplacerefs)";
bool is_status_or_filter_key(const char *key) {
    static const char *const keys[] = {"core.excludesfile", "core.attributesfile", "core.autocrlf", "core.eol",
        "core.safecrlf", "core.filemode", "core.symlinks", "core.ignorecase", "core.precomposeunicode",
        "core.trustctime", "core.checkstat", "core.ignorestat", "core.checkroundtripencoding",
        "core.usereplacerefs", "attr.tree", nullptr};
    for (size_t i = 0; keys[i]; ++i) if (!strcasecmp(key, keys[i])) return true;
    return !strncasecmp(key, "filter.", 7);
}
String dirname_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return String(".");
    if (slash == path) return String("/");
    return String(path, (size_t)(slash - path));
}
// Resolve an include path the way Git does: "~/" is $HOME, a relative path is relative to the
// directory of the file that contains the directive.
bool resolve_include(const char *value, const char *including_file, String &out) {
    if (!strncmp(value, "~/", 2)) {
        const char *home = getenv("HOME");
        if (!home || !*home) return false;
        out = joinp(home, value + 2);
        return true;
    }
    if (value[0] == '/') { out.assign(value); return true; }
    if (!including_file) return false;
    String dir = dirname_of(including_file);
    out = joinp(dir.c_str(), value);
    return true;
}
int scan_include_target(const char *root, const char *path, const char *directive, int depth) {
    if (depth > 10) return refuse(WFS_E_GIT_POLICY, "configuration includes are nested more than 10 deep");
    struct stat st;
    if (stat(path, &st)) return errno == ENOENT ? 0 : -errno; // Git ignores a missing include
    const char *names[] = {"config", "--file", path, "--null", "--name-only", "--list", nullptr};
    Vec<char> listing;
    if (int rc = git(root, names, &listing)) return rc;
    bool nested = false;
    for (size_t i = 0; i < listing.size() && listing[i];) {
        const char *key = listing.data() + i;
        if (is_status_or_filter_key(key))
            return refuse(WFS_E_GIT_POLICY, "%s includes %s, which sets %s", directive, path, key);
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
        if (nl && resolve_include(nl + 1, path, target))
            if (int nrc = scan_include_target(root, target.c_str(), directive, depth + 1)) return nrc;
        i += strlen(entry) + 1;
    }
    return 0;
}
int reject_ambient_policy(const char *root) {
    if (getenv("GIT_ATTR_SOURCE")) return refuse(WFS_E_GIT_POLICY, "GIT_ATTR_SOURCE is set in the environment");
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
    // Conditional includes, active or not: "<origin>\0<key>\n<value>\0".
    Vec<char> includes; status = -1;
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
        if (int src = scan_include_target(root, target.c_str(), directive.c_str(), 0)) return src;
    }
    return 0;
}
// Tracked files whose `filter` attribute names a defined driver (Git LFS, git-crypt, ...) would
// have that driver executed by status in the source and in the World; WorldFS neither runs nor
// reproduces it. A driver that is defined but unused (a machine-wide `git lfs install` in a
// repository without LFS files) or used but undefined (pointer files as plain content) is fine.
int reject_used_filters(const char *root) {
    const char *defined_args[] = {"config", "--null", "--name-only", "--get-regexp", "^filter\\.", nullptr};
    Vec<char> defined; int status = -1;
    int rc = git(root, defined_args, &defined, &status, false, true);
    if (rc == WFS_E_GIT_FAILED && status == 1) return 0; // no driver anywhere: nothing can run
    if (rc) return rc;
    Vec<char> paths;
    const char *ls_args[] = {"ls-files", "-z", nullptr};
    if ((rc = git(root, ls_args, &paths))) return rc;
    if (paths.size() <= 1) return 0;
    FILE *input = tmpfile();
    if (!input) return -errno;
    if (fwrite(paths.data(), 1, paths.size() - 1, input) != paths.size() - 1 || fflush(input)) {
        int err = errno ? -errno : -EIO; fclose(input); return err;
    }
    rewind(input);
    const char *attr_args[] = {"check-attr", "--stdin", "-z", "filter", nullptr};
    Vec<char> attrs;
    rc = git(root, attr_args, &attrs, nullptr, false, true, fileno(input));
    fclose(input);
    if (rc) return rc;
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
            if (strncmp(key + 7, driver, dlen) || key[7 + dlen] != '.') continue;
            const char *field = key + 8 + dlen;
            if (!strcmp(field, "clean") || !strcmp(field, "smudge") || !strcmp(field, "process"))
                return refuse(WFS_E_GIT_POLICY, "tracked file %s uses the '%s' filter (e.g. Git LFS), which WorldFS does not run", path, driver);
        }
    }
    return 0;
}
int reject_external_visibility_state(const char *root, bool managed) {
    if (managed) return 0;
    for (const char *key : {"transfer.hideRefs", "uploadpack.hideRefs"})
        if (int rc = reject_configured_policy(root, key)) return rc;
    const char *args[] = {"reflog", "exists", "refs/stash", nullptr};
    int status = -1, rc = git(root, args, nullptr, &status);
    if (!rc) return refuse(WFS_E_GIT_UNSUPPORTED, "the stash has entries; a mirror cannot preserve the stash stack");
    if (rc != WFS_E_GIT_FAILED || status != 1) return rc;
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
// GIT_OPTIONAL_LOCKS=0 (set by git()) keeps status from refreshing the index it inspects.
int require_clean_tree(const char *root) {
    const char *args[] = {"status", "--porcelain=v1", "-z", "--untracked-files=normal",
        "--", ".", ":(exclude).world", ":(exclude).world-git", nullptr};
    Vec<char> dirty;
    // With the user's own global/system configuration (global ignores, autocrlf, ...), so
    // "clean" means what `git status` in the source and in the World both say.
    if (int rc = git(root, args, &dirty, nullptr, false, true)) return rc;
    return dirty.size() > 1 ? WFS_E_GIT_DIRTY : 0;
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
    bool worktree_config = false; Vec<GitSetting> worktree_settings;
    if (int wt_rc = capture_worktree_config(s.root.c_str(), worktree_config, worktree_settings)) return wt_rc;
    if (worktree_config != s.worktree_config || !same_settings(worktree_settings, s.worktree_settings)) return -EBUSY;
    if (!s.managed) {
        Vec<GitSetting> carried;
        if (int carry_rc = capture_carried_config(s.root.c_str(), carried)) return carry_rc;
        if (!same_settings(carried, s.carried)) return -EBUSY;
    }
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
    return reject_unlisted_symrefs(s.root.c_str(), refs);
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
    Vec<const char *> args;
    for (const char *a : {"--no-replace-objects", "rev-list", "-n", "1", "--full-history", "--all"}) args.emplace_back(a);
    for (const auto &tip : tips) args.emplace_back(tip.c_str());
    for (const char *a : {"--", ":(top,literal).world", ":(top,literal).world-git"}) args.emplace_back(a);
    args.emplace_back(nullptr);
    Vec<char> hit;
    if (int rc = git(root, args.data(), &hit)) return rc;
    return hit.size() > 1 ? refuse(WFS_E_GIT_UNSUPPORTED, "a preserved commit tracks the reserved path .world or .world-git") : 0;
}
int git_source(const char *root, bool include_changes, GitSource &out) {
    g_reason[0] = '\0';
    if (int rc = nested_check(root)) return rc;
    String dot = joinp(root, ".git"), managed = joinp(root, ".world-git");
    struct stat st;
    bool has_managed = lstat(managed.c_str(), &st) == 0;
    if (!has_managed && errno != ENOENT) return -errno;
    if (lstat(dot.c_str(), &st))
        return errno == ENOENT && !has_managed ? 0 : refuse(WFS_E_GIT_UNSUPPORTED, ".world-git exists without its .git marker");
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) return refuse(WFS_E_GIT_UNSUPPORTED, ".git is neither a directory nor a file");
    out.present = true; out.root = root;
    if (has_managed) {
        Vec<char> contents;
        if (int rc = read_bytes(dot.c_str(), contents)) return rc;
        if (contents.size() != strlen(marker) || memcmp(contents.data(), marker, contents.size()))
            return refuse(WFS_E_GIT_UNSUPPORTED, "the .git file is not the WorldFS marker");
        out.managed = true;
    }
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
    if (out.managed) {
        // A managed copy has byte-identical configuration files and import commands ignore
        // global/system scope, so the effective list can only differ through an include that
        // resolves differently from the copy's location (hooksPath, identity, anything).
        const char *list[] = {"config", "--list", "--includes", "--null", nullptr};
        if (int list_rc = git(root, list, &out.effective_config)) return list_rc;
    }
    const char *head_args[] = {"rev-parse", "--verify", "HEAD^{commit}", nullptr};
    if (value(root, head_args, out.head)) return refuse(WFS_E_GIT_UNSUPPORTED, "the repository has no commit yet");
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
    if ((rc = reject_unlisted_symrefs(root, out.symrefs))) return rc;
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
        if (!strncmp(entry, "160000 ", 7))
            return refuse(WFS_E_GIT_UNSUPPORTED, "submodule (gitlink) %s", strchr(entry, '\t') ? strchr(entry, '\t') + 1 : entry);
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
// The identity the source's commits would carry, with the user's ambient configuration: a
// conditional include (`includeIf "gitdir:~/work/"`) can supply it at the source's location and
// not at the World's. When the World would resolve a different identity, the source's is written
// to the World's own configuration, so its commits are attributed the way the source's are.
int pin_identity(const char *source, const char *clone) {
    for (const char *key : {"user.name", "user.email"}) {
        const char *args[] = {"config", "--get", key, nullptr};
        Vec<char> sv, cv; int ss = -1, cs = -1;
        int rc = git(source, args, &sv, &ss, false, true);
        if (rc == WFS_E_GIT_FAILED && ss == 1) continue; // the source has none: nothing to keep
        if (rc) return rc;
        rc = git(clone, args, &cv, &cs, false, true);
        if (rc && !(rc == WFS_E_GIT_FAILED && cs == 1)) return rc;
        String want(sv.data()), have(rc ? "" : cv.data());
        if (!want.empty() && want.back() == '\n') want.pop_back();
        if (!have.empty() && have.back() == '\n') have.pop_back();
        if (rc || want != have)
            if (int wrc = config(clone, key, want.c_str())) return wrc;
    }
    return 0;
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
            !same_symrefs(copy.symrefs, s.symrefs) || !same_settings(copy.settings, s.settings) ||
            // A relative include can resolve differently from the copy's location.
            !same_settings(copy.identity, s.identity) || !same_bytes(copy.effective_config, s.effective_config) ||
            copy.worktree_config != s.worktree_config || !same_settings(copy.worktree_settings, s.worktree_settings)) return -EBUSY;
        if (!same_bytes(copy.refs, s.refs) ||
            copy.orig_present != s.orig_present || copy.orig_head != s.orig_head) return -EBUSY;
        if (int rc = reject_copied_locks(clone)) return rc;
        if (int rc = pin_identity(s.root.c_str(), clone)) return rc;
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
    if (s.worktree_config) {
        // With worktreeConfig on, core.bare must live in the main worktree's config.worktree
        // (git-worktree(1)); left in the common config it would make the owned linked worktree
        // bare too. The owned repository's main worktree is the bare mirror itself.
        if (int rc = config(clone, "extensions.worktreeConfig", "true")) return rc;
        const char *bare[] = {"--git-dir", repo.c_str(), "config", "--worktree", "core.bare", "true", nullptr};
        if (int rc = git(clone, bare)) return rc;
        if (int rc = unset_config(clone, "core.bare")) return rc;
        for (const auto &setting : s.worktree_settings) {
            const char *args[] = {"config", "--worktree", setting.key.c_str(), setting.value.c_str(), nullptr};
            if (int rc = git(clone, args)) return rc;
        }
    }
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
    // The mirror's own remote points at the source: it is not an implicit write-back channel and
    // is removed. The source's own remotes, upstreams and aliases are carried instead.
    const char *remote[] = {"config", "--local", "--remove-section", "remote.origin", nullptr};
    if (int rc = git(clone, remote)) return rc;
    for (const auto &setting : s.carried) {
        const char *args[] = {"config", "--local", "--add", setting.key.c_str(), setting.value.c_str(), nullptr};
        if (int rc = git(clone, args)) return rc;
    }
    Vec<char> imported_refs;
    if (int rc = capture_refs(clone, imported_refs)) return rc;
    if (!same_bytes(imported_refs, s.refs)) return -EBUSY;
    if (int rc = source_unchanged(s)) return rc;
    if (int rc = pin_identity(s.root.c_str(), clone)) return rc;
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
    if (!s.managed) return refuse(WFS_E_GIT_UNSUPPORTED, "not a WorldFS-managed Git World");
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

extern "C" const char *wfs_git_reason(void) { return wfs::g_reason; }

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
    if (!fs_realpath(world_root, world_real) && world_real == repo_real)
        return refuse(WFS_E_GIT_TARGET, "the target repository is the World itself");
    // A branch checked out in any worktree of the target would be moved under its user.
    Vec<char> worktrees;
    const char *wt_args[] = {"worktree", "list", "--porcelain", "-z", nullptr};
    if (int rc = git(repo, wt_args, &worktrees)) return rc;
    String checked("branch "); checked.append(ref.c_str());
    for (size_t i = 0; i < worktrees.size() && worktrees[i];) {
        const char *line = worktrees.data() + i;
        if (!strcmp(line, checked.c_str()))
            return refuse(WFS_E_GIT_TARGET, "%s is checked out in the target repository; choose another name with --branch", branch);
        i += strlen(line) + 1;
    }
    // The World's own status is a diagnostic for the caller; take it before anything changes, so
    // an error here can never be reported for a publication that already happened.
    int dirty = require_clean_tree(world_root);
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
    String source_ref;
    if (info.branch[0]) { source_ref.assign("refs/heads/"); source_ref.append(info.branch); }
    else source_ref.assign("HEAD");
    String spec;
    spec.append(source_ref.c_str()); spec.push_back(':'); spec.append(staging);
    const char *fetch[] = {"fetch", "--quiet", "--no-tags", "--no-write-fetch-head", "--no-recurse-submodules",
                           "--", world_root, spec.c_str(), nullptr};
    int rc = git(repo, fetch);
    String now;
    if (!rc) {
        const char *now_args[] = {"rev-parse", "--verify", staging, nullptr};
        rc = value(repo, now_args, now);
    }
    if (!rc && !force) {
        // Shared history: at least one of the World's commits is already in the repository.
        // An unrelated repository would otherwise gain a branch with a foreign root.
        String exclude("--exclude="); exclude.append(staging);
        const char *all_args[] = {"rev-list", "--count", now.c_str(), nullptr};
        const char *new_args[] = {"rev-list", "--count", now.c_str(), exclude.c_str(), "--not", "--all", nullptr};
        String all, fresh;
        if (!(rc = value(repo, all_args, all)) && !(rc = value(repo, new_args, fresh)) && all == fresh)
            rc = refuse(WFS_E_GIT_TARGET, "%s shares no history with the World; is it the repository the World came from? (--force skips this check)", repo);
    }
    if (!rc && !force && !old.empty() && old != now) {
        const char *ff[] = {"merge-base", "--is-ancestor", old.c_str(), now.c_str(), nullptr};
        int status = -1;
        int ff_rc = git(repo, ff, nullptr, &status, true);
        if (ff_rc == WFS_E_GIT_FAILED && status == 1)
            rc = refuse(WFS_E_GIT_TARGET, "%s in the target repository has commits the World's branch does not; this is not a fast-forward (--force overwrites it)", branch);
        else rc = ff_rc;
    }
    if (!rc && old != now) {
        // Compare-and-swap against the value checked above; hooks are disabled by git().
        const char *zero = info.head[0] && strlen(info.head) == 64
            ? "0000000000000000000000000000000000000000000000000000000000000000"
            : "0000000000000000000000000000000000000000";
        const char *update[] = {"update-ref", "-m", "world fs publish", ref.c_str(), now.c_str(),
                                old.empty() ? zero : old.c_str(), nullptr};
        rc = git(repo, update);
    }
    int drop_rc = 0;
    if (!now.empty()) {
        // Delete only the value this call fetched there.
        const char *drop[] = {"update-ref", "-d", staging, now.c_str(), nullptr};
        drop_rc = git(repo, drop, nullptr, nullptr, true);
    }
    if (rc) return rc;
    if (drop_rc) return drop_rc;
    snprintf(out->ref, sizeof out->ref, "%s", ref.c_str());
    snprintf(out->old_oid, sizeof out->old_oid, "%s", old.c_str());
    snprintf(out->new_oid, sizeof out->new_oid, "%s", now.c_str());
    out->dirty = dirty == WFS_E_GIT_DIRTY;
    return 0;
}
