#define _GNU_SOURCE
/* scos-files: the original SCos Files surface as a standalone Linux app.
 * Real POSIX directories and files; no external file-manager process. */
#include "files_core.h"
#include "util.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "0.5.7"
#endif
#define TB_Y 8
#define TB_H 26
#define LIST_Y 44
#define ROW_H 24
#define STATUS_H 24

static Display *dpy;
static int scr, running = 1, need_redraw = 1;
static Window win;
static GC gc;
static Pixmap back;
static XFontStruct *font;
static int win_w = 700, win_h = 500;
static struct scos_theme theme;
static unsigned long main_px, bg_px, text_px, dark_px, hover_px, taskbar_px;
static Atom a_protocols, a_delete, a_net_name, a_utf8, a_pid;

static char cwd[FILES_PATH_MAX];
static struct file_list listing;
static int show_hidden, scroll, selected = -1, hover_row = -1, hover_btn = -1;
static char status_text[512] = "Ready";
static char history[32][FILES_PATH_MAX];
static int history_n;
static long last_click_ms;
static int last_click_row = -1;

static int menu_visible, menu_x, menu_y, menu_w = 190, menu_n, menu_row = -1;
static int menu_context_row = -1;
static const char *menu_labels[4];

enum { DIALOG_NONE, DIALOG_NEW_FOLDER, DIALOG_RENAME, DIALOG_PATH, DIALOG_MESSAGE };
static int dialog_kind;
static char dialog_title[64], dialog_input[FILES_PATH_MAX], dialog_target[256];

static long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static unsigned long alloc_rgb(uint32_t rgb)
{
    XColor c;
    memset(&c, 0, sizeof(c));
    c.red = (unsigned short)(((rgb >> 16) & 255) * 257);
    c.green = (unsigned short)(((rgb >> 8) & 255) * 257);
    c.blue = (unsigned short)((rgb & 255) * 257);
    c.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(dpy, DefaultColormap(dpy, scr), &c)) return BlackPixel(dpy, scr);
    return c.pixel;
}

static void fill(Drawable d, int x, int y, int w, int h, unsigned long px)
{
    XSetForeground(dpy, gc, px);
    XFillRectangle(dpy, d, gc, x, y, w, h);
}

static void outline(Drawable d, int x, int y, int w, int h, unsigned long px)
{
    XSetForeground(dpy, gc, px);
    XDrawRectangle(dpy, d, gc, x, y, w - 1, h - 1);
}

static void text_out(Drawable d, int x, int y, const char *s, unsigned long fg)
{
    XSetForeground(dpy, gc, fg);
    XDrawString(dpy, d, gc, x, y, s, (int)strlen(s));
}

static void clipped_text(Drawable d, int x, int y, const char *s, int max_w,
                         unsigned long fg)
{
    if (max_w <= 0) return;
    char latin[1024], buf[1024];
    utf8_to_latin1(s ? s : "", latin, sizeof(latin));
    snprintf(buf, sizeof(buf), "%s", latin);
    if (XTextWidth(font, buf, (int)strlen(buf)) > max_w) {
        int fitted = 0;
        for (int len = (int)strlen(buf) - 1; len >= 0; len--) {
            buf[len] = 0;
            char candidate[1030];
            int n = snprintf(candidate, sizeof(candidate), "%s...", buf);
            if (n >= 0 && n < (int)sizeof(candidate) && XTextWidth(font, candidate, n) <= max_w) {
                text_out(d, x, y, candidate, fg); fitted = 1; break;
            }
        }
        if (!fitted) return;
    } else text_out(d, x, y, buf, fg);
}

static int visible_rows(void)
{
    int rows = (win_h - LIST_Y - STATUS_H - 6) / ROW_H;
    return rows < 1 ? 1 : rows;
}

static void clamp_scroll(void)
{
    int maximum = listing.count - visible_rows();
    if (maximum < 0) maximum = 0;
    if (scroll > maximum) scroll = maximum;
    if (scroll < 0) scroll = 0;
}

static void update_title(void)
{
    const char *base = strrchr(cwd, '/');
    base = base && base[1] ? base + 1 : cwd;
    char title[512], latin[512];
    snprintf(title, sizeof(title), "SCos Files - %.498s", base);
    utf8_to_latin1(title, latin, sizeof(latin));
    XStoreName(dpy, win, latin);
    XChangeProperty(dpy, win, a_net_name, a_utf8, 8, PropModeReplace,
                    (unsigned char *)title, (int)strlen(title));
}

