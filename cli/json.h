// Small JSON writer for the CLI; no additional runtime dependencies.
#pragma once
#include "worldfs/worldfs.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static void json_string(FILE *out, const char *value) {
    fputc('"', out);
    const unsigned char *p = (const unsigned char *)value;
    while (*p) {
        unsigned char c = *p++;
        if (c == '"' || c == '\\') { fputc('\\', out); fputc(c, out); }
        else if (c < 0x20) fprintf(out, "\\u%04x", c);
        else if (c < 0x80) fputc(c, out);
        else {
            // Preserve UTF-8; represent undecodable filesystem bytes with surrogateescape.
            int n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2
                                                        : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
            bool valid = n != 0;
            for (int i = 0; valid && i < n; ++i)
                if (p[i] < 0x80 || p[i] > 0xbf) valid = false;
            if (valid && ((c == 0xe0 && p[0] < 0xa0) || (c == 0xed && p[0] >= 0xa0) ||
                          (c == 0xf0 && p[0] < 0x90) || (c == 0xf4 && p[0] >= 0x90))) valid = false;
            if (valid) { fputc(c, out); fwrite(p, 1, (size_t)n, out); p += n; }
            else fprintf(out, "\\udc%02x", c);
        }
    }
    fputc('"', out);
}

struct Json {
    FILE *out;
    bool first = true;
    explicit Json(FILE *f) : out(f) { fputc('{', out); }
    ~Json() { fputc('}', out); }
    void key(const char *name) {
        if (!first) fputc(',', out);
        first = false;
        json_string(out, name); fputc(':', out);
    }
    void str(const char *name, const char *value) { key(name); json_string(out, value); }
    void num(const char *name, uint64_t value) { key(name); fprintf(out, "%llu", (unsigned long long)value); }
    void signed_num(const char *name, int64_t value) { key(name); fprintf(out, "%lld", (long long)value); }
    void boolean(const char *name, bool value) { key(name); fputs(value ? "true" : "false", out); }
    void ref(const char *name, char kind, wfs_id id) {
        key(name);
        if (id) fprintf(out, "\"%c%llu\"", kind, (unsigned long long)id);
        else fputs("null", out);
    }
};

static const char *json_state(int state) {
    switch (state) {
    case WFS_ST_CREATING: return "creating";
    case WFS_ST_ACTIVE: return "active";
    case WFS_ST_TRASHED: return "trashed";
    case WFS_ST_DEAD: return "dead";
    case WFS_ST_TRASHING: return "trashing";
    default: return "unknown";
    }
}

static void json_snapshot(FILE *out, const wfs_snapshot_rec &v) {
    Json j(out);
    j.num("schema_version", 1); j.str("kind", "snapshot"); j.ref("id", 'S', v.id);
    j.str("name", v.name); j.str("path", v.path); j.str("src_path", v.src_path);
    j.ref("from_world", 'W', v.from_world); j.str("state", json_state(v.state));
    j.signed_num("created_at", v.created_at); j.signed_num("trashed_at", v.trashed_at);
    j.num("entries", v.entries); j.num("hardlinks", v.hardlinks);
    j.num("hl_groups", v.hl_groups); j.num("hl_external", v.hl_external);
    j.num("root_mode", v.root_mode); j.str("protection", v.hard ? "hard" : "gate");
}

static void json_world(FILE *out, const wfs_world_rec &v) {
    Json j(out);
    j.num("schema_version", 1); j.str("kind", "world"); j.ref("id", 'W', v.id);
    j.str("name", v.name); j.str("path", v.path); j.str("state", json_state(v.state));
    j.ref("parent_world", 'W', v.parent_world); j.ref("snapshot_id", 'S', v.snapshot_id);
    j.str("origin", v.origin == WFS_O_SNAPSHOT ? "snapshot" :
                    v.origin == WFS_O_WORLD ? "world" : v.origin == WFS_O_ADOPTED ? "adopted" : "unknown");
    j.signed_num("created_at", v.created_at); j.signed_num("trashed_at", v.trashed_at);
    j.num("entries", v.entries); j.num("dir_dev", v.dir_dev); j.num("dir_ino", v.dir_ino);
    j.num("fsevents_id", v.fsevents_id); j.boolean("present", v.present);
}

static int json_list(wfs_store *s, wfs_snapshot_rec *v, size_t cap, size_t *n) {
    return wfs_snapshot_list(s, v, cap, n);
}
static int json_list(wfs_store *s, wfs_world_rec *v, size_t cap, size_t *n) {
    return wfs_world_list(s, 1, v, cap, n);
}

static int json_list(wfs_store *s, wfs_pool_stat *v, size_t cap, size_t *n) {
    return wfs_pool_status(s, v, cap, n);
}

static void json_trash(FILE *out, const wfs_trash_stat &ts) {
    Json j(out);
    j.num("schema_version", 1);
#define TRASH(field) j.num(#field, ts.field)
    TRASH(entries); TRASH(deleting); TRASH(due); TRASH(worlds); TRASH(snapshots);
    TRASH(tree_entries); TRASH(bytes_estimate); TRASH(volume_free_bytes); TRASH(volume_total_bytes);
    TRASH(creating_stranded); TRASH(pool_stranded); TRASH(dirs_unreadable);
    TRASH(dirs_unreadable_errno); TRASH(trash_blocked); TRASH(trash_foreign);
    TRASH(worker_done); TRASH(worker_remaining);
#undef TRASH
    j.signed_num("worker_pid", ts.worker_pid); j.signed_num("worker_started_at", ts.worker_started_at);
    j.str("dirs_unreadable_path", ts.dirs_unreadable_path);
    j.str("blocked_path", ts.blocked_path); j.str("foreign_path", ts.foreign_path);
}

// The core reports the total count even when the buffer is too small. Retry boundedly if
// another process grows the list between calls; never read past capacity or silently truncate.
template <class T> static int json_read_list(wfs_store *s, T **out, size_t *n) {
    *out = NULL;
    int rc = json_list(s, (T *)NULL, 0, n);
    if (rc) return rc;
    for (int attempt = 0; attempt < 8; ++attempt) {
        size_t cap = *n;
        if (!cap) return 0;
        if (cap > SIZE_MAX / sizeof(T)) return -ENOMEM;
        T *v = (T *)calloc(cap, sizeof(T));
        if (!v) return -ENOMEM;
        rc = json_list(s, v, cap, n);
        if (!rc && *n <= cap) { *out = v; return 0; }
        free(v);
        if (rc) return rc;
    }
    return -EAGAIN;
}
