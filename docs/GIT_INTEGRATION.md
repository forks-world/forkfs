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
Git settings are not imported. Local `user.name` and `user.email` are preserved, and so is an
identity a conditional include supplies at the source's location; normal Git commands in a
World also use the user's usual Git configuration. Import does not create a
remote back to the source.

The source's repository-local remotes travel into the World: remote URLs, push URLs,
fetch and push refspecs, tag and prune options, remote groups, branch upstreams
(`branch.<name>.remote`/`merge`/`pushRemote`/`rebase`), `url.<base>.insteadOf` rewrites,
`remote.pushDefault`, `push.default`, `push.autoSetupRemote`, `fetch.prune` and aliases.
`git fetch origin` and `git push origin <branch>` therefore work in a World as in the
source; nothing is fetched or pushed automatically, and the World's own `world/W<n>` branch
starts without an upstream. A relative local remote path is made absolute against the source,
so it keeps reaching the same repository after the source is deleted. A relative remote URL
that any `url.<base>.insteadOf` or `pushInsteadOf` rule matches is refused instead (make the
URL absolute or remove the rule), because a rewrite of a relative path cannot be carried
faithfully to a World in another location. When such a rule instead rewrites an absolute
remote URL -- for example a per-account rule reached through a conditional include such as
`includeIf "gitdir:~/work/"` -- the World does not carry the rewrite rule's effect raw:
it records the fetch and push URLs Git actually uses for that remote at the source, so the
World reaches the same endpoints wherever it is placed. A pinned URL that another rule would
rewrite again is refused, since the World would then resolve it differently than the source
does; simplify the rewrite rules before importing. A conditional rule that only applies at the
World's own location applies there, exactly as it would for any repository placed there.
Settings Git
runs on its own are not carried: hooks and `core.hooksPath`, `remote.<name>.uploadpack`,
`receivepack` and `vcs`, `branch.<name>.mergeOptions`, `core.sshCommand` and credential
helpers (a global credential helper still applies). These settings are rechecked before
publication like the rest of the captured state.

## Getting work back to the source

`world fs publish W<n>` copies a Git World's commits into the repository the World was
imported from, as a branch named like the World's (`world/W<n>`):

```sh
world fs publish W1                          # refs/heads/world/W1 in the source repository
world fs publish W1 --branch feature/login   # choose the name
world fs publish W1 --repo ~/src/other-clone # another clone of the same project
git -C ~/src/project merge world/W1          # merging stays a Git decision
```

It is an ordinary `git fetch` into that repository followed by one compare-and-swap ref
update: the checkout, index, working tree and other branches are not touched, nothing is
merged, and nothing is pushed anywhere. The World's path is canonicalized before it is used
as the fetch operand (and to detect a `url.*.insteadOf` rewrite of it), so a relative World
path resolves the same way for this check as it does for the fetch itself, regardless of the
target repository's own directory. The default repository is found by following the
World back through world forks and checkpoints to the directory `init` imported. Without
`--force`, publishing refuses a branch that is checked out in the target, an update that is
not a fast-forward, and a repository that shares no history with the World. A detached World
needs `--branch`. Uncommitted World changes are not published; the command says so.
Publishing re-checks the configuration policy first, so reading the World's status never
runs a filter attached after the import. The branch update and the removal of the private
staging ref are one ref transaction; if that final update fails instead -- for example
another process moved the branch first -- the staging ref is still removed before publish
reports the failure, so a failed publish never leaves one behind.

## Uncommitted content

A dirty source is refused by default. Pass `--include-changes` to `init`, `checkpoint`, or
`fork --from W<n>` to explicitly preserve staged changes, unstaged changes, and untracked
files. The copied index preserves the staging boundary; no automatic `git add`, commit,
reset or checkout is applied to user files. Ignored build/data files are always copied,
including when a source is otherwise clean. Forking an immutable snapshot carries the
content already captured by that snapshot without another opt-in.

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
- Nested repositories/submodules, sparse or split indexes, shallow/partial clones, and
  object alternates are refused. So is a repository whose index, or any commit reachable
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
- Git LFS hydration, recursive submodule import, a shared refs/object service, a switch to
  discard current edits and materialize only HEAD, and cross-machine history transfer are
  not provided. This increment does not close every requirement in Issue #7.
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
- Git Worlds use the ordinary temporary-tree fork path. `pool fill` rejects Git snapshots;
  it does not build entries that Git-aware forks cannot consume.
- Git setup runs inside the uncommitted clone before the normal exclusive publish rename.
  A failed Git command does not publish a World or change the source repository; existing
  temporary-tree recovery handles interrupted work. No new store schema is introduced.

Validation: `cli_git_test` uses disposable real repositories and covers clean/dirty imports,
staging preservation, imported linked worktrees after source deletion, independent commits,
branch collisions, detached HEAD, move/discard/restore/checkpoint, hard snapshots, pool
refusal, Git setup rollback, environment isolation and Git commits inside the exec sandbox.

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
decided with them, exactly as the user's own `git status` decides it. Other import Git
commands still disable global and system configuration.

What cannot be shared is refused with a Git configuration error that names the reason:

- A filter that tracked files actually use (for example Git LFS), from any scope. A filter
  that is only defined, such as the one a machine-wide `git lfs install` adds, is fine in a
  repository whose files do not use it; so is a `filter=` attribute whose driver is not
  defined anywhere. Filters are never executed by the import.
- A conditional `includeIf` whose target sets status or filter settings, in any scope and
  whether or not it is active at the source: the condition (a `gitdir:` pattern, a branch)
  can evaluate differently at the World's location. Conditional includes that set other
  things, typically a work identity, are allowed. When any conditional include sets
  `user.name` or `user.email`, the identity the source resolves is written into the World's
  own configuration (an identity the source lacks is written as an explicit empty value), so
  the World's commits carry the source's author wherever the World is placed.
- A relative `core.excludesFile` or `core.attributesFile` in global or system
  configuration: Git resolves it from each repository's location, so the source and a
  World could read different files. Use an absolute or `~/` path.
- A relative `GIT_CONFIG_GLOBAL` or `GIT_CONFIG_SYSTEM` in the environment: Git resolves
  it per repository too, so it can name a different file beside the source than beside a
  World. Use an absolute path.
- Status settings given as command configuration (`GIT_CONFIG_COUNT`,
  `GIT_CONFIG_PARAMETERS`, `-c`): they belong to one invocation only.
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