static int load_directory(const char *path, int remember)
{
    char resolved[FILES_PATH_MAX];
    if (!realpath(path, resolved)) {
        snprintf(status_text, sizeof(status_text), "%s: %s", path, strerror(errno));
        need_redraw = 1; return 0;
    }
    struct file_list next;
    char error[256];
    if (!files_list_directory(resolved, show_hidden, &next, error, sizeof(error))) {
        snprintf(status_text, sizeof(status_text), "%s", error);
        need_redraw = 1; return 0;
    }
    if (remember && cwd[0] && strcmp(cwd, resolved)) {
        if (history_n < 32) {
            snprintf(history[history_n], sizeof(history[0]), "%s", cwd);
            history_n++;
        }
    }
    files_list_free(&listing); listing = next;
    snprintf(cwd, sizeof(cwd), "%s", resolved);
    selected = hover_row = last_click_row = -1; scroll = 0;
    snprintf(status_text, sizeof(status_text), "%d entries", listing.count);
    update_title(); need_redraw = 1;
    return 1;
}

static void refresh_directory(void)
{
    char path[FILES_PATH_MAX];
    snprintf(path, sizeof(path), "%s", cwd);
    load_directory(path, 0);
}

static void go_back(void)
{
    if (history_n <= 0) { snprintf(status_text, sizeof(status_text), "No previous folder"); need_redraw = 1; return; }
    char path[FILES_PATH_MAX];
    snprintf(path, sizeof(path), "%s", history[--history_n]);
    if (!load_directory(path, 0)) history_n++;
}

static void go_up(void)
{
    char parent[FILES_PATH_MAX];
    if (files_parent_path(cwd, parent, sizeof(parent))) load_directory(parent, 1);
}

static void spawn_document(const char *path)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        close(ConnectionNumber(dpy));
        if (fork() == 0) {
            execlp("xdg-open", "xdg-open", path, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid < 0) snprintf(status_text, sizeof(status_text), "Cannot launch document opener: %s", strerror(errno));
    else snprintf(status_text, sizeof(status_text), "Opening %.503s", path);
    need_redraw = 1;
}

static void open_row(int row)
{
    if (row < 0 || row >= listing.count) return;
    char full[FILES_PATH_MAX];
    if (!files_join_path(cwd, listing.items[row].name, full, sizeof(full))) {
        snprintf(status_text, sizeof(status_text), "Path is too long"); need_redraw = 1; return;
    }
    if (listing.items[row].is_dir) load_directory(full, 1);
    else spawn_document(full);
}

static void dialog_open(int kind, const char *title, const char *input, const char *target)
{
    dialog_kind = kind;
    snprintf(dialog_title, sizeof(dialog_title), "%s", title);
    snprintf(dialog_input, sizeof(dialog_input), "%s", input ? input : "");
    snprintf(dialog_target, sizeof(dialog_target), "%s", target ? target : "");
    menu_visible = 0; need_redraw = 1;
}

static void dialog_accept(void)
{
    char error[256];
    int ok = 1;
    if (dialog_kind == DIALOG_NEW_FOLDER) {
        ok = files_create_folder(cwd, dialog_input, error, sizeof(error));
        if (ok) { snprintf(status_text, sizeof(status_text), "Created %.503s", dialog_input); refresh_directory(); }
    } else if (dialog_kind == DIALOG_RENAME) {
        ok = files_rename_entry(cwd, dialog_target, dialog_input, error, sizeof(error));
        if (ok) { snprintf(status_text, sizeof(status_text), "Renamed %.240s to %.240s", dialog_target, dialog_input); refresh_directory(); }
    } else if (dialog_kind == DIALOG_PATH) {
        ok = load_directory(dialog_input, 1);
    }
    if (!ok && dialog_kind != DIALOG_PATH) snprintf(status_text, sizeof(status_text), "%s", error);
    dialog_kind = DIALOG_NONE; need_redraw = 1;
}

