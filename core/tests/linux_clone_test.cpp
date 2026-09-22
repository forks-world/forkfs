// Linux backend regression: a restrictive umask must not make cloned directories unusable.
#include "internal.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

    remove_tree(root);
    puts("linux_clone_test: restrictive umask clone: PASS");
    return 0;
}
