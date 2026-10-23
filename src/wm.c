/*
 * SCos native - window manager / compositor / desktop shell.
 *
 * Draws the wallpaper, desktop icons, decorated windows (drag, resize,
 * minimize, maximize, close, focus), context menus, modal dialogs, the
 * taskbar with running apps + clock, and the mouse cursor. All interaction
 * is mouse- and keyboard-driven through the PS/2 drivers.
 */
#include "scos.h"

#define MAX_WINDOWS 48
#define TASKBAR_H 40
static void desktop_load(void);
static void desktop_save(void);
static __attribute__((noinline)) void menu_cancel(void);
static void icon_rect(int i, int *x, int *y, int *w, int *h);
static void desk_rename_ask(int item);
static void desk_openwith(int item);
#define BTN_SZ 18

/* Stable slots: app/tab/dialog back-pointers must survive closing an earlier window. */
static struct window window_slots[MAX_WINDOWS];
static struct window *wins[MAX_WINDOWS];
static int win_count;
static int next_z = 10, next_id = 1;
static struct window *focused_w;
static struct window *modal_w;
static int dirty = 1;

/* r39: per-app CPU accounting. Every app callback (paint/tick/mouse/
 * key) is timed with the TSC; once per second the accumulated cycles
 * become a percent of the machine's measured TSC rate (the same
 * calibration SysMon already shows as MHz). Real numbers only: on a
 * box without TSC nothing is instrumented and the column reads 0. */
static u64 app_cyc[MAX_WINDOWS];
static u32 app_pct[MAX_WINDOWS];
static int cyc_ok;                     /* TSC usable (set in wm_run) */
#define APP_T0(w) u64 cyc_t0 = cyc_ok ? cpu_busy_cycles() : 0; int cyc_id = (w)->id; uintptr_t prior_owner=heap_set_owner((uintptr_t)(w))
#define APP_T1(w) do { heap_set_owner(prior_owner); if (cyc_ok) { \
    u64 elapsed = cpu_busy_cycles() - cyc_t0; \
    for (int ci = 0; ci < win_count; ci++) \
        if (wins[ci]->id == cyc_id) { app_cyc[ci] += elapsed; break; } \
    } } while (0)

static void cyc_reset_all(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++) { app_cyc[i] = 0; app_pct[i] = 0; }
}

u32 wm_win_cpu_pct(struct window *w)
{
    if (!w) return 0;
    for(int i=0;i<win_count;i++)if(wins[i]==w)return app_pct[i];
    return 0;
}

/* ---- damage-rect compositing: repaint and flip only what changed ---- */
struct drect { int x, y, w, h; };
static struct drect dmg[8];
static int ndmg;
static int full_dirty;
static int tb_dirty, icons_dirty, menu_dirty, launch_dirty;

static void wm_full(void) { dirty = 1; full_dirty = 1; }

/* held full-screen overlays (diagnostics, error screen) paint straight to
 * the framebuffer; when they hand control back, the WM's dirty-region
 * bookkeeping is stale and partial repaints (the taskbar clock) would draw
 * OVER the leftover overlay until some unrelated event dirtied everything.
 * Overlay code calls this on exit so the next paint is a full one. */
void wm_request_full(void) { wm_full(); }

static int mx = 300, my = 300;
static int mouse_rx, mouse_ry, mouse_sens;
static u8 mbuttons;
static struct window *drag_win, *resize_win, *app_capture;
static int drag_ox, drag_oy, resize_ox, resize_oy;

static struct {
    int active, x, y, w, h;
    const char **items;
    int n;
    menu_cb cb;
    void *ud;
} menu;

/* easter egg: home/documents/file.scv spawns ERROR popups (as in web sim) */
static int egg_armed;
static u64 egg_last_spawn;

static u64 last_blink_phase;

static int icon_hover = -1;

/* ---- taskbar layout: scrollable app buttons + power button + clock ---- */
#define TB_CLOCK_W 68
#define TB_POWER_W 34
static int power_hover;
static int picker_ids[MAX_WINDOWS];
static char picker_labels[MAX_WINDOWS][80];
static const char *picker_items[MAX_WINDOWS];
static void picker_answer(int item,void *ud){
    (void)ud;if(item<0||item>=MAX_WINDOWS)return;
    for(int i=0;i<win_count;i++)if(wins[i]->id==picker_ids[item]){
        struct window *w=wins[i];if(w->state==WIN_STATE_MIN)w->state=w->restore_state==WIN_STATE_MAX?WIN_STATE_MAX:WIN_STATE_NORMAL;wm_focus(w);return;
    }
}
static __attribute__((noinline)) void picker_open(void){
    if(!win_count){static const char *empty[]={"No open windows"};wm_menu(48,screen_h-TASKBAR_H-32,empty,1,NULL,NULL);return;}
    for(int i=0;i<win_count;i++){picker_ids[i]=wins[i]->id;strcpy(picker_labels[i],wins[i]->state==WIN_STATE_MIN?"[-] ":wins[i]==focused_w?"[+] ":"    ");strncat(picker_labels[i],wins[i]->title,63);picker_items[i]=picker_labels[i];}
    wm_menu(48,screen_h-TASKBAR_H-win_count*24-8,picker_items,win_count,picker_answer,NULL);
    menu.w=320;if(menu.w>screen_w-4)menu.w=screen_w-4;
}

#define PIN_MAX 8
static char task_pins[PIN_MAX][32];
static int pin_count;
int wm_taskbar_capacity(void){int n=(screen_w-TB_CLOCK_W-TB_POWER_W-16-168-screen_w/4)/40;if(n<0)n=0;if(n>PIN_MAX)n=PIN_MAX;return n;}
int wm_taskbar_pinned(const char *id){for(int i=0;i<pin_count;i++)if(!strcmp(task_pins[i],id))return 1;return 0;}
static void pins_save(void){char data[PIN_MAX*33+1];data[0]=0;for(int i=0;i<pin_count;i++){strcat(data,task_pins[i]);strcat(data,"\n");}vfs_write("home/taskbar.txt",data,strlen(data));}
static void pin_remove(const char *id){for(int i=0;i<pin_count;i++)if(!strcmp(task_pins[i],id)){for(int j=i;j<pin_count-1;j++)strcpy(task_pins[j],task_pins[j+1]);pin_count--;pins_save();wm_full();return;}}
int wm_taskbar_pin(const char *id){
    if(wm_taskbar_pinned(id)){pin_remove(id);return 1;}
    struct app *a=app_find(id);if(!a||id[0]=='_'||strlen(id)>30)return 0;
    if(pin_count>=wm_taskbar_capacity())return 0;
    strcpy(task_pins[pin_count++],id);pins_save();wm_full();return 1;
}
static void pins_load(void){
    pin_count=0;u32 size=0;char *p=vfs_read("home/taskbar.txt",&size);
    if(!p){const char *defaults[]={"files","terminal","studio","settings"};for(unsigned i=0;i<4&&pin_count<wm_taskbar_capacity();i++)if(app_find(defaults[i]))strcpy(task_pins[pin_count++],defaults[i]);return;}
    if(size>PIN_MAX*33)return;
    u32 start=0;for(u32 i=0;i<=size&&pin_count<PIN_MAX;i++)if(i==size||p[i]=='\n'){
        u32 n=i-start;char id[32];if(n&&n<=30){memcpy(id,p+start,n);id[n]=0;int valid=1;for(u32 j=0;j<n;j++)if(!((id[j]>='a'&&id[j]<='z')||(id[j]>='0'&&id[j]<='9')||id[j]=='-'))valid=0;struct app *a=valid?app_find(id):NULL;if(a&&id[0]!='_'&&!wm_taskbar_pinned(id))strcpy(task_pins[pin_count++],id);}start=i+1;
    }
}
static char pin_menu_id[32];
static void pin_menu_answer(int item,void *ud){(void)ud;
    if(item==0)wm_open_app(pin_menu_id,NULL);
    else if(item==1)pin_remove(pin_menu_id);
    else if(item==2)wm_desk_show_app(pin_menu_id);
}

#define NOTICE_MAX 3
/* The text a notification can carry is bounded because it is stored, not because the box is: the box is as
 * tall as the text needs (`notice_height'), so the only limit worth having is the storage, and a message
 * that needs five rows gets five rows. */
#define NOTICE_W 320
#define NOTICE_PAD 10
#define NOTICE_GAP 6
#define NOTICE_HEAD 26            /* title row, and the gap below it */
#define NOTICE_FOOT 22            /* the dismiss line and its padding */
struct notice {char title[48],text[512];u32 until;int warning;};
static struct notice notices[NOTICE_MAX];
static int notice_count;
void wm_notify(const char *title,const char *text,int warning){
    if(notice_count==NOTICE_MAX){for(int i=1;i<NOTICE_MAX;i++)notices[i-1]=notices[i];notice_count--;}
    struct notice *n=&notices[notice_count++];memset(n,0,sizeof(*n));strncpy(n->title,title,47);strncpy(n->text,text,sizeof(n->text)-1);n->text[sizeof(n->text)-1]=0;n->warning=!!warning;n->until=warning?0:(u32)tick_count+1000;wm_full();
    klog("notification: %s: %s",n->title,n->text);
}
/* The box is sized by the same layout the painter uses, from the same shared word wrap: a height computed a
 * second way is a height that can disagree with the text inside it, which is how a box ends up with a row
 * drawn outside its own frame. */
static int notice_cols(void){return s_text_cols(NOTICE_W-2*NOTICE_PAD);}
static int notice_rows(const struct notice *n)
{
    int r=s_wrap_rows(n->text,notice_cols());
    return r<1?1:r;
}
static int notice_height(const struct notice *n){return NOTICE_HEAD+notice_rows(n)*FONT_H+NOTICE_FOOT;}
static int notice_top(int index)
{
    int y=10;
    for(int i=0;i<index&&i<notice_count;i++)y+=notice_height(&notices[i])+NOTICE_GAP;
    return y;
}

static void notice_remove(int i){if(i<0||i>=notice_count)return;for(int j=i;j<notice_count-1;j++)notices[j]=notices[j+1];notice_count--;wm_full();}
/* Hit-testing walks the same heights, so a click lands on the notice the user can see rather than on the
 * fourth box in a stack of shorter ones. */
static int notice_at(int x,int y){
    if(x<screen_w-NOTICE_W-10||x>=screen_w-10||y<10)return -1;
    for(int i=0;i<notice_count;i++){int top=notice_top(i);if(y>=top&&y<top+notice_height(&notices[i]))return i;}
    return -1;
}
static void paint_notices(void){
    const struct theme *t=theme_current();int x=screen_w-NOTICE_W-10;
    for(int i=0;i<notice_count;i++){
        struct notice *n=&notices[i];int y=notice_top(i),h=notice_height(n),rows=notice_rows(n);
        u32 accent=n->warning?0xffbb55:t->main;
        s_fill(&screen,x,y,NOTICE_W,h,t->win_bg);s_frame_rect(&screen,x,y,NOTICE_W,h,accent);
        s_clip_text(&screen,x+NOTICE_PAD,y+6,n->title,accent,NOTICE_W-2*NOTICE_PAD);
        /* One surface, one layout: the box was sized by notice_rows() and the text is painted by the
         * shared paragraph painter, both of them wrapping at the same width.  A sub-surface clipped to the
         * text area would be the tidy thing to do and cannot be - `struct surface' has no stride, so a
         * region of the screen is not a surface - which leaves the guarantee where it should be: the two
         * callers agree because they call the same function, and if they ever stop agreeing, a row is drawn
         * past the frame in a screenshot rather than being silently cut. */
        s_text_wrap(&screen,x+NOTICE_PAD,y+NOTICE_HEAD,NOTICE_W-2*NOTICE_PAD,n->text,t->text);
        s_text(&screen,x+NOTICE_PAD,y+NOTICE_HEAD+rows*FONT_H+4,"Click to dismiss",t->main);
    }
}

static void tb_power_rect(int *x, int *y, int *w, int *h)
{
    *x = screen_w - TB_CLOCK_W - TB_POWER_W - 16;
    *y = screen_h - TASKBAR_H + 6;
    *w = TB_POWER_W;
    *h = TASKBAR_H - 12;
}

static void power_menu_cb(int item, void *ud)
{
    if(item<0)return;
    (void)ud;
    /* Linux: "Restart"/"Power Off" end the scoswm session and return to
     * the host desktop/session.  scoswm must not reboot or power off the
     * host machine it is running on. */
    (void)item;
    platform_request_quit();
}

/* ------------------------------------------------------------- utils ---- */
static u32 blend(u32 a, u32 b, int t)
{
    u32 ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    u32 br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    return (((ar * (255 - t) + br * t) / 255) << 16) |
           (((ag * (255 - t) + bg * t) / 255) << 8) |
           (((ab * (255 - t) + bb * t) / 255));
}

static int in_rect(int px, int py, int x, int y, int w, int h)
{
    return px >= x && py >= y && px < x + w && py < y + h;
}

int wm_win_count(void) { return win_count; }
struct window *wm_win_at(int i) { return i>=0&&i<win_count?wins[i]:NULL; }
struct window *wm_focused(void) { return focused_w; }
int wm_dialog_active(void) { return modal_w != NULL; }

int wm_content_w(struct window *w) { return w->w - 2; }
int wm_content_h(struct window *w) { return w->h - WIN_TITLEBAR - 1; }

void wm_redraw(struct window *w) { if (w) w->dirty = 1; dirty = 1; }

static void win_alloc_buf(struct window *w)
{
    int cw = wm_content_w(w), ch = wm_content_h(w);
    if (cw < 8) cw = 8;
    if (ch < 8) ch = 8;
    w->surf.px = palloc_owned((size_t)cw * ch * 4,(uintptr_t)w);
    w->surf.w = cw;
    w->surf.h = ch;
    if (w->surf.px) memset(w->surf.px, 0, (u32)cw * ch * 4);
}

static void win_free_buf(struct window *w)
{
    if (w->surf.px) {
        pfree(w->surf.px, w->surf.w * w->surf.h * 4);
        w->surf.px = NULL;
    }
}

/* App-requested geometry changes are transactional and never call back into
 * paint synchronously. Lua disallows these operations from its paint callback. */
