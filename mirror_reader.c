/*
 * mirror_reader: the only process that reads any of the tracked source
 * folders listed in source_dir.conf (default: a single entry pointing at
 * Documents/antigravity). Copies every tracked .md file from each into its
 * own subfolder under ~/.knowledge-base-sync/mirror (outside the TCC-protected
 * Documents folder) so sync.py never needs Documents (or any other
 * protected-folder) access at all. Grant folder access to THIS compiled
 * binary only -- never to /usr/bin/python3 or /bin/bash, which are shared by
 * everything else on the machine.
 *
 * Each source folder is read from ~/.knowledge-base-sync/source_dir.conf (one
 * "label: path" per line -- path relative to $HOME unless it starts with
 * "/") at every run, not compiled in -- so adding, renaming, or removing a
 * tracked folder is a config edit, not a recompile. Each source is mirrored
 * to mirror/<label>/... so two sources can never collide on a shared
 * relative path. A source's path can point anywhere on disk -- e.g. a
 * mounted Google Drive folder under ~/Library/CloudStorage/... -- but that
 * likely needs a *different*, usually broader, TCC grant than Documents
 * Folder access (Google Drive mounts aren't Documents/Desktop/Downloads, so
 * macOS has no narrow per-folder toggle for them; expect to need Full Disk
 * Access for this binary instead). source_dir.conf itself lives outside any
 * protected folder, so reading it needs no TCC access.
 *
 * Safety: if a source's scan finds drastically fewer files than that
 * source's own subtree of the mirror already holds, refuse to touch that
 * source's mirror subtree -- other sources are unaffected and still sync
 * normally. A collapsed scan (e.g. TCC access not yet granted, a cloud
 * drive not currently mounted, or source_dir.conf pointing at a folder that
 * no longer exists) must never be read as "everything was deleted" -- that
 * exact failure mode deleted 334 files from the Onyx connector on
 * 2026-09-11. This is the
 * layer where that has to be caught, before sync.py ever sees it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <libgen.h>
#include <errno.h>
#include <ctype.h>

#define MAX_PATH_LEN 4096
#define MAX_FILES 20000
#define MAX_SOURCES 32
#define DEFAULT_SOURCE_REL "Documents/antigravity"
#define COLLAPSE_GUARD_MIN_TOTAL 10
#define COLLAPSE_GUARD_RATIO 2 /* refuse if scan finds under 1/this of what's tracked */

typedef struct {
    char label[64];       /* "" means mirror flat at mirror root (legacy/default) */
    char abs_path[MAX_PATH_LEN];
} Source;

static Source sources[MAX_SOURCES];
static int n_sources = 0;

static char root_dst_base[MAX_PATH_LEN];

static char src_files[MAX_FILES][MAX_PATH_LEN];
static int src_count = 0;

static char dst_files[MAX_FILES][MAX_PATH_LEN];
static int dst_count = 0;

static const char *EXCLUDE_DIRS[] = {
    "node_modules", ".venv", "venv", ".git", "__pycache__",
    ".next", "dist", "build", "site-packages", ".cache", NULL
};

static int is_excluded_dir(const char *name) {
    for (int i = 0; EXCLUDE_DIRS[i]; i++) {
        if (strcmp(name, EXCLUDE_DIRS[i]) == 0) return 1;
    }
    return 0;
}

static int has_suffix(const char *s, const char *suffix) {
    size_t ls = strlen(s), lsuf = strlen(suffix);
    return ls >= lsuf && strcmp(s + ls - lsuf, suffix) == 0;
}

/* case-insensitive check for "claude.md" basename */
static int is_claude_md(const char *basename) {
    if (strlen(basename) != 9) return 0; /* "claude.md" */
    char lower[16];
    for (int i = 0; i < 9; i++) lower[i] = (char)tolower((unsigned char)basename[i]);
    lower[9] = '\0';
    return strcmp(lower, "claude.md") == 0;
}

