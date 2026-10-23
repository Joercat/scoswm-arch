/* SCos native - Settings app (themes, system info, reset) */
#include "scos.h"

struct settings_ui { int hover_tile, hover_reset, hover_pref, scroll; };

#define TILE_W 120
#define TILE_H 80

/* prefs section geometry: top y, reset-button y */
static void st_layout(struct surface *s, int *sy, int *ry)
{
    (void)s;
    int iy = 60 + ((theme_count() + 3) / 4) * (TILE_H + 12) + 16;
    *sy = iy + 70;
    *ry = *sy + 186;
}
/* hover_pref ids: 0/1 sens -/+, 2/3 dbl -/+, 4 restore icons */
static void st_pref_rect(int id, int sy, int *x, int *y, int *w, int *h)
{
    int row = (id < 2) ? 0 : (id < 4 ? 1 : 2);
    *y = sy + 22 + row * 30;
    if (id == 0 || id == 2) { *x = 210; *w = 24; *h = 20; }
    else if (id == 1 || id == 3) { *x = 326; *w = 24; *h = 20; }
    /* r39: 190 px - "Restore desktop icons" is 21 chars * 8 px + 20 px
     * padding; the old 170 let the label overflow the button edge */
    else { *x = 16; *w = 190; *h = 24; *y = sy + 84; }
}


static void st_open(struct window *w, void *arg)
{
    (void)arg;
    struct settings_ui *ui = palloc(sizeof(*ui));
    if (!ui) return;             /* OOM: wm_open_app reports it centrally */
    ui->hover_tile = -1;
    ui->hover_reset = -1;
    ui->hover_pref = -1;
    ui->scroll = 0;
    w->data = ui;
    wm_track_mem(w, (int)sizeof(*ui));
}

static void st_close(struct window *w)
{
    if (w->data) pfree(w->data, sizeof(struct settings_ui));
    w->data = NULL;
}

static void tile_rect(int i, int sw, int *x, int *y)
{
    *x = 16 + (i % 4) * (TILE_W + 12);
    if (*x + TILE_W > sw - 8) { *x = 16 + (i % 2) * (TILE_W + 12); *y = 60 + (i / 2) * (TILE_H + 12); }
    else *y = 60 + (i / 4) * (TILE_H + 12);
}

