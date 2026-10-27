#define _XOPEN_SOURCE 700
#include "term_emulation.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { P_NORMAL, P_ESC, P_CSI, P_OSC, P_OSC_ESC, P_CHARSET };

static term_cell blank_cell(const term_emulator *t)
{
    term_cell c;
    c.ch = ' '; c.fg = t->fg; c.bg = t->bg; c.attrs = 0;
    return c;
}

static void line_free(term_line *l)
{
    free(l->cells);
    l->cells = NULL; l->len = l->cap = 0;
}

static void line_init(term_line *l, int cols, term_cell blank)
{
    l->cells = malloc((size_t)cols * sizeof(*l->cells));
    l->len = l->cap = cols;
    for (int i = 0; i < cols; i++) l->cells[i] = blank;
}

static void line_resize(term_line *l, int cols, term_cell blank)
{
    term_cell *p = realloc(l->cells, (size_t)cols * sizeof(*p));
    if (!p) return;                 /* keep the old, valid line on OOM */
    l->cells = p;
    if (cols > l->cap)
        for (int i = l->cap; i < cols; i++) p[i] = blank;
    l->cap = cols;
    l->len = cols;
}

static void history_push(term_emulator *t, term_line *old);

static void screen_free(term_screen *s)
{
    if (!s->lines) return;
    for (int i = 0; i < s->rows; i++) line_free(&s->lines[i]);
    free(s->lines);
    s->lines = NULL; s->rows = s->cols = 0;
}

static void screen_init(term_screen *s, int cols, int rows, term_cell blank)
{
    memset(s, 0, sizeof(*s));
    s->cols = cols; s->rows = rows;
    s->lines = calloc((size_t)rows, sizeof(*s->lines));
    for (int i = 0; i < rows; i++) line_init(&s->lines[i], cols, blank);
    s->scroll_top = 0; s->scroll_bottom = rows - 1;
}

static void screen_resize(term_emulator *t, term_screen *s, int cols, int rows,
                          term_cell blank)
{
    if (!s->lines) return;
    if (cols == s->cols && rows == s->rows) return;
    term_line *lines = calloc((size_t)rows, sizeof(*lines));
    if (!lines) return;
    int keep = rows < s->rows ? rows : s->rows;
    int old_first = rows < s->rows ? s->rows - rows : 0;
    /* Keep the active bottom rows when shrinking.  Removed main-screen
     * rows remain available in scrollback instead of silently vanishing. */
    for (int i = 0; i < old_first; i++) {
        if (s == &t->main_screen) history_push(t, &s->lines[i]);
        else line_free(&s->lines[i]);
    }
    for (int i = 0; i < keep; i++) {
        lines[i] = s->lines[old_first + i];
        line_resize(&lines[i], cols, blank);
    }
    for (int i = keep; i < rows; i++) line_init(&lines[i], cols, blank);
    free(s->lines);
    s->lines = lines; s->cols = cols; s->rows = rows;
    s->cy -= old_first;
    s->saved_cy -= old_first;
    if (s->cx >= cols) s->cx = cols - 1;
    if (s->cy >= rows) s->cy = rows - 1;
    if (s->cx < 0) s->cx = 0;
    if (s->cy < 0) s->cy = 0;
    if (s->saved_cx >= cols) s->saved_cx = cols - 1;
    if (s->saved_cy >= rows) s->saved_cy = rows - 1;
    if (s->saved_cx < 0) s->saved_cx = 0;
    if (s->saved_cy < 0) s->saved_cy = 0;
    s->scroll_top = 0; s->scroll_bottom = rows - 1;
    s->wrap_pending = 0;
}

static void history_push(term_emulator *t, term_line *old)
{
    if (t->history_count == TERM_HISTORY_LIMIT) {
        line_free(&t->history[0]);
        memmove(&t->history[0], &t->history[1],
                (TERM_HISTORY_LIMIT - 1) * sizeof(t->history[0]));
        t->history_count--;
    }
    t->history[t->history_count++] = *old;
    old->cells = NULL; old->len = old->cap = 0;
    if (t->scroll_offset > 0) t->scroll_offset++;
}

