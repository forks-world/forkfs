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

// Config IDs join label/scope/key with raw 0x1f. Escape field data without
// emitting that separator, including escape bytes themselves, to preserve identity.
static inline char *guard_encode_id_field(const char *value) {
    size_t len = strlen(value);
    if (len > ((size_t)-1 - 1) / 2) return NULL;
    char *out = (char *)malloc(2 * len + 1);
    if (!out) return NULL;
    size_t at = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c == 0x1d || c == 0x1f) {
            out[at++] = '\x1d';
            out[at++] = c == 0x1d ? 'd' : 'f';
        } else out[at++] = (char)c;
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
    {"http", "followredirects", 2, false},
    {"http", "proxy", 2, false}, {"remote", "proxy", 1, false},
    {"http", "sslcert", 2, false}, {"http", "proxysslcert", 2, false},
    {"http", "sslcertpasswordprotected", 2, false},
    {"http", "proxysslcertpasswordprotected", 2, false},
    {"user", "signingkey", 0, false},
    {"credential", "interactive", 0, false},
    {"http", "proactiveauth", 2, false},
    {"core", "attributesfile", 0, false}, {"core", "usereplacerefs", 0, false},
    {"tar", "remote", 1, false},       {"uploadarchive", "allowunreachable", 0, false},
    {"core", "worktree", 0, false},       {"tar", "command", 1, false},
    {"core", "hookspath", 0, false},      {"core", "fsmonitor", 0, false},
    {"core", "sshcommand", 0, false},     {"core", "editor", 0, false},
    {"core", "pager", 0, false},          {"core", "askpass", 0, false},
    {"core", "gitproxy", 0, false},       {"core", "alternaterefscommand", 0, false},
    {"sequence", "editor", 0, false},     {"credential", "helper", 2, false},
    {"filter", "clean", 1, false},        {"filter", "smudge", 1, false},
    {"filter", "process", 1, false},      {"diff", "external", 0, false},
    {"diff", "tool", 0, false},           {"diff", "guitool", 0, false},
    {"difftool", "guidefault", 0, false}, {"mergetool", "guidefault", 0, false},
    {"difftool", "prompt", 0, false}, {"mergetool", "prompt", 0, false},
    {"merge", "tool", 0, false},          {"merge", "guitool", 0, false},
    {"diff", "command", 1, false},        {"diff", "textconv", 1, false},
    {"merge", "defaulttoupstream", 0, false}, {"merge", "ff", 0, false},
    {"merge", "recursive", 1, false},
    {"merge", "default", 0, false},      {"merge", "renormalize", 0, false},
    {"merge", "driver", 1, false},        {"mergetool", "cmd", 1, false},
    {"mergetool", "path", 1, false},      {"difftool", "cmd", 1, false},
    {"difftool", "path", 1, false},       {"gpg", "program", 2, false},
    {"commit", "gpgsign", 0, false},      {"tag", "gpgsign", 0, false},
    {"tag", "forcesignannotated", 0, false}, {"push", "gpgsign", 0, false},
    {"gpg", "format", 0, false},
    {"log", "showsignature", 0, false},   {"merge", "verifysignatures", 0, false},
    {"rebase", "autostash", 0, false},
    {"pull", "autostash", 0, false}, {"merge", "autostash", 0, false},
    {"rebase", "instructionformat", 0, false},
    {"format", "commitlistformat", 0, false}, {"format", "coverletter", 0, false},
    {"format", "pretty", 0, false},       {"pretty", NULL, 2, false},
    {"gpg", "defaultkeycommand", 2, false}, {"gc", "recentobjectshook", 0, false},
    {"fetch", "bundleuri", 0, false},
    {"fetch", "all", 0, false},          {"remotes", NULL, 2, false},
    {"remote", "skipdefaultupdate", 1, false}, {"remote", "skipfetchall", 1, false},
    {"remote", "url", 1, false},          {"remote", "pushurl", 1, false},
    {"url", "insteadof", 1, false},       {"url", "pushinsteadof", 1, false},
    {"remote", "uploadpack", 1, false},   {"remote", "receivepack", 1, false},
    {"push", "default", 0, false}, {"push", "autosetupremote", 0, false},
    {"branch", "merge", 1, false},
    {"remote", "push", 1, false}, {"remote", "mirror", 1, false},
    {"receive", "procreceiverefs", 0, false},
    {"receive", "denydeletecurrent", 0, false},
    {"receive", "denynonfastforwards", 0, false}, {"receive", "denydeletes", 0, false},
    {"receive", "autogc", 0, false},    {"maintenance", "auto", 0, false},
    {"maintenance", "strategy", 0, false}, {"maintenance", "repo", 0, false},
    {"gc", "auto", 0, false},          {"gc", "autopacklimit", 0, false},
    {"receive", "denycurrentbranch", 0, false}, {"remote", "promisor", 1, false},
    {"remote", "partialclonefilter", 1, false}, {"extensions", "partialclone", 0, false},
    {"checkout", "guess", 0, false}, {"checkout", "defaultremote", 0, false},
    {"pull", "rebase", 0, false}, {"branch", "rebase", 1, false},
    {"pull", "ff", 0, false},
    {"pull", "twohead", 0, false},        {"pull", "octopus", 0, false},
    {"branch", "mergeoptions", 1, false}, {"branch", "remote", 1, false},
    {"branch", "pushremote", 1, false},   {"remote", "pushdefault", 0, false},
    {"uploadpack", "hiderefs", 0, false}, {"receive", "hiderefs", 0, false},
    {"transfer", "hiderefs", 0, false},
    {"remote", "vcs", 1, false},          {"uploadpack", "packobjectshook", 0, false},
    {"sendemail", "identity", 0, false},
    {"sendemail", "confirm", 2, false},
    {"sendemail", "annotate", 2, false}, {"sendemail", "suppresscc", 2, false},
    {"sendemail", "validate", 2, false}, {"sendemail", "useimaponly", 2, false},
    {"sendemail", "imapsentfolder", 2, false},
    {"sendemail", "tocmd", 2, false},     {"sendemail", "cccmd", 2, false},
    {"sendemail", "headercmd", 2, false}, {"sendemail", "sendmailcmd", 2, false},
    {"sendemail", "smtpserver", 2, false},
    {"include", "path", 0, false},        {"includeif", "path", 1, false},
    {"submodule", "active", 2, false},   {"submodule", "url", 1, false},
    {"submodule", "recurse", 0, false},  {"fetch", "recursesubmodules", 0, false},
    {"submodule", "fetchrecursesubmodules", 1, false},
    {"submodule", "ignore", 1, false},
    {"diff", "ignoresubmodules", 0, false}, {"diff", "submodule", 0, false},
    {"status", "submodulesummary", 0, false},
    {"push", "recursesubmodules", 0, false},
    {"alias", NULL, 2, false},             {"submodule", "update", 1, false},
    {"pager", NULL, 0, false},            {"interactive", "difffilter", 0, false},
    {"imap", "tunnel", 0, false},
    {"instaweb", "httpd", 0, false},      {"guitool", "cmd", 1, false},
    {"help", "autocorrect", 0, false},
    {"help", "browser", 0, false},        {"help", "format", 0, false},
    {"instaweb", "browser", 0, false},     {"man", "viewer", 0, false},
    {"web", "browser", 0, false},         {"browser", "cmd", 1, false},
    {"browser", "path", 1, false},        {"man", "cmd", 1, false},
    {"man", "path", 1, false},            {"init", "templatedir", 0, false},
    {"hook", "command", 1, false},        {"trailer", "command", 1, false},
    {"trailer", "cmd", 1, false},         {"protocol", "allow", 2, false},
    {"lfs", "gitprotocol", 0, false},
    {"lfs", "url", 0, false}, {"lfs", "pushurl", 0, false},
    {"remote", "lfsurl", 1, false}, {"remote", "lfspushurl", 1, false},
    {"lfs", "access", 2, false},
    {"lfs", "fetchinclude", 0, false}, {"lfs", "fetchexclude", 0, false},
    {"lfs", "basictransfersonly", 0, false},
    {"lfs", "standalonetransferagent", 2, false},
    {"lfs", "path", 1, false},            {"lfs", "clean", 1, false},
    {"lfs", "smudge", 1, false},
};

