#define _POSIX_C_SOURCE 200809L
#include "files_core.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void set_error(char *error, size_t cap, const char *operation, const char *detail)
{
    if (!error || !cap) return;
    snprintf(error, cap, "%s: %s", operation, detail ? detail : "failed");
}

void files_list_free(struct file_list *list)
{
    if (!list) return;
    for (int i = 0; i < list->count; i++) free(list->items[i].name);
    free(list->items);
    list->items = NULL; list->count = 0;
}

static int entry_compare(const void *left, const void *right)
{
    const struct file_entry *a = left, *b = right;
    if (a->is_dir != b->is_dir) return b->is_dir - a->is_dir;
    int folded = strcasecmp(a->name, b->name);
    return folded ? folded : strcmp(a->name, b->name);
}

int files_join_path(const char *base, const char *name, char *out, size_t cap)
{
    if (!base || !*base || !name || !*name || !out || !cap) return 0;
    size_t bn = strlen(base), nn = strlen(name);
    int slash = base[bn - 1] != '/';
    if (bn + (size_t)slash + nn + 1 > cap) return 0;
    memcpy(out, base, bn);
    if (slash) out[bn++] = '/';
    memcpy(out + bn, name, nn + 1);
    return 1;
}

int files_parent_path(const char *path, char *out, size_t cap)
{
    if (!path || !*path || !out || !cap || strlen(path) >= cap) return 0;
    snprintf(out, cap, "%s", path);
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = 0;
    if (n == 1 && out[0] == '/') return 1;
    char *slash = strrchr(out, '/');
    if (!slash) return 0;
    if (slash == out) out[1] = 0;
    else *slash = 0;
    return 1;
}

int files_list_directory(const char *path, int show_hidden, struct file_list *out,
                         char *error, size_t error_cap)
{
    if (!out) return 0;
    out->items = NULL; out->count = 0;
    DIR *dir = opendir(path);
    if (!dir) { set_error(error, error_cap, path, strerror(errno)); return 0; }
    int cap = 0;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || (!show_hidden && name[0] == '.'))
            continue;
        if (out->count == cap) {
            int next = cap ? cap * 2 : 32;
            void *p = realloc(out->items, (size_t)next * sizeof(*out->items));
            if (!p) {
                closedir(dir); files_list_free(out);
                set_error(error, error_cap, "Out of memory", NULL); return 0;
            }
            out->items = p; cap = next;
        }
        struct file_entry *item = &out->items[out->count];
        memset(item, 0, sizeof(*item));
        item->name = strdup(name);
        if (!item->name) {
            closedir(dir); files_list_free(out);
            set_error(error, error_cap, "Out of memory", NULL); return 0;
        }
        char full[FILES_PATH_MAX];
        struct stat st;
        if (files_join_path(path, name, full, sizeof(full)) && stat(full, &st) == 0) {
            item->is_dir = S_ISDIR(st.st_mode);
            item->size = S_ISREG(st.st_mode) ? (unsigned long long)st.st_size : 0;
        }
        out->count++;
        errno = 0;
    }
    int read_error = errno;
    closedir(dir);
    if (read_error) {
        files_list_free(out);
        set_error(error, error_cap, path, strerror(read_error)); return 0;
    }
    qsort(out->items, (size_t)out->count, sizeof(*out->items), entry_compare);
    return 1;
}

static int valid_new_name(const char *name)
{
    return name && *name && strcmp(name, ".") && strcmp(name, "..") && !strchr(name, '/');
}

int files_create_folder(const char *base, const char *name, char *error, size_t error_cap)
{
    if (!valid_new_name(name)) {
        set_error(error, error_cap, "Invalid folder name", NULL); return 0;
    }
    char path[FILES_PATH_MAX];
    if (!files_join_path(base, name, path, sizeof(path))) {
        set_error(error, error_cap, "Folder path is too long", NULL); return 0;
    }
    if (mkdir(path, 0777) != 0) {
        set_error(error, error_cap, "Cannot create folder", strerror(errno)); return 0;
    }
    return 1;
}

int files_rename_entry(const char *base, const char *old_name, const char *new_name,
                       char *error, size_t error_cap)
{
    if (!valid_new_name(old_name) || !valid_new_name(new_name)) {
        set_error(error, error_cap, "Invalid file name", NULL); return 0;
    }
    char from[FILES_PATH_MAX], to[FILES_PATH_MAX];
    if (!files_join_path(base, old_name, from, sizeof(from)) ||
        !files_join_path(base, new_name, to, sizeof(to))) {
        set_error(error, error_cap, "File path is too long", NULL); return 0;
    }
    struct stat st;
    if (lstat(to, &st) == 0) {
        set_error(error, error_cap, "That name is already in use", NULL); return 0;
    }
    if (errno != ENOENT) {
        set_error(error, error_cap, "Cannot check destination", strerror(errno)); return 0;
    }
    if (rename(from, to) != 0) {
        set_error(error, error_cap, "Rename failed", strerror(errno)); return 0;
    }
    return 1;
}