static void scroll_up(term_emulator *t, term_screen *s, int count)
{
    if (count < 1) count = 1;
    while (count-- > 0) {
        int top = s->scroll_top, bottom = s->scroll_bottom;
        term_line old = s->lines[top];
        for (int i = top; i < bottom; i++) s->lines[i] = s->lines[i + 1];
        line_init(&s->lines[bottom], s->cols, blank_cell(t));
        if (!t->alt_active && top == 0 && bottom == s->rows - 1)
            history_push(t, &old);
        else
            line_free(&old);
    }
}

static void scroll_down(term_emulator *t, term_screen *s, int count)
{
    if (count < 1) count = 1;
    while (count-- > 0) {
        int top = s->scroll_top, bottom = s->scroll_bottom;
        term_line old = s->lines[bottom];
        for (int i = bottom; i > top; i--) s->lines[i] = s->lines[i - 1];
        line_init(&s->lines[top], s->cols, blank_cell(t));
        line_free(&old);
    }
}

static void linefeed(term_emulator *t)
{
    term_screen *s = t->screen;
    s->wrap_pending = 0;
    if (s->cy == s->scroll_bottom) scroll_up(t, s, 1);
    else if (s->cy < s->rows - 1) s->cy++;
}

static void reverse_linefeed(term_emulator *t)
{
    term_screen *s = t->screen;
    s->wrap_pending = 0;
    if (s->cy == s->scroll_top) scroll_down(t, s, 1);
    else if (s->cy > 0) s->cy--;
}

static void save_cursor(term_emulator *t)
{
    t->screen->saved_cx = t->screen->cx;
    t->screen->saved_cy = t->screen->cy;
}

static void restore_cursor(term_emulator *t)
{
    t->screen->cx = t->screen->saved_cx;
    t->screen->cy = t->screen->saved_cy;
    if (t->screen->cx >= t->screen->cols) t->screen->cx = t->screen->cols - 1;
    if (t->screen->cy >= t->screen->rows) t->screen->cy = t->screen->rows - 1;
    if (t->screen->cx < 0) t->screen->cx = 0;
    if (t->screen->cy < 0) t->screen->cy = 0;
    t->screen->wrap_pending = 0;
}

static void clear_line_range(term_emulator *t, int row, int first, int last)
{
    term_screen *s = t->screen;
    if (row < 0 || row >= s->rows) return;
    if (first < 0) first = 0;
    if (last >= s->cols) last = s->cols - 1;
    for (int i = first; i <= last; i++) s->lines[row].cells[i] = blank_cell(t);
}

static void clear_screen(term_emulator *t, int home)
{
    term_screen *s = t->screen;
    for (int i = 0; i < s->rows; i++) clear_line_range(t, i, 0, s->cols - 1);
    if (home) { s->cx = s->cy = 0; }
    s->wrap_pending = 0;
}

static void erase_display(term_emulator *t, int mode)
{
    term_screen *s = t->screen;
    if (mode == 0) {
        clear_line_range(t, s->cy, s->cx, s->cols - 1);
        for (int i = s->cy + 1; i < s->rows; i++) clear_line_range(t, i, 0, s->cols - 1);
    } else if (mode == 1) {
        for (int i = 0; i < s->cy; i++) clear_line_range(t, i, 0, s->cols - 1);
        clear_line_range(t, s->cy, 0, s->cx);
    } else if (mode == 2 || mode == 3) {
        clear_screen(t, 0);
        if (mode == 3) {
            for (int i = 0; i < t->history_count; i++) line_free(&t->history[i]);
            t->history_count = 0; t->scroll_offset = 0;
        }
    }
}

static void insert_lines(term_emulator *t, int count)
{
    term_screen *s = t->screen;
    if (s->cy < s->scroll_top || s->cy > s->scroll_bottom) return;
    if (count < 1) count = 1;
    while (count-- > 0) {
        line_free(&s->lines[s->scroll_bottom]);
        for (int i = s->scroll_bottom; i > s->cy; i--) s->lines[i] = s->lines[i - 1];
        line_init(&s->lines[s->cy], s->cols, blank_cell(t));
    }
}

