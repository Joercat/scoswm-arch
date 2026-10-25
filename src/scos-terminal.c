#define _GNU_SOURCE
/* scos-terminal: the SCos terminal surface as a real Linux application.
 * Bash (or an explicitly supplied -e command) owns a genuine PTY; this
 * program only emulates the terminal and draws it with Xlib. */
#include "term_emulation.h"
#include "util.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "0.5.3"
#endif
#define TAB_H 22
#define TAB_W 74
#define MAX_TABS 6
#define CONTENT_X 6
#define CONTENT_Y (TAB_H + 4)

static Display *dpy;
static int scr;
static Window win;
static GC gc;
static XFontStruct *font;
static XIM xim;
static XIC xic;
static Pixmap back;
static int win_w = 700, win_h = 450;
static int cell_w = 8, cell_h = 16, font_ascent = 12;
static int running = 1, need_redraw = 1, focused = 1;
static int hold_open;
static struct scos_theme theme;
static unsigned long main_pixel, bg_pixel, text_pixel, taskbar_pixel;

static Atom a_wm_protocols, a_wm_delete, a_net_wm_name, a_utf8;
static Atom a_clipboard, a_targets, a_paste_property, a_net_wm_pid;

struct tab {
    term_emulator em;
    pid_t pid;
    int fd;
    int eof;
    int exited;
    int exit_status;
    int activity;
};
static struct tab *tabs[MAX_TABS];
static int ntabs, active_tab;

static char *clipboard;
static size_t clipboard_len;
static int selecting, selection_active;
static int sel_anchor_row, sel_anchor_col, sel_row, sel_col;

static struct { uint32_t rgb; unsigned long pixel; } *color_cache;
static int color_cache_n, color_cache_cap;

static unsigned long color_channel(unsigned int value, unsigned long mask)
{
    if (!mask) return 0;
    int shift = 0, bits = 0;
    unsigned long m = mask;
    while (!(m & 1)) { m >>= 1; shift++; }
    while (m & 1) { m >>= 1; bits++; }
    unsigned long maximum = bits >= (int)(8 * sizeof(unsigned long)) ?
        ~0UL : ((1UL << bits) - 1);
    return (((unsigned long)value * maximum + 127) / 255) << shift;
}

static unsigned long rgb_pixel(uint32_t rgb)
{
    for (int i = 0; i < color_cache_n; i++)
        if (color_cache[i].rgb == rgb) return color_cache[i].pixel;
    unsigned int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    unsigned long pixel;
    Visual *visual = DefaultVisual(dpy, scr);
    if (visual->class == TrueColor) {
        /* Arbitrary terminal RGB values must not exhaust the X colormap. */
        pixel = (color_channel(r, visual->red_mask) & visual->red_mask) |
                (color_channel(g, visual->green_mask) & visual->green_mask) |
                (color_channel(b, visual->blue_mask) & visual->blue_mask);
    } else {
        XColor c;
        c.red = (unsigned short)(r * 257);
        c.green = (unsigned short)(g * 257);
        c.blue = (unsigned short)(b * 257);
        if (!XAllocColor(dpy, DefaultColormap(dpy, scr), &c)) return BlackPixel(dpy, scr);
        pixel = c.pixel;
    }
    if (color_cache_n == color_cache_cap) {
        int cap = color_cache_cap ? color_cache_cap * 2 : 128;
        void *p = realloc(color_cache, (size_t)cap * sizeof(*color_cache));
        if (p) { color_cache = p; color_cache_cap = cap; }
    }
    if (color_cache_n < color_cache_cap) {
        color_cache[color_cache_n].rgb = rgb;
        color_cache[color_cache_n].pixel = pixel;
        color_cache_n++;
    }
    return pixel;
}

static struct tab *current_tab(void)
{
    return active_tab >= 0 && active_tab < ntabs ? tabs[active_tab] : NULL;
}

static void text_out(Drawable d, int x, int y, const char *s, unsigned long fg)
{
    XSetForeground(dpy, gc, fg);
    XDrawString(dpy, d, gc, x, y, s, (int)strlen(s));
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

static int content_cols(void)
{
    int n = (win_w - CONTENT_X * 2) / cell_w;
    return n < 2 ? 2 : n;
}

static int content_rows(void)
{
    int n = (win_h - CONTENT_Y - 4) / cell_h;
    return n < 2 ? 2 : n;
}

static void update_pty_size(struct tab *t)
{
    if (!t || t->fd < 0) return;
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)content_cols();
    ws.ws_row = (unsigned short)content_rows();
    ioctl(t->fd, TIOCSWINSZ, &ws);
}

static void update_title(void)
{
    struct tab *t = current_tab();
    char title[256], latin[256];
    if (t && t->em.title[0] && strcmp(t->em.title, "Terminal"))
        snprintf(title, sizeof(title), "SCos Terminal - %s", t->em.title);
    else snprintf(title, sizeof(title), "SCos Terminal");
    utf8_to_latin1(title, latin, sizeof(latin));
    XStoreName(dpy, win, latin);
    XChangeProperty(dpy, win, a_net_wm_name, a_utf8, 8, PropModeReplace,
                    (unsigned char *)title, (int)strlen(title));
}