static void draw_toolbar_button(int b)
{
    int x = 8 + b * 34;
    int hover = hover_btn == b;
    fill(back, x, TB_Y, 30, TB_H, hover ? main_px : hover_px);
    outline(back, x, TB_Y, 30, TB_H, main_px);
    unsigned long fg = hover ? dark_px : main_px;
    XSetForeground(dpy, gc, fg);
    if (b == 0) {
        XDrawLine(dpy, back, gc, x + 9, TB_Y + 13, x + 21, TB_Y + 13);
        XDrawLine(dpy, back, gc, x + 9, TB_Y + 13, x + 14, TB_Y + 8);
        XDrawLine(dpy, back, gc, x + 9, TB_Y + 13, x + 14, TB_Y + 18);
    } else if (b == 1) {
        XDrawLine(dpy, back, gc, x + 15, TB_Y + 7, x + 15, TB_Y + 19);
        XDrawLine(dpy, back, gc, x + 15, TB_Y + 7, x + 10, TB_Y + 12);
        XDrawLine(dpy, back, gc, x + 15, TB_Y + 7, x + 20, TB_Y + 12);
    } else if (b == 2) {
        XDrawArc(dpy, back, gc, x + 9, TB_Y + 7, 13, 13, 35 * 64, 285 * 64);
        XDrawLine(dpy, back, gc, x + 20, TB_Y + 7, x + 22, TB_Y + 12);
    } else {
        fill(back, x + 7, TB_Y + 8, 7, 3, fg);
        outline(back, x + 7, TB_Y + 10, 16, 10, fg);
    }
}

static void draw_dialog(void)
{
    if (!dialog_kind) return;
    int w = 420, h = dialog_kind == DIALOG_MESSAGE ? 112 : 142;
    int x = (win_w - w) / 2, y = (win_h - h) / 2;
    fill(back, x, y, w, h, bg_px); outline(back, x, y, w, h, main_px);
    clipped_text(back, x + 14, y + 23, dialog_title, w - 28, main_px);
    if (dialog_kind != DIALOG_MESSAGE) {
        fill(back, x + 14, y + 38, w - 28, 28, taskbar_px);
        outline(back, x + 14, y + 38, w - 28, 28, main_px);
        clipped_text(back, x + 21, y + 57, dialog_input, w - 42, text_px);
    } else {
        clipped_text(back, x + 14, y + 54,
                     "For safety, delete files explicitly from a terminal.", w - 28, text_px);
    }
    int by = y + h - 34;
    fill(back, x + w - 184, by, 80, 25, main_px); outline(back, x + w - 184, by, 80, 25, main_px);
    text_out(back, x + w - 158, by + 17, "OK", dark_px);
    outline(back, x + w - 94, by, 80, 25, main_px);
    text_out(back, x + w - 76, by + 17, "Cancel", main_px);
}

static void draw(void)
{
    if (!back) return;
    fill(back, 0, 0, win_w, win_h, bg_px);
    for (int b = 0; b < 4; b++) draw_toolbar_button(b);
    int px = 8 + 4 * 34 + 6;
    fill(back, px, TB_Y, win_w - px - 8, TB_H, hover_px);
    outline(back, px, TB_Y, win_w - px - 8, TB_H, main_px);
    clipped_text(back, px + 6, TB_Y + 18, cwd, win_w - px - 20, main_px);

    clamp_scroll();
    int rows = visible_rows();
    for (int row = scroll; row < listing.count && row < scroll + rows; row++) {
        int y = LIST_Y + 4 + (row - scroll) * ROW_H;
        int is_selected = row == selected, is_hover = row == hover_row;
        if (is_selected) fill(back, 4, y, win_w - 8, ROW_H, main_px);
        else if (is_hover) fill(back, 4, y, win_w - 8, ROW_H, hover_px);
        unsigned long fg = is_selected ? dark_px : text_px;
        unsigned long accent = is_selected ? dark_px : main_px;
        struct file_entry *item = &listing.items[row];
        if (item->is_dir) {
            fill(back, 12, y + 6, 6, 3, accent);
            outline(back, 12, y + 8, 14, 9, accent);
        } else {
            outline(back, 13, y + 4, 11, 15, accent);
            fill(back, 15, y + 8, 7, 1, accent);
            fill(back, 15, y + 11, 7, 1, accent);
        }
        clipped_text(back, 34, y + 17, item->name, win_w - 142, fg);
        clipped_text(back, win_w - 92, y + 17, item->is_dir ? "Folder" : "File", 80, accent);
    }
    if (!listing.count) text_out(back, 12, LIST_Y + 22, "(empty directory)", text_px);
    fill(back, 0, win_h - STATUS_H, win_w, STATUS_H, taskbar_px);
    XDrawLine(dpy, back, gc, 0, win_h - STATUS_H, win_w, win_h - STATUS_H);
    clipped_text(back, 10, win_h - 7, status_text, win_w - 20, text_px);

    if (menu_visible) {
        int h = menu_n * 24 + 6;
        fill(back, menu_x, menu_y, menu_w, h, bg_px);
        outline(back, menu_x, menu_y, menu_w, h, main_px);
        for (int i = 0; i < menu_n; i++) {
            int y = menu_y + 3 + i * 24;
            if (i == menu_row) fill(back, menu_x + 1, y, menu_w - 2, 24, main_px);
            clipped_text(back, menu_x + 10, y + 17, menu_labels[i], menu_w - 20,
                         i == menu_row ? dark_px : text_px);
        }
    }
    draw_dialog();
    XCopyArea(dpy, back, win, gc, 0, 0, win_w, win_h, 0, 0);
    need_redraw = 0;
}

