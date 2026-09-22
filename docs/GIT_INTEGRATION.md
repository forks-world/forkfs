# Git-aware Worlds

`world fs init` detects a Git repository at the source root. It imports an independent
repository into the snapshot; each subsequent `world fs fork` automatically creates a
native linked worktree on a new `world/W<n>` branch. A conflicting existing branch is
preserved and a numeric suffix is chosen (`world-W<n>` if `world` itself is a branch). Ordinary directories need no Git installation.
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
operation before publication. This does not make an actively edited directory a coherent
point-in-time snapshot: stop editors/builds before importing or checkpointing. The existing
World exec lock also continues to apply. `--force` bypasses that lock, not the explicit
uncommitted-content choice.

The fork's baseline commit is recorded as `worldfs.baseline` in its own Git configuration,
separately from the filesystem snapshot ID. `inspect W<n>` (including JSON) reports the
baseline, current HEAD, branch and common directory for live managed Git Worlds. A user
may explicitly detach HEAD; inspection then reports an empty branch. Use `git diff` and
`git diff --cached` to review source changes. `world fs diff` still compares filesystem
content, including Git administrative changes such as branch/index updates.

## Limits of this increment

- Only a root repository with an existing commit is supported. Detached HEAD is supported;
  unborn repositories are refused. External linked worktrees are safely imported by
  resolving their source Git administration and constructing fresh local administration.
- Nested repositories/submodules, sparse or split indexes, shallow/partial clones, and
  object alternates are refused. Managed Worlds with symlinked administration or additional
  linked worktrees are refused by fork/checkpoint. Discard also refuses registered extra
  worktrees, including with `--force`, until they have been removed with Git. Do not create
  new Git registrations in trashed trees. Merge/rebase/cherry-pick/revert in progress
  is also refused. Re-import older snapshots that still contain an unconverted `.git`.
- Git LFS hydration, recursive submodule import, a shared refs/object service, a switch to
  discard current edits and materialize only HEAD, and cross-machine history transfer are
  not provided. This increment does not close every requirement in Issue #7.
- Repository-local `core.excludesFile` and `core.attributesFile` overrides are unsupported.
  They can point outside the repository, and merging their rules into `info/exclude` or
  `info/attributes` would change Git's precedence; these overrides are not imported. Use the
  repository-local `info/exclude` and `info/attributes` files, which are preserved.
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
and `core.checkRoundtripEncoding`. Included and worktree-specific values are
captured; boolean values are normalized without losing valueless true settings.
Absent settings stay absent in the owned repository. The policy is checked again
before publication along with the source index and local rules.

Effective repository `filter.*` definitions are rejected before status inspection.
Arbitrary configuration, executable conversions, hooks, and external policy paths
are not imported. Global and system Git configuration are disabled during import.

### External reference restrictions

Initial imports from external repositories reject any configured `transfer.hideRefs`
or `uploadpack.hideRefs`, because the mirror transport may omit those refs. They
also reject an existing `refs/stash` reflog: mirroring a ref does not preserve the
stash stack. Ref tips without a stash reflog remain supported. Import does not
promise preservation of other external reflog history.

These restrictions do not apply to managed Worlds: their Git administration is
cloned as part of the filesystem, retaining hidden refs and native stash stacks
through forks and checkpoints. External eligibility is checked again around import.