static void delete_lines(term_emulator *t, int count)
{
    term_screen *s = t->screen;
    if (s->cy < s->scroll_top || s->cy > s->scroll_bottom) return;
    if (count < 1) count = 1;
    while (count-- > 0) {
        line_free(&s->lines[s->cy]);
        for (int i = s->cy; i < s->scroll_bottom; i++) s->lines[i] = s->lines[i + 1];
        line_init(&s->lines[s->scroll_bottom], s->cols, blank_cell(t));
    }
}

static void put_codepoint(term_emulator *t, uint32_t cp)
{
    term_screen *s = t->screen;
    if (cp == 0 || cp > 0x10ffff) cp = 0xfffd;
    int width = wcwidth((wchar_t)cp);
    if (width < 0) width = 1;
    if (width == 0) return;          /* combining mark: keep the prior cell */
    if (s->wrap_pending && t->autowrap) { s->cx = 0; linefeed(t); }
    if (width == 2 && s->cx >= s->cols - 1 && t->autowrap) {
        s->cx = 0; linefeed(t);
    }
    if (s->cy < 0) s->cy = 0;
    if (s->cy >= s->rows) s->cy = s->rows - 1;
    term_cell c = blank_cell(t);
    c.ch = cp; c.fg = t->fg; c.bg = t->bg; c.attrs = t->attrs;
    s->lines[s->cy].cells[s->cx] = c;
    if (width == 2 && s->cx + 1 < s->cols) {
        term_cell cont = c; cont.ch = 0;
        s->lines[s->cy].cells[s->cx + 1] = cont;
    }
    if (s->cx + width >= s->cols) {
        s->cx = s->cols - 1;
        s->wrap_pending = t->autowrap;
    } else {
        s->cx += width;
        s->wrap_pending = 0;
    }
}

static void reply_add(term_emulator *t, const char *s)
{
    size_t n = strlen(s);
    if (n > sizeof(t->reply) - 1 - t->reply_len)
        n = sizeof(t->reply) - 1 - t->reply_len;
    memcpy(t->reply + t->reply_len, s, n);
    t->reply_len += n;
    t->reply[t->reply_len] = 0;
}

static uint32_t ansi_color(int n)
{
    static const uint32_t basic[16] = {
        0x000000, 0xcc0000, 0x4e9a06, 0xc4a000,
        0x3465a4, 0x75507b, 0x06989a, 0xd3d7cf,
        0x555753, 0xef2929, 0x8ae234, 0xfce94f,
        0x729fcf, 0xad7fa8, 0x34e2e2, 0xeeeeec
    };
    if (n >= 0 && n < 16) return basic[n];
    if (n >= 16 && n <= 231) {
        int v = n - 16, r = v / 36, g = (v / 6) % 6, b = v % 6;
        int levels[6] = { 0, 95, 135, 175, 215, 255 };
        return ((uint32_t)levels[r] << 16) | ((uint32_t)levels[g] << 8) | (uint32_t)levels[b];
    }
    if (n >= 232 && n <= 255) {
        int v = 8 + (n - 232) * 10;
        return ((uint32_t)v << 16) | ((uint32_t)v << 8) | (uint32_t)v;
    }
    return 0xffffff;
}

