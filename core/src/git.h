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
int git_branch(const char *clone, wfs_id world);
}
