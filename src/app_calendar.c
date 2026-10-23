/* SCos native - Calendar app */
#include "scos.h"

struct cal {
    int month, year;        /* 0-based month */
    int hover_day, hover_btn;   /* -1 none; btn 0 prev 1 next 2 today */
};

static const char *cal_months[12] = { "January","February","March","April",
    "May","June","July","August","September","October","November","December" };
static const char *cal_days[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };

static void cal_open(struct window *w, void *arg)
{
    (void)arg;
    struct rtc_time rt;
    rtc_read(&rt);
    struct cal *c = palloc(sizeof(*c));
    if (!c) return;              /* OOM: wm_open_app reports it centrally */
    memset(c, 0, sizeof(*c));
    c->month = rt.mon - 1;
    c->year = rt.year;
    c->hover_day = -1;
    c->hover_btn = -1;
    w->data = c;
    wm_track_mem(w, (int)sizeof(*c));
}

static void cal_close(struct window *w)
{
    if (w->data) pfree(w->data, sizeof(struct cal));
    w->data = NULL;
}

static int days_in_month(int month, int year)
{
    static const int d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month == 1) {
        int leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return 28 + leap;
    }
    return d[month];
}

/* 0=Sunday */
static int first_weekday(int month, int year)
{
    int y = year - (month <= 1 ? 1 : 0);
    int era = y / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned mp = (unsigned)(month + 10) % 12;
    unsigned doy = (153 * mp + 2) / 5 + 1 - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;
    return (int)((days % 7 + 7) % 7);
}

#define CAL_TOOL_H 40
#define CELL_W 78
#define CELL_H 52

static void cal_paint(struct window *w)
{
    struct cal *c = w->data;
    struct surface *s = &w->surf;
    const struct theme *t = theme_current();
    s_fill(s, 0, 0, s->w, s->h, t->win_bg);

    /* toolbar */
    s_fill(s, 0, 0, s->w, CAL_TOOL_H, t->win_bg);
    for (int b = 0; b < 3; b++) {
        int bx = 8 + b * 34;
        int bw = b == 2 ? 60 : 30;
        u32 bg = c->hover_btn == b ? t->main : color_blend(t->win_bg,t->main,15);
        u32 fg = c->hover_btn == b ? t->title_text : t->main;
        s_fill(s, bx, 7, bw, 26, bg);
        s_frame_rect(s, bx, 7, bw, 26, t->main);
        if (b == 0) { s_line(s, bx + 17, 14, bx + 11, 20, fg); s_line(s, bx + 11, 20, bx + 17, 26, fg); }
        else if (b == 1) { s_line(s, bx + 13, 14, bx + 19, 20, fg); s_line(s, bx + 19, 20, bx + 13, 26, fg); }
        else s_text(s, bx + 10, 13, "Today", fg);
    }
    char title[48];
    strcpy(title, cal_months[c->month]);
    strcat(title, " ");
    char yb[8];
    fmt_u32(yb, c->year);
    strcat(title, yb);
    int tx=144+(s->w-152-s_text_width(title))/2;if(tx<144)tx=144;
    s_clip_text(s,tx,13,title,t->text,s->w-tx-8);

    /* grid */
    int gx = 8, gy = CAL_TOOL_H + 6;
    int cw = (s->w - 16) / 7;
    int ch=(s->h-CAL_TOOL_H-36)/6;if(ch<20)ch=20;
    for (int d = 0; d < 7; d++) {
        s_fill(s, gx + d * cw, gy, cw - 2, 20, ((t->main >> 1) & 0x7F7F7F));
        s_text(s, gx + d * cw + (cw - 2 - s_text_width(cal_days[d])) / 2, gy + 2, cal_days[d], t->title_text);
    }
    gy += 22;
    int fd = first_weekday(c->month, c->year);
    int dim = days_in_month(c->month, c->year);
    struct rtc_time now;
    rtc_read(&now);
    for (int day = 1; day <= dim; day++) {
        int idx = fd + day - 1;
        int col = idx % 7, row = idx / 7;
        int x = gx + col * cw, y = gy + row * ch;
        int is_today = day == now.day && c->month == now.mon - 1 && c->year == now.year;
        u32 bg = is_today ? t->main : (c->hover_day == day ? ((t->main >> 2) & 0x3F3F3F) : t->win_bg);
        s_fill(s, x, y, cw - 2, ch - 2, bg);
        s_frame_rect(s, x, y, cw - 2, ch - 2, ((t->main >> 1) & 0x7F7F7F));
        char db[4];
        fmt_u32(db, day);
        s_text(s, x + 6, y + 6, db, is_today ? t->title_text : t->text);
    }
}

