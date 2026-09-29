#pragma once
#include "internal.h"

namespace wfs {
struct GitSymref {
    String name, target;
};
struct GitSetting {
    String key, value;
};
// One executable hook file of the source's hooks directory, carried by --with-hooks.
struct GitHook {
    String name;
    Vec<char> bytes;
    uint32_t mode = 0;
};
// A linked worktree registered in a repository's common directory (`worktrees/<id>`) besides the
// checkout being imported or copied -- an AI agent's (Claude Code's `.claude/worktrees/<name>`, a
// Codex worktree elsewhere) or the user's own. It is a separate checkout of the same repository,
// not part of the tree: its refs and objects are the repository's and travel with it, its
// checkout and its registration do not (collect_worktrees).
struct GitWorktree {
    String id;        // the registration's name under <common>/worktrees
    String path;      // its checkout directory as registered (for messages)
    String rel;       // that checkout relative to the tree's root, when it exists inside the tree
    bool inside = false;   // the checkout is (or, gone, was registered) inside the tree
};
// The captured state of one repository -- the root, or one initialized submodule -- that the
// import reproduces in owned administration and rechecks before publication.
struct GitRepoState {
    bool managed = false;
    // The repository's worktree (for a submodule, its directory in the source) and its absolute
    // Git directory as Git resolves it.
    String root, admin, head, index_path;
    // HEAD's symbolic target (refs/heads/...), or empty when HEAD is detached. Submodules keep it.
    String head_ref;
    Vec<char> index;
    String exclude_path;
    Vec<char> exclude;
    String attributes_path;
    Vec<char> attributes;
    String squash_path;
    Vec<char> squash;
    bool squash_present = false;
    // Patterns a disabled sparse checkout keeps for a later `git sparse-checkout init`.
    String sparse_path;
    Vec<char> sparse;
    bool sparse_present = false;
    String fetch_path;
    Vec<char> fetch;
    bool fetch_present = false;
    Vec<GitSymref> symrefs;
    Vec<GitSetting> settings;
    // extensions.worktreeConfig and the worktree-scoped settings it carries. Only the keys a
    // disabled sparse checkout leaves behind are admitted (see capture_worktree_config).
    // Managed Worlds only: the whole effective configuration (`config --list --includes`), so
    // a copy whose relative includes resolve differently at its new location is caught.
    Vec<char> effective_config;
    bool worktree_config = false;
    Vec<GitSetting> worktree_settings;
    // External sources: the repository-local remotes, upstream tracking, URL rewrites, push
    // defaults, submodule settings and aliases that travel into the owned repository
    // (capture_carried_config).
    Vec<GitSetting> carried;
    // user.name / user.email as the source defines them (present ones only, possibly empty).
    Vec<GitSetting> identity;
    Vec<char> refs;
    // The stash stack: refs/stash (in `refs`) and its reflog, whose entries are stash@{1..n}.
    // The reflog lives in the common directory, shared by every worktree of the repository.
    // Ordinary Git clones copy only the ref, so the reflog travels as bytes; this implementation
    // clones the complete object store and verifies every reflog-only entry after restoration.
    // `stash_list` is Git's own reading of the stack (capture_stash), compared as a whole in copy.
    String stash_path;
    Vec<char> stash;
    bool stash_present = false;
    Vec<char> stash_list;
    bool orig_present = false;
    String orig_head;
    // rr-cache of an external source, as sorted records (see capture_rerere); managed Worlds
    // carry theirs inside the cloned tree. import_bytes includes rerere_bytes.
    Vec<char> rerere;
    bool rerere_present = false;
    uint64_t rerere_bytes = 0;
    uint64_t import_bytes = 0;
    // Git LFS local media cache under the repository's common directory. The payload store is
    // copied into the World's owned common directory when LFS is active.
    String lfs_objects;
    uint64_t lfs_bytes = 0;
    uint64_t lfs_entries = 0;
    bool lfs_present = false;
    bool lfs_active = false;
    bool lfs_filter_setup = false;
    bool lfs_skip_smudge = false;
    bool lfs_skip_process = false;
    // LFS is used only by the committed submodule target selected by --committed-only.
    bool lfs_target_only = false;
    Vec<char> lfs_manifest;
    // Effective LFS endpoint settings and the worktree/HEAD .lfsconfig bytes validated on
    // import, rechecked with the other source configuration before the owned copy is published.
    Vec<char> lfs_endpoint_state;
    // External sources: the common object directory the owned repository's objects are cloned
    // from, and how many entries it held when captured (the metadata a copy-on-write clone
    // costs; see git_import_budget).
    String objects;
    uint64_t object_entries = 0;
    // --with-hooks, external sources only (a managed World's hooks travel inside its cloned
    // .world-git): the executable, regular, non-.sample files of the common hooks directory and
    // a repository-local core.hooksPath, installed in the owned repository (capture_hooks).
    bool with_hooks = false;
    Vec<GitHook> hooks;
    bool hooks_path_present = false;
    String hooks_path;
    // When the index records gitlinks: the .gitmodules settings the published copy will have
    // (`config --null --list`), rechecked against the copy before publication.
    bool gitmodules_checked = false;
    Vec<char> gitmodules;
    // The root only: the repository's other linked worktrees. An external source lists those
    // whose checkout is inside the tree (admitted by nested_walk, left out of the copy); a
    // managed World lists every extra registration, whose checkouts may be anywhere.
    Vec<GitWorktree> worktrees;
    // The root only: the absolute common directory those registrations were read from.
    String common;
};
// One initialized submodule, at any depth. Git looks for a submodule's repository in
// `$GIT_DIR/modules/<name>` of its superproject -- for the root, the World's own per-worktree
// directory .world-git/repo.git/worktrees/active -- so a nested one lives in
// `modules/<name>/modules/<name>`. The worktree's `.git` file and the module's core.worktree
// are relative, like the root's own links.
struct GitModule {
    String path;       // relative to the tree's root
    String name;       // from .gitmodules
    String gitdir;     // relative to .world-git/repo.git/worktrees/active
    // --committed-only: the commit the superproject's committed tree records for it.
    String target;
    GitRepoState repo;
};
// Git administration is owned by the tree, so the existing rename/trash/GC protocol also
// owns all of its Git resources. No worktree is registered in the user's source repository.
struct GitSource : GitRepoState {
    bool present = false;
    // Set when the caller did not pass --include-changes: the published copy must be clean too.
    bool require_clean = false;
    // --committed-only: the source may be dirty, but the copy is reset to HEAD before
    // publication (reset_to_head); ignored files stay. The source itself is never touched.
    bool committed_only = false;
    // Initialized submodules, parents before their own submodules. An external import clones
    // each one's objects too (git_import_budget).
    Vec<GitModule> modules;
    // The root's index records gitlinks, initialized or not.
    bool has_gitlinks = false;
};
int git_source(const char *root, bool include_changes, GitSource &out, bool committed_only = false,
               bool with_hooks = false);
int git_import(const GitSource &source, const char *clone);
// The free space an external import needs on the volume holding `near`, beyond the tree clone
// itself: each repository's rerere cache, plus its object directory's clone metadata where
// that directory can be cloned there sharing data, or its full logical size where it is copied.
uint64_t git_import_budget(const GitSource &source, const char *near);
int git_discard_check(const char *root);
// The linked worktrees the last git_import on this thread left out of its copy, one per line
// (see wfs_git_omitted_worktrees). Cleared by the operations that import.
void git_clear_omitted();
int git_branch(const char *clone, wfs_id world);
}