static void st_paint(struct window *w)
{
    struct settings_ui *ui = w->data;
    struct surface *s = &w->surf;
    const struct theme *t = theme_current();
    s_fill(s, 0, 0, s->w, s->h, t->win_bg);

    s_text(s, 16, 16-ui->scroll, "Appearance", t->main);
    s_text(s, 16, 38-ui->scroll, "Theme:", t->text);

    for (int i = 0; i < theme_count(); i++) {
        const struct theme *th = theme_get(i);
        int x, y;
        tile_rect(i, s->w, &x, &y);
        y -= ui->scroll;
        s_fill(s, x, y, TILE_W, TILE_H, th->bg_top);
        s_fill(s, x, y, TILE_W, 14, th->main);
        s_clip_text(s, x + 8, y + 24, th->name, th->main,TILE_W-16);
        s_frame_rect(s, x, y, TILE_W, TILE_H, th->main);
        if (i == theme_index_of_id(t->id))
            s_frame_rect(s, x - 2, y - 2, TILE_W + 4, TILE_H + 4, t->text);
        if (ui->hover_tile == i)
            s_frame_rect(s, x - 4, y - 4, TILE_W + 8, TILE_H + 8, t->main);
    }

    int sy, ry_ignored;
    st_layout(s, &sy, &ry_ignored);
    sy -= ui->scroll;
    s_text(s, 16, sy, "Mouse & Desktop", t->main);
    const struct prefs *pf = prefs_get();
    char val[24];
    s_text(s, 16, sy + 26, "Mouse speed:", t->text);
    fmt_u32(val, (u32)pf->mouse_sens);
    s_text(s, 250, sy + 26, val, t->main);
    s_text(s, 16, sy + 56, "Double-click:", t->text);
    fmt_u32(val, (u32)pf->dbl_ms);
    strcat(val, " ms");
    s_text(s, 250, sy + 56, val, t->main);
    for (int id = 0; id < 4; id++) {
        int x, y, ww, hh;
        st_pref_rect(id, sy, &x, &y, &ww, &hh);
        u32 bg = ui->hover_pref == id ? t->main : color_blend(t->win_bg,t->main,15);
        s_fill(s, x, y, ww, hh, bg);
        s_frame_rect(s, x, y, ww, hh, t->main);
        s_text(s, x + 8, y + 4, (id & 1) ? "+" : "-",
               ui->hover_pref == id ? t->title_text : t->main);
    }
    {
        int x, y, ww, hh;
        st_pref_rect(4, sy, &x, &y, &ww, &hh);
        u32 bg = ui->hover_pref == 4 ? t->main : color_blend(t->win_bg,t->main,15);
        s_fill(s, x, y, ww, hh, bg);
        s_frame_rect(s, x, y, ww, hh, t->main);
        s_text(s, x + 10, y + 5, "Restore desktop icons",
               ui->hover_pref == 4 ? t->title_text : t->main);
    }

    int iy = sy + 116;
    s_text(s, 16, iy, "System Information", t->main);
    char line[96];
    strcpy(line, "OS Version: unnumbered x64 development");
    s_text(s, 16, iy + 22, line, t->text);
    char sz[32];
    u32 used = vfs_usage_bytes();
    if (used < 1024) { fmt_u32(sz, used); strcat(sz, " bytes"); }
    else if (used < 1024 * 1024) { fmt_u32(sz, used / 1024); strcat(sz, "."); fmt_u32(sz + strlen(sz), (used % 1024) * 100 / 1024); strcat(sz, " KB"); }
    else { fmt_u32(sz, used / (1024 * 1024)); strcat(sz, " MB"); }
    strcpy(line, "Storage Used: ");
    strcat(line, sz);
    s_text(s, 16, iy + 42, line, t->text);

    int ry = iy + 70;
    u32 bg = ui->hover_reset == 1 ? t->main : color_blend(t->win_bg,t->main,15);
    s_fill(s, 16, ry, 130, 26, bg);
    s_frame_rect(s, 16, ry, 130, 26, t->main);
    s_text(s, 26, ry + 5, "Factory Reset", ui->hover_reset == 1 ? t->title_text : t->main);

}

static void reset_confirm_cb(int ok, const char *text, void *ud)
{
    (void)text; (void)ud;
    if (!ok) return;
    /* wipe everything back to shipping defaults and persist the wipe */
    if (!vfs_factory_reset()) {
        wm_error_popup("Reset failed: out of memory.\nFiles unchanged; not restarting.");
        return;
    }
    theme_set_index(0);
    wm_theme_changed();
    prefs_set_mouse(PREFS_MOUSE_DEFAULT);
    prefs_set_dbl(PREFS_DBL_DEFAULT);
    settings_save();
    int persistent=fs_image_available();
    if (persistent && !fs_image_save()) {
        wm_error_popup("RAM defaults restored.\nDisk reset FAILED; see klog.\nNot restarting. Disk save\nmay be incomplete.");
        return;
    }
    /* tell the user what happened, then restart the machine */
    fb_clear(0x000000);
    const struct theme *t = theme_current();
    const char *m1 = "Factory reset complete";
    const char *m2 = persistent ? "Snapshot file cleared; defaults verified on disk."
                                : "RAM reset only. No verified persistence disk; disks untouched.";
    const char *m3 = "scoswm is exiting...";
    s_text_scaled(&screen, (screen_w - s_text_width(m1) * 3) / 2, screen_h / 2 - 70, m1, t->main, 3);
    s_text(&screen, (screen_w - s_text_width(m2)) / 2, screen_h / 2, m2, 0xCCCCCC);
    s_text(&screen, (screen_w - s_text_width(m3)) / 2, screen_h / 2 + 30, m3, 0xAAAAAA);
    fb_flip();
    sleep_ms(1500);
    platform_request_quit();   /* Linux: a factory reset exits scoswm instead of rebooting */
    return;
}

