/* Flowing text: which characters go on which row.  See kernel/desktop/textwrap.c.
 *
 * Two kinds of text exist on this desktop.  A label in a fixed cell - a taskbar button, a table column -
 * has to clip, because the cell has the size it has and a name that overflows it is a fact about the name.
 * Anything a person *reads* has no such excuse: output, reports and notifications must show all of their
 * text, which means the widget has to grow to fit, and growing needs the row count, not just the rows.
 * Both are here so the widget and the painter cannot disagree about how many lines there are.
 *
 * This header has no dependencies beyond the text cell, which is what lets the host test compile the
 * layout on its own: `tools/tests/textwrap_host.c'.
 */
#ifndef SCOS_TEXTWRAP_H
#define SCOS_TEXTWRAP_H

#include "text.h"

/* Columns a row of `width' pixels holds, never less than one. */
int  s_text_cols(int width);

/* One row of a word-wrapped layout.  `*rowlen' is how many characters to draw, `*advance' how far to move
 * through the string - they differ by the space, tab or newline the break consumed.  Returns 0 when the
 * text is over, which is also how a caller knows to stop: a row of zero length is a blank line, drawn as
 * nothing and still occupying a row. */
int  s_wrap_next(const char *text, int cols, int *rowlen, int *advance);

/* How many rows this text needs at this width - what a box sizes itself by. */
int  s_wrap_rows(const char *text, int cols);

/* Copy the next row into `out' for drawing.  Returns the text after the row, or 0 when exhausted. */
const char *s_wrap_row(const char *text, int cols, char *out, int out_size);

#endif