int wm_resize_window(struct window *w,int width,int height)
{
    if(!w||!w->app||w->state!=WIN_STATE_NORMAL||w==resize_win||w==drag_win)return 0;
    int minw=w->app->min_w>320?w->app->min_w:320;
    int minh=w->app->min_h>200?w->app->min_h:200;
    if(width<minw)width=minw;
    if(height<minh)height=minh;
    if(width>screen_w-8)width=screen_w-8;
    if(height>screen_h-TASKBAR_H-8)height=screen_h-TASKBAR_H-8;
    if(width==w->w&&height==w->h)return 1;
    int cw=width-2,ch=height-WIN_TITLEBAR-1;
    u32 *p=palloc_owned((size_t)cw*ch*4,(uintptr_t)w);if(!p)return 0;
    memset(p,0,(size_t)cw*ch*4);win_free_buf(w);
    w->w=width;w->h=height;w->surf=(struct surface){p,cw,ch};
    if(w->x+width>screen_w)w->x=screen_w-width;
    if(w->y+height>screen_h-TASKBAR_H)w->y=screen_h-TASKBAR_H-height;
    w->dirty=1;wm_full();return 1;
}
int wm_move_window(struct window *w,int x,int y)
{
    if(!w||!w->app||w->state!=WIN_STATE_NORMAL||w==drag_win||w==resize_win)return 0;
    if(x<0)x=0;
    if(y<0)y=0;
    if(x>screen_w-w->w)x=screen_w-w->w;
    if(y>screen_h-TASKBAR_H-w->h)y=screen_h-TASKBAR_H-w->h;
    w->x=x;w->y=y;wm_full();return 1;
}

void wm_set_title(struct window *w, const char *title)
{
    strncpy(w->title, title, sizeof(w->title) - 1);
    w->chrome_dirty = 1;
    dirty = 1;
}

void wm_focus(struct window *w)
{
    if (!w||w->state==WIN_STATE_MIN) return;
    w->z = ++next_z;
    focused_w = w;
    wm_full();
}

void wm_minimize_window(struct window *w)
{
    if(!w||w->state==WIN_STATE_MIN)return;
    w->restore_state=w->state;w->state=WIN_STATE_MIN;
    if(app_capture==w)app_capture=NULL;
    if(focused_w==w){focused_w=NULL;for(int i=0;i<win_count;i++)if(wins[i]->state!=WIN_STATE_MIN&&(!focused_w||wins[i]->z>focused_w->z))focused_w=wins[i];}
    wm_full();
}

static struct window *win_at_point(int x, int y)
{
    if(y>=screen_h-TASKBAR_H)return NULL;
    struct window *best = NULL;
    for (int i = 0; i < win_count; i++) {
        struct window *w = wins[i];
        if (w->state == WIN_STATE_MIN) continue;
        if (in_rect(x, y, w->x, w->y, w->w, w->h))
            if (!best || w->z > best->z) best = w;
    }
    return best;
}

int wm_app_running(const char *app_id)
{
    struct app *a = app_find(app_id);
    if (!a) return 0;
    int n = 0;
    for (int i = 0; i < win_count; i++)
        if (wins[i]->app == a) n++;
    return n;
}

void wm_track_mem(struct window *w, ptrdiff_t delta)
{
    if (!w) return;
    if(delta<0){size_t n=0-(size_t)delta;w->data_bytes=n>w->data_bytes?0:w->data_bytes-n;}
    else if((size_t)delta<=SIZE_MAX-w->data_bytes)w->data_bytes+=(size_t)delta;
    else panic("window memory accounting overflow");
}

void wm_set_console(struct window *w, void *term)
{
    if (w) w->console = term;
}

static void *pending_console;      /* consumed by the next wm_open_app */
void wm_set_pending_console(void *term)
{
    pending_console = term;
}

void wm_clear_console(void *term)
{
    for (int i = 0; i < win_count; i++)
        if (wins[i]->console == term) wins[i]->console = NULL;
}

/* one real app event -> the terminal tab that launched this app, like a
 * Linux program writing to the console it was started from */
void app_log(struct window *w, const char *line)
{
    if (!w || !w->console || !line || !line[0]) return;
    char buf[208];
    strcpy(buf, "[");
    strncat(buf, w->app ? w->app->id : "app", 24);
    strcat(buf, "] ");
    strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    term_console_line(w->console, buf);
}

void wm_close_window(struct window *w)
{
    menu_cancel();
    if (!w || w->closing) return;
    w->closing = 1;
    if (w->console && w->app)
        term_console_exit(w->console, w->app->id);
    if (w->app && w->app->close) w->app->close(w);
    win_free_buf(w);
    int idx = -1;
    for (int i = 0; i < win_count; i++) if (wins[i] == w) idx = i;
    if (idx >= 0) {
        for (int i = idx; i < win_count - 1; i++) wins[i] = wins[i + 1];
        win_count--;
        cyc_reset_all();   /* r39: window indices shifted; CPU% rebuilds
                            * from zero within the next second */
    }
    w->app=NULL;w->data=NULL;
    if (focused_w == w) focused_w = NULL;
    if (modal_w == w) modal_w = NULL;
    if (drag_win == w) drag_win = NULL;
    if (app_capture == w) app_capture = NULL;
    if (resize_win == w) resize_win = NULL;
    if (!focused_w) {
        struct window *top = NULL;
        for (int i = 0; i < win_count; i++)
            if (wins[i]->state!=WIN_STATE_MIN&&(!top || wins[i]->z > top->z)) top = wins[i];
        focused_w = top;
    }
    wm_full();
}

struct window *wm_open_app(const char *app_id, void *arg)
{
    /* console hand-off: `appstrt` parks the launching terminal tab here so
     * the app's open()-time log lines already reach it */
    void *cons = pending_console;
    pending_console = NULL;
    struct app *app = app_find(app_id);
    if (!app) return NULL;
    if (app->single)
        for (int i = 0; i < win_count; i++)
            if (wins[i]->app == app) {
                if (wins[i]->state == WIN_STATE_MIN) wins[i]->state = wins[i]->restore_state==WIN_STATE_MAX?WIN_STATE_MAX:WIN_STATE_NORMAL;
                if (cons) wins[i]->console = cons;
                wm_focus(wins[i]);
                if(arg&&app->document){APP_T0(wins[i]);app->document(wins[i],arg);APP_T1(wins[i]);}
                return wins[i];
            }
    if (win_count >= MAX_WINDOWS) return NULL;
    struct window *w = NULL;
    for(int i=0;i<MAX_WINDOWS;i++)if(!window_slots[i].app){w=&window_slots[i];break;}
    if(!w)return NULL;
    wins[win_count++]=w;
    cyc_reset_all();
    memset(w, 0, sizeof(*w));
    w->console = cons;
    w->id = next_id++;
    w->app = app;
    strncpy(w->title, app->title, sizeof(w->title) - 1);
    int dw = app->def_w ? app->def_w : 600;
    int dh = app->def_h ? app->def_h : 400;
    if (dw > screen_w - 8) dw = screen_w - 8;
    if (dh > screen_h - TASKBAR_H - 8) dh = screen_h - TASKBAR_H - 8;
    w->w = dw; w->h = dh;
    w->x = 40 + (win_count * 28) % (screen_w - dw - 60 > 0 ? screen_w - dw - 60 : 1);
    w->y = 24 + (win_count * 26) % (screen_h - TASKBAR_H - dh - 40 > 0 ? screen_h - TASKBAR_H - dh - 40 : 1);
    w->z = ++next_z;
    w->state = WIN_STATE_NORMAL;
    win_alloc_buf(w);
    if (!w->surf.px) {
        win_count--;
        w->app=NULL;
        err_notify(app->id,
                   "Not enough memory to open this window. Close some "
                   "windows or apps and try again.", NULL, 0);
        return NULL;
    }
    w->dirty = 1;
    if (app->open) { APP_T0(w);app->open(w, arg);APP_T1(w); }
    if (app->uses_data && !w->data) {
        win_count--;             /* app could not allocate its state */
        win_free_buf(w);w->app=NULL;
        err_notify(app->id,
                   "Not enough memory to open this app. Close some "
                   "windows or apps and try again.", NULL, 0);
        return NULL;
    }
    if(modal_w)focused_w=modal_w;else if(w->state!=WIN_STATE_MIN)focused_w=w;
    wm_full();
    return w;
}

void wm_init(void)
{
    win_count = 0;
    focused_w = NULL;
    modal_w = NULL;
    mx = screen_w / 2;
    my = screen_h / 2;
    desktop_load();pins_load();
    wm_full();
}

/* -------------------------------------------------------------- menus ---- */
static __attribute__((noinline)) void menu_cancel(void){
    if(!menu.active)return;
    menu.active=0;
    if(menu.cb)menu.cb(-1,menu.ud);
}
void wm_menu(int x, int y, const char **items, int n, menu_cb cb, void *ud)
{
    menu_cancel();
    menu.active = 1;
    menu.items = items;
    menu.n = n;
    menu.cb = cb;
    menu.ud = ud;
    menu.w = 170;
    for(int i=0;i<n;i++){int width=s_text_width(items[i])+20;if(width>320)width=320;if(width>menu.w)menu.w=width;}
    menu.h = n * 24 + 6;
    menu.x = x; menu.y = y;
    if (menu.x + menu.w > screen_w) menu.x = screen_w - menu.w - 2;
    if (menu.y + menu.h > screen_h) menu.y = screen_h - menu.h - 2;
    if(menu.y<2)menu.y=2;
    if(menu.x<2)menu.x=2;
    menu_dirty = 1;
    dirty = 1;
}

/* ------------------------------------------------------------- paint ---- */
static struct surface wp_cache;
static int wp_valid;

void wm_wallpaper_invalidate(void) { wp_valid = 0; }

/*
 * The wallpaper is static per theme: render it once into a cache and blit
 * from there on every composite (per-pixel gradient + discs used to run on
 * every single repaint, which dominated CPU use in slow emulators).
 */
static void paint_wallpaper(void)
{
    const struct theme *t = theme_current();
    if (!wp_valid) {
        if (!wp_cache.px) {
            wp_cache.px = palloc_owned((size_t)screen_w * screen_h * 4,HEAP_WM);
            wp_cache.w = screen_w;
            wp_cache.h = screen_h;
        }
        if (wp_cache.px) {
            struct surface *c = &wp_cache;
            u32 v[10];theme_values(t,v);
            if(v[7]==0)s_fill(c,0,0,screen_w,screen_h,t->bg_top);
            else s_vgrad(c,0,0,screen_w,screen_h,t->bg_top,t->bg_bot);
            if(v[7]>=2)for(int x=0;x<screen_w;x+=(int)v[8])
                s_fill(c,x,0,v[7]==3?(int)v[8]/2:1,screen_h,v[9]);
            if(v[7]==2)for(int y=0;y<screen_h;y+=(int)v[8])
                s_fill(c,0,y,screen_w,1,v[9]);
            wp_valid = 1;
        }
    }
    if (wp_valid)
        memcpy(screen.px, wp_cache.px, (u32)screen_w * screen_h * 4);
    else
        s_fill(&screen, 0, 0, screen_w, screen_h, t->bg_top);
}

static int icon_band = 232;   /* px height of the icon region actually in use */
static int in_full;           /* paint_all is running: skip partial restores */

/* restore one rectangle of clean wallpaper into the back buffer (used by
 * partial repaints to erase stale hover/selection/cursor pixels) */
static void wp_restore_rect(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;
    if (w <= 0 || h <= 0) return;
    if (wp_valid) {
        for (int r = 0; r < h; r++)
            memcpy(screen.px + (u32)(y + r) * screen_w + x,
                   wp_cache.px + (u32)(y + r) * screen_w + x, (u32)w * 4);
    } else {
        s_fill(&screen, x, y, w, h, theme_current()->bg_top);
    }
}

/* ---- desktop items: app launchers + pinned files on a snap grid ---- */
#define DESK_MAX  32
#define DESK_COLS 11
#define DESK_ROWS 7
struct ditem { int kind; char app[32]; char path[192]; char label[40];
               int gx, gy, hidden; };
static struct ditem items[DESK_MAX];
static int nitems;
static u32 desk_sel;                       /* selection bitmask */
static int desk_drag = -1, desk_drag_ox, desk_drag_oy, desk_drag_moved;
static int desk_sx, desk_sy;
static int band_button;
static int band_active, band_x0, band_y0, band_x1, band_y1;
/* ------------------------------------------------------- drag and drop ---- */
/* A drag carries one of two things, because those are the two things a desktop can hold: the id of an
 * application, or a path.  Nothing else is invented, so no target has to guess which of the two it is
 * being offered.  The *source* decides when a drag begins (six pixels of movement, the same threshold
 * the icon grid uses, so a click never becomes a drop by accident) and the *WM* decides what a drop
 * means, by what is under the pointer when the button is released - see wm_dnd_target(). */
static struct { int active; int kind; char payload[256]; char label[48]; } dnd;
static int dnd_dirty;
/* The box the shadow actually covered on the last painted frame.  A drag's label is clamped to the far
 * side of the cursor near a screen edge, so recomputing "where it was" from the previous cursor position
 * would be a guess; the painter records the truth instead, and the next move repairs exactly that. */
static int dnd_lx, dnd_ly, dnd_lw, dnd_lh;
static void dnd_rect(int *x, int *y, int *w, int *h);
static struct { int active; char q[48]; int qpos; int hover, offset; } launch;
/* The launcher row a press landed on, and where that press was.  A launcher that opened the window on
 * the button-down could not also be a place to pick an application up, so the click is decided at
 * release: same row = launch, moved = drag. */
static int launch_press = -1, launch_press_x, launch_press_y;

