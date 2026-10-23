/*
 * SCos native - Browser app stub.
 *
 * The web simulation embedded an <iframe>. A real browser inside the kernel
 * would need a NIC driver, a full TCP/IP stack, TLS, and an HTML/CSS layout
 * engine - well over 10k lines - so, per project scope, it is skipped.
 * This window explains that honestly instead of pretending.
 */
#include "scos.h"

/* r36: the explanation page is ~300 px tall - in a short window the tail
 * used to be clipped and unreachable (no wheel handler at all). */
struct br_data { int scroll; };

#define BR_CONTENT_H 310

static void br_open(struct window *w, void *arg)
{
    (void)arg;
    struct br_data *b = palloc(sizeof(*b));
    if (b) b->scroll = 0;
    w->data = b;
}

static void br_close(struct window *w)
{
    if (w->data) { pfree(w->data, sizeof(struct br_data)); w->data = NULL; }
}

static void br_paint(struct window *w)
{
    struct surface *s = &w->surf;
    const struct theme *t = theme_current();
    s_fill(s, 0, 0, s->w, s->h, t->win_bg);
    s_frame_rect(s, 8, 8, s->w - 16, 26, t->main);
    s_clip_text(s,14,14,"https://(no network stack)",t->text,s->w-28);

    static const char *lines[] = {
        "Web Browser is not available on SCos.",
        "",
        "A browser needs a network path, and this kernel has none:",
        "A real kernel-side browser would require:",
        "  - a network card driver (e1000/rtl8139/...)",
        "  - a full TCP/IP stack with DNS",
        "  - TLS 1.3 + certificate validation",
        "  - an HTML/CSS/JS rendering engine",
        "Together far beyond this project's scope (10k+ lines),",
        "so this app was intentionally skipped.",
        "",
        "All other desktop apps run directly on this kernel.",
    };
    struct br_data *b = w->data;
    int maxs = BR_CONTENT_H - s->h;
    if (maxs < 0) maxs = 0;
    int scroll = b ? b->scroll : 0;
    if (scroll > maxs) scroll = maxs;
    int y = 50 - scroll;
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        if (y >= 42 && y+16 <= s->h)
            s_clip_text(s,16,y,lines[i],i==0?t->main:t->text,s->w-32);
        y += 20;
    }
}

static void br_scroll_by(struct window *w, int lines_delta)
{
    struct br_data *b = w->data;
    if (!b) return;
    int maxs = BR_CONTENT_H - w->surf.h;
    if (maxs < 0) maxs = 0;
    int ns = b->scroll + lines_delta * 20;
    if (ns < 0) ns = 0;
    if (ns > maxs) ns = maxs;
    if (ns != b->scroll) { b->scroll = ns; wm_redraw(w); }
}

static void br_key(struct window *w, struct key_event *e)
{
    if (!e->pressed) return;
    if (e->keycode == KEY_PGUP) br_scroll_by(w, -3);
    else if (e->keycode == KEY_PGDN) br_scroll_by(w, 3);
}
static void br_mouse(struct window *w, struct mouse_event *e, int x, int y)
{
    (void)x; (void)y;
    if (e->type == MEV_WHEEL) br_scroll_by(w, -(int)e->wheel);
}

struct app app_browser = {
    .desktop_label = "Browser",
    .id = "browser", .title = "Web Browser", .icon = ICON_BROWSER, .single = 0,
    .def_w = 640, .def_h = 420,
    .open = br_open, .paint = br_paint, .key = br_key, .mouse = br_mouse,
    .close = br_close,
};

SCOS_APP(app_browser, 003);
