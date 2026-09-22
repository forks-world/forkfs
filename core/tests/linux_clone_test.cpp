// Linux backend regression: a restrictive umask must not make cloned directories unusable.
#include "internal.h"

#include <atomic>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

extern "C" ssize_t __real_lgetxattr(const char *, const char *, void *, size_t);
extern "C" ssize_t __real_llistxattr(const char *, char *, size_t);
extern "C" int __real_lsetxattr(const char *, const char *, const void *, size_t, int);

namespace {
constexpr const char *kFakeXattr = "security.selinux";
const char *g_fake_src = nullptr;
const char *g_fake_dst = nullptr;
const unsigned char *g_fake_src_value = nullptr;
size_t g_fake_src_len = 0;
const unsigned char *g_fake_dst_value = nullptr;
size_t g_fake_dst_len = 0;
bool g_fake_enabled = false;
bool g_fake_dst_erange = false;
std::atomic<int> g_fake_set_calls{0};

bool fake_path(const char *path, const char *expected) {
    return g_fake_enabled && expected && !strcmp(path, expected);
}

ssize_t fake_get(const unsigned char *value, size_t len, void *out, size_t size, bool erange) {
    if (!out) return (ssize_t)len;
    if (erange || size < len) {
        errno = ERANGE;
        return -1;
    }
    if (len) memcpy(out, value, len);
    return (ssize_t)len;
}
} // namespace

extern "C" ssize_t __wrap_lgetxattr(const char *path, const char *name, void *value, size_t size) {
    if (!strcmp(name, kFakeXattr) && fake_path(path, g_fake_src))
        return fake_get(g_fake_src_value, g_fake_src_len, value, size, false);
    if (!strcmp(name, kFakeXattr) && fake_path(path, g_fake_dst))
        return fake_get(g_fake_dst_value, g_fake_dst_len, value, size, g_fake_dst_erange);
    return __real_lgetxattr(path, name, value, size);
}

extern "C" ssize_t __wrap_llistxattr(const char *path, char *list, size_t size) {
    if (!fake_path(path, g_fake_src)) return __real_llistxattr(path, list, size);
    const size_t len = strlen(kFakeXattr) + 1;
    if (!list) return (ssize_t)len;
    if (size < len) {
        errno = ERANGE;
        return -1;
    }
    memcpy(list, kFakeXattr, len);
    return (ssize_t)len;
}

extern "C" int __wrap_lsetxattr(const char *path, const char *name, const void *value,
                                 size_t size, int flags) {
    if (!strcmp(name, kFakeXattr) && fake_path(path, g_fake_dst)) {
        (void)value;
        (void)size;
        (void)flags;
        g_fake_set_calls.fetch_add(1, std::memory_order_relaxed);
        errno = EPERM;
        return -1;
    }
    return __real_lsetxattr(path, name, value, size, flags);
}

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s (%s)\n", __FILE__, __LINE__, #x, strerror(errno)); \
        exit(1); \
    } \
} while (0)
#define CHECK_OK(x) do { \
    int _rc = (x); \
    if (_rc != 0) { \
        fprintf(stderr, "%s:%d: %s -> %d (%s)\n", __FILE__, __LINE__, #x, _rc, wfs_strerror(_rc)); \
        exit(1); \
    } \
} while (0)

static void join(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s/%s", a, b);
    CHECK(n > 0 && (size_t)n < cap);
}

static mode_t mode_of(const char *path) {
    struct stat st;
    CHECK(lstat(path, &st) == 0);
    return st.st_mode & 07777;
}

static void write_file(const char *path, const char *text, mode_t mode) {
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(fd >= 0);
    size_t len = strlen(text);
    CHECK(write(fd, text, len) == (ssize_t)len);
    CHECK(close(fd) == 0);
    CHECK(chmod(path, mode) == 0);
}

static void check_file(const char *path, const char *text) {
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    char got[256];
    size_t len = strlen(text);
    CHECK(len < sizeof got);
    CHECK(read(fd, got, sizeof got) == (ssize_t)len);
    CHECK(memcmp(got, text, len) == 0);
    CHECK(read(fd, got, 1) == 0);
    CHECK(close(fd) == 0);
}

