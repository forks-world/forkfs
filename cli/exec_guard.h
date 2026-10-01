#pragma once
// Pure helpers behind `world exec`'s Git guard, kept apart from main.cpp so they can be unit
// tested (cli/tests/exec_guard_test.cpp). libc only (arch.md §39).
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Escape `in` so a seatbelt regex matches it literally. Every POSIX ERE metacharacter gets a
// backslash; the result is still to be written as an SBPL string (sb_quote), which escapes the
// backslashes and quotes once more. A World path is user-chosen: `W.1` must not also match
// `WX1`, and `a[b]` must match itself, not `ab`. Returns false when `out` is too small.
static inline bool sb_regex_escape(const char *in, char *out, size_t cap) {
    size_t at = 0;
    for (const char *p = in; *p; ++p) {
        if (strchr(".[]()*+?{}|^$\\", *p)) {
            if (at + 1 >= cap) return false;
            out[at++] = '\\';
        }
        if (at + 1 >= cap) return false;
        out[at++] = *p;
    }
    if (at >= cap) return false;
    out[at] = 0;
    return true;
}

// Encode one config value before joining records with raw 0x1e. Tags preserve
// implicit booleans versus explicit empty strings; escapes preserve value boundaries.
static inline char *guard_encode_value(const char *value, bool explicit_value) {
    size_t len = explicit_value && value ? strlen(value) : 0;
    if (len > ((size_t)-1 - 2) / 2) return NULL;
    char *out = (char *)malloc(2 * len + 2);
    if (!out) return NULL;
    size_t at = 0;
    out[at++] = explicit_value ? 'v' : 'n';
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c == 0x1d || c == 0x1e) out[at++] = '\x1d';
        out[at++] = (char)c;
    }
    out[at] = 0;
    return out;
}

// Whether a Git configuration entry, as `git config --list` prints it (section and name
// lowercased, a subsection verbatim), makes Git run a command or load configuration that could.
// `world exec` reports changes to these after the command exits; the list is documented in
// docs/GIT_INTEGRATION.md ("Running agents with world exec").
struct GuardKey {
    const char *section;
    const char *name;  // NULL: any name in the section
    int sub;           // 0: no subsection, 1: subsection required, 2: either
    bool bang;         // only values starting with `!` run a command
};

static const GuardKey kGuardKeys[] = {
    {"core", "attributesfile", 0, false},
    {"core", "worktree", 0, false},       {"tar", "command", 1, false},
    {"core", "hookspath", 0, false},      {"core", "fsmonitor", 0, false},
    {"core", "sshcommand", 0, false},     {"core", "editor", 0, false},
    {"core", "pager", 0, false},          {"core", "askpass", 0, false},
    {"core", "gitproxy", 0, false},       {"core", "alternaterefscommand", 0, false},
    {"sequence", "editor", 0, false},     {"credential", "helper", 2, false},
    {"filter", "clean", 1, false},        {"filter", "smudge", 1, false},
    {"filter", "process", 1, false},      {"diff", "external", 0, false},
    {"diff", "tool", 0, false},           {"diff", "guitool", 0, false},
    {"merge", "tool", 0, false},          {"merge", "guitool", 0, false},
    {"diff", "command", 1, false},        {"diff", "textconv", 1, false},
    {"merge", "driver", 1, false},        {"mergetool", "cmd", 1, false},
    {"mergetool", "path", 1, false},      {"difftool", "cmd", 1, false},
    {"difftool", "path", 1, false},       {"gpg", "program", 2, false},
    {"commit", "gpgsign", 0, false},      {"tag", "gpgsign", 0, false},
    {"tag", "forcesignannotated", 0, false}, {"push", "gpgsign", 0, false},
    {"gpg", "format", 0, false},
    {"gpg", "defaultkeycommand", 2, false}, {"gc", "recentobjectshook", 0, false},
    {"remote", "uploadpack", 1, false},   {"remote", "receivepack", 1, false},
    {"branch", "mergeoptions", 1, false}, {"branch", "remote", 1, false},
    {"branch", "pushremote", 1, false},   {"remote", "pushdefault", 0, false},
    {"remote", "vcs", 1, false},          {"uploadpack", "packobjectshook", 0, false},
    {"sendemail", "tocmd", 2, false},     {"sendemail", "cccmd", 2, false},
    {"sendemail", "headercmd", 2, false}, {"sendemail", "sendmailcmd", 2, false},
    {"sendemail", "smtpserver", 2, false},
    {"include", "path", 0, false},        {"includeif", "path", 1, false},
    {"alias", NULL, 2, false},             {"submodule", "update", 1, true},
    {"pager", NULL, 0, false},            {"interactive", "difffilter", 0, false},
    {"imap", "tunnel", 0, false},
    {"instaweb", "httpd", 0, false},      {"guitool", "cmd", 1, false},
    {"help", "browser", 0, false},        {"help", "format", 0, false},
    {"instaweb", "browser", 0, false},     {"man", "viewer", 0, false},
    {"web", "browser", 0, false},         {"browser", "cmd", 1, false},
    {"browser", "path", 1, false},        {"man", "cmd", 1, false},
    {"man", "path", 1, false},            {"init", "templatedir", 0, false},
    {"hook", "command", 1, false},        {"trailer", "command", 1, false},
    {"trailer", "cmd", 1, false},         {"protocol", "allow", 2, false},
    {"lfs", "path", 1, false},            {"lfs", "clean", 1, false},
    {"lfs", "smudge", 1, false},
};

