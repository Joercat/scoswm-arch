#define _POSIX_C_SOURCE 200809L
#include "files_core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; \
} } while (0)

static void touch(const char *path)
{
    FILE *f = fopen(path, "wb"); assert(f);
    fputs("abc", f); fclose(f);
}

int main(void)
{
    char dir[] = "/tmp/scos-files-test-XXXXXX";
    assert(mkdtemp(dir));
    char sub[512], file[512], hidden[512], error[256];
    snprintf(sub, sizeof(sub), "%s/Folder", dir);
    snprintf(file, sizeof(file), "%s/document.txt", dir);
    snprintf(hidden, sizeof(hidden), "%s/.secret", dir);
    assert(mkdir(sub, 0755) == 0); touch(file); touch(hidden);

    struct file_list list;
    CHECK(files_list_directory(dir, 0, &list, error, sizeof(error)));
    CHECK(list.count == 2);
    CHECK(list.items[0].is_dir && !strcmp(list.items[0].name, "Folder"));
    CHECK(!list.items[1].is_dir && !strcmp(list.items[1].name, "document.txt"));
    CHECK(list.items[1].size == 3);
    files_list_free(&list);
    CHECK(files_list_directory(dir, 1, &list, error, sizeof(error)));
    CHECK(list.count == 3);
    files_list_free(&list);

    char path[512];
    CHECK(files_join_path(dir, "Folder", path, sizeof(path)));
    CHECK(!strcmp(path, sub));
    CHECK(files_parent_path(sub, path, sizeof(path)) && !strcmp(path, dir));
    CHECK(files_parent_path("/", path, sizeof(path)) && !strcmp(path, "/"));
    CHECK(!files_join_path(dir, "too-long", path, 8));

    CHECK(files_create_folder(dir, "new folder", error, sizeof(error)));
    CHECK(files_list_directory(dir, 0, &list, error, sizeof(error)));
    CHECK(list.count == 3 && list.items[0].is_dir && list.items[1].is_dir);
    files_list_free(&list);
    CHECK(!files_create_folder(dir, "bad/name", error, sizeof(error)));

    CHECK(files_rename_entry(dir, "document.txt", "renamed.txt", error, sizeof(error)));
    snprintf(file, sizeof(file), "%s/renamed.txt", dir);
    CHECK(access(file, F_OK) == 0);
    CHECK(!files_rename_entry(dir, "renamed.txt", "Folder", error, sizeof(error)));
    CHECK(strstr(error, "already") != NULL);

    CHECK(files_list_directory(dir, 0, &list, error, sizeof(error)));
    int found = 0;
    for (int i = 0; i < list.count; i++) if (!strcmp(list.items[i].name, "renamed.txt")) found = 1;
    CHECK(found);
    files_list_free(&list);
    CHECK(!files_list_directory("/definitely/not/a/scos-folder", 0, &list, error, sizeof(error)));

    /* The temporary directory is intentionally small enough to clean without
     * invoking a shell or external file manager. */
    char nested[512];
    snprintf(nested, sizeof(nested), "%s/new folder", dir); rmdir(nested);
    snprintf(nested, sizeof(nested), "%s/.secret", dir); unlink(nested);
    unlink(file); rmdir(sub); rmdir(dir);
    if (failures) printf("test_files: %d failures\n", failures);
    else printf("test_files: all checks passed\n");
    return failures ? 1 : 0;
}
