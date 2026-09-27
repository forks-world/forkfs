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
World. Initial import mirrors resolvable `refs/*` with Git's `--mirror` mode and
`--no-hardlinks`, then restores each captured symbolic-ref edge before creating the private
worktree; subsequent filesystem forks use APFS cloning, including the Git objects. Git's
[worktree documentation](https://git-scm.com/docs/git-worktree) describes these per-worktree
administrative links.

Git refs whose symbolic targets are outside `refs/`, malformed, dangling or cyclic are
unsupported by this import contract and may be omitted by Git's mirror operation. The
source is rechecked for the captured symbolic-ref map before publication.

This deliberately does not put a shared writable repository in the store: the existing
exec sandbox can continue denying writes to the entire store and every other World.
Moving, trashing, restoring, checkpointing or collecting a World moves or removes its Git
administration with the same tree. No separate registry needs a best-effort cleanup and no
external worktree becomes a GC target. Source worktree registrations are not imported.
Deleting the original source after a successful import does not remove Git objects needed
by the World, including staged blobs which have not yet appeared in any commit.

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

## Submodules

An initialized submodule -- a directory the index records as a gitlink that holds a `.git`,
at any depth up to 8 levels of nesting -- is imported with the root. Its repository is copied
the way the root's is (a `--mirror --no-hardlinks` clone, then its index, symbolic refs, local
rules, rerere cache, status settings, identity, carried remotes and submodule settings,
`SQUASH_MSG`, `FETCH_HEAD`, `ORIG_HEAD` and, with `--with-hooks`, its hooks) into the place
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
configuration policy and filters, extensions, partial/shallow/alternates, stash and hidden
refs, grafts, in-progress operations, dangling symbolic refs, reserved paths in its history,
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
refused. A plain nested repository -- a `.git` anywhere below the root that is not an
initialized submodule's, including deeper inside an uninitialized submodule's directory or
inside a submodule's own `.world-git` directory (only the World's root owns that name) -- is
still refused.

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
not a fast-forward, and a repository that shares no history with the World. A detached World
needs `--branch`. Uncommitted World changes are not published; the command says so.
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
  not ignored stay behind; a pending `SQUASH_MSG`, which describes staged content, is not
  carried either. Only the copy is reset (`update-index --refresh`, `clean -fd` without
  `-x`, `read-tree --reset -u HEAD`), never the source: its HEAD, index and files are left
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

## Limits of this increment

- Only a root repository with an existing commit is supported. Detached HEAD is supported;
  unborn repositories are refused. External linked worktrees are safely imported by
  resolving their source Git administration and constructing fresh local administration.
- Plain nested repositories (not submodules; see [Submodules](#submodules)), sparse or split
  indexes, shallow/partial clones, and object alternates are refused, in the root and in every
  submodule. So is a repository whose index, or any commit reachable
  from its refs, HEAD, `ORIG_HEAD` or `FETCH_HEAD`, contains the root `.world` file or
  anything under `.world-git`: WorldFS owns those paths, `info/exclude` cannot hide a
  tracked file, and an ordinary checkout of such a commit overwrites the World marker. Partial clones are recognized by `extensions.partialClone`,
  by any `remote.<name>.promisor` or `remote.<name>.partialclonefilter` setting, and by
  `pack-*.promisor` markers, because a local mirror copies an object database without its
  missing objects. Managed Worlds with symlinked administration or additional
  linked worktrees are refused by fork/checkpoint. Discard also refuses registered extra
  worktrees, including with `--force`, until they have been removed with Git. Do not create
  new Git registrations in trashed trees. Forking or checkpointing a World whose Git
  administration holds any `*.lock` file (a running Git command or a stale lock after a
  crash) is refused, so the lock is never copied into the child. Merge/rebase/cherry-pick/revert in progress
  is also refused, as is an unconcluded `git notes merge`, whose state is invisible to
  `git status`. Re-import older snapshots that still contain an unconverted `.git`.
- Git LFS hydration, reftable repositories, a shared refs/object service, cross-machine
  history transfer, and publishing submodule commits are not provided. This increment does not close every requirement in Issue #7.
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
  are refused because a mirror clone does not carry them.
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
source deletion, absorbed, old-style, nested and uninitialized submodules and their
refusals, independent commits, branch collisions, detached HEAD,
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

- A filter that tracked files actually use (for example Git LFS), from any scope. A filter
  that is only defined, such as the one a machine-wide `git lfs install` adds, is fine in a
  repository whose files do not use it; so is a `filter=` attribute whose driver is not
  defined anywhere. With `--committed-only`, the files and attributes of HEAD count too, so
  a filter HEAD assigns is refused even when uncommitted edits remove the assignment. Filters
  are never executed by the import.
- A conditional `includeIf` whose target sets status or filter settings, in any scope and
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
specific cause (for example `reftable ref storage` or `nested Git repository or submodule`).
Mirror imports
use an empty template directory so installed Git templates cannot add hooks or rules.

### External reference restrictions

Symbolic refs whose target does not exist (for example `git symbolic-ref
refs/heads/alias refs/heads/future`) are refused: Git's ref listing and the mirror both
omit them, so the import could not preserve them. Repositories using the reftable ref
backend are refused because it offers no read-only way to find such refs; the owned
repositories themselves always use the files backend.

Initial imports from external repositories reject any configured `transfer.hideRefs`
or `uploadpack.hideRefs`, because the mirror transport may omit those refs. They
also reject an existing `refs/stash` reflog: mirroring a ref does not preserve the
stash stack. Ref tips without a stash reflog remain supported. Import does not
promise preservation of other external reflog history.

These restrictions do not apply to managed Worlds: their Git administration is
cloned as part of the filesystem, retaining hidden refs and native stash stacks
through forks and checkpoints. External eligibility is checked again around import.

External repositories with `info/grafts` are rejected because their local ancestry
overrides are not transported by a mirror. Active bisect and sequencer sessions,
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

External imports budget the full logical size of the common Git object directory
and the rerere cache in addition to filesystem clone metadata and the free-space reserve. This includes
objects outside a linked worktree. Managed Worlds use filesystem cloning for their
owned object databases and do not incur this additional full-copy budget.
