/*
 * SCos native - Klondike Solitaire.
 *
 * Full rules: 7 tableau piles (descending, alternating colours), four
 * foundations (ascending per suit, aces first), stock with recycle,
 * face-down cards flip when exposed, stack moves between tableau piles,
 * win detection. Cards use the shared procedural painter (cards.c).
 * Mouse: click a card to pick it up (stacks included), click a target
 * pile to drop; click the stock to draw. Keys: N = new game.
 */
#include "scos.h"

#define CW 56
#define CH 80
/*
 * Responsive layout: column pitch and vertical origin adapt to the window
 * content size so a maximised window spreads the board instead of leaving
 * it huddled in the top-left corner.
 */
struct sollay { int x0, colw, topy, taby; };
static struct sollay sol_L;
static void sol_layout(struct window *w, struct sollay *L)
{
    int cw = wm_content_w(w), ch = wm_content_h(w);
    int colw = (cw - 24) / 7;
    if (colw < 60) colw = 60;
    if (colw > 140) colw = 140;
    int x0 = (cw - colw * 7) / 2 + (colw - CW) / 2;
    if (x0 < 4) x0 = 4;
    int yoff = (ch - 520) / 3;
    if (yoff < 0) yoff = 0;
    if (yoff > 160) yoff = 160;
    L->x0 = x0; L->colw = colw; L->topy = 8 + yoff; L->taby = 104 + yoff;
}
#define COLX(c) (sol_L.x0 + (c) * sol_L.colw)
#define TOP_Y (sol_L.topy)
#define TAB_Y (sol_L.taby)
#define DOWN_OFF 12
#define UP_OFF 22

struct sol {
    u8 tab[7][24];  u8 tabup[7][24]; int tabn[7];
    u8 stock[52];   int stockn;
    u8 waste[52];   int wasten;
    u8 found[4][13]; int foundn[4];
    int sel_src;    /* -1 none, 0..6 tableau, 7 waste, 8..11 foundation */
    int sel_idx;
    int hover;      /* column under mouse, -1 none */
    int hover_btn;
    int moves;
    int won;
    u32 seed;
    u32 t0;
    struct window *owner;      /* appstrt console + memory attribution */
};

static u32 sl_rand(struct sol *g)
{
    g->seed ^= g->seed << 13; g->seed ^= g->seed >> 17; g->seed ^= g->seed << 5;
    return g->seed;
}

static int is_red(u8 c) { int s = c / 13; return s == 0 || s == 1; }
static int rank_of(u8 c) { return (c % 13) + 1; }

