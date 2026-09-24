#pragma once
#include "internal.h"

namespace wfs {
struct GitSymref {
    String name, target;
};
struct GitSetting {
    String key, value;
};
// Git administration is owned by the tree, so the existing rename/trash/GC protocol also
// owns all of its Git resources. No worktree is registered in the user's source repository.
struct GitSource {
    bool present = false;
    bool managed = false;
    String root, head, index_path;
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
    // user.name / user.email as the source defines them (present ones only, possibly empty).
    Vec<GitSetting> identity;
    Vec<char> refs;
    bool orig_present = false;
    String orig_head;
    // rr-cache of an external source, as sorted records (see capture_rerere); managed Worlds
    // carry theirs inside the cloned tree. import_bytes includes rerere_bytes.
    Vec<char> rerere;
    bool rerere_present = false;
    uint64_t rerere_bytes = 0;
    uint64_t import_bytes = 0;
    // Set when the caller did not pass --include-changes: the published copy must be clean too.
    bool require_clean = false;
};
int git_source(const char *root, bool include_changes, GitSource &out);
int git_import(const GitSource &source, const char *clone);
int git_discard_check(const char *root);
int git_branch(const char *clone, wfs_id world);
}
