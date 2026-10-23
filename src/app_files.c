/* SCos native - Files explorer app (toolbar, navigation, context menus) */
#include "scos.h"

struct files {
    char path[256];
    char names[64][VFS_NAME];
    int n;
    int hover_row;          /* -1 = none */
    int hover_btn;
    u8 synth[64];           /* 0 = real entry, 1 = app shortcut, 2 = pin */
    int notice;             /* 1 = show "hidden system dir" notice */
    u64 last_down_tick;     /* double-click detection */
    int last_down_row;
    char targets[64][192]; /* stable app IDs or pinned paths, never parsed labels */
    int scroll;             /* r36: first visible row (wheel scrolling) */

    /* Everything a test or another component might mirror by offset ends here.  Below this line the
     * layout is private to this file, so new state is appended, never inserted. */
    int press_row;          /* row a left button is currently held on, for dragging out */
    int press_x, press_y;
};

/* tools/tests/test_management.py reads a live Files window's state out of the guest by mirroring this
 * struct byte for byte, because that is the only way to drive a real double-click on the row it means
 * rather than on a row index the widget happens to like.  That makes the field order an interface, and a
 * field added in the middle would make the test parse nonsense instead of failing; so the contiguity it
 * assumes is asserted here, at build time, where the compiler knows the layout. */
#define FILES_OFF(field) __builtin_offsetof(struct files, field)
#define FILES_SIZE(field) sizeof(((struct files *)0)->field)
_Static_assert(FILES_OFF(n) == FILES_OFF(path) + FILES_SIZE(path) + FILES_SIZE(names),
               "files: path and names must stay packed at the head of the struct (test mirror)");
_Static_assert(FILES_OFF(synth) == FILES_OFF(n) + 3 * 4,
               "files: no padding before synth (test mirror)");
_Static_assert(FILES_OFF(notice) == FILES_OFF(synth) + FILES_SIZE(synth),
               "files: notice must immediately follow synth (test mirror)");
_Static_assert(FILES_OFF(targets) == FILES_OFF(last_down_row) + 4,
               "files: targets must immediately follow last_down_row (test mirror)");
_Static_assert(FILES_OFF(scroll) == FILES_OFF(targets) + FILES_SIZE(targets),
               "files: scroll must immediately follow targets (test mirror)");

#define TB_Y 8
#define TB_H 26
#define ROW_H 24
#define LIST_Y 44

static int fr_in(int px, int py, int x, int y, int w, int h)
{
    return px >= x && py >= y && px < x + w && py < y + h;
}

static u32 fr_mix(u32 a, u32 b, int t)
{
    u32 ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    u32 br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    return (((ar * (255 - t) + br * t) / 255) << 16) |
           (((ag * (255 - t) + bg * t) / 255) << 8) |
           (((ab * (255 - t) + bb * t) / 255));
}

static void files_load(struct files *f)
{
    for (int i = 0; i < 64; i++) f->synth[i] = 0;
    f->notice = 0;f->last_down_row=-1;f->last_down_tick=0;f->hover_row=-1;
    memset(f->targets,0,sizeof(f->targets));
    f->scroll = 0;              /* r36: new directory starts at the top */
    /* Keep system files terminal-only, including aliases through ../. */
    struct vfs_node *dir = vfs_lookup(f->path);
    struct vfs_node *system=vfs_lookup("/system");
    for(struct vfs_node *n=dir;n;n=n->parent)if(n==system){f->notice=1;f->n=0;return;}
    if(!dir||!dir->is_dir){f->notice=2;f->n=0;return;}
    f->n = vfs_list(dir, f->names, 64);
    /* the desktop folder mirrors the desktop: app shortcuts + pins */
    if (dir == vfs_lookup("home/desktop")) {
        int cnt = wm_desk_vis_count();
        for (int i = 0; i < cnt && f->n < 64; i++) {
            char app[32], path[192], label[40];
            int kind = 0;
            if (!wm_desk_vis_get(i, app, path, label, &kind)) continue;
            strncpy(f->names[f->n],label,VFS_NAME-1);f->names[f->n][VFS_NAME-1]=0;
            strncpy(f->targets[f->n],kind==0?app:path,191);
            f->synth[f->n]=(u8)(kind+1);
            f->n++;
        }
    }
}