static void sgr(term_emulator *t, const int *p, int np)
{
    if (!np) {
        t->fg = t->default_fg; t->bg = t->default_bg; t->attrs = 0;
        return;
    }
    for (int i = 0; i < np; i++) {
        int n = p[i] < 0 ? 0 : p[i];
        if (n == 0) { t->fg = t->default_fg; t->bg = t->default_bg; t->attrs = 0; }
        else if (n == 1) t->attrs |= TERM_ATTR_BOLD;
        else if (n == 2) t->attrs |= TERM_ATTR_DIM;
        else if (n == 4) t->attrs |= TERM_ATTR_UNDERLINE;
        else if (n == 5) t->attrs |= TERM_ATTR_BLINK;
        else if (n == 7) t->attrs |= TERM_ATTR_REVERSE;
        else if (n == 22) t->attrs &= ~(TERM_ATTR_BOLD | TERM_ATTR_DIM);
        else if (n == 24) t->attrs &= ~TERM_ATTR_UNDERLINE;
        else if (n == 25) t->attrs &= ~TERM_ATTR_BLINK;
        else if (n == 27) t->attrs &= ~TERM_ATTR_REVERSE;
        else if (n == 39) t->fg = t->default_fg;
        else if (n == 49) t->bg = t->default_bg;
        else if (n >= 30 && n <= 37) t->fg = ansi_color(n - 30);
        else if (n >= 40 && n <= 47) t->bg = ansi_color(n - 40);
        else if (n >= 90 && n <= 97) t->fg = ansi_color(n - 90 + 8);
        else if (n >= 100 && n <= 107) t->bg = ansi_color(n - 100 + 8);
        else if ((n == 38 || n == 48) && i + 1 < np) {
            uint32_t *dst = n == 38 ? &t->fg : &t->bg;
            if (p[i + 1] == 5 && i + 2 < np) {
                *dst = ansi_color(p[i + 2]); i += 2;
            } else if (p[i + 1] == 2 && i + 4 < np) {
                int r = p[i + 2] < 0 ? 0 : p[i + 2] > 255 ? 255 : p[i + 2];
                int g = p[i + 3] < 0 ? 0 : p[i + 3] > 255 ? 255 : p[i + 3];
                int b = p[i + 4] < 0 ? 0 : p[i + 4] > 255 ? 255 : p[i + 4];
                *dst = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                i += 4;
            }
        }
    }
}

static void enter_alt(term_emulator *t, int clear)
{
    if (t->alt_active) return;
    t->alt_saved_cx = t->main_screen.cx;
    t->alt_saved_cy = t->main_screen.cy;
    if (!t->alt_screen.lines)
        screen_init(&t->alt_screen, t->main_screen.cols, t->main_screen.rows, blank_cell(t));
    t->alt_active = 1; t->screen = &t->alt_screen;
    t->scroll_offset = 0;
    if (clear) clear_screen(t, 1);
}

static void leave_alt(term_emulator *t)
{
    if (!t->alt_active) return;
    t->alt_active = 0; t->screen = &t->main_screen;
    t->scroll_offset = 0;
    t->screen->cx = t->alt_saved_cx; t->screen->cy = t->alt_saved_cy;
    restore_cursor(t);
}

static int parse_params(const char *s, int *p, int *is_private)
{
    int np = 0, value = -1;
    *is_private = 0;
    if (*s == '?' || *s == '>' || *s == '<' || *s == '=') {
        *is_private = (unsigned char)*s;
        s++;
    }
    for (; *s; s++) {
        if (isdigit((unsigned char)*s)) {
            if (value < 0) value = 0;
            if (value < 1000000) value = value * 10 + (*s - '0');
        } else if (*s == ';' || *s == ':') {
            if (np < TERM_MAX_PARAMS) p[np++] = value;
            value = -1;
        }
    }
    if (np < TERM_MAX_PARAMS) p[np++] = value;
    return np;
}

static int param(const int *p, int np, int i, int def)
{
    return i < np && p[i] >= 0 ? p[i] : def;
}