static int write_input(struct tab *t, const void *data, size_t len)
{
    if (!t || t->fd < 0 || t->exited) return 0;
    const char *p = data;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(t->fd, p + done, len - done);
        if (n > 0) { done += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { t->fd, POLLOUT, 0 };
            if (poll(&pfd, 1, 250) > 0) continue;
        }
        break;
    }
    return (int)done;
}

static void paste_bytes(struct tab *t, const char *data, size_t len)
{
    if (!t || !data || !len) return;
    t->em.scroll_offset = 0;
    selecting = selection_active = 0;
    need_redraw = 1;
    if (t->em.bracketed_paste) write_input(t, "\033[200~", 6);
    write_input(t, data, len);
    if (t->em.bracketed_paste) write_input(t, "\033[201~", 6);
}

static void flush_replies(struct tab *t)
{
    if (t->em.reply_len) {
        write_input(t, t->em.reply, t->em.reply_len);
        t->em.reply_len = 0; t->em.reply[0] = 0;
    }
    if (t->em.title_changed) {
        t->em.title_changed = 0;
        if (t == current_tab()) update_title();
        need_redraw = 1;
    }
}

static void read_tab(struct tab *t)
{
    /* A deceased child's slave side can still have buffered output. */
    if (!t || t->fd < 0) return;
    unsigned char buf[8192];
    for (int round = 0; round < 32; round++) {
        ssize_t n = read(t->fd, buf, sizeof(buf));
        if (n > 0) {
            term_feed(&t->em, buf, (size_t)n);
            flush_replies(t);
            if (t != current_tab()) t->activity = 1;
            if (t == current_tab()) selection_active = 0;
            need_redraw = 1;
            continue;
        }
        if (n == 0) { t->eof = 1; break; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EIO) { t->eof = 1; break; }
        t->eof = 1; break;
    }
}

static void close_tab(int index)
{
    if (index < 0 || index >= ntabs) return;
    struct tab *t = tabs[index];
    if (t->pid > 0) {
        if (waitpid(t->pid, NULL, WNOHANG) == 0) kill(t->pid, SIGHUP);
        t->pid = 0;
    }
    if (t->fd >= 0) close(t->fd);
    term_free(&t->em);
    free(t);
    for (int i = index; i + 1 < ntabs; i++) tabs[i] = tabs[i + 1];
    ntabs--;
    if (active_tab >= index && active_tab > 0) active_tab--;
    if (active_tab >= ntabs) active_tab = ntabs - 1;
    selection_active = selecting = 0;
    if (!ntabs) running = 0;
    else { update_title(); update_pty_size(current_tab()); }
    need_redraw = 1;
}

static void reap_tabs(void)
{
    for (int i = ntabs - 1; i >= 0; i--) {
        struct tab *t = tabs[i];
        if (t->pid > 0) {
            int status;
            pid_t r = waitpid(t->pid, &status, WNOHANG);
            if (r == t->pid) {
                t->pid = 0; t->exited = 1; t->exit_status = status;
            }
        }
        if (!t->exited || t->exited == 2) continue;
        if (!t->eof && t->fd >= 0) continue; /* drain the PTY before closing */
        if (hold_open) {
            char msg[96];
            if (WIFEXITED(t->exit_status))
                snprintf(msg, sizeof(msg), "\r\n[process exited with status %d]\r\n", WEXITSTATUS(t->exit_status));
            else if (WIFSIGNALED(t->exit_status))
                snprintf(msg, sizeof(msg), "\r\n[process ended by signal %d]\r\n", WTERMSIG(t->exit_status));
            else snprintf(msg, sizeof(msg), "\r\n[process exited]\r\n");
            term_feed(&t->em, (const unsigned char *)msg, strlen(msg));
            t->exited = 2;
            if (t->fd >= 0) { close(t->fd); t->fd = -1; }
            need_redraw = 1;
        } else {
            close_tab(i);
        }
    }
}

static void spawn_child(struct tab *t, char *const command[], int cols, int rows)
{
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols; ws.ws_row = (unsigned short)rows;
    t->fd = -1;
    t->pid = forkpty(&t->fd, NULL, NULL, &ws);
    if (t->pid == 0) {
        const char *shell = getenv("SCOS_SHELL");
        if (!shell || !*shell) shell = access("/bin/bash", X_OK) == 0 ? "/bin/bash" : NULL;
        if (!shell || !*shell) shell = getenv("SHELL");
        if (!shell || !*shell) shell = "/bin/sh";
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        char cbuf[16], rbuf[16];
        snprintf(cbuf, sizeof(cbuf), "%d", cols); snprintf(rbuf, sizeof(rbuf), "%d", rows);
        setenv("COLUMNS", cbuf, 1); setenv("LINES", rbuf, 1);
        if (command && command[0]) {
            execvp(command[0], command);
            dprintf(STDERR_FILENO, "scos-terminal: cannot execute %s: %s\r\n", command[0], strerror(errno));
        } else {
            setenv("SHELL", shell, 1);
            char *const args[] = { (char *)shell, NULL };
            execvp(shell, args);
            dprintf(STDERR_FILENO, "scos-terminal: cannot execute %s: %s\r\n", shell, strerror(errno));
        }
        _exit(127);
    }
    if (t->pid < 0) {
        const char *msg = "scos-terminal: forkpty failed\r\n";
        term_feed(&t->em, (const unsigned char *)msg, strlen(msg));
        t->exited = 2; t->fd = -1;
        return;
    }
    int flags = fcntl(t->fd, F_GETFL, 0);
    if (flags >= 0) fcntl(t->fd, F_SETFL, flags | O_NONBLOCK);
}