static void files_open(struct window *w, void *arg)
{
    struct files *f = palloc(sizeof(struct files));
    if (!f) return;              /* OOM: wm_open_app reports it centrally */
    memset(f, 0, sizeof(*f));
    if (arg) strncpy(f->path, (char *)arg, sizeof(f->path) - 1);
    else strcpy(f->path, "/");
    if(!f->path[0])strcpy(f->path,"/");
    f->hover_row = -1;
    f->hover_btn = -1;
    files_load(f);
    w->data = f;
    wm_track_mem(w, (int)sizeof(*f));
    {
        char lg[260];
        strcpy(lg, "browsing ");
        strncat(lg, f->path, 240);
        app_log(w, lg);
    }
}

static void files_close(struct window *w)
{
    if (w->data) pfree(w->data, sizeof(struct files));
    w->data = NULL;
}

/* The last component of a path, for a drag label: the pinned rows carry full paths, and a shadow that
 * reads "/home/apps/counter.cat" is unreadable next to the cursor. */
static const char *files_base_name(const char *path)
{
    const char *base = path;
    for (const char *q = path; *q; q++) if (*q == '/') base = q + 1;
    return base;
}

static int files_is_dir_row(struct files *f, int row)
{
    struct vfs_node *dir = vfs_lookup(f->path);
    if (!dir) return 0;
    for (struct vfs_node *c = dir->child; c; c = c->sibling)
        if (!strcmp(c->name, f->names[row])) return c->is_dir;
    return 0;
}

static int files_full_path(struct files *f, int row, char *out)
{
    if(row<0||row>=f->n||strlen(f->path)+strlen(f->names[row])+2>=256)return 0;
    strcpy(out, f->path);
    if (out[strlen(out) - 1] != '/') strcat(out, "/");
    strcat(out, f->names[row]);
    return 1;
}

static void files_open_row(struct window *w,int row){
    struct files *f=w->data;if(row<0||row>=f->n)return;
    if(f->synth[row]==1){if(!wm_open_app(f->targets[row],NULL))wm_notify("Application unavailable","This shortcut no longer resolves to an installed application.",1);return;}
    if(f->synth[row]==2){struct vfs_node *n=vfs_lookup(f->targets[row]);if(n&&n->is_dir)wm_open_app("files",f->targets[row]);else if(n)app_open_document(f->targets[row]);else wm_notify("Missing file","The pinned file no longer exists.",1);return;}
    char full[256];if(!files_full_path(f,row,full)){wm_notify("Files","Path too long.",1);return;}
    if(files_is_dir_row(f,row)){strcpy(f->path,full);files_load(f);wm_redraw(w);}else app_open_document(full);
}

/* ------------------------------------------------------- context menus --- */
/* A row's menu is the set of things that can be done to *this* entry, so a directory and a file do not
 * share one list: nothing opens a folder, and a folder can still be named, pinned and deleted. */
static const char *menu_file_items[] = { "Open", "Open with...", "Rename...", "Pin to Desktop", "Delete" };
static const char *menu_dir_items[] = { "Open", "Rename...", "Pin to Desktop", "Delete" };
static const char *menu_shortcut_items[] = { "Open" };
static const char *menu_bg_items[] = { "New Folder", "Refresh" };

struct fm_ctx { struct window *w; int row,id,dir; };

static struct window *files_dialog_win;
static int files_dialog_id;

