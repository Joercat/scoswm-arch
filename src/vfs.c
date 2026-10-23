/*
 * SCos native - in-memory virtual file system.
 *
 * Mirrors the web simulation's localStorage filesystem: a tree of directories
 * and string files. On Linux it is optionally persisted to a snapshot file
 * (see platform_linux.c: fs_image_save/fs_image_load).
 */
#include "scos.h"

struct vfs_node *vfs_root;

static void node_free_recursive(struct vfs_node *n);

static struct vfs_node *node_new(const char *name, int is_dir, struct vfs_node *parent)
{
    struct vfs_node *n = palloc_owned(sizeof(struct vfs_node),HEAP_VFS);
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, VFS_NAME - 1);
    n->is_dir = is_dir;
    n->parent = parent;
    if (parent) {
        n->sibling = parent->child;
        parent->child = n;
    }
    return n;
}

static struct vfs_node *dir_child(struct vfs_node *dir, const char *name)
{
    for (struct vfs_node *c = dir->child; c; c = c->sibling)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}

int vfs_init_defaults(void)
{
    struct vfs_node *old=vfs_root;
    struct vfs_node *fresh=node_new("",1,NULL);
    if (!fresh) return 0;
    vfs_root=fresh;
    static const char welcome[] =
        "Welcome to SCos!\nThis is your personal computer system.";

    static const char changelog[] =
        "SCos x64 development Changelog:\n"
        "- Bare-metal x86 kernel: bootloader, drivers, window manager\n"
        "- GOP framebuffer desktop with mouse-driven window manager\n"
        "- PS/2 keyboard and mouse drivers, PIT clock, CMOS RTC\n"
        "- Terminal, Files, Notepad, Calendar, Settings, About apps\n"
        "- PCI/xHCI HID, verified ATA data saves, UEFI ACPI power";

    static const char settings[] = "{\n  \"theme\": \"matrix-1\",\n  \"version\": \"x64-dev\"\n}";

    static const char about[] =
        "SCos - bare-metal x86 operating system\n"
        "Version: unnumbered development\n"
        "Boots via x64 UEFI into a native 64-bit kernel.\n"
        "Type 'neofetch' in the Terminal for a live hardware report.";

    if (!vfs_mkdir("home") || !vfs_mkdir("home/documents") ||
        !vfs_mkdir("home/downloads") || !vfs_mkdir("home/desktop") ||
        !vfs_mkdir("system") ||
        !vfs_write("home/documents/welcome.txt",welcome,sizeof(welcome)-1) ||
        !vfs_write("home/documents/changelog.txt",changelog,sizeof(changelog)-1) ||
        !vfs_write("system/settings.json",settings,sizeof(settings)-1) ||
        !vfs_write("system/about.txt",about,sizeof(about)-1)) {
        vfs_root=old; node_free_recursive(fresh); return 0;
    }
    if (!vfs_mkdir("home/apps") || !vfs_mkdir("home/projects") || !vfs_mkdir("home/demos")) {
        vfs_root=old; node_free_recursive(fresh); return 0;
    }
    /* Linux port: Lua/CAT demo packages are not ported yet (PORTING.md),
     * so no .cat demos or Lua notices are seeded here. */
    if (old) node_free_recursive(old);
    return 1;
}

struct vfs_node *vfs_lookup(const char *path)
{
    if (!path) return NULL;
    struct vfs_node *cur = vfs_root;
    const char *p = path;
    while (*p == '/') p++;
    if (!*p) return vfs_root;
    char part[VFS_NAME];
    while (*p) {
        const char *slash = p;
        while (*slash && *slash != '/') slash++;
        int len = (int)(slash - p);
        if (len == 0) { p = slash + 1; continue; }
        if (len >= VFS_NAME) return NULL;
        strncpy(part, p, len);
        part[len] = 0;
        if (!strcmp(part, ".")) { /* stay */ }
        else if (!strcmp(part, "..")) { if (cur->parent) cur = cur->parent; }
        else {
            cur = dir_child(cur, part);
            if (!cur) return NULL;
        }
        p = slash;
        while (*p == '/') p++;
    }
    return cur;
}

int vfs_is_dir(struct vfs_node *n) { return n && n->is_dir; }

int vfs_list(struct vfs_node *dir, char names[][VFS_NAME], int max)
{
    int count = 0;
    if (!dir || !dir->is_dir) return 0;
    /* directories first, then files (creation order reversed internally) */
    for (int pass = 0; pass < 2; pass++)
        for (struct vfs_node *c = dir->child; c; c = c->sibling) {
            if ((pass == 0) != (c->is_dir != 0)) continue;
            if (count >= max) return count;
            strcpy(names[count++], c->name);
        }
    return count;
}

char *vfs_read(const char *path, u32 *len)
{
    struct vfs_node *n = vfs_lookup(path);
    if (!n || n->is_dir) return NULL;
    if (len) *len = n->size;
    return n->data;
}

static int node_set_data(struct vfs_node *n, const char *data, u32 len)
{
    if (len > 0xffffffffu-65 || (len && !data)) return 0;
    if (n->cap < len+1) {
        u32 cap=len+65;
        char *nd=palloc_owned(cap,HEAP_VFS);
        if (!nd) return 0;
        if (len) memcpy(nd,data,len); /* copy before releasing an aliased source */
        nd[len]=0;
        if (n->data) pfree(n->data,n->cap);
        n->data=nd; n->cap=cap;
    } else {
        if (len) memmove(n->data,data,len);
        n->data[len]=0;
    }
    n->size=len;
    return 1;
}