static int ditem_icon(struct ditem *d)
{
    if (d->kind == 0) { struct app *a = app_find(d->app); if (a) return a->icon; }
    return ICON_NOTEPAD;
}
static int cell_taken(int gx, int gy, int skip)
{
    for (int i = 0; i < nitems; i++)
        if (i != skip && !items[i].hidden && items[i].gx == gx && items[i].gy == gy) return 1;
    return 0;
}
static void desktop_save(void)
{
    char buf[2048];
    strcpy(buf, "SCOSDESK1\n");
    for (int i = 0; i < nitems; i++) {
        char n[8];
        fmt_u32(n, (u32)items[i].kind); strcat(buf, n); strcat(buf, "|");
        strcat(buf, items[i].app); strcat(buf, "|");
        strcat(buf, items[i].path); strcat(buf, "|");
        strcat(buf, items[i].label); strcat(buf, "|");
        fmt_u32(n, (u32)items[i].gx); strcat(buf, n); strcat(buf, "|");
        fmt_u32(n, (u32)items[i].gy); strcat(buf, n); strcat(buf, "|");
        fmt_u32(n, (u32)items[i].hidden); strcat(buf, n); strcat(buf, "\n");
        if (strlen(buf) > 1900) break;
    }
    vfs_write("system/desktop.json", buf, (u32)strlen(buf));
}
static void desktop_load(void)
{
    nitems = 0; desk_sel = 0;
    u32 len = 0;
    char *stored=vfs_read("system/desktop.json",&len),*data=NULL;
    if(stored&&len<=16384){data=palloc(len+1);if(data){memcpy(data,stored,len);data[len]=0;}}
    if (data && len > 10 && !memcmp(data, "SCOSDESK1", 9)) {
        char *p = data + 10;
        while (*p && nitems < DESK_MAX) {
            char *fld[7]; int nf = 0;
            fld[nf++] = p;
            while (*p && *p != '\n') { if (*p == '|') { *p = 0; if (nf < 7) fld[nf++] = p + 1; } p++; }
            if (*p) *p++ = 0;
            if (nf < 7) break;
            struct ditem *d = &items[nitems];
            d->kind = (int)str_to_u32(fld[0]);
            strncpy(d->app, fld[1], 31); d->app[31] = 0;
            strncpy(d->path, fld[2], 191); d->path[191] = 0;
            strncpy(d->label, fld[3], 39); d->label[39] = 0;
            d->gx = (int)str_to_u32(fld[4]); d->gy = (int)str_to_u32(fld[5]);
            d->hidden = (int)str_to_u32(fld[6]);
            nitems++;
        }
    }
    if(data)pfree(data,len+1);
    if (!nitems) {
        for (int i = 0; i < app_count() && nitems < DESK_MAX; i++) {
            const struct app *a = app_at(i);
            if (a->id[0] == '_') continue;
            struct ditem *d = &items[nitems++];
            d->kind = 0;
            strncpy(d->app, a->id, 31);
            d->path[0] = 0;
            strncpy(d->label, a->desktop_label ? a->desktop_label : a->title, 39);
            d->gx = (nitems - 1) % 8; d->gy = (nitems - 1) / 8; d->hidden = 0;
        }
    }
}
static void desktop_open(struct ditem *d)
{
    if (d->kind == 0) wm_open_app(d->app, NULL);
    else app_open_document(d->path);
}
static void desk_remove_sel(void)
{
    for (int i = 0; i < nitems; i++) {
        if (!(desk_sel & (1u << i))) continue;
        if (items[i].kind == 0) items[i].hidden = 1;
        else {
            for (int k = i; k + 1 < nitems; k++) items[k] = items[k + 1];
            nitems--; i--;
        }
    }
    desk_sel = 0;
    desktop_save();
}
void wm_desktop_pin_file(const char *path)
{
    if (nitems >= DESK_MAX) return;
    for (int i = 0; i < nitems; i++)
        if (!items[i].kind && !strcmp(items[i].path, path) == 0 && items[i].kind == 1 && !strcmp(items[i].path, path)) return;
    struct ditem *d = &items[nitems];
    d->kind = 1;
    d->app[0] = 0;
    strncpy(d->path, path, 191); d->path[191] = 0;
    const char *base = path;
    for (const char *q = path; *q; q++) if (*q == '/') base = q + 1;
    strncpy(d->label, base, 39); d->label[39] = 0;
    int placed = 0;
    for (int gy = 0; gy < DESK_ROWS && !placed; gy++)
        for (int gx = 0; gx < DESK_COLS && !placed; gx++)
            if (!cell_taken(gx, gy, -1)) { d->gx = gx; d->gy = gy; placed = 1; }
    if (!placed) { d->gx = 0; d->gy = 0; }
    d->hidden = 0;
    nitems++;
    desktop_save();
    icons_dirty = 1;
    dirty = 1;
}
/* Forget one desktop entry entirely (a pinned path the user no longer wants on the desktop).  Hiding is
 * the common case and reversible; this is not, and it is only ever offered for the file kind, where the
 * shortcut and the file are different things and the file stays exactly where it was. */
static void desk_forget(int i)
{
    if (i < 0 || i >= nitems) return;
    for (int k = i; k + 1 < nitems; k++) items[k] = items[k + 1];
    nitems--;
    desk_sel = 0;
    desktop_save();
    icons_dirty = 1;
    dirty = 1;
}

/* Files is the one application that can show a folder, so "where did this come from" is answered by
 * opening it there rather than by inventing a properties window for a shortcut. */
static void desk_reveal(const char *path)
{
    char dir[256];
    struct vfs_node *n = vfs_lookup(path);
    if (n && n->is_dir) { wm_open_app("files", (void *)path); return; }
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char *slash = 0;
    for (char *q = dir; *q; q++) if (*q == '/') slash = q;
    if (slash) slash[1] = 0; else strcpy(dir, "/");
    if (!dir[0]) strcpy(dir, "/");
    if (!wm_open_app("files", dir)) wm_notify("Files unavailable", "The folder could not be opened.", 1);
}

/* ------------------------------------------------- per-item actions ---- */
/* An icon's menu is what *that icon* is able to do, computed each time: an application can be pinned
 * and hidden, a shortcut to a file can be followed and forgotten, and a built-in is never offered an
 * "Uninstall" it would refuse.  The labels come from the same table the actions run, so the menu, the
 * keyboard paths and the test suite cannot drift into three descriptions of the same desktop. */
static char desk_labels[6][40];
/* "Open with..." needs a list that outlives the call that builds it, because the menu hands the answer
 * back later; one list is enough, since only one menu can be raised at a time. */
static char desk_openwith_path[300];
static char desk_openwith_ids[16][32];
static char desk_openwith_text[16][40];
static const char *desk_openwith_labels[16];
static int desk_openwith_count;
int wm_desk_actions(int item, const char **labels, int max)
{
    if (item < 0 || item >= nitems || !labels || max < 6) return -1;
    struct app *a = items[item].kind == 0 ? app_find(items[item].app) : 0;
    int n = 0;
/* Each line is written into storage that outlives this call (a menu keeps the pointer until it is
 * answered) and counted in a separate statement, because `labels[n++] = desk_labels[n - 1]` reads and
 * writes n without a sequence point in between. */
#define DESK_LINE(text) do { strcpy(desk_labels[n], (text)); labels[n] = desk_labels[n]; n++; } while (0)
    DESK_LINE("Open");
    if (a) DESK_LINE(wm_taskbar_pinned(a->id) ? "Unpin from taskbar" : "Pin to taskbar");
    else DESK_LINE("Show folder in Files");
    DESK_LINE(a ? "Hide this icon" : "Remove this shortcut");
    /* A shortcut keeps the two lines its own row has in File Explorer, so the same work is possible
     * from either surface.  An application icon is left alone: its name comes from the application,
     * and it has exactly one handler. */
    if (!a) { DESK_LINE("Open with..."); DESK_LINE("Rename shortcut"); }
    /* Built-in apps are not removable, so the line is not offered for them at all; the uninstall
     * request itself still refuses, because a menu is a convenience and not the protection. */
    if (a && a->external) DESK_LINE("Uninstall app");
#undef DESK_LINE
    return n;
}

int wm_desk_invoke(int item, int action)
{
    const char *labels[6];
    int n = wm_desk_actions(item, labels, 6);
    if (n < 0 || action < 0 || action >= n) return -1;
    struct app *a = items[item].kind == 0 ? app_find(items[item].app) : 0;
    if (action == 0) {
        desk_sel = (1u << item);
        desktop_open(&items[item]);
        return 0;
    }
    if (a) {
        if (!strcmp(labels[action], "Unpin from taskbar")) { pin_remove(a->id); return 0; }
        if (!strcmp(labels[action], "Pin to taskbar")) {
            if (!wm_taskbar_pin(a->id))
                wm_notify("Taskbar full", "Unpin something first - the number of pins follows the "
                          "width of the screen.", 1);
            return 0;
        }
        if (!strcmp(labels[action], "Hide this icon")) {
            items[item].hidden = 1;
            desk_sel = 0;
            desktop_save();
            icons_dirty = 1;
            dirty = 1;
            return 0;
        }
        if (!strcmp(labels[action], "Uninstall app")) { app_request_uninstall(a->id); return 0; }
        return -1;
    }
    if (!strcmp(labels[action], "Show folder in Files")) { desk_reveal(items[item].path); return 0; }
    if (!strcmp(labels[action], "Remove this shortcut")) { desk_forget(item); return 0; }
    if (!strcmp(labels[action], "Open with...")) { desk_openwith(item); return 0; }
    if (!strcmp(labels[action], "Rename shortcut")) { desk_rename_ask(item); return 0; }
    return -1;
}

/* 1 = the app's icon is hidden, 0 = it is on the desktop, -1 = it has no desktop entry at all.  The
 * taskbar needs this because "put that icon back" is only worth offering for an app whose icon was put
 * away, and the answer belongs to the desktop rather than to whoever is asking. */
int wm_desk_app_state(const char *app_id)
{
    if (!app_id || !app_id[0]) return -1;
    for (int i = 0; i < nitems; i++)
        if (items[i].kind == 0 && !strcmp(items[i].app, app_id)) return items[i].hidden ? 1 : 0;
    return -1;
}

int wm_desk_show_app(const char *app_id)
{
    if (!app_id || !app_id[0]) return -1;
    for (int i = 0; i < nitems; i++)
        if (items[i].kind == 0 && !strcmp(items[i].app, app_id) && items[i].hidden) {
            items[i].hidden = 0;
            icons_dirty = 1;
            dirty = 1;
            desktop_save();
            return 0;
        }
    return -1;
}

static void desk_item_menu_cb(int item, void *ud)
{
    if (item < 0) return;
    int which = (int)(intptr_t)ud;
    if (which >= 0) { wm_desk_invoke(which, item); wm_full(); }
}
/* bring back every hidden app icon and re-add missing registered apps */
static void desk_restore(void)
{
    lua_apps_refresh();
    for (int i = 0; i < nitems; i++)
        if (items[i].kind == 0) items[i].hidden = 0;
    for (int a = 0; a < app_count(); a++) {
        const struct app *ap = app_at(a);
        if (ap->id[0] == '_') continue;
        int found = 0;
        for (int i = 0; i < nitems; i++)
            if (items[i].kind == 0 && strcmp(items[i].app, ap->id) == 0) {found = 1;strncpy(items[i].label,ap->desktop_label?ap->desktop_label:ap->title,39);items[i].label[39]=0;}
        if (found || nitems >= DESK_MAX) continue;
        struct ditem *d = &items[nitems];
        memset(d, 0, sizeof(*d));
        d->kind = 0;
        strcpy(d->app, ap->id);
        strcpy(d->label, ap->desktop_label ? ap->desktop_label : ap->title);
        int placed = 0;
        for (int gy = 0; gy < DESK_ROWS && !placed; gy++)
            for (int gx = 0; gx < DESK_COLS && !placed; gx++)
                if (!cell_taken(gx, gy, -1)) { d->gx = gx; d->gy = gy; placed = 1; }
        if (placed) nitems++;
    }
    desktop_save();
    icons_dirty = 1;
    dirty = 1;
}

void wm_desktop_restore(void) { desk_restore(); }

/* enumerate visible desktop items for the file manager's desktop view */
/* Install only this application's shortcut, without unhiding unrelated icons. */
void wm_desktop_install(const char *id)
{
    struct app *a=app_find(id);if(!a||id[0]=='_')return;
    for(int i=0;i<nitems;i++)if(items[i].kind==0&&!strcmp(items[i].app,id)){
        items[i].hidden=0;strncpy(items[i].label,a->desktop_label?a->desktop_label:a->title,39);items[i].label[39]=0;desktop_save();wm_full();return;
    }
    if(nitems>=DESK_MAX)return;
    for(int y=0;y<DESK_ROWS;y++)for(int x=0;x<DESK_COLS;x++)if(!cell_taken(x,y,-1)){
        struct ditem *d=&items[nitems++];memset(d,0,sizeof(*d));strcpy(d->app,id);strncpy(d->label,a->desktop_label?a->desktop_label:a->title,39);d->gx=x;d->gy=y;desktop_save();wm_full();return;
    }
}

void wm_desktop_remove_app(const char *id){
    for(int i=nitems-1;i>=0;i--)if(items[i].kind==0&&!strcmp(items[i].app,id)){for(int j=i;j<nitems-1;j++)items[j]=items[j+1];nitems--;}
    desk_sel=0;desk_drag=-1;pin_remove(id);desktop_save();wm_full();
}

int wm_desk_vis_count(void)
{
    int n = 0;
    for (int i = 0; i < nitems; i++)
        if (!items[i].hidden) n++;
    return n;
}

int wm_desk_vis_get(int idx, char *app, char *path, char *label, int *kind)
{
    int n = 0;
    for (int i = 0; i < nitems; i++) {
        if (items[i].hidden) continue;
        if (n++ != idx) continue;
        strcpy(app, items[i].app);
        strcpy(path, items[i].path);
        strcpy(label, items[i].label);
        *kind = items[i].kind;
        return 1;
    }
    return 0;
}

static void desk_empty_menu_cb(int item, void *ud)
{
    if(item<0)return;
    (void)ud;
    if (item == 1) { wm_open_app("files", 0); return; }
    if (item == 2) { wm_open_app("terminal", 0); return; }
    desk_restore();
}

static void desk_sel_menu_cb(int item, void *ud)
{
    if(item<0)return;
    (void)ud;
    if (item == 0) desk_remove_sel();
    else desk_sel = 0;
}

static void icon_rect(int i, int *x, int *y, int *w, int *h)
{
    *x = 16 + items[i].gx * 88;
    *y = 16 + items[i].gy * 96;
    *w = 80; *h = 88;
}
static void desk_rename_answer(int ok, const char *text, void *ud)
{
    int item = (int)(intptr_t)ud;
    if (!ok || item < 0 || item >= nitems || !text || !text[0]) return;
    if (items[item].kind != 1) return;
    char next[sizeof items[item].label];
    int n = 0;
    /* A shortcut's name is stored in the desktop record, whose fields are separated by '|'; a name
     * carrying one would be read back as two fields, so it is replaced rather than escaped. */
    for (; text[n] && n + 1 < (int)sizeof next; n++) next[n] = text[n] == '|' ? '-' : text[n];
    next[n] = 0;
    if (!next[0] || (next[0] == ' ' && !next[1])) return;
    strcpy(items[item].label, next);
    desktop_save();
    icons_dirty = 1;
    dirty = 1;
}

static void desk_rename_ask(int item)
{
    static char ask[224];
    if (item < 0 || item >= nitems) return;
    if (items[item].kind != 1) {
        wm_notify("Not renamable", "That name belongs to the application itself - only a shortcut to "
                  "a file can be renamed here.", 0);
        return;
    }
    /* The field starts empty and the old name goes in the question, which is how File Explorer's own
     * rename asks it: an input pre-filled at the end invites a new name typed onto the end of the old
     * one, and the desktop has no selection key to clear it with. */
    strcpy(ask, "Rename \"");
    strncat(ask, items[item].label, sizeof items[item].label);
    strcat(ask, "\" to:");
    wm_dialog("Rename shortcut", ask, "", desk_rename_answer, (void *)(intptr_t)item);
    dirty = 1;
}

static void desk_openwith_answer(int item, void *ud)
{
    (void)ud;
    if (item < 0 || item >= desk_openwith_count) return;
    if (!wm_open_app(desk_openwith_ids[item], desk_openwith_path))
        wm_notify("Cannot open it there", "That application did not take the file.", 1);
}