static int new_tab(char *const command[])
{
    if (ntabs >= MAX_TABS) return 0;
    struct tab *t = calloc(1, sizeof(*t));
    if (!t) return 0;
    term_init(&t->em, content_cols(), content_rows(), theme.text, theme.win_bg);
    tabs[ntabs] = t;
    active_tab = ntabs;
    ntabs++;
    spawn_child(t, command, content_cols(), content_rows());
    update_title();
    need_redraw = 1;
    return 1;
}

static void selection_bounds(int *r1, int *c1, int *r2, int *c2)
{
    if (sel_anchor_row < sel_row ||
        (sel_anchor_row == sel_row && sel_anchor_col <= sel_col)) {
        *r1 = sel_anchor_row; *c1 = sel_anchor_col; *r2 = sel_row; *c2 = sel_col;
    } else {
        *r1 = sel_row; *c1 = sel_col; *r2 = sel_anchor_row; *c2 = sel_anchor_col;
    }
}

static int selected_cell(int row, int col)
{
    if (!selection_active) return 0;
    int r1, c1, r2, c2; selection_bounds(&r1, &c1, &r2, &c2);
    if (row < r1 || row > r2) return 0;
    if (r1 == r2) return col >= c1 && col <= c2;
    if (row == r1) return col >= c1;
    if (row == r2) return col <= c2;
    return 1;
}

static void append_utf8(char **buf, size_t *len, size_t *cap, uint32_t cp)
{
    unsigned char out[4]; int n;
    if (cp < 0x80) { out[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800) {
        out[0] = 0xc0 | (cp >> 6); out[1] = 0x80 | (cp & 63); n = 2;
    } else if (cp < 0x10000) {
        out[0] = 0xe0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 63); out[2] = 0x80 | (cp & 63); n = 3;
    } else {
        out[0] = 0xf0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 63);
        out[2] = 0x80 | ((cp >> 6) & 63); out[3] = 0x80 | (cp & 63); n = 4;
    }
    if (*len + (size_t)n + 2 > *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        while (nc < *len + (size_t)n + 2) nc *= 2;
        char *p = realloc(*buf, nc);
        if (!p) return;
        *buf = p; *cap = nc;
    }
    memcpy(*buf + *len, out, (size_t)n); *len += (size_t)n; (*buf)[*len] = 0;
}

static void copy_selection(void)
{
    struct tab *t = current_tab();
    if (!t || !selection_active) return;
    int r1, c1, r2, c2; selection_bounds(&r1, &c1, &r2, &c2);
    char *text = NULL; size_t len = 0, cap = 0;
    for (int r = r1; r <= r2; r++) {
        const term_line *line = term_view_line(&t->em, r);
        int first = r == r1 ? c1 : 0;
        int last = r == r2 ? c2 : content_cols() - 1;
        if (line) {
            if (last >= line->len) last = line->len - 1;
            while (last >= first && line->cells[last].ch == ' ') last--;
            for (int c = first; c <= last; c++) {
                uint32_t cp = line->cells[c].ch;
                if (cp) append_utf8(&text, &len, &cap, cp);
            }
        }
        if (r < r2) append_utf8(&text, &len, &cap, '\n');
    }
    free(clipboard); clipboard = text; clipboard_len = len;
    if (clipboard) {
        XSetSelectionOwner(dpy, XA_PRIMARY, win, CurrentTime);
        XSetSelectionOwner(dpy, a_clipboard, win, CurrentTime);
    }
}

static void style_cell(const term_emulator *em, const term_cell *in,
                       int row, int col, int *fg, int *bg, int *underline)
{
    term_cell c = *in;
    if (c.ch == 0) c.ch = ' ';
    uint32_t f = c.fg, b = c.bg;
    if (c.attrs & TERM_ATTR_DIM) f = color_blend_rgb(f, b, 110);
    if (c.attrs & TERM_ATTR_BOLD) f = color_blend_rgb(f, 0xffffff, 35);
    if (c.attrs & TERM_ATTR_REVERSE) { uint32_t tmp = f; f = b; b = tmp; }
    if (selected_cell(row, col)) { uint32_t tmp = f; f = b; b = tmp; }
    if (em->scroll_offset == 0 && em->cursor_visible && focused &&
        row == em->screen->cy && col == em->screen->cx) {
        uint32_t tmp = f; f = b; b = tmp;
    }
    *fg = (int)f; *bg = (int)b; *underline = (c.attrs & TERM_ATTR_UNDERLINE) != 0;
}