static void newfolder_cb(int ok, const char *text, void *ud)
{
    (void)ud;
    if (!ok || !text || !files_dialog_win || !files_dialog_win->data || files_dialog_win->id!=files_dialog_id) return;
    struct files *f = files_dialog_win->data;
    if(f->notice){wm_error_popup("/system is terminal-only.");return;}
    char full[300];
    strcpy(full, f->path);
    if (full[strlen(full) - 1] != '/') strcat(full, "/");
    if(strlen(full)+strlen(text)>=sizeof(full)||!text[0]){wm_error_popup("Folder path too long or empty.");return;}
    strcat(full, text);
    if(!vfs_mkdir(full)){wm_error_popup("Cannot create folder: path, permissions or memory.");return;}
    {
        char lg[320];
        strcpy(lg, "created folder ");
        strncat(lg, full, 300);
        app_log(files_dialog_win, lg);
    }
    files_load(f);
    wm_redraw(files_dialog_win);
}

/* What a row's menu can do is decided by the entry, not by the file manager's mood: a document goes to
 * another application, a name gets changed in place, and a deletion still belongs to `rm -s` in the
 * terminal, because an explorer that silently unlinks files is how data is lost.  Each branch reads the
 * label rather than the index, so the two lists (file, directory) cannot disagree with the code. */
static char rename_dir[256], rename_old[128], rename_ask[192];
static char openwith_path[256];
static int openwith_count;
static char openwith_ids[16][32];
static const char *openwith_labels[16];
static char openwith_text[16][40];

static void files_rename_cb(int ok, const char *text, void *ud)
{
    (void)ud;
    struct window *w = files_dialog_win;
    if (!ok || !text || !text[0] || !w || !w->data || w->id != files_dialog_id) return;
    struct files *f = w->data;
    if (f->notice) { wm_error_popup("/system is terminal-only."); return; }
    char to[300];
    if ((int)strlen(text) > 120) { wm_error_popup("That name is too long."); return; }
    for (const char *q = text; *q; q++)
        if (*q == '/') { wm_error_popup("Names cannot contain a slash."); return; }
    strcpy(to, rename_dir);
    if (to[strlen(to) - 1] != '/') strcat(to, "/");
    strcat(to, text);
    char from[300];
    strcpy(from, rename_dir);
    if (from[strlen(from) - 1] != '/') strcat(from, "/");
    strcat(from, rename_old);
    if (vfs_lookup(to)) { wm_error_popup("That name is already taken here."); return; }
    if (vfs_rename(from, to)) {
        char log[340];
        strcpy(log, "renamed ");
        strncat(log, from, 150);
        strncat(log, " to ", 8);
        strncat(log, to, 150);
        app_log(w, log);
    } else wm_error_popup("Rename failed: the name is in use or the entry is protected.");
    strcpy(f->path, rename_dir);
    files_load(f);
    wm_redraw(w);
}

static void openwith_cb(int item, void *ud)
{
    struct fm_ctx *ctx = ud;
    if (item >= 0) {
        /* The window is only needed to repaint the list afterwards; a document that opens elsewhere is
         * not this file manager's business. */
        struct window *w = ctx && ctx->w && ctx->w->id == ctx->id ? ctx->w : 0;
        const char *id = openwith_ids[item];
        if (id && id[0] && !wm_open_app(id, openwith_path) && w)
            wm_notify("Cannot open it there", "That application did not take the file.", 1);
        if (w) wm_redraw(w);
    }
    pfree(ctx, sizeof(*ctx));
}

