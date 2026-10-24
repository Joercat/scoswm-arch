#include "term_emulation.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; \
} } while (0)

static void feed(term_emulator *t, const char *s)
{
    term_feed(t, (const unsigned char *)s, strlen(s));
}

static void line_text(const term_line *l, char *out, int cap)
{
    int n = 0;
    if (!l) { if (cap > 0) out[0] = 0; return; }
    for (int i = 0; i < l->len && n < cap - 1; i++) {
        uint32_t ch = l->cells[i].ch;
        out[n++] = ch > 0 && ch < 127 ? (char)ch : ' ';
    }
    out[n] = 0;
}

static void expect_line(term_emulator *t, int row, const char *prefix)
{
    char text[128];
    line_text(term_view_line(t, row), text, sizeof(text));
    CHECK(!strncmp(text, prefix, strlen(prefix)));
}

int main(void)
{
    term_emulator t;
    term_init(&t, 8, 3, 0x39ff14, 0x1a1a1a);
    feed(&t, "abc");
    CHECK(t.screen->cx == 3);
    expect_line(&t, 0, "abc");

    feed(&t, "\r\n1234567890");
    expect_line(&t, 1, "12345678");
    expect_line(&t, 2, "90");
    CHECK(t.screen->cx == 2);

    term_free(&t);
    term_init(&t, 8, 3, 0x39ff14, 0x1a1a1a);
    feed(&t, "1\r\n2\r\n3\r\n4");
    CHECK(t.history_count == 1);
    expect_line(&t, 0, "2");
    expect_line(&t, 1, "3");
    expect_line(&t, 2, "4");
    term_scroll(&t, 1);
    CHECK(t.scroll_offset == 1);
    expect_line(&t, 0, "1");
    expect_line(&t, 1, "2");
    expect_line(&t, 2, "3");
    term_scroll(&t, -1);
    CHECK(t.scroll_offset == 0);

    feed(&t, "\033[1;1H\033[31mR\033[0m");
    const term_line *line = term_view_line(&t, 0);
    CHECK(line->cells[0].ch == 'R');
    CHECK(line->cells[0].fg == 0xcc0000);
    feed(&t, "\033[2;3H\033[38;2;1;2;3mT");
    line = term_view_line(&t, 1);
    CHECK(line->cells[2].ch == 'T' && line->cells[2].fg == 0x010203);
    feed(&t, "\033[2J");
    CHECK(t.screen->cy == 1 && t.screen->cx == 3);
    expect_line(&t, 0, "        ");

    feed(&t, "\033[1;1Hcaf\303\251");
    line = term_view_line(&t, 0);
    CHECK(line->cells[3].ch == 0xe9);

    term_scroll(&t, 1);
    feed(&t, "\033[1;1Hmain\033[?1049h");
    CHECK(t.alt_active && t.screen == &t.alt_screen);
    CHECK(t.scroll_offset == 0);
    expect_line(&t, 0, "        ");
    term_scroll(&t, 1);
    CHECK(t.scroll_offset == 0);
    feed(&t, "vim");
    expect_line(&t, 0, "vim");
    feed(&t, "\033[?1049l");
    CHECK(!t.alt_active && t.screen == &t.main_screen);
    CHECK(t.scroll_offset == 0);
    line = term_view_line(&t, 0);
    CHECK(line->cells[0].ch == 'm' && line->cells[1].ch == 'a');

    feed(&t, "\033[?1h\033[?1002h\033[?1006h");
    CHECK(t.application_cursor && t.mouse_mode == 1002 && t.mouse_sgr);
    feed(&t, "\033[?1006l");
    CHECK(t.mouse_mode == 1002 && !t.mouse_sgr);
    feed(&t, "\033[?1002l\033[?1l");
    CHECK(!t.application_cursor && !t.mouse_mode);

    feed(&t, "\033]0;bash - tests\007");
    CHECK(t.title_changed && !strcmp(t.title, "bash - tests"));
    t.reply_len = 0; t.reply[0] = 0;
    feed(&t, "\033[6n");
    CHECK(strstr(t.reply, "R") != NULL);
    CHECK(t.reply[0] == 033);

    term_resize(&t, 12, 4);
    CHECK(t.screen->cols == 12 && t.screen->rows == 4);
    CHECK(t.main_screen.lines[0].cap == 12);
    term_free(&t);

    term_init(&t, 5, 3, 0x39ff14, 0x1a1a1a);
    feed(&t, "one\r\ntwo\r\nthree");
    term_resize(&t, 5, 2);
    CHECK(t.history_count == 1);
    expect_line(&t, 0, "two  ");
    expect_line(&t, 1, "three");
    CHECK(t.screen->cy == 1);
    CHECK(t.history[0].cells[0].ch == 'o');
    term_free(&t);
    CHECK(t.main_screen.lines == NULL && t.alt_screen.lines == NULL);

    printf("test_terminal: %d checks, %d failures\n", checks, failures);
    if (failures) return 1;
    puts("test_terminal: all checks passed");
    return 0;
}