static void resize_window(int w, int h)
{
    if (w < 400) w = 400;
    if (h < 240) h = 240;
    if (w == win_w && h == win_h && back) return;
    Pixmap p = XCreatePixmap(dpy, win, w, h, DefaultDepth(dpy, scr));
    if (!p) return;
    if (back) XFreePixmap(dpy, back);
    back = p; win_w = w; win_h = h;
    clamp_scroll(); need_redraw = 1;
}

static int row_at(int y)
{
    int row = scroll + (y - LIST_Y - 4) / ROW_H;
    if (y < LIST_Y + 4 || row < scroll || row >= listing.count || row >= scroll + visible_rows()) return -1;
    return row;
}

static void open_menu(int x, int y, int row)
{
    menu_visible = 1; menu_row = -1; menu_context_row = row;
    if (row >= 0) {
        menu_labels[0] = "Open"; menu_labels[1] = "Rename...";
        menu_labels[2] = "Delete (terminal only)"; menu_n = 3;
        selected = row;
    } else {
        menu_labels[0] = "New Folder";
        menu_labels[1] = show_hidden ? "Hide dotfiles" : "Show hidden files";
        menu_labels[2] = "Refresh"; menu_n = 3;
    }
    int h = menu_n * 24 + 6;
    menu_x = x; menu_y = y;
    if (menu_x + menu_w > win_w - 2) menu_x = win_w - menu_w - 2;
    if (menu_y + h > win_h - STATUS_H - 2) menu_y = win_h - STATUS_H - h - 2;
    if (menu_x < 2) menu_x = 2;
    if (menu_y < 2) menu_y = 2;
    need_redraw = 1;
}

static void menu_action(int index)
{
    int row = menu_context_row;
    if (row >= 0 && row < listing.count) {
        if (index == 0) open_row(row);
        else if (index == 1) dialog_open(DIALOG_RENAME, "New name:", "", listing.items[row].name);
        else if (index == 2) dialog_open(DIALOG_MESSAGE, "Delete files with rm in SCos Terminal", NULL, NULL);
    } else {
        if (index == 0) dialog_open(DIALOG_NEW_FOLDER, "New folder name:", "new folder", NULL);
        else if (index == 1) { show_hidden = !show_hidden; refresh_directory(); }
        else if (index == 2) refresh_directory();
    }
    menu_visible = 0; need_redraw = 1;
}

static void handle_key(XKeyEvent *event)
{
    KeySym sym = XLookupKeysym(event, 0);
    if (dialog_kind) {
        if (sym == XK_Escape) { dialog_kind = DIALOG_NONE; need_redraw = 1; return; }
        if (sym == XK_Return || sym == XK_KP_Enter) { dialog_accept(); return; }
        if (dialog_kind == DIALOG_MESSAGE) return;
        if (sym == XK_BackSpace) {
            size_t n = strlen(dialog_input); if (n) dialog_input[n - 1] = 0;
            need_redraw = 1; return;
        }
        if ((event->state & ControlMask) && (sym == XK_u || sym == XK_U)) {
            dialog_input[0] = 0; need_redraw = 1; return;
        }
        char buf[128];
        int n = XLookupString(event, buf, sizeof(buf), NULL, NULL);
        if (n > 0) {
            size_t used = strlen(dialog_input);
            if (used + (size_t)n < sizeof(dialog_input)) {
                memcpy(dialog_input + used, buf, (size_t)n);
                dialog_input[used + (size_t)n] = 0;
            }
            need_redraw = 1;
        }
        return;
    }
    if (menu_visible) {
        if (sym == XK_Escape) { menu_visible = 0; need_redraw = 1; }
        return;
    }
    if (sym == XK_F5) { refresh_directory(); return; }
    if (sym == XK_BackSpace || ((event->state & Mod1Mask) && sym == XK_Up)) { go_up(); return; }
    if ((event->state & Mod1Mask) && sym == XK_Left) { go_back(); return; }
    if ((event->state & ControlMask) && (sym == XK_h || sym == XK_H)) {
        show_hidden = !show_hidden; refresh_directory(); return;
    }
    if ((event->state & ControlMask) && (sym == XK_l || sym == XK_L)) {
        dialog_open(DIALOG_PATH, "Open folder path:", cwd, NULL); return;
    }
    if (sym == XK_Up || sym == XK_Down) {
        if (listing.count) {
            selected += sym == XK_Up ? -1 : 1;
            if (selected < 0) selected = 0;
            if (selected >= listing.count) selected = listing.count - 1;
            if (selected < scroll) scroll = selected;
            if (selected >= scroll + visible_rows()) scroll = selected - visible_rows() + 1;
            need_redraw = 1;
        }
        return;
    }
    if (sym == XK_Return || sym == XK_KP_Enter) { open_row(selected); return; }
    if (sym == XK_Escape) { selected = -1; menu_visible = 0; need_redraw = 1; }
}

