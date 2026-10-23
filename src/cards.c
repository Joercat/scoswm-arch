/*
 * SCos native - shared procedural playing-card rendering.
 * Used by Blackjack and Klondike Solitaire: white face with rank + suit
 * pips, or a hatched back for face-down cards.
 */
#include "scos.h"

#define CARD_W 56
#define CARD_H 80

int card_w(void) { return CARD_W; }
int card_h(void) { return CARD_H; }

const char *card_rank_str(int r)
{
    static const char *n[] = { "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" };
    return n[r - 1];
}

static void draw_suit(struct surface *s, int x, int y, int suit, u32 c, int r)
{
    if (suit == 1) {                                  /* diamond */
        for (int dy = -r; dy <= r; dy++) {
            int w = r - (dy < 0 ? -dy : dy);
            s_fill(s, x - w, y + dy, w * 2 + 1, 1, c);
        }
    } else if (suit == 0) {                           /* heart */
        s_disc(s, x - r / 2, y - r / 4, r / 2 + 1, c);
        s_disc(s, x + r / 2, y - r / 4, r / 2 + 1, c);
        for (int dy = 0; dy <= r; dy++) {
            int w = r - dy;
            s_fill(s, x - w, y + dy, w * 2 + 1, 1, c);
        }
    } else if (suit == 2) {                           /* spade */
        for (int dy = -r; dy <= 0; dy++) {
            int w = r + dy;                    /* point up, widen downwards */
            s_fill(s, x - w, y + dy, w * 2 + 1, 1, c);
        }
        s_disc(s, x - r / 2, y + r / 4, r / 2, c);
        s_disc(s, x + r / 2, y + r / 4, r / 2, c);
        s_fill(s, x - 1, y + r / 2, 3, r / 2 + 2, c);
    } else {                                          /* club */
        s_disc(s, x, y - r / 2, r / 2 + 1, c);
        s_disc(s, x - r / 2 - 1, y + r / 4, r / 2 + 1, c);
        s_disc(s, x + r / 2 + 1, y + r / 4, r / 2 + 1, c);
        s_fill(s, x - 1, y + r / 4, 3, r, c);
    }
}

void card_draw(struct surface *s, int x, int y, u8 card, int face_down,
             u32 back_color)
{
    if (face_down) {
        s_fill(s, x, y, CARD_W, CARD_H, back_color);
        s_frame_rect(s, x, y, CARD_W, CARD_H, 0xFFFFFF);
        for (int i = 4; i < CARD_W - 4; i += 6)
            s_line(s, x + i, y + 4, x + i, y + CARD_H - 5, (back_color >> 1) & 0x7F7F7F);
        s_frame_rect(s, x + 2, y + 2, CARD_W - 4, CARD_H - 4, 0xFFFFFF);
        return;
    }
    int rank = (card % 13) + 1, suit = card / 13;
    u32 col = (suit == 0 || suit == 1) ? 0xCC2222 : 0x101010;
    s_fill(s, x, y, CARD_W, CARD_H, 0xFAFAFA);
    s_frame_rect(s, x, y, CARD_W, CARD_H, 0x404040);
    s_text(s, x + 5, y + 5, card_rank_str(rank), col);
    draw_suit(s, x + 10, y + 30, suit, col, 5);
    draw_suit(s, x + CARD_W / 2, y + CARD_H / 2 + 6, suit, col, 12);
}