static uint32_t single_byte_fallback(uint32_t cp)
{
    if (cp <= 255) return cp;
    /* A configured legacy font can still show usable program borders. */
    switch (cp) {
    case 0x2500: case 0x2504: case 0x2508: case 0x254c: case 0x2574:
        return '-';
    case 0x2502: case 0x2506: case 0x250a: case 0x2551: case 0x2575:
        return '|';
    case 0x2550: return '=';
    case 0x2571: return '/';
    case 0x2572: return '\\';
    case 0x2588: case 0x2589: case 0x258a: case 0x258b:
    case 0x258c: case 0x258d: case 0x258e: case 0x258f:
    case 0x2590: case 0x2591: case 0x2592: case 0x2593:
        return '#';
    default:
        if ((cp >= 0x250c && cp <= 0x254b) ||
            (cp >= 0x2552 && cp <= 0x256c)) return '+';
        return '?';
    }
}

static XChar2b cell_char(uint32_t cp)
{
    if (!cp || cp == ' ') cp = ' ';
    if (cp > 0xffff) cp = 0xfffd;
    if (cp > 255 && font->max_byte1 == 0) cp = single_byte_fallback(cp);
    XChar2b out = { (unsigned char)(cp >> 8), (unsigned char)(cp & 255) };
    return out;
}

static void terminal_text_out(Drawable d, int x, int y, const XChar2b *text,
                              int count, unsigned long fg)
{
    if (count < 1) return;
    XSetForeground(dpy, gc, fg);
    XDrawString16(dpy, d, gc, x, y, (XChar2b *)text, count);
}

static void draw_terminal_line(struct tab *t, int row)
{
    const term_line *line = term_view_line(&t->em, row);
    int cols = content_cols();
    int y = CONTENT_Y + row * cell_h;
    int col = 0;
    while (col < cols) {
        term_cell raw;
        if (line && col < line->len) raw = line->cells[col];
        else { raw.ch = ' '; raw.fg = t->em.default_fg; raw.bg = t->em.default_bg; raw.attrs = 0; }
        int fg, bg, ul;
        style_cell(&t->em, &raw, row, col, &fg, &bg, &ul);
        int end = col + 1;
        while (end < cols && end - col < 512) {
            term_cell next;
            if (line && end < line->len) next = line->cells[end];
            else { next.ch = ' '; next.fg = t->em.default_fg; next.bg = t->em.default_bg; next.attrs = 0; }
            int f2, b2, u2; style_cell(&t->em, &next, row, end, &f2, &b2, &u2);
            if (f2 != fg || b2 != bg || u2 != ul) break;
            end++;
        }
        int count = end - col;
        XChar2b text[512];
        int nchars = 0;
        for (int i = 0; i < count; i++) {
            uint32_t cp = ' ';
            if (line && col + i < line->len) cp = line->cells[col + i].ch;
            /* A two-cell glyph supplies its own advance in a Unicode font;
             * do not draw its continuation cell a second time. */
            if (!cp && font->max_byte1 > 0) continue;
            text[nchars++] = cell_char(cp);
        }
        fill(back, CONTENT_X + col * cell_w, y, count * cell_w, cell_h, rgb_pixel((uint32_t)bg));
        terminal_text_out(back, CONTENT_X + col * cell_w, y + font_ascent,
                          text, nchars, rgb_pixel((uint32_t)fg));
        if (ul) fill(back, CONTENT_X + col * cell_w, y + cell_h - 2, count * cell_w, 1, rgb_pixel((uint32_t)fg));
        col = end;
    }
}

static void draw_tab_strip(void)
{
    fill(back, 0, 0, win_w, TAB_H, taskbar_pixel);
    int x = 4;
    for (int i = 0; i < ntabs && x + TAB_W <= win_w - 4; i++) {
        int active = i == active_tab;
        fill(back, x, 2, TAB_W - 3, TAB_H - 4,
             active ? bg_pixel : rgb_pixel(color_blend_rgb(theme.win_bg, theme.main, 12)));
        outline(back, x, 2, TAB_W - 3, TAB_H - 4, main_pixel);
        char label[32], latin[32];
        const char *name = tabs[i]->em.title[0] ? tabs[i]->em.title : "term";
        if (!strcmp(name, "Terminal")) name = "term";
        snprintf(label, sizeof(label), "%d:%.12s%s", i + 1, name, tabs[i]->activity && !active ? "*" : "");
        utf8_to_latin1(label, latin, sizeof(latin));
        text_out(back, x + 5, 5 + font_ascent, latin, active ? main_pixel : text_pixel);
        text_out(back, x + TAB_W - 16, 5 + font_ascent, "x", text_pixel);
        x += TAB_W;
    }
    if (x + 26 <= win_w - 4 && ntabs < MAX_TABS) {
        fill(back, x, 2, 24, TAB_H - 4, rgb_pixel(color_blend_rgb(theme.win_bg, theme.main, 12)));
        outline(back, x, 2, 24, TAB_H - 4, main_pixel);
        text_out(back, x + 7, 4 + font_ascent, "+", main_pixel);
    }
}

