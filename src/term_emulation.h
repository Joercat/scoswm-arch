/* Small, display-independent VT100/xterm screen model used by scos-terminal.
 * No X11 or process code belongs here: the unit tests feed bytes directly. */
#ifndef SCOS_TERM_EMULATION_H
#define SCOS_TERM_EMULATION_H

#include <stddef.h>
#include <stdint.h>

#define TERM_HISTORY_LIMIT 1024
#define TERM_MAX_PARAMS 32
#define TERM_TITLE_MAX 128

#define TERM_ATTR_BOLD      1u
#define TERM_ATTR_DIM       2u
#define TERM_ATTR_UNDERLINE 4u
#define TERM_ATTR_REVERSE   8u
#define TERM_ATTR_BLINK    16u

typedef struct {
    uint32_t ch;                 /* Unicode scalar; 0 = wide-char continuation */
    uint32_t fg, bg;             /* 0xRRGGBB */
    unsigned char attrs;
} term_cell;

typedef struct {
    term_cell *cells;
    int len, cap;
} term_line;

typedef struct {
    term_line *lines;
    int cols, rows;
    int cx, cy;
    int saved_cx, saved_cy;
    int scroll_top, scroll_bottom;
    int wrap_pending;
} term_screen;

typedef struct {
    term_screen main_screen, alt_screen;
    term_screen *screen;
    int alt_active;
    int alt_saved_cx, alt_saved_cy;

    term_line history[TERM_HISTORY_LIMIT];
    int history_count;
    int scroll_offset;            /* 0 = live bottom; positive = lines back */

    uint32_t default_fg, default_bg;
    uint32_t fg, bg;
    unsigned char attrs;
    int cursor_visible, autowrap, bracketed_paste;
    int application_cursor;
    int mouse_mode;                /* 1000, 1002 or 1003; 0 = no mouse reports */
    int mouse_sgr;                 /* 1006: extended SGR mouse coordinates */

    char title[TERM_TITLE_MAX];
    int title_changed;
    char reply[256];              /* device-status replies for the PTY */
    size_t reply_len;
    int dirty;

    int parser_state;
    uint32_t utf8_acc, utf8_min;
    int utf8_need;
    char csi[256];
    int csi_len;
    char osc[512];
    int osc_len;
} term_emulator;

void term_init(term_emulator *t, int cols, int rows, uint32_t fg, uint32_t bg);
void term_free(term_emulator *t);
void term_resize(term_emulator *t, int cols, int rows);
void term_feed(term_emulator *t, const unsigned char *data, size_t len);
void term_scroll(term_emulator *t, int older_lines);
const term_line *term_view_line(const term_emulator *t, int view_row);
int term_view_first(const term_emulator *t);

#endif