/* The desktop's share of Files' per-row menu.  A shortcut points at a file, so the file's other
 * handlers have to be reachable from the icon too - otherwise the shortcut is a launch button with
 * one fixed answer, and the file it names can only be opened elsewhere. */
static void desk_openwith(int item)
{
    if (item < 0 || item >= nitems || items[item].kind != 1) return;
    struct vfs_node *node = vfs_lookup(items[item].path);
    if (!node) {
        wm_notify("File is gone", "That shortcut points at something that is not on this machine any "
                  "more.", 1);
        return;
    }
    if (node->is_dir) { desk_reveal(items[item].path); return; }
    desk_openwith_count = 0;
    for (int i = 0; i < app_count() && desk_openwith_count < 16; i++) {
        struct app *a = app_at(i);
        if (!a || a->id[0] == '_' || (!a->document && !a->open)) continue;
        strcpy(desk_openwith_ids[desk_openwith_count], a->id);
        strncpy(desk_openwith_text[desk_openwith_count], a->title,
                sizeof desk_openwith_text[0] - 1);
        desk_openwith_text[desk_openwith_count][sizeof desk_openwith_text[0] - 1] = 0;
        desk_openwith_labels[desk_openwith_count] = desk_openwith_text[desk_openwith_count];
        desk_openwith_count++;
    }
    if (!desk_openwith_count) {
        wm_notify("Nothing else can open this", "No installed application accepts documents.", 1);
        return;
    }
    strcpy(desk_openwith_path, items[item].path);
    int x, y, w, h;
    icon_rect(item, &x, &y, &w, &h);
    wm_menu(x, y + h + 2, desk_openwith_labels, desk_openwith_count, desk_openwith_answer, NULL);
    dirty = 1;
}

u32 wm_desk_sel_mask(void) { return desk_sel; }

int wm_desk_pos(int vis_idx, int *gx, int *gy)
{
    int n = 0;
    for (int i = 0; i < nitems; i++) {
        if (items[i].hidden) continue;
        if (n++ != vis_idx) continue;
        if (gx) *gx = items[i].gx;
        if (gy) *gy = items[i].gy;
        return 0;
    }
    return -1;
}

int wm_desk_action_index(int item, const char *label)
{
    const char *labels[6];
    int n = wm_desk_actions(item, labels, 6);
    if (n < 0 || !label) return -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(labels[i], label)) return i;
    return -1;
}

int wm_app_minimized(const char *app_id)
{
    if (!app_id || !app_id[0]) return -1;
    int any = 0, shown = 0;
    for (int i = 0; i < win_count; i++) {
        struct window *w = wins[i];
        if (!w->app || strcmp(w->app->id, app_id)) continue;
        any = 1;
        if (w->state != WIN_STATE_MIN) shown = 1;
    }
    return any ? (shown ? 0 : 1) : -1;
}

static void paint_icons(void)
{
    const struct theme *t = theme_current();
    icon_hover = -1;
    /* restore the whole icon band from clean wallpaper, not just the cells:
     * a cursor burnt into a GAP between cells used to survive the repaint,
     * get baked into cur_buf as fake "background", and stamp sticky ghost
     * spots everywhere the cursor went afterwards */
    int band = 16;
    for (int i = 0; i < nitems; i++) {
        if (items[i].hidden) continue;
        int bot = 16 + items[i].gy * 96 + 88;
        if (bot > band) band = bot;
    }
    band += 8;
    icon_band = band;
    if (!in_full) wp_restore_rect(0, 0, screen_w, band);
    /* no hover feedback while dragging/resizing/modally busy: the pointer is
     * occupied and highlighting launch buttons underneath is misleading */
    int interactive = !drag_win && !resize_win && desk_drag < 0 &&
                      !band_active && !menu.active && !launch.active && !modal_w && !dnd.active &&
                      !win_at_point(mx, my);
    for (int i = 0; i < nitems; i++) {
        if (items[i].hidden) continue;
        int x, y, w, h;
        icon_rect(i, &x, &y, &w, &h);
        if (desk_drag == i && desk_drag_moved) { x = mx - desk_drag_ox; y = my - desk_drag_oy; }
        if (desk_sel & (1u << i)) {
            s_fill(&screen, x, y, w, h, blend(t->bg_top, t->main, 30));
            s_frame_rect(&screen, x, y, w, h, t->main);
        } else if (interactive && in_rect(mx, my, x, y, w, h)) {
            icon_hover = i;
            s_fill(&screen, x, y, w, h, blend(t->bg_top, t->main, 18));
        }
        s_icon(&screen, ditem_icon(&items[i]), x + (w - 24) / 2, y + 8, t->main);
        int tw = s_text_width(items[i].label);if(tw>w)tw=w;
        s_clip_text(&screen, x + (w - tw) / 2, y + 40, items[i].label, t->text, w);
    }
    if (band_active) {
        int x = band_x0 < band_x1 ? band_x0 : band_x1;
        int y = band_y0 < band_y1 ? band_y0 : band_y1;
        int w = band_x0 < band_x1 ? band_x1 - band_x0 : band_x0 - band_x1;
        int h = band_y0 < band_y1 ? band_y1 - band_y0 : band_y0 - band_y1;
        s_fill(&screen, x, y, w, h, blend(t->bg_top, t->main, 15));
        s_frame_rect(&screen, x, y, w, h, t->main);
    }
}

static void draw_win_button(struct surface *s, int x, int y, int kind, u32 fg)
{
    /* kind 0 minimize, 1 maximize, 2 close */
    if (kind == 0) s_fill(s, x + 4, y + 12, 10, 2, fg);
    else if (kind == 1) s_frame_rect(s, x + 4, y + 4, 10, 10, fg);
    else {
        s_line(s, x + 4, y + 4, x + 13, y + 13, fg);
        s_line(s, x + 13, y + 4, x + 4, y + 13, fg);
    }
}

static void paint_win_chrome(struct window *w)
{
    const struct theme *t = theme_current();
    int x = w->x, y = w->y, ww = w->w, hh = w->h;
    s_fill(&screen, x + 4, y + 4, ww, hh, blend(t->bg_bot, 0x000000, 60));
    u32 title_bg = (focused_w == w) ? t->main : blend(t->main, t->win_bg, 55);
    s_fill(&screen, x, y, ww, hh, t->win_bg);
    s_frame_rect(&screen, x, y, ww, hh, t->main);
    s_fill(&screen, x + 1, y + 1, ww - 2, WIN_TITLEBAR - 2, title_bg);
    u32 title_fg = (focused_w == w) ? t->title_text : t->text;
    s_clip_text(&screen, x + 8, y + 5, w->title, title_fg, ww - 90);
    for (int b = 0; b < 3; b++)
        draw_win_button(&screen, x + ww - 22 - (2 - b) * 22, y + 3, b, title_fg);
    u32 hz = blend(t->win_bg, t->main, 60);
    for (int i = 0; i < 4; i++)
        s_fill(&screen, x + ww - 12 + i * 3, y + hh - 12 + i * 3, 2, 2, hz);
}

static void paint_win_content(struct window *w)
{
    if (!w->surf.px) return;     /* allocation failed - chrome only */
    s_blit(&screen, &w->surf, w->x + 1, w->y + WIN_TITLEBAR);
}

static void paint_window(struct window *w)
{
    if (w->state == WIN_STATE_MIN) return;
    paint_win_chrome(w);
    paint_win_content(w);
}

#define CUR_W 14
#define CUR_H 19
static u32 *cur_buf;
static int cur_bx, cur_by, cur_bw, cur_bh, cur_have;
static void cur_draw(void);
static void cur_restore(void);
static void paint_icons(void);
static void paint_taskbar(void);
static void paint_menu(void);
static void paint_dnd(void);
static void paint_launcher(void);


static void damage_add(int x, int y, int w, int h)
{
    if(x<0){w+=x;x=0;}if(y<0){h+=y;y=0;}
    if(w>screen_w-x)w=screen_w-x;
    if(h>screen_h-y)h=screen_h-y;
    if(w<=0||h<=0)return;
    if (ndmg >= 8) { ndmg = 0; dmg[0].x = 0; dmg[0].y = 0;
                    dmg[0].w = screen_w; dmg[0].h = screen_h; ndmg = 1; return; }
    for (int i = 0; i < ndmg; i++) {
        struct drect *r = &dmg[i];
        if (x < r->x + r->w + 8 && r->x < x + w + 8 &&
            y < r->y + r->h + 8 && r->y < y + h + 8) {
            int x1 = r->x < x ? r->x : x, y1 = r->y < y ? r->y : y;
            int x2 = r->x + r->w > x + w ? r->x + r->w : x + w;
            int y2 = r->y + r->h > y + h ? r->y + r->h : y + h;
            r->x = x1; r->y = y1; r->w = x2 - x1; r->h = y2 - y1;
            return;
        }
    }
    dmg[ndmg].x = x; dmg[ndmg].y = y; dmg[ndmg].w = w; dmg[ndmg].h = h;
    ndmg++;
}

/* Rebuild damaged pixels from wallpaper upward under a scissor. Never mutate
 * the scene while collecting mouse reports and never expand damage by blitting
 * an entire overlapping window. Cached app surfaces remain independent. */
static __attribute__((noinline)) void paint_partial(void)
{
    if(cur_have){damage_add(cur_bx,cur_by,cur_bw,cur_bh);cur_restore();}
    if(icons_dirty)damage_add(0,0,screen_w,icon_band);
    if(tb_dirty)damage_add(0,screen_h-TASKBAR_H,screen_w,TASKBAR_H);
    if(menu.active&&menu_dirty)damage_add(menu.x,menu.y,menu.w,menu.h);
    if(launch.active&&launch_dirty)damage_add(8,screen_h-TASKBAR_H-308,300,308);
    if(dnd.active&&dnd_dirty){int dx,dy,dw,dh;dnd_rect(&dx,&dy,&dw,&dh);damage_add(dx,dy,dw,dh);}
    for(int i=0;i<win_count;i++){
        struct window *w=wins[i];if(w->state==WIN_STATE_MIN)continue;
        if(w->dirty&&w->app&&w->app->paint){APP_T0(w);w->app->paint(w);APP_T1(w);damage_add(w->x+1,w->y+WIN_TITLEBAR,w->surf.w,w->surf.h);w->dirty=0;}
        if(w->chrome_dirty){damage_add(w->x,w->y,w->w+4,w->h+4);w->chrome_dirty=0;}
    }
    damage_add(mx,my,CUR_W,CUR_H);
    for(int r=0;r<ndmg;r++){
        struct drect *d=&dmg[r];fb_scene_clip(d->x,d->y,d->w,d->h);
        wp_restore_rect(d->x,d->y,d->w,d->h);
        in_full=1;paint_icons();in_full=0;
        u8 drawn[MAX_WINDOWS]={0};
        for(int pass=0;pass<win_count;pass++){
            int k=-1;for(int i=0;i<win_count;i++)if(!drawn[i]&&wins[i]->state!=WIN_STATE_MIN&&(k<0||wins[i]->z<wins[k]->z))k=i;
            if(k<0)break;
            drawn[k]=1;struct window *w=wins[k];
            if(w->x<d->x+d->w&&w->x+w->w+4>d->x&&w->y<d->y+d->h&&w->y+w->h+4>d->y)paint_window(w);
        }
        paint_taskbar();paint_menu();if(launch.active)paint_launcher();paint_notices();paint_dnd();
        fb_scene_unclip();
    }
    cur_draw();
    for(int i=0;i<ndmg;i++)fb_flip_rect(dmg[i].x,dmg[i].y,dmg[i].w,dmg[i].h);
    ndmg=0;icons_dirty=tb_dirty=menu_dirty=launch_dirty=dnd_dirty=0;
}

static void paint_menu(void)
{
    if (!menu.active) return;
    const struct theme *t = theme_current();
    s_fill(&screen, menu.x, menu.y, menu.w, menu.h, t->win_bg);
    s_frame_rect(&screen, menu.x, menu.y, menu.w, menu.h, t->main);
    for (int i = 0; i < menu.n; i++) {
        int iy = menu.y + 3 + i * 24;
        if (in_rect(mx, my, menu.x, iy, menu.w, 24))
            s_fill(&screen, menu.x + 1, iy, menu.w - 2, 24, blend(t->win_bg, t->main, 25));
        s_clip_text(&screen, menu.x + 10, iy + 4, menu.items[i], t->text,menu.w-20);
    }
}

static void paint_taskbar(void)
{
    const struct theme *t = theme_current();
    int y = screen_h - TASKBAR_H;
    s_fill(&screen, 0, y, screen_w, TASKBAR_H, t->taskbar_bg);
    s_fill(&screen, 0, y, screen_w, 1, t->main);

    /* launcher button */
    u32 lbg = launch.active ? blend(t->taskbar_bg, t->main, 45) : blend(t->taskbar_bg, t->main, 12);
    s_fill(&screen, 8, y + 6, 34, TASKBAR_H - 12, lbg);
    s_frame_rect(&screen, 8, y + 6, 34, TASKBAR_H - 12, blend(t->taskbar_bg, t->main, 60));
    u32 lc = launch.active ? t->title_text : t->main;
    for (int q = 0; q < 4; q++)
        s_fill(&screen, 15 + (q % 2) * 11, y + 12 + (q / 2) * 9, 8, 6, lc);

    s_fill(&screen,48,y+6,110,TASKBAR_H-12,blend(t->taskbar_bg,t->main,20));
    s_frame_rect(&screen,48,y+6,110,TASKBAR_H-12,t->main);
    char label[24],count[12];strcpy(label,"Windows ");fmt_u32(count,win_count);strcat(label,count);
    s_clip_text(&screen,56,y+12,label,t->text,94);

    int visible=pin_count,cap=wm_taskbar_capacity();if(visible>cap)visible=cap;
    for(int i=0;i<visible;i++){int x=168+i*40;struct app *a=app_find(task_pins[i]);if(!a)continue;
        /* Dimmer when every window of that application is put away - the bar is the only place left
         * that can say so, and double-clicking the entry is how it gets undone. */
        s_fill(&screen,x,y+6,34,TASKBAR_H-12,blend(t->taskbar_bg,t->main,wm_app_minimized(task_pins[i])==1?7:20));s_frame_rect(&screen,x,y+6,34,TASKBAR_H-12,t->main);s_icon(&screen,a->icon,x+5,y+8,t->text);}
    /* power button */
    int px, py, pw, ph;
    tb_power_rect(&px, &py, &pw, &ph);
    power_hover = in_rect(mx, my, px, py, pw, ph) && !drag_win && !resize_win &&
                  desk_drag < 0 && !band_active && !menu.active && !modal_w && !dnd.active;
    s_fill(&screen, px, py, pw, ph, power_hover ? blend(t->taskbar_bg, t->main, 45) : blend(t->taskbar_bg, t->main, 12));
    s_frame_rect(&screen, px, py, pw, ph, blend(t->taskbar_bg, t->main, 60));
    u32 pc = power_hover ? t->title_text : t->main;
    s_circle(&screen, px + pw / 2, py + ph / 2 + 2, 7, pc);
    s_fill(&screen, px + pw / 2 - 1, py + ph / 2 - 8, 3, 9, power_hover ? t->title_text : t->taskbar_bg);
    s_line(&screen, px + pw / 2, py + ph / 2 - 8, px + pw / 2, py + ph / 2 - 1, pc);

    struct rtc_time rt;
    rtc_read(&rt);
    char clock[16];
    char a[4], b[4], c[4];
    fmt_pad2(a, rt.hour); fmt_pad2(b, rt.min); fmt_pad2(c, rt.sec);
    strcpy(clock, a); strcat(clock, ":"); strcat(clock, b); strcat(clock, ":"); strcat(clock, c);
    int tw = s_text_width(clock);
    s_text(&screen, screen_w - tw - 10, y + 12, clock, t->main);
}

