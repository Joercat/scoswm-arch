/* The text cell, in one place, because wrapping is arithmetic on it.
 *
 * The font is a fixed 8x16 bitmap cell, which is what makes a wrapped layout exact rather than approximate:
 * a row holds `width / FONT_W' characters, no measurement and no per-glyph advance are involved, and so the
 * number of rows a string needs - the number a notification's height and a terminal's scroll range are
 * computed from - is the same number the painter produces.  If the font ever becomes proportional, this
 * header is where a measurement function belongs, and every caller of s_text_cols keeps working.
 */
#ifndef SCOS_TEXT_H
#define SCOS_TEXT_H

#define FONT_W 8
#define FONT_H 16

#endif