static void st_mouse(struct window *w, struct mouse_event *e, int x, int y)
{
    struct settings_ui *ui = w->data;
    struct surface *s = &w->surf;
    if(e->type==MEV_WHEEL){int sy,ry;st_layout(s,&sy,&ry);int max=ry+44-s->h;if(max<0)max=0;ui->scroll-=e->wheel*32;if(ui->scroll<0)ui->scroll=0;if(ui->scroll>max)ui->scroll=max;wm_redraw(w);}
    y += ui->scroll;
    int oldt = ui->hover_tile, oldr = ui->hover_reset, oldp = ui->hover_pref;
    ui->hover_tile = -1;
    ui->hover_reset = -1;
    for (int i = 0; i < theme_count(); i++) {
        int tx, ty;
        tile_rect(i, s->w, &tx, &ty);
        if (x >= tx && y >= ty && x < tx + TILE_W && y < ty + TILE_H) ui->hover_tile = i;
    }
    int sy, ry;
    st_layout(s, &sy, &ry);
    ui->hover_pref = -1;
    for (int id = 0; id < 5; id++) {
        int hx, hy, hw, hh;
        st_pref_rect(id, sy, &hx, &hy, &hw, &hh);
        if (x >= hx && y >= hy && x < hx + hw && y < hy + hh) ui->hover_pref = id;
    }
    if (x >= 16 && y >= ry && x < 146 && y < ry + 26) ui->hover_reset = 1;
    if (oldt != ui->hover_tile || oldr != ui->hover_reset ||
        oldp != ui->hover_pref) wm_redraw(w);

    if (e->type != MEV_BUTTON || !e->down || e->button != MBTN_LEFT) return;
    if (ui->hover_tile >= 0) {
        theme_set_index(ui->hover_tile);
        wm_theme_changed();
        settings_save();
        {
            char lg[80];
            strcpy(lg, "theme switched to ");
            strncat(lg, theme_current()->name, 40);
            app_log(w, lg);
        }
        wm_redraw(w);
    } else if (ui->hover_pref == 0) {
        prefs_set_mouse(prefs_get()->mouse_sens - 1);
        wm_redraw(w);
    } else if (ui->hover_pref == 1) {
        prefs_set_mouse(prefs_get()->mouse_sens + 1);
        wm_redraw(w);
    } else if (ui->hover_pref == 2) {
        prefs_set_dbl(prefs_get()->dbl_ms - 100);
        wm_redraw(w);
    } else if (ui->hover_pref == 3) {
        prefs_set_dbl(prefs_get()->dbl_ms + 100);
        wm_redraw(w);
    } else if (ui->hover_pref == 4) {
        wm_desktop_restore();
        wm_redraw(w);
    } else if (ui->hover_reset == 1) {
        char message[256];
        if (fs_image_available()) {
            strcpy(message,"Reset the RAM filesystem and the snapshot file:\n");
            strncat(message,fs_image_target(),40);
            strcat(message,"\nNot secure erasure.\nExit scoswm after the reset?");
        } else strcpy(message,"Reset RAM to defaults and exit scoswm?\nNo writable snapshot file.\nALL disks will remain untouched.");
        wm_dialog("Factory Reset",message,NULL,reset_confirm_cb,NULL);
    }
}

static void st_key(struct window *w, struct key_event *e) { (void)w; (void)e; }

struct app app_settings = {
    .desktop_label = "Settings",
    .uses_data = 1, .id = "settings", .title = "Settings", .icon = ICON_SETTINGS, .single = 0,
    .def_w = 620, .def_h = 700, .min_w = 560, .min_h = 300,
    .open = st_open, .paint = st_paint, .key = st_key,
    .mouse = st_mouse, .close = st_close,
};

SCOS_APP(app_settings, 005);