/* classic arrow pointer: white fill with black outline, hotspot at tip */
static const float cur_poly[7][2] = {
    { 0, 0 }, { 0, 14 }, { 3.5f, 10.5f }, { 6, 16.5f },
    { 8.5f, 15.5f }, { 6, 9.5f }, { 10, 9.5f },
};
static void paint_cursor(void)
{
    const int n = 7;
    for (int y = 0; y < 17; y++) {
        float xs[8];
        int nx = 0;
        for (int i = 0; i < n; i++) {
            float y0 = cur_poly[i][1], y1 = cur_poly[(i + 1) % n][1];
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                float x0 = cur_poly[i][0], x1 = cur_poly[(i + 1) % n][0];
                xs[nx++] = x0 + (y + 0.5f - y0) * (x1 - x0) / (y1 - y0);
            }
        }
        for (int i = 0; i + 1 < nx; i++)
            for (int j = i + 1; j < nx; j++)
                if (xs[j] < xs[i]) { float t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
        for (int i = 0; i + 1 < nx; i += 2)
            for (int x = (int)xs[i]; x <= (int)xs[i + 1]; x++)
                s_pixel(&screen, mx + x, my + y, 0xFFFFFF);
    }
    for (int i = 0; i < n; i++) {
        int x0 = mx + (int)cur_poly[i][0], y0 = my + (int)cur_poly[i][1];
        int x1 = mx + (int)cur_poly[(i + 1) % n][0], y1 = my + (int)cur_poly[(i + 1) % n][1];
        s_line(&screen, x0, y0, x1, y1, 0x000000);
    }
}

static int launch_match(int idx)
{
    struct app *a = app_at(idx);
    if (!a || a->id[0] == '_') return 0;
    if (!launch.q[0]) return 1;
    return strstr(a->id, launch.q) || strstr(a->title, launch.q);
}

/* Hit testing must not depend on the previous rendered hover frame. */
static int launcher_at_point(void)
{
    int row = 0, skipped = 0;
    for (int i = 0; i < app_count() && row < 10; i++) {
        if (!launch_match(i)) continue;
        if (skipped++ < launch.offset) continue;
        if (in_rect(mx, my, 14, screen_h - TASKBAR_H - 255 + row * 24, 288, 22)) return i;
        row++;
    }
    return -1;
}

static void paint_launcher(void)
{
    const struct theme *t = theme_current();
    int px = 8, py = screen_h - TASKBAR_H - 308, pw = 300, ph = 300;
    s_fill(&screen, px, py, pw, ph, t->win_bg);
    s_frame_rect(&screen, px, py, pw, ph, t->main);
    s_text(&screen, px + 10, py + 8, "Apps: type to search / scroll", t->main);
    s_frame_rect(&screen, px + 10, py + 26, pw - 20, 20, t->main);
    s_text(&screen, px + 14, py + 30, launch.q, t->text);
    if ((tick_count / 50) % 2 == 0)
        s_fill(&screen, px + 14 + s_text_width(launch.q), py + 30, 2, 12, t->main);
    int y = py + 56;
    launch.hover = -1;
    int row = 0, skipped = 0;
    for (int i = 0; i < app_count(); i++) {
        if (!launch_match(i)) continue;
        if (skipped++ < launch.offset) continue;
        if (row >= 10) break;
        if (in_rect(mx, my, px + 6, y - 3, pw - 12, 22)) {
            launch.hover = i;
            s_fill(&screen, px + 6, y - 3, pw - 12, 22, blend(t->win_bg, t->main, 25));
        }
        s_icon(&screen, app_at(i)->icon, px + 12, y, t->main);
        s_clip_text(&screen, px + 44, y + 4, app_at(i)->title, t->text,pw-52);
        y += 24; row++;
    }
    if (!row) s_text(&screen, px + 14, y, "(no matching app)", ((t->main >> 1) & 0x7F7F7F));
}


static __attribute__((noinline)) void paint_all(void)
{
    paint_wallpaper();
    in_full = 1;
    paint_icons();
    in_full = 0;
    /* windows in z order (bottom first) */
    static u8 drawn[MAX_WINDOWS];
    for (int i = 0; i < win_count; i++) drawn[i] = 0;
    for (int pass = 0; pass < win_count; pass++) {
        struct window *lowest = NULL;
        for (int i = 0; i < win_count; i++) {
            if (drawn[i] || wins[i]->state == WIN_STATE_MIN) continue;
            if (!lowest || wins[i]->z < lowest->z) lowest = wins[i];
        }
        if (!lowest) break;
        for(int di=0;di<win_count;di++)if(wins[di]==lowest){drawn[di]=1;break;}
        if (lowest->dirty && lowest->app && lowest->app->paint) {
            { APP_T0(lowest); lowest->app->paint(lowest); APP_T1(lowest); }
            lowest->dirty = 0;
        }
        paint_window(lowest);
    }
    paint_taskbar();
    paint_menu();
    if (launch.active) paint_launcher();
    paint_notices();
    paint_dnd();                /* the full and the damaged path must agree, or a drag leaves a trail */
    tb_dirty = 0; icons_dirty = 0; menu_dirty = 0; launch_dirty = 0; dnd_dirty = 0; ndmg = 0;
    for (int i = 0; i < win_count; i++) wins[i]->chrome_dirty = 0;
    cur_have = 0;                 /* scene was fully redrawn under cursor */
    cur_draw();
    fb_flip();
}

/* ------------------------------------------------------- cursor overlay ----
 * Moving the mouse over plain desktop used to trigger a full-screen
 * composite per event. The cursor now lives in a tiny saved-background
 * overlay: erase old position, save+draw new one, flip. ~300 pixels of
 * memcpy instead of a megapixel repaint.
 */

static void cur_restore(void)
{
    if (!cur_have || !cur_buf) return;
    for (int r = 0; r < cur_bh; r++)
        memcpy(screen.px + (u32)(cur_by + r) * screen_w + cur_bx,
               cur_buf + (u32)r * CUR_W, (u32)cur_bw * 4);
    cur_have = 0;
}

static void cur_draw(void)
{
    cur_restore();
    int x = mx, y = my, w = CUR_W, h = CUR_H;
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;
    if (w <= 0 || h <= 0) return;
    if (!cur_buf) cur_buf = palloc_owned(CUR_W * CUR_H * 4,HEAP_WM);
    if (!cur_buf) { paint_cursor(); return; }
    for (int r = 0; r < h; r++)
        memcpy(cur_buf + (u32)r * CUR_W,
               screen.px + (u32)(y + r) * screen_w + x, (u32)w * 4);
    cur_bx = x; cur_by = y; cur_bw = w; cur_bh = h;
    paint_cursor();
    cur_have = 1;
}

/* pure cursor move: erase + redraw overlay only, flip the damaged union */
static void cur_move(void)
{
    int ox = cur_bx, oy = cur_by, ow = cur_bw, oh = cur_bh, have = cur_have;
    cur_restore();
    cur_draw();
    int x0 = mx, y0 = my, x1 = mx + CUR_W, y1 = my + CUR_H;
    if (have) {
        if (ox < x0) x0 = ox;
        if (oy < y0) y0 = oy;
        if (ox + ow > x1) x1 = ox + ow;
        if (oy + oh > y1) y1 = oy + oh;
    }
    fb_flip_rect(x0, y0, x1 - x0, y1 - y0);
}

/* does anything hoverable live under the cursor? */
static int move_needs_composite(struct window **wout)
{
    if (menu.active) return 1;
    if (launch.active &&
        in_rect(mx, my, 8, screen_h - TASKBAR_H - 308, 300, 308)) return 1;
    if (my >= screen_h - TASKBAR_H) return 1;
    struct window *w = win_at_point(mx, my);
    if(modal_w&&w!=modal_w)return 0;
    if (w) { *wout = w; return 1; }
    for (int i = 0; i < nitems; i++) {
        if (items[i].hidden) continue;
        int x, y, ww, hh;
        icon_rect(i, &x, &y, &ww, &hh);
        if (in_rect(mx, my, x, y, ww, hh)) return 1;
    }
    return 0;
}

/* one frame of a window drag / resize / rubber-band / icon drag: restore the
 * union region to clean wallpaper, mark everything overlapping it for the
 * damage compositor, and let paint_partial do a targeted repaint. Replaces
 * the old full-screen wm_full() per mouse-move (which burned ~80% CPU). */
static void interact_repaint(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;
    if (w <= 0 || h <= 0) return;
    damage_add(x, y, w, h);
    dirty = 1;
}

/* r36 RESIZE PERF ROOT FIX: every mouse report during a resize used to
 * run palloc + pfree + a full-surface memset + a full app repaint.  At
 * a USB mouse's 125-1000 Hz report rate that is megabytes of memset and
 * repaint per SECOND of dragging - the field report's "resizing windows
 * immediately makes CPU go to 100% and lags the system really bad".
 * Now handle_mouse only records the target geometry; resize_flush()
 * applies it at most once per ~20 ms (50 Hz) from the main loop, and
 * unconditionally (force) when the button releases. */
static int rs_nw, rs_nh, rs_pending;
static u32 rs_last;
static void resize_flush(int force)
{
    if (!resize_win || !rs_pending) return;
    if (!force && rs_last && (u32)tick_count - rs_last < 2) return;
    struct window *w = resize_win;
    int nw = rs_nw, nh = rs_nh;
    rs_pending = 0;
    rs_last = (u32)tick_count;
    if (nw == w->w && nh == w->h) return;
    int cw = nw - 2, ch = nh - WIN_TITLEBAR - 1;
    u32 *np = palloc_owned((size_t)cw * ch * 4,(uintptr_t)w);
    if (!np) return;                       /* OOM: keep the old geometry */
    int oldw=w->w,oldh=w->h;
    pfree(w->surf.px, (u32)w->surf.w * w->surf.h * 4);
    w->w = nw; w->h = nh;
    w->surf.px = np; w->surf.w = cw; w->surf.h = ch;
    memset(np, 0, (u32)cw * ch * 4);
    if (w->app && w->app->paint) { APP_T0(w); w->app->paint(w); APP_T1(w); }
    w->dirty = 0;
    interact_repaint(w->x,w->y,(oldw>nw?oldw:nw)+4,(oldh>nh?oldh:nh)+4);
}

/* Where a drag would end up if the button were released at (px,py), named in out: "app-window:<id>"
 * for the window under the pointer, "taskbar:<id>" for a pin, "desktop" for the wallpaper, "launcher"
 * or "menu" for the surfaces a drag dismisses, and "none".  Both wm_dnd_drop() and the tests go
 * through it, so a suite can assert what a drop *means* on a given geometry instead of trusting that
 * the pixel under the cursor happened to be the one intended. */
static int dnd_target_at(int px, int py, char *out, int cap)
{
    if (!out || cap < 8) return -1;
    out[0] = 0;
    if (menu.active) { strcpy(out, "menu"); return 0; }
    if (launch.active && in_rect(px, py, 8, screen_h - TASKBAR_H - 308, 300, 308)) {
        strcpy(out, "launcher"); return 0;
    }
    if (py >= screen_h - TASKBAR_H) {
        int visible = pin_count, c = wm_taskbar_capacity();
        if (visible > c) visible = c;
        for (int i = 0; i < visible; i++)
            if (in_rect(px, py, 168 + i * 40, screen_h - TASKBAR_H + 6, 34, TASKBAR_H - 12)) {
                strcat(strcpy(out, "taskbar:"), task_pins[i]);
                return 0;
            }
        strcpy(out, "none");
        return 0;
    }
    struct window *w = win_at_point(px, py);
    if (w && w->app) { strcat(strcpy(out, "app-window:"), w->app->id); return 0; }
    if (notice_at(px, py) >= 0) { strcpy(out, "none"); return 0; }
    strcpy(out, "desktop");
    return 0;
}

int wm_dnd_target(int px, int py, char *out, int cap)
{
    if (!dnd.active) return -1;
    return dnd_target_at(px, py, out, cap);
}

int wm_dnd_active(void) { return dnd.active; }

int wm_dnd_begin(int kind, const char *payload, const char *label)
{
    if (!payload || !payload[0] || kind < 0 || kind > DND_FILE) return -1;
    dnd.active = 1;
    dnd.kind = kind;
    strncpy(dnd.payload, payload, sizeof dnd.payload - 1);
    dnd.payload[sizeof dnd.payload - 1] = 0;
    strncpy(dnd.label, label && label[0] ? label : payload, sizeof dnd.label - 1);
    dnd.label[sizeof dnd.label - 1] = 0;
    dnd_dirty = 1;
    dirty = 1;
    return 0;
}

/* The label has to be damaged on its way out as well as on its way in, or the last thing a cancelled
 * drag said stays burnt into the wallpaper. */
static void dnd_damage_now(void)
{
    if (!dnd.active) return;
    if (dnd_lw > 0) damage_add(dnd_lx, dnd_ly, dnd_lw, dnd_lh);
    int x, y, w, h;
    dnd_rect(&x, &y, &w, &h);
    damage_add(x, y, w, h);
    dnd_lw = 0;                   /* released: nothing is painted until the next drag begins */
}

int wm_dnd_cancel(void)
{
    if (!dnd.active) return -1;
    dnd_damage_now();
    dnd.active = 0;
    dnd_dirty = 1;
    dirty = 1;
    return 0;
}

/* A drop either does something or it does nothing, and it never half-does it: the payload is copied out
 * and the drag state is cleared before any target runs, so an app that opens a window (or a Files
 * navigation, which repaints) cannot see a drag still in progress and react to its own window. */
