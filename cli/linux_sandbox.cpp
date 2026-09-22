// Namespace policy belongs to the CLI; the embeddable filesystem core has no runner dependency.
#include "linux_sandbox.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/close_range.h>
#include <limits.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
int socket_filter() {
    // Network is shared for development tools. Do not expose host Unix-domain services
    // (Docker, session D-Bus, abstract sockets): readonly mounts do not block connect().
    // socketpair remains available for IPC between the command and its children.
#if defined(__x86_64__)
    constexpr unsigned arch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    constexpr unsigned arch = AUDIT_ARCH_AARCH64;
#else
    return -ENOTSUP;
#endif
#if defined(__x86_64__) || defined(__aarch64__)
    const struct sock_filter program[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, arch, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
#ifdef __x86_64__
        // Reject the x32 ABI too; otherwise its syscall numbers bypass the native filter.
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x40000000, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
#endif
        // io_uring can create/connect sockets without the socket syscall.
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_io_uring_setup, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
        // A datagram socketpair can be reconnected to a host Unix socket with connect()
        // or sendto(). Permit only stream pairs, whose peer cannot be replaced.
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socketpair, 0, 5),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1])),
        BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0xf),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SOCK_STREAM, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    int fd = (int)::syscall(SYS_memfd_create, "world-seccomp", 0);
    if (fd < 0) return -errno;
    if (::write(fd, program, sizeof program) != (ssize_t)sizeof program || ::lseek(fd, 0, SEEK_SET) < 0) {
        int e = errno ? errno : EIO; ::close(fd); return -e;
    }
    return fd; // intentionally inherited by bwrap, which consumes/closes it before the command
#endif
}
} // namespace

int linux_sandbox_exec(const char *root, const char *store, char *const command[]) {
    // The caller may have inherited writable files/directory handles. Bubblewrap preserves
    // arbitrary descriptors: seal all of them before creating the one policy fd it needs.
    // Keep only the caller-authorized stdin/stdout/stderr. Unsupported kernels fail closed.
    if (::syscall(SYS_close_range, 3u, ~0u, CLOSE_RANGE_CLOEXEC)) return -errno;
    int filter = socket_filter();
    if (filter < 0) return filter;
    char fd[32];
    ::snprintf(fd, sizeof fd, "%d", filter);
    const char *policy[] = {
        "/usr/bin/bwrap",
        "--unshare-user", "--unshare-pid", "--unshare-ipc", "--unshare-uts",
        "--unshare-cgroup-try", "--die-with-parent", "--new-session",
        "--disable-userns", "--assert-userns-disabled", "--cap-drop", "ALL",
        "--ro-bind", "/", "/",
        "--proc", "/proc", "--dev", "/dev",
        "--tmpfs", "/tmp", "--tmpfs", "/var/tmp", "--tmpfs", "/run",
        "--bind", root, root,
        "--tmpfs", store, "--remount-ro", store,
        "--setenv", "TMPDIR", "/tmp",
        "--unsetenv", "DBUS_SESSION_BUS_ADDRESS",
        "--unsetenv", "SSH_AUTH_SOCK",
        "--chdir", root, "--seccomp", fd,
    };
    // systemd-resolved commonly keeps the resolver file under /run. Preserve that
    // single file after replacing /run so DNS still works without exposing its sockets.
    char resolver[PATH_MAX];
    bool have_resolver = ::realpath("/etc/resolv.conf", resolver) != nullptr;
    size_t n = 0;
    while (command[n]) ++n;
    constexpr size_t prefix = sizeof policy / sizeof policy[0];
    char **args = (char **)::calloc(prefix + n + 5, sizeof(char *));
    if (!args) { ::close(filter); return -ENOMEM; }
    for (size_t i = 0; i < prefix; ++i) args[i] = (char *)policy[i];
    size_t at = prefix;
    if (have_resolver) {
        args[at++] = (char *)"--ro-bind";
        args[at++] = resolver;
        args[at++] = resolver;
    }
    args[at++] = (char *)"--";
    for (size_t i = 0; i < n; ++i) args[at++] = command[i];
    ::execv(args[0], args);
    int e = errno;
    ::free(args);
    ::close(filter);
    return -e;
}
