// `world exec`'s Git guard: the seatbelt regex escaping and the command-running key allowlist.
// Plain asserts and libc only, like core_test.cpp (arch.md §39). Whether seatbelt honours the
// escaped rules end to end is covered by cli/tests/git_test.py.
#include "../exec_guard.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static void escapes(const char *in, const char *want) {
    char out[256];
    CHECK(sb_regex_escape(in, out, sizeof out));
    if (strcmp(out, want)) {
        fprintf(stderr, "sb_regex_escape(%s) = %s, wanted %s\n", in, out, want);
        exit(1);
    }
}

int main() {
    escapes("/Users/me/worlds/W1/app", "/Users/me/worlds/W1/app");
    escapes("/tmp/w.1", "/tmp/w\\.1");
    escapes("/a+b/(c)/[d]/{2}", "/a\\+b/\\(c\\)/\\[d\\]/\\{2\\}");
    escapes("/x*y?z|w^v$", "/x\\*y\\?z\\|w\\^v\\$");
    escapes("/back\\slash", "/back\\\\slash");
    // Not regex metacharacters: left alone (sb_quote handles the quote).
    escapes("/q\"uote -_ ~%,", "/q\"uote -_ ~%,");
    escapes("", "");

    char small[5];
    CHECK(sb_regex_escape("abcd", small, sizeof small));
    CHECK(!strcmp(small, "abcd"));
    CHECK(!sb_regex_escape("abcde", small, sizeof small));
    CHECK(!sb_regex_escape("ab.d", small, sizeof small));  // "ab\.d" needs 6 bytes

    // Keys that make Git run a command.
    const char *runs[] = {
        "core.attributesfile", "core.worktree", "tar.tar.gz.command", "tar.custom.command", "gc.recentobjectshook",
        "core.hookspath", "core.fsmonitor", "core.sshcommand", "core.editor", "core.pager",
        "core.askpass", "core.gitproxy", "core.alternaterefscommand", "sequence.editor",
        "credential.helper", "credential.https://example.com.helper", "filter.lfs.clean",
        "filter.x.smudge", "filter.x.process", "diff.external", "diff.pdf.textconv",
        "diff.pdf.command", "merge.ours.driver", "mergetool.vim.cmd", "difftool.x.path",
        "gpg.program", "gpg.ssh.program", "gpg.ssh.defaultkeycommand", "remote.origin.uploadpack",
        "remote.origin.receivepack", "remote.origin.vcs", "uploadpack.packobjectshook",
        "sendemail.tocmd", "sendemail.work.sendmailcmd", "sendemail.smtpserver", "include.path",
        "includeif.gitdir:/x/.path", "pager.log", "interactive.difffilter", "web.browser",
        "imap.tunnel", "instaweb.httpd", "guitool.test.cmd", "browser.ff.cmd", "man.x.cmd", "init.templatedir", "hook.lint.command",
        "trailer.sign.command", "protocol.allow", "protocol.ext.allow",
        "lfs.customtransfer.x.path", "lfs.extension.x.clean",
        "CORE.HOOKSPATH", "Core.FsMonitor",
    };
    for (const char *k : runs) {
        if (!guard_key_runs_command(k, "cmd")) { fprintf(stderr, "%s should be watched\n", k); exit(1); }
    }
    // All aliases can dispatch external commands or inject -c command-running settings.
    // Submodule update runs commands only with a leading `!`.
    CHECK(guard_key_runs_command("alias.x", "!rm -rf /"));
    CHECK(guard_key_runs_command("alias.co", "checkout"));
    CHECK(guard_key_runs_command("alias.x", "-c core.sshCommand=./payload ls-remote origin"));
    CHECK(guard_key_runs_command("alias.external", "custom-command"));
    CHECK(guard_key_runs_command("submodule.lib.update", "!sh evil"));
    CHECK(!guard_key_runs_command("submodule.lib.update", "rebase"));
    CHECK(guard_key_runs_command("alias.x", NULL));

    // Helper transports can execute programs through direct URLs or rewrite targets.
    const char *helpers[] = {"::repo", "ext::sh -c evil", "custom::repo", "https::repo", "ssh::repo",
                            "custom://repo", "9helper://repo", "HTTPS://example.com/repo", "a+b.c-d://repo"};
    for (const char *url : helpers) {
        CHECK(guard_key_runs_command("remote.origin.url", url));
        CHECK(guard_key_runs_command("remote.origin.pushurl", url));
        CHECK(guard_key_runs_command("submodule.library.url", url));
        char key[256];
        snprintf(key, sizeof key, "url.%s.insteadOf", url);
        CHECK(guard_key_runs_command(key, "https://example.com/"));
        snprintf(key, sizeof key, "url.%s.pushInsteadOf", url);
        CHECK(guard_key_runs_command(key, "work:"));
    }
    const char *ordinary[] = {"ssh://host/repo", "http://host/repo", "https://host/repo",
                             "ftp://host/repo", "ftps://host/repo", "git://host/repo", "file:///repo", "git+ssh://host/repo", "ssh+git://host/repo",
                             "user@host:repo", "user@[::1]:repo", "[::1]:repo", "ssh://[::1]/repo", "./path::repo",
                             "/tmp/path::repo", "relative/path::repo", "host:repo", "_x::repo", "x_y::repo", "bad_helper://repo", ""};
    for (const char *url : ordinary) {
        CHECK(!guard_key_runs_command("remote.origin.url", url));
        CHECK(!guard_key_runs_command("remote.origin.pushurl", url));
        CHECK(!guard_key_runs_command("submodule.library.url", url));
        char key[256];
        snprintf(key, sizeof key, "url.%s.insteadof", url);
        CHECK(!guard_key_runs_command(key, "ext::value-is-not-the-target"));
    }
    CHECK(!guard_key_runs_command("submodule.url", "ext::evil"));
    CHECK(!guard_key_runs_command("submodule.library.url", NULL));
    CHECK(!guard_key_runs_command("remote.url", "ext::evil"));
    CHECK(!guard_key_runs_command("remote.origin.url", NULL));

    // Keys that legitimately change during agent work, or only name things.
    const char *quiet[] = {
        "user.email", "user.name", "remote.origin.url", "remote.origin.fetch",
        "branch.main.remote", "branch.main.merge", "core.bare", "tar.command", "tar.custom.remote",
        "gc.custom.recentobjectshook", "gc.pruneexpire", "core.editorx", "core.custom.attributesfile", "filter.clean", "diff.command", "merge.driver", "core.x.hookspath",
        "include.x.path", "includeif.path", "hook.command", "remote.uploadpack", "merge.tool",
        "imap.host", "imap.custom.tunnel", "instaweb.port", "instaweb.custom.httpd", "guitool.cmd", "guitool.test.title",
        "credential.username", "gpg.format", "init.defaultbranch", "", ".", "core.", "nodot",
    };
    for (const char *k : quiet) {
        if (guard_key_runs_command(k, "!cmd")) { fprintf(stderr, "%s should not be watched\n", k); exit(1); }
    }
    puts("exec_guard_test: ok");
    return 0;
}