static void handle_button(XButtonEvent *event)
{
    int x = event->x, y = event->y;
    if (dialog_kind) {
        if (event->button != Button1) return;
        int w = 420, h = dialog_kind == DIALOG_MESSAGE ? 112 : 142;
        int dx = (win_w - w) / 2, dy = (win_h - h) / 2, by = dy + h - 34;
        if (y >= by && y < by + 25) {
            if (x >= dx + w - 184 && x < dx + w - 104) dialog_accept();
            else if (x >= dx + w - 94 && x < dx + w - 14) { dialog_kind = DIALOG_NONE; need_redraw = 1; }
        }
        return;
    }
    if (menu_visible) {
        if (event->button == Button1) {
            int index = (y - menu_y - 3) / 24;
            if (x >= menu_x && x < menu_x + menu_w && index >= 0 && index < menu_n) menu_action(index);
            else { menu_visible = 0; need_redraw = 1; }
        } else if (event->button == Button3) { menu_visible = 0; need_redraw = 1; }
        return;
    }
    if (event->button == Button4 || event->button == Button5) {
        scroll += event->button == Button4 ? -3 : 3;
        clamp_scroll(); need_redraw = 1; return;
    }
    if (event->button == Button3) {
        int row = row_at(y);
        if (row >= 0) selected = row;
        open_menu(x, y, row); return;
    }
    if (event->button != Button1) return;
    if (y < LIST_Y) {
        for (int b = 0; b < 4; b++) {
            int bx = 8 + b * 34;
            if (x >= bx && x < bx + 30 && y >= TB_Y && y < TB_Y + TB_H) {
                if (b == 0) go_back(); else if (b == 1) go_up(); else if (b == 2) refresh_directory();
                else dialog_open(DIALOG_NEW_FOLDER, "New folder name:", "new folder", NULL);
                return;
            }
        }
        int px = 8 + 4 * 34 + 6;
        if (x >= px) dialog_open(DIALOG_PATH, "Open folder path:", cwd, NULL);
        return;
    }
    int row = row_at(y);
    selected = row;
    long now = monotonic_ms();
    if (row >= 0 && row == last_click_row && now - last_click_ms <= 450) {
        open_row(row); last_click_row = -1;
    } else last_click_row = row;
    last_click_ms = now; need_redraw = 1;
}

static void handle_motion(XMotionEvent *event)
{
    if (dialog_kind) return;
    if (menu_visible) {
        int row = -1;
        if (event->x >= menu_x && event->x < menu_x + menu_w) {
            int index = (event->y - menu_y - 3) / 24;
            if (index >= 0 && index < menu_n) row = index;
        }
        if (row != menu_row) { menu_row = row; need_redraw = 1; }
        return;
    }
    int old_row = hover_row, old_btn = hover_btn;
    hover_row = row_at(event->y); hover_btn = -1;
    if (event->y < LIST_Y) {
        for (int b = 0; b < 4; b++) {
            int bx = 8 + b * 34;
            if (event->x >= bx && event->x < bx + 30 && event->y >= TB_Y && event->y < TB_Y + TB_H)
                hover_btn = b;
        }
    }
    if (old_row != hover_row || old_btn != hover_btn) need_redraw = 1;
}