static void remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        CHECK(errno == ENOENT);
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        CHECK(unlink(path) == 0);
        return;
    }
    CHECK(chmod(path, 0700) == 0);
    DIR *dir = opendir(path);
    CHECK(dir != nullptr);
    while (struct dirent *entry = readdir(dir)) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[4096];
        join(child, sizeof child, path, entry->d_name);
        remove_tree(child);
    }
    CHECK(closedir(dir) == 0);
    CHECK(rmdir(path) == 0);
}

static void xattr_case(const unsigned char *src_value, size_t src_len,
                       const unsigned char *dst_value, size_t dst_len,
                       int expected_rc, int expected_set_calls, bool destination_erange) {
    char root[] = "/tmp/wfs-linux-xattr.XXXXXX";
    CHECK(mkdtemp(root) != nullptr);
    char src[4096], dst[4096];
    join(src, sizeof src, root, "source");
    join(dst, sizeof dst, root, "clone");
    CHECK(mkdir(src, 0755) == 0);

    g_fake_src = src;
    g_fake_dst = dst;
    g_fake_src_value = src_value;
    g_fake_src_len = src_len;
    g_fake_dst_value = dst_value;
    g_fake_dst_len = dst_len;
    g_fake_dst_erange = destination_erange;
    g_fake_set_calls.store(0, std::memory_order_relaxed);
    g_fake_enabled = true;
    int rc = wfs::fs_clone_tree(src, dst, true);
    g_fake_enabled = false;

    CHECK(rc == expected_rc);
    CHECK(g_fake_set_calls.load(std::memory_order_relaxed) == expected_set_calls);
    remove_tree(root);
}

int main() {
    char root[] = "/tmp/wfs-linux-clone.XXXXXX";
    CHECK(mkdtemp(root) != nullptr);

    char src[4096], dst[4096], nested[4096], deep[4096], dst_nested[4096], dst_deep[4096];
    char src_file[4096], src_leaf[4096], dst_file[4096], dst_leaf[4096];
    join(src, sizeof src, root, "source");
    join(dst, sizeof dst, root, "clone");
    join(nested, sizeof nested, src, "nested");
    join(deep, sizeof deep, nested, "deep");
    join(dst_nested, sizeof dst_nested, dst, "nested");
    join(dst_deep, sizeof dst_deep, dst_nested, "deep");
    join(src_file, sizeof src_file, nested, "file.txt");
    join(src_leaf, sizeof src_leaf, deep, "leaf.txt");
    join(dst_file, sizeof dst_file, dst, "nested/file.txt");
    join(dst_leaf, sizeof dst_leaf, dst, "nested/deep/leaf.txt");

    CHECK(mkdir(src, 0755) == 0);
    CHECK(mkdir(nested, 0750) == 0);
    CHECK(mkdir(deep, 0710) == 0);
    CHECK(chmod(src, 0755) == 0);
    CHECK(chmod(nested, 0750) == 0);
    CHECK(chmod(deep, 0710) == 0);
    write_file(src_file, "nested contents\n", 0640);
    write_file(src_leaf, "deep contents\n", 0604);

    mode_t before = umask(0777);
    int rc = wfs::fs_clone_tree(src, dst, true);
    CHECK(umask(before) == 0777);
    CHECK_OK(rc);

    CHECK(mode_of(src) == 0755);
    CHECK(mode_of(nested) == 0750);
    CHECK(mode_of(deep) == 0710);
    CHECK(mode_of(dst) == 0755);
    CHECK(mode_of(dst_nested) == 0750);
    CHECK(mode_of(dst_deep) == 0710);
    check_file(src_file, "nested contents\n");
    check_file(src_leaf, "deep contents\n");
    check_file(dst_file, "nested contents\n");
    check_file(dst_leaf, "deep contents\n");

    // A protected label may be inherited by the destination and reject writes. Matching bytes
    // must skip the setter; differing values and an unreadable value must remain errors.
    xattr_case((const unsigned char *)"label", 5, (const unsigned char *)"label", 5, 0, 0, false);
    xattr_case((const unsigned char *)"label", 5, (const unsigned char *)"other", 5, -EPERM, 1, false);
    xattr_case((const unsigned char *)"label", 5, (const unsigned char *)"different", 9, -EPERM, 1, false);
    xattr_case(nullptr, 0, nullptr, 0, 0, 0, false);
    xattr_case((const unsigned char *)"label", 5, (const unsigned char *)"label", 5, -ERANGE, 0, true);

    remove_tree(root);
    puts("linux_clone_test: restrictive umask and protected xattr clone: PASS");
    return 0;
}