static void draw(void)
{
    if (!back) return;
    fill(back, 0, 0, win_w, win_h, bg_pixel);
    draw_tab_strip();
    struct tab *t = current_tab();
    if (t) for (int r = 0; r < content_rows(); r++) draw_terminal_line(t, r);
    XCopyArea(dpy, back, win, gc, 0, 0, win_w, win_h, 0, 0);
    need_redraw = 0;
}

static void clear_selection(void)
{
    selecting = selection_active = 0;
    need_redraw = 1;
}

static void scroll_view(int amount)
{
    struct tab *t = current_tab();
    if (!t) return;
    clear_selection();
    term_scroll(&t->em, amount);
    need_redraw = 1;
}

static void resize_terminal(int w, int h)
{
    if (w < 160) w = 160;
    if (h < 80) h = 80;
    if (w == win_w && h == win_h && back) return;
    Pixmap resized = XCreatePixmap(dpy, win, w, h, DefaultDepth(dpy, scr));
    if (!resized) return;
    win_w = w; win_h = h;
    Pixmap old = back;
    back = resized;
    if (old) XFreePixmap(dpy, old);
    for (int i = 0; i < ntabs; i++) {
        term_resize(&tabs[i]->em, content_cols(), content_rows());
        update_pty_size(tabs[i]);
    }
    clear_selection();
    need_redraw = 1;
}

static void request_paste(Atom selection)
{
    struct tab *t = current_tab();
    if (!t || t->exited) return;
    if (selection == a_clipboard && clipboard && XGetSelectionOwner(dpy, a_clipboard) == win) {
        paste_bytes(t, clipboard, clipboard_len); return;
    }
    if (selection == XA_PRIMARY && clipboard && XGetSelectionOwner(dpy, XA_PRIMARY) == win) {
        paste_bytes(t, clipboard, clipboard_len); return;
    }
    Atom owner = XGetSelectionOwner(dpy, selection);
    if (owner == None) {
        int n = 0; char *data = XFetchBytes(dpy, &n);
        if (data) { if (n > 0) paste_bytes(t, data, (size_t)n); XFree(data); }
        return;
    }
    XConvertSelection(dpy, selection, a_utf8, a_paste_property, win, CurrentTime);
}

static void send_special(struct tab *t, const char *s, int alt)
{
    if (alt) write_input(t, "\033", 1);
    write_input(t, s, strlen(s));
}

static void handle_key(XKeyEvent *e)
{
    struct tab *t = current_tab();
    if (!t) return;
    KeySym ks = XLookupKeysym(e, 0);
    unsigned int state = e->state;
    int ctrl = (state & ControlMask) != 0;
    int shift = (state & ShiftMask) != 0;
    int alt = (state & Mod1Mask) != 0;

    /* Keep Bash/readline's Ctrl+T and Ctrl+W intact.  GUI tab shortcuts
     * use Shift, like other Linux terminals. */
    if (ctrl && shift && (ks == XK_t || ks == XK_T)) { new_tab(NULL); return; }
    if (ctrl && shift && (ks == XK_w || ks == XK_W)) { close_tab(active_tab); return; }
    if (ctrl && (ks == XK_Tab || ks == XK_ISO_Left_Tab)) {
        active_tab = (active_tab + (shift ? ntabs - 1 : 1)) % ntabs;
        tabs[active_tab]->activity = 0;
        clear_selection(); update_title(); need_redraw = 1; return;
    }
    if (ctrl && shift && (ks == XK_c || ks == XK_C)) { copy_selection(); return; }
    if (ctrl && shift && (ks == XK_v || ks == XK_V)) { request_paste(a_clipboard); return; }
    if (shift && ks == XK_Page_Up) { scroll_view(content_rows() - 1); return; }
    if (shift && ks == XK_Page_Down) { scroll_view(-(content_rows() - 1)); return; }
    if (t->exited) return;
    if (t->em.scroll_offset) {
        t->em.scroll_offset = 0;
        clear_selection();
    }

    const char *esc = NULL;
    switch (ks) {
    case XK_Return: case XK_KP_Enter: send_special(t, "\r", alt); return;
    case XK_BackSpace: send_special(t, "\177", alt); return;
    case XK_Tab: send_special(t, "\t", alt); return;
    case XK_Escape: write_input(t, "\033", 1); return;
    case XK_Up: esc = t->em.application_cursor ? "\033OA" : "\033[A"; break;
    case XK_Down: esc = t->em.application_cursor ? "\033OB" : "\033[B"; break;
    case XK_Right: esc = t->em.application_cursor ? "\033OC" : "\033[C"; break;
    case XK_Left: esc = t->em.application_cursor ? "\033OD" : "\033[D"; break;
    case XK_Home: esc = t->em.application_cursor ? "\033OH" : "\033[H"; break;
    case XK_End: esc = t->em.application_cursor ? "\033OF" : "\033[F"; break;
    case XK_Insert: esc = "\033[2~"; break;
    case XK_Delete: esc = "\033[3~"; break;
    case XK_Page_Up: esc = "\033[5~"; break;
    case XK_Page_Down: esc = "\033[6~"; break;
    case XK_F1: esc = "\033OP"; break;
    case XK_F2: esc = "\033OQ"; break;
    case XK_F3: esc = "\033OR"; break;
    case XK_F4: esc = "\033OS"; break;
    case XK_F5: esc = "\033[15~"; break;
    case XK_F6: esc = "\033[17~"; break;
    case XK_F7: esc = "\033[18~"; break;
    case XK_F8: esc = "\033[19~"; break;
    case XK_F9: esc = "\033[20~"; break;
    case XK_F10: esc = "\033[21~"; break;
    case XK_F11: esc = "\033[23~"; break;
    case XK_F12: esc = "\033[24~"; break;
    default: break;
    }
    if (esc) { send_special(t, esc, alt); return; }
    if (ctrl && ((ks >= XK_a && ks <= XK_z) || (ks >= XK_A && ks <= XK_Z))) {
        char c = (char)(ks & 0x1f);
        if (alt) write_input(t, "\033", 1);
        write_input(t, &c, 1); return;
    }
    char buf[256];
    int n;
    if (xic) {
        KeySym text_sym;
        Status status = 0;
        n = XmbLookupString(xic, e, buf, sizeof(buf), &text_sym, &status);
        if (status != XLookupChars && status != XLookupBoth) n = 0;
    } else {
        n = XLookupString(e, buf, sizeof(buf), NULL, NULL);
    }
    if (n > 0) {
        if (alt) write_input(t, "\033", 1);
        write_input(t, buf, (size_t)n);
    }
}

