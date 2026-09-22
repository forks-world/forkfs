#pragma once
#include "internal.h"

namespace wfs {
struct GitSymref {
    String name, target;
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
    Vec<GitSymref> symrefs;
};
int git_source(const char *root, bool include_changes, GitSource &out);
int git_import(const GitSource &source, const char *clone);
int git_discard_check(const char *root);
int git_branch(const char *clone, wfs_id world);
}
