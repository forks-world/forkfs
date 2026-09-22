#pragma once
// Called only in the forked child. Replaces the process with the namespace runner;
// on setup/exec failure returns -errno. The caller must exit, never run unconfined.
int linux_sandbox_exec(const char *root, const char *store, char *const command[]);