int wm_dnd_drop(int px, int py)
{
    if (!dnd.active) return -1;
    int kind = dnd.kind;
    char payload[sizeof dnd.payload];
    memcpy(payload, dnd.payload, sizeof payload);
    char target[48];
    /* The target is resolved while the drag still exists - the shadow has to be erased and the state
     * cleared before any target runs, because the target may open a window and repaint. */
    int known = dnd_target_at(px, py, target, sizeof target) == 0;
    dnd_damage_now();
    dnd.active = 0;
    dnd_dirty = 1;
    dirty = 1;
    if (!known || !strcmp(target, "none") || !strcmp(target, "menu") || !strcmp(target, "launcher"))
        return -1;
    if (!strcmp(target, "desktop")) {
        if (kind == DND_APP) wm_desktop_install(payload);
        else wm_desktop_pin_file(payload);
        return 0;
    }
    if (!strncmp(target, "taskbar:", 8)) {
        const char *id = target + 8;
        if (kind == DND_APP) {
            if (!wm_taskbar_pinned(id) && !wm_taskbar_pin(id))
                wm_notify("Taskbar full", "Unpin something first - the number of pins follows the "
                          "width of the screen.", 1);
            return 0;
        }
        struct app *a = app_find(id);
        if (!a) return -1;
        if (!a->document && !a->open) {
            wm_notify("Cannot open that here", a->title, 0);
            return -1;
        }
        return wm_open_app(id, payload) ? 0 : -1;
    }
    if (!strncmp(target, "app-window:", 11)) {
        const char *id = target + 11;
        if (kind == DND_APP) {
            struct window *w = win_at_point(px, py);
            if (w) wm_focus(w);
            return 0;
        }
        struct app *a = app_find(id);
        if (!a) return -1;
        /* Files is the one app whose content *is* a folder, so a folder dropped on it navigates rather
         * than trying to open the directory as a document. */
        if (!strcmp(id, "files")) {
            struct vfs_node *n = vfs_lookup(payload);
            if (n && n->is_dir) return wm_open_app("files", (void *)payload) ? 0 : -1;
        }
        /* A drop on a window normally means "open it with this application", but the application may
         * have a better answer for the exact place the pointer is over: dropping a file on a folder
         * row in File Explorer moves the file into that folder.  Declining leaves the default. */
        struct window *w = win_at_point(px, py);
        if (w && w->app == a && a->drop) {
            int took;
            APP_T0(w);
            took = a->drop(w, payload, px - (w->x + 1), py - (w->y + WIN_TITLEBAR));
            APP_T1(w);
            if (took) return 0;
        }
        return wm_open_app(id, payload) ? 0 : -1;
    }
    return -1;
}

static void dnd_rect(int *x, int *y, int *w, int *h)
{
    *w = (int)strlen(dnd.label) * 8 + 20;
    if (*w > 240) *w = 240;
    *h = 24;
    *x = mx + 12;
    *y = my + 16;
    if (*x + *w > screen_w - 2) *x = mx - *w - 8;
    if (*y + *h > screen_h - 2) *y = my - *h - 8;
}

static void paint_dnd(void)
{
    if (!dnd.active) return;
    int x, y, w, h;
    const struct theme *t = theme_current();
    dnd_rect(&x, &y, &w, &h);
    dnd_lx = x; dnd_ly = y; dnd_lw = w; dnd_lh = h;
    s_fill(&screen, x, y, w, h, color_blend(t->win_bg, t->main, 40));
    s_frame_rect(&screen, x, y, w, h, t->main);
    s_clip_text(&screen, x + 10, y + 5, dnd.label, t->text, w - 20);
}

/* -------------------------------------------------------------- input ---- */
static void desktop_context(void)
{
        int hit = -1;
        for (int i = 0; i < nitems; i++) {
            if (items[i].hidden) continue;
            int x, y, ww, hh;
            icon_rect(i, &x, &y, &ww, &hh);
            if (in_rect(mx, my, x, y, ww, hh)) { hit = i; break; }
        }
        if (hit >= 0) {
            if (!(desk_sel & (1u << hit))) desk_sel = (1u << hit);
            static const char *m[6];
            int n = wm_desk_actions(hit, m, 6);
            wm_menu(mx, my, m, n > 0 ? n : 1, desk_item_menu_cb, (void *)(intptr_t)hit);
        } else if (desk_sel) {
            static const char *m[2] = { "Remove from Desktop", "Clear Selection" };
            wm_menu(mx, my, m, 2, desk_sel_menu_cb, NULL);
        } else {
            /* Empty wallpaper is a surface too, and it offers what belongs here: the icons that were put
             * away, and the two places the rest of the work actually happens. */
            static const char *m[3] = { "Restore hidden icons", "Open Files", "Open Terminal" };
            wm_menu(mx, my, m, 3, desk_empty_menu_cb, NULL);
        }
    wm_full();
}

