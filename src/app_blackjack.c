/*
 * SCos native - Blackjack.
 *
 * A real card game against the dealer: 52-card deck shuffled with a
 * hardware-timed seed (PIT tick + CMOS seconds), cards drawn procedurally
 * (rank + suit pips), dealer stands on 17, blackjacks pay 3:2 in the tally.
 * Mouse buttons or the H / S / N keys play.
 */
#include "scos.h"

#define CARD_W 56
#define CARD_H 80

#define MAXC 10

struct bj {
    u8 deck[52];
    int top;
    u8 player[MAXC], dealer[MAXC];
    int pn, dn;
    int state;                 /* 0 = playing, 1 = finished */
    int result;                /* 1 win, 2 loss, 3 push, 4 blackjack */
    int wins, losses, pushes;
    u32 seed;
    int hover;
    struct window *win;        /* appstrt console + memory attribution */
};

static u32 bj_rand(struct bj *b)
{
    b->seed ^= b->seed << 13;
    b->seed ^= b->seed >> 17;
    b->seed ^= b->seed << 5;
    return b->seed;
}

static void bj_shuffle(struct bj *b)
{
    for (int i = 0; i < 52; i++) b->deck[i] = (u8)i;
    for (int i = 51; i > 0; i--) {
        u32 j = bj_rand(b) % (u32)(i + 1);
        u8 t = b->deck[i]; b->deck[i] = b->deck[j]; b->deck[j] = t;
    }
    b->top = 0;
}

static u8 bj_draw(struct bj *b)
{
    if (b->top >= 52) bj_shuffle(b);
    return b->deck[b->top++];
}

static int card_value(u8 c)
{
    int r = (c % 13) + 1;
    if (r == 1) return 11;
    if (r > 10) return 10;
    return r;
}

static int hand_value(const u8 *h, int n)
{
    int tot = 0, aces = 0;
    for (int i = 0; i < n; i++) {
        tot += card_value(h[i]);
        if ((h[i] % 13) + 1 == 1) aces++;
    }
    while (tot > 21 && aces) { tot -= 10; aces--; }
    return tot;
}

static void bj_new_round(struct bj *b)
{
    bj_shuffle(b);
    app_log(b->win, "new round dealt");
    b->pn = b->dn = 0;
    b->player[b->pn++] = bj_draw(b);
    b->dealer[b->dn++] = bj_draw(b);
    b->player[b->pn++] = bj_draw(b);
    b->dealer[b->dn++] = bj_draw(b);
    b->state = 0;
    b->result = 0;
    if (hand_value(b->player, b->pn) == 21) {
        b->state = 1;
        b->result = 4;
        b->wins++;
    }
}

static void bj_finish(struct bj *b, int result)
{
    b->state = 1;
    b->result = result;
    if (result == 1 || result == 4) b->wins++;
    else if (result == 2) b->losses++;
    else b->pushes++;
    /* real outcome to the launching console */
    int p = hand_value(b->player, b->pn), d = hand_value(b->dealer, b->dn);
    char lg[80], n[8];
    if (result == 4) strcpy(lg, "blackjack! you win 3:2");
    else if (result == 1) {
        strcpy(lg, d > 21 ? "dealer busts - you win " : "you win ");
        fmt_u32(n, (u32)p); strcat(lg, n); strcat(lg, " vs ");
        fmt_u32(n, (u32)d); strcat(lg, n);
    } else if (result == 2) {
        strcpy(lg, p > 21 ? "you bust with " : "dealer wins ");
        fmt_u32(n, (u32)(p > 21 ? p : d)); strcat(lg, n);
        if (p <= 21) { strcat(lg, " vs "); fmt_u32(n, (u32)p); strcat(lg, n); }
    } else strcpy(lg, "push - bet returned");
    app_log(b->win, lg);
}

static void bj_stand(struct bj *b)
{
    if (b->state) return;
    while (hand_value(b->dealer, b->dn) < 17) b->dealer[b->dn++] = bj_draw(b);
    int p = hand_value(b->player, b->pn), d = hand_value(b->dealer, b->dn);
    if (d > 21 || p > d) bj_finish(b, 1);
    else if (p < d) bj_finish(b, 2);
    else bj_finish(b, 3);
}

static void bj_hit(struct bj *b)
{
    if (b->state || b->pn >= MAXC) return;
    b->player[b->pn++] = bj_draw(b);
    int p = hand_value(b->player, b->pn);
    if (p > 21) bj_finish(b, 2);
    else if (p == 21) bj_stand(b);
}

/* ------------------------------------------------------------ drawing ---- */
static const char *BTN_LABELS[3] = { "New Round", "Hit", "Stand" };

