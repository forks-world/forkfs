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

    // Encoding is injective across raw separators, escape bytes and value presence.
    char *single = guard_encode_value("status\x1e!payload", true);
    char *escaped = guard_encode_value("\x1d\x1e\\x1e", true);
    char *empty = guard_encode_value("", true);
    char *implicit = guard_encode_value("", false);
    CHECK(single && escaped && empty && implicit);
    CHECK(!strcmp(single, "vstatus\x1d\x1e!payload"));
    CHECK(strcmp(single, "vstatus\x1ev!payload"));  // two independently tagged values
    CHECK(!strcmp(escaped, "v\x1d\x1d\x1d\x1e\\x1e"));
    CHECK(!strcmp(empty, "v"));
    CHECK(!strcmp(implicit, "n"));
    CHECK(strcmp(empty, implicit));
    free(single); free(escaped); free(empty); free(implicit);

    char *field = guard_encode_id_field("a\x1f" "local\x1f" "credential.x");
    char *field_escape = guard_encode_id_field("a\x1d" "f");
    CHECK(field && field_escape);
    CHECK(!strchr(field, '\x1f'));
    CHECK(!strcmp(field, "a\x1d" "flocal\x1d" "fcredential.x"));
    CHECK(!strcmp(field_escape, "a\x1d" "df"));
    CHECK(strcmp(field_escape, "a\x1d" "f"));
    free(field); free(field_escape);

    // Keys that make Git run a command.
    const char *runs[] = {
        "rebase.autostash", "REBASE.AUTOSTASH", "pull.autostash", "merge.autostash",
        "checkout.defaultremote", "checkout.guess", "pull.rebase", "branch.main.rebase", "branch.team.topic.rebase",
        "lfs.gitprotocol", "pull.ff", "lfs.url", "lfs.pushurl", "remote.origin.lfsurl", "remote.team.origin.lfspushurl",
        "lfs.fetchinclude", "lfs.fetchexclude", "LFS.FETCHINCLUDE", "LFS.FETCHEXCLUDE",
        "lfs.access", "lfs.https://example.invalid/repo.access", "LFS.ACCESS", "merge.defaulttoupstream", "merge.ff",
        "http.followredirects", "http.https://example.invalid/repo.followredirects", "HTTP.FOLLOWREDIRECTS",
        "http.proactiveauth", "http.https://example.invalid/repo.proactiveauth", "HTTP.PROACTIVEAUTH",
        "credential.interactive", "CREDENTIAL.INTERACTIVE",
        "push.autosetupremote", "branch.main.merge", "branch.team.topic.merge",
        "push.default", "remote.origin.push", "remote.origin.mirror", "remote.team.origin.push",
        "http.proxy", "http.https://example.invalid/repo.proxy", "remote.origin.proxy", "remote.team.origin.proxy",
        "http.sslcert", "http.proxysslcert", "http.https://example.invalid/repo.sslcert",
        "HTTP.https://example.invalid/repo.PROXYSSLCERT",
        "http.sslcertpasswordprotected", "http.proxysslcertpasswordprotected", "user.signingkey",
        "http.https://example.invalid/repo.sslcertpasswordprotected",
        "HTTP.https://example.invalid/repo.PROXYSSLCERTPASSWORDPROTECTED",
        "receive.autogc", "maintenance.auto", "maintenance.strategy", "maintenance.repo", "gc.auto", "gc.autopacklimit",
        "attr.tree", "ATTR.TREE", "maintenance.pack-refs.enabled", "MAINTENANCE.pack-refs.SCHEDULE",
        "maintenance.gc.enabled", "maintenance.prefetch.enabled", "maintenance.prefetch.schedule", "maintenance.gc.schedule", "fetch.bundleuri",
        "MAINTENANCE.gc.ENABLED", "Maintenance.prefetch.Schedule",
        "remote.origin.partialclonefilter", "extensions.partialclone",
        "receive.denynonfastforwards", "RECEIVE.DENYNONFASTFORWARDS",
        "receive.denydeletes", "RECEIVE.DENYDELETES",
        "uploadpack.hiderefs", "receive.hiderefs", "transfer.hiderefs",
        "UPLOADPACK.HIDEREFS", "RECEIVE.HIDEREFS", "TRANSFER.HIDEREFS",
        "receive.fsck.missingemail", "FETCH.fsck.SKIPLIST", "RECEIVE.fsck.MISSINGEMAIL",
        "fetch.fsck.zeropaddedfilemode", "receive.fsck.skiplist",
        "push.useforceifincludes", "PUSH.USEFORCEIFINCLUDES", "receive.certnonceseed", "RECEIVE.CERTNONCESEED",
        "push.followtags", "PUSH.FOLLOWTAGS", "push.pushoption", "PUSH.PUSHOPTION",
        "receive.shallowupdate", "RECEIVE.SHALLOWUPDATE",
        "receive.advertiseatomic", "RECEIVE.ADVERTISEATOMIC", "receive.advertisepushoptions", "RECEIVE.ADVERTISEPUSHOPTIONS",
        "receive.fsckobjects", "RECEIVE.FSCKOBJECTS", "receive.maxinputsize", "RECEIVE.MAXINPUTSIZE",
        "receive.denydeletecurrent", "RECEIVE.DENYDELETECURRENT", "core.usereplacerefs", "CORE.USEREPLACEREFS",
        "difftool.prompt", "mergetool.prompt", "DIFFTOOL.PROMPT", "MERGETOOL.PROMPT",
        "receive.denycurrentbranch", "remote.origin.promisor", "remote.team.origin.promisor",
        "uploadarchive.allowunreachable", "tar.custom.remote", "tar.tar.gz.remote",
        "core.attributesfile", "core.worktree", "tar.tar.gz.command", "tar.custom.command", "gc.recentobjectshook",
        "core.hookspath", "core.fsmonitor", "core.sshcommand", "core.editor", "core.pager",
        "core.askpass", "core.gitproxy", "core.alternaterefscommand", "sequence.editor",
        "credential.helper", "credential.https://example.com.helper", "filter.lfs.clean",
        "difftool.guidefault", "mergetool.guidefault", "diff.tool", "diff.guitool", "merge.tool", "merge.guitool",
        "filter.x.smudge", "filter.x.process", "diff.external", "diff.pdf.textconv",
        "merge.default", "merge.renormalize", "merge.ours.recursive", "merge.team.driver.recursive",
        "diff.pdf.command", "merge.ours.driver", "mergetool.vim.cmd", "difftool.x.path",
        "diff.ignoresubmodules", "diff.submodule", "status.submodulesummary",
        "submodule.library.ignore", "submodule.team.library.ignore",
        "submodule.recurse", "push.recursesubmodules", "fetch.recursesubmodules", "submodule.library.fetchrecursesubmodules",
        "format.commitlistformat", "format.coverletter", "rebase.instructionformat", "submodule.active", "submodule.library.active", "submodule.team.library.active",
        "log.showsignature", "merge.verifysignatures", "format.pretty", "pretty.signature", "pretty.team.signature",
        "commit.gpgsign", "tag.gpgsign", "tag.forcesignannotated", "push.gpgsign", "gpg.format",
        "gpg.program", "gpg.ssh.program", "gpg.ssh.defaultkeycommand", "remote.origin.uploadpack",
        "am.threeway", "AM.THREEWAY", "fetch.prune", "fetch.prunetags", "FETCH.PRUNETAGS",
        "remote.origin.tagopt", "REMOTE.Team.Origin.TAGOPT",
        "remote.origin.fetch", "remote.team.origin.fetch", "REMOTE.Origin.FETCH",
        "remote.origin.prune", "remote.team.origin.prunetags", "REMOTE.Origin.PRUNE",
        "fetch.fsckobjects", "TRANSFER.FSCKOBJECTS",
        "hook.lint.event", "HOOK.Lint.ENABLED", "hook.pre-commit.enabled", "hook.team.lint.event",
        "gc.packrefs", "GC.PACKREFS", "fetch.followremotehead", "FETCH.FOLLOWREMOTEHEAD",
        "remote.origin.followremotehead", "REMOTE.Team.Origin.FOLLOWREMOTEHEAD",
        "fetch.all", "remotes.default", "remotes.team.group",
        "remote.origin.skipdefaultupdate", "remote.origin.skipfetchall", "remote.team.origin.skipfetchall",
        "pull.twohead", "pull.octopus", "receive.procreceiverefs", "branch.main.mergeoptions", "branch.main.remote", "branch.main.pushremote", "remote.pushdefault",
        "branch.topic.with.dots.mergeoptions", "remote.origin.receivepack", "remote.origin.vcs", "uploadpack.packobjectshook",
        "sendemail.confirm", "sendemail.work.confirm", "SENDEMAIL.CONFIRM", "SENDEMAIL.Work.CONFIRM",
        "sendemail.annotate", "sendemail.work.annotate", "sendemail.suppresscc", "sendemail.work.suppresscc",
        "sendemail.validate", "sendemail.work.validate", "sendemail.useimaponly", "sendemail.work.useimaponly",
        "sendemail.imapsentfolder", "sendemail.work.imapsentfolder", "sendemail.identity", "sendemail.tocmd", "sendemail.work.sendmailcmd", "sendemail.smtpserver", "include.path",
        "lfs.customtransfer.payload.args", "lfs.customtransfer.payload.direction",
        "LFS.CUSTOMTRANSFER.Agent.ARGS", "lfs.Customtransfer.payload.direction",
        "lfs.basictransfersonly", "help.autocorrect", "lfs.standalonetransferagent", "lfs.https://example.invalid/repo.standalonetransferagent",
        "help.browser", "help.format", "instaweb.browser", "man.viewer",
        "includeif.gitdir:/x/.path", "pager.log", "interactive.difffilter", "web.browser",
        "imap.tunnel", "instaweb.httpd", "guitool.test.cmd", "browser.ff.cmd", "man.x.cmd", "init.templatedir", "hook.lint.command",
        "trailer.sign.command", "protocol.allow", "protocol.ext.allow",
        "lfs.customtransfer.x.path", "lfs.extension.x.clean",
        "CORE.HOOKSPATH", "Core.FsMonitor",
    };
    for (const char *k : runs) {
        if (!guard_key_runs_command(k, "cmd")) { fprintf(stderr, "%s should be watched\n", k); exit(1); }
    }
    CHECK(guard_key_runs_command("receive.denynonfastforwards", "false"));
    CHECK(guard_key_runs_command("receive.denynonfastforwards", NULL));
    const char *gates[] = {"core.usereplacerefs", "receive.denydeletecurrent", "receive.denydeletes", "difftool.prompt", "mergetool.prompt"};
    for (const char *key : gates) {
        CHECK(guard_key_runs_command(key, "false"));
        CHECK(guard_key_runs_command(key, "true"));
        CHECK(guard_key_runs_command(key, NULL));
    }
    CHECK(guard_key_runs_command("sendemail.confirm", "never"));
    CHECK(guard_key_runs_command("sendemail.work.confirm", NULL));
    const char *hidden_refs[] = {"uploadpack.hiderefs", "receive.hiderefs", "transfer.hiderefs"};
    for (const char *key : hidden_refs) {
        CHECK(guard_key_runs_command(key, "refs/heads/main"));
        CHECK(guard_key_runs_command(key, "!refs/heads/main"));
        CHECK(guard_key_runs_command(key, NULL));
    }
    CHECK(guard_key_runs_command("lfs.fetchinclude", "*"));
    CHECK(guard_key_runs_command("lfs.fetchexclude", NULL));
    CHECK(guard_key_runs_command("http.followredirects", "initial"));
    CHECK(guard_key_runs_command("http.https://example.invalid/repo.followredirects", "true"));
    CHECK(guard_key_runs_command("http.followredirects", NULL));
    const char *transaction_gates[] = {"am.threeway", "fetch.prune", "fetch.prunetags",
                                       "remote.origin.prune", "remote.origin.prunetags"};
    for (const char *key : transaction_gates) {
        CHECK(guard_key_runs_command(key, "true"));
        CHECK(guard_key_runs_command(key, "false"));
        CHECK(guard_key_runs_command(key, NULL));
    }
    CHECK(guard_key_runs_command("remote.origin.fetch", "+refs/heads/*:refs/remotes/origin/*"));
    CHECK(guard_key_runs_command("remote.origin.fetch", "^refs/heads/private/*"));
    CHECK(guard_key_runs_command("remote.origin.fetch", ""));
    CHECK(guard_key_runs_command("remote.origin.fetch", NULL));
    CHECK(guard_key_runs_command("remote.origin.tagopt", "--no-tags"));
    CHECK(guard_key_runs_command("remote.origin.tagopt", "--tags"));
    CHECK(guard_key_runs_command("remote.origin.tagopt", NULL));
    const char *activation_keys[] = {"hook.lint.event", "hook.pre-commit.enabled", "fetch.fsckobjects", "transfer.fsckobjects"};
    for (const char *key : activation_keys) {
        CHECK(guard_key_runs_command(key, "true"));
        CHECK(guard_key_runs_command(key, "false"));
        CHECK(guard_key_runs_command(key, NULL));
    }
    CHECK(guard_key_runs_command("receive.shallowupdate", "true"));
    CHECK(guard_key_runs_command("receive.shallowupdate", "false"));
    CHECK(guard_key_runs_command("receive.shallowupdate", NULL));
    CHECK(guard_key_runs_command("push.followtags", "true"));
    CHECK(guard_key_runs_command("push.followtags", "false"));
    CHECK(guard_key_runs_command("push.followtags", NULL));
    CHECK(guard_key_runs_command("push.pushoption", "ci.skip"));
    CHECK(guard_key_runs_command("push.pushoption", ""));
    CHECK(guard_key_runs_command("push.pushoption", NULL));
    CHECK(guard_key_runs_command("gc.packrefs", "false"));
    CHECK(guard_key_runs_command("gc.packrefs", "true"));
    CHECK(guard_key_runs_command("gc.packrefs", NULL));
    CHECK(guard_key_runs_command("attr.tree", "HEAD"));
    CHECK(guard_key_runs_command("attr.tree", NULL));
    CHECK(guard_key_runs_command("maintenance.pack-refs.enabled", "false"));
    CHECK(guard_key_runs_command("maintenance.pack-refs.enabled", "true"));
    CHECK(guard_key_runs_command("maintenance.pack-refs.schedule", "hourly"));
    CHECK(guard_key_runs_command("maintenance.pack-refs.schedule", NULL));
    const char *follow_keys[] = {"fetch.followremotehead", "remote.origin.followremotehead"};
    for (const char *key : follow_keys) {
        CHECK(guard_key_runs_command(key, "never"));
        CHECK(guard_key_runs_command(key, "always"));
        CHECK(guard_key_runs_command(key, NULL));
    }
    CHECK(guard_key_runs_command("push.useforceifincludes", "true"));
    CHECK(guard_key_runs_command("push.useforceifincludes", "false"));
    CHECK(guard_key_runs_command("push.useforceifincludes", NULL));
    CHECK(guard_key_runs_command("receive.certnonceseed", "test-seed"));
    CHECK(guard_key_runs_command("receive.certnonceseed", ""));
    CHECK(guard_key_runs_command("receive.certnonceseed", NULL));
    CHECK(guard_key_runs_command("receive.fsck.missingemail", "error"));
    CHECK(guard_key_runs_command("receive.fsck.missingemail", "ignore"));
    CHECK(guard_key_runs_command("fetch.fsck.skiplist", "path"));
    CHECK(guard_key_runs_command("fetch.fsck.skiplist", NULL));
    const char *advertisement_keys[] = {"receive.advertiseatomic", "receive.advertisepushoptions"};
    for (const char *key : advertisement_keys) {
        CHECK(guard_key_runs_command(key, "true"));
        CHECK(guard_key_runs_command(key, "false"));
        CHECK(guard_key_runs_command(key, NULL));
    }
    CHECK(guard_key_runs_command("receive.fsckobjects", "true"));
    CHECK(guard_key_runs_command("receive.fsckobjects", "false"));
    CHECK(guard_key_runs_command("receive.fsckobjects", NULL));
    CHECK(guard_key_runs_command("receive.maxinputsize", "1"));
    CHECK(guard_key_runs_command("receive.maxinputsize", "0"));
    CHECK(guard_key_runs_command("receive.maxinputsize", NULL));
    // All aliases can dispatch external commands or inject -c command-running settings.
    // Built-in submodule update modes can activate retained child filters and commands.
    CHECK(guard_key_runs_command("alias.x", "!rm -rf /"));
    CHECK(guard_key_runs_command("alias.co", "checkout"));
    CHECK(guard_key_runs_command("alias.x", "-c core.sshCommand=./payload ls-remote origin"));
    CHECK(guard_key_runs_command("alias.external", "custom-command"));
    CHECK(guard_key_runs_command("submodule.lib.update", "!sh evil"));
    const char *update_modes[] = {"none", "checkout", "rebase", "merge", ""};
    for (const char *mode : update_modes)
        CHECK(guard_key_runs_command("submodule.lib.update", mode));
    CHECK(guard_key_runs_command("submodule.lib.update", NULL));
    CHECK(!guard_key_runs_command("submodule.update", "checkout"));
    CHECK(!guard_key_runs_command("submodule.lib.updateextra", "checkout"));
    CHECK(guard_key_runs_command("alias.x", NULL));

    // Every endpoint or rewrite selector can activate an unchanged helper rewrite or SSH command.
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
        CHECK(guard_key_runs_command("remote.origin.url", url));
        CHECK(guard_key_runs_command("remote.origin.pushurl", url));
        CHECK(guard_key_runs_command("submodule.library.url", url));
        char key[256];
        snprintf(key, sizeof key, "url.%s.insteadof", url);
        CHECK(guard_key_runs_command(key, "ext::value-is-not-the-target"));
    }
    CHECK(!guard_key_runs_command("submodule.url", "ext::evil"));
    CHECK(guard_key_runs_command("submodule.library.url", NULL));
    CHECK(!guard_key_runs_command("remote.url", "ext::evil"));
    CHECK(guard_key_runs_command("remote.origin.url", NULL));

    const char *signature_sorts[] = {"signature:grade", "-signature:signer", "*signature:key",
                                     "version:signature:grade", "-v:*signature:grade"};
    for (const char *value : signature_sorts) {
        CHECK(guard_key_runs_command("branch.sort", value));
        CHECK(guard_key_runs_command("tag.sort", value));
    }
    const char *ordinary_sorts[] = {"refname", "-version:refname", "v:refname", "signaturex:grade",
                                   "Signature:grade", "--signature:grade", "version:-signature:grade", ""};
    for (const char *value : ordinary_sorts) {
        CHECK(!guard_key_runs_command("branch.sort", value));
        CHECK(!guard_key_runs_command("tag.sort", value));
    }
    CHECK(!guard_key_runs_command("branch.sort", NULL));
    CHECK(!guard_key_runs_command("branch.topic.sort", "signature:grade"));
    CHECK(!guard_key_runs_command("tag.topic.sort", "signature:grade"));

    // Keys that legitimately change during agent work, or only name things.
    const char *quiet[] = {
        "pull.custom.autostash", "merge.custom.autostash", "pull.autostashextra",
        "rebase.custom.autostash", "rebase.autostashextra",
        "checkout.custom.defaultremote", "checkout.defaultremoteextra", "checkout.custom.guess", "pull.custom.rebase", "branch.rebase", "branch.main.rebaseextra",
        "lfs.custom.gitprotocol", "lfs.gitprotocolextra", "pull.custom.ff", "lfs.https://example.invalid.url", "lfs.custom.pushurl",
        "remote.lfsurl", "remote.lfspushurl", "remote.origin.lfsurlextra",
        "lfs.url.fetchinclude", "lfs.url.fetchexclude", "lfs.fetchincludeextra", "lfs.fetchexcludeextra",
        "lfs.accessextra", "lfs.https://example.invalid/repo.accessextra", "merge.custom.defaulttoupstream", "merge.custom.ff", "merge.ffextra",
        "http.followredirectsextra", "http.https://example.invalid/repo.followredirectsextra",
        "http.proactiveauthextra", "http.https://example.invalid/repo.proactiveauthextra",
        "credential.https://example.invalid.interactive", "credential.interactiveextra",
        "push.custom.autosetupremote", "branch.merge", "branch.main.mergeextra",
        "push.custom.default", "remote.push", "remote.mirror", "remote.origin.pushextra", "remote.origin.mirrorextra",
        "remote.proxy", "http.proxyextra", "remote.origin.proxyextra",
        "http.sslcertextra", "http.https://example.invalid/repo.proxysslcertextra",
        "http.sslkey", "http.sslcainfo", "http.sslcerttype",
        "user.custom.signingkey", "user.signingkeyextra", "http.sslcertpasswordprotectedextra",
        "http.https://example.invalid/repo.proxysslcertpasswordprotectedextra",
        "lfs.args", "lfs.direction", "lfs.customtransfer.args", "lfs.customtransfer..args",
        "lfs.customtransferextra.payload.args", "lfs.customtransfer.team.agent.direction",
        "lfs.customtransfer.Team.Agent.args",
        "lfs.https://example.invalid/repo.args", "lfs.payload.direction",
        "lfs.custom.basictransfersonly", "help.custom.autocorrect", "help.autocorrectextra", "lfs.standalonetransferagentextra",
        "lfs.https://example.invalid/repo.standalonetransferagentextra",
        "receive.custom.autogc", "maintenance.custom.auto", "maintenance.custom.strategy", "maintenance.custom.repo",
        "gc.custom.auto", "gc.custom.autopacklimit", "maintenance.GC.enabled", "maintenance.Prefetch.enabled",
        "attr.custom.tree", "attr.treeextra", "maintenance.Pack-refs.enabled",
        "maintenance.team.pack-refs.schedule", "maintenance.pack-refs.enabledextra",
        "maintenance.GC.schedule", "maintenance.gc.scheduleextra", "fetch.custom.bundleuri", "maintenance.commit-graph.enabled", "maintenance.repack.enabled",
        "maintenance.team.prefetch.enabled", "maintenance.enabled",
        "remote.partialclonefilter", "extensions.custom.partialclone",
        "receive.custom.denynonfastforwards", "receive.denynonfastforwardsextra",
        "receive.custom.denydeletes", "receive.denydeletesextra",
        "uploadpack.custom.hiderefs", "receive.custom.hiderefs", "transfer.custom.hiderefs",
        "uploadpack.hiderefsextra", "receive.hiderefsextra", "transfer.hiderefsextra",
        "receive.custom.fsckobjects", "receive.custom.maxinputsize", "receive.fsckobjectsextra", "receive.maxinputsizeextra",
        "receive.custom.advertiseatomic", "receive.custom.advertisepushoptions", "receive.advertiseatomicextra", "receive.advertisepushoptionsextra",
        "receive.FSCK.missingemail", "fetch.Fsck.skiplist", "receive.fsck.extra.missingemail",
        "receive.fsck", "fetch.fsck.", "transfer.fsck.skiplist",
        "push.remote.useforceifincludes", "push.useforceifincludesextra",
        "receive.remote.certnonceseed", "receive.certnonceseedextra",
        "gc.custom.packrefs", "gc.packrefsextra", "fetch.custom.followremotehead",
        "fetch.followremoteheadextra", "remote.followremotehead", "remote.origin.followremoteheadextra",
        "push.remote.followtags", "push.followtagsextra", "push.remote.pushoption", "push.pushoptionextra",
        "receive.custom.shallowupdate", "receive.shallowupdateextra",
        "receive.custom.denydeletecurrent", "receive.denydeletecurrentextra",
        "core.custom.usereplacerefs", "core.usereplacerefsextra",
        "difftool.payload.prompt", "mergetool.payload.prompt", "difftool.promptextra", "mergetool.promptextra",
        "receive.custom.denycurrentbranch", "remote.promisor",
        "sendemail.confirmextra", "sendemail.work.confirmextra",
        "sendemail.work.identity", "sendemail.work.annotation", "sendemail.suppress", "fetch.custom.all", "remotes",
        "am.custom.threeway", "am.threewayextra", "fetch.custom.prune", "fetch.custom.prunetags",
        "remote.prune", "remote.prunetags", "remote.origin.prunetagsExtra",
        "remote.skipdefaultupdate", "remote.skipfetchall", "remote.origin.skipfetch",
        "remote.tagopt", "remote.origin.tagoptextra", "remote.fetch", "remote.origin.fetchextra", "user.email", "user.name", "remote.origin.name", "url.insteadof", "url.pushinsteadof",
        "pull.custom.twohead", "pull.custom.octopus", "receive.custom.procreceiverefs", "branch.remote", "branch.pushremote", "branch.mergeoptions", "remote.origin.pushdefault",
        "core.bare", "tar.command", "tar.remote", "uploadarchive.custom.allowunreachable",
        "gc.custom.recentobjectshook", "gc.pruneexpire", "core.editorx", "core.custom.attributesfile", "filter.clean", "diff.command", "merge.driver", "merge.recursive", "merge.custom.default", "merge.custom.renormalize", "core.x.hookspath",
        "fetch.custom.fsckobjects", "transfer.custom.fsckobjects", "fetch.fsckobjectsextra",
        "hook.event", "hook.enabled", "hook.lint.enabledextra",
        "include.x.path", "includeif.path", "hook.command", "remote.uploadpack", "merge.payload.tool", "diff.payload.guitool",
        "difftool.custom.guidefault", "mergetool.custom.guidefault",
        "push.custom.recursesubmodules", "submodule.library.recurse", "fetch.custom.recursesubmodules", "submodule.fetchrecursesubmodules",
        "format.custom.commitlistformat", "format.custom.coverletter", "rebase.custom.instructionformat", "diff.custom.ignoresubmodules", "diff.custom.submodule", "status.custom.submodulesummary", "submodule.ignore", "submodule.library.ignoreextra", "submodule.library.activeextra",
        "log.custom.showsignature", "merge.custom.verifysignatures", "format.custom.pretty",
        "help.custom.browser", "help.custom.format", "instaweb.custom.browser", "man.custom.viewer",
        "imap.host", "imap.custom.tunnel", "instaweb.port", "instaweb.custom.httpd", "guitool.cmd", "guitool.test.title",
        "commit.custom.gpgsign", "tag.custom.gpgsign", "tag.custom.forcesignannotated",
        "push.custom.gpgsign", "gpg.custom.format", "credential.username", "init.defaultbranch", "", ".", "core.", "nodot",
    };
    for (const char *k : quiet) {
        if (guard_key_runs_command(k, "!cmd")) { fprintf(stderr, "%s should not be watched\n", k); exit(1); }
    }
    puts("exec_guard_test: ok");
    return 0;
}