static __attribute__((noinline)) void handle_mouse(struct mouse_event *e)
{
    if (e->type == MEV_MOVE) {
        /* pre-move geometry for the interaction damage union */
        int omx = mx, omy = my;
        int odrag = 0, odx = 0, ody = 0, odw = 0, odh = 0;
        if (drag_win) {
            odrag = 1;
            odx = drag_win->x; ody = drag_win->y;
            odw = drag_win->w; odh = drag_win->h;
        }
        int ores = 0, orx = 0, ory = 0, orw = 0, orh = 0;
        if (resize_win) {
            ores = 1;
            orx = resize_win->x; ory = resize_win->y;
            orw = resize_win->w; orh = resize_win->h;
        }
        int obx1 = band_x1, oby1 = band_y1;
        int sens = prefs_get()->mouse_sens;
        if (sens != mouse_sens) {
            mouse_rx = mouse_ry = 0;
            mouse_sens = sens;
        }
        /* Carry subpixel movement across reports, including negative
         * deltas. Low sensitivity must not discard slow mouse motion. */
        int dx = e->dx * sens + mouse_rx;
        int dy = -e->dy * sens + mouse_ry; /* PS/2: +dy is up */
        mx += dx / 3; my += dy / 3;
        mouse_rx = dx % 3; mouse_ry = dy % 3;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx > screen_w - 1) mx = screen_w - 1;
        if (my > screen_h - 1) my = screen_h - 1;
        if ((mx == 0 && mouse_rx < 0) || (mx == screen_w - 1 && mouse_rx > 0)) mouse_rx = 0;
        if ((my == 0 && mouse_ry < 0) || (my == screen_h - 1 && mouse_ry > 0)) mouse_ry = 0;
        if (drag_win) {
            drag_win->x = mx - drag_ox;
            drag_win->y = my - drag_oy;
            if (drag_win->y < 0) drag_win->y = 0;
            if (drag_win->y > screen_h - TASKBAR_H - WIN_TITLEBAR)
                drag_win->y = screen_h - TASKBAR_H - WIN_TITLEBAR;
        }
        if (band_active) {
            band_x1 = mx; band_y1 = my;
        }
        if (desk_drag >= 0 && !desk_drag_moved &&
            (mx - desk_sx > 6 || mx - desk_sx < -6 || my - desk_sy > 6 || my - desk_sy < -6))
            desk_drag_moved = 1;
        if (dnd.active) dnd_dirty = 1;
        if (launch_press >= 0 && launch.active && !dnd.active && (mx - launch_press_x > 6 ||
            mx - launch_press_x < -6 || my - launch_press_y > 6 || my - launch_press_y < -6)) {
            /* Six pixels of movement turns the press into a pick-up.  The launcher closes because the
             * drop target has to be visible for the drag to mean anything. */
            struct app *a = app_at(launch_press);
            launch_press = -1;
            launch.active = 0;
            if (a) wm_dnd_begin(DND_APP, a->id, a->title);
            wm_full();
            return;
        }
        if (resize_win) {
            struct window *w = resize_win;
            int nw = mx - w->x + resize_ox, nh = my - w->y + resize_oy;
            int minw = 320, minh = 200;
            if (w->app) {
                if (w->app->min_w > minw) minw = w->app->min_w;
                if (w->app->min_h > minh) minh = w->app->min_h;
            }
            if (nw < minw) nw = minw;
            if (nh < minh) nh = minh;
            if (nw > screen_w) nw = screen_w;
            if (nh > screen_h - TASKBAR_H) nh = screen_h - TASKBAR_H;
            if (nw != w->w || nh != w->h) {
                /* r36: record the target only - resize_flush() applies
                 * it once per frame, throttled to ~50 Hz (see above) */
                rs_nw = nw; rs_nh = nh; rs_pending = 1;
            }
        }
        if (drag_win || band_active || resize_win || desk_drag >= 0 || dnd.active) {
            int x0 = omx < mx ? omx : mx, y0 = omy < my ? omy : my;
            int x1 = (omx > mx ? omx : mx) + CUR_W;
            int y1 = (omy > my ? omy : my) + CUR_H;
#define UN(ux, uy, uw, uh) do { \
                int a0_ = (ux), b0_ = (uy), a1_ = (ux) + (uw), b1_ = (uy) + (uh); \
                if (a0_ < x0) { x0 = a0_; } \
                if (b0_ < y0) { y0 = b0_; } \
                if (a1_ > x1) { x1 = a1_; } \
                if (b1_ > y1) { y1 = b1_; } \
            } while (0)
            if (odrag) UN(odx, ody, odw + 4, odh + 4);
            if (drag_win) UN(drag_win->x, drag_win->y, drag_win->w + 4, drag_win->h + 4);
            if (ores) UN(orx, ory, orw + 4, orh + 4);
            if (resize_win) UN(resize_win->x, resize_win->y,
                               resize_win->w + 4, resize_win->h + 4);
            if (band_active) {
                int bx = band_x0 < band_x1 ? band_x0 : band_x1;
                int by = band_y0 < band_y1 ? band_y0 : band_y1;
                int bw = (band_x0 < band_x1 ? band_x1 - band_x0 : band_x0 - band_x1) + 2;
                int bh = (band_y0 < band_y1 ? band_y1 - band_y0 : band_y0 - band_y1) + 2;
                UN(bx, by, bw, bh);
                int ox = band_x0 < obx1 ? band_x0 : obx1;
                int oy = band_y0 < oby1 ? band_y0 : oby1;
                int ow = (band_x0 < obx1 ? obx1 - band_x0 : band_x0 - obx1) + 2;
                int oh = (band_y0 < oby1 ? oby1 - band_y0 : band_y0 - oby1) + 2;
                UN(ox, oy, ow, oh);
            }
            if (dnd.active) {
                if (dnd_lw > 0) UN(dnd_lx, dnd_ly, dnd_lw, dnd_lh);
                int dx, dy, dw, dh;
                dnd_rect(&dx, &dy, &dw, &dh);
                UN(dx, dy, dw, dh);
            }
            if (desk_drag >= 0) {
                int ix, iy, iw, ih;
                icon_rect(desk_drag, &ix, &iy, &iw, &ih);
                UN(ix, iy, iw, ih);
                if (desk_drag_moved) {
                    UN(omx - desk_drag_ox, omy - desk_drag_oy, iw + 2, ih + 2);
                    UN(mx - desk_drag_ox, my - desk_drag_oy, iw + 2, ih + 2);
                }
            }
#undef UN
            interact_repaint(x0, y0, x1 - x0, y1 - y0);
            return;
        }
        if(app_capture&&(!modal_w||modal_w==app_capture)){
            struct window *c=app_capture;if(c->app&&c->app->mouse){APP_T0(c);c->app->mouse(c,e,mx-c->x-1,my-c->y-WIN_TITLEBAR);APP_T1(c);}cur_move();return;
        }
        if(notice_at(mx,my)>=0){cur_move();return;}
        struct window *hw = NULL;
        int zone = move_needs_composite(&hw);
        /* hover signature: composite only when the hover TARGET changes,
         * not on every pixel of motion; apps get the move event and repaint
         * their own surface only when their internal hover state flips */
        static struct window *last_hw;
        static int last_zone, last_icon = -2, last_tb = -2;
        static int last_mrow = -2, last_lrow = -2;
        int icon = -1, tbz = -1, mrow = -1, lrow = -1;
        if (menu.active) {
            for (int i = 0; i < menu.n; i++)
                if (in_rect(mx, my, menu.x, menu.y + 3 + i * 24, menu.w, 24)) mrow = i;
        } else if (launch.active &&
                   in_rect(mx, my, 8, screen_h - TASKBAR_H - 308, 300, 308)) {
            lrow = launcher_at_point();
        } else if (my >= screen_h - TASKBAR_H) {
            int x, y, w, h;
            tb_power_rect(&x, &y, &w, &h);
            tbz = in_rect(mx, my, x, y, w, h);
        } else if (!hw) {
            for (int i = 0; i < nitems; i++) {
                if (items[i].hidden) continue;
                int x, y, ww, hh;
                icon_rect(i, &x, &y, &ww, &hh);
                if (in_rect(mx, my, x, y, ww, hh)) { icon = i; break; }
            }
        }
        if (hw != last_hw || zone != last_zone || icon != last_icon ||
            tbz != last_tb || mrow != last_mrow || lrow != last_lrow) {
            if (icon != last_icon) icons_dirty = 1;
            if (tbz != last_tb) tb_dirty = 1;
            if (mrow != last_mrow) menu_dirty = 1;
            if (lrow != last_lrow) launch_dirty = 1;
            last_hw = hw; last_zone = zone; last_icon = icon; last_tb = tbz;
            last_mrow = mrow; last_lrow = lrow;
            dirty = 1;
        }
        if (hw && (!modal_w||modal_w==hw) && hw->app && hw->app->mouse)
            { APP_T0(hw); hw->app->mouse(hw, e, mx - (hw->x + 1), my - (hw->y + WIN_TITLEBAR)); APP_T1(hw); }
        cur_move();                  /* cheap overlay-only cursor move */
        (void)zone;
        return;
    }
    if(e->type==MEV_WHEEL&&notice_at(mx,my)>=0)return;
    if (e->type == MEV_WHEEL && launch.active &&
        in_rect(mx,my,8,screen_h-TASKBAR_H-308,300,308)) {
        int count=0;
        for(int i=0;i<app_count();i++)if(launch_match(i))count++;
        launch.offset += e->wheel>0 ? -1 : 1;
        if(launch.offset>count-10)launch.offset=count>10?count-10:0;
        if(launch.offset<0)launch.offset=0;
        launch_dirty=1;dirty=1;return;
    }
    if(e->type==MEV_WHEEL&&my>=screen_h-TASKBAR_H)return;
    if (e->type == MEV_WHEEL) {
        struct window *w = win_at_point(mx, my);
        if (w && (!modal_w||modal_w==w) && w->app && w->app->mouse) {
            { APP_T0(w); w->app->mouse(w, e, mx - (w->x + 1), my - (w->y + WIN_TITLEBAR)); APP_T1(w); }
            w->dirty = 1;            /* app scrolled: repaint its content */
        }
        dirty = 1;
        return;
    }
    /* buttons */
    if(e->down&&!app_capture&&!drag_win&&!resize_win){int notice=notice_at(mx,my);if(notice>=0){if(e->button==MBTN_LEFT)notice_remove(notice);return;}}
    mbuttons = e->buttons;
    if (e->down) {
        static u64 last_down_tick;
        static int last_down_x, last_down_y, last_down_btn;
        int dbl = (e->button == last_down_btn &&
                   (tick_count - last_down_tick) <= (u64)prefs_get()->dbl_ms / 10 &&
                   mx - last_down_x <= 6 && mx - last_down_x >= -6 &&
                   my - last_down_y <= 6 && my - last_down_y >= -6);
        last_down_tick = tick_count;
        last_down_x = mx; last_down_y = my; last_down_btn = e->button;
        if (dbl && !menu.active && !launch.active && !modal_w && e->button == MBTN_LEFT && my < screen_h - TASKBAR_H &&
            !win_at_point(mx, my)) {
            for (int i = 0; i < nitems; i++) {
                if (items[i].hidden) continue;
                int x, y, ww, hh;
                icon_rect(i, &x, &y, &ww, &hh);
                if (in_rect(mx, my, x, y, ww, hh)) {
                    desktop_open(&items[i]);
                    dirty = 1;
                    return;
                }
            }
        }
    }
    if (!e->down) {
        if (dnd.active) { wm_dnd_drop(mx, my); wm_full(); return; }
        if (launch_press >= 0) {
            /* A release that is still on the row the press started on is the click, and it launches.
             * Anywhere else - another row, the search field, outside the panel - closes the launcher the
             * same way any menu would.  A drag from the same press was already consumed above, and it
             * never reaches this branch. */
            int press = launch_press;
            launch_press = -1;
            if (launch.active && press == launcher_at_point()) {
                struct app *a = app_at(press);
                launch.active = 0;
                if (a) wm_open_app(a->id, NULL);
            } else if (launch.active) {
                launch.active = 0;
                launch.hover = -1;
            }
            wm_full();
            return;
        }
        if(app_capture){struct window *c=app_capture;if(!e->buttons)app_capture=NULL;if(c->app&&c->app->mouse){APP_T0(c);c->app->mouse(c,e,mx-c->x-1,my-c->y-WIN_TITLEBAR);APP_T1(c);}dirty=1;return;}
        if(band_active&&e->button!=band_button)return;
        if(band_active&&band_button==MBTN_RIGHT&&mx-band_x0<=4&&mx-band_x0>=-4&&my-band_y0<=4&&my-band_y0>=-4){band_active=0;desktop_context();return;}
        if (desk_drag >= 0) {
            if (desk_drag_moved) {
                int gx = (mx - desk_drag_ox + 40 - 16) / 88;
                int gy = (my - desk_drag_oy + 44 - 16) / 96;
                if (gx < 0) gx = 0;
                if (gx > DESK_COLS - 1) gx = DESK_COLS - 1;
                if (gy < 0) gy = 0;
                if (gy > DESK_ROWS - 1) gy = DESK_ROWS - 1;
                if (cell_taken(gx, gy, desk_drag)) {       /* nearest free cell */
                    for (int r = 1; r < 8 && cell_taken(gx, gy, desk_drag); r++)
                        for (int dy = -r; dy <= r && cell_taken(gx, gy, desk_drag); dy++)
                            for (int dx = -r; dx <= r && cell_taken(gx, gy, desk_drag); dx++) {
                                int nx = gx + dx, ny = gy + dy;
                                if (nx >= 0 && nx < DESK_COLS && ny >= 0 && ny < DESK_ROWS &&
                                    !cell_taken(nx, ny, desk_drag)) { gx = nx; gy = ny; }
                            }
                }
                items[desk_drag].gx = gx; items[desk_drag].gy = gy;
                desktop_save();
            }
            desk_drag = -1;
        }
        if (band_active) {
            band_active = 0;
            if ((band_x1 - band_x0 > 4 || band_x1 - band_x0 < -4) &&
                (band_y1 - band_y0 > 4 || band_y1 - band_y0 < -4)) {
                int x = band_x0 < band_x1 ? band_x0 : band_x1;
                int y = band_y0 < band_y1 ? band_y0 : band_y1;
                int w = band_x0 < band_x1 ? band_x1 - band_x0 : band_x0 - band_x1;
                int h = band_y0 < band_y1 ? band_y1 - band_y0 : band_y0 - band_y1;
                desk_sel = 0;
                for (int i = 0; i < nitems; i++) {
                    if (items[i].hidden) continue;
                    int ix, iy, iw, ih;
                    icon_rect(i, &ix, &iy, &iw, &ih);
                    if (ix < x + w && ix + iw > x && iy < y + h && iy + ih > y)
                        desk_sel |= (1u << i);
                }
            }
        }
        drag_win = NULL;
        resize_flush(1);          /* r36: apply the final resize target */
        resize_win = NULL;
        wm_full();
        return;
    }

    if (menu.active) {
        if (in_rect(mx, my, menu.x, menu.y, menu.w, menu.h)) {
            int item = my < menu.y + 3 ? -1 : (my - menu.y - 3) / 24;
            menu_cb cb = menu.cb;
            void *ud = menu.ud;
            menu.active = 0;
            if (cb) cb(item>=0&&item<menu.n?item:-1,ud);
        } else {
            menu_cancel();
        }
        wm_full();
        return;
    }

    if (launch.active) {
        int px = 8, py = screen_h - TASKBAR_H - 308, pw = 300, ph = 300;
        if (in_rect(mx, my, px, py, pw, ph)) {
            int hit = launcher_at_point();
            /* Pressing an entry in the launcher and moving is how an application gets onto the desktop
             * or the bar; releasing without moving is the normal "launch it" click, so the two are
             * separated by the same six pixels the icon grid uses. */
            if (hit >= 0 && e->button == MBTN_LEFT) {
                launch_press = hit;
                launch_press_x = mx;
                launch_press_y = my;
            }
            wm_full();
            return;
        }
        if (!in_rect(mx, my, 8, screen_h - TASKBAR_H + 6, 34, 28)) {
            launch.active = 0;
            wm_full();
            return;
        }
    }

    struct window *w = win_at_point(mx, my);
    if(modal_w&&w!=modal_w)return;
    if (w) {
        wm_focus(w);
        int rx = mx - w->x, ry = my - w->y;
        /* title buttons */
        if (ry < WIN_TITLEBAR && e->button==MBTN_LEFT) {
            for (int b = 0; b < 3; b++) {
                int bx = w->w - 22 - (2 - b) * 22;
                if (in_rect(rx, ry, bx, 3, BTN_SZ, BTN_SZ)) {
                    if (b == 0) { wm_minimize_window(w); }
                    else if (b == 1) {
                        if (w->state == WIN_STATE_MAX) {
                            win_free_buf(w);
                            w->x = w->px; w->y = w->py; w->w = w->pw; w->h = w->ph;
                            w->state = WIN_STATE_NORMAL;
                            win_alloc_buf(w);
                            w->dirty = 1;
                        } else {
                            w->px = w->x; w->py = w->y; w->pw = w->w; w->ph = w->h;
                            win_free_buf(w);
                            w->x = 0; w->y = 0;
                            w->w = screen_w; w->h = screen_h - TASKBAR_H;
                            w->state = WIN_STATE_MAX; wm_full();
                            w->dirty = 1;
                            win_alloc_buf(w);
                        }
                    } else {
                        wm_close_window(w);
                    }
                    wm_full();
                    return;
                }
            }
            if(w->state==WIN_STATE_MAX)return;
            drag_win = w;
            drag_ox = rx; drag_oy = ry;
            wm_full();
            return;
        }
        /* resize handle */
        if (e->button==MBTN_LEFT && in_rect(rx, ry, w->w - 16, w->h - 16, 16, 16) && w->state != WIN_STATE_MAX) {
            resize_win = w;
            resize_ox = w->w - rx; resize_oy = w->h - ry;
            wm_full();
            return;
        }
        app_capture=w;
        if (w->app && w->app->mouse)
            { APP_T0(w); w->app->mouse(w, e, mx - (w->x + 1), my - (w->y + WIN_TITLEBAR)); APP_T1(w); }
        dirty = 1;
        return;
    }

    /* desktop items: left = select/drag/open, right = context menu
     * (taskbar area is handled further below) */
    if (e->button == MBTN_LEFT && my < screen_h - TASKBAR_H) {
        for (int i = 0; i < nitems; i++) {
            if (items[i].hidden) continue;
            int x, y, ww, hh;
            icon_rect(i, &x, &y, &ww, &hh);
            if (in_rect(mx, my, x, y, ww, hh)) {
                if (!(desk_sel & (1u << i))) desk_sel = (1u << i);
                desk_drag = i; desk_drag_moved = 0;
                desk_drag_ox = mx - x; desk_drag_oy = my - y;
                desk_sx = mx; desk_sy = my;
                wm_full();
                return;
            }
        }
        band_active = 1;band_button=MBTN_LEFT;
        band_x0 = band_x1 = mx; band_y0 = band_y1 = my;
        desk_sel = 0;
        dirty = 1;
        return;
    }
    if(e->button==MBTN_RIGHT&&my<screen_h-TASKBAR_H){
        band_active=1;band_button=MBTN_RIGHT;band_x0=band_x1=mx;band_y0=band_y1=my;dirty=1;return;
    }
    /* taskbar */
    if (my >= screen_h - TASKBAR_H) {
        if (in_rect(mx, my, 8, screen_h - TASKBAR_H + 6, 34, TASKBAR_H - 12)) {
            lua_apps_refresh();
            launch.offset = 0;
            launch.active = !launch.active;
            launch_press = -1;
            wm_full();
            launch.q[0] = 0; launch.qpos = 0;
            dirty = 1;
            return;
        }
        int visible=pin_count,cap=wm_taskbar_capacity();if(visible>cap)visible=cap;
        for(int i=0;i<visible;i++)if(in_rect(mx,my,168+i*40,screen_h-TASKBAR_H+6,34,TASKBAR_H-12)){
            if(e->button==MBTN_RIGHT){
                /* One more line, only when it would do something: an app whose desktop icon was put away
                 * can have it back from the bar it is already pinned to. */
                static const char *choices[3];
                int n = 0;
                choices[n++] = "Open";
                choices[n++] = "Unpin from bar";
                if (wm_desk_app_state(task_pins[i]) == 1) choices[n++] = "Put its icon back";
                strcpy(pin_menu_id, task_pins[i]);
                wm_menu(168 + i * 40, screen_h - TASKBAR_H - (n * 24 + 8), choices, n, pin_menu_answer, NULL);
            }
            else if(e->button==MBTN_LEFT&&e->down){
                /* The bar's own double-click: put that application's window away, or bring it back.
                 * The first press of the pair is an ordinary click, which raises or launches the
                 * window, so the gesture always ends where the user means it to.  The timing is
                 * deliberately not shared with the icon grid's: a press on the bar must not be taken
                 * for the second half of a double-click on a desktop icon, or the other way round. */
                static u64 bar_last_tick;
                static int bar_last_i = -1, bar_last_x, bar_last_y;
                int pair = bar_last_i == i &&
                           (tick_count - bar_last_tick) <= (u64)prefs_get()->dbl_ms / 10 &&
                           mx - bar_last_x <= 6 && mx - bar_last_x >= -6 &&
                           my - bar_last_y <= 6 && my - bar_last_y >= -6;
                bar_last_tick = tick_count; bar_last_x = mx; bar_last_y = my; bar_last_i = i;
                struct window *t = 0;
                for (int k = 0; k < win_count; k++)
                    if (wins[k]->app && !strcmp(wins[k]->app->id, task_pins[i])) { t = wins[k]; break; }
                if (pair && t) {
                    if (t->state != WIN_STATE_MIN) wm_minimize_window(t);
                    else {
                        t->state = t->restore_state == WIN_STATE_MAX ? WIN_STATE_MAX : WIN_STATE_NORMAL;
                        wm_focus(t);
                    }
                    bar_last_i = -1;      /* the pair is spent, so a third click starts a new one */
                } else {
                    wm_open_app(task_pins[i], NULL);
                }
                wm_full();return;
            }
            else if(e->button==MBTN_LEFT){wm_full();return;}
        }
        int px, py, pw, ph;
        tb_power_rect(&px, &py, &pw, &ph);
        if (in_rect(mx, my, px, py, pw, ph)) {
            static const char *items[2] = { "Restart", "Power Off" };
            wm_menu(px - 140, py - 2 * 24 - 8, items, 2, power_menu_cb, NULL);
            dirty = 1;
            return;
        }
        if(in_rect(mx,my,48,screen_h-TASKBAR_H+6,110,TASKBAR_H-12)){picker_open();wm_full();return;}
    }
    dirty = 1;
}

static void handle_key(struct key_event *e)
{
    /* Ctrl+Alt+Del: the escape hatch firmware used to provide */
    if (e->pressed && e->ctrl && e->alt &&
        (e->keycode == KEY_DELETE || e->keycode == 0x7F)) {
        klog("wm: ctrl+alt+del - exiting scoswm (Linux port)");
        platform_request_quit();
    }
    /* r37: Ctrl+Alt+F1 - the Linux-style console switch.  Hands screen
     * and keyboard to the kernel maintenance tty (Linux's VT1); from
     * there Ctrl+Alt+F7 or the 'wm' command returns to the desktop.
     * Gives the user a rescue console even when no terminal app is
     * open or the WM's app layer is wedged. */
    if (e->pressed && e->ctrl && e->alt && e->keycode >= KEY_F1 && e->keycode <= KEY_F6) {
        tty_request = e->keycode - KEY_F1 + 1;
        return;
    }
    if (menu.active && e->pressed && e->keycode == 27) {
        menu_cancel();
        wm_full();
        return;
    }
    if (launch.active) {
        if (e->pressed && e->keycode == 27) { launch.active = 0; wm_full(); return; }
        if (e->pressed && e->keycode == '\n') {
            int matches = 0;
            for (int i = 0; i < app_count(); i++)
                if (launch_match(i) && matches++ >= launch.offset) {
                    wm_open_app(app_at(i)->id, NULL); break;
                }
            launch.active = 0;
            wm_full();
            return;
        }
        edit_line(launch.q, &launch.qpos, sizeof(launch.q), e);
        launch.offset = 0;
        launch_dirty = 1;
        dirty = 1;
        return;
    }
    /* Keyboard parity with the mouse on the icon grid, and only while the desktop itself has the
     * keyboard - a focused window keeps its own Enter.  Enter opens everything selected, Delete
     * (below) hides it, F2 renames a shortcut. */
    if (e->pressed && desk_sel && !focused_w && !modal_w && !menu.active && !launch.active) {
        if (e->keycode == '\r' || e->keycode == '\n') {
            u32 sel = desk_sel;
            for (int i = 0; i < nitems; i++)
                if ((sel & (1u << i)) && !items[i].hidden) desktop_open(&items[i]);
            dirty = 1;
            return;
        }
        if (e->keycode == KEY_F2) {
            for (int i = 0; i < nitems; i++)
                if ((desk_sel & (1u << i)) && !items[i].hidden) { desk_rename_ask(i); break; }
            dirty = 1;
            return;
        }
    }
    if (e->pressed && e->keycode == KEY_DELETE && desk_sel && !focused_w && !modal_w) {
        desk_remove_sel();
        dirty = 1;
        return;
    }
    struct window *w = modal_w ? modal_w : focused_w;
    if (w && w->app && w->app->key) { APP_T0(w); w->app->key(w, e); APP_T1(w); }
    dirty = 1;
}