static void report_mouse(struct tab *t, int button, int x, int y,
                         unsigned int state, int release, int motion)
{
    if (!t || !t->em.mouse_mode || t->exited) return;
    int col = (x - CONTENT_X) / cell_w + 1;
    int row = (y - CONTENT_Y) / cell_h + 1;
    if (col < 1) col = 1;
    if (row < 1) row = 1;
    if (col > content_cols()) col = content_cols();
    if (row > content_rows()) row = content_rows();

    unsigned int code;
    if (motion) {
        code = 32;
        if (state & Button1Mask) code += 0;
        else if (state & Button2Mask) code += 1;
        else if (state & Button3Mask) code += 2;
        else code += 3;
    } else if (button == Button4 || button == Button5) {
        code = button == Button4 ? 64 : 65;
    } else if (release && !t->em.mouse_sgr) {
        code = 3;
    } else {
        code = (unsigned int)(button - Button1);
    }
    if (state & ShiftMask) code += 4;
    if (state & Mod1Mask) code += 8;
    if (state & ControlMask) code += 16;

    if (t->em.mouse_sgr) {
        char seq[64];
        int n = snprintf(seq, sizeof(seq), "\033[<%u;%d;%d%c", code, col, row,
                         release ? 'm' : 'M');
        if (n > 0) write_input(t, seq, (size_t)n);
    } else {
        /* The original three-byte protocol cannot encode larger cells. */
        if (col > 223 || row > 223) return;
        char seq[6] = { '\033', '[', 'M', (char)(32 + code),
                        (char)(32 + col), (char)(32 + row) };
        write_input(t, seq, sizeof(seq));
    }
}

static void handle_button(XButtonEvent *e)
{
    struct tab *t = current_tab();
    if (!t) return;
    if (e->y >= CONTENT_Y && t->em.mouse_mode && !(e->state & ShiftMask) &&
        e->button >= Button1 && e->button <= Button5) {
        report_mouse(t, e->button, e->x, e->y, e->state, 0, 0);
        return;
    }
    if (e->button == Button4) { scroll_view(3); return; }
    if (e->button == Button5) { scroll_view(-3); return; }
    if (e->y < TAB_H && e->button == Button1) {
        int x = 4;
        for (int i = 0; i < ntabs && x + TAB_W <= win_w - 4; i++) {
            if (e->x >= x && e->x < x + TAB_W - 3) {
                if (e->x >= x + TAB_W - 18) close_tab(i);
                else { active_tab = i; tabs[i]->activity = 0; clear_selection(); update_title(); need_redraw = 1; }
                return;
            }
            x += TAB_W;
        }
        if (ntabs < MAX_TABS && e->x >= x && e->x <= x + 24) new_tab(NULL);
        return;
    }
    if (e->y < CONTENT_Y) return;
    if (e->button == Button2) { request_paste(XA_PRIMARY); return; }
    if (e->button != Button1) return;
    int col = (e->x - CONTENT_X) / cell_w;
    int row = (e->y - CONTENT_Y) / cell_h;
    if (col < 0) col = 0;
    if (col >= content_cols()) col = content_cols() - 1;
    if (row < 0) row = 0;
    if (row >= content_rows()) row = content_rows() - 1;
    sel_anchor_row = sel_row = row; sel_anchor_col = sel_col = col;
    selecting = 1; selection_active = 0; need_redraw = 1;
}