static void mkdirs(const char *path) {
    char tmp[MAX_PATH_LEN];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    size_t len = strlen(tmp);
    if (len == 0) return;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void scan_dir(const char *abs_dir, const char *rel_dir, char files[][MAX_PATH_LEN], int *count) {
    DIR *d = opendir(abs_dir);
    if (!d) return;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char abs_path[MAX_PATH_LEN];
        snprintf(abs_path, sizeof(abs_path), "%s/%s", abs_dir, entry->d_name);

        struct stat st;
        if (lstat(abs_path, &st) != 0) continue;

        char rel_path[MAX_PATH_LEN];
        if (rel_dir[0] == '\0') {
            snprintf(rel_path, sizeof(rel_path), "%s", entry->d_name);
        } else {
            snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_dir, entry->d_name);
        }

        if (S_ISDIR(st.st_mode)) {
            if (is_excluded_dir(entry->d_name)) continue;
            scan_dir(abs_path, rel_path, files, count);
        } else if (S_ISREG(st.st_mode)) {
            if (!has_suffix(entry->d_name, ".md")) continue;
            if (is_claude_md(entry->d_name)) continue;
            if (*count < MAX_FILES) {
                strncpy(files[*count], rel_path, MAX_PATH_LEN - 1);
                files[*count][MAX_PATH_LEN - 1] = '\0';
                (*count)++;
            }
        }
    }
    closedir(d);
}

static void trim(char *s) {
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    size_t start = 0;
    while (s[start] && isspace((unsigned char)s[start])) start++;
    if (start > 0) memmove(s, s + start, len - start + 1);
}

/* Label becomes a single mirror/ subdirectory name -- keep it to characters
 * that are always safe there and can't escape the mirror root. */
static int is_valid_label(const char *label) {
    if (label[0] == '\0') return 0;
    for (const char *p = label; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_')) return 0;
    }
    return 1;
}

static void resolve_path(char *out, size_t out_size, const char *home, const char *path) {
    if (path[0] == '/') {
        snprintf(out, out_size, "%s", path);
    } else {
        snprintf(out, out_size, "%s/%s", home, path);
    }
}

/* Reads ~/.knowledge-base-sync/source_dir.conf: one "label: path" per line (path
 * relative to $HOME unless it starts with "/"), blank lines and lines
 * starting with "#" ignored. A line with no ":" is treated as a bare path
 * with no label (mirrors flat at the mirror root -- legacy single-source
 * behavior). Falls back to a single DEFAULT_SOURCE_REL entry (also flat) if
 * the file is missing, empty, or contains no valid lines -- this file lives
 * outside any TCC-protected folder, so no special access is needed to read
 * it. */
static void load_sources(const char *home) {
    char conf_path[MAX_PATH_LEN];
    snprintf(conf_path, sizeof(conf_path), "%s/.knowledge-base-sync/source_dir.conf", home);

    FILE *f = fopen(conf_path, "r");
    if (f) {
        char line[MAX_PATH_LEN];
        while (fgets(line, sizeof(line), f)) {
            char *hash = strchr(line, '#');
            if (hash) *hash = '\0';
            trim(line);
            if (line[0] == '\0') continue;

            char label[64] = "";
            char *path = line;
            char *colon = strchr(line, ':');
            if (colon) {
                *colon = '\0';
                path = colon + 1;
                strncpy(label, line, sizeof(label) - 1);
                trim(label);
            }
            trim(path);
            if (path[0] == '\0') continue;

            if (label[0] != '\0' && !is_valid_label(label)) {
                fprintf(stderr, "Skipping source_dir.conf line: label '%s' must be alphanumeric/-/_ only\n", label);
                continue;
            }
            if (n_sources >= MAX_SOURCES) {
                fprintf(stderr, "Skipping source_dir.conf line: MAX_SOURCES (%d) reached\n", MAX_SOURCES);
                continue;
            }

            Source *s = &sources[n_sources++];
            strncpy(s->label, label, sizeof(s->label) - 1);
            resolve_path(s->abs_path, sizeof(s->abs_path), home, path);
        }
        fclose(f);
    }

    if (n_sources == 0) {
        Source *s = &sources[n_sources++];
        s->label[0] = '\0';
        resolve_path(s->abs_path, sizeof(s->abs_path), home, DEFAULT_SOURCE_REL);
    }
}