static void day_dialog_cb(int ok, const char *text, void *ud) { (void)ok; (void)text; (void)ud; }

static void cal_mouse(struct window *w, struct mouse_event *e, int x, int y)
{
    struct cal *c = w->data;
    struct surface *s = &w->surf;
    int oldb = c->hover_btn, oldd = c->hover_day;
    c->hover_btn = -1;
    c->hover_day = -1;
    for (int b = 0; b < 3; b++) {
        int bw = b == 2 ? 60 : 30;
        if (x >= 8 + b * 34 && x < 8 + b * 34 + bw && y >= 7 && y < 33) c->hover_btn = b;
    }
    int gx = 8, gy = CAL_TOOL_H + 6 + 22;
    int cw = (s->w - 16) / 7;
    int fd = first_weekday(c->month, c->year);
    int dim = days_in_month(c->month, c->year);
    if (y >= gy && x >= gx && x < gx+7*cw) {
        int ch=(w->surf.h-CAL_TOOL_H-36)/6;if(ch<20)ch=20;
        int col = (x - gx) / cw, row = (y - gy) / ch;
        if (col >= 0 && col < 7 && row >= 0) {
            int day = row * 7 + col - fd + 1;
            if (day >= 1 && day <= dim) c->hover_day = day;
        }
    }
    if (oldb != c->hover_btn || oldd != c->hover_day) wm_redraw(w);
    if (e->type != MEV_BUTTON || !e->down || e->button != MBTN_LEFT) return;

    if (c->hover_btn == 0) {
        if (--c->month < 0) { c->month = 11; c->year--; }
        wm_redraw(w);
    } else if (c->hover_btn == 1) {
        if (++c->month > 11) { c->month = 0; c->year++; }
        wm_redraw(w);
    } else if (c->hover_btn == 2) {
        struct rtc_time rt;
        rtc_read(&rt);
        c->month = rt.mon - 1;
        c->year = rt.year;
        wm_redraw(w);
    } else if (c->hover_day >= 0) {
        /* weekday of selected date */
        int wd = first_weekday(c->month, c->year);
        wd = (wd + c->hover_day - 1) % 7;
        char msg[96];
        strcpy(msg, "Selected date: ");
        strcat(msg, cal_days[wd]);
        strcat(msg, " ");
        strcat(msg, cal_months[c->month]);
        char b[8];
        fmt_u32(b, c->hover_day);
        strcat(msg, b);
        strcat(msg, " ");
        fmt_u32(b, c->year);
        strcat(msg, b);
        wm_dialog("Calendar", msg, NULL, day_dialog_cb, NULL);
    }
}

static void cal_key(struct window *w, struct key_event *e) { (void)w; (void)e; }

struct app app_calendar = {
    .desktop_label = "Calendar",
    .uses_data = 1, .id = "calendar", .title = "Calendar", .icon = ICON_CALENDAR, .single = 0,
    .def_w = 600, .def_h = 460, .min_h = 240,
    .open = cal_open, .paint = cal_paint, .key = cal_key,
    .mouse = cal_mouse, .close = cal_close,
};

SCOS_APP(app_calendar, 004);