// Git's transport prefix grammar (url.c): an alphanumeric first byte, then
// alphanumerics or +.-. Explicit helper syntax is distinct from scp/IPv6/local paths.
static inline bool guard_helper_url(const char *url, size_t len) {
    if (!url || !len) return false;
    size_t n = 0;
    for (; n < len; ++n) {
        unsigned char c = (unsigned char)url[n];
        bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum && !(n && (c == '+' || c == '.' || c == '-'))) break;
    }
    if (n + 1 >= len || url[n] != ':') return false;
    if (url[n + 1] == ':') return true;  // even ssh:: and https:: explicitly select helpers
    if (!n || n + 2 >= len || url[n + 1] != '/' || url[n + 2] != '/') return false;
    // Native transports and conventional curl helpers stay quiet for ordinary URL edits.
    // Git's helper dispatch is case-sensitive, so HTTPS:// must not match https:// here.
    const char *ordinary[] = {"file", "git", "ssh", "git+ssh", "ssh+git", "http", "https", "ftp", "ftps"};
    for (size_t i = 0; i < sizeof ordinary / sizeof ordinary[0]; ++i)
        if (strlen(ordinary[i]) == n && !memcmp(url, ordinary[i], n)) return false;
    return true;
}

static inline bool guard_key_runs_command(const char *key, const char *value) {
    const char *first = strchr(key, '.'), *last = strrchr(key, '.');
    if (!first || !last[1]) return false;
    size_t section = (size_t)(first - key);
    bool has_sub = last != first;
    const char *name = last + 1;
    if (has_sub && section == 6 && !strncasecmp(key, "remote", section) &&
        (!strcasecmp(name, "url") || !strcasecmp(name, "pushurl")))
        return value && guard_helper_url(value, strlen(value));
    if (has_sub && section == 9 && !strncasecmp(key, "submodule", section) && !strcasecmp(name, "url"))
        return value && guard_helper_url(value, strlen(value));
    if (has_sub && section == 3 && !strncasecmp(key, "url", section) &&
        (!strcasecmp(name, "insteadof") || !strcasecmp(name, "pushinsteadof")))
        return guard_helper_url(first + 1, (size_t)(last - first - 1));
    for (size_t i = 0; i < sizeof kGuardKeys / sizeof kGuardKeys[0]; ++i) {
        const GuardKey &k = kGuardKeys[i];
        if (strlen(k.section) != section || strncasecmp(key, k.section, section)) continue;
        if ((k.sub == 0 && has_sub) || (k.sub == 1 && !has_sub)) continue;
        if (k.name && strcasecmp(name, k.name)) continue;
        if (k.bang && (!value || value[0] != '!')) continue;
        return true;
    }
    return false;
}