int vfs_write(const char *path, const char *data, u32 len)
{
    if (!path || strlen(path)>=256) return 0;
    char dirpath[256];
    vfs_parent_path(path, dirpath);
    struct vfs_node *dir = vfs_lookup(dirpath);
    if (!dir || !dir->is_dir) return 0;

    const char *base = path + strlen(path);
    while (base > path && *(base - 1) != '/') base--;
    if (!*base || strlen(base)>=VFS_NAME || !strcmp(base,".") || !strcmp(base,"..")) return 0;

    struct vfs_node *n = dir_child(dir, base);
    int created=!n;
    if (!n) n = node_new(base, 0, dir);
    if (!n || n->is_dir) return 0;
    if (node_set_data(n,data,len)) return 1;
    if (created) { dir->child=n->sibling; pfree(n,sizeof(*n)); }
    return 0;
}

int vfs_mkdir(const char *path)
{
    if (!path || strlen(path)>=256) return 0;
    char dirpath[256];
    vfs_parent_path(path, dirpath);
    struct vfs_node *dir = vfs_lookup(dirpath);
    if (!dir || !dir->is_dir) return 0;
    const char *base = path + strlen(path);
    while (base > path && *(base - 1) != '/') base--;
    if (!*base || strlen(base)>=VFS_NAME || !strcmp(base,".") || !strcmp(base,"..")) return 0;
    if (dir_child(dir, base)) return 0;
    return node_new(base, 1, dir) != NULL;
}

static void node_free_recursive(struct vfs_node *n)
{
    struct vfs_node *c = n->child;
    while (c) {
        struct vfs_node *next = c->sibling;
        node_free_recursive(c);
        c = next;
    }
    if (n->data) pfree(n->data, n->cap);
    pfree(n, sizeof(*n));
}

int vfs_delete(const char *path)
{
    struct vfs_node *n = vfs_lookup(path);
    if (!n || n == vfs_root || !n->parent) return 0;
    struct vfs_node *p = n->parent;
    struct vfs_node *prev = NULL;
    for (struct vfs_node *c = p->child; c; prev = c, c = c->sibling)
        if (c == n) break;
    if (prev) prev->sibling = n->sibling; else p->child = n->sibling;
    node_free_recursive(n);
    return 1;
}

u32 vfs_usage_bytes(void)
{
    u32 total = 0;
    /* walk iteratively */
    struct vfs_node *stack[64];
    int sp = 0;
    stack[sp++] = vfs_root;
    while (sp) {
        struct vfs_node *n = stack[--sp];
        total += n->size + (u32)strlen(n->name);
        for (struct vfs_node *c = n->child; c; c = c->sibling)
            if (sp < 64) stack[sp++] = c;
    }
    return total;
}

char *vfs_parent_path(const char *path, char *out)
{
    int len = (int)strlen(path);
    int i = len - 1;
    while (i >= 0 && path[i] == '/') i--;
    while (i >= 0 && path[i] != '/') i--;
    if (i < 0) { out[0] = '/'; out[1] = 0; return out; }
    strncpy(out, path, i + 1);
    out[i + 1] = 0;
    return out;
}

int vfs_rename(const char *oldp, const char *newp)
{
    if (!oldp || !newp || strlen(newp)>=256) return 0;
    struct vfs_node *n = vfs_lookup(oldp);
    if (!n || !n->parent) return 0;
    const char *slash = newp;
    for (const char *q = newp; *q; q++) if (*q == '/') slash = q + 1;
    if (!*slash || strlen(slash)>=VFS_NAME || !strcmp(slash,".") || !strcmp(slash,"..")) return 0;
    char parent[256];
    int pl = (int)(slash - newp);
    if (pl >= (int)sizeof(parent)) return 0;
    memcpy(parent, newp, pl);
    parent[pl] = 0;
    struct vfs_node *p = vfs_lookup(pl ? parent : "/");
    if (!p || !p->is_dir) return 0;
    if (dir_child(p, slash)) return 0;
    for (struct vfs_node *a=p;a;a=a->parent) if (a==n) return 0;
    struct vfs_node **l = &n->parent->child;
    while (*l && *l != n) l = &(*l)->sibling;
    if (*l) *l = n->sibling;
    n->sibling = p->child;
    p->child = n;
    n->parent = p;
    strncpy(n->name, slash, VFS_NAME - 1);
    n->name[VFS_NAME - 1] = 0;
    return 1;
}

/* wipe everything back to factory defaults (used by Settings) */
int vfs_factory_reset(void)
{
    return vfs_init_defaults();
}

struct vfs_node *vfs_child(struct vfs_node *dir, const char *name)
{
    return dir_child(dir, name);
}

/* RAM filesystem status; no obsolete BIOS-sector copies. */
void vfs_free_tree(struct vfs_node *n) { if (n) node_free_recursive(n); }


void system_files_init(int have_disk)
{
    /* Install missing native-Studio project templates, never overwrite edits.
     * Old .lua files remain data; the app loader now accepts only .cat. */
    if(!vfs_lookup("home/apps"))vfs_mkdir("home/apps");
    if(!vfs_lookup("home/demos"))vfs_mkdir("home/demos");
    if(!vfs_lookup("home/projects"))vfs_mkdir("home/projects");
    const char *info=have_disk ?
        "SCos window manager, Linux port.\n"
        "Snapshot persistence available: run 'save' in Terminal to write the\n"
        "whole RAM filesystem to the snapshot file named by fs_image_target.\n"
        "The snapshot is rewritten whole; it is not journaled.\n" :
        "SCos window manager, Linux port.\n"
        "No writable snapshot location. This filesystem is RAM-backed.\n"
        "Settings and documents are lost when scoswm exits.\n";
    vfs_write("/system/README.txt", info, strlen(info));
}