static void file_menu_cb(int item, void *ud)
{
    struct fm_ctx *ctx = ud;
    struct window *w = ctx->w;
    if(item<0||w->id!=ctx->id||!w->data){pfree(ctx,sizeof(*ctx));return;}
    struct files *f = w->data;
    const char *label = ctx->dir ? menu_dir_items[item] : menu_file_items[item];
    char full[300];
    if (strcmp(label, "Open")) {
        if (!files_full_path(f, ctx->row, full)) {
            pfree(ctx, sizeof(*ctx));
            wm_error_popup("Path too long.");
            return;
        }
        if (files_is_dir_row(f, ctx->row)) strcat(full, "/");
    }
    if (!strcmp(label, "Open")) {
        files_open_row(w, ctx->row);
        pfree(ctx, sizeof(*ctx));
    } else if (!strcmp(label, "Pin to Desktop")) {
        wm_desktop_pin_file(full);
        pfree(ctx, sizeof(*ctx));
    } else if (!strcmp(label, "Rename...")) {
        strcpy(rename_dir, f->path);
        strcpy(rename_old, f->names[ctx->row]);
        files_dialog_win = w;
        files_dialog_id = w->id;
        /* The current name goes in the question, not in the field: this dialog has no text selection, so
         * pre-filling it would make the user delete the old name by hand before typing the new one. */
        strcpy(rename_ask, "New name for \"");
        strncat(rename_ask, f->names[ctx->row], sizeof(rename_ask) - strlen(rename_ask) - 3);
        strcat(rename_ask, "\":");
        /* An empty field, not a missing one: wm_dialog only builds the text input when `input` is a real
         * string, and a NULL there turns this into a yes/no question with nowhere to type. */
        wm_dialog("Rename", rename_ask, "", files_rename_cb, 0);
        pfree(ctx, sizeof(*ctx));
    } else if (!strcmp(label, "Open with...")) {
        /* One line per application that can take a document.  The list is built here, from the registry,
         * because which apps accept a path is a property of the app set, not of this row. */
        openwith_count = 0;
        for (int i = 0; i < app_count() && openwith_count < 16; i++) {
            struct app *a = app_at(i);
            if (!a || a->id[0] == '_' || (!a->document && !a->open)) continue;
            strcpy(openwith_ids[openwith_count], a->id);
            strncpy(openwith_text[openwith_count], a->title, sizeof(openwith_text[0]) - 1);
            openwith_text[openwith_count][sizeof(openwith_text[0]) - 1] = 0;
            openwith_labels[openwith_count] = openwith_text[openwith_count];
            openwith_count++;
        }
        struct fm_ctx *nc = palloc(sizeof(*nc));
        if (!nc) { pfree(ctx, sizeof(*ctx)); wm_error_popup("No memory for the menu."); return; }
        nc->w = w; nc->id = w->id; nc->row = -1; nc->dir = 0;
        strcpy(openwith_path, full);
        pfree(ctx, sizeof(*ctx));
        if (!openwith_count) { wm_notify("Nothing else can open this", "No installed application "
                                       "accepts documents.", 1); return; }
        wm_menu(mx_abs(w, 40), 60, openwith_labels, openwith_count, openwith_cb, nc);
    } else {
        pfree(ctx,sizeof(*ctx));
        wm_dialog("Files","Delete files with rm -s in the terminal.",NULL,NULL,NULL);
    }
    wm_redraw(w);
}

static void bg_menu_cb(int item, void *ud)
{
    struct fm_ctx *ctx = ud;
    struct window *w=ctx->w;
    if(item<0||w->id!=ctx->id||!w->data){pfree(ctx,sizeof(*ctx));return;}
    struct files *f = w->data;
    if (item == 0) {
        files_dialog_win = ctx->w;files_dialog_id=ctx->w->id;
        wm_dialog("New Folder", "Enter folder name:", "new folder", newfolder_cb, NULL);
    } else {
        files_load(f);
    }
    pfree(ctx, sizeof(*ctx));
    wm_redraw(w);
}

/* -------------------------------------------------------------- paint ---- */
/* One move, from whichever folder the pointer was over.  Renaming into a directory is the whole of
 * it - the VFS refuses a collision and refuses a folder moved inside itself, so nothing here has to
 * second-guess what it is handed. */