static void csi_handle(term_emulator *t, unsigned char final)
{
    int p[TERM_MAX_PARAMS], private_flag = 0;
    int np = parse_params(t->csi, p, &private_flag);
    term_screen *s = t->screen;
    int n = param(p, np, 0, 1); if (n < 1) n = 1;
    switch (final) {
    case 'A': s->cy -= n; if (s->cy < 0) s->cy = 0; s->wrap_pending = 0; break;
    case 'B': s->cy += n; if (s->cy >= s->rows) s->cy = s->rows - 1; s->wrap_pending = 0; break;
    case 'C': s->cx += n; if (s->cx >= s->cols) s->cx = s->cols - 1; s->wrap_pending = 0; break;
    case 'D': s->cx -= n; if (s->cx < 0) s->cx = 0; s->wrap_pending = 0; break;
    case 'E': s->cy += n; if (s->cy >= s->rows) s->cy = s->rows - 1; s->cx = 0; break;
    case 'F': s->cy -= n; if (s->cy < 0) s->cy = 0; s->cx = 0; break;
    case 'G': case '`':
        s->cx = param(p, np, 0, 1) - 1;
        if (s->cx < 0) s->cx = 0;
        if (s->cx >= s->cols) s->cx = s->cols - 1;
        break;
    case 'H': case 'f':
        s->cy = param(p, np, 0, 1) - 1;
        s->cx = param(p, np, 1, 1) - 1;
        if (s->cy < 0) s->cy = 0;
        if (s->cy >= s->rows) s->cy = s->rows - 1;
        if (s->cx < 0) s->cx = 0;
        if (s->cx >= s->cols) s->cx = s->cols - 1;
        s->wrap_pending = 0;
        break;
    case 'J': erase_display(t, param(p, np, 0, 0)); break;
    case 'K': {
        int mode = param(p, np, 0, 0);
        if (mode == 0) clear_line_range(t, s->cy, s->cx, s->cols - 1);
        else if (mode == 1) clear_line_range(t, s->cy, 0, s->cx);
        else clear_line_range(t, s->cy, 0, s->cols - 1);
        break; }
    case 'L': insert_lines(t, n); break;
    case 'M': delete_lines(t, n); break;
    case 'P': {
        int count = n, at = s->cx;
        if (count > s->cols - at) count = s->cols - at;
        for (int i = at; i + count < s->cols; i++) s->lines[s->cy].cells[i] = s->lines[s->cy].cells[i + count];
        clear_line_range(t, s->cy, s->cols - count, s->cols - 1);
        break; }
    case '@': {
        int count = n, at = s->cx;
        if (count > s->cols - at) count = s->cols - at;
        for (int i = s->cols - 1; i >= at + count; i--) s->lines[s->cy].cells[i] = s->lines[s->cy].cells[i - count];
        clear_line_range(t, s->cy, at, at + count - 1);
        break; }
    case 'S': scroll_up(t, s, n); break;
    case 'T': scroll_down(t, s, n); break;
    case 'X': clear_line_range(t, s->cy, s->cx, s->cx + n - 1); break;
    case 'd':
        s->cy = param(p, np, 0, 1) - 1;
        if (s->cy < 0) s->cy = 0;
        if (s->cy >= s->rows) s->cy = s->rows - 1;
        break;
    case 'r': {
        int top = param(p, np, 0, 1) - 1, bottom = param(p, np, 1, s->rows) - 1;
        if (top >= 0 && bottom < s->rows && top < bottom) {
            s->scroll_top = top; s->scroll_bottom = bottom; s->cx = s->cy = 0;
        }
        break; }
    case 's': save_cursor(t); break;
    case 'u': restore_cursor(t); break;
    case 'm': sgr(t, p, np); break;
    case 'n':
        if (param(p, np, 0, 0) == 5) reply_add(t, "\033[0n");
        else if (param(p, np, 0, 0) == 6) {
            char r[64]; snprintf(r, sizeof(r), "\033[%d;%dR", s->cy + 1, s->cx + 1); reply_add(t, r);
        }
        break;
    case 'c': reply_add(t, "\033[?1;2c"); break;
    case 'h': case 'l': {
        int on = final == 'h';
        for (int i = 0; i < np; i++) {
            int mode = p[i] < 0 ? 0 : p[i];
            if (private_flag == '?') {
                if (mode == 25) t->cursor_visible = on;
                else if (mode == 1) t->application_cursor = on;
                else if (mode == 7) t->autowrap = on;
                else if (mode == 47 || mode == 1047 || mode == 1049) {
                    if (on) enter_alt(t, mode != 47); else leave_alt(t);
                } else if (mode == 1048) { if (on) save_cursor(t); else restore_cursor(t); }
                else if (mode == 2004) t->bracketed_paste = on;
                else if (mode == 1000 || mode == 1002 || mode == 1003) {
                    if (on) t->mouse_mode = mode;
                    else if (t->mouse_mode == mode) t->mouse_mode = 0;
                } else if (mode == 1006) t->mouse_sgr = on;
            }
        }
        break; }
    default: break;
    }
}

