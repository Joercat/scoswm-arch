/*
 * SCos native - About app.
 *
 * Shows the logo plus a live report of the host this port is actually
 * running on: CPU brand, host RAM, snapshot storage, window size, uptime.
 */
#include "scos.h"

static void ab_paint(struct window *w)
{
    struct surface *s = &w->surf;
    const struct theme *t = theme_current();
    s_fill(s, 0, 0, s->w, s->h, t->win_bg);

    int cx = s->w / 2;
    int phase = (int)(tick_count / 8) % 8;
    s_scos_logo(s, cx - 37, 14, t->main, 2, phase);

    int y = 66;
    char line[120];
    strcpy(line, "SCos - Linux port - build " SCOS_BUILD_TAG);
    int tw=s_text_width(line);if(tw>s->w-56)tw=s->w-56;s_clip_text(s,cx-tw/2,y,line,t->main,s->w-56); y += 24;
    /* The commit the panel is describing, so a screenshot of this window is evidence. */
    scos_build_stamp(line, sizeof(line));
    tw=s_text_width(line);if(tw>s->w-56)tw=s->w-56;s_clip_text(s,cx-tw/2,y,line,t->text,s->w-56); y += 24;

    s_line(s, 24, y, s->w - 24, y, t->main); y += 14;

    char cpu[49];
    cpu_brand(cpu, sizeof(cpu));
    strcpy(line, "CPU:      "); strcat(line, cpu);
    s_clip_text(s, 28, y, line, t->text, s->w - 56); y += 20;

    u64 tot = 0, fre = 0;
    mm_stats(&tot, &fre);
    strcpy(line, "Memory:   ");
    char n[16];
    fmt_u64(n, tot / 1024); strcat(line, n); strcat(line, " MB managed, ");
    fmt_u64(n, fre / 1024); strcat(line, n); strcat(line, " MB free");
    s_clip_text(s,28,y,line,t->text,s->w-56); y += 20;

    const char *model = ata_model();
    strcpy(line, "Disk:     ");
    strcat(line, model && model[0] ? model : "snapshot file (no ATA disk on Linux)");
    s_clip_text(s, 28, y, line, t->text, s->w - 56); y += 20;

    strcpy(line, "Video:    ");
    char v[12];
    fmt_u32(v, (u32)screen_w); strcat(line, v); strcat(line, "x");
    fmt_u32(v, (u32)screen_h); strcat(line, v); strcat(line, "x");
    fmt_u32(v, fb_bpp()); strcat(line, v);
    strcat(line, " host window (SDL2)");
    s_clip_text(s,28,y,line,t->text,s->w-56); y += 20;

    u32 up = uptime_ms() / 1000;
    strcpy(line, "Uptime:   ");
    fmt_u32(v, up / 3600); strcat(line, v); strcat(line, "h ");
    fmt_u32(v, (up / 60) % 60); strcat(line, v); strcat(line, "m ");
    fmt_u32(v, up % 60); strcat(line, v); strcat(line, "s");
    s_clip_text(s,28,y,line,t->text,s->w-56); y += 20;

    strcpy(line, "Theme:    ");
    strcat(line, t->name);
    s_clip_text(s,28,y,line,t->text,s->w-56); y += 26;

    static const char *feats[] = {
        "SCos desktop ported to Linux (SDL2 host window)",
        "Host keyboard + mouse input, 100 Hz tick, host clock",
        "Software framebuffer compositor with window manager and themes",
        "RAM filesystem; snapshot-file saves (XDG data dir)",
        "Terminal, Files, Notepad, Calendar, Settings,",
        "Blackjack, System Monitor, Web Browser (stub)",
    };
    for (unsigned i = 0; i < sizeof(feats) / sizeof(feats[0]); i++) {
        s_text(s, 28, y, "-", t->main);
        s_clip_text(s, 40, y, feats[i], t->text, s->w - 68);
        y += 19;
    }
}

static void ab_key(struct window *w, struct key_event *e) { (void)w; (void)e; }
static void ab_mouse(struct window *w, struct mouse_event *e, int x, int y) { (void)w; (void)e; (void)x; (void)y; }

struct app app_about = {
    .desktop_label = "About",
    .id = "about", .title = "About SCos", .icon = ICON_INFO, .single = 0,
    .def_w = 520, .def_h = 460,.min_h=440,
    .paint = ab_paint, .key = ab_key, .mouse = ab_mouse,
};

SCOS_APP(app_about, 006);