static int contains(char files[][MAX_PATH_LEN], int count, const char *needle) {
    for (int i = 0; i < count; i++) {
        if (strcmp(files[i], needle) == 0) return 1;
    }
    return 0;
}

static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    char dst_dir[MAX_PATH_LEN];
    strncpy(dst_dir, dst, sizeof(dst_dir) - 1);
    dst_dir[sizeof(dst_dir) - 1] = '\0';
    mkdirs(dirname(dst_dir));
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
    return 0;
}

/* Syncs one source into its mirror subtree. Returns 0 on success, 1 if the
 * collapse guard tripped (that source's mirror subtree was left untouched;
 * other sources are handled independently and are not affected). */
static int sync_one_source(const Source *s) {
    char root_dst[MAX_PATH_LEN];
    if (s->label[0] == '\0') {
        snprintf(root_dst, sizeof(root_dst), "%s", root_dst_base);
    } else {
        snprintf(root_dst, sizeof(root_dst), "%s/%s", root_dst_base, s->label);
    }
    mkdirs(root_dst);

    src_count = 0;
    dst_count = 0;
    scan_dir(s->abs_path, "", src_files, &src_count);
    scan_dir(root_dst, "", dst_files, &dst_count);

    printf("[%s] Source scan: %d files under %s\n",
           s->label[0] ? s->label : "(default)", src_count, s->abs_path);
    printf("[%s] Mirror currently holds: %d files\n",
           s->label[0] ? s->label : "(default)", dst_count);

    if (dst_count > COLLAPSE_GUARD_MIN_TOTAL && src_count < dst_count / COLLAPSE_GUARD_RATIO) {
        fprintf(stderr,
            "[%s] Refusing to update: source scan found only %d files but "
            "the mirror already holds %d. This looks like a broken scan "
            "(access not granted to this binary, or the source isn't "
            "currently mounted/reachable), not real deletions. Leaving "
            "this source's mirror subtree untouched.\n",
            s->label[0] ? s->label : "(default)", src_count, dst_count);
        return 1;
    }

    int copied = 0, removed = 0;
    for (int i = 0; i < src_count; i++) {
        char abs_src[MAX_PATH_LEN], abs_dst[MAX_PATH_LEN];
        snprintf(abs_src, sizeof(abs_src), "%s/%s", s->abs_path, src_files[i]);
        snprintf(abs_dst, sizeof(abs_dst), "%s/%s", root_dst, src_files[i]);

        struct stat st_src, st_dst;
        int need_copy = 1;
        if (stat(abs_dst, &st_dst) == 0 && stat(abs_src, &st_src) == 0) {
            if (st_dst.st_size == st_src.st_size && st_dst.st_mtime >= st_src.st_mtime) {
                need_copy = 0;
            }
        }
        if (need_copy) {
            if (copy_file(abs_src, abs_dst) == 0) copied++;
        }
    }

    for (int i = 0; i < dst_count; i++) {
        if (!contains(src_files, src_count, dst_files[i])) {
            char abs_dst[MAX_PATH_LEN];
            snprintf(abs_dst, sizeof(abs_dst), "%s/%s", root_dst, dst_files[i]);
            if (unlink(abs_dst) == 0) removed++;
        }
    }

    printf("[%s] Copied/updated: %d, removed: %d\n", s->label[0] ? s->label : "(default)", copied, removed);
    return 0;
}

int main(void) {
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "HOME not set\n");
        return 1;
    }
    snprintf(root_dst_base, sizeof(root_dst_base), "%s/.knowledge-base-sync/mirror", home);
    mkdirs(root_dst_base);

    load_sources(home);

    int any_guard_tripped = 0;
    for (int i = 0; i < n_sources; i++) {
        if (sync_one_source(&sources[i]) != 0) any_guard_tripped = 1;
    }
    return any_guard_tripped ? 1 : 0;
}