static int files_move_into(struct window *w, const char *dir, const char *src)
{
    struct files *f = w->data;
    const char *base = files_base_name(src);
    char from[300], dst[300];
    if (!f || !base[0]) return 0;
    if ((int)strlen(dir) + (int)strlen(base) + 2 >= (int)sizeof dst) {
        wm_error_popup("The new path is too long to move that file.");
        return 1;
    }
    strcpy(from, src);            /* the drop's payload belongs to the drag, not to us */
    strcpy(dst, dir);
    int dl = (int)strlen(dst);
    if (dl && dst[dl - 1] != '/') { dst[dl] = '/'; dst[dl + 1] = 0; }
    strcat(dst, base);
    if (!strcmp(from, dst)) return 1;      /* it is already sitting in that folder */
    if (vfs_rename(from, dst)) files_load(f);
    else wm_notify("Move failed", "That file could not be moved into the folder - a name may already "
                   "be taken there.", 1);
    wm_redraw(w);
    return 1;
}

static int files_drop(struct window *w, const char *path, int x, int y)
{
    struct files *f = w->data;
    if (!f || f->notice || !path || !path[0]) return 0;
    if (x < 4 || x > w->surf.w - 4 || y < LIST_Y + 4) return 0;   /* toolbar: not ours */
    if (!vfs_lookup(path)) return 0;                              /* it vanished mid-drag */
    int row = f->scroll + (y - LIST_Y - 4) / ROW_H;
    if (row < 0) return 0;
    /* Below the last row is the folder being shown, so a drop there files the thing here. */
    if (row >= f->n) return files_move_into(w, f->path, path);
    if (f->synth[row] || !files_is_dir_row(f, row)) return 0;      /* a file is not a container */
    char dir[300];
    if (!files_full_path(f, row, dir)) return 0;
    return files_move_into(w, dir, path);
}

static void files_paint(struct window *w)
{
    struct files *f = w->data;
    struct surface *s = &w->surf;
    const struct theme *t = theme_current();
    s_fill(s, 0, 0, s->w, s->h, t->win_bg);


    /* toolbar buttons: back, up, refresh, new folder */
    for (int b = 0; b < 4; b++) {
        int bx = 8 + b * 34;
        u32 bg = f->hover_btn == b ? t->main : color_blend(t->win_bg,t->main,15);
        u32 fg = f->hover_btn == b ? t->title_text : t->main;
        s_fill(s, bx, TB_Y, 30, TB_H, bg);
        s_frame_rect(s, bx, TB_Y, 30, TB_H, t->main);
        if (b == 0) {
            s_line(s, bx + 9, TB_Y + 13, bx + 21, TB_Y + 13, fg);
            s_line(s, bx + 9, TB_Y + 13, bx + 14, TB_Y + 8, fg);
            s_line(s, bx + 9, TB_Y + 13, bx + 14, TB_Y + 18, fg);
        } else if (b == 1) {
            s_line(s, bx + 15, TB_Y + 7, bx + 15, TB_Y + 19, fg);
            s_line(s, bx + 15, TB_Y + 7, bx + 10, TB_Y + 12, fg);
            s_line(s, bx + 15, TB_Y + 7, bx + 20, TB_Y + 12, fg);
        } else if (b == 2) {
            s_circle(s, bx + 15, TB_Y + 13, 6, fg);
            s_fill(s, bx + 16, TB_Y + 4, 8, 4, bg);
            s_line(s, bx + 15, TB_Y + 7, bx + 22, TB_Y + 8, fg);
        } else {
            s_fill(s, bx + 7, TB_Y + 8, 7, 3, fg);
            s_frame_rect(s, bx + 7, TB_Y + 10, 16, 10, fg);
        }
    }
    /* path box */
    int px = 8 + 4 * 34 + 6;
    s_fill(s, px, TB_Y, s->w - px - 8, TB_H, color_blend(t->win_bg,t->main,15));
    s_frame_rect(s, px, TB_Y, s->w - px - 8, TB_H, t->main);
    s_clip_text(s, px + 6, TB_Y + 5, f->path, t->main, s->w - px - 20);

    if(f->notice){s_clip_text(s,12,LIST_Y+10,f->notice==1?"/system is terminal-only.":"Directory unavailable. Go up or refresh.",t->text,s->w-24);return;}
    /* entries (r36: scrollable - rows past the clip used to be
     * unreachable: no wheel handler, no offset, silently invisible) */
    int rows = (s->h - LIST_Y - 6) / ROW_H;
    int maxs = f->n - rows;
    if (maxs < 0) maxs = 0;
    if (f->scroll > maxs) f->scroll = maxs;
    if (f->scroll < 0) f->scroll = 0;
    for (int i = f->scroll; i < f->n && i < f->scroll + rows; i++) {
        int y = LIST_Y + 4 + (i - f->scroll) * ROW_H;
        if (i == f->hover_row)
            s_fill(s, 4, y, s->w - 8, ROW_H, fr_mix(t->win_bg, t->main, 18));
        if (files_is_dir_row(f, i)) {
            s_fill(s, 12, y + 6, 6, 3, t->main);
            s_frame_rect(s, 12, y + 8, 14, 9, t->main);
        } else {
            s_frame_rect(s, 13, y + 4, 11, 15, t->main);
            s_fill(s, 15, y + 8, 7, 1, t->main);
            s_fill(s, 15, y + 11, 7, 1, t->main);
        }
        if (f->synth[i])                     /* shortcut badge */
            s_fill(s, 21, y + 15, 5, 5, t->main);
        s_clip_text(s,34,y+4,f->names[i],t->text,s->w-126);
        s_clip_text(s,s->w-84,y+4,f->synth[i]==1?"App":f->synth[i]==2?"Shortcut":files_is_dir_row(f,i)?"Folder":"File",t->main,76);
    }
    if (!f->n)
        s_text(s, 12, LIST_Y + 10, "(empty directory)", t->text);
}

