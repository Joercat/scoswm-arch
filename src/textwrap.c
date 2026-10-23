/* Where a piece of text has to break.
 *
 * The desktop drew text one way for a long time: take as many characters as fit and overwrite the last
 * three with dots.  That is right for a label in a fixed cell - a taskbar button has the width it has, and
 * a name that does not fit is a fact about the name, not about the system - and wrong for anything a person
 * reads.  On a command's output, a diagnostic panel or a notification, cutting the sentence cuts the
 * information, and the second half of a long line is where the answer usually is: the line that ends
 * "GPU acceleration: driver module nvidia (family nvidia) read this chip's own registers and reports what
 * it found; it offers ..." is exactly the line whose ending mattered.
 *
 * So flowing text is laid out here, in rows, and the widgets that show it size themselves from the same
 * count - `s_wrap_rows' is what a notification's height is made of and what the terminal's scrollbar counts
 * - because a wrapped row that no one made room for is a row that is not drawn.  One algorithm, shared: two
 * implementations of word wrapping drift, and the drift shows up as a box that clipped text it had already
 * counted, or a panel that reserved a row it never painted.
 *
 * This file is also deliberately free of surfaces and state: the layout is arithmetic on a string, which is
 * why it can be - and is - tested on the host (`tools/tests/textwrap_host.c`) against cases a running
 * desktop cannot show you, like a word longer than a row and a blank line inside a paragraph.
 */
#include "textwrap.h"

int s_text_cols(int width)
{
    int cols = width / FONT_W;
    return cols < 1 ? 1 : cols;
}

/* One row of a word-wrapped layout.  A row is filled to `cols' characters and then ended at the last space
 * it holds, so a word moves to the next row whole; a word that alone exceeds a row is split instead, because
 * any other choice would leave the walk where it started.  `*rowlen' is how much to draw and `*advance' how
 * far to move - they differ by the space, tab or newline that the break consumed, which is not drawn twice
 * at the top of the next row.  Returns 0 once the text is over.  A newline produces a row of its own, so a
 * blank line inside a report stays a blank line rather than being swallowed. */
int s_wrap_next(const char *text, int cols, int *rowlen, int *advance)
{
    int n = 0, space = -1;

    if (cols < 1) cols = 1;
    if (!text || !*text) return 0;
    while (n < cols && text[n] && text[n] != '\n') {
        if (text[n] == ' ' || text[n] == '\t') space = n;
        n++;
    }
    /* Only back off to that last space when the character the row stopped on would otherwise be split in
     * the middle of a word; a row that ends where a word ends is a full row, not a broken one. */
    if (n == cols && text[n] && text[n] != '\n' && text[n] != ' ' && text[n] != '\t' && space > 0)
        n = space;
    *rowlen = n;
    *advance = n;
    while (text[*advance] == ' ' || text[*advance] == '\t') (*advance)++;
    if (text[*advance] == '\n') (*advance)++;
    return 1;
}

/* How many rows this text needs at this width - the number every sizing caller asks for. */
int s_wrap_rows(const char *text, int cols)
{
    int rows = 0, len, adv;
    const char *p = text;

    while (s_wrap_next(p, cols, &len, &adv)) { rows++; p += adv; }
    return rows;
}

/* Copy one row into `out' for drawing.  Returns the text after the row, or 0 at the end, which lets a
 * painter loop without knowing anything about how rows are chosen. */
const char *s_wrap_row(const char *text, int cols, char *out, int out_size)
{
    int len, adv;

    if (out_size <= 0) return text;
    if (!s_wrap_next(text, cols, &len, &adv)) { out[0] = 0; return 0; }
    if (len > out_size - 1) len = out_size - 1;
    for (int i = 0; i < len; i++) out[i] = text[i];
    out[len] = 0;
    /* 0 and a pointer at the terminator are the same fact and different contracts: a caller loops `while
     * (p)', so reporting the end as a valid position would make it paint one empty row after every string -
     * and, for anything sizing itself by counting rows, reserve a row that has nothing in it. */
    const char *after = text + adv;
    return *after ? after : 0;
}