// Git ref sorting accepts reverse/version prefixes and an optional dereference marker.
// Signature atoms invoke the configured verifier; ordinary name/version sorts stay quiet.
static inline bool guard_signature_sort(const char *value) {
    if (!value) return false;
    if (*value == '-') ++value;
    if (!strncmp(value, "version:", 8)) value += 8;
    else if (!strncmp(value, "v:", 2)) value += 2;
    if (*value == '*') ++value;
    return !strncmp(value, "signature", 9) && (!value[9] || value[9] == ':');
}

static inline bool guard_key_runs_command(const char *key, const char *value) {
    const char *first = strchr(key, '.'), *last = strrchr(key, '.');
    if (!first || !last[1]) return false;
    size_t section = (size_t)(first - key);
    bool has_sub = last != first;
    const char *name = last + 1;
    // Custom transfer arguments and direction select behavior of an existing adapter.
    // Restrict these to lfs.customtransfer.<name>; URL-scoped LFS keys are different.
    if (has_sub && section == 3 && !strncasecmp(key, "lfs", section) &&
        last - first > 16 && !strncasecmp(first + 1, "customtransfer.", 15) &&
        !memchr(first + 16, '.', (size_t)(last - first - 16)) &&
        (!strcasecmp(name, "args") || !strcasecmp(name, "direction"))) return true;
    // Git's task names are case-sensitive subsections; unrelated maintenance tasks do
    // not select these hook- or remote-running operations.
    if (has_sub && section == 11 && !strncasecmp(key, "maintenance", section)) {
        size_t task = (size_t)(last - first - 1);
        bool gc = task == 2 && !memcmp(first + 1, "gc", 2);
        bool prefetch = task == 8 && !memcmp(first + 1, "prefetch", 8);
        if ((gc || prefetch) && !strcasecmp(name, "enabled")) return true;
        if ((gc || prefetch) && !strcasecmp(name, "schedule")) return true;
    }
    if (!has_sub && !strcasecmp(name, "sort") &&
        ((section == 6 && !strncasecmp(key, "branch", section)) ||
         (section == 3 && !strncasecmp(key, "tag", section))))
        return guard_signature_sort(value);
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