static void bj_paint(struct window *w)
{
    struct bj *b = w->data;
    struct surface *s = &w->surf;
    const struct theme *t = theme_current();
    s_fill(s, 0, 0, s->w, s->h, color_blend(t->win_bg,t->main,12));
    /* felt texture */
    for (int yy = 8; yy < s->h; yy += 16)
        s_fill(s, 0, yy, s->w, 1, color_blend(t->win_bg,t->main,18));

    char line[96];
    strcpy(line, "Blackjack - dealer stands on 17");
    s_clip_text(s,12,8,"Blackjack",t->main,100);
    strcpy(line, "Won: ");
    char n[8];
    fmt_u32(n, (u32)b->wins); strcat(line, n);
    strcat(line, "   Lost: ");
    fmt_u32(n, (u32)b->losses); strcat(line, n);
    strcat(line, "   Push: ");
    fmt_u32(n, (u32)b->pushes); strcat(line, n);
    s_clip_text(s,120,8,line,t->text,s->w-132);

    int ds=(s->w-160-CARD_W)/(b->dn>1?b->dn-1:1),ps=(s->w-160-CARD_W)/(b->pn>1?b->pn-1:1);if(ds>CARD_W+10)ds=CARD_W+10;if(ps>CARD_W+10)ps=CARD_W+10;
    s_text(s, 12, 34, "Dealer", t->text);
    for (int i = 0; i < b->dn; i++)
        card_draw(s, 70 + i * ds, 28, b->dealer[i],
                  (i == 1 && !b->state), t->main);
    if (b->state || b->dn) {
        strcpy(line, "  = ");
        fmt_u32(n, (u32)((b->state) ? hand_value(b->dealer, b->dn)
                                    : hand_value(b->dealer, 1)));
        strcat(line, n);
        s_text(s, s->w-76, 60, line, t->text);
    }

    s_text(s, 12, 140, "You", t->text);
    for (int i = 0; i < b->pn; i++)
        card_draw(s, 70 + i * ps, 134, b->player[i], 0, t->main);
    strcpy(line, "  = ");
    fmt_u32(n, (u32)hand_value(b->player, b->pn));
    strcat(line, n);
    s_text(s, s->w-76, 166, line, t->text);

    if (b->state) {
        const char *msg =
            b->result == 1 ? "You win!" :
            b->result == 2 ? "Dealer wins." :
            b->result == 3 ? "Push - nobody wins." :
                             "BLACKJACK! You win!";
        u32 mc = b->result == 2 ? 0xFF6666 : t->main;
        int tw = s_text_width(msg);
        s_fill(s, (s->w - tw) / 2 - 10, 240, tw + 20, 28, t->win_bg);
        s_frame_rect(s, (s->w - tw) / 2 - 10, 240, tw + 20, 28, mc);
        s_text(s, (s->w - tw) / 2, 246, msg, mc);
    }

    for (int i = 0; i < 3; i++) {
        int bx = 12 + i * 104, by = s->h - 40;
        int enabled = (i == 0) || (b->state == 0 && i > 0);
        u32 bg = b->hover == i && enabled ? t->main : color_blend(t->win_bg,t->main,15);
        u32 fg = b->hover == i && enabled ? t->title_text : (enabled ? t->main : color_blend(t->text,t->win_bg,55));
        s_fill(s, bx, by, 96, 28, bg);
        s_frame_rect(s, bx, by, 96, 28, fg);
        s_text(s, bx + (96 - s_text_width(BTN_LABELS[i])) / 2, by + 6, BTN_LABELS[i], fg);
    }
    s_clip_text(s,340,s->h-32,"N new, H hit, S stand",t->text,s->w-352);
}

static void bj_action(struct bj *b, int i)
{
    if (i == 0) bj_new_round(b);
    else if (i == 1 && !b->state) bj_hit(b);
    else if (i == 2 && !b->state) bj_stand(b);
}

static void bj_mouse(struct window *w, struct mouse_event *e, int x, int y)
{
    struct bj *b = w->data;
    int old = b->hover;
    b->hover = -1;
    for (int i = 0; i < 3; i++) {
        int bx = 12 + i * 104, by = w->surf.h - 40;
        if (x >= bx && x < bx + 96 && y >= by && y < by + 28) b->hover = i;
    }
    if (e->type == MEV_BUTTON && e->down && e->button == MBTN_LEFT && b->hover >= 0)
        bj_action(b, b->hover);
    if (old != b->hover) wm_redraw(w);
    else if (e->type == MEV_BUTTON) wm_redraw(w);
}

static void bj_key(struct window *w, struct key_event *e)
{
    struct bj *b = w->data;
    if (!e->pressed) return;
    if (e->keycode == 'n' || e->keycode == 'N') bj_action(b, 0);
    else if (e->keycode == 'h' || e->keycode == 'H') bj_action(b, 1);
    else if (e->keycode == 's' || e->keycode == 'S') bj_action(b, 2);
    wm_redraw(w);
}

static void bj_open(struct window *w, void *arg)
{
    (void)arg;
    struct bj *b = palloc(sizeof(*b));
    if (!b) return;              /* OOM: wm_open_app reports it centrally */
    memset(b, 0, sizeof(*b));
    struct rtc_time rt;
    rtc_read(&rt);
    b->seed = (u32)tick_count * 2654435761u ^ (u32)rt.sec * 40503u ^ 0x9E3779B9u;
    if (!b->seed) b->seed = 1;
    b->hover = -1;
    b->win = w;
    w->data = b;
    wm_track_mem(w, (int)sizeof(*b));
    bj_new_round(b);
}

static void bj_close(struct window *w)
{
    if (w->data) pfree(w->data, sizeof(struct bj));
}

struct app app_blackjack = {
    .desktop_label = "Blackjack",
    .uses_data = 1, .id = "blackjack", .title = "Blackjack", .icon = ICON_CARDS, .single = 1,
    .def_w = 640, .def_h = 480,.min_w=600,.min_h=400,
    .open = bj_open, .paint = bj_paint, .key = bj_key,
    .mouse = bj_mouse, .close = bj_close,
};

SCOS_APP(app_blackjack, 007);