static void handle_motion(XMotionEvent *e)
{
    struct tab *t = current_tab();
    if (t && e->y >= CONTENT_Y && !(e->state & ShiftMask) &&
        (t->em.mouse_mode == 1003 ||
         (t->em.mouse_mode == 1002 &&
          (e->state & (Button1Mask | Button2Mask | Button3Mask))))) {
        report_mouse(t, 0, e->x, e->y, e->state, 0, 1);
        return;
    }
    if (!selecting) return;
    int col = (e->x - CONTENT_X) / cell_w;
    int row = (e->y - CONTENT_Y) / cell_h;
    if (col < 0) col = 0;
    if (col >= content_cols()) col = content_cols() - 1;
    if (row < 0) row = 0;
    if (row >= content_rows()) row = content_rows() - 1;
    if (row != sel_row || col != sel_col) {
        sel_row = row; sel_col = col; selection_active = 1; need_redraw = 1;
    }
}

static void handle_selection_request(XSelectionRequestEvent *req)
{
    XSelectionEvent reply;
    memset(&reply, 0, sizeof(reply));
    reply.type = SelectionNotify; reply.display = req->display; reply.requestor = req->requestor;
    reply.selection = req->selection; reply.target = req->target; reply.time = req->time;
    reply.property = None;
    if (clipboard && (req->selection == XA_PRIMARY || req->selection == a_clipboard)) {
        if (req->target == a_targets) {
            Atom targets[3] = { a_targets, a_utf8, XA_STRING };
            XChangeProperty(dpy, req->requestor, req->property, XA_ATOM, 32,
                            PropModeReplace, (unsigned char *)targets, 3);
            reply.property = req->property;
        } else if (req->target == a_utf8 || req->target == XA_STRING) {
            XChangeProperty(dpy, req->requestor, req->property, req->target, 8,
                            PropModeReplace, (unsigned char *)clipboard, (int)clipboard_len);
            reply.property = req->property;
        }
    }
    XSendEvent(dpy, req->requestor, False, 0, (XEvent *)&reply);
}

static void handle_selection_notify(XSelectionEvent *e)
{
    struct tab *t = current_tab();
    if (!t || e->property == None) return;
    Atom type; int format; unsigned long n, after; unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, e->property, 0, 1 << 20, True,
                           AnyPropertyType, &type, &format, &n, &after, &data) == Success && data) {
        if (format == 8 && n) paste_bytes(t, (const char *)data, n);
        XFree(data);
    }
}

static void handle_event(XEvent *e)
{
    switch (e->type) {
    case Expose: if (e->xexpose.count == 0) need_redraw = 1; break;
    case ConfigureNotify: resize_terminal(e->xconfigure.width, e->xconfigure.height); break;
    case KeyPress: handle_key(&e->xkey); break;
    case ButtonPress: handle_button(&e->xbutton); break;
    case ButtonRelease: {
        struct tab *t = current_tab();
        XButtonEvent *b = &e->xbutton;
        if (t && b->y >= CONTENT_Y && t->em.mouse_mode &&
            !(b->state & ShiftMask) && b->button >= Button1 && b->button <= Button3) {
            report_mouse(t, b->button, b->x, b->y, b->state, 1, 0);
        } else if (b->button == Button1 && selecting) {
            selecting = 0;
            if (selection_active) copy_selection();
            need_redraw = 1;
        }
        break; }
    case MotionNotify: handle_motion(&e->xmotion); break;
    case SelectionRequest: handle_selection_request(&e->xselectionrequest); break;
    case SelectionNotify: handle_selection_notify(&e->xselection); break;
    case FocusIn:
        focused = 1;
        if (xic) XSetICFocus(xic);
        need_redraw = 1;
        break;
    case FocusOut:
        focused = 0;
        if (xic) XUnsetICFocus(xic);
        need_redraw = 1;
        break;
    case ClientMessage:
        if (e->xclient.message_type == a_wm_protocols &&
            (Atom)e->xclient.data.l[0] == a_wm_delete) running = 0;
        break;
    default: break;
    }
}

static void load_theme_and_font(void)
{
    struct scos_config cfg; config_defaults(&cfg);
    const char *home = getenv("HOME");
    char path[512];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(path, sizeof(path), "%s/scoswm/scoswm.conf", xdg);
    else if (home && *home) snprintf(path, sizeof(path), "%s/.config/scoswm/scoswm.conf", home);
    else snprintf(path, sizeof(path), "scoswm.conf");
    config_load_file(path, &cfg);
    const struct scos_theme *th = theme_by_id(cfg.theme);
    if (th) theme = *th; else theme_apply_custom(&theme, &cfg);
    if (cfg.font[0]) font = XLoadQueryFont(dpy, cfg.font);
    if (!font) font = XLoadQueryFont(dpy, "-misc-fixed-medium-r-normal--14-130-75-75-c-70-iso10646-1");
    if (!font) font = XLoadQueryFont(dpy, "-misc-fixed-medium-r-normal--15-140-75-75-c-90-iso10646-1");
    if (!font) font = XLoadQueryFont(dpy, "-misc-fixed-medium-r-normal--14-130-75-75-c-70-iso8859-1");
    if (!font) font = XLoadQueryFont(dpy, "9x15");
    if (!font) font = XLoadQueryFont(dpy, "10x20");
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (!font) { fprintf(stderr, "scos-terminal: no usable X core font\n"); exit(1); }
    cell_w = XTextWidth(font, "M", 1);
    if (cell_w < 1) cell_w = font->max_bounds.width;
    if (cell_w < 1) cell_w = 8;
    font_ascent = font->ascent;
    cell_h = font->ascent + font->descent + 2;
    main_pixel = rgb_pixel(theme.main); bg_pixel = rgb_pixel(theme.win_bg);
    text_pixel = rgb_pixel(theme.text); taskbar_pixel = rgb_pixel(theme.taskbar_bg);
}

