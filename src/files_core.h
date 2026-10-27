/* POSIX filesystem operations for the standalone SCos Files application. */
#ifndef SCOS_FILES_CORE_H
#define SCOS_FILES_CORE_H

#include <stddef.h>

#define FILES_PATH_MAX 4096

struct file_entry {
    char *name;
    unsigned long long size;
    int is_dir;
};

struct file_list {
    struct file_entry *items;
    int count;
};

void files_list_free(struct file_list *list);
int files_list_directory(const char *path, int show_hidden, struct file_list *out,
                         char *error, size_t error_cap);
int files_join_path(const char *base, const char *name, char *out, size_t cap);
int files_parent_path(const char *path, char *out, size_t cap);
int files_create_folder(const char *base, const char *name, char *error, size_t error_cap);
int files_rename_entry(const char *base, const char *old_name, const char *new_name,
                       char *error, size_t error_cap);

#endif
