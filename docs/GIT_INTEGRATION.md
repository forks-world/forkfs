# Git-aware Worlds

`world fs init` detects a Git repository at the source root. It imports an independent
repository into the snapshot; each subsequent `world fs fork` automatically creates a
native linked worktree on a new `world/W<n>` branch. A conflicting existing branch is
preserved and the first free numeric suffix is chosen, however many generated names
already exist (`world-W<n>` if `world` itself is a branch). Ordinary directories need no Git installation.
Git repositories require Git 2.48 or newer on PATH (relative worktree support).

```sh
world fs init ~/src/project
world fs fork --from S1 --to ~/worlds/task
world fs inspect W1 --json       # git.branch, git.head, git.baseline, git.git_dir
world exec W1 -- git status
world exec W1 -- git commit -am 'task change'
world fs checkpoint W1           # a filesystem snapshot, never an implicit Git commit
```

## Owned administration and isolation

Each tree owns `.world-git/repo.git`, a private bare repository with one linked worktree.
Its root `.git` file and the reverse worktree pointer are relative. The branch, HEAD,
index, refs and object database belong to that tree, not the original source or another
World. Initial import builds the owned repository the way `git clone --mirror` would --
the same bare layout, packed `refs/*`, HEAD and symbolic-ref edges -- but does not
transfer objects through Git: the source's common object directory is cloned with the
filesystem's copy-on-write primitive (see [Import cost](#import-cost)) and the captured refs
are written over it in one `update-ref` transaction. Subsequent filesystem forks use APFS
cloning, including the Git objects. Git's
[worktree documentation](https://git-scm.com/docs/git-worktree) describes these per-worktree
administrative links. The owned repository also sets `worktree.useRelativePaths=true`, so a
linked worktree created in a World later -- by an AI coding agent, typically -- gets relative
links as well and keeps working when the World is moved, trashed and restored (see
[Linked worktrees of AI coding agents](#linked-worktrees-of-ai-coding-agents)). Worlds
imported before this setting existed do not have it and are forked as they are: a fork is a
copy of its parent's configuration, and the setting only decides how Git spells the links of
worktrees created afterwards (`git config worktree.useRelativePaths true` adds it by hand).

Git refs whose symbolic targets are outside `refs/`, malformed, dangling or cyclic are
unsupported by this import contract and may be omitted by Git's mirror operation. The
source is rechecked for the captured symbolic-ref map before publication.

This deliberately does not put a shared writable repository in the store: the existing
exec sandbox can continue denying writes to the entire store and every other World.
Moving, trashing, restoring, checkpointing or collecting a World moves or removes its Git
administration with the same tree. No separate registry needs a best-effort cleanup and no
external worktree becomes a GC target. Source worktree registrations are not imported, and
neither are the checkouts of the source's other linked worktrees that lie inside its tree
(see [Linked worktrees of AI coding agents](#linked-worktrees-of-ai-coding-agents)).
Deleting the original source after a successful import does not remove Git objects needed
by the World, including staged blobs which have not yet appeared in any commit.

### Linked worktrees of AI coding agents

AI coding agents work in extra linked worktrees of the repository. Claude Code creates
`<repository root>/.claude/worktrees/<name>` on a `worktree-<name>` branch -- inside the tree,
possibly nested in another such worktree, locked with `git worktree lock` until its cleanup, and
ignored only when the `**/.claude/worktrees/` rule it writes to `info/exclude` (or the user's own
`.gitignore`) is present. Codex creates a detached worktree outside the repository and writes
`core.worktree` into its `config.worktree`. A linked worktree other than the World's own checkout
is a separate checkout of the repository, not part of the World, which is the same rule as for
source worktree registrations above: work committed on a branch travels (every ref of the
repository is imported, so the branch and its commits are in the World), its checkout,
uncommitted state and HEAD do not, and the source or parent is never modified. A detached
worktree's HEAD -- every Codex worktree is detached -- is per-worktree state, so commits reachable
only from it are not referenced in the World (put them on a branch first to carry them). Each one left out is
named in a `note:` line on stderr.

- `init` admits a `.git` file inside the source tree only when it is exactly a registered
  worktree of the source's own repository: its `gitdir` resolves to `<common dir>/worktrees/<id>`
  of the common directory being imported, that registration's `gitdir` resolves back to this
  `.git` file, and its `commondir` back to the same common directory. Such a checkout is not
  walked (a worktree nested in it goes with it), is excluded from the cleanliness check by exact
  path -- `git status` lists one that is not ignored as an untracked directory -- and is left
  out of the snapshot's copy, whatever `--include-changes` or `--committed-only` say. The
  pre-clone scan (entry count, space budget, hardlink groups) never enters it, and neither does
  a copy that walks the tree (Linux, or `fork --copy` across volumes); APFS clones the whole root
  in one `clonefile(2)` and the checkout is removed from that copy before publication. (That one
  clone fails with EACCES on an unreadable directory anywhere in the tree, an agent's checkout
  included, as it does for an unreadable file of the tree itself.) A name in the checkout is a
  name leaving the tree, as a replaced `.git`'s is: it is no hardlink group's member, and the
  tree's own names that shared its inode keep their own group, or become plain files, rather
  than counting as linked from outside. A `.git` file leading into another repository,
  one whose registration does not lead back to it and a dangling one are still refused as a
  nested repository; a `.git` directory is a nested repository, admitted only when it is
  self-contained (see [Nested repositories](#nested-repositories)). The source's registrations are rechecked after the copy
  and must be exactly the captured ones: a worktree added inside the source while it was being
  cloned, or one removed meanwhile (whose directory may already hold ordinary files again), fails
  the import as busy, to be retried, rather than publishing a copy that lacks those files.
- `fork` and `checkpoint` of a World drop every `worktrees/<id>` registration other than
  `active` from the child copy -- with whatever Git or the agent keeps there (`locked`,
  `CLAUDE_BASE`, `codex-thread.json`, `config.worktree`, the index, logs) -- together with the
  checkout when it lies inside the World. Whether it does is read from the registration's
  `gitdir`: a relative one is resolved from the World's own administration, an absolute one
  names the parent's path and is resolved there. The child's `git worktree list` shows only its
  own checkout, and a branch that was checked out in a dropped worktree is an ordinary branch
  in the child. The parent World and any directory outside it are never touched. A registration
  that cannot be read (a symlink, a special file, a missing or garbled `gitdir`), or whose
  checkout is inside the World but whose `.git` does not lead back to it, is refused rather than
  guessed at; so is a checkout in a submodule's path.
- `discard` moves an in-tree worktree to the trash with the World, registration and all, and
  `restore` brings both back. A worktree outside the World would be left with a `.git` pointing
  into a discarded tree, so it still blocks discard, `--force` included; the refusal names each
  such checkout and the command that removes it, `git -C <World> worktree remove <path>` (or
  `git worktree prune` for one whose directory no longer exists).
- `publish` sends the World's own branch as before; the other worktrees do not block it, and an
  in-tree one does not count as an uncommitted change of the World.

Only the root repository's linked worktrees are handled this way. A linked worktree of a
submodule repository is still refused by fork, checkpoint and discard, and an in-tree `.git`
file belonging to a submodule's repository is still refused by `init`.

`.world` and `.world-git/` are excluded from Git status using the private repository's
`info/exclude`; source-local `info/exclude` rules are preserved before those reserved entries
are appended. Source-local `info/attributes` rules are also preserved. These are reserved
administration names. Source hooks and local executable
Git settings are not imported by default (see [Project hooks](#project-hooks)). Local `user.name` and `user.email` are preserved, and so is an
identity a conditional include supplies at the source's location; normal Git commands in a
World also use the user's usual Git configuration. Import does not create a
remote back to the source.

The source's repository-local remotes travel into the World: remote URLs, push URLs,
fetch and push refspecs, tag and prune options, remote groups, branch upstreams
(`branch.<name>.remote`/`merge`/`pushRemote`/`rebase`), `url.<base>.insteadOf` rewrites,
`remote.pushDefault`, `push.default`, `push.autoSetupRemote`, `fetch.prune` and aliases.
`git fetch origin` and `git push origin <branch>` therefore work in a World as in the
source; nothing is fetched or pushed automatically, and the World's own `world/W<n>` branch
starts without an upstream. Any carried configuration for that exact branch name -- for
example a stale `branch.world/W1.remote`/`merge` left behind by an earlier branch of that
name, or configuration copied wholesale from a World this one was itself forked from -- is
removed when the branch is created, so it cannot resurrect an upstream the World never had.
A candidate name that ordinary Git would still read `branch.<name>.*` settings for from
global or system configuration -- configuration this import cannot remove -- is skipped in
favor of the next free suffix, the same way a colliding ref is.
A relative local remote path is made absolute against the source, so it keeps reaching the
same repository after the source is deleted. When the path exists, it is resolved through the
filesystem the same way Git itself would reach it -- including through a symlink a `..`
component in the path walks back out of -- rather than collapsed lexically, which a symlink
could make point somewhere else. When the path does not exist in full (the remote was never
fetched into the source, or names a path only `git push` would create), its longest existing
prefix is resolved through the filesystem the same way, and whatever is still missing is
joined onto that real path -- so a symlink earlier in the path (e.g. `link/new.git` with `link`
a symlink elsewhere) still lands where Git would put it once the missing part exists. The
missing part is refused instead of guessed, and the remote URL must be made absolute first,
when it contains a `..` component (there is no filesystem left to resolve it through) or when
its first missing component is itself a dangling symlink (one whose own target does not exist,
so where Git would actually follow it cannot be told from here).
A remote `url` or `pushurl` containing a line break (`\n` or `\r`) is refused outright, even
though such a value is legal Git configuration for a local path: `git remote get-url` emits a
remote's URLs newline-separated, so a URL that itself contains one could no longer be told apart
from two separate URLs once carried; rename the path before importing.
A relative remote URL that any `url.<base>.insteadOf` or `pushInsteadOf` rule matches is refused instead (make the
URL absolute or remove the rule), because a rewrite of a relative path cannot be carried
faithfully to a World in another location. When such a rule instead rewrites an absolute
remote URL -- for example a per-account rule reached through a conditional include such as
`includeIf "gitdir:~/work/"` -- the World does not carry the rewrite rule's effect raw:
it records the fetch and push URLs Git actually uses for that remote at the source, so the
World reaches the same endpoints wherever it is placed. Whichever URL a remote ends up carrying
-- a pinned one, or (when no rewrite applies to that remote) its own raw, absolute URL, carried
as-is -- is refused if any collected `insteadOf`/`pushInsteadOf` rule could still rewrite it,
including one that lives in a conditional include inactive at the source: such a rule could
become active once the World moves elsewhere and redirect a URL the source itself never
rewrote, or redirect a pinned URL to somewhere other than what the source actually resolved.
Simplify the rewrite rules before importing. A conditional rule that only applies at the
World's own location applies there, exactly as it would for any repository placed there. A
remote that had an explicit `remote.<name>.pushurl` at the source keeps an explicit pinned
pushurl in the World even when it resolves to the same URL as the pinned fetch URL, since Git
never falls back to a remote's (possibly rewritten) fetch URL once it has an explicit pushurl,
and leaving it implicit would expose it to a `pushInsteadOf` rule active at the World's own
location. A remote that has only a `pushurl` and no `url` at all -- which Git supports -- is
pinned the same way, from its effective push URL alone. A remote is one unit: if any of its
repository-local settings are carried -- not only `url`/`pushurl`, but also `fetch`, `push`,
`tagopt`, `prune`, and the other subsectioned `remote.<name>.*` settings this section
carries -- and that same remote's `url` or `pushurl` is also set in global, system, or command
(`GIT_CONFIG_*`/`-c`) configuration, the import is refused, even if the remote has no
repository-local `url`/`pushurl` of its own and its only URL is the shared one: the World
reads that same ambient configuration, so pinning the ambient value on top would duplicate the
URL, could contact a push URL twice, or (via the rewrite pass above) send the World to a
different endpoint than the source actually reaches. Keep all of a carried remote's settings,
including its URLs, in the repository-local configuration only.
Settings Git
runs on its own are not carried: hooks and `core.hooksPath` (unless `--with-hooks`), `remote.<name>.uploadpack`,
`receivepack` and `vcs`, `branch.<name>.mergeOptions`, `core.sshCommand` and credential
helpers (a global credential helper still applies). These settings are rechecked before
publication like the rest of the captured state.

## Project hooks

Hooks are not carried by default, so commits in a World skip the project's hooks. When the
source has any -- executable, non-`.sample` files (or symlinks) in its hooks directory, or a
repository-local `core.hooksPath` such as husky's `.husky` -- `init` says so in one `note:`
line on stderr that names `--with-hooks`.

`world fs init <dir> --with-hooks` carries them into the owned repository:

- the executable, regular, non-`.sample` files of the source's hooks directory (the common
  one, also for a linked worktree) are copied into `.world-git/repo.git/hooks` with their
  modes; files Git would not run (not executable), subdirectories and special files are not;
- a repository-local `core.hooksPath` is carried as the source resolved it. A relative value
  that stays inside the tree refers to the World's own copy (hooks run at the worktree root),
  so `.husky` works as-is; one that leaves the tree (`../shared-hooks`) is carried as the
  absolute directory the source used, never re-resolved beside the World. An absolute value is
  kept as the user opted into it. A relative value with a `..` after a directory name
  (`link/../hooks`) is refused: Git resolves it through that directory, possibly a symlink,
  so it cannot be collapsed faithfully. While `core.hooksPath` is set, Git ignores the default hooks
  directory, so it is neither scanned nor copied; an in-tree hooks directory travels with the
  tree and is refused if it, a component of its path or any entry in it -- at any depth, since
  hooks commonly source nested helpers -- is a symlink (for a hooks path of `.`, only the
  hook-named files at the root count). With
  `--committed-only` the in-tree hooks directory must be committed with no pending changes
  inside it (for a hooks path of `.`, the hook-named files at the root), and none of those
  hooks may be marked skip-worktree or assume-unchanged, which hides edits from status: the reset to HEAD
  would otherwise remove the directory or its uncommitted hooks and leave the World silently
  skipping them. A hooks path inside `.git`, `.world-git` or `.world` is refused, since the
  import replaces that administration. A global `core.hooksPath` needs no carrying: global
  configuration is shared.

A symlinked hook, or a symlinked hooks directory, is refused with a reason rather than
followed: the World would otherwise run whatever the link reaches later. The captured hooks
and `core.hooksPath` are rechecked before publication like the rest of the captured state,
so a hook changed during the import aborts it. Hooks never run during WorldFS's own Git
commands (import, fork, checkpoint, publish): every one of them sets `core.hooksPath=/dev/null`.
Worlds forked or checkpointed from a World keep its hooks, since its whole `.world-git`
(hooks directory and configuration included) is cloned with it. So `--committed-only` from a
World requires the same of a relative `core.hooksPath` set in it -- committed, nothing pending
inside -- even without `--with-hooks`.

### Running agents with `world exec`

A World's Git administration, `.world-git/repo.git`, lives inside the World, and `world exec`
lets the command write the World. Without more, an agent could plant a hook, `core.hooksPath`,
`core.fsmonitor`, a filter, `core.sshCommand` or a credential helper there that runs later,
**outside any sandbox**, the first time you run Git in the World yourself. (Codex's own sandbox
has the same gap for this layout: [openai/codex#49303](https://github.com/openai/codex/issues/49303).)
So `world exec` does two things.

**Denied inside a sandboxed exec** (writes only; Git still reads and runs the hooks there):

- `.world-git/repo.git/hooks` and everything below it, the directory entry included, so it
  cannot be replaced, removed or renamed away;
- the `hooks` directory of every submodule repository under
  `.world-git/repo.git/worktrees/active/modules/` (nested ones too). On macOS this is a
  seatbelt regex, so a submodule that `git submodule update --init` clones *during* the exec
  is covered as well: its clone may create `hooks` (as a directory, not a symlink) and Git's
  `*.sample` templates in it, which Git never runs, and nothing else (seatbelt cannot tell
  that `mkdir` from renaming a prepared directory into place, so a *new* repository's hooks
  directory can still arrive that way; the report below lists what it contains). Existing
  `.gitmodules` declarations are read with a bounded, confined `git config --no-includes`
  query before sandboxed exec on macOS, so slashed names containing `hooks` can initialize.
  Every parsed declaration record counts toward the shared 65536-entry capture limit.
  Names must be relative with no empty, `.` or `..` components; overlapping hook namespaces
  are refused. Namespace prefixes immediately before a `hooks` component must have neither
  `HEAD` nor `refs`, and those markers remain denied so that prefix cannot become a repository
  or a linked-worktree common directory. Ordinary names such as `refs` and `HEAD` stay valid
  when they do not conflict with a required reservation. Exec creates missing locator
  directories for hook-name exceptions under its lock before sandboxing,
  then pins them without a create exception; empty namespaces can remain after a failed exec.
  Planned repository roots permit directory creation but reject symlinks, subject to the
  same directory-create/rename limitation above. Newly introduced names
  containing `hooks` remain denied until the next exec;
- the directory each repository's effective `core.hooksPath` names (the World's and every
  submodule's, a relative value resolved from that repository's checkout as Git does), when
  that is outside the tree (a global `~/.githooks`, a shared directory) or inside
  `.world-git`; its hooks are also included in the report below. This protection applies
  even inside a submodule whose administration path contains a `hooks` name component;
- what tells Git where those repositories are: the World's `.git` file,
  `.world-git/repo.git/worktrees/active/commondir`, and the directory entries on the way to
  each repository (`.world-git`, `repo.git`, `worktrees`, `active`, `modules`, each existing
  submodule repository and the directories of a slashed submodule name), which cannot be
  renamed or removed. Operations that move them (`git worktree repair`, `git submodule
  absorbgitdirs`) fail inside the exec; run them outside.

On macOS, every protected hook directory also pins its ancestor directory entries, including
ancestors outside the World. Raw traversal components discarded by `..` are pinned too, so
renaming an ancestor or replacing a traversed directory with a symlink cannot expose the hooks
through another path. These are entry restrictions, so sibling files remain writable. Missing
ancestors have no directory-creation exception; create the intended directory outside exec.
Default-hook ancestor pins apply only to existing repositories and preserve planned submodule
initialization described above.

A tool that installs hooks (`git lfs install`, `pre-commit install`, `husky` writing to the
default hooks directory) fails inside a sandboxed exec for the same reason; run it outside.
A repository-local `core.hooksPath` pointing into the tree (husky's `.husky`) is **not**
denied: project hooks remain editable. Their changes are reported even when ignored or
untracked, since Git status and the review diff may hide them. Git's configuration (`config`, `config.worktree`) is
not denied either: `git remote add`, `git push -u`, `git branch --set-upstream-to` and
`git config` legitimately write it. Hence the second part.

**Reported after every exec** (sandboxed or `--no-sandbox`, whatever the command's exit
status, which `world exec` keeps). Before the command starts, exec captures each repository
-- the World and every submodule repository present -- whole, and the same capture decides
the sandbox rules above:

Before sandboxed exec, the fixed administration locators, including `repo.git/worktrees`,
must be real directories rather than symlinks. The World's `.git` and active worktree `commondir` must be regular,
complete pointers to its own administration, each with exactly one hardlink. Writable aliases
to a pointer would otherwise evade pathname protection. Existing `.git` entries in other effective
checkouts, and existing submodule `commondir` pointers, must likewise name the administration
being guarded. Missing optional checkout pointers are allowed (for example a redirected
`core.worktree` without `.git`). Pointer parsing reads the complete bounded file, rejects NUL
or oversized content, trims only trailing CR/LF and compares canonical targets. Preexisting
redirected, symlinked or directory pointers refuse sandboxed exec; `--no-sandbox` retains
observational reporting so a command can repair them.

- its effective configuration, `git config --list --includes --show-scope` read through its
  own administration: system, global, local, worktree, inherited command scope and included files -- the sandbox does
  not stop writes to `~/.gitconfig`, and `--no-sandbox` stops nothing. Every scope is kept
  per repository, since an `includeIf "gitdir:..."` or `"onbranch:..."` can give one
  submodule settings the World does not have; a change to a global or system entry that is
  identical in the World's own listing is reported once, under the World;
- each repository's `config.worktree`, even while `extensions.worktreeConfig` is disabled,
  including its dormant include graph. Missing files are recorded; later creation, removal
  and content edits are reported under the same regular-file and resource limits. Absent
  worktree files do not consume the 256-target include budget; present ones do;
- every include target declared by effective configuration, including inactive conditional
  includes and their nested targets. Git expands paths; relative targets retain the including
  file's origin directory. Missing files are recorded, so creating a dormant payload is reported.
  Existing targets must be regular files; symlinks, special files and unsupported origins make
  capture unavailable. Content fingerprints also report benign changes in these files; conditions
  do not have to become active during exec. Traversal is limited to 32 levels, 256 targets and
  256 origin spellings, shares the 64 MiB content/65536-entry budgets, and has five seconds of
  aggregate confined query time per capture. These files remain writable;
- each repository's private `info/attributes` and effective user/system attributes files:
  changes can activate an unchanged filter
  definition without altering tracked project files. Regular files are hashed in full within
  the shared capture budget, including edits through hardlink aliases. Symlinked or other
  nonregular attribute files invalidate capture rather than silently omitting their content.
  Git resolves `core.attributesFile`, its XDG default and the system attributes path per
  repository; `GIT_ATTR_NOSYSTEM` is honored and an effective `/dev/null` source is disabled.
  Shared paths are captured and reported once;
- the hooks Git could run: name, mode and content of every non-`.sample` entry of its hooks
  directory and of every effective `core.hooksPath`, including paths inside the project tree.
  Shared hook directories are captured once;
- the entries that tell Git where it is -- the World's `.git` and `commondir`, each
  submodule checkout's `.git` -- by entry type (file, directory, symlink, missing) and
  content, and whether a file still names its own repository.

It captures them again after the command and prints one line per difference to stderr.
Configuration comparison preserves repository-label/key identity boundaries and each value boundary and distinguishes an implicit boolean
from an explicitly empty value. Reports show these as `(implicit)` and `""`; control bytes and
literal backslashes are escaped rather than interpreted as value boundaries or terminal controls. A
`.git` replaced by another type (`git init` over it, a symlink) or rewritten is reported as a
change, a removed one (`submodule deinit`, `git rm`) as a removal; only a submodule checkout's
`.git` that appears naming its own repository (`submodule update --init`) is not reported:

```
world: WARNING: exec changed a Git setting that runs commands: local core.fsmonitor: (unset) -> touch /tmp/x
world: WARNING: exec changed a Git setting that runs commands: submodule lib-module local credential.helper: (unset) -> store
world: WARNING: exec added a Git hook: .world-git/repo.git/worktrees/active/modules/vendor/lib/hooks/post-checkout
world: WARNING: exec changed Git repository attributes: .world-git/repo.git/info/attributes (can activate configured filters)
world: WARNING: exec changed where Git finds a repository: .git (a file naming its repository -> a directory)
world: WARNING: exec removed libs/lib/.git, which told Git where a repository is
```

The settings watched (Git's lowercase key names; `*` is any subsection): `core.hookspath`,
`core.worktree` (checkout and relative-hook redirection), `core.fsmonitor`, `core.sshcommand`, `core.editor`, `core.pager`, `core.askpass`,
`core.attributesfile` (can activate configured filters), `core.gitproxy`, `core.alternaterefscommand`, `sequence.editor`, `credential.helper` and
`credential.*.helper`, `filter.*.clean|smudge|process`, `diff.external`,
`diff.*.command|textconv`, `diff.tool|guitool`, `merge.tool|guitool`, `difftool.guidefault`, `mergetool.guidefault` (select configured commands), `merge.*.driver|recursive`, `merge.default` (including selection of unchanged merge drivers),
`merge.renormalize` (can activate configured conversion filters), `mergetool.*.cmd|path`, `difftool.*.cmd|path`,
`commit.gpgsign`, `tag.gpgsign`, `tag.forcesignannotated`, `push.gpgsign` (enable signing),
`log.showsignature`, `merge.verifysignatures`, `rebase.instructionformat`, `format.pretty`,
`pretty.*`, `format.commitlistformat`, `format.coverletter` (can activate signature verification),
`branch.sort` and `tag.sort` when selecting a `signature` atom (ordinary name/version sorts stay quiet),
`user.signingkey` (removal can activate the configured SSH default-key command),
`http[.*].sslcert|proxysslcert|sslcertpasswordprotected|proxysslcertpasswordprotected` (can activate certificate password helpers),
`gpg.format` (selects the signing program), `gpg.program` and `gpg.*.program`, `gpg[.*].defaultkeycommand`, `gc.recentobjectshook`,
`remote.*.skipdefaultupdate|skipfetchall`, `fetch.bundleuri`, `fetch.all`, `remotes.*` (can activate unchanged remote helpers),
`remote.*.uploadpack|receivepack|vcs`, `branch.*.mergeoptions`, `pull.twohead`, `pull.octopus` (can select external merge strategies),
`branch.*.remote|pushremote`, `remote.pushdefault` (can select preconfigured helper remotes),
`remote.*.promisor|partialclonefilter`, `extensions.partialclone`
(can activate a configured helper when fetching missing objects),
`uploadpack.packobjectshook`, `receive.procreceiverefs` (activates the configured proc-receive hook),
`receive.denycurrentbranch` (can enable the existing push-to-checkout hook),
`receive.autogc`, `maintenance.auto`, `gc.auto`, `gc.autopacklimit`, `maintenance.strategy`,
`maintenance.repo` (registers repositories for an existing maintenance scheduler),
`maintenance.gc.enabled|schedule`, `maintenance.prefetch.enabled|schedule`
(can activate existing maintenance hooks or remote helpers; task names are exact),
`sendemail.identity` (selects configured mail commands),
`sendemail[.*].annotate|suppresscc|validate|useimaponly|imapsentfolder` (activate configured editors, mail commands or hooks),
`sendemail[.*].tocmd|cccmd|headercmd|sendmailcmd|smtpserver`, `include.path`,
`submodule.active`, `submodule.*.active`, `submodule.*.url` (can activate configured update commands),
`submodule.recurse`, `fetch.recursesubmodules`, `push.recursesubmodules`,
`submodule.*.fetchrecursesubmodules` (can activate child fetch/push helpers and hooks),
`submodule.*.ignore`, `diff.ignoresubmodules`, `diff.submodule`, `status.submodulesummary`
(can activate child status, diff or log commands and their configured programs),
`includeif.*.path`, `alias.*` (including ordinary aliases that dispatch commands or inject `-c` settings), `submodule.*.update` whose value
starts with `!`, `pager.*`, `interactive.difffilter`, `web.browser`, `help.autocorrect` (can dispatch corrected external Git commands), `help.browser`, `help.format`, `instaweb.browser`, `man.viewer` (select configured viewers), `browser.*.cmd|path`, `instaweb.httpd`, `guitool.*.cmd`, `imap.tunnel`,
`man.*.cmd|path`, `init.templatedir`, `hook.*.command`, `trailer.*.command|cmd`, `tar.*.command|remote`, `uploadarchive.allowunreachable`
(including activation of an unchanged archive command),
`protocol.allow` and `protocol.*.allow` (which can enable `ext::` URLs), and
`lfs.*.path|clean|smudge` (custom transfer agents and extensions), plus
`lfs[.*].standalonetransferagent` (selects an existing custom transfer command, including URL-scoped settings),
`lfs.customtransfer.<name>.args|direction` (change arguments or enable upload/download adapters),
and `lfs.basictransfersonly` (can restore custom transfer adapters). Remote `url`/`pushurl`
and `url.<target>.insteadOf`/`pushInsteadOf` changes are always reported. SSH endpoints can
activate an unchanged `core.sshCommand`; an existing rewrite can map an ordinary URL or local
path to a helper transport. Restricting reports by the newly written URL's scheme would miss
these activations. Submodule URLs are likewise always reported: adding even an ordinary URL
can activate an unchanged custom update command when no active selector overrides it.
The key list and value-sensitive rules are in `cli/exec_guard.h`. Branch remote selectors and
`remote.pushdefault` are reported even when choosing an ordinary remote: a remote name can
activate unchanged helper configuration. Setting endpoints and tracking remains allowed;
fetch refspecs, descriptive names and `branch.*.merge` stay quiet.
Each repository is read before and after with five
queries (plus the include graph and macOS pre-exec declaration queries described above): a configuration listing, `--path --get core.hooksPath`, `rev-parse --show-toplevel`,
and `git var GIT_ATTR_GLOBAL`/`GIT_ATTR_SYSTEM`,
so Git expands `~user` and applies its checkout rules when resolving relative hooks. Missing hook directories resolve through existing ancestors;
unresolvable ancestors and `..` in a missing suffix invalidate capture. Mutable symlink components
in effective hook paths are unsupported, even inside the project: an alias could be retargeted
and restored during exec. Only the exact root-owned macOS `/tmp`, `/var` and `/etc` system
aliases to `/private` counterparts are accepted, with a protected root-owned parent. The queries use the absolute Git 2.48+ path
selected by CMake at build time, unaffected by runtime `PATH`, and do not execute hooks or
fsmonitor. Configuration introspection runs under a separate read-only sandbox: Seatbelt on
macOS, or Bubblewrap with network and process isolation on Linux. It can write only `/dev/null` (needed by Git startup) and cannot
access the network; account lookup on macOS is allowed for `~user` expansion. Each query has
a five-second deadline and a 64 MiB output limit; no tree is walked. If configuration cannot
be read (missing configured Git or sandbox, a malformed file, or a query exceeding these
limits), sandboxed exec refuses before running the command; `--no-sandbox` says so in a
`note:` line and runs without the report;
a World without `.world-git` at startup gets neither the rules nor the report. If the command
removes or replaces previously detected Git administration, or makes its scan incomplete,
exec warns that the final hooks and settings could not be inspected. This includes missing,
renamed or replaced active worktree administration. An interrupted scan
is identified as cancellation rather than a replacement. An incomplete administration scan (read/allocation error,
path truncation, depth over 32, more than 4096 repositories or 65536 directory entries) refuses
sandboxed exec; `--no-sandbox` reports the unavailable guard and preserves the command status.
Hook, pointer and repository-attributes hashing has a 64 MiB aggregate content budget per capture; oversized files,
read errors and record/entry limits invalidate the capture explicitly. Incomplete initial
hook captures also refuse sandboxed exec. Any noninterrupted final capture failure emits a
`WARNING` that changed hooks and settings could not be inspected, while keeping the command status. Small hooks are hashed
in full, so same-size, same-mtime rewrites remain detectable. Symlinked default hooks directories and symlinked entries in guarded
hooks directories are unsupported: their executable target could change outside the directory
policy. They invalidate capture, refusing sandboxed exec and producing an explicit note with
`--no-sandbox`; symlinked Git administration pointers remain recorded by link target.
Regular hook files with multiple hardlinks are likewise unsupported, since a writable alias
could change their bytes outside the guarded directory. The check applies to hook files, not
directory link counts. Administration pointers also require a single link for sandboxed exec,
but their contents remain observable with `--no-sandbox`. Signals received during post-command
inspection cancel the active query, reap its child, release the exec lock and return
`128 + signal`; signals while the requested command runs continue to be forwarded to it.

The exec sandbox is not a general confinement: it keeps the command away from the store and
from other Worlds, and from the hooks above. `~/.gitconfig`, `~/.ssh`, shell startup files and
everything else outside the World stay writable on macOS (`(allow default)`), which is why the
report covers effective configuration. On Linux the host filesystem is read-only in the
sandbox, but hooks are protected with read-only bind mounts of the directories that exist
when the exec starts, and the directories on the way to them are bound onto themselves so they
cannot be renamed. Duplicate paths keep the read-only policy, and descendant locator binds
never reopen a read-only hooks path; a submodule first initialized during the exec gets writable hooks there,
which the report then lists. What neither platform stops, and the report does not cover: a
change to other project content that runs code (a `Makefile`, `package.json` scripts,
`.envrc`) -- review the World's diff before running it. Effective Git attributes files and
hook directories are exceptions: their contents are reported even when the configured path
is inside the World, including ignored files.
This does not scan the project tree for `.gitattributes` files.

## Git LFS

Repositories that use the stock Git LFS filter are supported at the root and in initialized
submodules. The tracked checkout may contain hydrated files or pointer files. External import
preserves the worktree bytes and index as captured, and copies the repository's local
`.git/lfs/objects` cache into the World's owned Git administration. Each payload's SHA-256,
fan-out path and recorded size are checked before publication, and the source cache is rechecked
afterward. To determine tracked-file status, import uses only Git LFS's canonical clean/process
filters with an isolated temporary `lfs.storage`; smudge/process are set to skip, so this check
does not download content or write to the source cache. The temporary storage is discarded.
A global LFS filter that no tracked file uses remains harmless. Pointer hashing detects whether the
installed Git LFS help advertises `--no-extensions`; older releases without that option use their
legacy raw SHA-256 pointer command. Unrecognized or failed help is refused rather than retried with
an unsafe command. Git LFS 3.8.0 is the tested version.

The owned cache is independent of the source. After import, deleting or moving the source and
its cache does not affect offline work. Forks and checkpoints carry each root and submodule cache
with their owned Git administration. `--committed-only` resets to HEAD with LFS smudging skipped
while the stock process filter still recognizes pointers, then runs `git lfs checkout` against
the local cache: available payloads are hydrated, while an
uncached pointer stays a pointer. Ordinary import and `--include-changes` retain the original
worktree and staged bytes.

Only the stock `git-lfs` clean, smudge and process commands are admitted when a tracked path uses
`filter=lfs`; unused custom or incomplete filters remain inert. Carried repository LFS extensions,
custom transfer agents, non-local storage, and relative endpoints are refused even if the current
checkout has no LFS paths or cache, because later checkouts could use them. Dormant settings from
global or system configuration are not carried or treated as repository policy. Absolute LFS
endpoint URLs are carried. A complete stock LFS filter setup from repository, global, or system
configuration is retained when only another preserved branch uses LFS. Accepted canonical
`smudge --skip` and `filter-process --skip` variants are retained in
the World's owned local filter configuration. An ordinary import installs and pins its generated pre-push hook in the World's own Git
administration, independent of ambient `core.hooksPath`; under `--with-hooks`, a carried in-tree
`core.hooksPath` must already contain an executable canonical Git LFS pre-push hook, so import
does not add untracked hook files. An existing pre-push hook is carried only when it is Git LFS's
canonical script (other supported hooks retain their normal handling). An ordinary `git push` then
transfers payloads to its remote. `publish` validates Git LFS pointers and `.lfsconfig` in every
commit reachable from the branch being published, including history already present in the
destination. Each required payload must be valid in the destination cache or available in the
World's local cache for copying. Publishing fails if a required historical payload is missing or
corrupt, or a historical `.lfsconfig` uses an unsupported endpoint. This can reject an otherwise
unrelated publish until missing legacy payloads are restored; publishing does not download
missing payloads.

## Submodules

An initialized submodule -- a directory the index records as a gitlink that holds a `.git`,
at any depth up to 8 levels of nesting -- is imported with the root. Its repository is copied
the way the root's is (its object directory cloned and its refs written, then its index, symbolic refs, local
rules, rerere cache, status settings, identity, carried remotes and submodule settings,
`SQUASH_MSG`, `FETCH_HEAD`, `ORIG_HEAD`, its stash stack and, with `--with-hooks`, its hooks) into the place
Git itself uses for a submodule of a linked worktree: `modules/<name>` of the superproject's
Git directory, which for the root is the World's per-worktree
`.world-git/repo.git/worktrees/active`, and `modules/<name>/modules/<name>` for a nested one.
Both links are relative, as Git writes them: the submodule's `.git` file
(`gitdir: ../../.world-git/repo.git/worktrees/active/modules/<name>`) and the module's
`core.worktree`. So `git status`, `git submodule status/update/foreach` and `git -C <path> ...`
work in a World with no network, after the source and the submodules' own origins are
deleted, and after a move, trash/restore, fork or checkpoint. A submodule keeps the HEAD it
had in the source, on its branch or detached; only the root gets a `world/W<n>` branch and a
baseline. The source may keep a submodule absorbed (its `.git` a file into the
superproject's `modules/`) or old-style (a `.git` directory in the submodule's worktree);
which repository it is is asked of Git, and it must be exactly one of those two with this
directory as its worktree. In the World every submodule is absorbed; the source is never
modified.

Every check the root's import makes is made on each submodule's repository too -- the
configuration policy and filters, extensions, partial clones and alternates, the
stash stack (see [Stash](#stash)), grafts, in-progress operations, dangling symbolic refs, reserved paths in its history,
symlinked or unexpected administration -- and each is rechecked unchanged before publication.
The `.gitmodules` settings of every repository with gitlinks are read again from the copy
-- after any `--committed-only` reset, so the bytes that will be published -- and must equal,
as a whole, the ones the import was built from (a renamed section would leave the owned
repository under a name the copy no longer uses). The copy must then hold a `.git` exactly at
the imported submodules (one initialized in the
source after it was checked would still lead back into the source), and a copy of a World is
captured again, submodules included, and compared with its source. A refusal
inside a submodule names it (`reason: submodule libs/lib: ...`). A submodule with no
`.gitmodules` entry, an unsafe name, two names sharing a repository directory, a submodule
path that is (or goes through) a symlink, or a `.git` that resolves to any other repository is
refused. Any other `.git` below the root or inside a submodule is a nested repository: carried
as files when it is self-contained, refused otherwise, and always refused inside an
uninitialized submodule's directory (see [Nested repositories](#nested-repositories)).

An uninitialized submodule (a gitlink without a `.git` in its directory) stays exactly as the
source has it: the gitlink and its (usually empty) directory. Its `.gitmodules` name is checked
like an initialized one's -- no unsafe name, and no name whose repository directory would lie
in or over another submodule's, comparing names under ASCII case folding whatever the volume
(`Lib` and `lib` are one directory on a case-insensitive volume, and a World can be forked onto
one) -- since initializing it later puts its repository there. Non-ASCII bytes are compared
exactly: names that differ only by Unicode case or normalization are not detected. A
gitlink with no `.gitmodules` entry at all (an "embedded" repository added by accident, which
Git tolerates) is imported as it is when uninitialized; an initialized one is refused. Every
check that reads `.gitmodules` -- names, collisions, relative URLs -- reads the one that will be
published: the worktree's, or with `--committed-only` the committed one. A `.gitmodules` missing
there is an empty mapping, never the index's or HEAD's copy, so with `--include-changes` a
deleted `.gitmodules` leaves an initialized submodule without the entry it needs (refused),
while uninitialized gitlinks are imported as they are. The superproject's
`submodule.active` and `submodule.<name>.url`, `.active`, `.branch`, `.shallow`,
`.fetchRecurseSubmodules`, `.ignore` and `.update` settings travel with the other carried
configuration, so `git submodule update --init` in the World clones it from the source's URL.
A relative configured `url` is made absolute like a relative remote URL (with the same
refusals): Git clones a configured URL from the superproject's worktree top, remote or not. A
URL that a URL rewrite rule from a conditional include matches
is refused, and so is a conditional include that sets `submodule.<name>.*`.
A `./` or `../` URL that only `.gitmodules` gives (no `submodule.<name>.url` in the
repository's configuration, typically for an uninitialized submodule) is resolved by Git
against the URL of the repository's default remote, and against the repository's own
directory when that remote has no URL. The default remote is `branch.<current>.remote` when
HEAD is on a branch that sets it (even to an empty value, which selects no remote at all);
otherwise the only remote when exactly one is configured, and `origin` otherwise. The base is
that remote's last `url` (a remote with only a `pushurl` has none). The branch remote, the
remotes and their URLs are taken from the configuration Git in the World reads: the
repository's own (as carried, with its includes) on top of the shared global and system
configuration, last value winning; a shared remote URL is accepted only when absolute. Every
submodule's URL is classified the same way: the winning `submodule.<name>.url` -- the
repository's own (made absolute by the import; a relative one set by hand in a World is
refused) or a shared one (accepted only when absolute) -- or else the `.gitmodules` URL by the
rule above, and whichever it is goes through the rewrite-rule check. Every
`submodule.<name>.url` in the configuration is classified this way, also one for a submodule
the checked-out commit does not have (a dormant one another branch uses): in an import it is
made absolute like the rest, in a World a relative one is refused.

These URL checks cover the configuration and the `.gitmodules` that is checked out (published)
at import. `.gitmodules` on other branches or in history is not inspected: after switching a
World to another branch, a relative `.gitmodules` URL of a submodule that was never initialized
resolves the way Git resolves it at the World's location, with the World's remotes -- the same
as in any other clone at another location. It is decided for HEAD as it
will be in the World -- the root's generated branch has no upstream, a submodule keeps its
source branch unless `--committed-only` detaches it -- and the URL is refused unless that
remote's URL travels with the World. The URL is then resolved the way Git 2.54 does (each
leading `../` drops the base's last `/`-component, `./` is skipped, one trailing `/` is
dropped) and refused where Git's result could not be reproduced faithfully: a `../` that would
cut into the host or root (Git then produces relative or malformed URLs), a split at an
scp-like `host:`, an empty remainder, or a relative base. Any other relative `.gitmodules` URL
(`sub.git`, `~/x.git`) is refused too: Git clones it as it is from the worktree top. The
resolved URL -- or an absolute one `.gitmodules` gives -- is refused when a URL rewrite rule
from a conditional include matches it, exactly like a carried URL. This is checked again
whenever a World is forked or checkpointed: every fork of a World or of its checkpoints
replaces the World's own branch (and whatever upstream it had) with a new `world/W<n>`. No URL
is pinned into the World's configuration for it, since a configured URL would make Git treat
the submodule as active.
`submodule.<name>.update` is carried only as `checkout`, `rebase`, `merge` or `none`: a
`!command` (which `git submodule update` would run) or anything else is refused. The import
itself never contacts a remote and never runs `git submodule update`.

Cleanliness covers every initialized submodule: its own index and worktree must be clean and
its HEAD must be the commit the superproject's index records. Each is checked directly after
its own policy checks; the superproject's status runs with `--ignore-submodules=dirty`, so it
never starts a status inside a submodule, and neither `submodule.<name>.ignore` nor
`diff.ignoreSubmodules` can hide a dirty or moved submodule. `--include-changes` carries each
submodule's uncommitted state (index preserved, worktree bytes cloned) and its HEAD even when
it is ahead of the gitlink. `--committed-only` resets each submodule's copy, parents first, to
the commit the superproject's HEAD records for it -- detached, when its HEAD is anywhere else --
with the root's rules for index marks and filters; it is refused when that commit is not in
the submodule's repository, when the index adds or removes a submodule HEAD does not, or when
the uncommitted `.gitmodules` names an initialized submodule differently. An uninitialized
submodule stays uninitialized.

A World is copied with byte-identical configuration, so nothing in it is made absolute the way
an import makes relative URLs absolute. Forking or checkpointing a World therefore refuses a
relative path set by hand in the World or any of its submodules -- `remote.<name>.url`/`pushurl`,
`submodule.<name>.url`, or a `core.hooksPath` that leaves the tree -- since the copy would
resolve it from its own location; make it absolute. The World's configuration is read with the
files it includes (a relative include that resolves differently in the copy is already caught
by the effective-configuration comparison), and a `branch.<name>.*` setting for the new World's
branch that comes from an included file, which WorldFS cannot remove, is refused.

Forking or checkpointing a World checks the submodule layout like the root's: the exact
relative links, no `.git` directory in a submodule, no linked worktree registered in a
submodule repository (discard refuses one too) and no lock or symlink anywhere in the
administration.

`publish` checks each of the World's initialized submodules the same way before reading
anything from it: a submodule whose `.git` or `core.worktree` link was changed, or whose path
goes through a symlink, is refused.
`publish` still copies only the root's commits and fetches or pushes nothing for any
submodule. It refuses when a commit being published records a submodule commit (a gitlink
added or changed against any parent) that the target could not check out: the target has
that submodule uninitialized (or is bare), or its submodule repository does not have that
commit. The reason names the path and the commit; get the commit into the target's submodule
first. This check applies with `--force` too. Its uncommitted-changes note covers submodules.

## Nested repositories

Real trees hold Git repositories that are neither the root nor a submodule: a package installed
editable from Git (`.venv/src/<package>/.git`), a vendored checkout (`third_party/foo/.git`), a
tool's cache, a scratch clone inside the project, generated examples. A **self-contained**
nested repository is carried as ordinary files, exactly like any other directory of the tree:
the tree clone copies its `.git` directory byte for byte, the World's own Git does not manage
it, and nothing of it is rewritten. `git -C <World>/<path> status/log/commit` works in the
World, after the source is deleted and after a fork, checkpoint, pool hand-out, move, trash or
restore; the World's copy and the source's are independent (a commit in one does not change
the other).

Copying Git administration is unsafe when it points outside itself: a copied linked worktree's
`.git` file keeps leading to the source's administration (Issue #7), and so do the other links
below. A nested repository is therefore admitted only when its administration is entirely
inside its own `.git` directory:

- its `.git` is a real directory, not a symlink and not a `.git` file, and holds only
  directories and regular files -- no `objects/`, `refs/`, `config` or anything else linked
  elsewhere, whose target the copy would read and write (the socket of Git's built-in
  fsmonitor daemon, `.git/fsmonitor--daemon.ipc`, is the one special file allowed);
- it has no `commondir` (the administration of a linked worktree, whose common directory is
  elsewhere), no `objects/info/alternates` and no `objects/info/http-alternates` (objects
  borrowed from another repository, as `git clone --shared` or `--reference` sets up);
- its `worktrees/` directory is absent or empty: a registration of one of its own linked
  worktrees names that checkout's absolute path in the source, which the copy would claim as its
  own; its `modules/` directory is absent or empty too (its submodules' repositories, whose
  checkouts and links are not examined);
- its configuration sets no `core.worktree` (a worktree elsewhere), no
  `extensions.worktreeConfig` (per-worktree settings), no `lfs.storage` that leads out of its
  `.git` -- an absolute path, or a relative one with a `..` component: the copy would read its
  Git LFS objects from the source's cache and write new ones into it -- and no `include.path` or
  `includeIf.<condition>.path` at all, whatever file it names and whatever its condition. An
  included file's own settings are not examined, and neither are its further includes and
  their conditions: a `gitdir:` condition matching the source's location, for example, could
  set `core.filemode=false` for the source only, so the copy would report changes the source
  does not. Resolving Git's include graph (path forms, conditions, depth) to examine them is
  not something to reimplement, and repositories whose own configuration includes files are
  rare. Conditional includes in the user's global or system configuration that target a nested
  repository's location are not examined either; they act on it as they would on any
  repository moved there.

A bare repository that happens to be named `.git` (`core.bare`) is admitted too: it names no
worktree at all. Each repository nested in an admitted one is checked the same way, at any
depth. A `.git` file is still refused unless it is one of the root's own linked worktrees
([Linked worktrees of AI coding agents](#linked-worktrees-of-ai-coding-agents)) or an
initialized submodule's, and so is any `.git` inside the directory of an uninitialized
submodule, where `git submodule update` would put the submodule's own checkout. Inside an
initialized submodule's tree, and inside a submodule's `.world-git` directory (only the World's
root owns that name), the rule is the same as in the root. Symlinks are never followed, so a
symlink to a repository elsewhere stays a symlink and is never entered. A refusal names the
repository and the cause, for example `reason: nested Git repository at <path>: it borrows
objects from another repository (objects/info/alternates)`.

The configuration is read from its file alone, with `git config --file <path>/.git/config
--no-includes`, run from `/`. No WorldFS Git command ever runs inside a nested repository, so
its hooks, filters and fsmonitor never run during `init`, `fork`, `checkpoint` or `publish`;
the root's own status, clean and reset commands only look at the directory to tell that it is a
repository.

The check runs wherever a tree is captured: `init` (also of a directory with no Git repository
at its root), `fork` and `checkpoint` of a World (whose copy is captured again before
publication), a fork from a snapshot or a pool entry. The nested
repositories admitted at capture are checked again in the copy before it is published: one
that started borrowing objects, registered a worktree or was turned into a `.git` file while
the tree was being cloned fails the operation as busy, to be retried, instead of being
published. A nested repository that appeared in the source during the clone may be published
like any other file the source gained meanwhile, but every `.git` in the copy is checked the
same way first, so one that is not self-contained fails the operation as busy too.

The nested repository is not part of the root's history. The root's `git status` lists one that
is not ignored as an untracked directory (`?? scratch/`), so a source holding one is dirty:
`--include-changes` carries it like any untracked content, and an ignored one
(`.venv/` in `.gitignore`) is always copied, like every ignored file. `--committed-only` leaves
a non-ignored nested repository behind whole -- `git clean -ffd` removes one in an untracked
directory, which plain `-fd` skips, and one that status cannot see (its directory also holds
tracked files, or its `.git` is not one Git takes for a repository) loses its `.git`, after
which the reset treats the rest of its directory like any other content -- and keeps the
ignored ones; the copy's status is then clean. `publish` copies the root branch's commits only
and never a nested repository; a non-ignored one counts as an uncommitted change in its note.

## Shallow clones

A shallow clone (`git clone --depth <n>`, `--shallow-since`, or a submodule cloned with
`submodule.<name>.shallow` or `git submodule update --depth`) is imported as it is, at the root
and in every submodule. Its common directory's `shallow` file lists the commits whose parents the
object store does not hold; it is copied into the owned repository byte for byte before
anything walks the history, so every walk of the import -- the connectivity check below, the
reserved-path scan of the refs, stash entries, `ORIG_HEAD` and `FETCH_HEAD`, the stash
verification -- stops at the boundary exactly as it does in the source (Git applies the file
also under `--no-replace-objects`). The file is part of the captured state: it is rechecked
unchanged before publication, and so is the object directory, so a `git fetch --deepen` or
`--unshallow` in the source during the import (or a `shallow.lock` of one still running)
fails the import as busy, to be retried. Partial clones remain refused (see
[Limits](#limits-of-this-increment)): their missing objects would have to be fetched lazily from
the network.

In the World, Git's own shallow semantics apply: `git log` stops at the boundary, and the
carried remote deepens it (`git fetch --deepen=<n> origin`, `--unshallow`) while that remote is
reachable. Forks, checkpoints and pooled forks clone the `shallow` file with the rest of
`.world-git`; `--committed-only`, `--include-changes`, stash and Git LFS behave as for a full
clone.

`publish` from a shallow World is an ordinary fetch, and Git refuses a ref from a shallow
repository whose new history reaches one of its shallow commits the target does not already
have, unless `--update-shallow` lets it write that commit into the target's own `shallow` file
-- which would make the target a shallow repository. Publish never passes that option, so it
never makes a target shallow:

- the shallow clone the World came from has the history down to the same boundary (or a deeper
  one), and a full clone of the project has all of it: both take the publish, and their
  `shallow` files (or their absence) are left as they were;
- a repository that shares the World's commits above the boundary but is itself shallow at a
  commit the World's new commits build on takes it as well: only those new commits are sent;
- a repository without the history below the World's boundary -- an empty repository, an
  unrelated one, `--force` or not -- is refused with a reason that names the boundary commit.
  Git has already received the objects when it drops the ref; they stay unreferenced in the
  target, as after any other refusal that follows the fetch, and its `shallow` file is not
  touched. Deepen the World first (`git fetch --deepen=<n>` or `--unshallow` in it) or publish
  into a repository that has that history.

## Getting work back to the source

`world fs publish W<n>` copies a Git World's commits into the repository the World was
imported from, as a branch named like the World's (`world/W<n>`):

```sh
world fs publish W1                          # refs/heads/world/W1 in the source repository
world fs publish W1 --branch feature/login   # choose the name
world fs publish W1 --repo ~/src/other-clone # another clone of the same project
git -C ~/src/project merge world/W1          # merging stays a Git decision
```

Publish first validates the World's own Git administration the same way fork and checkpoint
do: a symlinked or otherwise foreign `.world-git` (or anything beneath it) is refused before
the World's repository is used for anything else, so a replaced administration cannot have
another repository's commits published in the World's name.

Import refuses `.world` or `.world-git` tracked anywhere in the history it preserves, but a
user can force-add and commit one of those reserved paths afterwards. Publish repeats that
check against the commit being published: if it, or any commit reachable from it, tracks
`.world` or `.world-git`, publish refuses and nothing is fetched into the target, even if a
later clean commit on top no longer has the path in its tree. This is checked through both
views of that history: the real commits and trees, ignoring any `refs/replace/*`, and again
with replacement refs honored -- since an active replacement that the target repository
carries identically (required by the replacement-ref compatibility check below) would
otherwise let a reserved path reach the target through the replaced view alone, even though
the raw scan sees only a safe tree.

It is an ordinary `git fetch` into that repository followed by one compare-and-swap ref
update: the checkout, index, working tree and other branches are not touched, nothing is
merged, and nothing is pushed anywhere. The World's path is canonicalized before it is used
as the fetch operand (and to detect a `url.*.insteadOf` rewrite of it), so a relative World
path resolves the same way for this check as it does for the fetch itself, regardless of the
target repository's own directory. The default repository is found by following the
World back through world forks and checkpoints to the directory `init` imported. The target
must be a distinct repository: the World's own working tree, and any other repository that
shares the World's own private common Git directory -- including `.world-git/repo.git` itself
or a linked worktree of it -- are refused as a publish target. Without
`--force`, publishing refuses a branch that is checked out in the target, an update that is
not a fast-forward, and a repository that shares no history with the World -- unless it has
no commit at all (see [Repositories with no commit yet](#repositories-with-no-commit-yet)). A
World with no commit has nothing to publish, and a shallow World cannot publish into a
repository that lacks the history below its boundary (see [Shallow clones](#shallow-clones)).
A detached World needs `--branch`. Uncommitted World changes are not published; the command says so.
Publishing re-checks the configuration policy first, so reading the World's status never
runs a filter attached after the import. The branch update and the removal of the private
staging ref are one ref transaction; if that final update fails instead -- for example
another process moved the branch first -- the staging ref is still removed before publish
reports the failure, so a failed publish never leaves one behind.

Publish copies exactly the commit that was inspected: if the World's branch (or detached
HEAD) advances between that inspection and the fetch the command runs, publish refuses
rather than pick up the newer, uninspected commit, and nothing was changed. Whichever side --
the World or the target repository -- has active, non-empty replacement refs
(`refs/replace/*`, under the effective `core.useReplaceRefs` policy) requires the other side to
carry identical ones, since a replacement changes what a commit's history and tree mean and the
fetch transfers only the branch tip; otherwise publish refuses before touching the target. This
comparison reads `core.useReplaceRefs` the way the user's own Git would -- including a global
or system setting, not just each repository's local configuration -- so a shared, ambient
`core.useReplaceRefs = false` disables the check in both repositories rather than making a
dormant replacement in one of them look active. The target repository's own replacement refs,
if any, are ignored when publish judges whether it shares history with the World and whether
the update is a fast-forward -- those checks look at the target's real history. Before any of
this, publish refuses if either the World or the target has legacy `info/grafts`: unlike
`refs/replace/*`, grafts are not disabled by `--no-replace-objects` and rewrite a commit's
parents outright, so one could make a diverged branch look like a fast-forward or shared
history to the checks that follow.

## Uncommitted content

A dirty source is refused by default; `init`, `checkpoint` and `fork --from W<n>` take one
of two explicit choices (passing both is a usage error):

- `--include-changes` carries the current work: staged changes, unstaged changes, and
  untracked files. The copied index preserves the staging boundary; no automatic `git add`,
  commit, reset or checkout is applied to user files.
- `--committed-only` creates from the committed version: the new snapshot or World's
  Git-visible content is exactly HEAD, with a clean `git status`. Staged and unstaged
  changes, deleted tracked files, files that were only staged, and untracked files that are
  not ignored stay behind, and so do nested repositories that are not ignored
  ([Nested repositories](#nested-repositories)); a pending `SQUASH_MSG`, which describes
  staged content, is not carried either. Only the copy is reset (`update-index --refresh`,
  `clean -ffd` without `-x`, `read-tree --reset -u HEAD`), never the source: its HEAD, index and files are left
  byte-for-byte as they were. Files that already match HEAD are not rewritten, so they stay
  clones of the source's blocks, and a hardlink group the reset replaces is dropped from the
  snapshot's record. No filter or hook runs. A source without a Git repository is refused.
Paths marked `skip-worktree` or `assume-unchanged` in the source are reset to HEAD too; the
marks are cleared in the copy, whose index becomes exactly HEAD.

Ignored build/data files are always copied, with either choice and when a source is
otherwise clean. Forking an immutable snapshot carries the content already captured by that
snapshot without another opt-in; `--committed-only` is refused there, since a snapshot has
no working state to leave behind (fork it and use Git, or checkpoint a World with the flag).

The source HEAD and index are checked again around import; detected changes fail the
operation before publication. Without `--include-changes`, the copied tree is also checked
for cleanliness before publication, so a tracked file edited after the source's own check
is refused rather than published as uncommitted content. This does not make an actively edited directory a coherent
point-in-time snapshot: stop editors/builds before importing or checkpointing. The existing
World exec lock also continues to apply. `--force` bypasses that lock, not the explicit
uncommitted-content choice.

The fork's baseline commit is recorded as `worldfs.baseline` in its own Git configuration,
separately from the filesystem snapshot ID. `inspect W<n>` (including JSON) reports the
baseline, current HEAD, branch and common directory for live managed Git Worlds. The
branch is HEAD's symbolic target without its `refs/heads/` prefix, so a tag sharing the
branch name does not change what is reported. A user may explicitly detach HEAD, or point
HEAD outside `refs/heads/`; inspection then reports an empty branch. Use `git diff` and
`git diff --cached` to review source changes. `world fs diff` still compares filesystem
content, including Git administrative changes such as branch/index updates.

### Repositories with no commit yet

A repository on an unborn branch -- `git init` before the first commit, or `git checkout
--orphan` in one with history -- is imported with no commit of its own: the owned repository
has the source's refs (none at all after `git init`) and its HEAD names the source's unborn
branch as captured. Files staged for the first commit are uncommitted changes like any other:
the import refuses them without `--include-changes` and preserves the index with it. An empty
repository with nothing staged or untracked imports without either flag. Every place that reads
HEAD -- capture, the recheck before publication, the reserved-path scan, LFS configuration,
submodules (a submodule on an orphan branch is a moved submodule; `--committed-only` detaches it
at the recorded commit) -- treats an unborn HEAD as "no commit", and a HEAD that is neither a
commit nor an unborn branch is still refused.

`--committed-only` is refused on an unborn branch (`init`, `checkpoint` and `fork --from W<n>`
alike): nothing is committed, so the committed version would be an empty tree that silently
leaves every staged and untracked file of the project behind. Commit first, or use
`--include-changes`.

A fork's HEAD becomes the unborn branch `world/W<n>` (chosen like any World branch) with an
empty baseline, and `inspect` reports that branch with an empty `head` and `baseline` (the text
form says `(no commit yet)` and `(none)`). The first `git commit` in the World creates the
branch as a root commit; a World forked before that keeps its empty baseline. Checkpoints,
forks and pooled forks of a World that is still unborn work as for any World, and a checkpoint
taken after the first commit forks with that commit as the baseline.

`publish` refuses a World with no commit: there is nothing to publish. A World with commits may
publish into a repository with no commit at all -- the source it came from, still unborn, or
any freshly initialized one -- without `--force`: there is no history to share or to lose, and
the World's branch becomes that repository's first. The target's own unborn HEAD is left as it
is. Into a repository that has commits, the shared-history rule applies unchanged.

## Limits of this increment

- Only a Git repository at the root of the source is imported. Detached HEAD and unborn
  branches are supported (see [Repositories with no commit yet](#repositories-with-no-commit-yet)).
  External linked worktrees are safely imported by
  resolving their source Git administration and constructing fresh local administration.
- Nested repositories that are not self-contained (see
  [Nested repositories](#nested-repositories)), sparse or split indexes, partial clones, and
  object alternates are refused, in the root and in every submodule. Shallow clones are
  supported (see [Shallow clones](#shallow-clones)). So is a repository whose index, or any commit reachable
  from its refs, HEAD, `ORIG_HEAD`, `FETCH_HEAD` or any stash entry (its worktree, index and
  untracked-files commits), contains the root `.world` file or
  anything under `.world-git`: WorldFS owns those paths, `info/exclude` cannot hide a
  tracked file, and an ordinary checkout of such a commit overwrites the World marker. Partial clones are recognized by `extensions.partialClone`,
  by any `remote.<name>.promisor` or `remote.<name>.partialclonefilter` setting, and by
  `pack-*.promisor` markers, because cloning an object database does not bring back its
  missing objects. Managed Worlds with symlinked administration, or with an additional
  linked worktree registration that cannot be read or does not match its in-tree checkout,
  are refused by fork/checkpoint; readable ones are left out of the child
  ([Linked worktrees of AI coding agents](#linked-worktrees-of-ai-coding-agents)). Discard
  refuses worktrees registered outside the World, including with `--force`, until they have
  been removed with Git; linked worktrees of submodule repositories are refused by all three.
  Do not create new Git registrations in trashed trees. The `locked` marker of `git worktree
  lock` is not a lock file. Forking or checkpointing a World whose Git
  administration holds any `*.lock` file (a running Git command or a stale lock after a
  crash) is refused, so the lock is never copied into the child; one inside a left-out
  registration (an agent committing in its worktree) is dropped with it. Merge/rebase/cherry-pick/revert in progress
  is also refused, as is an unconcluded `git notes merge`, whose state is invisible to
  `git status`. Re-import older snapshots that still contain an unconverted `.git`.
- Reftable repositories, a shared refs/object service, cross-machine history transfer, and
  publishing submodule commits are not provided. This increment does not close every requirement
  in Issue #7.
- Repository-local `core.excludesFile` and `core.attributesFile` overrides are unsupported.
  They can point outside the repository, and merging their rules into `info/exclude` or
  `info/attributes` would change Git's precedence; these overrides are not imported. Use the
  repository-local `info/exclude` and `info/attributes` files, which are preserved. Patterns
  that `git sparse-checkout disable` leaves in `info/sparse-checkout` are preserved for a later
  `sparse-checkout init`, together with the `extensions.worktreeConfig` switches it leaves in
  `config.worktree` (`core.sparseCheckout`, `core.sparseCheckoutCone`, `index.sparse`); an
  enabled sparse checkout, or any other worktree-scoped setting, is still refused.
- Repository extensions are admitted only when the owned repository reproduces them:
  `extensions.objectFormat` (SHA-1 and SHA-256 repositories), the files ref backend, and the
  sparse-checkout `worktreeConfig` case above. Others, such as `extensions.preciousObjects`,
  are refused because the owned repository does not carry them.
- Git snapshots can be pooled (`pool fill S<n>`). An entry is a plain clone of the snapshot
  with no branch of its own; the fork that takes it runs the same per-World Git setup an
  ordinary fork runs on its temporary tree -- the managed-layout checks, the `world/W<n>`
  branch and the baseline -- on the entry before it gets its marker and its public name, so
  a handed-out Git World is indistinguishable from an ordinary fork. That setup is a few
  Git commands and a walk for nested repositories and submodules, so a Git pool hit saves the clone but is
  not the bare marker-and-rename hand-out of a plain snapshot. An entry the setup has touched
  never goes back into the pool: if the setup (or anything after it) fails, the entry is
  removed, nothing is published, and the fork falls through to an ordinary clone, which
  fails the same way when the setup itself is the problem. `WFS_E_GIT_POOL` is no longer
  returned; the code stays reserved in the C ABI.
- Git setup runs inside the uncommitted clone before the normal exclusive publish rename.
  A failed Git command does not publish a World or change the source repository; existing
  temporary-tree recovery handles interrupted work. No new store schema is introduced.

Validation: `cli_git_test` uses disposable real repositories and covers clean/dirty imports,
committed-only imports, opt-in hooks, staging preservation, imported linked worktrees after
source deletion, Claude Code and Codex style agent worktrees in the source and in Worlds
(ignored or not, nested, locked, absolute or relative, hardlinked, malformed or foreign), absorbed, old-style, nested and uninitialized submodules and their
refusals, self-contained nested repositories (ignored, untracked, nested in each other or in a
submodule, under `--committed-only`, changed during the clone) and each of their refusals, shallow clones and unborn branches, independent commits, branch collisions, detached HEAD,
move/discard/restore/checkpoint, hard snapshots, pooled Git forks and their setup failures,
Git setup rollback, environment isolation and Git commits inside the exec sandbox.

### Repository status policy

Imports preserve effective repository settings for `core.autocrlf`, `core.eol`,
`core.safecrlf`, `core.filemode`, `core.symlinks`, `core.ignorecase`,
`core.precomposeunicode`, `core.trustctime`, `core.checkstat`, `core.ignorestat`,
`core.checkRoundtripEncoding`, and `core.useReplaceRefs` (mirrored `refs/replace/*`
must keep the meaning the source gave them). Included and worktree-specific values are
captured; boolean values are normalized without losing valueless true settings.
Absent settings stay absent in the owned repository. The policy is checked again
before publication along with the source index and local rules.

Global and system configuration is shared: a World's Git reads the same `~/.gitconfig`
and system file as the source's. Settings that come from them unconditionally (a global
ignore file via `core.excludesFile`, `core.attributesFile`, `core.autocrlf` and the other
status settings above) therefore mean the same thing on both sides, and cleanliness is
decided with them, exactly as the user's own `git status` decides it. The shared files are
whichever ones the import's environment names: `~/.gitconfig`/XDG and the system file by
default, or an absolute `GIT_CONFIG_GLOBAL`/`GIT_CONFIG_SYSTEM` (or `GIT_CONFIG_NOSYSTEM`)
when the environment sets one. A World is meant to be used under that same configuration;
an override set for one import command only is not recorded in the World, so identity or
status settings that came only from it are not carried, and a later command run without it
behaves as the user's own Git would without it, while an effective remote URL it rewrote
stays pinned as imported, the same as any other `insteadOf` rewrite (above). To carry
settings independent of the environment, put them in the source repository's own
configuration, which the import does capture. Other import Git commands still disable
global and system configuration.

What cannot be shared is refused with a Git configuration error that names the reason:

- A filter tracked files actually use, from any scope, unless it is the supported stock Git LFS
  filter described above. Custom or incomplete `filter.lfs` commands, LFS extensions and an
  external `lfs.storage` are refused. A filter that is only defined, such as the one a
  machine-wide `git lfs install` adds, is fine in a repository whose files do not use it; so is
  a `filter=` attribute whose driver is not defined anywhere. With `--committed-only`, the files
  and attributes of HEAD count too, so a filter HEAD assigns is refused even when uncommitted
  edits remove the assignment. Unsupported filters are never executed; the stock Git LFS status
  path is isolated in temporary storage as described above.
- A conditional `includeIf` whose target sets status, filter, or LFS settings (`lfs.*`,
  `remote.lfsdefault`, `remote.lfspushdefault`), in any scope and
  whether or not it is active at the source: the condition (a `gitdir:` pattern, a branch)
  can evaluate differently at the World's location. The same is refused for a target that
  sets per-remote or per-branch settings (`remote.<name>.*`, `branch.<name>.*`), since a
  condition inactive at the source but active at the World's location could otherwise add a
  URL to a carried remote or an upstream to the World's generated branch once the World is
  in place; section-wide settings without a name, such as `remote.pushDefault` or
  `branch.autoSetupMerge`, are unaffected. Conditional includes that set other things,
  typically a work identity, are allowed. When any conditional include sets `user.name` or
  `user.email`, the identity the source resolves is written into the World's own
  configuration (an identity the source lacks is written as an explicit empty value), so the
  World's commits carry the source's author wherever the World is placed.
- A relative `core.excludesFile` or `core.attributesFile` in global or system
  configuration: Git resolves it from each repository's location, so the source and a
  World could read different files. Use an absolute or `~/` path.
- A relative `GIT_CONFIG_GLOBAL` or `GIT_CONFIG_SYSTEM` in the environment: Git resolves
  it per repository too, so it can name a different file beside the source than beside a
  World. Use an absolute path.
- Status settings, URL rewrites (`url.<base>.insteadOf`/`pushInsteadOf`) or per-remote or
  per-branch settings (`remote.<name>.*`, `branch.<name>.*`) given as command configuration
  (`GIT_CONFIG_COUNT`, `GIT_CONFIG_PARAMETERS`, `-c`): they belong to one invocation only, and
  would otherwise be recorded permanently in the World (for example a rewritten remote URL
  pinned as if it were the source's actual configuration).
- `GIT_ATTR_SOURCE` in the environment and `attr.tree` in any scope: they make the user's
  Git read attributes from a tree-ish instead of the worktree.

Unconditional includes remain supported; when a World is forked or checkpointed, the copy's
whole effective repository configuration must match the source World's, so a relative
include that resolves to different content at the new location (hooks, identity or any
other setting) makes the operation fail.
Other refusals report `unsupported Git layout` followed by a `reason:` line naming the
specific cause (for example `reftable ref storage`, or `nested Git repository at <path>: it
sets core.worktree`).
Owned repositories
are created with an empty template directory so installed Git templates cannot add hooks or rules.

### External reference restrictions

Symbolic refs whose target does not exist (for example `git symbolic-ref
refs/heads/alias refs/heads/future`) are refused: Git's ref listing and the mirror both
omit them, so the import could not preserve them. Repositories using the reftable ref
backend are refused because it offers no read-only way to find such refs; the owned
repositories themselves always use the files backend.

Refs hidden with `transfer.hideRefs` or `uploadpack.hideRefs` are imported like any other
ref. Those settings only hide refs from a client that fetches from or pushes to the
repository over a transport, and the import uses none: it lists the refs with
`for-each-ref`, clones the object directory and writes the refs itself (see
[Import cost](#import-cost)), and the recheck before publication compares the same complete
listing. The settings themselves are the source's serving policy, repository-local and outside
the carried configuration, so the World has the refs without the settings: nothing in it is
hidden. (A World is not a server; `publish` fetches the World's branch by its exact name.) A
World's own hiding settings travel with its cloned `.world-git` through forks and
checkpoints, together with the refs.

### Stash

The whole stash stack is imported, in the root and in every initialized submodule (each
repository has its own). A stash is `refs/stash` plus its reflog, `logs/refs/stash` in the
repository's common directory (shared by all of its worktrees): `stash@{0}` is the ref and
`stash@{1..n}` exist only as reflog entries. The object-store clone includes objects that only
older reflog entries reach. Import writes the stash reflog byte for byte where the owned
repository's Git reads it (for the root, `.world-git/repo.git/logs/refs/stash`, which the World's
worktree shares), then restores the reflog and verifies every entry's commit, index and
untracked-files commit/tree in that object store. Nothing in the source changes and no hook or
filter runs.

The reflog's bytes are part of the captured state: a stash pushed, dropped or popped in the
source before publication aborts the import like any other ref change. The copy that will be
published is then read back by Git itself -- the same reflog bytes, the same entries (selector,
commit, tree, parents and message) and every commit, tree and blob of every entry present --
and must match the source as a whole. Stash entries are history the World can check out
(`git stash apply` writes their trees), so the reserved-path check covers them too: an entry
whose worktree, index or untracked-files tree holds `.world` or `.world-git` is refused. In the
World, after the source is deleted, `git stash list/show/apply/pop` work as they did in the
source; forks and checkpoints (with `--committed-only` too, which never touches the stash) keep
the stack, since the whole `.world-git` is cloned. `publish` moves one branch only and never
publishes a stash.

Only the stash reflog is carried, because the stash *is* a reflog. Other reflogs (`HEAD`,
branches) are not imported, and import does not promise preservation of that history.

External repositories with `info/grafts` are rejected because their local ancestry
overrides are not carried into the owned repository. Active bisect and sequencer sessions,
like unfinished merges, cherry-picks and rebases, must be completed or aborted
before import or checkpoint; their administrative state is not a clean baseline.

A completed, conflict-free squash merge may be imported with `--include-changes`.
Its staged changes and passive `SQUASH_MSG` are preserved so the next Git commit
retains the prepared message. Conflicted squash merges remain unsupported.

Imports retain `ORIG_HEAD` recovery state and exact optional `FETCH_HEAD` records
when present, and compare the complete
ref-name/object-ID snapshot before publication. A concurrent non-HEAD ref update
aborts the import rather than publishing a stale mirror. Sources must still remain
quiescent during import; validation does not lock arbitrary external Git writers.

Learned `git rerere` resolutions in the common `rr-cache` are copied into the owned
repository and rechecked before publication, so recurring conflicts still resolve after
the source is deleted. Git's layout of one directory per conflict holding regular files
is required: a symlinked cache, nested directories or special files are refused.

### Import cost

An external import does not byte-copy the Git object store. Each owned repository -- the
root's and every submodule's -- is built by one helper (`own_repository` in
`core/src/git.cpp`):

1. `git init --bare --template=` with the source's object format.
2. The source's common `objects/` directory is cloned with `fs_clone_tree`: clonefile on
   APFS, reflinks on XFS/Btrfs, a (sparse-aware) copy on ext4 or when the object directory
   is on another volume. Symlinks are never followed. The clone is then reduced to what an
   object store holds -- loose objects, packs with their `.idx`/`.rev`/`.bitmap`/`.keep`/
   `.mtimes`, multi-pack indexes, `info/packs` and commit-graphs. Files Git leaves
   mid-write (`tmp_obj_*`, `tmp_pack_*`, `.tmp-*`) and `incoming-*` quarantine directories
   of a push not yet accepted are dropped: they are not part of the repository, and anything
   reachable that is missing is caught below. A symlink or special file, alternates or a
   promisor pack found in the clone are refused as they are in the source. A shallow clone's
   captured `shallow` file is written next to it.
3. The captured refs are written with one `update-ref --stdin` transaction (which checks
   each tip object exists) and packed with `pack-refs --all`; HEAD, the transient
   `remote.origin` mirror settings (removed again with the rest of the import) and the
   symbolic-ref edges follow. Nothing is fetched or re-packed.

The result matches what `git clone --mirror --no-hardlinks` produced, except that ref
directories emptied by `pack-refs` (for example `refs/notes`) remain, and a SHA-256
repository has its `[extensions]` section first in `config`.

A source that repacks or collects garbage during the import can make the clone miss an
object (a pack deleted before the clone reached it). Before publication every owned
repository is walked -- `rev-list --objects` over all refs and HEADs, reflogs, the index,
`ORIG_HEAD` and `FETCH_HEAD` tips, with `--no-replace-objects`, down to a shallow clone's
boundary -- and a missing object fails the import as busy (retryable) rather than publishing it. Changes to the source's object
directory are also detected by the existing size recheck. The walk is Git's own
post-fetch connectivity check; it costs a history traversal without reading blob contents.

The free-space preflight follows the tree clone's budget: where the object directory can be
cloned into the store's volume sharing data, it costs its metadata (1 KiB per entry);
where it is copied (ext4, another volume) the full logical size of the object directory is
budgeted. The rerere cache is always budgeted in full. Managed Worlds clone their owned
object databases with the rest of the tree and incur no additional budget.
