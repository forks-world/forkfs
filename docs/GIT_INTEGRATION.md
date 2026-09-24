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
Git settings are not imported. Local `user.name` and `user.email` are preserved; normal Git
commands in a World also use the user's usual Git configuration. Import does not create a
remote back to the source. Fetching/pushing requires explicitly configuring a remote.

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

Effective repository `filter.*` definitions are rejected before status inspection.
Arbitrary configuration, executable conversions, hooks, and external policy paths
are not imported. A configuration-only probe reads global and system configuration
key names, including active includes. Status-policy keys listed above, external
attributes/ignore overrides, and filter definitions in those scopes are rejected
before deciding cleanliness, even with `--include-changes`. This avoids silently
changing the user's normal Git status semantics. The probe does not execute
filters or copy configuration values; other import Git commands continue to
disable global and system configuration. Identity-only global configuration is
allowed and remains available to ordinary Git commands in the World.
Conditional `includeIf` directives are unsupported in any scope, even when
inactive at capture: moving a World or switching branches can activate policies
that were not visible before publication. Unconditional includes remain supported.
`GIT_ATTR_SOURCE` in the environment and `attr.tree` in any configuration scope are
refused as well: they make the user's Git read attributes from a tree-ish that import
commands and the World would not use.
The same policy rejection covers command configuration injected through
`GIT_CONFIG_COUNT` and `GIT_CONFIG_PARAMETERS`. Only the read-only probe receives
those variables; normal import commands continue to discard them. Mirror imports
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