static void create_window(void)
{
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 80, 80, win_w, win_h, 0,
                              main_pixel, bg_pixel);
    /* Every pixel is painted through the backing pixmap. Disabling the
     * server's background erase avoids a blank frame during live resize. */
    XSetWindowBackgroundPixmap(dpy, win, None);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask |
                 ButtonReleaseMask | PointerMotionMask | StructureNotifyMask |
                 FocusChangeMask);
    XSetWMProtocols(dpy, win, &a_wm_delete, 1);
    XStoreName(dpy, win, "SCos Terminal");
    XClassHint cls = { "scos-terminal", "SCosTerminal" };
    XSetClassHint(dpy, win, &cls);
    pid_t pid = getpid();
    XChangeProperty(dpy, win, a_net_wm_pid, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&pid, 1);
    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize; hints.min_width = 160; hints.min_height = 80;
    XSetWMNormalHints(dpy, win, &hints);
    /* No per-window core-font cursor: inherit the root/session cursor so
     * KDE, GTK and SCos windows all use the same configured pointer theme. */
    back = XCreatePixmap(dpy, win, win_w, win_h, DefaultDepth(dpy, scr));
    xim = XOpenIM(dpy, NULL, NULL, NULL);
    if (xim) {
        xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                        XNClientWindow, win, XNFocusWindow, win, NULL);
        if (!xic) { XCloseIM(xim); xim = NULL; }
    }
    XMapWindow(dpy, win);
    XFlush(dpy);
}

static void usage(void)
{
    printf("scos-terminal %s - SCos terminal for real Linux shells\n"
           "usage: scos-terminal [-hold] [-e COMMAND [ARG...]]\n"
           "  -hold             keep the tab open after the command exits\n"
           "  -e COMMAND ...    execute COMMAND instead of the interactive shell\n"
           "Tabs: Ctrl+Shift+T new, Ctrl+Shift+W close, Ctrl+Tab cycle.\n"
           "Copy/paste: Ctrl+Shift+C/V; mouse wheel and Shift+PageUp/Down scroll.\n",
           VERSION);
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    XSetLocaleModifiers("");
    char **command = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        if (!strcmp(argv[i], "--version")) { printf("scos-terminal %s\n", VERSION); return 0; }
        if (!strcmp(argv[i], "-hold") || !strcmp(argv[i], "--hold")) { hold_open = 1; continue; }
        if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "--")) {
            if (i + 1 >= argc) { fprintf(stderr, "scos-terminal: -e needs a command\n"); return 2; }
            command = &argv[i + 1]; break;
        }
        fprintf(stderr, "scos-terminal: unknown option %s\n", argv[i]); return 2;
    }
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "scos-terminal: cannot open DISPLAY\n"); return 1; }
    scr = DefaultScreen(dpy);
    gc = XCreateGC(dpy, RootWindow(dpy, scr), 0, NULL);
    a_wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    a_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    a_net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    a_targets = XInternAtom(dpy, "TARGETS", False);
    a_paste_property = XInternAtom(dpy, "SCOS_TERMINAL_PASTE", False);
    a_net_wm_pid = XInternAtom(dpy, "_NET_WM_PID", False);
    load_theme_and_font();
    XSetFont(dpy, gc, font->fid);
    create_window();
    if (!new_tab(command)) { fprintf(stderr, "scos-terminal: could not create a tab\n"); return 1; }
    update_title();
    while (running) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (!XFilterEvent(&e, None)) handle_event(&e);
        }
        for (int i = 0; i < ntabs; i++) read_tab(tabs[i]);
        reap_tabs();
        if (!running) break;
        if (need_redraw) draw();
        struct pollfd fds[MAX_TABS + 1];
        int nfds = 1;
        fds[0].fd = ConnectionNumber(dpy); fds[0].events = POLLIN; fds[0].revents = 0;
        for (int i = 0; i < ntabs; i++) {
            fds[nfds].fd = tabs[i]->fd; fds[nfds].events = tabs[i]->fd >= 0 ? POLLIN : 0;
            fds[nfds].revents = 0; nfds++;
        }
        poll(fds, nfds, 250);
    }
    while (ntabs) close_tab(ntabs - 1);
    free(clipboard);
    free(color_cache);
    if (xic) XDestroyIC(xic);
    if (xim) XCloseIM(xim);
    if (back) XFreePixmap(dpy, back);
    if (font) XFreeFont(dpy, font);
    if (gc) XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