/* --------------------------------------------------------------- tick ---- */
static void wm_tick(void)
{
    for(int i=notice_count-1;i>=0;i--)if(notices[i].until&&(i32)((u32)tick_count-notices[i].until)>=0)notice_remove(i);
    for (int i = 0; i < win_count; i++)
        if (wins[i]->app && wins[i]->app->tick && wins[i]->state != WIN_STATE_MIN)
            { APP_T0((wins[i])); wins[i]->app->tick(wins[i]); APP_T1((wins[i])); }

    /* r39: fold each window's accumulated TSC cycles into a percent
     * once per second (budget = cpu_mhz() * 1e6 ticks, the same PIT-
     * calibrated TSC rate SysMon reports). */
    if (cyc_ok) {
        static u64 previous;
        u64 now=rdtsc();
        if(now-previous>=cpu_tsc_hz){
            u64 budget=now-previous;previous=now;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                app_pct[i] = budget ? (u32)(app_cyc[i] * 100 / budget) : 0;
                if (app_pct[i] > 100) app_pct[i] = 100;
                app_cyc[i] = 0;
            }
        }
    }

    /* clock / blink phase: repaint only the rects that actually show them */
    u64 phase = tick_count / 50;
    if (phase != last_blink_phase) {
        last_blink_phase = phase;
        for (int i = 0; i < win_count; i++)
            if (wins[i]->state != WIN_STATE_MIN) wins[i]->chrome_dirty = 1;
        dirty = 1;
    }
    {
        static u32 last_sec_tick;
        if (tick_count / 100 != last_sec_tick) {
            last_sec_tick = (u32)(tick_count / 100);
            tb_dirty = 1;
            dirty = 1;
        }
    }
    if (launch.active && (tick_count % 50) == 0) {
        launch_dirty = 1;
        dirty = 1;
    }

    /* easter egg, mirrors the web simulation's file.scv behaviour */
    if (tick_count > 500) {
        if (!egg_armed) { egg_armed = 1; egg_last_spawn = tick_count; }
        if ((tick_count % 50) == 0 && vfs_lookup("home/documents/file.scv")) {
            if (tick_count - egg_last_spawn >= 30 && win_count < MAX_WINDOWS - 2) {
                egg_last_spawn = tick_count;
                wm_error_popup("A critical error has occurred!");
            }
        }
    }
}

/* ----------------------------------------------------------- main loop --- */
/* If the timer interrupt ever stops arriving while wall-clock time keeps
 * advancing (lost EOI / stuck PIC on some real hardware), re-arm the PIC and
 * PIT so the system recovers instead of hanging. */
static void irq_watchdog(void)
{
    static u32 last_sec, last_tick, last_wd;
    if ((u32)tick_count - last_wd < 100) return;   /* 1 Hz, tick-based */
    last_wd = (u32)tick_count;
    struct rtc_time rt;
    rtc_read(&rt);
    if (last_sec && rt.sec != last_sec && (u32)tick_count == last_tick) {
        /* Bare metal re-armed the PIC here; on Linux a stalled tick means
         * the host event loop stalled, and there is no PIC to re-arm. */
        klog("wm: tick watchdog noticed stalled ticks");
        err_notify("interrupt controller",
                   "IRQ watchdog re-armed the PIC (ticks had stopped)", 0, 0);
    }
    last_sec = rt.sec;
    last_tick = (u32)tick_count;
}

static u32 last_paint_tick;
volatile int wm_in_idle;

void wm_theme_changed(void)
{
    wp_valid = 0;
    for(int i=0;i<win_count;i++)wins[i]->dirty=1;
    wm_full();
}

/* Called only after dispatch has unwound, never from inside an app
 * callback. Terminating the compositor also destroys its dependent apps. */
static void wm_destroy_session(void)
{
    menu_cancel();
    u64 total, before, after; mm_stats(&total,&before);
    pending_console = NULL;
    for (int i=0;i<win_count;i++) wins[i]->console=NULL;
    while (win_count) wm_close_window(wins[win_count-1]);
    focused_w=modal_w=drag_win=resize_win=NULL;
    mouse_rx=mouse_ry=mouse_sens=0;
    rs_pending=0; band_active=0; desk_drag=-1; desk_sel=0;
    menu.active=0; launch.active=0; launch_press=-1; ndmg=0; mbuttons=0;app_capture=NULL;
    if(dnd.active){dnd.active=0;dnd_dirty=0;}
    if (wp_cache.px) {
        pfree(wp_cache.px,(u32)wp_cache.w*wp_cache.h*4);
        memset(&wp_cache,0,sizeof(wp_cache));
    }
    wp_valid=0;
    if (cur_buf) pfree(cur_buf,CUR_W*CUR_H*4);
    cur_buf=NULL; cur_have=0;
    cyc_reset_all();
    mm_stats(&total,&after);
    klog("wm: terminated; application windows and compositor caches released (%u KB reclaimed)",
         (u32)(after >= before ? after-before : 0));
}

int wm_stop_requested;

void wm_run(void)
{
    wm_stop_requested = 0;
    wm_full(); /* Restart from the base console must replace its entire scene. */
    input_guard_armed = 1;
    cyc_ok = cpu_mhz() != 0;   /* r39: per-app CPU metering needs TSC */

    klog("wm: entering main loop");
    for (;;) {
        if (wm_stop_requested) goto stopped;
        irq_watchdog();
        if (err_pending()) err_show_pending();
        if (platform_quit_requested()) goto stopped;
        platform_pump(); /* host input + tick clock (was: PS/2 + USB HID polling) */
        struct mouse_event me;
        while (mouse_poll(&me)) handle_mouse(&me);
        resize_flush(0);           /* r36: once-per-frame resize apply */
        if (tty_request) {         /* r36: kernel maintenance console */
            tty_select(tty_request);
            tty_request = 0;
            tty_run(1);            /* returns when the user types 'wm' */
            wm_full();             /* tty repainted both buffers */
        }
        struct key_event ke;
        while (kbd_poll(&ke)) {handle_key(&ke);if(tty_request||wm_stop_requested)break;}
        if (wm_stop_requested) goto stopped;

        static u64 last_tick;
        if (tick_count != last_tick) {
            last_tick = tick_count;
            wm_tick();
        }
        if (dirty && (u32)tick_count - last_paint_tick >= 2) {  /* 50 fps cap */
            last_paint_tick = (u32)tick_count;
            if (full_dirty) { paint_all(); full_dirty = 0; }
            else paint_partial();
            dirty = 0;
        }
        wm_in_idle = 1;
        platform_wait_idle();   /* was cpu_hlt(): sleep until host input/frame */
        wm_in_idle = 0;
    }
stopped:
    wm_destroy_session();
}

/* ------------------------------------------------------------- dialogs --- */
struct dialog_data {
    char title[48];
    char message[256];
    char input[128];
    int  has_input;
    int  pos;
    int  resolved;
    dialog_cb cb;
    void *ud;
};

#define DLG_OK_X 40
#define DLG_CANCEL_X 130
#define DLG_BTN_Y_OFF 24

/* Native dialogs wrap within their own content box, never over controls. */
static int dialog_text(struct surface *s,const char *text,int width,int height,u32 color)
{
    int cols=width/8,rows=height/18,used=0;if(cols>127)cols=127;if(cols<1)return 0;
    while(*text&&used<rows){char line[128];int n=0;while(*text&&*text!='\n'&&n<cols)line[n++]=*text++;if(*text=='\n')text++;line[n]=0;if(used==rows-1&&*text&&n>=3){line[n-1]='.';line[n-2]='.';line[n-3]='.';}if(s)s_text(s,14,14+used*18,line,color);used++;}
    return used;
}

static void dlg_paint(struct window *w)
{
    struct dialog_data *d = w->data;
    const struct theme *t = theme_current();
    struct surface *s = &w->surf;
    s_fill(s, 0, 0, s->w, s->h, t->win_bg);
    int y=s->h-76;
    dialog_text(s,d->message,s->w-28,d->has_input?y-20:s->h-58,t->text);
    if(d->has_input){
        s_frame_rect(s,14,y,s->w-28,24,t->main);
        int cols=(s->w-40)/8,start=d->pos>=cols?d->pos-cols+1:0;
        s_clip_text(s,18,y+4,d->input+start,t->text,s->w-40);
        if((tick_count/50)%2==0)s_fill(s,18+(d->pos-start)*8,y+4,2,16,t->main);
    }
    y = s->h - 40;
    s_fill(s, DLG_OK_X, y, 70, 26, blend(t->win_bg, t->main, 25));
    s_frame_rect(s, DLG_OK_X, y, 70, 26, t->main);
    s_text(s, DLG_OK_X + 22, y + 5, "OK", t->text);
    s_fill(s, DLG_CANCEL_X, y, 80, 26, blend(t->win_bg, t->main, 12));
    s_frame_rect(s, DLG_CANCEL_X, y, 80, 26, t->main);
    s_text(s, DLG_CANCEL_X + 16, y + 5, "Cancel", t->text);
}

static void dlg_resolve(struct window *w, int ok)
{
    struct dialog_data *d = w->data;
    d->resolved = 1;
    dialog_cb cb = d->cb;
    void *ud = d->ud;
    char *text = (ok && d->has_input) ? d->input : NULL;
    modal_w = NULL;
    if (cb) cb(ok, text, ud);
    wm_close_window(w);
}

static void dlg_mouse(struct window *w, struct mouse_event *e, int x, int y)
{
    if (e->type != MEV_BUTTON || !e->down || e->button != MBTN_LEFT) return;
    int by = w->surf.h - 40;
    if (in_rect(x, y, DLG_OK_X, by, 70, 26)) dlg_resolve(w, 1);
    else if (in_rect(x, y, DLG_CANCEL_X, by, 80, 26)) dlg_resolve(w, 0);
}

static void dlg_key(struct window *w, struct key_event *e)
{
    struct dialog_data *d = w->data;
    if (d->has_input) {
        if (e->pressed && e->keycode == '\n') { dlg_resolve(w, 1); return; }
        if (e->pressed && e->keycode == 27)   { dlg_resolve(w, 0); return; }
        edit_line(d->input, &d->pos, sizeof(d->input), e);
        wm_redraw(w);      /* r36: same echo bug class as the terminal -
                            * typed dialog input was invisible until OK */
        return;
    }
    if (e->pressed && (e->keycode == '\n' || e->keycode == 27))
        dlg_resolve(w, e->keycode == '\n');
}

static void dlg_close(struct window *w)
{
    struct dialog_data *d = w->data;
    if (d && !d->resolved && d->cb) d->cb(0, NULL, d->ud);
    if (d) pfree(d, sizeof(*d));
    w->data = NULL;
}

static struct app dialog_app = {
    .id = "_dialog", .title = "Dialog", .icon = ICON_INFO, .single = 0,
    .def_w = 420, .def_h = 200,
    .paint = dlg_paint, .key = dlg_key, .mouse = dlg_mouse, .close = dlg_close,
};

void wm_dialog(const char *title, const char *message, const char *input,
               dialog_cb cb, void *ud)
{
    struct window *w = wm_open_app("_dialog", NULL);
    if (!w) { if (cb) cb(0, NULL, ud); return; }
    struct dialog_data *d = palloc_owned(sizeof(*d),(uintptr_t)w);
    if (!d) { wm_close_window(w); if (cb) cb(0,NULL,ud); return; }
    memset(d, 0, sizeof(*d));
    strncpy(d->title, title, sizeof(d->title) - 1);
    strncpy(d->message, message, sizeof(d->message) - 1);
    if (input) {
        d->has_input = 1;
        strncpy(d->input, input, sizeof(d->input) - 1);
        d->pos = (int)strlen(d->input);
    }
    d->cb = cb; d->ud = ud;
    w->data = d;
    int lines=dialog_text(NULL,d->message,390,4096,0);
    wm_resize_window(w,420,lines*18+90+(d->has_input?36:0));
    wm_set_title(w, title);
    modal_w = w;
    wm_full();
    wm_focus(w);
}

/* -------------------------------------------------------- error popups --- */
struct err_data { char text[128]; };

static void err_paint(struct window *w)
{
    struct surface *s = &w->surf;
    s_fill(s, 0, 0, s->w, s->h, 0x1a1a1a);
    dialog_text(s,w->data?((struct err_data *)w->data)->text:"Error",s->w-28,s->h-28,0xFF3333);
}

static void err_close(struct window *w)
{
    if (w->data) pfree(w->data, sizeof(struct err_data));
    w->data = NULL;
}

static struct app error_app = {
    .id = "_error", .title = "ERROR", .icon = ICON_INFO, .single = 0,
    .def_w = 400, .def_h = 180,
    .paint = err_paint, .close = err_close,
};

void wm_error_popup(const char *text)
{
    struct window *w = wm_open_app("_error", NULL);
    if (!w) return;
    struct err_data *d = palloc_owned(sizeof(*d),(uintptr_t)w);
    if (!d) { wm_close_window(w); err_notify("ui",text,NULL,0); return; }
    memset(d,0,sizeof(*d));
    strncpy(d->text, text, sizeof(d->text) - 1);
    w->data = d;
    /* error windows appear near-randomly, like the web sim */
    w->x = 60 + (int)(tick_count * 7 % 400);
    w->y = 40 + (int)(tick_count * 5 % 250);
}

void wm_fatal_screen(const char *line1, const char *line2)
{
    if (!screen.px) { for (;;) cpu_hlt(); }
    fb_clear(0x000080);
    s_text_scaled(&screen, 40, 60, line1, 0xFFFFFF, 3);
    s_text(&screen, 40, 140, line2, 0xFFFFFF);
    s_text(&screen, 40, 170, "The system has been halted. Power off manually.", 0xFFFFFF);
    fb_flip();
}

/* The WM registers its internal clients through the same app interface. */
SCOS_APP(dialog_app, 900);
SCOS_APP(error_app, 901);

void wm_poweroff_screen(void)
{
    fb_clear(0x000000);
    const struct theme *t = theme_current();
    s_text_scaled(&screen, (screen_w - 4 * 8 * 3) / 2, screen_h / 2 - 60, "SCos", t->main, 3);
    const char *m = "It is now safe to turn off your computer.";
    s_text(&screen, (screen_w - s_text_width(m)) / 2, screen_h / 2, m, 0xAAAAAA);
    fb_flip();
}