/* -------------------------------------------------------------- input ---- */
static void files_mouse(struct window *w, struct mouse_event *e, int x, int y)
{
    struct files *f = w->data;
    /* r36: wheel scrolls the entry list (3 rows per notch) */
    if (e->type == MEV_WHEEL) {
        int rows = (w->surf.h - LIST_Y - 6) / ROW_H;
        int maxs = f->n - rows;
        if (maxs < 0) maxs = 0;
        int ns = f->scroll - (int)e->wheel * 3;
        if (ns < 0) ns = 0;
        if (ns > maxs) ns = maxs;
        if (ns != f->scroll) { f->scroll = ns; wm_redraw(w); }
        return;
    }
    int old_btn = f->hover_btn, old_row = f->hover_row;
    f->hover_btn = -1;
    f->hover_row = -1;
    if (y < LIST_Y) {
        for (int b = 0; b < 4; b++)
            if (fr_in(x, y, 8 + b * 34, TB_Y, 30, TB_H)) f->hover_btn = b;
    } else if(x>=4&&x<w->surf.w-4&&y>=LIST_Y+4&&!f->notice) {
        int rows = (w->surf.h - LIST_Y - 6) / ROW_H;
        int row = f->scroll + (y - LIST_Y - 4) / ROW_H;
        /* r36: hover must ignore rows below the visible clip (the old
         * hit test selected INVISIBLE rows whenever n > rows) */
        if (row >= f->scroll && row < f->n && row < f->scroll + rows)
            f->hover_row = row;
    }
    if (f->hover_btn != old_btn || f->hover_row != old_row) wm_redraw(w);

    /* Dragging a row out of the list is how a file reaches the desktop, another window, or a taskbar
     * entry.  The row is remembered on the press and only becomes a drag past six pixels of movement -
     * the WM uses the same distance for its own icon drags, so a click cannot turn into a pick-up by
     * accident, and a pick-up cannot lose the click. */
    if (e->type == MEV_MOVE) {
        if (f->press_row >= 0 && f->press_row < f->n && (x - f->press_x > 6 || x - f->press_x < -6 ||
                                                         y - f->press_y > 6 || y - f->press_y < -6)) {
            int row = f->press_row;
            char full[300];
            f->press_row = -1;
            if (!f->synth[row] && files_full_path(f, row, full))
                wm_dnd_begin(DND_FILE, full, f->names[row]);
            else if (f->synth[row] == 2)
                wm_dnd_begin(DND_FILE, f->targets[row], files_base_name(f->targets[row]));
        }
        return;
    }
    if (e->type != MEV_BUTTON || !e->down) {
        if (e->type == MEV_BUTTON) f->press_row = -1;
        return;
    }

    if (e->button == MBTN_RIGHT) {

        struct fm_ctx *ctx = palloc(sizeof(*ctx));
        if(!ctx){err_notify("files","Not enough memory for menu.",NULL,0);return;}
        ctx->id=w->id;ctx->w = w;
        ctx->row = f->hover_row;
        if (f->hover_row >= 0) {
            ctx->dir = files_is_dir_row(f, f->hover_row);
            /* A synthesised row is an application shortcut or a pinned path, and the only thing to do
             * with it here is open it: it is not a file in this folder, so "rename" would lie. */
            if (f->synth[f->hover_row])
                wm_menu(mx_abs(w, x), my_abs(w, y), menu_shortcut_items, 1, file_menu_cb, ctx);
            else
                wm_menu(mx_abs(w, x), my_abs(w, y), ctx->dir ? menu_dir_items : menu_file_items,
                        ctx->dir ? 4 : 5, file_menu_cb, ctx);
        } else
            wm_menu(mx_abs(w, x), my_abs(w, y), menu_bg_items, 2, bg_menu_cb, ctx);
        return;
    }
    if (e->button != MBTN_LEFT) return;
    f->press_row = f->hover_row;
    f->press_x = x;
    f->press_y = y;

    if (f->hover_btn >= 0) {
        switch (f->hover_btn) {
        case 0:
        case 1: {
            char tmp[256];
            strcpy(tmp, f->path);
            int len = (int)strlen(tmp);
            if (len && tmp[len - 1] == '/') tmp[len - 1] = 0;
            char *slash = NULL;
            for (char *p = tmp; *p; p++) if (*p == '/') slash = p;
            if (slash) { slash[1] = 0; strcpy(f->path, tmp); }
            else strcpy(f->path, "/");
            files_load(f);
            break;
        }
        case 2:
            files_load(f);
            break;
        case 3:
            files_dialog_win = w;files_dialog_id=w->id;
            wm_dialog("New Folder", "Enter folder name:", "new folder", newfolder_cb, NULL);
            break;
        }
        wm_redraw(w);
        return;
    }
    if (f->hover_row >= 0) {
        int row = f->hover_row;
        u64 now = tick_count;
        int dbl = (row == f->last_down_row &&
                   (now - f->last_down_tick) <= (u64)prefs_get()->dbl_ms / 10);
        f->last_down_tick = now;
        f->last_down_row = row;
        if (!dbl) { wm_redraw(w); return; }   /* first click selects */
        files_open_row(w,row);
        wm_redraw(w);
    }
}

static void files_key(struct window *w, struct key_event *e)
{
    if(e->pressed&&e->keycode==KEY_F5){files_load(w->data);wm_redraw(w);}
}

struct app app_files = {
    .desktop_label = "Files",
    .uses_data = 1, .id = "files", .title = "File Explorer", .icon = ICON_FOLDER, .single = 0,
    .drop = files_drop,
    .def_w = 700, .def_h = 500,
    .open = files_open, .paint = files_paint, .key = files_key,
    .mouse = files_mouse, .close = files_close,
};

SCOS_APP(app_files, 000);