static void sl_new(struct sol *g)
{
    u8 deck[52];
    for (int i = 0; i < 52; i++) deck[i] = (u8)i;
    for (int i = 51; i > 0; i--) {
        u32 j = sl_rand(g) % (u32)(i + 1);
        u8 t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    memset(g->tabn, 0, sizeof(g->tabn));
    memset(g->foundn, 0, sizeof(g->foundn));
    g->stockn = g->wasten = 0;
    int d = 0;
    for (int c = 0; c < 7; c++)
        for (int k = 0; k <= c; k++) {
            g->tab[c][g->tabn[c]] = deck[d++];
            g->tabup[c][g->tabn[c]] = (k == c);
            g->tabn[c]++;
        }
    while (d < 52) g->stock[g->stockn++] = deck[d++];
    g->sel_src = -1; g->sel_idx = 0;
    g->moves = 0; g->won = 0;
    g->t0 = (u32)(uptime_ms() / 1000);
    app_log(g->owner, "new game dealt");
}

static void sl_check_win(struct sol *g)
{
    int was = g->won;
    g->won = (g->foundn[0] + g->foundn[1] + g->foundn[2] + g->foundn[3]) == 52;
    if (g->won && !was) {
        char lg[80], n[12];
        strcpy(lg, "game won in ");
        fmt_u32(n, (u32)g->moves); strcat(lg, n); strcat(lg, " moves, ");
        fmt_u32(n, (u32)(uptime_ms() / 1000 - g->t0)); strcat(lg, n);
        strcat(lg, " s");
        app_log(g->owner, lg);
    }
}

static void sl_flip_top(struct sol *g, int c)
{
    if (g->tabn[c] && !g->tabup[c][g->tabn[c] - 1])
        g->tabup[c][g->tabn[c] - 1] = 1;
}

/* can stack card..end of source be placed on tableau column c? */
static int sl_tab_ok(struct sol *g, int c, u8 card)
{
    if (!g->tabn[c]) return rank_of(card) == 13;
    int ti = g->tabn[c] - 1;
    if (!g->tabup[c][ti]) return 0;
    u8 top = g->tab[c][ti];
    return rank_of(top) == rank_of(card) + 1 && is_red(top) != is_red(card);
}

static int sl_found_ok(struct sol *g, int f, u8 card)
{
    return card / 13 == (u8)f && rank_of(card) == g->foundn[f] + 1;
}

/* move current selection onto tableau column c */
static void sl_move_tab(struct sol *g, int c)
{
    int n;
    u8 buf[24];
    if (g->sel_src >= 0 && g->sel_src <= 6) {
        int s = g->sel_src;
        n = g->tabn[s] - g->sel_idx;
        for (int i = 0; i < n; i++) buf[i] = g->tab[s][g->sel_idx + i];
        if (!sl_tab_ok(g, c, buf[0])) return;
        g->tabn[s] = g->sel_idx;
        sl_flip_top(g, s);
    } else if (g->sel_src == 7) {
        if (!g->wasten) return;
        n = 1; buf[0] = g->waste[g->wasten - 1];
        if (!sl_tab_ok(g, c, buf[0])) return;
        g->wasten--;
    } else if (g->sel_src >= 8) {
        int f = g->sel_src - 8;
        if (!g->foundn[f]) return;
        n = 1; buf[0] = g->found[f][g->foundn[f] - 1];
        if (!sl_tab_ok(g, c, buf[0])) return;
        g->foundn[f]--;
    } else return;
    for (int i = 0; i < n; i++) {
        g->tab[c][g->tabn[c]] = buf[i];
        g->tabup[c][g->tabn[c]] = 1;
        g->tabn[c]++;
    }
    g->moves++;
    g->sel_src = -1;
    sl_check_win(g);
}

static void sl_move_found(struct sol *g, int f)
{
    u8 card;
    if (g->sel_src >= 0 && g->sel_src <= 6) {
        int s = g->sel_src;
        if (g->sel_idx != g->tabn[s] - 1) return;      /* single card only */
        card = g->tab[s][g->sel_idx];
        if (!sl_found_ok(g, f, card)) return;
        g->tabn[s]--;
        sl_flip_top(g, s);
    } else if (g->sel_src == 7) {
        if (!g->wasten) return;
        card = g->waste[g->wasten - 1];
        if (!sl_found_ok(g, f, card)) return;
        g->wasten--;
    } else return;
    g->found[f][g->foundn[f]++] = card;
    g->moves++;
    g->sel_src = -1;
    sl_check_win(g);
}

static void sl_draw_stock(struct sol *g)
{
    if (g->stockn) {
        g->waste[g->wasten++] = g->stock[--g->stockn];
        g->moves++;
    } else if (g->wasten) {
        while (g->wasten) g->stock[g->stockn++] = g->waste[--g->wasten];
        g->moves++;
    }
    g->sel_src = -1;
}

/* ------------------------------------------------------------ drawing ---- */
static void empty_pile(struct surface *s, int x, int y, u32 col, const char *hint)
{
    s_frame_rect(s, x, y, CW, CH, col);
    if (hint) s_text(s, x + CW / 2 - 4, y + CH / 2 - 8, hint, col);
}

static int tableau_step(struct surface *s,struct sol *g,int col,int index){
    int step=g->tabup[col][index]?UP_OFF:DOWN_OFF,total=0;
    for(int i=0;i<g->tabn[col]-1;i++)total+=g->tabup[col][i]?UP_OFF:DOWN_OFF;
    int room=s->h-78-TAB_Y-CH;if(room<1)room=1;
    if(total>room){step=step*room/total;if(step<1)step=1;}return step;
}

static void sl_paint(struct window *w)
{
    sol_layout(w, &sol_L);
    struct sol *g = w->data;
    struct surface *s = &w->surf;
    const struct theme *t = theme_current();
    s_fill(s, 0, 0, s->w, s->h, color_blend(t->win_bg,t->main,12));
    for (int yy = 8; yy < s->h; yy += 16) s_fill(s, 0, yy, s->w, 1, color_blend(t->win_bg,t->main,18));

    /* stock + waste */
    if (g->stockn) card_draw(s, COLX(0), TOP_Y, g->stock[g->stockn - 1], 1, t->main);
    else empty_pile(s, COLX(0), TOP_Y, ((t->main >> 1) & 0x7F7F7F), NULL);
    if (g->wasten) {
        u8 c = g->waste[g->wasten - 1];
        card_draw(s, COLX(1), TOP_Y, c, 0, t->main);
        if (g->sel_src == 7) s_frame_rect(s, COLX(1) - 2, TOP_Y - 2, CW + 4, CH + 4, t->main);
    } else empty_pile(s, COLX(1), TOP_Y, ((t->main >> 1) & 0x7F7F7F), NULL);

    /* foundations */
    static const char *fh[4] = { "H", "D", "S", "C" };
    for (int f = 0; f < 4; f++) {
        int x = COLX(3 + f);
        if (g->foundn[f]) {
            card_draw(s, x, TOP_Y, g->found[f][g->foundn[f] - 1], 0, t->main);
            if (g->sel_src == 8 + f) s_frame_rect(s, x - 2, TOP_Y - 2, CW + 4, CH + 4, t->main);
        } else empty_pile(s, x, TOP_Y, ((t->main >> 1) & 0x7F7F7F), fh[f]);
    }

    /* tableau */
    for (int c = 0; c < 7; c++) {
        int x = COLX(c);
        if (!g->tabn[c]) {
            empty_pile(s, x, TAB_Y, ((t->main >> 1) & 0x7F7F7F), NULL);
            if (g->hover == c && g->sel_src >= 0) s_frame_rect(s, x - 2, TAB_Y - 2, CW + 4, CH + 4, t->main);
            continue;
        }
        int y = TAB_Y;
        for (int i = 0; i < g->tabn[c]; i++) {
            int sel = (g->sel_src == c && i >= g->sel_idx);
            card_draw(s, x, y, g->tab[c][i], !g->tabup[c][i], t->main);
            if (sel) s_frame_rect(s, x - 2, y - 2, CW + 4, CH + 4, t->main);
            y += tableau_step(s,g,c,i);
        }
        if (g->hover == c && g->sel_src >= 0 && g->sel_src != c)
            s_frame_rect(s, x - 2, y - tableau_step(s,g,c,g->tabn[c]-1) - 2, CW + 4, CH + 4, t->main);
    }

    /* status + button */
    char line[96], n[12];
    strcpy(line, "Moves: ");
    fmt_u32(n, (u32)g->moves); strcat(line, n);
    strcat(line, "   Time: ");
    fmt_u32(n, (u32)(uptime_ms() / 1000) - g->t0); strcat(line, n); strcat(line, "s");
    strcat(line, "   Stock: ");
    fmt_u32(n, (u32)g->stockn); strcat(line, n);
    s_fill(s,0,s->h-66,s->w,66,t->win_bg);
    s_clip_text(s,12,s->h-60,line,t->text,s->w-24);

    int bx = 12, by = s->h - 38;
    u32 bg = g->hover_btn ? t->main : color_blend(t->win_bg,t->main,15);
    u32 fg = g->hover_btn ? t->title_text : t->main;
    s_fill(s, bx, by, 96, 28, bg);
    s_frame_rect(s, bx, by, 96, 28, fg);
    s_text(s, bx + 14, by + 6, "New Game", fg);
    s_clip_text(s,bx+110,by+6,"draw: stock - move: card, then pile",t->text,s->w-bx-122);

    if (g->won) {
        const char *m = "You win! All foundations complete.";
        int tw = s_text_width(m);
        s_fill(s, (s->w - tw) / 2 - 12, 210, tw + 24, 30, t->win_bg);
        s_frame_rect(s, (s->w - tw) / 2 - 12, 210, tw + 24, 30, t->main);
        s_text(s, (s->w - tw) / 2, 217, m, t->main);
    }
}

/* -------------------------------------------------------------- input ---- */
static int col_at(struct window *w, int x)
{
    sol_layout(w, &sol_L);
    if (x < sol_L.x0) return -1;
    int c = (x - sol_L.x0) / sol_L.colw;
    if (c > 6 || (x - sol_L.x0) % sol_L.colw >= CW + 8) return -1;
    return c;
}

static void sl_mouse(struct window *w, struct mouse_event *e, int x, int y)
{
    sol_layout(w, &sol_L);
    struct sol *g = w->data;
    struct surface *s = &w->surf;
    int old_sel = g->sel_src, old_hov = g->hover, old_btn = g->hover_btn;
    g->hover = -1;
    g->hover_btn = (x >= 12 && x < 108 && y >= s->h - 38 && y < s->h - 10);

    int c = col_at(w, x);
    if (c >= 0&&y<s->h-66) g->hover = c;

    if (e->type != MEV_BUTTON || !e->down || e->button != MBTN_LEFT) {
        if (old_sel != g->sel_src || old_hov != g->hover || old_btn != g->hover_btn) wm_redraw(w);
        return;
    }
    if (g->hover_btn) { sl_new(g); wm_redraw(w); return; }
    if (g->won||y>=s->h-66) return;

    /* top row */
    if (y >= TOP_Y && y < TOP_Y + CH && c >= 0) {
        if (c == 0) sl_draw_stock(g);
        else if (c == 1) {
            if (g->wasten) g->sel_src = (g->sel_src == 7) ? -1 : 7;
        } else if (c >= 3) {
            int f = c - 3;
            if (g->sel_src >= 0) sl_move_found(g, f);
            else if (g->foundn[f]) { g->sel_src = 8 + f; }
        }
        wm_redraw(w);
        return;
    }
    /* tableau */
    if (y >= TAB_Y && c >= 0) {
        if (g->sel_src >= 0 && g->sel_src != c) { sl_move_tab(g, c); wm_redraw(w); return; }
        /* find clicked card: walk offsets */
        int yy = TAB_Y, idx = -1;
        for (int i = 0; i < g->tabn[c]; i++) {
            int h = (i == g->tabn[c] - 1) ? CH : tableau_step(s,g,c,i);
            if (y >= yy && y < yy + h) idx = i;
            yy += h;
        }
        if (idx < 0) {                      /* empty column: drop here */
            if (!g->tabn[c] && g->sel_src >= 0) sl_move_tab(g, c);
            wm_redraw(w);
            return;
        }
        if (!g->tabup[c][idx]) { wm_redraw(w); return; }   /* face down */
        if (g->sel_src == c && g->sel_idx == idx) g->sel_src = -1;
        else { g->sel_src = c; g->sel_idx = idx; }
        wm_redraw(w);
        return;
    }
    if (old_sel != g->sel_src || old_hov != g->hover || old_btn != g->hover_btn) wm_redraw(w);
}

static void sl_key(struct window *w, struct key_event *e)
{
    struct sol *g = w->data;
    if (!e->pressed) return;
    if (e->keycode == 'n' || e->keycode == 'N') { sl_new(g); wm_redraw(w); }
}

static void sl_open(struct window *w, void *arg)
{
    (void)arg;
    struct sol *g = palloc(sizeof(*g));
    if (!g) return;              /* OOM: wm_open_app reports it centrally */
    memset(g, 0, sizeof(*g));
    struct rtc_time rt;
    rtc_read(&rt);
    g->seed = (u32)tick_count * 2654435761u ^ (u32)rt.sec * 40503u ^ 0x85EBCA6Bu;
    if (!g->seed) g->seed = 7;
    g->sel_src = -1;
    g->hover = -1;
    g->owner = w;
    w->data = g;
    wm_track_mem(w, (int)sizeof(*g));
    sl_new(g);
}

static void sl_close(struct window *w)
{
    if (w->data) pfree(w->data, sizeof(struct sol));
}

struct app app_solitaire = {
    .desktop_label = "Solitaire",
    .uses_data = 1, .id = "solitaire", .title = "Solitaire", .icon = ICON_SOL, .single = 1,
    .def_w = 700, .def_h = 540,.min_w=600,.min_h=500,
    .open = sl_open, .paint = sl_paint, .key = sl_key,
    .mouse = sl_mouse, .close = sl_close,
};

SCOS_APP(app_solitaire, 009);