static void load_theme_font(void)
{
    struct scos_config cfg; config_defaults(&cfg);
    const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
    char path[512];
    if (xdg && *xdg) snprintf(path, sizeof(path), "%s/scoswm/scoswm.conf", xdg);
    else if (home && *home) snprintf(path, sizeof(path), "%s/.config/scoswm/scoswm.conf", home);
    else snprintf(path, sizeof(path), "scoswm.conf");
    config_load_file(path, &cfg);
    const struct scos_theme *th = theme_by_id(cfg.theme);
    if (th) theme = *th; else theme_apply_custom(&theme, &cfg);
    if (cfg.font[0]) font = XLoadQueryFont(dpy, cfg.font);
    if (!font) font = XLoadQueryFont(dpy, "9x15");
    if (!font) font = XLoadQueryFont(dpy, "10x20");
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (!font) { fprintf(stderr, "scos-files: no usable X core font\n"); exit(1); }
    XSetFont(dpy, gc, font->fid);
    main_px = alloc_rgb(theme.main); bg_px = alloc_rgb(theme.win_bg);
    text_px = alloc_rgb(theme.text); dark_px = alloc_rgb(theme.title_text);
    hover_px = alloc_rgb(color_blend_rgb(theme.win_bg, theme.main, 15));
    taskbar_px = alloc_rgb(theme.taskbar_bg);
}

static void usage(void)
{
    printf("scos-files %s - standalone SCos file explorer for real Linux files\n"
           "usage: scos-files [DIRECTORY]\n"
           "F5 refresh, Ctrl+L location, Ctrl+H hidden files, Backspace up.\n", VERSION);
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    const char *start = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        if (!strcmp(argv[i], "--version")) { printf("scos-files %s\n", VERSION); return 0; }
        if (argv[i][0] == '-') { fprintf(stderr, "scos-files: unknown option %s\n", argv[i]); return 2; }
        if (start) { fprintf(stderr, "scos-files: open one directory at a time\n"); return 2; }
        start = argv[i];
    }
    if (!start) start = getenv("HOME") && *getenv("HOME") ? getenv("HOME") : "/";
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "scos-files: cannot open DISPLAY\n"); return 1; }
    scr = DefaultScreen(dpy); gc = XCreateGC(dpy, RootWindow(dpy, scr), 0, NULL);
    a_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    a_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    a_net_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    a_pid = XInternAtom(dpy, "_NET_WM_PID", False);
    load_theme_font();
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 100, 80, win_w, win_h, 0, main_px, bg_px);
    XSetWindowBackgroundPixmap(dpy, win, None);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask |
                 PointerMotionMask | StructureNotifyMask);
    XSetWMProtocols(dpy, win, &a_delete, 1);
    XClassHint cls = { "scos-files", "SCosFiles" };
    XSetClassHint(dpy, win, &cls);
    pid_t pid = getpid();
    XChangeProperty(dpy, win, a_pid, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
    XSizeHints hints; memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize; hints.min_width = 400; hints.min_height = 240;
    XSetWMNormalHints(dpy, win, &hints);
    back = XCreatePixmap(dpy, win, win_w, win_h, DefaultDepth(dpy, scr));
    signal(SIGCHLD, SIG_IGN);
    if (!load_directory(start, 0)) {
        snprintf(cwd, sizeof(cwd), "%s", start);
        update_title();
    }
    XMapWindow(dpy, win); XFlush(dpy);
    while (running) {
        while (XPending(dpy)) {
            XEvent event; XNextEvent(dpy, &event);
            switch (event.type) {
            case Expose: if (event.xexpose.count == 0) need_redraw = 1; break;
            case ConfigureNotify: resize_window(event.xconfigure.width, event.xconfigure.height); break;
            case KeyPress: handle_key(&event.xkey); break;
            case ButtonPress: handle_button(&event.xbutton); break;
            case MotionNotify: handle_motion(&event.xmotion); break;
            case ClientMessage:
                if (event.xclient.message_type == a_protocols &&
                    (Atom)event.xclient.data.l[0] == a_delete) running = 0;
                break;
            default: break;
            }
        }
        if (!running) break;
        if (need_redraw) draw();
        int fd = ConnectionNumber(dpy);
        fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
        struct timeval tv = { 0, 250000 };
        select(fd + 1, &rf, NULL, NULL, &tv);
    }
    files_list_free(&listing);
    if (back) XFreePixmap(dpy, back);
    if (font) XFreeFont(dpy, font);
    if (gc) XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win); XCloseDisplay(dpy);
    return 0;
}