static void osc_handle(term_emulator *t)
{
    char *semi = strchr(t->osc, ';');
    if (!semi) return;
    if ((t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';') {
        snprintf(t->title, sizeof(t->title), "%s", semi + 1);
        t->title_changed = 1;
    }
}

static void reset_terminal(term_emulator *t)
{
    int cols = t->main_screen.cols, rows = t->main_screen.rows;
    uint32_t fg = t->default_fg, bg = t->default_bg;
    for (int i = 0; i < t->history_count; i++) line_free(&t->history[i]);
    screen_free(&t->main_screen); screen_free(&t->alt_screen);
    memset(&t->alt_screen, 0, sizeof(t->alt_screen));
    t->history_count = 0; t->scroll_offset = 0; t->alt_active = 0;
    t->fg = fg; t->bg = bg; t->attrs = 0;
    t->cursor_visible = 1; t->autowrap = 1; t->bracketed_paste = 0;
    t->application_cursor = 0; t->mouse_mode = 0; t->mouse_sgr = 0;
    screen_init(&t->main_screen, cols, rows, blank_cell(t));
    t->screen = &t->main_screen;
}

static void normal_byte(term_emulator *t, uint32_t cp)
{
    term_screen *s = t->screen;
    if (cp == 0x1b) { t->parser_state = P_ESC; return; }
    if (cp == '\n' || cp == '\v' || cp == '\f') { linefeed(t); return; }
    if (cp == '\r') { s->cx = 0; s->wrap_pending = 0; return; }
    if (cp == '\b') {
        if (s->wrap_pending) s->wrap_pending = 0;
        else if (s->cx > 0) s->cx--;
        return;
    }
    if (cp == '\t') {
        s->cx = (s->cx + 8) & ~7;
        if (s->cx >= s->cols) s->cx = s->cols - 1;
        s->wrap_pending = 0; return;
    }
    if (cp < 0x20 || cp == 0x7f) return;
    put_codepoint(t, cp);
}

void term_init(term_emulator *t, int cols, int rows, uint32_t fg, uint32_t bg)
{
    memset(t, 0, sizeof(*t));
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    t->default_fg = fg; t->default_bg = bg; t->fg = fg; t->bg = bg;
    t->cursor_visible = 1; t->autowrap = 1;
    screen_init(&t->main_screen, cols, rows, blank_cell(t));
    t->screen = &t->main_screen;
    snprintf(t->title, sizeof(t->title), "Terminal");
}

void term_free(term_emulator *t)
{
    screen_free(&t->main_screen); screen_free(&t->alt_screen);
    for (int i = 0; i < t->history_count; i++) line_free(&t->history[i]);
    t->history_count = 0;
}

void term_resize(term_emulator *t, int cols, int rows)
{
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    term_cell blank = blank_cell(t);
    screen_resize(t, &t->main_screen, cols, rows, blank);
    if (t->alt_screen.lines) screen_resize(t, &t->alt_screen, cols, rows, blank);
    if (t->scroll_offset > t->history_count) t->scroll_offset = t->history_count;
    t->dirty = 1;
}

void term_scroll(term_emulator *t, int older_lines)
{
    if (t->alt_active) { t->scroll_offset = 0; return; }
    t->scroll_offset += older_lines;
    if (t->scroll_offset < 0) t->scroll_offset = 0;
    if (t->scroll_offset > t->history_count) t->scroll_offset = t->history_count;
    t->dirty = 1;
}

int term_view_first(const term_emulator *t)
{
    int first = t->history_count - t->scroll_offset;
    return first < 0 ? 0 : first;
}

const term_line *term_view_line(const term_emulator *t, int view_row)
{
    const term_screen *s = t->screen;
    if (view_row < 0 || view_row >= s->rows) return NULL;
    int index = term_view_first(t) + view_row;
    if (index < 0 || index >= t->history_count + s->rows) return NULL;
    if (index < t->history_count) return &t->history[index];
    return &s->lines[index - t->history_count];
}

void term_feed(term_emulator *t, const unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char b = data[i];
        t->dirty = 1;
        if (t->parser_state == P_NORMAL) {
            if (t->utf8_need) {
                if ((b & 0xc0) == 0x80) {
                    t->utf8_acc = (t->utf8_acc << 6) | (b & 0x3f);
                    if (--t->utf8_need == 0) {
                        if (t->utf8_acc < t->utf8_min || t->utf8_acc > 0x10ffff ||
                            (t->utf8_acc >= 0xd800 && t->utf8_acc <= 0xdfff))
                            normal_byte(t, 0xfffd);
                        else normal_byte(t, t->utf8_acc);
                    }
                } else {
                    t->utf8_need = 0;
                    normal_byte(t, 0xfffd);
                    i--;
                }
                continue;
            }
            if (b < 0x80) { normal_byte(t, b); continue; }
            if ((b & 0xe0) == 0xc0) { t->utf8_need = 1; t->utf8_min = 0x80; t->utf8_acc = b & 0x1f; }
            else if ((b & 0xf0) == 0xe0) { t->utf8_need = 2; t->utf8_min = 0x800; t->utf8_acc = b & 0x0f; }
            else if ((b & 0xf8) == 0xf0) { t->utf8_need = 3; t->utf8_min = 0x10000; t->utf8_acc = b & 7; }
            else normal_byte(t, 0xfffd);
            continue;
        }
        if (t->parser_state == P_ESC) {
            if (b == '[') { t->parser_state = P_CSI; t->csi_len = 0; t->csi[0] = 0; }
            else if (b == ']') { t->parser_state = P_OSC; t->osc_len = 0; t->osc[0] = 0; }
            else if (b == '7') { save_cursor(t); t->parser_state = P_NORMAL; }
            else if (b == '8') { restore_cursor(t); t->parser_state = P_NORMAL; }
            else if (b == 'D') { linefeed(t); t->parser_state = P_NORMAL; }
            else if (b == 'M') { reverse_linefeed(t); t->parser_state = P_NORMAL; }
            else if (b == 'E') { t->screen->cx = 0; linefeed(t); t->parser_state = P_NORMAL; }
            else if (b == 'c') { reset_terminal(t); t->parser_state = P_NORMAL; }
            else if (b == '(' || b == ')') { t->parser_state = P_CHARSET; }
            else t->parser_state = P_NORMAL;
            continue;
        }
        if (t->parser_state == P_CHARSET) { t->parser_state = P_NORMAL; continue; }
        if (t->parser_state == P_CSI) {
            if (b >= 0x40 && b <= 0x7e) {
                t->csi[t->csi_len] = 0;
                csi_handle(t, b);
                t->parser_state = P_NORMAL;
            } else if (t->csi_len < (int)sizeof(t->csi) - 1) {
                t->csi[t->csi_len++] = (char)b;
            }
            continue;
        }
        if (t->parser_state == P_OSC) {
            if (b == 7) { t->osc[t->osc_len] = 0; osc_handle(t); t->parser_state = P_NORMAL; }
            else if (b == 0x1b) t->parser_state = P_OSC_ESC;
            else if (t->osc_len < (int)sizeof(t->osc) - 1) t->osc[t->osc_len++] = (char)b;
            continue;
        }
        if (t->parser_state == P_OSC_ESC) {
            if (b == '\\') { t->osc[t->osc_len] = 0; osc_handle(t); t->parser_state = P_NORMAL; }
            else { t->parser_state = P_OSC; }
        }
    }
}
