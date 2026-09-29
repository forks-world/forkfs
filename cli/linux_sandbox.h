#pragma once
// Called only in the forked child. Replaces the process with the namespace runner;
// on setup/exec failure returns -errno. The caller must exit, never run unconfined.
// `mounts` (NULL or NULL-terminated) are further Bubblewrap mount arguments below `root`, parents
// first: how exec keeps the World's Git hooks read-only.
int linux_sandbox_exec(const char *root, const char *store, char *const mounts[], char *const command[]);
