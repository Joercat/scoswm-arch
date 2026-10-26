/*
 * scoswm — a real X11 window manager that looks and behaves like SCos.
 *
 * This is NOT the SCos desktop running in a window: it manages ordinary
 * X11 applications (terminals, browsers, editors — including the large
 * majority of Wayland programs, which run through XWayland).  The SCos
 * part is the look and the interaction model, taken from SCos's wm.c:
 *
 *   - a 26 px titlebar with minimise/maximise/close buttons (18 px),
 *     accent-coloured when focused, blended when not
 *   - a 40 px bottom taskbar: 2x2-grid launcher button, a "Windows N"
 *     picker, pinned-app shortcuts, a power button and a clock
 *   - desktop icons on an 88x96 snap grid over a gradient+grid
 *     wallpaper, rubber-band selection, right-click context menus
 *   - the four SCos themes (Matrix, Blue Sky, Purple, Matrix red),
 *     switchable from ~/.config/scoswm/scoswm.conf
 *
 * Why X11 and Xlib: it is the display stack where the proprietary
 * NVIDIA driver and nouveau (and Intel/AMD) all behave identically and
 * maturely, clients render directly through the driver with no
 * compositor in between, and a non-compositing WM like this one costs
 * a few MB and ~0% CPU at idle.  XWayland covers Wayland-only clients.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>
#include <X11/extensions/shape.h>

#include <dlfcn.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "util.h"
#include "wifi.h"

#ifndef VERSION
#define VERSION "0.5.7"
#endif

#define TITLE_H 26
#define BTN_SZ 18
#define TOP_H (TITLE_H + 1)          /* titlebar + top border */
#define DESK_COL_W 88
#define DESK_ROW_H 96
#define DESK_EDGE_GAP 16          /* keep the right rule off physical overscan */
#define DESK_RULE_W 2
#define BAR_DATE_W 48
#define BAR_CLOCK_W 76
#define BAR_SECTION_GAP 8
#define TOOL_W 540
#define TOOL_H 390

/* ------------------------------------------------------------------ state */
static Display *dpy;
static int scr_no;
static Window root, wmcheck, xsettings_win, bar_win, edge_win, launch_win, menu_win, tool_win, anim_win, overview_win;
static int edge_input_passthrough;
static GC gc;
static XFontStruct *font;
static int screen_w, screen_h;
static struct scos_config cfg;
char **saved_argv;
static struct scos_theme theme;
static int bar_h = 40;
static int running = 1;

static struct {
    unsigned long main, bg_top, bg_bot, win_bg, text, title_text, taskbar_bg;
    unsigned long title_off, bar_hi, grid, dark;
} PX;

enum { A_NET_WM_NAME, A_UTF8_STRING, A_WM_DELETE_WINDOW, A_WM_TAKE_FOCUS,
       A_NET_SUPPORTED, A_NET_CLIENT_LIST, A_NET_ACTIVE_WINDOW,
       A_NET_SUPPORTING_WM_CHECK, A_NET_WM_DESKTOP, A_NET_NUMBER_OF_DESKTOPS,
       A_NET_CURRENT_DESKTOP, A_NET_WORKAREA, A_NET_FRAME_EXTENTS,
       A_NET_CLOSE_WINDOW, A_NET_MOVERESIZE_WINDOW, A_NET_WM_STATE,
       A_NET_WM_STATE_MAXIMIZED_VERT, A_NET_WM_STATE_MAXIMIZED_HORZ,
       A_NET_WM_STATE_FULLSCREEN, A_NET_WM_STATE_HIDDEN,
       A_NET_WM_WINDOW_TYPE, A_NET_WM_WINDOW_TYPE_DOCK,
       A_NET_WM_WINDOW_TYPE_DESKTOP, A_NET_WM_WINDOW_TYPE_NOTIFICATION,
       A_NET_WM_WINDOW_TYPE_DIALOG, A_NET_WM_STRUT_PARTIAL,
       A_NET_WM_ICON, A_NET_WM_PID, A_COUNT };
static Atom atoms[A_COUNT];
static const char *const atom_names[A_COUNT] = {
    "_NET_WM_NAME", "UTF8_STRING", "WM_DELETE_WINDOW", "WM_TAKE_FOCUS",
    "_NET_SUPPORTED", "_NET_CLIENT_LIST", "_NET_ACTIVE_WINDOW",
    "_NET_SUPPORTING_WM_CHECK", "_NET_WM_DESKTOP", "_NET_NUMBER_OF_DESKTOPS",
    "_NET_CURRENT_DESKTOP", "_NET_WORKAREA", "_NET_FRAME_EXTENTS",
    "_NET_CLOSE_WINDOW", "_NET_MOVERESIZE_WINDOW", "_NET_WM_STATE",
    "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ",
    "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE_HIDDEN",
    "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK",
    "_NET_WM_WINDOW_TYPE_DESKTOP", "_NET_WM_WINDOW_TYPE_NOTIFICATION",
    "_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_STRUT_PARTIAL",
    "_NET_WM_ICON", "_NET_WM_PID",
};

struct xicon {
    char key[768];
    int size, w, h, failed, owned;
    Pixmap pix, mask;
    struct xicon *next;
};
static struct xicon *xicon_cache;

struct client {
    Window win, frame;
    struct xicon *icon;
    int x, y, w, h;                /* frame position, client size */
    int placed;                    /* last geometry actually sent to X */
    int placed_x, placed_y, placed_fw, placed_fh;
    int placed_cx, placed_cy, placed_cw, placed_ch;
    int sx, sy, sw, sh;            /* saved geometry for max/fullscreen */
    int minimized, maximized, fullscreen;
    Window transient_for;
    XSizeHints hints; int has_hints;
    char title[256];
    char wclass[128];              /* WM_CLASS name/class, lower case */
};

static struct client **clients;
static int nclients, clients_cap;
static struct client *active_client;

#define OVERVIEW_MARGIN 32
#define OVERVIEW_HEADER 48
#define OVERVIEW_GUTTER 16
struct overview_tile {
    Window win;
    int x, y, w, h;
    Pixmap preview;
    int preview_w, preview_h;
};
static struct overview_tile *overview_tiles;
static int overview_count, overview_active, overview_hover = -1;
static Pixmap overview_pm;
static int overview_pm_w, overview_pm_h;

static struct desktop_app apps[1024];
static int napps;

enum core_action { CORE_NONE, CORE_TERMINAL, CORE_FILES, CORE_SYSMON, CORE_SETTINGS };
enum { SHORTCUT_LAUNCHER, SHORTCUT_BAR, SHORTCUT_DESKTOP };
#define MAX_PINS 32
struct pin { char label[64]; char glyph[4]; char icon[160]; char command[512]; char match[96]; int action; int slot; };
static struct pin pins[MAX_PINS];       /* quick launch / taskbar */
static int npins;
static struct pin desk_pins[MAX_PINS];  /* independent Home/desktop shortcuts */
static int ndesk_pins;

static unsigned long desk_sel_mask;
static int desk_hover = -1;
static int bar_hover = -1;          /* -1 none, >=0 pin, -2 start, -3 windows, -4 power, -5 Wi-Fi */
static int bar_press = -1;          /* bar_items idx held down, -1 none */
static int bar_flash_pin = -1;      /* pin idx showing launch feedback */
static time_t bar_flash_until;

#define WIFI_MAX_ADAPTERS 32
#define WIFI_MAX_NETWORKS 64
#define WIFI_VISIBLE_NETWORKS 6
#define WIFI_NETWORK_Y 156
static struct wifi_adapter wifi_adapters[WIFI_MAX_ADAPTERS];
static int wifi_adapter_count, wifi_connected_count, wifi_hotspot_count;
static struct wifi_network wifi_networks[WIFI_MAX_NETWORKS];
static int wifi_network_count, wifi_selected = -1, wifi_network_scroll;
static int wifi_password_editing;
static char wifi_password[128], wifi_status[192] = "Click Scan to discover networks";
static struct {
    int fd, kind;                 /* 1 scan, 2 connect */
    pid_t pid;
    size_t length;
    char output[65536];
} wifi_job = { -1, 0, 0, 0, { 0 } };

static struct client *hover_frame_client;
static int hover_title_btn = -1;
static Cursor wm_cursor;
static int strut_l, strut_r, strut_t, strut_b;   /* external (other docks) */

static void die(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "scoswm: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

int wm_conflict;
static int wm_conflict_error(Display *d, XErrorEvent *e)
{
    (void)d;
    if (e->error_code == BadAccess) wm_conflict = 1;
    return 0;
}

static int xioerror(Display *d)
{
    (void)d;
    fprintf(stderr, "scoswm: connection to the X server was lost, exiting\n");
    fflush(stderr);
    _exit(1);
}

static int xerror(Display *d, XErrorEvent *e)
{
    (void)d;
    if (getenv("SCOSWM_DEBUG")) {
        char buf[128];
        XGetErrorText(d, e->error_code, buf, sizeof(buf));
        fprintf(stderr, "scoswm: X error %s (request %d)\n", buf, e->request_code);
    }
    return 0;
}

static unsigned long alloc_rgb(unsigned int rgb)
{
    XColor c;
    c.red = (unsigned short)(((rgb >> 16) & 0xFF) * 257);
    c.green = (unsigned short)(((rgb >> 8) & 0xFF) * 257);
    c.blue = (unsigned short)((rgb & 0xFF) * 257);
    Colormap cm = DefaultColormap(dpy, scr_no);
    if (!XAllocColor(dpy, cm, &c)) return BlackPixel(dpy, scr_no);
    return c.pixel;
}

/* ------------------------------------------------------------------- misc */
static int cmd_exists(const char *name)
{
    if (strchr(name, '/')) return access(name, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) return 0;
    char buf[1024];
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        int len = colon ? (int)(colon - p) : (int)strlen(p);
        snprintf(buf, sizeof(buf), "%.*s/%s", len, p, name);
        if (access(buf, X_OK) == 0) return 1;
        if (!colon) break;
        p = colon + 1;
    }
    return 0;
}

static void spawn(const char *cmd)
{
    if (!cmd || !*cmd) return;
    /* double-fork so the child is init's problem, like every other WM */
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            close(ConnectionNumber(dpy));
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    int st; while (waitpid(-1, &st, WNOHANG) > 0) { }
}

static const char *detect_terminal(void)
{
    static char found[128];
    if (cfg.terminal[0]) return cfg.terminal;
    static const char *const list[] = {
        "scos-terminal", "alacritty", "kitty", "st", "x-terminal-emulator",
        "xfce4-terminal", "gnome-terminal", "konsole", "mate-terminal",
        "xterm", NULL
    };
    for (int i = 0; list[i]; i++)
        if (cmd_exists(list[i])) { snprintf(found, sizeof(found), "%s", list[i]); return found; }
    return "scos-terminal";
}

static const char *detect_files_exec(void)
{
    /* Files is part of this desktop, just like SCos Terminal. Never make
     * the default depend on whichever third-party file manager happens to
     * be installed. An explicit cfg.files command remains an override. */
    return "scos-files";
}

static void spawn_files_at(const char *path)
{
    char cmd[1024];
    if (!path || !*path) path = getenv("HOME") ? getenv("HOME") : "/";
    if (cfg.files[0]) {
        /* A configured command with arguments is run exactly as written;
         * a bare executable is given the requested folder. */
        if (!strchr(cfg.files, ' ') && !strchr(cfg.files, '\t'))
            snprintf(cmd, sizeof(cmd), "%s %s", cfg.files, path);
        else
            snprintf(cmd, sizeof(cmd), "%s", cfg.files);
    } else {
        const char *exec = detect_files_exec();
        if (exec) snprintf(cmd, sizeof(cmd), "%s %s", exec, path);
        else snprintf(cmd, sizeof(cmd), "xdg-open %s", path);
    }
    spawn(cmd);
}

static void spawn_files_home(void)
{
    spawn_files_at(getenv("HOME") ? getenv("HOME") : "/");
}

/* ------------------------------------------------------------ work area */
static void work_area(int *x, int *y, int *w, int *h)
{
    *x = strut_l; *y = strut_t;
    *w = screen_w - strut_l - strut_r;
    *h = screen_h - strut_t - strut_b - bar_h;
    if (*w < 100) *w = 100;
    if (*h < 100) *h = 100;
}

/* -------------------------------------------------------------- clients */
static struct client *find_client(Window w)
{
    for (int i = 0; i < nclients; i++)
        if (clients[i]->win == w || clients[i]->frame == w) return clients[i];
    return NULL;
}

static void client_apply_hints(struct client *c, int *w, int *h)
{
    if (*w < 80) *w = 80;
    if (*h < 50) *h = 50;
    if (c->has_hints) {
        if (c->hints.flags & PMinSize) {
            if (*w < c->hints.min_width) *w = c->hints.min_width;
            if (*h < c->hints.min_height) *h = c->hints.min_height;
        }
        if (c->hints.flags & PMaxSize && c->hints.max_width > 0) {
            if (*w > c->hints.max_width) *w = c->hints.max_width;
            if (*h > c->hints.max_height) *h = c->hints.max_height;
        }
        if ((c->hints.flags & PResizeInc) && c->hints.width_inc > 1 && c->hints.height_inc > 1) {
            int bw = (c->hints.flags & PBaseSize) ? c->hints.base_width : 0;
            int bh = (c->hints.flags & PBaseSize) ? c->hints.base_height : 0;
            *w = bw + ((*w - bw) / c->hints.width_inc) * c->hints.width_inc;
            *h = bh + ((*h - bh) / c->hints.height_inc) * c->hints.height_inc;
        }
    }
    int wx, wy, ww, wh; work_area(&wx, &wy, &ww, &wh);
    if (*w > ww) *w = ww;
    if (*h > wh) *h = wh;
}

static int frame_w(const struct client *c)
{
    return c->fullscreen ? c->w : c->w + 2;
}
static int frame_h(const struct client *c)
{
    return c->fullscreen ? c->h : c->h + TOP_H + 1;
}

static void client_place(struct client *c)
{
    int fw = frame_w(c), fh = frame_h(c);
    int cx = c->fullscreen ? 0 : 1, cy = c->fullscreen ? 0 : TOP_H;
    if (!c->placed || c->placed_x != c->x || c->placed_y != c->y ||
        c->placed_fw != fw || c->placed_fh != fh)
        XMoveResizeWindow(dpy, c->frame, c->x, c->y, fw, fh);
    /* Moving a frame must not issue a same-size resize to the application. */
    if (!c->placed || c->placed_cx != cx || c->placed_cy != cy ||
        c->placed_cw != c->w || c->placed_ch != c->h)
        XMoveResizeWindow(dpy, c->win, cx, cy, c->w, c->h);
    /* Synthetic ConfigureNotify supplies root-relative coordinates after a
     * reparented move as required by ICCCM, but skip duplicate notifications. */
    if (!c->placed || c->placed_x != c->x || c->placed_y != c->y ||
        c->placed_cx != cx || c->placed_cy != cy ||
        c->placed_cw != c->w || c->placed_ch != c->h) {
        XEvent ev; memset(&ev, 0, sizeof(ev));
        ev.type = ConfigureNotify;
        ev.xconfigure.display = dpy; ev.xconfigure.event = c->win; ev.xconfigure.window = c->win;
        ev.xconfigure.x = c->x + cx;
        ev.xconfigure.y = c->y + cy;
        ev.xconfigure.width = c->w; ev.xconfigure.height = c->h;
        ev.xconfigure.border_width = 0; ev.xconfigure.above = None;
        ev.xconfigure.override_redirect = False;
        XSendEvent(dpy, c->win, False, StructureNotifyMask, &ev);
    }
    c->placed = 1;
    c->placed_x = c->x; c->placed_y = c->y;
    c->placed_fw = fw; c->placed_fh = fh;
    c->placed_cx = cx; c->placed_cy = cy;
    c->placed_cw = c->w; c->placed_ch = c->h;
}

static void read_title(struct client *c)
{
    Atom type; int fmt; unsigned long n = 0, after = 0; unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, c->win, atoms[A_NET_WM_NAME], 0, 64, False,
                           atoms[A_UTF8_STRING], &type, &fmt, &n, &after, &data) == Success && data) {
        utf8_to_latin1((const char *)data, c->title, sizeof(c->title));
        XFree(data);
    } else {
        if (data) XFree(data);
        char *name = NULL;
        if (XFetchName(dpy, c->win, &name) && name) {
            snprintf(c->title, sizeof(c->title), "%s", name);
            XFree(name);
        } else snprintf(c->title, sizeof(c->title), "window");
    }
}

static void read_class(struct client *c)
{
    XClassHint ch;
    c->wclass[0] = 0;
    if (XGetClassHint(dpy, c->win, &ch)) {
        snprintf(c->wclass, sizeof(c->wclass), "%s %s",
                 ch.res_name ? ch.res_name : "", ch.res_class ? ch.res_class : "");
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
        for (char *p = c->wclass; *p; p++) *p = (char)tolower((unsigned char)*p);
    }
}

static void read_hints(struct client *c)
{
    long supplied = 0;
    c->has_hints = XGetWMNormalHints(dpy, c->win, &c->hints, &supplied) != 0;
}

static int has_proto(struct client *c, Atom a)
{
    Atom *list = NULL; int n = 0, found = 0;
    if (XGetWMProtocols(dpy, c->win, &list, &n)) {
        for (int i = 0; i < n; i++) if (list[i] == a) found = 1;
        XFree(list);
    }
    return found;
}

static void send_proto(struct client *c, Atom a)
{
    XEvent ev; memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.xclient.window = c->win;
    ev.xclient.message_type = XInternAtom(dpy, "WM_PROTOCOLS", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)a; ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, c->win, False, NoEventMask, &ev);
}

/* forward declarations for the UI half */
static void draw_frame(struct client *c);
static void draw_desktop(void);
static void draw_bar(void);
static void bar_layout(void);
static int bar_power_x(void);
static int bar_wifi_x(void);
static void animate_minimize(struct client *c);
static void animate_restore(struct client *c);
static struct xicon *xicon_get(const char *spec, int size);
static void draw_xicon(Window dst, struct xicon *xi, int x, int y);
static struct xicon *client_load_icon(struct client *c);
static void xicon_destroy_owned(struct xicon *xi);
static void save_settings(const char *what);
static void redraw_everything(void);
static void update_ewmh_client_list(void);
static void launcher_toggle(int open);
static void wm_notify_no_windows(void);
static void menu_close(void);
enum { TOOL_NONE, TOOL_SETTINGS, TOOL_SYSMON, TOOL_WIFI };
enum { TOOL_HOVER_NONE, TOOL_HOVER_CLOSE, TOOL_HOVER_WIFI_PASSWORD,
       TOOL_HOVER_WIFI_CONNECT, TOOL_HOVER_WIFI_SCAN,
       TOOL_HOVER_BAR_DEC, TOOL_HOVER_BAR_INC, TOOL_HOVER_ICON_DEC,
       TOOL_HOVER_ICON_INC, TOOL_HOVER_GRID,
       TOOL_HOVER_NETWORK = 100, TOOL_HOVER_THEME = 200, TOOL_HOVER_FIELD = 300 };
static int tool_kind;
static int tool_hover;
static int tool_release_pending;
static void tool_open(int kind);
static void tool_close(void);
static void draw_tool(void);
static int tool_hover_at(int x, int y);
static int tool_key(XKeyEvent *e);
static int tool_button(XButtonEvent *e);
static void spawn_files_home(void);
static void menu_open(int kind, int x, int y, Window target);
static void overview_show(void);
static void overview_close(void);
static void overview_clients_changed(void);
static void draw_overview(void);
static void draw_right_rule(void);
static void restack_right_rule(void);
static int desktop_cols(void);
static int desktop_slot_count(void);
static void desktop_normalize_slots(void);

static void set_wm_state(struct client *c, int state)
{
    long data[2] = { state, None };
    XChangeProperty(dpy, c->win, XInternAtom(dpy, "WM_STATE", False),
                    XInternAtom(dpy, "WM_STATE", False), 32, PropModeReplace,
                    (unsigned char *)data, 2);
    /* _NET_WM_STATE mirror */
    Atom st[3]; int n = 0;
    if (c->minimized) st[n++] = atoms[A_NET_WM_STATE_HIDDEN];
    if (c->maximized) { st[n++] = atoms[A_NET_WM_STATE_MAXIMIZED_VERT]; st[n++] = atoms[A_NET_WM_STATE_MAXIMIZED_HORZ]; }
    if (c->fullscreen) st[n++] = atoms[A_NET_WM_STATE_FULLSCREEN];
    XChangeProperty(dpy, c->win, atoms[A_NET_WM_STATE], XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)st, n);
}

static void raise_client_frame(struct client *c)
{
    if (c->fullscreen) {
        XRaiseWindow(dpy, c->frame);
    } else {
        /* Keep both pieces of WM chrome above ordinary frames. Previously
         * frames were inserted directly below the bar but above edge_win,
         * so a covering application could completely hide the right rule. */
        XWindowChanges wc;
        wc.sibling = edge_win ? edge_win : bar_win;
        wc.stack_mode = Below;
        XConfigureWindow(dpy, c->frame, CWSibling | CWStackMode, &wc);
    }
}

static void activate(struct client *c, int raise)
{
    if (!c) return;
    if (overview_active) overview_close();
    if (c->minimized) {
        c->minimized = 0;
        animate_restore(c);
        set_wm_state(c, NormalState);
    }
    /* move to the top of the manage list = stacking/focus order */
    for (int i = 0; i < nclients; i++)
        if (clients[i] == c) {
            memmove(&clients[i], &clients[i + 1], (nclients - i - 1) * sizeof(*clients));
            clients[nclients - 1] = c;
            break;
        }
    if (raise) raise_client_frame(c);
    XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
    if (has_proto(c, atoms[A_WM_TAKE_FOCUS])) send_proto(c, atoms[A_WM_TAKE_FOCUS]);
    struct client *old = active_client;
    active_client = c;
    XChangeProperty(dpy, root, atoms[A_NET_ACTIVE_WINDOW], XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&c->win, 1);
    if (old && old != c) draw_frame(old);
    draw_frame(c);
    draw_bar();
}

static void focus_top(void)
{
    for (int i = nclients - 1; i >= 0; i--)
        if (!clients[i]->minimized) { activate(clients[i], 1); return; }
    active_client = NULL;
    XDeleteProperty(dpy, root, atoms[A_NET_ACTIVE_WINDOW]);
    XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
    draw_bar();
}

static void minimize(struct client *c)
{
    if (!c || c->minimized) return;
    animate_minimize(c);
    c->minimized = 1;
    XUnmapWindow(dpy, c->frame);
    XUnmapWindow(dpy, c->win);
    set_wm_state(c, IconicState);
    if (active_client == c) focus_top(); else { draw_frame(c); draw_bar(); }
}

static void frame_position_for_restore(struct client *c)
{
    client_place(c);
    draw_frame(c);
    draw_bar();
}

static void set_maximized(struct client *c, int on)
{
    if (!c || c->fullscreen || c->maximized == !!on) return;
    if (on) {
        c->sx = c->x; c->sy = c->y; c->sw = c->w; c->sh = c->h;
        int x, y, w, h; work_area(&x, &y, &w, &h);
        c->x = x; c->y = y; c->w = w - 2; c->h = h - TOP_H - 1;
        c->maximized = 1;
    } else {
        c->x = c->sx; c->y = c->sy; c->w = c->sw; c->h = c->sh;
        c->maximized = 0;
    }
    set_wm_state(c, NormalState);
    frame_position_for_restore(c);
}

static void set_fullscreen(struct client *c, int on)
{
    if (!c || c->fullscreen == !!on) return;
    if (on) {
        if (!c->maximized) { c->sx = c->x; c->sy = c->y; c->sw = c->w; c->sh = c->h; }
        c->x = 0; c->y = 0; c->w = screen_w; c->h = screen_h;
        c->fullscreen = 1;
    } else {
        c->x = c->sx; c->y = c->sy; c->w = c->sw; c->h = c->sh;
        c->fullscreen = 0;
    }
    set_wm_state(c, NormalState);
    frame_position_for_restore(c);
    raise_client_frame(c);
}

static void close_client(struct client *c)
{
    if (!c) return;
    if (has_proto(c, atoms[A_WM_DELETE_WINDOW])) send_proto(c, atoms[A_WM_DELETE_WINDOW]);
    else XKillClient(dpy, c->win);
}

static struct {
    int kind;               /* 0 none 1 move 2 resize 3 band 4 shortcut */
    struct client *c;
    int start_x, start_y;   /* pointer at grab */
    int ox, oy, ow, oh;     /* frame geometry, or icon grab offset for shortcuts */
    int edges;              /* resize edges: 1 left 2 right 4 top 8 bottom */
    int moved;
    int band_x, band_y, band_drawn;
    int pin_index, pin_source;
    int current_x, current_y, target_slot, target_order;
} drag;

/* Shortcut clicks are completed on release, after the shared drag detector
 * has rejected motion.  A single click activates an existing window; a
 * second click within the X11 double-click interval starts a new instance.
 * An unopened taskbar pin still launches on a single click, but that launch
 * waits briefly so a double-click does not accidentally start two copies. */
static struct {
    int source, index, x, y;
    Time time;
} shortcut_click = { .source = -1, .index = -1 };
static struct pin taskbar_pending_pin;
static int taskbar_launch_pending;
static long long taskbar_pending_since;

static long long monotonic_msec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void grab_drag_pointer(struct client *c, Time t)
{
    /* Do not rely on the client's implicit button grab: it can keep
     * motion and release events away from the WM, especially when the
     * drag began in the client area. Grab the frame (or root for a
     * desktop band) until the release is handled by on_button(). */
    XGrabPointer(dpy, c ? c->frame : root, False,
                 PointerMotionMask | ButtonReleaseMask,
                 GrabModeAsync, GrabModeAsync, None, None, t);
}

static void remove_client(struct client *c, int reparent)
{
    int idx = -1;
    for (int i = 0; i < nclients; i++) if (clients[i] == c) idx = i;
    if (idx < 0) return;
    if (reparent) {
        XGrabServer(dpy);
        XReparentWindow(dpy, c->win, root, c->x + 1, c->y + TOP_H);
        XRemoveFromSaveSet(dpy, c->win);
        XUngrabServer(dpy);
    }
    if (drag.c == c) {
        XUngrabPointer(dpy, CurrentTime);
        drag.kind = 0; drag.c = NULL;
    }
    if (hover_frame_client == c) {
        hover_frame_client = NULL;
        hover_title_btn = -1;
    }
    XDestroyWindow(dpy, c->frame);
    xicon_destroy_owned(c->icon);
    memmove(&clients[idx], &clients[idx + 1], (nclients - idx - 1) * sizeof(*clients));
    nclients--;
    if (active_client == c) active_client = NULL;
    free(c);
    update_ewmh_client_list();
    if (!active_client) focus_top(); else draw_bar();
    overview_clients_changed();
}

static Atom window_type_of(Window w)
{
    Atom type; int fmt; unsigned long n = 0, after = 0; unsigned char *data = NULL;
    Atom first = None;
    if (XGetWindowProperty(dpy, w, atoms[A_NET_WM_WINDOW_TYPE], 0, 4, False, XA_ATOM,
                           &type, &fmt, &n, &after, &data) == Success && data) {
        if (n > 0) first = ((Atom *)data)[0];
        XFree(data);
    }
    return first;
}

static void recalc_struts(void)
{
    strut_l = strut_r = strut_t = strut_b = 0;
    Window r, parent, *kids = NULL; unsigned int nkids = 0;
    if (!XQueryTree(dpy, root, &r, &parent, &kids, &nkids)) return;
    for (unsigned int i = 0; i < nkids; i++) {
        Atom type; int fmt; unsigned long n = 0, after = 0; unsigned char *data = NULL;
        if (kids[i] == bar_win) continue;
        if (XGetWindowProperty(dpy, kids[i], atoms[A_NET_WM_STRUT_PARTIAL], 0, 12, False,
                               XA_CARDINAL, &type, &fmt, &n, &after, &data) == Success && data) {
            long *s = (long *)data;
            if (n >= 4) {
                if (s[0] > strut_l) strut_l = (int)s[0];
                if (s[1] > strut_r) strut_r = (int)s[1];
                if (s[2] > strut_t) strut_t = (int)s[2];
                if (s[3] > strut_b) strut_b = (int)s[3];
            }
            XFree(data);
        }
    }
    if (kids) XFree(kids);
    /* publish our own work area */
    int x, y, w, h; work_area(&x, &y, &w, &h);
    long wa[4] = { x, y, w, h };
    XChangeProperty(dpy, root, atoms[A_NET_WORKAREA], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)wa, 4);
}

static void manage(Window win)
{
    if (win == bar_win || win == edge_win || win == launch_win || win == menu_win || win == tool_win ||
        win == anim_win || win == overview_win || win == wmcheck || win == xsettings_win) return;
    if (find_client(win)) return;
    XWindowAttributes at;
    if (!XGetWindowAttributes(dpy, win, &at)) return;
    if (at.override_redirect) return;

    Atom wt = window_type_of(win);
    if (wt == atoms[A_NET_WM_WINDOW_TYPE_DOCK] ||
        wt == atoms[A_NET_WM_WINDOW_TYPE_DESKTOP] ||
        wt == atoms[A_NET_WM_WINDOW_TYPE_NOTIFICATION]) {
        /* panels/desktops/notifications keep their own geometry */
        XMapWindow(dpy, win);
        recalc_struts();
        return;
    }

    struct client *c = calloc(1, sizeof(*c));
    c->win = win;
    c->w = at.width > 0 ? at.width : 400;
    c->h = at.height > 0 ? at.height : 300;
    read_title(c); read_hints(c); read_class(c);
    c->icon = client_load_icon(c);
    client_apply_hints(c, &c->w, &c->h);
    c->x = at.x - 1; c->y = at.y - TOP_H;
    int wx, wy, ww, wh; work_area(&wx, &wy, &ww, &wh);
    if (c->x + frame_w(c) > wx + ww) c->x = wx + ww - frame_w(c);
    if (c->y + frame_h(c) > wy + wh) c->y = wy + wh - frame_h(c);
    if (c->x < wx) c->x = wx;
    if (c->y < wy) c->y = wy;
    Window tw = None;
    if (XGetTransientForHint(dpy, win, &tw) && tw && tw != root) {
        c->transient_for = tw;
        struct client *p = find_client(tw);
        if (p) {  /* centre dialogs over their parent, SCos-modal style */
            c->x = p->x + (frame_w(p) - frame_w(c)) / 2;
            c->y = p->y + (frame_h(p) - frame_h(c)) / 2;
            if (c->x < wx) c->x = wx;
            if (c->y < wy) c->y = wy;
        }
    }
    /* initial state from the window itself */
    Atom type; int fmt; unsigned long n = 0, after = 0; unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, atoms[A_NET_WM_STATE], 0, 8, False, XA_ATOM,
                           &type, &fmt, &n, &after, &data) == Success && data) {
        Atom *st = (Atom *)data;
        for (unsigned long i = 0; i < n; i++) {
            if (st[i] == atoms[A_NET_WM_STATE_FULLSCREEN]) c->fullscreen = 1;
            if (st[i] == atoms[A_NET_WM_STATE_MAXIMIZED_VERT] ||
                st[i] == atoms[A_NET_WM_STATE_MAXIMIZED_HORZ]) c->maximized = 1;
            if (st[i] == atoms[A_NET_WM_STATE_HIDDEN]) c->minimized = 1;
        }
        XFree(data);
    }

    XSetWindowAttributes fa;
    fa.background_pixel = PX.win_bg;
    if (wm_cursor) fa.cursor = wm_cursor;
    fa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                    PointerMotionMask | EnterWindowMask | LeaveWindowMask |
                    StructureNotifyMask;
    fa.override_redirect = True;
    c->frame = XCreateWindow(dpy, root, c->x, c->y, frame_w(c), frame_h(c), 0,
                             CopyFromParent, InputOutput, CopyFromParent,
                             CWBackPixel | CWEventMask | CWOverrideRedirect |
                             (wm_cursor ? CWCursor : 0), &fa);
    XSetWindowBackgroundPixmap(dpy, c->frame, None);
    if (c->fullscreen) { c->x = 0; c->y = 0; c->w = screen_w; c->h = screen_h; }
    else if (c->maximized) {
        c->sx = c->x; c->sy = c->y; c->sw = c->w; c->sh = c->h;
        c->x = wx; c->y = wy; c->w = ww - 2; c->h = wh - TOP_H - 1;
    }

    XSetWindowBorderWidth(dpy, win, 0);
    XReparentWindow(dpy, win, c->frame, c->fullscreen ? 0 : 1, c->fullscreen ? 0 : TOP_H);
    XAddToSaveSet(dpy, win);
    XSelectInput(dpy, win, StructureNotifyMask | PropertyChangeMask | FocusChangeMask);
    /* A passive grab lets the WM offer edge resizing without selecting
     * (and potentially stealing) the application's normal input. Ordinary
     * left clicks are replayed to the client in on_button(); an edge
     * press keeps the grab so motion and release reliably reach the WM. */
    XGrabButton(dpy, Button1, 0, win, False,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeSync, GrabModeAsync, None, None);
    for (int b = Button1; b <= Button3; b++)
        XGrabButton(dpy, b, Mod1Mask, win, False,
                    ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                    GrabModeAsync, GrabModeAsync, None, None);
    client_place(c);

    long ext[4] = { 1, 1, TOP_H, 1 };
    XChangeProperty(dpy, win, atoms[A_NET_FRAME_EXTENTS], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)ext, 4);
    long zero = 0;
    XChangeProperty(dpy, win, atoms[A_NET_WM_DESKTOP], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&zero, 1);
    set_wm_state(c, c->minimized ? IconicState : NormalState);

    if (nclients == clients_cap) {
        clients_cap = clients_cap ? clients_cap * 2 : 16;
        clients = realloc(clients, clients_cap * sizeof(*clients));
        if (!clients) die("out of memory");
    }
    clients[nclients++] = c;
    update_ewmh_client_list();

    if (!c->minimized) {
        draw_frame(c);
        XMapWindow(dpy, win);
        XMapWindow(dpy, c->frame);
        activate(c, 1);
    }
    recalc_struts();
    draw_bar();
    overview_clients_changed();
}

static void update_ewmh_client_list(void)
{
    Window *list = malloc(sizeof(Window) * (nclients > 0 ? nclients : 1));
    if (!list) return;
    int n = 0;
    for (int i = 0; i < nclients; i++) list[n++] = clients[i]->win;
    XChangeProperty(dpy, root, atoms[A_NET_CLIENT_LIST], XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)list, n);
    free(list);
}

/* ------------------------------------------------------------- launcher */
/* The launcher is intentionally curated. Its normal view contains only
 * the four SCos core apps and the user's pins. Typing a search term is
 * the deliberate escape hatch that also searches installed .desktop
 * applications, so hundreds of utilities do not clutter daily use. */
static int launch_open, launch_offset, launch_sel, launch_pin_mode, launch_all;
#define ACTION_ALLAPPS (-1)   /* pseudo-item: switch to browsing all apps */
static char launch_q[64];

struct launch_item { char name[128]; char glyph[4]; char icon[160]; char app_id[96]; char command[1024]; int action; int app_idx; };
static struct launch_item launch_items[2100];
static int launch_nitems;

static const struct { const char *name, *glyph, *icon, *keywords; int action; } core_apps[] = {
    { "Terminal", "Tr", "scos-terminal", "terminal shell console command", CORE_TERMINAL },
    { "Files",    "Fi", "scos-files", "files file manager home documents downloads", CORE_FILES },
    { "SysMon",   "Sy", "scos-sysmon", "sysmon system monitor cpu memory gpu processes", CORE_SYSMON },
    { "Settings", "Se", "scos-settings", "settings theme configuration preferences", CORE_SETTINGS },
};

static void run_core_action(int action);
static void launch_cmd_for(const struct desktop_app *a, char *out, int cap);
static void build_pins(void);

/* Taskbar and desktop shortcuts share an identity, not a presence or
 * position. Legacy configs are copied to both once; every later change is
 * saved independently. */
static int pin_equal(const struct pin *a, const struct pin *b)
{
    if (!a || !b || a->action != b->action) return 0;
    if (a->action != CORE_NONE) return 1;
    if (a->match[0] && b->match[0]) return !strcasecmp(a->match, b->match);
    return !strcmp(a->command, b->command);
}

static int pin_list_find(const struct pin *list, int count, const struct pin *needle)
{
    for (int i = 0; i < count; i++) if (pin_equal(&list[i], needle)) return i;
    return -1;
}

static int app_pin_mask(int app_idx)
{
    if (app_idx < 0 || app_idx >= napps) return 0;
    int mask = 0;
    for (int i = 0; i < npins; i++)
        if (pins[i].action == CORE_NONE && !strcasecmp(pins[i].match, apps[app_idx].id)) mask |= 1;
    for (int i = 0; i < ndesk_pins; i++)
        if (desk_pins[i].action == CORE_NONE && !strcasecmp(desk_pins[i].match, apps[app_idx].id)) mask |= 2;
    return mask;
}

static const char *pin_token(const struct pin *p)
{
    return p->action == CORE_TERMINAL ? "terminal" :
           p->action == CORE_FILES ? "files" :
           p->action == CORE_SYSMON ? "sysmon" :
           p->action == CORE_SETTINGS ? "settings" :
           p->match[0] ? p->match : p->command;
}

static void append_config_token(char *out, size_t cap, int *first, const char *token)
{
    if (!out || !first || !token || !*token) return;
    size_t used = strlen(out);
    if (!*first && used + 1 < cap) { out[used++] = ','; out[used] = 0; }
    strncat(out, token, cap - strlen(out) - 1);
    *first = 0;
}

static void pins_sync_config(void)
{
    cfg.pins[0] = 0; cfg.desktop_pins[0] = 0; cfg.desktop_slots[0] = 0;
    int first = 1;
    for (int i = 0; i < npins; i++) append_config_token(cfg.pins, sizeof(cfg.pins), &first, pin_token(&pins[i]));
    first = 1;
    for (int i = 0; i < ndesk_pins; i++) {
        append_config_token(cfg.desktop_pins, sizeof(cfg.desktop_pins), &first, pin_token(&desk_pins[i]));
        char slot[24];
        snprintf(slot, sizeof(slot), "%s%d", i ? "," : "", desk_pins[i].slot);
        strncat(cfg.desktop_slots, slot, sizeof(cfg.desktop_slots) - strlen(cfg.desktop_slots) - 1);
    }
    cfg.has_desktop_pins = 1;
}

static void launch_add(const char *name, const char *glyph, const char *icon, const char *command, int action, int app_idx)
{
    if (!name || !*name || launch_nitems >= (int)(sizeof(launch_items) / sizeof(launch_items[0]))) return;
    for (int i = 0; i < launch_nitems; i++)
        if (launch_items[i].action == action && !strcmp(launch_items[i].name, name) &&
            !strcmp(launch_items[i].command, command ? command : "")) return;
    struct launch_item *it = &launch_items[launch_nitems++];
    snprintf(it->name, sizeof(it->name), "%s", name);
    snprintf(it->glyph, sizeof(it->glyph), "%s", glyph && *glyph ? glyph : "A");
    snprintf(it->command, sizeof(it->command), "%s", command ? command : "");
    snprintf(it->icon, sizeof(it->icon), "%s", icon ? icon : "");
    if (app_idx >= 0 && app_idx < napps) snprintf(it->app_id, sizeof(it->app_id), "%s", apps[app_idx].id);
    it->action = action;
    it->app_idx = app_idx;
}

static int launch_rebuild(void)
{
    launch_nitems = 0;
    int searching = launch_q[0] != 0;
    for (unsigned int i = 0; i < sizeof(core_apps) / sizeof(core_apps[0]); i++)
        if (ci_contains(core_apps[i].name, launch_q) || ci_contains(core_apps[i].keywords, launch_q))
            launch_add(core_apps[i].name, core_apps[i].glyph, core_apps[i].icon, "", core_apps[i].action, -1);

    /* Pins are part of the user's normal desktop, so keep them visible. */
    for (int i = 0; i < npins; i++) {
        if (pins[i].action != CORE_NONE) continue;
        if (ci_contains(pins[i].label, launch_q)) {
            int app_idx = -1;
            for (int j = 0; j < napps; j++) if (!strcasecmp(apps[j].id, pins[i].match)) app_idx = j;
            launch_add(pins[i].label, pins[i].glyph, pins[i].icon, pins[i].command, CORE_NONE, app_idx);
        }
    }
    for (int i = 0; i < ndesk_pins; i++) {
        if (desk_pins[i].action != CORE_NONE) continue;
        if (ci_contains(desk_pins[i].label, launch_q)) {
            int app_idx = -1;
            for (int j = 0; j < napps; j++) if (!strcasecmp(apps[j].id, desk_pins[i].match)) app_idx = j;
            launch_add(desk_pins[i].label, desk_pins[i].glyph, desk_pins[i].icon,
                       desk_pins[i].command, CORE_NONE, app_idx);
        }
    }

    /* Installed applications: shown while searching, or all of them
     * in "All Apps" browse mode; the curated default view instead
     * ends with an entry that switches to browsing. */
    if (!searching && !launch_all && napps > 0)
        launch_add("All installed apps...", ">>", "", "", ACTION_ALLAPPS, -1);
    if (searching || launch_all) {
        for (int i = 0; i < napps; i++) {
            if (searching && !ci_contains(apps[i].name, launch_q) && !ci_contains(apps[i].exec, launch_q)) continue;
            char cmd[1024];
            launch_cmd_for(&apps[i], cmd, sizeof(cmd));
            char glyph[4] = { apps[i].name[0] ? apps[i].name[0] : 'A', 0 };
            launch_add(apps[i].name, glyph, apps[i].icon[0] ? apps[i].icon : apps[i].id, cmd, CORE_NONE, i);
        }
    }
    if (launch_sel >= launch_nitems) launch_sel = launch_nitems > 0 ? launch_nitems - 1 : 0;
    if (launch_sel < 0) launch_sel = 0;
    return launch_nitems;
}

static void launch_cmd_for(const struct desktop_app *a, char *out, int cap)
{
    char cmd[512];
    exec_strip_field_codes(a->exec, cmd, sizeof(cmd));
    if (a->is_terminal) snprintf(out, cap, "%s -e %s", detect_terminal(), cmd);
    else snprintf(out, cap, "%s", cmd);
}

static void launch_exec_item(const struct launch_item *it)
{
    if (!it) return;
    if (it->action != CORE_NONE) run_core_action(it->action);
    else spawn(it->command);
}

static int command_matches_client(const char *command, const struct client *c)
{
    if (!command || !c) return 0;
    while (*command == ' ' || *command == '\t') command++;
    char executable[160];
    size_t n = 0;
    while (command[n] && command[n] != ' ' && command[n] != '\t' && n + 1 < sizeof(executable)) {
        executable[n] = command[n]; n++;
    }
    executable[n] = 0;
    if ((executable[0] == '\'' || executable[0] == '"') && n >= 2 &&
        executable[n - 1] == executable[0]) {
        executable[n - 1] = 0;
        memmove(executable, executable + 1, n - 1);
    }
    char *base = strrchr(executable, '/');
    base = base ? base + 1 : executable;
    if (strlen(base) < 3 || !strcmp(base, "env") || !strcmp(base, "sh") ||
        !strcmp(base, "bash") || !strcmp(base, "flatpak") || !strcmp(base, "snap") ||
        !strcmp(base, "gtk-launch") || !strcmp(base, "xdg-open")) return 0;
    return ci_contains(c->wclass, base);
}

static int pin_matches_client(const struct pin *p, const struct client *c)
{
    if (!p || !c || p->action == CORE_SYSMON || p->action == CORE_SETTINGS) return 0;
    if (p->match[0] && ci_contains(c->wclass, p->match)) return 1;
    if (p->action == CORE_TERMINAL) {
        if (ci_contains(c->wclass, "scos-terminal")) return 1;
        return command_matches_client(cfg.terminal[0] ? cfg.terminal : detect_terminal(), c);
    }
    if (p->action == CORE_FILES) {
        if (ci_contains(c->wclass, "scos-files")) return 1;
        return command_matches_client(cfg.files[0] ? cfg.files : detect_files_exec(), c);
    }
    return command_matches_client(p->command, c);
}

static struct client *pin_last_client(const struct pin *p)
{
    struct client *match = NULL;
    /* activate() keeps this list in most-recently-active order. */
    for (int k = 0; k < nclients; k++)
        if (pin_matches_client(p, clients[k])) match = clients[k];
    return match;
}

static void launch_pin_new(const struct pin *p)
{
    if (!p) return;
    if (p->action != CORE_NONE) run_core_action(p->action);
    else spawn(p->command);
}

static void launch_pin_value(const struct pin *p)
{
    if (!p) return;
    struct client *match = pin_last_client(p);
    if (match) activate(match, 1);
    else launch_pin_new(p);
}

static int taskbar_insert(const struct pin *p, int order)
{
    int existing = pin_list_find(pins, npins, p);
    if (existing >= 0) {
        if (order < 0 || order > npins) order = npins;
        if (order > existing) order--;
        if (order < 0) order = 0;
        if (order > npins - 1) order = npins - 1;
        if (order != existing) {
            struct pin copy = pins[existing];
            memmove(&pins[existing], &pins[existing + 1], (npins - existing - 1) * sizeof(pins[0]));
            memmove(&pins[order + 1], &pins[order], (npins - order - 1) * sizeof(pins[0]));
            pins[order] = copy;
        }
        return order;
    }
    if (npins >= MAX_PINS) return -1;
    if (order < 0 || order > npins) order = npins;
    memmove(&pins[order + 1], &pins[order], (npins - order) * sizeof(pins[0]));
    pins[order] = *p; pins[order].slot = -1; npins++;
    return order;
}

static void taskbar_remove(int index)
{
    if (index < 0 || index >= npins) return;
    memmove(&pins[index], &pins[index + 1], (npins - index - 1) * sizeof(pins[0]));
    npins--;
}

static int desktop_slot_owner(int slot, int ignore)
{
    for (int i = 0; i < ndesk_pins; i++)
        if (i != ignore && desk_pins[i].slot == slot) return i;
    return -1;
}

static int desktop_free_slot(int preferred, int ignore)
{
    int total = desktop_slot_count(), cols = desktop_cols();
    if (total <= 0) return -1;
    if (preferred < 0 || preferred >= total) preferred = 0;
    int best = -1, best_distance = 0x7fffffff;
    int pc = preferred % cols, pr = preferred / cols;
    for (int slot = 0; slot < total; slot++) {
        if (desktop_slot_owner(slot, ignore) >= 0) continue;
        int dc = slot % cols - pc, dr = slot / cols - pr;
        int distance = dc * dc + dr * dr;
        if (distance < best_distance) { best = slot; best_distance = distance; }
    }
    return best;
}

static int desktop_place(const struct pin *p, int preferred)
{
    int index = pin_list_find(desk_pins, ndesk_pins, p);
    if (index < 0) {
        if (ndesk_pins >= MAX_PINS) return -1;
        index = ndesk_pins++;
        desk_pins[index] = *p;
    }
    int slot = desktop_free_slot(preferred, index);
    if (slot < 0) {
        if (index == ndesk_pins - 1 && pin_list_find(desk_pins, ndesk_pins - 1, p) < 0) ndesk_pins--;
        return -1;
    }
    desk_pins[index].slot = slot;
    return index;
}

static void desktop_remove(int index)
{
    if (index < 0 || index >= ndesk_pins) return;
    memmove(&desk_pins[index], &desk_pins[index + 1], (ndesk_pins - index - 1) * sizeof(desk_pins[0]));
    ndesk_pins--;
    desk_sel_mask = 0; desk_hover = -1;
}

static void desktop_move_slot(int index, int target)
{
    if (index < 0 || index >= ndesk_pins || target < 0 || target >= desktop_slot_count()) return;
    int owner = desktop_slot_owner(target, index);
    int old = desk_pins[index].slot;
    if (owner >= 0) desk_pins[owner].slot = old;
    desk_pins[index].slot = target;
}

static void shortcuts_saved(const char *what)
{
    pins_sync_config();
    save_settings(what);
    redraw_everything();
}

static int shortcut_over_taskbar(int x, int y)
{
    return y >= screen_h - bar_h && y < screen_h && x >= 168 && x < bar_wifi_x() - 8;
}

static int shortcut_bar_order(int x)
{
    int order = (x - 168 + 20) / 40; /* 34 px pin + 6 px gap */
    if (order < 0) order = 0;
    if (order > npins) order = npins;
    return order;
}

static void shortcut_drag_begin(int source, int index, const XButtonEvent *e, int icon_x, int icon_y)
{
    if (taskbar_launch_pending &&
        (source != shortcut_click.source || index != shortcut_click.index)) {
        launch_pin_new(&taskbar_pending_pin);
        taskbar_launch_pending = 0;
    }
    drag.kind = 4; drag.c = NULL;
    drag.pin_source = source; drag.pin_index = index;
    drag.start_x = drag.current_x = e->x_root;
    drag.start_y = drag.current_y = e->y_root;
    drag.ox = source == SHORTCUT_DESKTOP ? e->x_root - icon_x : 40;
    drag.oy = source == SHORTCUT_DESKTOP ? e->y_root - icon_y : 44;
    drag.moved = 0; drag.target_slot = drag.target_order = -1;
    XGrabPointer(dpy, source == SHORTCUT_BAR ? bar_win : root, False,
                 PointerMotionMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync,
                 None, None, e->time);
}

static void shortcut_drag_motion(const XMotionEvent *e)
{
    int old_moved = drag.moved, old_slot = drag.target_slot, old_order = drag.target_order;
    drag.current_x = e->x_root; drag.current_y = e->y_root;
    if (!drag.moved && (abs(e->x_root - drag.start_x) > 5 || abs(e->y_root - drag.start_y) > 5))
        drag.moved = 1;
    drag.target_slot = drag.target_order = -1;
    if (drag.moved && shortcut_over_taskbar(e->x_root, e->y_root)) {
        drag.target_order = shortcut_bar_order(e->x_root);
    } else if (drag.moved && e->y_root >= 0 && e->y_root < screen_h - bar_h) {
        int cols = desktop_cols(), total = desktop_slot_count();
        int rows = total / cols; if (rows < 1) rows = 1;
        int top_x = e->x_root - drag.ox, top_y = e->y_root - drag.oy;
        int col = (top_x - 16 + DESK_COL_W / 2) / DESK_COL_W;
        int row = (top_y - 16 + DESK_ROW_H / 2) / DESK_ROW_H;
        if (col < 0) col = 0;
        if (col >= cols) col = cols - 1;
        if (row < 0) row = 0;
        if (row >= rows) row = rows - 1;
        drag.target_slot = row * cols + col;
    }
    if (old_moved != drag.moved || old_slot != drag.target_slot || old_order != drag.target_order) {
        draw_desktop(); draw_bar();
    }
}

static void shortcut_drag_finish(const XButtonEvent *e)
{
    int source = drag.pin_source, index = drag.pin_index, moved = drag.moved;
    int target_slot = drag.target_slot, target_order = drag.target_order;
    struct pin p;
    if (source == SHORTCUT_BAR && index >= 0 && index < npins) p = pins[index];
    else if (source == SHORTCUT_DESKTOP && index >= 0 && index < ndesk_pins) p = desk_pins[index];
    else { drag.kind = 0; return; }
    drag.kind = 0; drag.c = NULL;
    XUngrabPointer(dpy, CurrentTime);
    bar_press = -1;
    if (!moved) {
        int dbl = shortcut_click.source == source && shortcut_click.index == index &&
                  (unsigned int)(e->time - shortcut_click.time) < 400 &&
                  abs(e->x_root - shortcut_click.x) < 6 &&
                  abs(e->y_root - shortcut_click.y) < 6;
        if (source == SHORTCUT_BAR) {
            bar_flash_pin = index; bar_flash_until = time(NULL) + 2;
            draw_bar();
        } else draw_desktop();
        if (dbl) {
            shortcut_click.source = shortcut_click.index = -1;
            taskbar_launch_pending = 0;
            launch_pin_new(&p);
        } else {
            shortcut_click.source = source; shortcut_click.index = index;
            shortcut_click.x = e->x_root; shortcut_click.y = e->y_root;
            shortcut_click.time = e->time;
            struct client *match = pin_last_client(&p);
            if (match) {
                taskbar_launch_pending = 0;
                activate(match, 1);
            } else if (source == SHORTCUT_BAR) {
                taskbar_pending_pin = p;
                taskbar_pending_since = monotonic_msec();
                taskbar_launch_pending = 1;
            }
        }
        if (source == SHORTCUT_BAR) draw_bar();
        return;
    }
    shortcut_click.source = shortcut_click.index = -1;
    if (source == SHORTCUT_BAR) taskbar_launch_pending = 0;
    int changed = 0;
    if (target_order >= 0) {
        if (source == SHORTCUT_BAR) changed = taskbar_insert(&p, target_order) >= 0;
        else if (taskbar_insert(&p, target_order) >= 0) {
            desktop_remove(index); changed = 1;
        }
    } else if (target_slot >= 0) {
        if (source == SHORTCUT_DESKTOP) {
            desktop_move_slot(index, target_slot); changed = 1;
        } else if (desktop_place(&p, target_slot) >= 0) {
            taskbar_remove(index); changed = 1;
        }
    }
    if (changed) shortcuts_saved("Shortcut layout");
    else { draw_desktop(); draw_bar(); }
}

static void run_core_action(int action)
{
    switch (action) {
    case CORE_TERMINAL: spawn(detect_terminal()); break;
    case CORE_FILES: spawn_files_home(); break;
    case CORE_SYSMON: tool_open(TOOL_SYSMON); break;
    case CORE_SETTINGS: tool_open(TOOL_SETTINGS); break;
    default: break;
    }
}

/* ---------------------------------------------------------------- menus */
enum { MENU_NONE, MENU_POWER, MENU_DESKTOP, MENU_TITLE, MENU_SHORTCUT,
       MENU_CONFIRM = 97, MENU_PICKER = 98, MENU_MESSAGE = 99 };
enum { SC_LAUNCH, SC_ADD_BAR, SC_ADD_DESK, SC_MOVE_BAR, SC_MOVE_DESK, SC_REMOVE_BAR, SC_REMOVE_DESK };
enum { POWER_LOCK, POWER_RESTART_WM, POWER_EXIT_WM, POWER_SUSPEND, POWER_REBOOT, POWER_OFF };
static int menu_kind, menu_n;
static int menu_hover = -1;
static char menu_labels[8][64];
static int menu_actions[8];
static struct pin menu_pin;
static int menu_pin_source;
static Window menu_target;           /* title menu: the client acted on */
static Window menu_item_targets[8];  /* stable window IDs for picker rows */
static int menu_bar_release;         /* ignore release from the opening click */
static int menu_x, menu_y, menu_w = 190;
static int confirm_action = -1, confirm_hover = 0;
static char confirm_title[96], confirm_detail[192], confirm_label[64];

static void draw_menu(void);

/* -------------------------------------------------------------- drawing */
static void text_out(Window w, int x, int y, const char *s, unsigned long fg)
{
    XSetForeground(dpy, gc, fg);
    XDrawString(dpy, w, gc, x, y, s, (int)strlen(s));
}

static int text_w(const char *s) { return XTextWidth(font, s, (int)strlen(s)); }

static void clipped_text(Window w, int x, int y, const char *s, int max_w, unsigned long fg)
{
    if (max_w <= 0) return;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", s ? s : "");
    if (text_w(buf) > max_w) {
        int len = (int)strlen(buf);
        int fitted = 0;
        while (len > 0) {
            len--;
            buf[len] = 0;
            char tmp[260];
            snprintf(tmp, sizeof(tmp), "%s...", buf);
            size_t tmp_len = strlen(tmp);
            if (text_w(tmp) <= max_w && tmp_len < sizeof(buf)) {
                memcpy(buf, tmp, tmp_len + 1);
                fitted = 1;
                break;
            }
        }
        if (!fitted) buf[0] = 0;
    }
    text_out(w, x, y, buf, fg);
}

static void fill(Window w, int x, int y, int width, int height, unsigned long px)
{
    XSetForeground(dpy, gc, px);
    XFillRectangle(dpy, w, gc, x, y, width, height);
}
static void rect(Window w, int x, int y, int width, int height, unsigned long px)
{
    XSetForeground(dpy, gc, px);
    XDrawRectangle(dpy, w, gc, x, y, width - 1, height - 1);
}

static int centered_baseline(int y, int height)
{
    int fh = font->ascent + font->descent;
    return y + (height - fh) / 2 + font->ascent;
}

/* Repaint WM surfaces into a backing store and present them in one copy.
 * Directly clearing and rebuilding a visible taskbar/frame produced a
 * blank flash on every clock tick, hover change and SysMon refresh. */
static Pixmap surface_backing(Window win, Pixmap *pm, int *old_w, int *old_h,
                              int width, int height)
{
    if (*pm && (*old_w != width || *old_h != height)) {
        XFreePixmap(dpy, *pm);
        *pm = None;
    }
    if (!*pm) {
        *pm = XCreatePixmap(dpy, win, width, height, DefaultDepth(dpy, scr_no));
        *old_w = width; *old_h = height;
    }
    return *pm;
}

static void surface_present(Window win, Pixmap pm, int width, int height)
{
    if (pm) XCopyArea(dpy, pm, win, gc, 0, 0, width, height, 0, 0);
}

static void draw_button_glyph(Window w, int x, int y, int kind, unsigned long fg)
{
    /* 0 minimise, 1 maximise, 2 close — SCos draw_win_button() */
    XSetForeground(dpy, gc, fg);
    if (kind == 0) {
        XFillRectangle(dpy, w, gc, x + 4, y + 12, 10, 2);
    } else if (kind == 1) {
        XDrawRectangle(dpy, w, gc, x + 4, y + 4, 9, 9);
    } else {
        XDrawLine(dpy, w, gc, x + 4, y + 4, x + 13, y + 13);
        XDrawLine(dpy, w, gc, x + 13, y + 4, x + 4, y + 13);
    }
}

/* One grow-only scratch pixmap is shared by all frame repaints. Keeping
 * a full-sized backing image per client would needlessly multiply memory
 * use for large browser windows. */
static Pixmap frame_pm = None;
static int frame_pm_w, frame_pm_h;

static Pixmap frame_backing(Window f, int width, int height)
{
    if (frame_pm && (width > frame_pm_w || height > frame_pm_h)) {
        XFreePixmap(dpy, frame_pm);
        frame_pm = None;
    }
    if (!frame_pm) {
        if (width < frame_pm_w) width = frame_pm_w;
        if (height < frame_pm_h) height = frame_pm_h;
        frame_pm = XCreatePixmap(dpy, f, width, height, DefaultDepth(dpy, scr_no));
        frame_pm_w = width; frame_pm_h = height;
    }
    return frame_pm;
}

static void draw_frame(struct client *c)
{
    if (!c || c->minimized || c->fullscreen) return;
    Window f = c->frame;
    int fw = frame_w(c), fh = frame_h(c);
    Pixmap pm = frame_backing(f, fw, fh);
    Drawable d = pm ? pm : f;
    int focused = active_client == c;
    fill(d, 0, 0, fw, fh, PX.win_bg);
    rect(d, 0, 0, fw, fh, PX.main);
    fill(d, 1, 1, fw - 2, TITLE_H - 1, focused ? PX.main : PX.title_off);
    clipped_text(d, 8, 5 + font->ascent, c->title, fw - 96,
                 focused ? PX.title_text : PX.text);
    unsigned long fg = focused ? PX.title_text : PX.text;
    for (int b = 0; b < 3; b++) {
        int bx = fw - 22 - (2 - b) * 22;
        int hover = c == hover_frame_client && b == hover_title_btn;
        /* The old close hover used PX.main, exactly the focused titlebar
         * colour, so it was literally invisible. Inverse hover keeps all
         * three glyphs visible on both focused and unfocused frames. */
        if (hover) fill(d, bx, 3, BTN_SZ, BTN_SZ, PX.title_text);
        draw_button_glyph(d, bx, 4, b, hover ? PX.main : fg);
    }
    /* resize grip, like SCos's diagonal dots */
    XSetForeground(dpy, gc, PX.bar_hi);
    for (int i = 0; i < 4; i++)
        XFillRectangle(dpy, d, gc, fw - 12 + i * 3, fh - 12 + i * 3, 2, 2);
    surface_present(f, pm, fw, fh);
}


/* ------------------------------------------------------------ X11 icons */
/* Icons are decoded once, cached as a colour pixmap plus a 1-bit alpha
 * mask, and copied by the X server thereafter. Desktop icons are at
 * most 24 px, taskbar pins 20 px and Windows-picker icons 16 px: the
 * normal freedesktop sizes, never enlarged. */
static void xicon_release_images(struct xicon *xi)
{
    if (!xi) return;
    if (xi->pix) XFreePixmap(dpy, xi->pix);
    if (xi->mask) XFreePixmap(dpy, xi->mask);
    xi->pix = xi->mask = None;
}

static int xicon_fill(struct xicon *xi, struct rgba_image *img)
{
    if (!img || !img->pixels) return 0;
    if (!rgba_image_scale_fit(img, xi->size, xi->size)) return 0;
    xi->w = img->w; xi->h = img->h;
    xi->pix = XCreatePixmap(dpy, root, xi->w, xi->h, DefaultDepth(dpy, scr_no));
    xi->mask = XCreatePixmap(dpy, root, xi->w, xi->h, 1);
    if (!xi->pix || !xi->mask) { xicon_release_images(xi); return 0; }

    XImage *im = XCreateImage(dpy, DefaultVisual(dpy, scr_no), DefaultDepth(dpy, scr_no),
                              ZPixmap, 0, NULL, xi->w, xi->h, 32, 0);
    if (!im) { xicon_release_images(xi); return 0; }
    im->data = malloc((size_t)im->bytes_per_line * xi->h);
    if (!im->data) { XFree(im); xicon_release_images(xi); return 0; }
    XInitImage(im);
    for (int y = 0; y < xi->h; y++)
        for (int x = 0; x < xi->w; x++) {
            unsigned char *px = img->pixels + ((size_t)y * xi->w + x) * 4;
            XPutPixel(im, x, y, ((unsigned long)px[0] << 16) |
                                ((unsigned long)px[1] << 8) | px[2]);
        }
    XPutImage(dpy, xi->pix, gc, im, 0, 0, 0, 0, xi->w, xi->h);
    XDestroyImage(im);

    GC mgc = XCreateGC(dpy, xi->mask, 0, NULL);
    XSetForeground(dpy, mgc, 0);
    XFillRectangle(dpy, xi->mask, mgc, 0, 0, xi->w, xi->h);
    XSetForeground(dpy, mgc, 1);
    for (int y = 0; y < xi->h; y++)
        for (int x = 0; x < xi->w; x++) {
            unsigned char *px = img->pixels + ((size_t)y * xi->w + x) * 4;
            if (px[3] >= 128) XFillRectangle(dpy, xi->mask, mgc, x, y, 1, 1);
        }
    XFreeGC(dpy, mgc);
    return 1;
}

static struct xicon *xicon_get(const char *spec, int size)
{
    if (!spec || !*spec || size <= 0) return NULL;
    char key[768];
    snprintf(key, sizeof(key), "%d:%s", size, spec);
    for (struct xicon *xi = xicon_cache; xi; xi = xi->next)
        if (!strcmp(xi->key, key)) return xi;

    struct xicon *xi = calloc(1, sizeof(*xi));
    if (!xi) return NULL;
    snprintf(xi->key, sizeof(xi->key), "%s", key);
    xi->size = size;
    char path[1024];
    struct rgba_image img;
    if (icon_resolve(spec, size, path, sizeof(path)) && rgba_image_load(path, &img)) {
        xi->failed = !xicon_fill(xi, &img);
        rgba_image_free(&img);
    } else xi->failed = 1;
    xi->next = xicon_cache;
    xicon_cache = xi;
    return xi;
}

static void draw_xicon(Window dst, struct xicon *xi, int x, int y)
{
    if (!xi || xi->failed || !xi->pix) return;
    if (xi->mask) {
        XSetClipMask(dpy, gc, xi->mask);
        XSetClipOrigin(dpy, gc, x, y);
    }
    XCopyArea(dpy, xi->pix, dst, gc, 0, 0, xi->w, xi->h, x, y);
    if (xi->mask) XSetClipMask(dpy, gc, None);
    return;
}

static int xicon_drawn(struct xicon *xi) { return xi && !xi->failed && xi->pix != None; }

static void xicon_destroy_owned(struct xicon *xi)
{
    if (!xi || !xi->owned) return;
    xicon_release_images(xi);
    free(xi);
}

static void app_exec_basename(const struct desktop_app *a, char *out, int cap)
{
    char cmd[512], first[512];
    exec_strip_field_codes(a->exec, cmd, sizeof(cmd));
    snprintf(first, sizeof(first), "%s", cmd);
    char *sp = strchr(first, ' ');
    if (sp) *sp = 0;
    const char *base = strrchr(first, '/');
    snprintf(out, cap, "%s", base ? base + 1 : first);
    for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
}

static int client_desktop_app(struct client *c)
{
    int best = -1, best_score = 0;
    char classbuf[160];
    snprintf(classbuf, sizeof(classbuf), "%s", c->wclass);
    for (char *tok = strtok(classbuf, " \t-._"); tok; tok = strtok(NULL, " \t-._")) {
        if (strlen(tok) < 2) continue;
        for (int i = 0; i < napps; i++) {
            char execbase[512];
            app_exec_basename(&apps[i], execbase, sizeof(execbase));
            int score = 0;
            if (!strcasecmp(tok, apps[i].id)) score = 100;
            else if (!strcasecmp(tok, execbase)) score = 90;
            else if (apps[i].icon[0] && !strcasecmp(tok, apps[i].icon)) score = 80;
            else if (strlen(apps[i].id) > 2 && strstr(c->wclass, apps[i].id)) score = 50;
            if (score > best_score) { best_score = score; best = i; }
        }
    }
    return best;
}

static int bar_icon_size(int maximum)
{
    int size = bar_h - 16; /* 6 px margins + 1 px borders + breathing room */
    if (size > maximum) size = maximum;
    if (size < 8) size = 8;
    return size;
}

static struct xicon *client_net_icon(struct client *c)
{
    int wanted = bar_icon_size(16);
    Atom type; int fmt; unsigned long n = 0, after = 0; unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, c->win, atoms[A_NET_WM_ICON], 0, 1 << 18, False,
                           XA_CARDINAL, &type, &fmt, &n, &after, &data) != Success || !data)
        return NULL;
    if (fmt != 32 || type != XA_CARDINAL) { XFree(data); return NULL; }
    unsigned long *cards = (unsigned long *)data;
    unsigned long best_off = 0, best_w = 0, best_h = 0, best_area = 0;
    int have = 0;
    for (unsigned long off = 0; off + 2 <= n; ) {
        unsigned long iw = cards[off], ih = cards[off + 1];
        if (iw == 0 || ih == 0 || iw > 512 || ih > 512) break;
        unsigned long count = iw * ih;
        if (off + 2 + count > n) break;
        unsigned long area = count;
        if (!have || (iw >= (unsigned long)wanted && ih >= (unsigned long)wanted &&
                      (best_w < (unsigned long)wanted || area < best_area)) ||
            (best_w < (unsigned long)wanted && area > best_area)) {
            best_off = off; best_w = iw; best_h = ih; best_area = area; have = 1;
        }
        off += 2 + count;
    }
    if (!have) { XFree(data); return NULL; }
    struct rgba_image img;
    img.w = (int)best_w; img.h = (int)best_h;
    img.pixels = malloc((size_t)img.w * img.h * 4);
    if (!img.pixels) { XFree(data); return NULL; }
    for (unsigned long i = 0; i < best_w * best_h; i++) {
        unsigned long v = cards[best_off + 2 + i];
        img.pixels[i * 4 + 0] = (unsigned char)(v >> 16);
        img.pixels[i * 4 + 1] = (unsigned char)(v >> 8);
        img.pixels[i * 4 + 2] = (unsigned char)v;
        img.pixels[i * 4 + 3] = (unsigned char)(v >> 24);
    }
    XFree(data);
    struct xicon *xi = calloc(1, sizeof(*xi));
    if (!xi) { rgba_image_free(&img); return NULL; }
    xi->size = wanted; xi->owned = 1;
    snprintf(xi->key, sizeof(xi->key), "client:0x%lx", c->win);
    if (!xicon_fill(xi, &img)) { xicon_destroy_owned(xi); xi = NULL; }
    rgba_image_free(&img);
    return xi;
}

static struct xicon *client_load_icon(struct client *c)
{
    int app = client_desktop_app(c);
    if (app >= 0) {
        const char *spec = apps[app].icon[0] ? apps[app].icon : apps[app].id;
        struct xicon *xi = xicon_get(spec, bar_icon_size(16));
        if (xicon_drawn(xi)) return xi;
    }
    return client_net_icon(c);
}

/* ---------------------------------------------------------------- desktop */
static void icon_rect(int i, int *x, int *y)
{
    int slot = i >= 0 && i < ndesk_pins && desk_pins[i].slot >= 0 ? desk_pins[i].slot : i;
    int cols = desktop_cols();
    *x = 16 + (slot % cols) * DESK_COL_W;
    *y = 16 + (slot / cols) * DESK_ROW_H;
}

static int desktop_right_rule(void)
{
    int x = screen_w > DESK_EDGE_GAP + 3 ? screen_w - DESK_EDGE_GAP - 1 : screen_w - 2;
    return x < 1 ? 1 : x;
}

static void paint_wallpaper(Drawable d, int width, int height)
{
    for (int y = 0; y < height; y++) {
        unsigned int rgb = color_blend_rgb(theme.bg_top, theme.bg_bot,
                                           height > 1 ? y * 255 / (height - 1) : 0);
        XSetForeground(dpy, gc, alloc_rgb(rgb));
        XFillRectangle(dpy, d, gc, 0, y, width, 1);
    }
    if (cfg.desktop_grid) {
        XSetForeground(dpy, gc, PX.grid);
        for (int x = 0; x < width; x += 64) XDrawLine(dpy, d, gc, x, 0, x, height);
        for (int y = 0; y < height; y += 64) XDrawLine(dpy, d, gc, 0, y, width, y);
    }
}

static void paint_desktop(Drawable d)
{
    /* Paint everything into the supplied drawable. When draw_desktop()
     * passes its wallpaper pixmap, the gradient, grid and icons must all
     * land in that pixmap: painting any layer straight onto the root is
     * then immediately covered by the pixmap copy, which both hid the
     * grid and made every repaint visibly flicker. */
    paint_wallpaper(d, screen_w, screen_h);
    /* Keep a wallpaper fallback beneath the dedicated two-pixel edge
     * surface. The click-through surface keeps the rule above app frames
     * and independent of root background restoration. */
    int desktop_h = screen_h - bar_h;
    int right_rule = desktop_right_rule();
    rect(d, 0, 0, right_rule + 1, desktop_h, PX.main);
    fill(d, right_rule, 0, 2, desktop_h, PX.main);

    /* Independent desktop shortcuts on a persistent snap grid. */
    int icon_size = cfg.desktop_icon_size;
    for (int i = 0; i < ndesk_pins; i++) {
        int x, y; icon_rect(i, &x, &y);
        if (desk_sel_mask & (1ul << i)) {
            fill(d, x, y, 80, 88, alloc_rgb(color_blend_rgb(theme.bg_top, theme.main, 30)));
            rect(d, x, y, 80, 88, PX.main);
        } else if (i == desk_hover) {
            fill(d, x, y, 80, 88, alloc_rgb(color_blend_rgb(theme.bg_top, theme.main, 16)));
            rect(d, x, y, 80, 88, PX.title_off);
        }
        int gx = x + (80 - icon_size) / 2, gy = y + 8;
        struct xicon *xi = xicon_get(desk_pins[i].icon, icon_size);
        if (xicon_drawn(xi)) {
            draw_xicon(d, xi, gx + (icon_size - xi->w) / 2, gy + (icon_size - xi->h) / 2);
        } else {
            rect(d, gx, gy, icon_size, icon_size, PX.main);
            int gw = text_w(desk_pins[i].glyph);
            text_out(d, gx + (icon_size - gw) / 2,
                     gy + (icon_size - font->ascent - font->descent) / 2 + font->ascent,
                     desk_pins[i].glyph, PX.main);
        }
        int lw = text_w(desk_pins[i].label);
        if (lw > 72) lw = 72;
        clipped_text(d, x + (80 - lw) / 2, y + 16 + icon_size + font->ascent,
                     desk_pins[i].label, 72, PX.text);
    }
    if (drag.kind == 3 && drag.moved) {
        int x0 = drag.start_x < drag.band_x ? drag.start_x : drag.band_x;
        int y0 = drag.start_y < drag.band_y ? drag.start_y : drag.band_y;
        int x1 = drag.start_x > drag.band_x ? drag.start_x : drag.band_x;
        int y1 = drag.start_y > drag.band_y ? drag.start_y : drag.band_y;
        if (x1 > x0 && y1 > y0) {
            fill(d, x0, y0, x1 - x0, y1 - y0,
                 alloc_rgb(color_blend_rgb(theme.bg_top, theme.main, 18)));
            rect(d, x0, y0, x1 - x0, y1 - y0, PX.main);
        }
    }
    if (drag.kind == 4 && drag.moved && drag.target_slot >= 0) {
        int cols = desktop_cols();
        int x = 16 + (drag.target_slot % cols) * DESK_COL_W;
        int y = 16 + (drag.target_slot / cols) * DESK_ROW_H;
        fill(d, x, y, 80, 88, alloc_rgb(color_blend_rgb(theme.bg_top, theme.main, 24)));
        rect(d, x, y, 80, 88, PX.main);
    }
}

static Pixmap desk_pm = None;
static int desk_pm_w, desk_pm_h;

/* Paint the wallpaper into a pixmap and install it as the root
 * background, so the X server itself restores the desktop instantly
 * when windows move or close. Painting straight onto the root (the
 * old way) left ghost images of closed windows until an Expose
 * repaint crawled down the screen, and flickered while doing it. */
static void draw_desktop(void)
{
    if (desk_pm && (desk_pm_w != screen_w || desk_pm_h != screen_h)) {
        XFreePixmap(dpy, desk_pm);
        desk_pm = None;
    }
    if (!desk_pm) {
        desk_pm = XCreatePixmap(dpy, root, screen_w, screen_h, DefaultDepth(dpy, scr_no));
        desk_pm_w = screen_w; desk_pm_h = screen_h;
    }
    if (desk_pm) {
        paint_desktop(desk_pm);
        XSetWindowBackgroundPixmap(dpy, root, desk_pm);
        XCopyArea(dpy, desk_pm, root, gc, 0, 0, screen_w, screen_h, 0, 0);
    } else {
        paint_desktop(root);
    }
    restack_right_rule();
}

static void expose_desktop(void)
{
    if (desk_pm && desk_pm_w == screen_w && desk_pm_h == screen_h)
        XCopyArea(dpy, desk_pm, root, gc, 0, 0, screen_w, screen_h, 0, 0);
    else
        draw_desktop();
}

static Pixmap edge_pm;
static int edge_pm_w, edge_pm_h;
static void draw_right_rule(void)
{
    if (!edge_win) return;
    Pixmap pm = surface_backing(edge_win, &edge_pm, &edge_pm_w, &edge_pm_h,
                                DESK_RULE_W, screen_h - bar_h);
    Drawable d = pm ? pm : edge_win;
    fill(d, 0, 0, DESK_RULE_W, screen_h - bar_h, PX.main);
    surface_present(edge_win, pm, DESK_RULE_W, screen_h - bar_h);
}

static void restack_right_rule(void)
{
    if (!edge_win || !bar_win) return;
    XWindowChanges wc;
    wc.sibling = bar_win;
    wc.stack_mode = Below;
    XConfigureWindow(dpy, edge_win, CWSibling | CWStackMode, &wc);
    draw_right_rule();
}

/* ------------------------------------------------------------------- bar */
struct bar_item { int idx; int x, w; };
static struct bar_item bar_items[96];
static int bar_n;

static int bar_clock_x(void)
{
    return screen_w - BAR_CLOCK_W;
}

static int bar_date_x(void)
{
    return bar_clock_x() - BAR_DATE_W;
}

static int bar_power_x(void)
{
    /* The date consumes most of the old empty clock gap, so Wi-Fi and
     * power move left only 16 px at 800 px rather than losing a wide strip. */
    return bar_date_x() - BAR_SECTION_GAP - 34;
}

static int bar_wifi_x(void)
{
    return bar_power_x() - 40;
}

static void bar_layout(void)
{
    bar_n = 0;
    int x = 168;
    int end = bar_wifi_x() - 8;
    if (end < x) end = x;

    /* The main bar contains permanent shortcuts only. Running windows
     * live exclusively in the Windows picker, as in the original SCos;
     * launching an app must not replace its pin or add another box. */
    for (int i = 0; i < npins && bar_n < 96; i++) {
        if (x + 34 > end) break;
        bar_items[bar_n].idx = i;
        bar_items[bar_n].x = x; bar_items[bar_n].w = 34; bar_n++;
        x += 40;
    }
}

static Pixmap bar_pm = None;
static int bar_pm_w, bar_pm_h;

static void draw_compact_date(Drawable d, const struct tm *tmv)
{
    char month_day[16], year[16];
    snprintf(month_day, sizeof(month_day), "%02d/%02d", tmv->tm_mon + 1, tmv->tm_mday);
    snprintf(year, sizeof(year), "%04d", tmv->tm_year + 1900);
    /* Use the very same core font as the clock and every other bar label.
     * The normal 13/14 px SCos fonts fit two rows even at bar_h = 28. */
    int line_h = font->ascent + font->descent;
    int y = (bar_h - line_h * 2) / 2;
    if (y < 0) y = 0;
    int md_w = text_w(month_day), year_w = text_w(year);
    text_out(d, bar_date_x() + (BAR_DATE_W - md_w) / 2, y + font->ascent,
             month_day, PX.main);
    text_out(d, bar_date_x() + (BAR_DATE_W - year_w) / 2,
             y + line_h + font->ascent, year, PX.main);
}

static void draw_bar(void)
{
    if (!bar_win) return;
    Pixmap pm = surface_backing(bar_win, &bar_pm, &bar_pm_w, &bar_pm_h,
                                screen_w, bar_h);
    Drawable d = pm ? pm : bar_win;
    fill(d, 0, 0, screen_w, bar_h, PX.taskbar_bg);
    fill(d, 0, 0, screen_w, 1, PX.main);
    int by = 6, bh = bar_h - 12;
    /* launcher button: the 2x2 SCos grid */
    int launch_lit = launch_open || bar_hover == -2;
    fill(d, 8, by, 34, bh, launch_lit ? PX.main : PX.taskbar_bg);
    rect(d, 8, by, 34, bh, PX.main);
    int cell_h = bh < 24 ? 4 : 6;
    int grid_h = cell_h * 2 + 3;
    int grid_x = 8 + (34 - 19) / 2, grid_y = by + (bh - grid_h) / 2;
    for (int q = 0; q < 4; q++)
        fill(d, grid_x + (q % 2) * 11, grid_y + (q / 2) * (cell_h + 3), 8, cell_h,
             launch_lit ? PX.title_text : PX.main);
    /* Windows N picker */
    char wl[32]; snprintf(wl, sizeof(wl), "Windows %d", nclients);
    int windows_lit = overview_active || bar_hover == -3;
    fill(d, 48, by, 110, bh, windows_lit ? PX.main : PX.taskbar_bg);
    rect(d, 48, by, 110, bh, PX.main);
    clipped_text(d, 56, centered_baseline(by, bh), wl, 94, windows_lit ? PX.title_text : PX.text);

    bar_layout();
    for (int i = 0; i < bar_n; i++) {
        struct bar_item *it = &bar_items[i];
        struct pin *p = &pins[it->idx];
        int lit = bar_press == i || bar_hover == i ||
                  (bar_flash_pin == it->idx && time(NULL) <= bar_flash_until);
        fill(d, it->x, by, 34, bh, lit ? PX.main : PX.taskbar_bg);
        rect(d, it->x, by, 34, bh, PX.main);
        struct xicon *xi = xicon_get(p->icon, bar_icon_size(20));
        if (xicon_drawn(xi))
            draw_xicon(d, xi, it->x + (34 - xi->w) / 2, by + (bh - xi->h) / 2);
        else {
            int gw = text_w(p->glyph);
            text_out(d, it->x + (34 - gw) / 2, centered_baseline(by, bh), p->glyph,
                     lit ? PX.title_text : PX.text);
        }
    }
    if (drag.kind == 4 && drag.moved && drag.target_order >= 0) {
        int drop_x = 168 + drag.target_order * 40 - 3;
        int drop_end = bar_wifi_x() - 10;
        if (drop_x > drop_end) drop_x = drop_end;
        fill(d, drop_x, by, 2, bh, PX.main);
    }
    /* Wi-Fi status: native adapter state, with per-device details and
     * NetworkManager actions in the panel opened by this button. */
    int wx = bar_wifi_x();
    int wifi_lit = bar_hover == -5;
    fill(d, wx, by, 34, bh, wifi_lit ? PX.main : PX.taskbar_bg);
    rect(d, wx, by, 34, bh, PX.main);
    unsigned long wifi_fg = wifi_lit ? PX.title_text :
        (wifi_connected_count || wifi_hotspot_count ? PX.main : PX.title_off);
    XSetForeground(dpy, gc, wifi_fg);
    /* Every arc shares the dot's centre. The old independently positioned
     * arcs left a large gap (and at 28 px could even cross the dot). */
    int wifi_radius = bh - 4;
    if (wifi_radius > 12) wifi_radius = 12;
    if (wifi_radius < 6) wifi_radius = 6;
    int top = by + (bh - (wifi_radius + 4)) / 2;
    int wifi_cx = wx + 17, wifi_cy = top + wifi_radius;
    int middle = wifi_radius * 2 / 3, inner = wifi_radius / 3;
    XDrawArc(dpy, d, gc, wifi_cx - wifi_radius, wifi_cy - wifi_radius,
             wifi_radius * 2, wifi_radius * 2, 45 * 64, 90 * 64);
    XDrawArc(dpy, d, gc, wifi_cx - middle, wifi_cy - middle, middle * 2, middle * 2, 45 * 64, 90 * 64);
    XDrawArc(dpy, d, gc, wifi_cx - inner, wifi_cy - inner, inner * 2, inner * 2, 45 * 64, 90 * 64);
    XFillRectangle(dpy, d, gc, wifi_cx - 2, wifi_cy - 1, 4, 4);
    if (!wifi_adapter_count)
        XDrawLine(dpy, d, gc, wx + 7, by + bh - 5, wx + 27, by + 4);

    /* power button */
    int pw = 34, px = bar_power_x();
    int power_lit = bar_hover == -4;
    fill(d, px, by, pw, bh, power_lit ? PX.main : PX.taskbar_bg);
    rect(d, px, by, pw, bh, PX.main);
    XSetForeground(dpy, gc, power_lit ? PX.title_text : PX.main);
    int radius = (bh - 4) / 2;
    if (radius > 7) radius = 7;
    if (radius < 3) radius = 3;
    int cx = px + pw / 2, cy = by + bh / 2 + 1;
    /* A 270-degree arc centred on 90 degrees leaves its opening at the
     * top, around the vertical stroke (0 degrees is the right side). */
    XDrawArc(dpy, d, gc, cx - radius, cy - radius, radius * 2, radius * 2,
             135 * 64, 270 * 64);
    XDrawLine(dpy, d, gc, cx, cy - radius - 1, cx, cy);
    /* clock */
    time_t now = time(NULL);
    struct tm tmv; localtime_r(&now, &tmv);
    char clock[16];
    snprintf(clock, sizeof(clock), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    draw_compact_date(d, &tmv);
    text_out(d, screen_w - text_w(clock) - 10, centered_baseline(0, bar_h), clock, PX.main);
    surface_present(bar_win, pm, screen_w, bar_h);
}

/* Short, synchronous minimise/restore transitions. Only a lightweight
 * placeholder changes size: resizing the real client six times made web
 * engines redraw a tiny page and then spend much longer scaling it back. */
static int taskbar_rect_for_client(struct client *c, int *x, int *y, int *w, int *h)
{
    /* Running windows no longer own taskbar boxes. Both transitions use
     * the stable Windows-picker target, so animation and client scaling
     * do not depend on a temporary running-window layout. */
    (void)c;
    if (!bar_win) return 0;
    *x = 48; *y = screen_h - bar_h + 6;
    *w = 110; *h = bar_h - 12;
    return 1;
}

static Pixmap anim_pm = None;
static int anim_pm_w, anim_pm_h;

static void draw_animation(struct client *c, int w, int h)
{
    Pixmap pm = surface_backing(anim_win, &anim_pm, &anim_pm_w, &anim_pm_h, w, h);
    Drawable d = pm ? pm : anim_win;
    fill(d, 0, 0, w, h, PX.win_bg);
    rect(d, 0, 0, w, h, PX.main);
    if (w > 2 && h > 2) fill(d, 1, 1, w - 2, h - 2 < TITLE_H - 1 ? h - 2 : TITLE_H - 1, PX.main);
    if (w >= 48 && h >= TITLE_H)
        clipped_text(d, 6, 5 + font->ascent, c->title, w - 12, PX.title_text);
    surface_present(anim_win, pm, w, h);
}

static void animate_frame_between(struct client *c,
                                  int sx, int sy, int sw, int sh,
                                  int tx, int ty, int tw, int th)
{
    for (int step = 1; step <= 6; step++) {
        int x = sx + (tx - sx) * step / 6;
        int y = sy + (ty - sy) * step / 6;
        int w = sw + (tw - sw) * step / 6;
        int h = sh + (th - sh) * step / 6;
        if (w < 4) w = 4;
        if (h < 4) h = 4;
        XMoveResizeWindow(dpy, anim_win, x, y, w, h);
        draw_animation(c, w, h);
        XFlush(dpy);
        usleep(12000);
    }
}

static void animation_show(struct client *c, int x, int y, int w, int h)
{
    XMoveResizeWindow(dpy, anim_win, x, y, w, h);
    draw_animation(c, w, h);
    XMapWindow(dpy, anim_win);
    XWindowChanges wc;
    wc.sibling = bar_win;
    wc.stack_mode = Below;
    XConfigureWindow(dpy, anim_win, CWSibling | CWStackMode, &wc);
}

static void animate_minimize(struct client *c)
{
    int tx, ty, tw, th;
    if (!c || c->fullscreen || !taskbar_rect_for_client(c, &tx, &ty, &tw, &th)) return;
    int sx = c->x, sy = c->y, sw = frame_w(c), sh = frame_h(c);
    XUnmapWindow(dpy, c->frame);
    animation_show(c, sx, sy, sw, sh);
    animate_frame_between(c, sx, sy, sw, sh, tx, ty, tw, th);
    XUnmapWindow(dpy, anim_win);
}

static void animate_restore(struct client *c)
{
    int tx, ty, tw, th;
    if (!c || c->fullscreen || !taskbar_rect_for_client(c, &tx, &ty, &tw, &th)) {
        XMapWindow(dpy, c->win);
        XMapWindow(dpy, c->frame);
        return;
    }
    int fx = c->x, fy = c->y, fw = frame_w(c), fh = frame_h(c);
    animation_show(c, tx, ty, tw, th);
    animate_frame_between(c, tx, ty, tw, th, fx, fy, fw, fh);
    /* The real frame/client kept their geometry; only map them again. */
    draw_frame(c);
    XMapWindow(dpy, c->win);
    XMapWindow(dpy, c->frame);
    XUnmapWindow(dpy, anim_win);
}

/* ------------------------------------------------------ launcher window */
static Pixmap launch_pm = None;
static int launch_pm_w, launch_pm_h;

static void draw_launcher(void)
{
    if (!launch_open) return;
    int h = 308, y = screen_h - bar_h - h - 4;
    XMoveResizeWindow(dpy, launch_win, 8, y, 300, h);
    Pixmap pm = surface_backing(launch_win, &launch_pm, &launch_pm_w, &launch_pm_h, 300, h);
    Drawable d = pm ? pm : launch_win;
    fill(d, 0, 0, 300, h, PX.win_bg);
    rect(d, 0, 0, 300, h, PX.main);
    text_out(d, 10, 8 + font->ascent, launch_pin_mode ? "App placement: click an app" : launch_all && !launch_q[0] ? "All installed apps - Esc goes back" : launch_q[0] ? "Search - right-click for placement" : "Core apps - type to search all", PX.main);
    rect(d, 10, 26, 280, 20, PX.main);
    text_out(d, 14, 30 + font->ascent, launch_q, PX.text);
    int caret_x = 14 + text_w(launch_q);
    if ((time(NULL) % 2) == 0) fill(d, caret_x, 30, 2, 12, PX.main);
    int n = launch_rebuild();
    int rows = (h - 64) / 24; if (rows > 10) rows = 10;
    if (launch_sel < launch_offset) launch_offset = launch_sel;
    if (launch_sel >= launch_offset + rows) launch_offset = launch_sel - rows + 1;
    int yy = 56;
    for (int r = 0; r < rows && launch_offset + r < n; r++) {
        struct launch_item *it = &launch_items[launch_offset + r];
        int selected = launch_offset + r == launch_sel;
        if (selected) fill(d, 6, yy - 3, 288, 22, PX.main);
        struct xicon *xi = xicon_get(it->icon, 20);
        if (xicon_drawn(xi)) draw_xicon(d, xi, 10, yy - 1);
        else text_out(d, 12, yy + 4 + font->ascent, it->glyph, selected ? PX.title_text : PX.main);
        /* A bright selection with dark text is actually distinguishable;
         * either green text colour on the old green row was not. */
        int pin_mask = it->app_idx >= 0 ? app_pin_mask(it->app_idx) : 0;
        clipped_text(d, 44, yy + 4 + font->ascent, it->name, pin_mask ? 205 : 248,
                     selected ? PX.title_text : PX.text);
        if (pin_mask)
            text_out(d, 262, yy + 4 + font->ascent, pin_mask == 3 ? "T+D" : pin_mask == 1 ? "T" : "D",
                     selected ? PX.title_text : PX.main);
        yy += 24;
    }
    if (!n) text_out(d, 14, yy + font->ascent, "(no core or installed app matches)", PX.title_off);
    surface_present(launch_win, pm, 300, h);
    XRaiseWindow(dpy, launch_win);
}

static void launcher_toggle(int open)
{
    if (open && !launch_open) {
        launch_q[0] = 0; launch_offset = 0; launch_sel = 0; launch_all = 0;
        launch_rebuild();
        XMapWindow(dpy, launch_win);
        launch_open = 1;
        XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime);
        draw_launcher();
    } else if (!open && launch_open) {
        launch_open = 0; launch_pin_mode = 0;
        XUnmapWindow(dpy, launch_win);
        XUngrabKeyboard(dpy, CurrentTime);
        draw_bar();
    }
}

/* -------------------------------------------------------- menu window */
static void menu_close(void)
{
    if (menu_kind == MENU_NONE) return;
    menu_kind = MENU_NONE;
    menu_hover = -1;
    menu_bar_release = 0;
    XUnmapWindow(dpy, menu_win);
    XUngrabKeyboard(dpy, CurrentTime);
}

static void menu_open(int kind, int x, int y, Window target)
{
    menu_close();
    menu_kind = kind; menu_target = target;
    menu_hover = -1;
    if (kind == MENU_POWER) {
        static const char *items[] = { "Lock", "Restart WM...", "Exit WM...", "Suspend...", "Reboot...", "Power Off..." };
        menu_n = 6;
        for (int i = 0; i < menu_n; i++) snprintf(menu_labels[i], 64, "%s", items[i]);
    } else if (kind == MENU_DESKTOP) {
        static const char *items[] = { "Terminal", "Files", "SysMon", "Settings",
                                       "Add/Remove App...", "App Launcher", "Restart WM", "Exit WM" };
        menu_n = 8;
        for (int i = 0; i < menu_n; i++) snprintf(menu_labels[i], 64, "%s", items[i]);
    } else {
        static const char *items[] = { "Minimise", "Maximise / Restore", "Close" };
        menu_n = 3;
        for (int i = 0; i < menu_n; i++) snprintf(menu_labels[i], 64, "%s", items[i]);
    }
    menu_w = 190;
    menu_x = x; menu_y = y;
    int w = menu_w, h = menu_n * 24 + 6;
    if (menu_x + w > screen_w) menu_x = screen_w - w - 2;
    if (menu_y + h > screen_h - bar_h) menu_y = screen_h - bar_h - h - 2;
    if (menu_y < 2) menu_y = 2;
    XMoveResizeWindow(dpy, menu_win, menu_x, menu_y, w, h);
    XMapWindow(dpy, menu_win);
    XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    draw_menu();
    menu_bar_release = 1;
}

static void shortcut_menu_item(const char *label, int action)
{
    if (menu_n >= 8) return;
    snprintf(menu_labels[menu_n], 64, "%s", label);
    menu_actions[menu_n] = action;
    menu_n++;
}

static void menu_open_shortcut(const struct pin *p, int source, int x, int y)
{
    if (!p) return;
    menu_close();
    menu_kind = MENU_SHORTCUT;
    menu_target = None;
    menu_pin = *p;
    menu_pin_source = source;
    menu_hover = -1; menu_n = 0;
    int on_bar = pin_list_find(pins, npins, p) >= 0;
    int on_desktop = pin_list_find(desk_pins, ndesk_pins, p) >= 0;
    shortcut_menu_item("Launch", SC_LAUNCH);
    if (source != SHORTCUT_BAR) {
        shortcut_menu_item(on_bar ? "Remove from taskbar" : "Add to taskbar",
                           on_bar ? SC_REMOVE_BAR : SC_ADD_BAR);
        if (source == SHORTCUT_DESKTOP) shortcut_menu_item("Move to taskbar", SC_MOVE_BAR);
    }
    if (source != SHORTCUT_DESKTOP) {
        shortcut_menu_item(on_desktop ? "Remove from desktop" : "Add to desktop",
                           on_desktop ? SC_REMOVE_DESK : SC_ADD_DESK);
        if (source == SHORTCUT_BAR) shortcut_menu_item("Move to desktop", SC_MOVE_DESK);
    }
    if (source == SHORTCUT_DESKTOP) shortcut_menu_item("Remove from desktop", SC_REMOVE_DESK);
    else if (source == SHORTCUT_BAR) shortcut_menu_item("Remove from taskbar", SC_REMOVE_BAR);

    menu_w = 220; menu_x = x; menu_y = y;
    int h = menu_n * 24 + 6;
    if (menu_x + menu_w > screen_w) menu_x = screen_w - menu_w - 2;
    if (menu_x < 2) menu_x = 2;
    if (menu_y + h > screen_h - bar_h) menu_y = screen_h - bar_h - h - 2;
    if (menu_y < 2) menu_y = 2;
    XMoveResizeWindow(dpy, menu_win, menu_x, menu_y, menu_w, h);
    XMapWindow(dpy, menu_win);
    XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    draw_menu();
    menu_bar_release = 1;
}

static Pixmap menu_pm = None;
static int menu_pm_w, menu_pm_h;

static void draw_menu(void)
{
    if (menu_kind == MENU_NONE) return;
    int w = menu_w, h = menu_kind == MENU_CONFIRM ? 126 : menu_n * 24 + 6;
    Pixmap pm = surface_backing(menu_win, &menu_pm, &menu_pm_w, &menu_pm_h, w, h);
    Drawable d = pm ? pm : menu_win;
    fill(d, 0, 0, w, h, PX.win_bg);
    rect(d, 0, 0, w, h, PX.main);
    if (menu_kind == MENU_CONFIRM) {
        clipped_text(d, 16, 14 + font->ascent, confirm_title, w - 32, PX.main);
        clipped_text(d, 16, 42 + font->ascent, confirm_detail, w - 32, PX.text);
        int by = 86;
        fill(d, 12, by, 120, 28, confirm_hover == 0 ? PX.main : PX.win_bg);
        rect(d, 12, by, 120, 28, PX.main);
        clipped_text(d, 24, by + 6 + font->ascent, "Cancel", 96,
                     confirm_hover == 0 ? PX.title_text : PX.text);
        fill(d, 188, by, 180, 28, confirm_hover == 1 ? PX.main : PX.win_bg);
        rect(d, 188, by, 180, 28, PX.main);
        clipped_text(d, 200, by + 6 + font->ascent, confirm_label, 156,
                     confirm_hover == 1 ? PX.title_text : PX.text);
        surface_present(menu_win, pm, w, h);
        XRaiseWindow(dpy, menu_win);
        return;
    }
    for (int i = 0; i < menu_n; i++) {
        int row_y = 3 + i * 24;
        if (i == menu_hover) fill(d, 2, row_y, w - 4, 24, PX.main);
        int text_x = 10;
        if (menu_kind == MENU_PICKER) {
            /* Keep the real app icon with its window now that the main
             * taskbar no longer has running-window buttons. The stored
             * window ID may have closed; a stale row simply has no icon. */
            struct client *c = find_client(menu_item_targets[i]);
            if (c && xicon_drawn(c->icon))
                draw_xicon(d, c->icon, 8, row_y + (24 - c->icon->h) / 2);
            text_x = 34;
        }
        clipped_text(d, text_x, row_y + 4 + font->ascent, menu_labels[i], w - text_x - 10,
                     i == menu_hover ? PX.title_text : PX.text);
    }
    surface_present(menu_win, pm, w, h);
    XRaiseWindow(dpy, menu_win);
}

static void wm_notify_no_windows(void)
{
    /* SCos showed a one-item "No open windows" menu; do the same */
    menu_kind = MENU_MESSAGE;
    menu_n = 1;
    snprintf(menu_labels[0], 64, "No open windows");
    menu_x = 48; menu_y = screen_h - bar_h - 32;
    menu_w = 300;
    XMoveResizeWindow(dpy, menu_win, menu_x, menu_y, menu_w, 30);
    XMapWindow(dpy, menu_win);
    XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    draw_menu();
}

/* ------------------------------------------------------------- interaction */
static struct { unsigned int button; int x, y; unsigned long t; int icon; } last_click;

static int title_button_at(struct client *c, int rx, int ry)
{
    if (ry < 0 || ry >= TITLE_H) return -1;
    int fw = frame_w(c);
    for (int b = 0; b < 3; b++) {
        int bx = fw - 22 - (2 - b) * 22;
        if (rx >= bx && rx < bx + BTN_SZ && ry >= 3 && ry < 3 + BTN_SZ) return b;
    }
    return -1;
}

/* Resize zones in frame coordinates: a 7 px band inside every edge and
 * corner of the frame. The old code only recognised an 18 px grip that
 * was tested on frame events, but the frame border is 1 px wide, so a
 * pointer over the visible edge is almost always over the *client*
 * window and the grip was effectively unreachable. Testing the client
 * area too (the caller passes client presses with the frame offset
 * already added) makes the edges real. */
static int resize_edges_at(struct client *c, int rx, int ry)
{
    if (c->maximized || c->fullscreen) return 0;
    if (title_button_at(c, rx, ry) >= 0) return 0;
    int fw = frame_w(c), fh = frame_h(c);
    int edges = 0;
    if (rx >= 0 && rx < 7) edges |= 1;
    if (rx >= fw - 7 && rx < fw) edges |= 2;
    if (ry >= 0 && ry < 5) edges |= 4;
    if (ry >= fh - 7 && ry < fh) edges |= 8;
    return edges;
}

static void power_perform(int action)
{
    switch (action) {
    case POWER_LOCK: {
        static const char *const lockers[] = { "slock", "i3lock", "xlock", "xscreensaver-command -lock", NULL };
        for (int i = 0; lockers[i]; i++) {
            char first[64]; snprintf(first, sizeof(first), "%s", lockers[i]);
            char *sp = strchr(first, ' '); if (sp) *sp = 0;
            if (cmd_exists(first)) { spawn(lockers[i]); break; }
        }
        break; }
    case POWER_RESTART_WM:
        while (nclients) remove_client(clients[0], 1);
        execvp("/proc/self/exe", saved_argv);
        break;
    case POWER_EXIT_WM: running = 0; break;
    case POWER_SUSPEND: spawn("systemctl suspend"); break;
    case POWER_REBOOT: spawn("systemctl reboot"); break;
    case POWER_OFF: spawn("systemctl poweroff"); break;
    }
}

static void power_request(int action)
{
    if (action == POWER_LOCK) { power_perform(action); return; }
    const char *title = "", *detail = "", *label = "";
    switch (action) {
    case POWER_RESTART_WM:
        title = "Restart SCosWM?"; detail = "Windows are preserved during restart."; label = "Restart WM"; break;
    case POWER_EXIT_WM:
        title = "Exit SCosWM?"; detail = "Session apps and unsaved work may close."; label = "Exit WM"; break;
    case POWER_SUSPEND:
        title = "Suspend this computer?"; detail = "Save work before suspending this PC."; label = "Suspend"; break;
    case POWER_REBOOT:
        title = "Reboot this computer?"; detail = "Unsaved work will be lost."; label = "Reboot"; break;
    case POWER_OFF:
        title = "Power off this computer?"; detail = "Unsaved work will be lost."; label = "Power Off"; break;
    default: return;
    }
    menu_close();
    menu_kind = MENU_CONFIRM; menu_n = 2; menu_hover = -1;
    confirm_action = action; confirm_hover = 0;
    snprintf(confirm_title, sizeof(confirm_title), "%s", title);
    snprintf(confirm_detail, sizeof(confirm_detail), "%s", detail);
    snprintf(confirm_label, sizeof(confirm_label), "%s", label);
    menu_w = 380; menu_x = (screen_w - menu_w) / 2;
    menu_y = (screen_h - bar_h - 126) / 2;
    if (menu_y < 8) menu_y = 8;
    XMoveResizeWindow(dpy, menu_win, menu_x, menu_y, menu_w, 126);
    XMapWindow(dpy, menu_win);
    XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    menu_bar_release = 0; /* opened from another menu's release, not a new press */
    draw_menu();
}

static void menu_action(int idx)
{
    int kind = menu_kind;
    Window target = menu_target;
    menu_close();
    if (kind == MENU_POWER) {
        if (idx >= POWER_LOCK && idx <= POWER_OFF) power_request(idx);
    } else if (kind == MENU_DESKTOP) {
        switch (idx) {
        case 0: run_core_action(CORE_TERMINAL); break;
        case 1: run_core_action(CORE_FILES); break;
        case 2: run_core_action(CORE_SYSMON); break;
        case 3: run_core_action(CORE_SETTINGS); break;
        case 4: launch_pin_mode = 1; launcher_toggle(1); break;
        case 5: launcher_toggle(1); break;
        case 6: power_request(POWER_RESTART_WM); break;
        case 7: power_request(POWER_EXIT_WM); break;
        }
    } else if (kind == MENU_SHORTCUT) {
        struct pin p = menu_pin;
        int source = menu_pin_source;
        int action = idx >= 0 && idx < 8 ? menu_actions[idx] : -1;
        if (action == SC_LAUNCH) {
            if (launch_open) launcher_toggle(0);
            launch_pin_value(&p);
            return;
        }
        int bar_index = pin_list_find(pins, npins, &p);
        int desk_index = pin_list_find(desk_pins, ndesk_pins, &p);
        int changed = 0;
        if (action == SC_ADD_BAR) changed = taskbar_insert(&p, -1) >= 0;
        else if (action == SC_ADD_DESK) changed = desktop_place(&p, -1) >= 0;
        else if (action == SC_MOVE_BAR && taskbar_insert(&p, -1) >= 0) {
            if (source == SHORTCUT_DESKTOP) desktop_remove(desk_index);
            changed = 1;
        } else if (action == SC_MOVE_DESK && desktop_place(&p, -1) >= 0) {
            if (source == SHORTCUT_BAR) taskbar_remove(bar_index);
            changed = 1;
        } else if (action == SC_REMOVE_BAR && bar_index >= 0) {
            taskbar_remove(bar_index); changed = 1;
        } else if (action == SC_REMOVE_DESK && desk_index >= 0) {
            desktop_remove(desk_index); changed = 1;
        }
        if (changed) shortcuts_saved("Shortcut layout");
    } else if (kind == MENU_TITLE) {
        struct client *c = find_client(target);
        if (c) {
            if (idx == 0) minimize(c);
            else if (idx == 1) set_maximized(c, !c->maximized);
            else if (idx == 2) close_client(c);
        }
    }
}

static void pick_window(Window target)
{
    /* Rows keep their own window IDs: another client closing while the
     * menu is open must not make a click activate a different window. */
    struct client *c = find_client(target);
    if (c) activate(c, 1);
}

/* ------------------------------------------------------- window overview */
static int overview_release_pending;

static int overview_screen_height(void)
{
    int height = screen_h - bar_h;
    return height > 1 ? height : 1;
}

static void overview_free_tiles(void)
{
    for (int i = 0; i < overview_count; i++)
        if (overview_tiles[i].preview) XFreePixmap(dpy, overview_tiles[i].preview);
    free(overview_tiles);
    overview_tiles = NULL; overview_count = 0; overview_hover = -1;
}

static int composite_state;
static Bool (*composite_query_extension)(Display *, int *, int *);
static void (*composite_redirect_window)(Display *, Window, int);
static void (*composite_unredirect_window)(Display *, Window, int);
static Pixmap (*composite_name_window_pixmap)(Display *, Window);

static int overview_composite_available(void)
{
    if (composite_state) return composite_state > 0;
    composite_state = -1;
    void *library = dlopen("libXcomposite.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!library) return 0;
    composite_query_extension = (Bool (*)(Display *, int *, int *))
        dlsym(library, "XCompositeQueryExtension");
    composite_redirect_window = (void (*)(Display *, Window, int))
        dlsym(library, "XCompositeRedirectWindow");
    composite_unredirect_window = (void (*)(Display *, Window, int))
        dlsym(library, "XCompositeUnredirectWindow");
    composite_name_window_pixmap = (Pixmap (*)(Display *, Window))
        dlsym(library, "XCompositeNameWindowPixmap");
    if (!composite_query_extension || !composite_redirect_window ||
        !composite_unredirect_window || !composite_name_window_pixmap) return 0;
    int event_base = 0, error_base = 0;
    if (!composite_query_extension(dpy, &event_base, &error_base)) return 0;
    composite_state = 1;
    return 1;
}

struct overview_channel {
    unsigned long mask, maximum;
    int shift;
};

static struct overview_channel overview_channel_format(unsigned long mask)
{
    struct overview_channel channel = { mask, 0, 0 };
    if (!mask) return channel;
    while (!(mask & 1)) { mask >>= 1; channel.shift++; }
    channel.maximum = mask;
    return channel;
}

static unsigned int overview_channel_to_8(unsigned long pixel,
                                          const struct overview_channel *channel)
{
    if (!channel->mask || !channel->maximum) return 0;
    unsigned long value = (pixel & channel->mask) >> channel->shift;
    return (unsigned int)((value * 255 + channel->maximum / 2) / channel->maximum);
}

static unsigned long overview_channel_from_8(unsigned int value,
                                             const struct overview_channel *channel)
{
    if (!channel->mask || !channel->maximum) return 0;
    unsigned long scaled = (value * channel->maximum + 127) / 255;
    return (scaled << channel->shift) & channel->mask;
}

static void overview_capture(struct overview_tile *tile, struct client *c)
{
    if (!tile || !c || c->minimized || c->w <= 0 || c->h <= 0 ||
        c->w > 16384 || c->h > 16384 || (long)c->w * c->h > 16777216L) return;
    int max_w = tile->w - 4, max_h = tile->h - 30;
    if (max_w <= 0 || max_h <= 0) return;
    double scale = (double)max_w / c->w;
    double other = (double)max_h / c->h;
    if (scale > other) scale = other;
    int pw = (int)(c->w * scale), ph = (int)(c->h * scale);
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;

    /* Obscured X11 windows do not have reliable readable pixels. Composite's
     * named pixmap does, including 32-bit application visuals. Redirect only
     * for the synchronous capture, then restore ordinary non-composited
     * rendering. With an external compositor, use its pixmap without changing
     * redirection. This is on-demand thumbnail capture, not a compositor. */
    Drawable source_drawable = c->win;
    Pixmap named = None;
    int unredirect = 0;
    if (overview_composite_available()) {
        Atom compositor_atom = XInternAtom(dpy, "_NET_WM_CM_S0", True);
        int external_compositor = compositor_atom && XGetSelectionOwner(dpy, compositor_atom);
        if (!external_compositor) composite_redirect_window(dpy, c->win, 0);
        named = composite_name_window_pixmap(dpy, c->win);
        if (named) {
            source_drawable = named;
            unredirect = !external_compositor;
        } else if (!external_compositor) {
            composite_unredirect_window(dpy, c->win, 0);
        }
    }
    XImage *source = XGetImage(dpy, source_drawable, 0, 0, (unsigned int)c->w, (unsigned int)c->h,
                               AllPlanes, ZPixmap);
    if (named) XFreePixmap(dpy, named);
    if (unredirect) composite_unredirect_window(dpy, c->win, 0);
    if (!source && source_drawable != c->win)
        source = XGetImage(dpy, c->win, 0, 0, (unsigned int)c->w, (unsigned int)c->h,
                           AllPlanes, ZPixmap);
    if (!source) return;
    if (!source->data || !source->red_mask || !source->green_mask || !source->blue_mask) {
        XDestroyImage(source); return;
    }

    Pixmap preview = XCreatePixmap(dpy, overview_win, pw, ph, DefaultDepth(dpy, scr_no));
    XImage *dest = XCreateImage(dpy, DefaultVisual(dpy, scr_no), DefaultDepth(dpy, scr_no),
                                ZPixmap, 0, NULL, pw, ph, 32, 0);
    if (!preview || !dest) {
        if (preview) XFreePixmap(dpy, preview);
        if (dest) XFree(dest);
        XDestroyImage(source); return;
    }
    if (!dest->red_mask || !dest->green_mask || !dest->blue_mask) {
        XFreePixmap(dpy, preview); XFree(dest); XDestroyImage(source); return;
    }
    dest->data = malloc((size_t)dest->bytes_per_line * ph);
    if (!dest->data) {
        XFreePixmap(dpy, preview); XFree(dest); XDestroyImage(source); return;
    }
    XInitImage(dest);
    struct overview_channel sr = overview_channel_format(source->red_mask);
    struct overview_channel sg = overview_channel_format(source->green_mask);
    struct overview_channel sb = overview_channel_format(source->blue_mask);
    struct overview_channel dr = overview_channel_format(dest->red_mask);
    struct overview_channel dg = overview_channel_format(dest->green_mask);
    struct overview_channel db = overview_channel_format(dest->blue_mask);
    int same_format = source->red_mask == dest->red_mask &&
                      source->green_mask == dest->green_mask &&
                      source->blue_mask == dest->blue_mask;
    for (int y = 0; y < ph; y++) {
        int sy = y * c->h / ph;
        for (int x = 0; x < pw; x++) {
            int sx = x * c->w / pw;
            unsigned long pixel = XGetPixel(source, sx, sy);
            if (!same_format) {
                unsigned int r = overview_channel_to_8(pixel, &sr);
                unsigned int g = overview_channel_to_8(pixel, &sg);
                unsigned int b = overview_channel_to_8(pixel, &sb);
                pixel = overview_channel_from_8(r, &dr) |
                        overview_channel_from_8(g, &dg) |
                        overview_channel_from_8(b, &db);
            }
            XPutPixel(dest, x, y, pixel);
        }
    }
    XPutImage(dpy, preview, gc, dest, 0, 0, 0, 0, pw, ph);
    tile->preview = preview; tile->preview_w = pw; tile->preview_h = ph;
    XDestroyImage(dest); XDestroyImage(source);
}

static void overview_rebuild(void)
{
    overview_free_tiles();
    if (!nclients) return;
    overview_tiles = calloc((size_t)nclients, sizeof(*overview_tiles));
    if (!overview_tiles) return;
    overview_count = nclients;
    int ax = OVERVIEW_MARGIN, ay = OVERVIEW_MARGIN + OVERVIEW_HEADER;
    int aw = screen_w - OVERVIEW_MARGIN * 2;
    int ah = overview_screen_height() - ay - OVERVIEW_MARGIN;
    if (aw < 1 || ah < 1) { overview_count = 0; return; }
    for (int i = 0; i < overview_count; i++) {
        struct overview_tile *tile = &overview_tiles[i];
        tile->win = clients[overview_count - 1 - i]->win;
        if (overview_count == 1) {
            tile->x = ax; tile->y = ay; tile->w = aw; tile->h = ah;
        } else if (overview_count == 2 || overview_count == 3) {
            int left = aw * 3 / 5;
            if (i == 0) {
                tile->x = ax; tile->y = ay; tile->w = left - OVERVIEW_GUTTER / 2; tile->h = ah;
            } else {
                int rows = overview_count - 1;
                tile->x = ax + left + OVERVIEW_GUTTER / 2;
                tile->y = ay + (i - 1) * (ah / rows);
                tile->w = aw - left - OVERVIEW_GUTTER / 2;
                tile->h = ah / rows - OVERVIEW_GUTTER;
                if (i == overview_count - 1) tile->h = ay + ah - tile->y;
            }
        } else {
            int cols = 3, rows = 2, span_rows = overview_count == 4 ? 1 : 2;
            while (cols * rows - 2 * span_rows < overview_count - 1) {
                if (rows >= cols) cols++; else rows++;
            }
            int cw = aw / cols, ch = ah / rows;
            if (i == 0) {
                tile->x = ax + OVERVIEW_GUTTER / 2; tile->y = ay + OVERVIEW_GUTTER / 2;
                tile->w = cw * 2 - OVERVIEW_GUTTER; tile->h = ch * span_rows - OVERVIEW_GUTTER;
            } else {
                int slot = 0, col = 0, row = 0;
                for (int n = 1; n <= i; n++) {
                    while (slot < cols * rows) {
                        col = slot % cols; row = slot / cols;
                        if (col < 2 && row < span_rows) slot++;
                        else break;
                    }
                    if (n < i) slot++;
                }
                tile->x = ax + col * cw + OVERVIEW_GUTTER / 2;
                tile->y = ay + row * ch + OVERVIEW_GUTTER / 2;
                tile->w = cw - OVERVIEW_GUTTER; tile->h = ch - OVERVIEW_GUTTER;
            }
        }
        /* Keep every tile inside the usable area even on tiny screens or
         * with enough windows to make a grid cell smaller than a titlebar. */
        if (tile->x < ax) tile->x = ax;
        if (tile->y < ay) tile->y = ay;
        if (tile->x >= ax + aw) tile->x = ax + aw - 1;
        if (tile->y >= ay + ah) tile->y = ay + ah - 1;
        if (tile->w < 1) tile->w = 1;
        if (tile->h < 1) tile->h = 1;
        if (tile->w > ax + aw - tile->x) tile->w = ax + aw - tile->x;
        if (tile->h > ay + ah - tile->y) tile->h = ay + ah - tile->y;
        overview_capture(tile, clients[overview_count - 1 - i]);
    }
    overview_hover = 0;
}

static void draw_overview(void)
{
    if (!overview_active) return;
    int height = overview_screen_height();
    Pixmap pm = surface_backing(overview_win, &overview_pm, &overview_pm_w, &overview_pm_h,
                                screen_w, height);
    Drawable d = pm ? pm : overview_win;
    paint_wallpaper(d, screen_w, height);
    text_out(d, OVERVIEW_MARGIN, OVERVIEW_MARGIN + font->ascent, "Window overview", PX.main);
    char hint[96];
    snprintf(hint, sizeof(hint), "%d windows - click to focus; right-click or Esc closes", overview_count);
    clipped_text(d, OVERVIEW_MARGIN + 160, OVERVIEW_MARGIN + font->ascent, hint,
                 screen_w - OVERVIEW_MARGIN * 2 - 160, PX.text);
    for (int i = 0; i < overview_count; i++) {
        struct overview_tile *tile = &overview_tiles[i];
        struct client *c = find_client(tile->win);
        int lit = i == overview_hover;
        fill(d, tile->x, tile->y, tile->w, tile->h, PX.win_bg);
        if (tile->w < 32 || tile->h < 48) {
            rect(d, tile->x, tile->y, tile->w, tile->h, PX.main);
            continue;
        }
        fill(d, tile->x + 1, tile->y + 1, tile->w - 2, 24, lit ? PX.main : PX.title_off);
        char title[300];
        snprintf(title, sizeof(title), "%s%.270s", c && c->minimized ? "[-] " : "",
                 c ? c->title : "Closed window");
        clipped_text(d, tile->x + 8, tile->y + 5 + font->ascent, title,
                     tile->w - 16, lit ? PX.title_text : PX.text);
        int cw = tile->w - 4, ch = tile->h - 28;
        if (tile->preview) {
            int px = tile->x + 2 + (cw - tile->preview_w) / 2;
            int py = tile->y + 26 + (ch - tile->preview_h) / 2;
            XCopyArea(dpy, tile->preview, d, gc, 0, 0, tile->preview_w, tile->preview_h, px, py);
        } else {
            fill(d, tile->x + 2, tile->y + 26, cw, ch, PX.bg_bot);
            if (c && xicon_drawn(c->icon))
                draw_xicon(d, c->icon, tile->x + (tile->w - c->icon->w) / 2,
                           tile->y + 26 + (ch - c->icon->h) / 2);
            clipped_text(d, tile->x + 10, tile->y + tile->h - 12,
                         c && c->minimized ? "Minimized - preview unavailable" : "Preview unavailable",
                         tile->w - 20, PX.text);
        }
        /* Paint all four edges last so a captured image can never leave a
         * white or overwritten strip on the tile's right side. */
        rect(d, tile->x, tile->y, tile->w, tile->h, PX.main);
    }
    surface_present(overview_win, pm, screen_w, height);
}

static void overview_map_below_bar(void)
{
    XMapWindow(dpy, overview_win);
    XWindowChanges wc;
    wc.sibling = bar_win;
    wc.stack_mode = Below;
    XConfigureWindow(dpy, overview_win, CWSibling | CWStackMode, &wc);
    /* A fullscreen frame may normally be above the dock. It must still be
     * behind the overview until the overview closes. */
    for (int i = 0; i < nclients; i++) {
        if (!clients[i]->fullscreen) continue;
        wc.sibling = overview_win;
        wc.stack_mode = Below;
        XConfigureWindow(dpy, clients[i]->frame, CWSibling | CWStackMode, &wc);
    }
}

static void overview_close(void)
{
    if (!overview_active) { overview_free_tiles(); return; }
    overview_active = 0; overview_release_pending = 0;
    XUnmapWindow(dpy, overview_win);
    XUngrabKeyboard(dpy, CurrentTime);
    overview_free_tiles();
    if (edge_input_passthrough) {
        XMapWindow(dpy, edge_win);
        restack_right_rule();
    }
    for (int i = 0; i < nclients; i++)
        if (clients[i]->fullscreen) XRaiseWindow(dpy, clients[i]->frame);
    if (active_client) XSetInputFocus(dpy, active_client->win, RevertToPointerRoot, CurrentTime);
    else XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
    draw_bar();
}

static void overview_show(void)
{
    if (overview_active) { overview_close(); return; }
    if (!nclients) { wm_notify_no_windows(); menu_bar_release = 1; return; }
    menu_close();
    if (launch_open) launcher_toggle(0);
    if (tool_kind != TOOL_NONE) tool_close();
    if (drag.kind) { drag.kind = 0; XUngrabPointer(dpy, CurrentTime); }
    overview_active = 1; overview_release_pending = 1;
    overview_rebuild();
    if (!overview_count) { overview_close(); return; }
    XMoveResizeWindow(dpy, overview_win, 0, 0, screen_w, overview_screen_height());
    XUnmapWindow(dpy, edge_win);
    overview_map_below_bar();
    XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    draw_overview(); draw_bar();
}

static void overview_clients_changed(void)
{
    if (!overview_active) return;
    XUnmapWindow(dpy, overview_win);
    overview_rebuild();
    if (!overview_count) { overview_close(); return; }
    overview_map_below_bar();
    draw_overview();
}

static int overview_tile_at(int x, int y)
{
    for (int i = 0; i < overview_count; i++) {
        struct overview_tile *tile = &overview_tiles[i];
        if (x >= tile->x && x < tile->x + tile->w && y >= tile->y && y < tile->y + tile->h)
            return i;
    }
    return -1;
}

/* keyboard focus cycling, SCos taskbar order */
static void cycle(int dir)
{
    if (!nclients) return;
    int cur = -1;
    for (int i = 0; i < nclients; i++) if (clients[i] == active_client) cur = i;
    for (int step = 1; step <= nclients; step++) {
        int i = ((cur + dir * step) % nclients + nclients) % nclients;
        if (!clients[i]->minimized) { activate(clients[i], 1); return; }
    }
}

/* --------------------------------------------------------------- events */
static void on_button(XButtonEvent *e)
{
    Window w = e->window;
    int x = e->x_root, y = e->y_root;
    struct client *press_client = find_client(w);
    int passive_client_press = e->type == ButtonPress && e->button == Button1 &&
                               !(e->state & Mod1Mask) && press_client &&
                               press_client->win == w;
    if (overview_active) {
        if (overview_release_pending && e->type == ButtonRelease) {
            overview_release_pending = 0; return;
        }
        if (w == bar_win) {
            if (e->type != ButtonPress) return;
            int windows_button = e->x >= 48 && e->x < 158 &&
                                 e->y >= 6 && e->y < bar_h - 6;
            overview_close();
            /* Right-clicking the same button toggles the overview shut.
             * Every other taskbar press continues through its normal path
             * after closing the overview, including pins, launcher and Wi-Fi. */
            if (windows_button && e->button == Button3) return;
        } else {
            if (e->type == ButtonPress) overview_release_pending = 0;
            if (w != overview_win) return;
            if (e->type == ButtonPress && e->button == Button3) { overview_close(); return; }
            if (e->type == ButtonRelease && e->button == Button1) {
                int index = overview_tile_at(e->x, e->y);
                Window target = index >= 0 ? overview_tiles[index].win : None;
                overview_close();
                if (target) {
                    struct client *c = find_client(target);
                    if (c) activate(c, 1);
                }
            }
            return;
        }
    }
    if (e->type == ButtonPress && w != root && desk_sel_mask) {
        desk_sel_mask = 0;
        draw_desktop();
    }

    /* Menus open on press. Do not let the release from that very same click
     * immediately dismiss the menu or activate the row under the pointer. */
    if (menu_kind != MENU_NONE && menu_bar_release && e->type == ButtonRelease) {
        menu_bar_release = 0;
        return;
    }
    if (menu_kind != MENU_NONE && w != menu_win) {
        menu_close();
        if (passive_client_press) XAllowEvents(dpy, ReplayPointer, e->time);
        return;
    }
    if (w == menu_win) {
        if (menu_kind == MENU_CONFIRM) {
            if (e->type == ButtonRelease && e->button == Button1 && e->y >= 86 && e->y < 114) {
                if (e->x >= 12 && e->x < 132) menu_close();
                else if (e->x >= 188 && e->x < 368) {
                    int action = confirm_action;
                    menu_close();
                    power_perform(action);
                }
            }
            return;
        }
        if (e->type != ButtonRelease || e->button != Button1) return;
        int idx = (e->y - 3) / 24;
        if (menu_kind == MENU_MESSAGE) menu_close();
        else if (menu_kind == MENU_PICKER) {
            if (idx >= 0 && idx < menu_n) {
                Window target = menu_item_targets[idx];
                menu_close(); pick_window(target);
            } else menu_close();
        }
        else if (idx >= 0 && idx < menu_n) menu_action(idx);
        else menu_close();
        return;
    }
    if (tool_kind != TOOL_NONE) {
        /* A pin/launcher/desktop press opens these panels. Its matching
         * release is not an outside click and must not close them again. */
        if (tool_release_pending && e->type == ButtonRelease) {
            tool_release_pending = 0;
            return;
        }
        if (e->type == ButtonPress) tool_release_pending = 0;
        if (w == tool_win) tool_button(e);
        else {
            tool_close();
            if (passive_client_press) XAllowEvents(dpy, ReplayPointer, e->time);
        }
        return;
    }
    if (launch_open && w != launch_win && w != bar_win) launcher_toggle(0);

    /* A button release ends any drag, wherever the pointer happens to
     * be. Finalizing only on the dragged window leaked the drag state
     * whenever the release landed on another window, the taskbar or a
     * menu: the window stayed glued to the pointer, and desktop
     * repaints (suppressed mid-drag) never resumed, so closed windows
     * left ghost frames on the desktop. */
    if (e->type == ButtonRelease && drag.kind) {
        if (drag.kind == 4) { shortcut_drag_finish(e); return; }
        XUngrabPointer(dpy, CurrentTime);
        if (drag.kind == 3) {
            drag.band_x = x; drag.band_y = y;
            desk_sel_mask = 0;
            int bx = drag.start_x < drag.band_x ? drag.start_x : drag.band_x;
            int byy = drag.start_y < drag.band_y ? drag.start_y : drag.band_y;
            int bx2 = drag.start_x > drag.band_x ? drag.start_x : drag.band_x;
            int by2 = drag.start_y > drag.band_y ? drag.start_y : drag.band_y;
            for (int i = 0; i < ndesk_pins; i++) {
                int ix, iy; icon_rect(i, &ix, &iy);
                if (ix < bx2 && ix + 80 > bx && iy < by2 && iy + 88 > byy)
                    desk_sel_mask |= 1ul << i;
            }
            drag.kind = 0;
        } else {
            struct client *dc = drag.c;
            drag.kind = 0; drag.c = NULL;
            if (dc && find_client(dc->win) == dc) draw_frame(dc);
        }
        draw_desktop();
        return;
    }

    if (w == bar_win) {
        if (e->type == ButtonRelease) {
            if (bar_press != -1) { bar_press = -1; draw_bar(); }
            return;
        }
        if (e->type != ButtonPress) return;
        if (e->button == Button4 || e->button == Button5) {
            cycle(e->button == Button5 ? 1 : -1);
            return;
        }
        if (e->x >= 8 && e->x < 42 && e->y >= 6 && e->y < bar_h - 6) {
            launcher_toggle(!launch_open); return;
        }
        if (e->x >= 48 && e->x < 158 && e->y >= 6 && e->y < bar_h - 6) {
            if (e->button == Button3) { overview_show(); return; }
            if (e->button != Button1) return;
            /* Windows picker as a SCos-style menu of open windows */
            if (!nclients) { wm_notify_no_windows(); menu_bar_release = 1; return; }
            menu_kind = MENU_PICKER;
            menu_hover = -1;
            menu_n = nclients > 8 ? 8 : nclients;
            for (int i = 0; i < menu_n; i++) {
                struct client *c = clients[nclients - 1 - i];
                menu_item_targets[i] = c->win;
                snprintf(menu_labels[i], 64, "%s %.55s", c->minimized ? "[-]" :
                         (active_client == c ? "[+]" : "   "), c->title);
            }
            menu_x = 48; menu_y = screen_h - bar_h - (menu_n * 24 + 8);
            menu_w = 300;
            XMoveResizeWindow(dpy, menu_win, menu_x, menu_y, menu_w, menu_n * 24 + 6);
            XMapWindow(dpy, menu_win);
            XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime);
            draw_menu();
            menu_bar_release = 1;
            return;
        }
        int pw_x = bar_power_x();
        if (e->x >= bar_wifi_x() && e->x < bar_wifi_x() + 34 && e->y >= 6 && e->y < bar_h - 6) {
            tool_open(TOOL_WIFI); return;
        }
        if (e->x >= pw_x && e->x < pw_x + 34 && e->y >= 6 && e->y < bar_h - 6) {
            menu_open(MENU_POWER, pw_x - 140, screen_h - bar_h - (6 * 24 + 8), None);
            menu_bar_release = 1;
            return;
        }
        for (int i = 0; i < bar_n; i++) {
            struct bar_item *it = &bar_items[i];
            if (e->x >= it->x && e->x < it->x + it->w &&
                e->y >= 6 && e->y < bar_h - 6) {
                if (e->button == Button3) {
                    menu_open_shortcut(&pins[it->idx], SHORTCUT_BAR, x, y);
                    menu_bar_release = 1;
                    return;
                }
                if (e->button == Button1) {
                    /* Press feedback is immediate, but activation waits for
                     * a no-motion release so the same gesture can become a
                     * taskbar reorder or a drag to the desktop. */
                    bar_press = i;
                    draw_bar();
                    shortcut_drag_begin(SHORTCUT_BAR, it->idx, e, 0, 0);
                }
                return;
            }
        }
        return;
    }

    if (w == launch_win) {
        if (e->type != ButtonPress) return;
        if (e->button == Button4 || e->button == Button5) {
            /* Move the selection, not just the scroll offset: draw_launcher()
             * clamps the offset so the selection stays visible, so nudging
             * the offset alone snapped straight back and the wheel looked
             * dead in the All Apps list. */
            int n2 = launch_rebuild();
            if (n2 > 0) {
                launch_sel += e->button == Button5 ? 3 : -3;
                if (launch_sel < 0) launch_sel = 0;
                if (launch_sel >= n2) launch_sel = n2 - 1;
            }
            draw_launcher();
            return;
        }
        int row = (e->y - 53) / 24;
        int n = launch_rebuild();
        int idx = launch_offset + row;
        if ((e->button == Button3 || (e->button == Button1 && launch_pin_mode)) &&
            idx >= 0 && idx < n && launch_items[idx].action != ACTION_ALLAPPS) {
            struct launch_item *item = &launch_items[idx];
            struct pin p; memset(&p, 0, sizeof(p)); p.slot = -1;
            snprintf(p.label, sizeof(p.label), "%s", item->name);
            snprintf(p.glyph, sizeof(p.glyph), "%s", item->glyph);
            snprintf(p.icon, sizeof(p.icon), "%s", item->icon);
            snprintf(p.command, sizeof(p.command), "%s", item->command);
            p.action = item->action;
            if (item->app_idx >= 0 && item->app_idx < napps)
                snprintf(p.match, sizeof(p.match), "%s", apps[item->app_idx].id);
            else if (item->action == CORE_NONE) {
                for (int i = 0; i < npins; i++)
                    if (!strcmp(pins[i].label, p.label) && !strcmp(pins[i].command, p.command)) { p = pins[i]; break; }
                for (int i = 0; i < ndesk_pins; i++)
                    if (!strcmp(desk_pins[i].label, p.label) && !strcmp(desk_pins[i].command, p.command)) { p = desk_pins[i]; break; }
            }
            menu_open_shortcut(&p, SHORTCUT_LAUNCHER, e->x_root, e->y_root);
            return;
        }
        if (e->button == Button1 && idx >= 0 && idx < n) {
            struct launch_item it = launch_items[idx];
            if (it.action == ACTION_ALLAPPS) {
                launch_all = 1; launch_sel = 0; launch_offset = 0;
                draw_launcher();
                return;
            }
            launcher_toggle(0);
            launch_exec_item(&it);
        }
        return;
    }

    if (w == root) {
        if (e->type == ButtonRelease && drag.kind == 3) {
            drag.kind = 0;
            desk_sel_mask = 0;
            int bx = drag.start_x < drag.band_x ? drag.start_x : drag.band_x;
            int byy = drag.start_y < drag.band_y ? drag.start_y : drag.band_y;
            int bx2 = drag.start_x > drag.band_x ? drag.start_x : drag.band_x;
            int by2 = drag.start_y > drag.band_y ? drag.start_y : drag.band_y;
            for (int i = 0; i < ndesk_pins; i++) {
                int ix, iy; icon_rect(i, &ix, &iy);
                if (ix < bx2 && ix + 80 > bx && iy < by2 && iy + 88 > byy)
                    desk_sel_mask |= 1ul << i;
            }
            draw_desktop();
            return;
        }
        if (e->type != ButtonPress) return;
        if (e->button == Button4 || e->button == Button5) {
            cycle(e->button == Button5 ? 1 : -1);
            return;
        }
        if (e->button == Button3) {
            for (int i = 0; i < ndesk_pins; i++) {
                int ix, iy; icon_rect(i, &ix, &iy);
                if (x >= ix && x < ix + 80 && y >= iy && y < iy + 88) {
                    menu_open_shortcut(&desk_pins[i], SHORTCUT_DESKTOP, x, y);
                    return;
                }
            }
            menu_open(MENU_DESKTOP, x, y, None);
            return;
        }
        if (e->button != Button1) return;
        int hit = -1;
        for (int i = 0; i < ndesk_pins; i++) {
            int ix, iy; icon_rect(i, &ix, &iy);
            if (x >= ix && x < ix + 80 && y >= iy && y < iy + 88) hit = i;
        }
        if (hit >= 0) {
            desk_sel_mask = 1ul << hit;
            draw_desktop();
            int icon_x, icon_y; icon_rect(hit, &icon_x, &icon_y);
            shortcut_drag_begin(SHORTCUT_DESKTOP, hit, e, icon_x, icon_y);
            return;
        }
        shortcut_click.source = shortcut_click.index = -1;
        /* rubber band on empty desktop: clear any icon selection at once,
         * otherwise the old highlight stayed painted until button release
         * and looked like clicking off an icon never un-highlighted it */
        if (desk_sel_mask) { desk_sel_mask = 0; draw_desktop(); }
        drag.kind = 3; drag.start_x = drag.band_x = x; drag.start_y = drag.band_y = y;
        drag.moved = 0; drag.band_drawn = 0;
        grab_drag_pointer(NULL, e->time);
        desk_sel_mask = 0;
        return;
    }

    struct client *c = find_client(w);
    if (!c) return;
    int is_frame = w == c->frame;

    if (e->type == ButtonRelease) {
        if (drag.kind && drag.c == c) {
            if (drag.kind == 2) { int w2 = c->w, h2 = c->h; client_apply_hints(c, &w2, &h2); c->w = w2; c->h = h2; client_place(c); }
            drag.kind = 0; drag.c = NULL;
            draw_frame(c);
            draw_desktop();
        }
        return;
    }
    if (e->type != ButtonPress) return;
    if (c->minimized) return;
    if (is_frame && (e->button == Button4 || e->button == Button5)) {
        cycle(e->button == Button5 ? 1 : -1);
        return;
    }
    if (e->button <= Button3) activate(c, 1);

    int rx = is_frame ? e->x : e->x + 1;
    int ry = is_frame ? e->y : e->y + TOP_H;
    /* Alt+drag anywhere: move (left) / resize (right) */
    if (e->state & Mod1Mask) {
        drag.kind = e->button == Button3 ? 2 : 1;
        drag.edges = e->button == Button3 ? (2 | 8) : 0;
        drag.c = c; drag.start_x = x; drag.start_y = y;
        drag.ox = c->x; drag.oy = c->y; drag.ow = c->w; drag.oh = c->h;
        grab_drag_pointer(c, e->time);
        return;
    }
    /* Edge/corner resize, reachable from the client area as well as the
     * 1 px frame: this is where the pointer actually is when someone
     * grabs a window edge. */
    if (e->button == Button1) {
        int edges = resize_edges_at(c, rx, ry);
        if (edges) {
            drag.kind = 2; drag.edges = edges;
            drag.c = c; drag.start_x = x; drag.start_y = y;
            drag.ox = c->x; drag.oy = c->y; drag.ow = c->w; drag.oh = c->h;
            /* The passive Button1 grab is already active here. Thaw its
             * synchronous pointer mode so the WM receives the drag. */
            XAllowEvents(dpy, AsyncPointer, e->time);
            return;
        }
    }
    if (!is_frame) {
        if (e->button == Button1) XAllowEvents(dpy, ReplayPointer, e->time);
        return;
    }

    if (ry < TITLE_H && e->button == Button1) {
        int b = title_button_at(c, rx, ry);
        if (b >= 0) {
            if (b == 0) minimize(c);
            else if (b == 1) set_maximized(c, !c->maximized);
            else close_client(c);
            return;
        }
        /* double-click on the titlebar toggles maximise, as in SCos */
        int dbl = (long)(e->time - last_click.t) < 400 && last_click.icon == -2 &&
                  x - last_click.x < 6 && x - last_click.x > -6;
        last_click.t = e->time; last_click.x = x; last_click.y = y; last_click.icon = -2;
        if (dbl && !c->fullscreen) { set_maximized(c, !c->maximized); return; }
        if (c->fullscreen) return;
        if (c->maximized) {
            /* Dragging a maximised titlebar restores the window and
             * keeps it under the pointer, so "everything opens
             * maximised" is escapable with the most natural gesture
             * there is (double-click titlebar and Alt+F10 already
             * toggled it, but nobody finds those first). */
            set_maximized(c, 0);
            int wx, wy, ww, wh; work_area(&wx, &wy, &ww, &wh);
            c->x = x - frame_w(c) / 2;
            c->y = y - TITLE_H / 2;
            if (c->x < wx) c->x = wx;
            if (c->x + frame_w(c) > wx + ww) c->x = wx + ww - frame_w(c);
            if (c->y < wy) c->y = wy;
            client_place(c);
            draw_frame(c);
        }
        drag.kind = 1; drag.edges = 0; drag.c = c; drag.start_x = x; drag.start_y = y;
        drag.ox = c->x; drag.oy = c->y; drag.ow = c->w; drag.oh = c->h;
        grab_drag_pointer(c, e->time);
        return;
    }
    if (ry < TITLE_H && e->button == Button3) {
        menu_open(MENU_TITLE, x, y, c->win);
        return;
    }
    /* bottom-right resize grip */
    if (e->button == Button1 && rx >= frame_w(c) - 18 && ry >= frame_h(c) - 18 &&
        !c->maximized && !c->fullscreen) {
        drag.kind = 2; drag.edges = 2 | 8; drag.c = c; drag.start_x = x; drag.start_y = y;
        drag.ox = c->x; drag.oy = c->y; drag.ow = c->w; drag.oh = c->h;
        grab_drag_pointer(c, e->time);
    }
}

static void on_motion(XMotionEvent *e)
{
    if (overview_active && e->window == overview_win) {
        int hover = overview_tile_at(e->x, e->y);
        if (hover != overview_hover) { overview_hover = hover; draw_overview(); }
        return;
    }
    if (overview_active && e->window != bar_win) return;
    if (tool_kind != TOOL_NONE && e->window != tool_win && tool_hover != TOOL_HOVER_NONE) {
        tool_hover = TOOL_HOVER_NONE;
        draw_tool();
    }
    if (!drag.kind) {
        /* Hover tracking: nothing anywhere highlighted under the pointer,
         * which made the whole desktop feel unresponsive. Each surface
         * remembers what is hovered and repaints only when it changes. */
        if (tool_kind != TOOL_NONE && e->window == tool_win) {
            int hover = tool_hover_at(e->x, e->y);
            if (hover != tool_hover) { tool_hover = hover; draw_tool(); }
        } else if (menu_kind != MENU_NONE && e->window == menu_win) {
            if (menu_kind == MENU_CONFIRM) {
                int hover = -1;
                if (e->y >= 86 && e->y < 114) {
                    if (e->x >= 12 && e->x < 132) hover = 0;
                    else if (e->x >= 188 && e->x < 368) hover = 1;
                }
                if (hover != confirm_hover) { confirm_hover = hover; draw_menu(); }
            } else {
                int idx = (e->y - 3) / 24;
                if (idx < 0 || idx >= menu_n) idx = -1;
                if (idx != menu_hover) { menu_hover = idx; draw_menu(); }
            }
        } else if (e->window == root) {
            int hit = -1;
            for (int i = 0; i < ndesk_pins; i++) {
                int ix, iy; icon_rect(i, &ix, &iy);
                if (e->x_root >= ix && e->x_root < ix + 80 &&
                    e->y_root >= iy && e->y_root < iy + 88) hit = i;
            }
            if (hit != desk_hover) { desk_hover = hit; draw_desktop(); }
        } else if (e->window == bar_win) {
            int h = -1;
            if (e->y >= 6 && e->y < bar_h - 6) {
                if (e->x >= 8 && e->x < 42) h = -2;
                else if (e->x >= 48 && e->x < 158) h = -3;
                else if (e->x >= bar_wifi_x() && e->x < bar_wifi_x() + 34) h = -5;
                else if (e->x >= bar_power_x() && e->x < bar_power_x() + 34) h = -4;
                else {
                    bar_layout();
                    for (int i = 0; i < bar_n; i++)
                        if (e->x >= bar_items[i].x && e->x < bar_items[i].x + bar_items[i].w) h = i;
                }
            }
            if (h != bar_hover) { bar_hover = h; draw_bar(); }
        } else if (launch_open && e->window == launch_win) {
            int n = launch_rebuild();
            int idx = launch_offset + (e->y - 53) / 24;
            if (idx >= 0 && idx < n && idx != launch_sel) {
                launch_sel = idx;
                draw_launcher();
            }
        } else {
            struct client *c = find_client(e->window);
            if (c && e->window == c->frame) {
                int b = title_button_at(c, e->x, e->y);
                if (c != hover_frame_client || b != hover_title_btn) {
                    struct client *old = hover_frame_client;
                    hover_frame_client = c; hover_title_btn = b;
                    if (old && old != c) draw_frame(old);
                    draw_frame(c);
                }
            } else if (hover_frame_client) {
                struct client *old = hover_frame_client;
                hover_frame_client = NULL; hover_title_btn = -1;
                draw_frame(old);
            }
        }
        return;
    }
    if (drag.kind == 3) {
        drag.band_x = e->x_root; drag.band_y = e->y_root;
        if (abs(drag.band_x - drag.start_x) > 4 || abs(drag.band_y - drag.start_y) > 4)
            drag.moved = 1;
        if (drag.moved) draw_desktop();
        return;
    }
    if (drag.kind == 4) { shortcut_drag_motion(e); return; }
    struct client *c = drag.c;
    if (!c) { drag.kind = 0; return; }
    if (drag.kind == 1) {                    /* move */
        if (c->maximized || c->fullscreen) return;
        c->x = drag.ox + (e->x_root - drag.start_x);
        c->y = drag.oy + (e->y_root - drag.start_y);
        int wx, wy, ww, wh; work_area(&wx, &wy, &ww, &wh);
        (void)ww; (void)wh;
        if (c->y < wy) c->y = wy;
        client_place(c);
    } else if (drag.kind == 2) {             /* resize */
        int dx = e->x_root - drag.start_x;
        int dy = e->y_root - drag.start_y;
        int nw = drag.ow, nh = drag.oh;
        if (drag.edges & 2) nw = drag.ow + dx;
        if (drag.edges & 8) nh = drag.oh + dy;
        if (drag.edges & 1) nw = drag.ow - dx;
        if (drag.edges & 4) nh = drag.oh - dy;
        client_apply_hints(c, &nw, &nh);
        /* left/top edges keep the opposite edge fixed */
        if (drag.edges & 1) c->x = drag.ox + (drag.ow - nw);
        if (drag.edges & 4) c->y = drag.oy + (drag.oh - nh);
        c->w = nw; c->h = nh;
        client_place(c);
    }
}

enum { MEDIA_VOLUME_MUTE, MEDIA_VOLUME_DOWN, MEDIA_VOLUME_UP,
       MEDIA_PLAY_PAUSE, MEDIA_NEXT, MEDIA_PREVIOUS,
       MEDIA_BRIGHTNESS_UP, MEDIA_BRIGHTNESS_DOWN };

static const char *media_command(int action)
{
    const char *configured = "";
    switch (action) {
    case MEDIA_VOLUME_MUTE: configured = cfg.volume_mute_cmd; break;
    case MEDIA_VOLUME_DOWN: configured = cfg.volume_down_cmd; break;
    case MEDIA_VOLUME_UP: configured = cfg.volume_up_cmd; break;
    case MEDIA_PLAY_PAUSE: configured = cfg.media_play_pause_cmd; break;
    case MEDIA_NEXT: configured = cfg.media_next_cmd; break;
    case MEDIA_PREVIOUS: configured = cfg.media_previous_cmd; break;
    case MEDIA_BRIGHTNESS_UP: configured = cfg.brightness_up_cmd; break;
    case MEDIA_BRIGHTNESS_DOWN: configured = cfg.brightness_down_cmd; break;
    }
    if (configured[0]) return configured;
    switch (action) {
    case MEDIA_VOLUME_MUTE:
        if (cmd_exists("wpctl")) return "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle";
        if (cmd_exists("pactl")) return "pactl set-sink-mute @DEFAULT_SINK@ toggle";
        if (cmd_exists("amixer")) return "amixer -q sset Master toggle";
        break;
    case MEDIA_VOLUME_DOWN:
        if (cmd_exists("wpctl")) return "wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%-";
        if (cmd_exists("pactl")) return "pactl set-sink-volume @DEFAULT_SINK@ -5%";
        if (cmd_exists("amixer")) return "amixer -q sset Master 5%-";
        break;
    case MEDIA_VOLUME_UP:
        if (cmd_exists("wpctl")) return "wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+";
        if (cmd_exists("pactl")) return "pactl set-sink-volume @DEFAULT_SINK@ +5%";
        if (cmd_exists("amixer")) return "amixer -q sset Master 5%+";
        break;
    case MEDIA_PLAY_PAUSE:
        if (cmd_exists("playerctl")) return "playerctl play-pause";
        break;
    case MEDIA_NEXT:
        if (cmd_exists("playerctl")) return "playerctl next";
        break;
    case MEDIA_PREVIOUS:
        if (cmd_exists("playerctl")) return "playerctl previous";
        break;
    case MEDIA_BRIGHTNESS_UP:
        if (cmd_exists("brightnessctl")) return "brightnessctl set +5%";
        if (cmd_exists("light")) return "light -A 5";
        break;
    case MEDIA_BRIGHTNESS_DOWN:
        if (cmd_exists("brightnessctl")) return "brightnessctl set 5%-";
        if (cmd_exists("light")) return "light -U 5";
        break;
    }
    return NULL;
}

static void run_media_action(int action)
{
    const char *command = media_command(action);
    if (command) spawn(command);
}

static int media_key_action(KeySym ks)
{
    int action = -1;
    if (ks == XF86XK_AudioMute) action = MEDIA_VOLUME_MUTE;
    else if (ks == XF86XK_AudioLowerVolume) action = MEDIA_VOLUME_DOWN;
    else if (ks == XF86XK_AudioRaiseVolume) action = MEDIA_VOLUME_UP;
    else if (ks == XF86XK_AudioPlay) action = MEDIA_PLAY_PAUSE;
    else if (ks == XF86XK_AudioPause) action = MEDIA_PLAY_PAUSE;
    else if (ks == XF86XK_AudioNext) action = MEDIA_NEXT;
    else if (ks == XF86XK_AudioPrev) action = MEDIA_PREVIOUS;
    else if (ks == XF86XK_MonBrightnessUp) action = MEDIA_BRIGHTNESS_UP;
    else if (ks == XF86XK_MonBrightnessDown) action = MEDIA_BRIGHTNESS_DOWN;
    if (action < 0) return 0;
    run_media_action(action);
    return 1;
}

static void on_key(XKeyEvent *e)
{
    KeySym ks = XLookupKeysym(e, 0);
    unsigned int mods = e->state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);

    /* Hardware/media keys remain available in panels and the overview. */
    if (!mods && media_key_action(ks)) return;

    if (overview_active) {
        if (ks == XK_Escape) { overview_close(); return; }
        if (ks == XK_Left || ks == XK_Up || ks == XK_Right || ks == XK_Down || ks == XK_Tab) {
            if (overview_count) {
                int direction = ks == XK_Left || ks == XK_Up ? -1 : 1;
                if (overview_hover < 0) overview_hover = 0;
                else overview_hover = (overview_hover + direction + overview_count) % overview_count;
                draw_overview();
            }
            return;
        }
        if (ks == XK_Return || ks == XK_KP_Enter) {
            if (overview_hover >= 0 && overview_hover < overview_count) {
                Window target = overview_tiles[overview_hover].win;
                overview_close();
                struct client *c = find_client(target);
                if (c) activate(c, 1);
            }
            return;
        }
        int global_key = (mods == Mod1Mask &&
                          (ks == XK_Tab || ks == XK_F4 || ks == XK_F9 || ks == XK_F10)) ||
                         (mods == (Mod1Mask | ShiftMask) && ks == XK_Tab) ||
                         (mods == Mod4Mask &&
                          (ks == XK_Return || ks == XK_KP_Enter || ks == XK_d || ks == XK_D ||
                           ks == XK_q || ks == XK_Q || ks == XK_m || ks == XK_M || ks == XK_comma ||
                           ks == XK_F1 || ks == XK_F2 || ks == XK_F3));
        if (!global_key) return;
        overview_close();
    }
    if (tool_kind != TOOL_NONE) { tool_key(e); return; }
    if (menu_kind != MENU_NONE) {
        if (menu_kind == MENU_CONFIRM) {
            if (ks == XK_Escape) menu_close();
            else if (ks == XK_Left || ks == XK_Right || ks == XK_Tab) {
                confirm_hover = confirm_hover == 0 ? 1 : 0; draw_menu();
            } else if (ks == XK_Return || ks == XK_KP_Enter) {
                int action = confirm_hover == 1 ? confirm_action : -1;
                menu_close();
                if (action >= 0) power_perform(action);
            }
        } else if (ks == XK_Escape) menu_close();
        return;
    }
    if (launch_open) {
        int n = launch_rebuild();
        if (ks == XK_Escape) {
            if (launch_all && !launch_q[0]) { launch_all = 0; launch_sel = 0; launch_offset = 0; draw_launcher(); }
            else launcher_toggle(0);
        }
        else if (ks == XK_Return) {
            if (n > 0) {
                struct launch_item it = launch_items[launch_sel];
                if (it.action == ACTION_ALLAPPS) {
                    launch_all = 1; launch_sel = 0; launch_offset = 0;
                    draw_launcher();
                } else {
                    launcher_toggle(0);
                    launch_exec_item(&it);
                }
            }
        }
        else if (ks == XK_Down) { if (launch_sel < n - 1) launch_sel++; draw_launcher(); }
        else if (ks == XK_Up) { if (launch_sel > 0) launch_sel--; draw_launcher(); }
        else if (ks == XK_Page_Down) { launch_sel += 10; if (launch_sel >= n) launch_sel = n - 1; draw_launcher(); }
        else if (ks == XK_Page_Up) { launch_sel -= 10; if (launch_sel < 0) launch_sel = 0; draw_launcher(); }
        else if (ks == XK_BackSpace) {
            size_t l = strlen(launch_q);
            if (l) { launch_q[l - 1] = 0; launch_sel = 0; draw_launcher(); }
        } else {
            char buf[16];
            int m = XLookupString(e, buf, sizeof(buf), NULL, NULL);
            int changed = 0;
            for (int i = 0; i < m; i++)
                if (buf[i] >= 32 && buf[i] < 127 && strlen(launch_q) < sizeof(launch_q) - 1) {
                    size_t l = strlen(launch_q);
                    launch_q[l] = buf[i]; launch_q[l + 1] = 0;
                    changed = 1; launch_sel = 0;
                }
            if (changed) draw_launcher();
        }
        return;
    }

    if (mods == Mod1Mask && ks == XK_Tab) cycle(1);
    else if (mods == (Mod1Mask | ShiftMask) && ks == XK_Tab) cycle(-1);
    else if (mods == Mod1Mask && ks == XK_F4 && active_client) close_client(active_client);
    else if (mods == Mod1Mask && ks == XK_F10 && active_client) set_maximized(active_client, !active_client->maximized);
    else if (mods == Mod1Mask && ks == XK_F9 && active_client) minimize(active_client);
    else if (mods == Mod4Mask && ks == XK_F1) run_media_action(MEDIA_VOLUME_MUTE);
    else if (mods == Mod4Mask && ks == XK_F2) run_media_action(MEDIA_VOLUME_DOWN);
    else if (mods == Mod4Mask && ks == XK_F3) run_media_action(MEDIA_VOLUME_UP);
    else if (mods == Mod4Mask && (ks == XK_Return || ks == XK_KP_Enter)) spawn(detect_terminal());
    else if (mods == Mod4Mask && (ks == XK_d || ks == XK_D)) launcher_toggle(1);
    else if (mods == Mod4Mask && (ks == XK_q || ks == XK_Q) && active_client) close_client(active_client);
    else if (mods == Mod4Mask && (ks == XK_m || ks == XK_M)) tool_open(TOOL_SYSMON);
    else if (mods == Mod4Mask && ks == XK_comma) tool_open(TOOL_SETTINGS);
}

static void grab_shortcuts(void)
{
    static const struct { KeySym ks; unsigned int mod; } grabs[] = {
        { XK_Tab, Mod1Mask }, { XK_Tab, Mod1Mask | ShiftMask },
        { XK_F4, Mod1Mask }, { XK_F10, Mod1Mask }, { XK_F9, Mod1Mask },
        { XK_Return, Mod4Mask }, { XK_d, Mod4Mask }, { XK_D, Mod4Mask },
        { XK_q, Mod4Mask }, { XK_Q, Mod4Mask },
        { XK_m, Mod4Mask }, { XK_M, Mod4Mask }, { XK_comma, Mod4Mask },
        { XK_F1, Mod4Mask }, { XK_F2, Mod4Mask }, { XK_F3, Mod4Mask },
        { XF86XK_AudioMute, 0 }, { XF86XK_AudioLowerVolume, 0 }, { XF86XK_AudioRaiseVolume, 0 },
        { XF86XK_AudioPlay, 0 }, { XF86XK_AudioPause, 0 },
        { XF86XK_AudioNext, 0 }, { XF86XK_AudioPrev, 0 },
        { XF86XK_MonBrightnessUp, 0 }, { XF86XK_MonBrightnessDown, 0 },
    };
    /* also with CapsLock / NumLock latched */
    static const unsigned int extras[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    for (unsigned int g = 0; g < sizeof(grabs) / sizeof(grabs[0]); g++) {
        KeyCode kc = XKeysymToKeycode(dpy, grabs[g].ks);
        if (!kc) continue;
        for (unsigned int e = 0; e < 4; e++)
            XGrabKey(dpy, kc, grabs[g].mod | extras[e], root, True,
                     GrabModeAsync, GrabModeAsync);
    }
}

/* --------------------------------------------------------------- config/pins */
static void pin_set_glyph(struct pin *p, const char *glyph)
{
    if (glyph && *glyph) { snprintf(p->glyph, sizeof(p->glyph), "%s", glyph); return; }
    p->glyph[0] = p->label[0] ? (char)toupper((unsigned char)p->label[0]) : 'A';
    p->glyph[1] = 0;
}

static int desktop_cols(void)
{
    int cols = (screen_w - 32) / DESK_COL_W;
    return cols > 0 ? cols : 1;
}

static int desktop_slot_count(void)
{
    int rows = (screen_h - bar_h - 32) / DESK_ROW_H;
    if (rows < 1) rows = 1;
    int count = desktop_cols() * rows;
    return count > 1024 ? 1024 : count;
}

static void build_pin_list(const char *spec, struct pin *list, int *count)
{
    *count = 0;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", spec ? spec : "");
    for (char *tok = strtok(buf, ","); tok && *count < MAX_PINS; tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        char *end = tok + strlen(tok);
        while (end > tok && end[-1] == ' ') *--end = 0;
        if (!*tok) continue;
        if (!strcasecmp(tok, "web-browser") || !strcasecmp(tok, "browser")) continue;

        struct pin *p = &list[*count];
        memset(p, 0, sizeof(*p));
        p->slot = -1;
        int core = -1;
        for (unsigned int i = 0; i < sizeof(core_apps) / sizeof(core_apps[0]); i++) {
            const char *id = core_apps[i].action == CORE_TERMINAL ? "terminal" :
                             core_apps[i].action == CORE_FILES ? "files" :
                             core_apps[i].action == CORE_SYSMON ? "sysmon" : "settings";
            if (!strcasecmp(tok, id) || !strcasecmp(tok, core_apps[i].name)) core = (int)i;
        }
        if (core >= 0) {
            snprintf(p->label, sizeof(p->label), "%s", core_apps[core].name);
            snprintf(p->glyph, sizeof(p->glyph), "%s", core_apps[core].glyph);
            snprintf(p->icon, sizeof(p->icon), "%s", core_apps[core].icon);
            p->action = core_apps[core].action;
            (*count)++;
            continue;
        }

        int found = -1;
        for (int i = 0; i < napps; i++) if (!strcasecmp(apps[i].id, tok)) found = i;
        if (found < 0)
            for (int i = 0; i < napps; i++)
                if (ci_contains(apps[i].id, tok) || ci_contains(apps[i].name, tok)) { found = i; break; }
        if (found >= 0) {
            char cmd[512];
            launch_cmd_for(&apps[found], cmd, sizeof(cmd));
            snprintf(p->command, sizeof(p->command), "%s", cmd);
            snprintf(p->label, sizeof(p->label), "%s", apps[found].name);
            snprintf(p->match, sizeof(p->match), "%s", apps[found].id);
            snprintf(p->icon, sizeof(p->icon), "%s", apps[found].icon[0] ? apps[found].icon : apps[found].id);
            pin_set_glyph(p, "");
            (*count)++;
            continue;
        }

        snprintf(p->label, sizeof(p->label), "%s", tok);
        snprintf(p->command, sizeof(p->command), "%s", tok);
        snprintf(p->match, sizeof(p->match), "%s", tok);
        snprintf(p->icon, sizeof(p->icon), "%s", tok);
        p->label[0] = (char)toupper((unsigned char)p->label[0]);
        pin_set_glyph(p, "");
        (*count)++;
    }
}

static void desktop_normalize_slots(void)
{
    int total = desktop_slot_count();
    if (total < 1) total = 1;
    unsigned char used[1024] = { 0 };
    char requested[512];
    snprintf(requested, sizeof(requested), "%s", cfg.desktop_slots);
    char *tok = strtok(requested, ",");
    for (int i = 0; i < ndesk_pins; i++) {
        int slot = tok ? (int)strtol(tok, NULL, 10) : -1;
        tok = strtok(NULL, ",");
        if (slot < 0 || slot >= total || used[slot]) {
            slot = 0;
            while (slot < total && used[slot]) slot++;
            if (slot >= total) slot = i % total;
        }
        desk_pins[i].slot = slot;
        used[slot] = 1;
    }
}

static void build_pins(void)
{
    build_pin_list(cfg.pins, pins, &npins);
    build_pin_list(cfg.desktop_pins, desk_pins, &ndesk_pins);
    desktop_normalize_slots();
}

/* ------------------------------------------------------------------ main */
static char config_path[512];

static void load_config(void)
{
    config_defaults(&cfg);
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0]) snprintf(config_path, sizeof(config_path), "%s/scoswm/scoswm.conf", xdg);
    else if (home && home[0]) snprintf(config_path, sizeof(config_path), "%s/.config/scoswm/scoswm.conf", home);
    else snprintf(config_path, sizeof(config_path), "scoswm.conf");
    config_load_file(config_path, &cfg);
    if (!cfg.has_desktop_pins) {
        snprintf(cfg.desktop_pins, sizeof(cfg.desktop_pins), "%s", cfg.pins);
        cfg.has_desktop_pins = 1;
    }
    const struct scos_theme *t = theme_by_id(cfg.theme);
    if (t) theme = *t;
    else theme_apply_custom(&theme, &cfg);
    bar_h = cfg.bar_height;
}

static void apply_palette(void)
{
    PX.main = alloc_rgb(theme.main);
    PX.bg_top = alloc_rgb(theme.bg_top);
    PX.bg_bot = alloc_rgb(theme.bg_bot);
    PX.win_bg = alloc_rgb(theme.win_bg);
    PX.text = alloc_rgb(theme.text);
    PX.title_text = alloc_rgb(theme.title_text);
    PX.taskbar_bg = alloc_rgb(theme.taskbar_bg);
    PX.title_off = alloc_rgb(color_blend_rgb(theme.main, theme.win_bg, 55));
    PX.bar_hi = alloc_rgb(color_blend_rgb(theme.taskbar_bg, theme.main, 45));
    PX.grid = alloc_rgb(color_blend_rgb(theme.bg_top, theme.main, 14));
    PX.dark = alloc_rgb(0x000000);
}

/* Publish one cursor choice through all three X11 configuration paths.
 * libXcursor/Qt 5 reads XCURSOR_THEME, Qt 6 commonly reads RESOURCE_MANAGER,
 * and GTK reads the XSETTINGS manager's Gtk/CursorThemeName. Setting only
 * the root cursor therefore left Breeze (or another theme) inside apps. */
static int cursor_resource_line(const char *line, size_t len)
{
    static const char *const keys[] = { "Xcursor.theme", "Xcursor.size" };
    while (len && (*line == ' ' || *line == '\t')) { line++; len--; }
    for (unsigned int k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        size_t n = strlen(keys[k]);
        if (len >= n && !strncmp(line, keys[k], n) &&
            (len == n || line[n] == ':' || line[n] == '=' ||
             line[n] == ' ' || line[n] == '\t')) return 1;
    }
    return 0;
}

static void publish_xresource_cursor(const char *theme_name, int cursor_size)
{
    Atom type = None; int format = 0;
    unsigned long n = 0, after = 0; unsigned char *data = NULL;
    /* First obtain the exact byte count, then read the complete database so
     * unrelated X resources survive and restarts do not append duplicates. */
    if (XGetWindowProperty(dpy, root, XA_RESOURCE_MANAGER, 0, 0, False, XA_STRING,
                           &type, &format, &n, &after, &data) != Success) return;
    if (data) { XFree(data); data = NULL; }
    if (after > 1024 * 1024) return;
    if (after) {
        Status status = XGetWindowProperty(dpy, root, XA_RESOURCE_MANAGER, 0, (long)((after + 3) / 4),
                                           False, XA_STRING, &type, &format, &n, &after, &data);
        if (status != Success) { if (data) XFree(data); return; }
    }
    size_t old_n = data && format == 8 ? n : 0;
    size_t cap = old_n + 256;
    char *out = calloc(1, cap);
    if (!out) { if (data) XFree(data); return; }
    size_t used = 0, pos = 0;
    while (pos < old_n) {
        size_t end = pos;
        while (end < old_n && data[end] != '\n') end++;
        if (!cursor_resource_line((const char *)data + pos, end - pos)) {
            size_t count = end - pos;
            memcpy(out + used, data + pos, count); used += count;
            if (end < old_n) out[used++] = '\n';
        }
        pos = end < old_n ? end + 1 : end;
    }
    if (used && out[used - 1] != '\n') out[used++] = '\n';
    int written = snprintf(out + used, cap - used, "Xcursor.theme: %s\nXcursor.size: %d\n",
                           theme_name, cursor_size);
    if (written > 0 && (size_t)written < cap - used) used += (size_t)written;
    XChangeProperty(dpy, root, XA_RESOURCE_MANAGER, XA_STRING, 8,
                    PropModeReplace, (unsigned char *)out, (int)used);
    free(out);
    if (data) XFree(data);
}

struct xsettings_buffer { unsigned char data[512]; size_t n; };
static void xs_u16(struct xsettings_buffer *b, unsigned int v)
{
    b->data[b->n++] = (unsigned char)v; b->data[b->n++] = (unsigned char)(v >> 8);
}
static void xs_u32(struct xsettings_buffer *b, unsigned long v)
{
    for (int i = 0; i < 4; i++) b->data[b->n++] = (unsigned char)(v >> (i * 8));
}
static void xs_bytes(struct xsettings_buffer *b, const char *s, size_t n)
{
    memcpy(b->data + b->n, s, n); b->n += n;
    while (b->n % 4) b->data[b->n++] = 0;
}
static void xs_name(struct xsettings_buffer *b, int type, const char *name)
{
    b->data[b->n++] = (unsigned char)type; b->data[b->n++] = 0;
    xs_u16(b, (unsigned int)strlen(name));
    xs_bytes(b, name, strlen(name));
    xs_u32(b, 1);                 /* last-change serial */
}
static void xs_string(struct xsettings_buffer *b, const char *name, const char *value)
{
    xs_name(b, 1, name);
    xs_u32(b, strlen(value)); xs_bytes(b, value, strlen(value));
}
static void xs_integer(struct xsettings_buffer *b, const char *name, int value)
{
    xs_name(b, 0, name); xs_u32(b, (unsigned long)(long)value);
}

static void publish_xsettings_cursor(const char *theme_name, int cursor_size)
{
    char selection_name[32];
    snprintf(selection_name, sizeof(selection_name), "_XSETTINGS_S%d", scr_no);
    Atom selection = XInternAtom(dpy, selection_name, False);
    Atom property = XInternAtom(dpy, "_XSETTINGS_SETTINGS", False);
    Atom manager = XInternAtom(dpy, "MANAGER", False);
    if (!xsettings_win)
        xsettings_win = XCreateSimpleWindow(dpy, root, -1, -1, 1, 1, 0, 0, 0);

    struct xsettings_buffer b; memset(&b, 0, sizeof(b));
    b.data[b.n++] = 0;            /* little-endian */
    b.data[b.n++] = 0;
    b.data[b.n++] = 0;
    b.data[b.n++] = 0;
    xs_u32(&b, 1);                /* settings serial */
    xs_u32(&b, 2);                /* two records */
    xs_string(&b, "Gtk/CursorThemeName", theme_name);
    xs_integer(&b, "Gtk/CursorThemeSize", cursor_size);
    XChangeProperty(dpy, xsettings_win, property, property, 8,
                    PropModeReplace, b.data, (int)b.n);
    XSetSelectionOwner(dpy, selection, xsettings_win, CurrentTime);

    XEvent ev; memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage; ev.xclient.window = root;
    ev.xclient.message_type = manager; ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime; ev.xclient.data.l[1] = (long)selection;
    ev.xclient.data.l[2] = (long)xsettings_win;
    XSendEvent(dpy, root, False, StructureNotifyMask, &ev);
}

static void publish_cursor_settings(void)
{
    const char *from_env = getenv("XCURSOR_THEME");
    char theme_name[128];
    snprintf(theme_name, sizeof(theme_name), "%s", from_env && *from_env ? from_env : "SCos");
    theme_name[strcspn(theme_name, "\r\n")] = 0;
    if (!theme_name[0]) snprintf(theme_name, sizeof(theme_name), "SCos");
    const char *size_env = getenv("XCURSOR_SIZE");
    int cursor_size = size_env ? (int)strtol(size_env, NULL, 10) : 24;
    if (cursor_size < 12 || cursor_size > 96) cursor_size = 24;
    publish_xresource_cursor(theme_name, cursor_size);
    publish_xsettings_cursor(theme_name, cursor_size);
}

/* Use the same themed left_ptr on the desktop and WM chrome. libXcursor is
 * loaded at runtime so the WM keeps its libX11-only link line; without it
 * we fall back to the core font cursor. */
typedef Cursor (*xcursor_load_fn)(Display *, const char *);
static void setup_cursor(void)
{
    void *h = dlopen("libXcursor.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libXcursor.so", RTLD_NOW | RTLD_LOCAL);
    if (h) {
        xcursor_load_fn load = (xcursor_load_fn)dlsym(h, "XcursorLibraryLoadCursor");
        if (load) wm_cursor = load(dpy, "left_ptr");
    }
    if (!wm_cursor) wm_cursor = XCreateFontCursor(dpy, XC_left_ptr);
    if (wm_cursor) XDefineCursor(dpy, root, wm_cursor);
}

static void create_ui_windows(void)
{
    XSetWindowAttributes at;
    at.override_redirect = True;
    if (wm_cursor) at.cursor = wm_cursor;

    /* The rule is real dock chrome, not just a wallpaper mark. It is later
     * kept directly below the taskbar and directly above app frames. */
    at.background_pixel = PX.main;
    at.event_mask = ExposureMask;
    edge_win = XCreateWindow(dpy, root, desktop_right_rule(), 0, DESK_RULE_W, screen_h - bar_h, 0,
                             CopyFromParent, InputOutput, CopyFromParent,
                             CWBackPixel | CWEventMask | CWOverrideRedirect, &at);
    XStoreName(dpy, edge_win, "SCosWM desktop right rule");
    XChangeProperty(dpy, edge_win, atoms[A_NET_WM_WINDOW_TYPE], XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&atoms[A_NET_WM_WINDOW_TYPE_DOCK], 1);
    /* The rule is paint-only chrome. An empty input region lets clicks pass
     * through to application titlebars and contents beneath its pixels. If
     * SHAPE is unavailable, retain the wallpaper fallback rather than put an
     * input-blocking strip over the desktop. */
    int shape_event = 0, shape_error = 0;
    if (XShapeQueryExtension(dpy, &shape_event, &shape_error)) {
        XShapeCombineRectangles(dpy, edge_win, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
        edge_input_passthrough = 1;
        XMapWindow(dpy, edge_win);
    }

    at.background_pixel = PX.taskbar_bg;
    at.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                    PointerMotionMask | LeaveWindowMask;
    bar_win = XCreateWindow(dpy, root, 0, screen_h - bar_h, screen_w, bar_h, 0,
                            CopyFromParent, InputOutput, CopyFromParent,
                            CWBackPixel | CWEventMask | CWOverrideRedirect |
                            (wm_cursor ? CWCursor : 0), &at);
    XChangeProperty(dpy, bar_win, atoms[A_NET_WM_WINDOW_TYPE], XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&atoms[A_NET_WM_WINDOW_TYPE_DOCK], 1);
    long strut[12] = { 0 };
    strut[3] = bar_h;                       /* bottom */
    strut[11] = screen_w;                   /* bottom_end_x */
    XChangeProperty(dpy, bar_win, atoms[A_NET_WM_STRUT_PARTIAL], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)strut, 12);
    XMapWindow(dpy, bar_win);

    at.background_pixel = PX.win_bg;
    at.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                    PointerMotionMask | LeaveWindowMask;
    launch_win = XCreateWindow(dpy, root, 8, screen_h - bar_h - 312, 300, 308, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWBackPixel | CWEventMask | CWOverrideRedirect |
                               (wm_cursor ? CWCursor : 0), &at);
    menu_win = XCreateWindow(dpy, root, 100, 100, 190, 100, 0,
                             CopyFromParent, InputOutput, CopyFromParent,
                             CWBackPixel | CWEventMask | CWOverrideRedirect |
                             (wm_cursor ? CWCursor : 0), &at);
    tool_win = XCreateWindow(dpy, root, 100, 100, TOOL_W, TOOL_H, 0,
                             CopyFromParent, InputOutput, CopyFromParent,
                             CWBackPixel | CWEventMask | CWOverrideRedirect |
                             (wm_cursor ? CWCursor : 0), &at);
    at.event_mask = 0;
    anim_win = XCreateWindow(dpy, root, 0, 0, 4, 4, 0,
                             CopyFromParent, InputOutput, CopyFromParent,
                             CWBackPixel | CWEventMask | CWOverrideRedirect, &at);
    at.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    overview_win = XCreateWindow(dpy, root, 0, 0, screen_w, overview_screen_height(), 0,
                                 CopyFromParent, InputOutput, CopyFromParent,
                                 CWBackPixel | CWEventMask | CWOverrideRedirect |
                                 (wm_cursor ? CWCursor : 0), &at);
    XStoreName(dpy, overview_win, "SCosWM window overview");
    Window buffered[] = { bar_win, launch_win, menu_win, tool_win, anim_win, overview_win };
    for (unsigned int i = 0; i < sizeof(buffered) / sizeof(buffered[0]); i++)
        XSetWindowBackgroundPixmap(dpy, buffered[i], None);
}


/* ------------------------------------------------------ core app panels */
/* Settings and SysMon are deliberately the only built-in windows.
 * They add no daemon and no background work: Settings only writes the
 * existing config file, and SysMon only samples /proc + /sys while it
 * is visible. Terminal and Files remain the user's real Linux programs. */
static int settings_field;              /* 0 none, 1 terminal, 2 files, 3 taskbar, 4 desktop */
static char settings_edit[1024];
static char settings_status[160];

static struct {
    double cpu_percent, load1, uptime_seconds;
    struct linux_meminfo mem;
    int processes, have_cpu_prev;
    unsigned long long prev_idle, prev_total;
    char hostname[128], kernel[256], gpu[256], temperature[96];
} sysmon_state;

static int read_small_file(const char *path, char *buf, int cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return 1;
}

static void ensure_parent_dirs(const char *path)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(buf, 0700); *p = '/'; }
    }
}

static void save_settings(const char *what)
{
    ensure_parent_dirs(config_path);
    if (config_save_file(config_path, &cfg))
        snprintf(settings_status, sizeof(settings_status), "%s saved", what);
    else
        snprintf(settings_status, sizeof(settings_status), "Could not save %.140s", config_path);
}

static void redraw_everything(void)
{
    XSetWindowBackground(dpy, bar_win, PX.taskbar_bg);
    XSetWindowBackground(dpy, launch_win, PX.win_bg);
    XSetWindowBackground(dpy, menu_win, PX.win_bg);
    XSetWindowBackground(dpy, tool_win, PX.win_bg);
    if (edge_win) {
        XSetWindowBackground(dpy, edge_win, PX.main);
        draw_right_rule();
    }
    XSetWindowBackground(dpy, overview_win, PX.bg_top);
    for (int i = 0; i < nclients; i++) {
        XSetWindowBackground(dpy, clients[i]->frame, PX.win_bg);
        draw_frame(clients[i]);
    }
    draw_desktop();
    draw_bar();
    if (launch_open) draw_launcher();
    if (menu_kind != MENU_NONE) draw_menu();
    if (overview_active) draw_overview();
}

static void apply_theme_live(int idx)
{
    const struct scos_theme *t = theme_by_index(idx);
    if (!t) return;
    theme = *t;
    snprintf(cfg.theme, sizeof(cfg.theme), "%s", t->id);
    cfg.has_custom = 0;
    apply_palette();
    redraw_everything();
    save_settings("Theme");
}

static void apply_bar_height_live(int v)
{
    if (v < 28) v = 28;
    if (v > 64) v = 64;
    cfg.bar_height = v;
    bar_h = v;
    desktop_normalize_slots();
    pins_sync_config();
    for (int i = 0; i < nclients; i++) {
        xicon_destroy_owned(clients[i]->icon);
        clients[i]->icon = client_load_icon(clients[i]);
    }
    XMoveResizeWindow(dpy, edge_win, desktop_right_rule(), 0, DESK_RULE_W, screen_h - bar_h);
    XMoveResizeWindow(dpy, bar_win, 0, screen_h - bar_h, screen_w, bar_h);
    int x, y, w, h; work_area(&x, &y, &w, &h);
    for (int i = 0; i < nclients; i++) {
        struct client *c = clients[i];
        if (c->maximized) {
            c->x = x; c->y = y; c->w = w - 2; c->h = h - TOP_H - 1;
            client_place(c); draw_frame(c);
        }
    }
    recalc_struts();
    redraw_everything();
    save_settings("Taskbar height");
}

static void apply_desktop_icon_size_live(int size)
{
    if (size != 24 && size != 32 && size != 48) return;
    cfg.desktop_icon_size = size;
    draw_desktop();
    save_settings("Desktop icon size");
}

static char *settings_target(int field, size_t *cap)
{
    if (field == 1) { *cap = sizeof(cfg.terminal); return cfg.terminal; }
    if (field == 2) { *cap = sizeof(cfg.files); return cfg.files; }
    if (field == 3) { *cap = sizeof(cfg.pins); return cfg.pins; }
    if (field == 4) { *cap = sizeof(cfg.desktop_pins); return cfg.desktop_pins; }
    return NULL;
}

static void start_settings_field(int field)
{
    size_t cap = 0;
    char *target = settings_target(field, &cap);
    if (!target) return;
    settings_field = field;
    snprintf(settings_edit, sizeof(settings_edit), "%s", target);
    snprintf(settings_status, sizeof(settings_status), "Editing - Enter saves, Esc cancels");
}

static void commit_settings_field(void)
{
    size_t cap = 0;
    char *target = settings_target(settings_field, &cap);
    if (!target) { settings_field = 0; return; }
    char *s = settings_edit;
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    snprintf(target, cap, "%s", s);
    int was_pins = settings_field >= 3;
    settings_field = 0;
    if (was_pins) { build_pins(); pins_sync_config(); desk_sel_mask = 0; redraw_everything(); }
    save_settings("Settings");
}

static void detect_gpu_info(void)
{
    char buf[1024];
    sysmon_state.gpu[0] = 0;
    if (read_small_file("/proc/driver/nvidia/version", buf, sizeof(buf))) {
        const char *v = strstr(buf, "Kernel Module");
        if (v) {
            v += strlen("Kernel Module");
            while (*v == ' ' || *v == '\t') v++;
            char version[32]; int i = 0;
            while (v[i] && v[i] != ' ' && v[i] != '\t' && v[i] != '\n' && i < (int)sizeof(version) - 1) {
                version[i] = v[i]; i++;
            }
            version[i] = 0;
            snprintf(sysmon_state.gpu, sizeof(sysmon_state.gpu), "NVIDIA proprietary %s", version);
        } else snprintf(sysmon_state.gpu, sizeof(sysmon_state.gpu), "NVIDIA proprietary");
        return;
    }
    for (int card = 0; card < 4; card++) {
        char linkpath[96], target[256];
        snprintf(linkpath, sizeof(linkpath), "/sys/class/drm/card%d/device/driver", card);
        ssize_t n = readlink(linkpath, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = 0;
        const char *driver = strrchr(target, '/');
        driver = driver ? driver + 1 : target;
        if (!strcmp(driver, "nouveau")) {
            snprintf(sysmon_state.gpu, sizeof(sysmon_state.gpu), "NVIDIA GPU - nouveau (DRM/KMS, card%d)", card);
            return;
        }
        if (!strcmp(driver, "nvidia")) {
            snprintf(sysmon_state.gpu, sizeof(sysmon_state.gpu), "NVIDIA GPU - proprietary driver (card%d)", card);
            return;
        }
        if (!sysmon_state.gpu[0])
            snprintf(sysmon_state.gpu, sizeof(sysmon_state.gpu), "GPU driver: %.225s (card%d)", driver, card);
    }
    if (sysmon_state.gpu[0]) return;
    snprintf(sysmon_state.gpu, sizeof(sysmon_state.gpu), "GPU: no DRM driver exposed in /sys");
}

static void detect_temperature(void)
{
    sysmon_state.temperature[0] = 0;
    DIR *root_dir = opendir("/sys/class/hwmon");
    if (!root_dir) { snprintf(sysmon_state.temperature, sizeof(sysmon_state.temperature), "Temperature: unavailable"); return; }
    struct dirent *entry;
    while ((entry = readdir(root_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[512], name[96] = "", value[64] = "";
        snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", entry->d_name);
        if (!read_small_file(path, name, sizeof(name))) continue;
        char *nl = strchr(name, '\n'); if (nl) *nl = 0;
        if (!strstr(name, "nvidia") && !strstr(name, "nouveau") && !strstr(name, "coretemp") &&
            !strstr(name, "k10temp") && !strstr(name, "zenpower") && !strstr(name, "amdgpu")) continue;
        snprintf(path, sizeof(path), "/sys/class/hwmon/%s/temp1_input", entry->d_name);
        if (!read_small_file(path, value, sizeof(value))) continue;
        long milli = strtol(value, NULL, 10);
        if (milli > 0) {
            char celsius[64];
            snprintf(celsius, sizeof(celsius), "%ld.%d", milli / 1000, (int)((milli % 1000) / 100));
            snprintf(sysmon_state.temperature, sizeof(sysmon_state.temperature),
                     "%.70s: %.21s C", name, celsius);
            break;
        }
    }
    closedir(root_dir);
    if (!sysmon_state.temperature[0])
        snprintf(sysmon_state.temperature, sizeof(sysmon_state.temperature), "Temperature: no supported sensor exposed");
}

static void refresh_sysmon(void)
{
    char buf[8192];
    if (read_small_file("/proc/stat", buf, sizeof(buf))) {
        unsigned long long idle = 0, total = 0;
        if (parse_cpu_stat_text(buf, &idle, &total)) {
            if (sysmon_state.have_cpu_prev && total > sysmon_state.prev_total) {
                unsigned long long dt = total - sysmon_state.prev_total;
                unsigned long long di = idle - sysmon_state.prev_idle;
                sysmon_state.cpu_percent = dt ? 100.0 * (1.0 - (double)di / (double)dt) : 0;
            }
            sysmon_state.prev_idle = idle; sysmon_state.prev_total = total;
            sysmon_state.have_cpu_prev = 1;
        }
    }
    if (read_small_file("/proc/meminfo", buf, sizeof(buf)))
        parse_meminfo_text(buf, &sysmon_state.mem);
    if (read_small_file("/proc/loadavg", buf, sizeof(buf)))
        parse_loadavg_text(buf, &sysmon_state.load1, &sysmon_state.processes);
    if (read_small_file("/proc/uptime", buf, sizeof(buf)))
        sysmon_state.uptime_seconds = strtod(buf, NULL);
    gethostname(sysmon_state.hostname, sizeof(sysmon_state.hostname) - 1);
    struct utsname un;
    if (uname(&un) == 0)
        snprintf(sysmon_state.kernel, sizeof(sysmon_state.kernel), "%s %s (%s)", un.sysname, un.release, un.machine);
    else snprintf(sysmon_state.kernel, sizeof(sysmon_state.kernel), "Linux");
    detect_gpu_info();
    detect_temperature();
}

static void wifi_merge_activity(void)
{
    char selected_ssid[WIFI_SSID_MAX] = "", selected_device[WIFI_IFNAME_MAX] = "";
    if (wifi_selected >= 0 && wifi_selected < wifi_network_count) {
        snprintf(selected_ssid, sizeof(selected_ssid), "%s", wifi_networks[wifi_selected].ssid);
        snprintf(selected_device, sizeof(selected_device), "%s", wifi_networks[wifi_selected].device);
    }
    wifi_network_count = wifi_merge_adapter_activity(wifi_networks, wifi_network_count,
                                                     WIFI_MAX_NETWORKS,
                                                     wifi_adapters, wifi_adapter_count);
    wifi_selected = -1;
    for (int i = 0; i < wifi_network_count; i++)
        if (selected_ssid[0] && !strcmp(wifi_networks[i].ssid, selected_ssid) &&
            !strcmp(wifi_networks[i].device, selected_device)) { wifi_selected = i; break; }
    int maximum = wifi_network_count - WIFI_VISIBLE_NETWORKS;
    if (maximum < 0) maximum = 0;
    if (wifi_network_scroll > maximum) wifi_network_scroll = maximum;
}

static void wifi_refresh_local(void)
{
    wifi_adapter_count = wifi_scan_adapters(wifi_adapters, WIFI_MAX_ADAPTERS);
    wifi_connected_count = 0; wifi_hotspot_count = 0;
    for (int i = 0; i < wifi_adapter_count; i++) {
        if (wifi_adapters[i].connected) wifi_connected_count++;
        if (wifi_adapters[i].hosted) wifi_hotspot_count++;
    }
    wifi_merge_activity();
}

static void wifi_job_finish(void)
{
    if (wifi_job.fd < 0) return;
    close(wifi_job.fd); wifi_job.fd = -1; wifi_job.pid = 0;
    wifi_job.output[wifi_job.length] = 0;
    if (wifi_job.kind == 1) {
        wifi_network_count = wifi_parse_nmcli_networks(wifi_job.output, wifi_networks, WIFI_MAX_NETWORKS);
        wifi_selected = -1; wifi_network_scroll = 0;
        wifi_merge_activity();
        if (wifi_network_count) {
            snprintf(wifi_status, sizeof(wifi_status), "%d rows, active first - select a network and card",
                     wifi_network_count);
        } else {
            char *nl = strpbrk(wifi_job.output, "\r\n"); if (nl) *nl = 0;
            snprintf(wifi_status, sizeof(wifi_status), "%.191s", wifi_job.output[0] ?
                     wifi_job.output : "No Wi-Fi networks reported");
        }
    } else if (wifi_job.kind == 2) {
        char *nl = strpbrk(wifi_job.output, "\r\n"); if (nl) *nl = 0;
        snprintf(wifi_status, sizeof(wifi_status), "%.191s", wifi_job.output[0] ?
                 wifi_job.output : "Connection request finished");
        wifi_refresh_local();
    }
    wifi_job.kind = 0;
    if (tool_kind == TOOL_WIFI) draw_tool();
    draw_bar();
}

static void wifi_job_read(void)
{
    if (wifi_job.fd < 0) return;
    for (int round = 0; round < 16; round++) {
        char buffer[4096];
        ssize_t n = read(wifi_job.fd, buffer, sizeof(buffer));
        if (n > 0) {
            size_t available = sizeof(wifi_job.output) - 1 - wifi_job.length;
            size_t copy = (size_t)n < available ? (size_t)n : available;
            memcpy(wifi_job.output + wifi_job.length, buffer, copy);
            wifi_job.length += copy;
            continue;
        }
        if (n == 0) { wifi_job_finish(); return; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        wifi_job_finish(); return;
    }
}

static int wifi_job_start(int kind, const struct wifi_network *network, const char *password)
{
    if (wifi_job.fd >= 0) {
        snprintf(wifi_status, sizeof(wifi_status), "Another Wi-Fi operation is still running");
        return 0;
    }
    if (!cmd_exists("nmcli")) {
        snprintf(wifi_status, sizeof(wifi_status),
                 "nmcli is needed for scan/connect; native adapter status remains available");
        return 0;
    }
    int pipes[2];
    if (pipe(pipes) != 0) {
        snprintf(wifi_status, sizeof(wifi_status), "Cannot start Wi-Fi operation: %s", strerror(errno));
        return 0;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(pipes[0]);
        dup2(pipes[1], STDOUT_FILENO); dup2(pipes[1], STDERR_FILENO);
        close(pipes[1]); close(ConnectionNumber(dpy));
        if (kind == 1)
            execlp("nmcli", "nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY,DEVICE",
                   "device", "wifi", "list", (char *)NULL);
        else if (network && password && *password)
            execlp("nmcli", "nmcli", "device", "wifi", "connect", network->ssid,
                   "password", password, "ifname", network->device, (char *)NULL);
        else if (network)
            execlp("nmcli", "nmcli", "device", "wifi", "connect", network->ssid,
                   "ifname", network->device, (char *)NULL);
        dprintf(STDERR_FILENO, "Could not execute nmcli: %s\n", strerror(errno));
        _exit(127);
    }
    if (pid < 0) {
        close(pipes[0]); close(pipes[1]);
        snprintf(wifi_status, sizeof(wifi_status), "Cannot start nmcli: %s", strerror(errno));
        return 0;
    }
    close(pipes[1]);
    int flags = fcntl(pipes[0], F_GETFL, 0);
    if (flags >= 0) fcntl(pipes[0], F_SETFL, flags | O_NONBLOCK);
    wifi_job.fd = pipes[0]; wifi_job.kind = kind; wifi_job.pid = pid; wifi_job.length = 0;
    if (kind == 1) snprintf(wifi_status, sizeof(wifi_status), "Scanning networks without blocking the desktop...");
    else snprintf(wifi_status, sizeof(wifi_status), "Connecting %.70s on %.20s...",
                  network->ssid, network->device);
    return 1;
}

static void wifi_connect_selected(void)
{
    if (wifi_selected < 0 || wifi_selected >= wifi_network_count) {
        snprintf(wifi_status, sizeof(wifi_status), "Select a network row first");
        return;
    }
    struct wifi_network *network = &wifi_networks[wifi_selected];
    if (network->hosted) {
        snprintf(wifi_status, sizeof(wifi_status), "This is your hosted hotspot, not a network to join");
        return;
    }
    if (!network->ssid[0] || !network->device[0]) {
        snprintf(wifi_status, sizeof(wifi_status), "A hidden SSID needs NetworkManager connection settings");
        return;
    }
    int secured = network->security[0] && strcmp(network->security, "--");
    size_t length = strlen(wifi_password);
    if (secured && strstr(network->security, "WPA") && length && length < 8) {
        snprintf(wifi_status, sizeof(wifi_status), "WPA passwords contain at least 8 characters");
        return;
    }
    if (wifi_job_start(2, network, wifi_password)) {
        wifi_password[0] = 0; wifi_password_editing = 0;
    }
}

static void wifi_scroll_networks(int amount)
{
    int maximum = wifi_network_count - WIFI_VISIBLE_NETWORKS;
    if (maximum < 0) maximum = 0;
    wifi_network_scroll += amount;
    if (wifi_network_scroll < 0) wifi_network_scroll = 0;
    if (wifi_network_scroll > maximum) wifi_network_scroll = maximum;
}

static void wifi_job_stop(void)
{
    if (wifi_job.fd >= 0) close(wifi_job.fd);
    if (wifi_job.pid > 0) kill(wifi_job.pid, SIGHUP);
    wifi_job.fd = -1; wifi_job.pid = 0; wifi_job.kind = 0;
}

static void tool_close(void)
{
    if (tool_kind == TOOL_NONE) return;
    tool_kind = TOOL_NONE;
    tool_hover = TOOL_HOVER_NONE;
    tool_release_pending = 0;
    settings_field = 0;
    wifi_password_editing = 0;
    XUnmapWindow(dpy, tool_win);
    XUngrabKeyboard(dpy, CurrentTime);
}

static void tool_open(int kind)
{
    menu_close();
    if (launch_open) launcher_toggle(0);
    tool_kind = kind;
    tool_hover = TOOL_HOVER_NONE;
    tool_release_pending = 1;
    settings_field = 0;
    if (kind == TOOL_SYSMON) refresh_sysmon();
    if (kind == TOOL_WIFI) {
        wifi_password_editing = 0; wifi_selected = -1;
        wifi_refresh_local();
        if (wifi_job.fd < 0) wifi_job_start(1, NULL, NULL);
    }
    int x = (screen_w - TOOL_W) / 2, y = (screen_h - bar_h - TOOL_H) / 2;
    if (x < 4) x = 4;
    if (y < 4) y = 4;
    XMoveResizeWindow(dpy, tool_win, x, y, TOOL_W, TOOL_H);
    XMapWindow(dpy, tool_win);
    XRaiseWindow(dpy, tool_win);
    XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    draw_tool();
}

static void draw_meter(Drawable d, const char *label, double frac, const char *value, int y)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    text_out(d, 16, y + font->ascent, label, PX.text);
    rect(d, 112, y, 300, 16, PX.main);
    fill(d, 113, y + 1, (int)(298 * frac), 14, PX.main);
    text_out(d, 424, y + font->ascent, value, PX.text);
}

static Pixmap tool_pm = None;
static int tool_pm_w, tool_pm_h;

static int tool_hover_at(int x, int y)
{
    if (x >= TOOL_W - 30 && x < TOOL_W && y >= 0 && y < 28) return TOOL_HOVER_CLOSE;
    if (tool_kind == TOOL_WIFI) {
        for (int i = 0; i < WIFI_VISIBLE_NETWORKS; i++) {
            int index = wifi_network_scroll + i;
            int row_y = WIFI_NETWORK_Y + i * 24;
            if (index < wifi_network_count && x >= 8 && x < 532 &&
                y >= row_y && y < row_y + 24) return TOOL_HOVER_NETWORK + i;
        }
        if (y >= 318 && y < 348) {
            if (x >= 100 && x < 330) return TOOL_HOVER_WIFI_PASSWORD;
            if (x >= 340 && x < 424) return TOOL_HOVER_WIFI_CONNECT;
            if (x >= 434 && x < 524) return TOOL_HOVER_WIFI_SCAN;
        }
    } else if (tool_kind == TOOL_SETTINGS) {
        if (y >= 62 && y < 102) {
            for (int i = 0; i < scos_theme_count; i++) {
                int theme_x = 16 + i * 128;
                if (x >= theme_x && x < theme_x + 120) return TOOL_HOVER_THEME + i;
            }
        }
        for (int field = 1; field <= 4; field++) {
            int field_y = 116 + (field - 1) * 36;
            if (x >= 100 && x < 524 && y >= field_y && y < field_y + 30)
                return TOOL_HOVER_FIELD + field;
        }
        if (y >= 268 && y < 298) {
            if (x >= 100 && x < 134) return TOOL_HOVER_BAR_DEC;
            if (x >= 204 && x < 238) return TOOL_HOVER_BAR_INC;
            if (x >= 300 && x < 334) return TOOL_HOVER_ICON_DEC;
            if (x >= 386 && x < 420) return TOOL_HOVER_ICON_INC;
            if (x >= 440 && x < 524) return TOOL_HOVER_GRID;
        }
    }
    return TOOL_HOVER_NONE;
}

static int tool_item_lit(int id, int selected)
{
    return selected || tool_hover == id;
}

static void tool_draw_box(Drawable d, int x, int y, int w, int h, int id, int selected)
{
    fill(d, x, y, w, h, tool_item_lit(id, selected) ? PX.main : PX.win_bg);
    rect(d, x, y, w, h, PX.main);
}

static void draw_tool(void)
{
    if (tool_kind == TOOL_NONE) return;
    Pixmap pm = surface_backing(tool_win, &tool_pm, &tool_pm_w, &tool_pm_h, TOOL_W, TOOL_H);
    Drawable d = pm ? pm : tool_win;
    fill(d, 0, 0, TOOL_W, TOOL_H, PX.win_bg);
    rect(d, 0, 0, TOOL_W, TOOL_H, PX.main);
    fill(d, 1, 1, TOOL_W - 2, 25, PX.main);
    text_out(d, 10, 6 + font->ascent,
             tool_kind == TOOL_SETTINGS ? "Settings" : tool_kind == TOOL_WIFI ? "Wi-Fi" : "SysMon",
             PX.title_text);
    int close_lit = tool_hover == TOOL_HOVER_CLOSE;
    fill(d, TOOL_W - 29, 2, 24, 22, close_lit ? PX.title_text : PX.main);
    draw_button_glyph(d, TOOL_W - 27, 4, 2, close_lit ? PX.main : PX.title_text);

    if (tool_kind == TOOL_WIFI) {
        char line[768];
        snprintf(line, sizeof(line), "%d connected network%s  |  %d hosted hotspot%s  |  %d adapter%s",
                 wifi_connected_count, wifi_connected_count == 1 ? "" : "s",
                 wifi_hotspot_count, wifi_hotspot_count == 1 ? "" : "s",
                 wifi_adapter_count, wifi_adapter_count == 1 ? "" : "s");
        clipped_text(d, 16, 42, line, 508, PX.text);
        int shown = wifi_adapter_count > 4 ? 3 : wifi_adapter_count;
        for (int i = 0; i < shown; i++) {
            struct wifi_adapter *adapter = &wifi_adapters[i];
            char signal[48] = "";
            if (adapter->signal_dbm > -1000) snprintf(signal, sizeof(signal), " %d dBm", adapter->signal_dbm);
            else if (adapter->quality >= 0) snprintf(signal, sizeof(signal), " q%d", adapter->quality);
            char state[160];
            if (adapter->hosted) snprintf(state, sizeof(state), "Hotspot: %.146s", adapter->ssid);
            else snprintf(state, sizeof(state), "%.159s",
                          adapter->connected ? adapter->ssid : (adapter->enabled ? "not connected" : "disabled"));
            snprintf(line, sizeof(line), "%.15s  %.128s%.47s%s%.63s", adapter->name,
                     state, signal, adapter->address[0] ? "  " : "", adapter->address);
            clipped_text(d, 16, 62 + i * 20, line, 508,
                         adapter->connected || adapter->hosted ? PX.main : PX.text);
        }
        if (wifi_adapter_count > 4) {
            snprintf(line, sizeof(line), "+%d more adapters (all remain available to nmcli)",
                     wifi_adapter_count - 3);
            clipped_text(d, 16, 122, line, 508, PX.text);
        } else if (!wifi_adapter_count) {
            clipped_text(d, 16, 62, "No native wireless adapter detected", 508, PX.title_off);
        }
        text_out(d, 16, 140, "Networks - active first (* connected, H hosted)", PX.main);
        if (!wifi_network_count)
            clipped_text(d, 16, WIFI_NETWORK_Y + 18,
                         wifi_job.kind == 1 ? "Scanning..." : "No scan results", 508, PX.text);
        for (int i = 0; i < WIFI_VISIBLE_NETWORKS; i++) {
            int index = wifi_network_scroll + i;
            if (index >= wifi_network_count) break;
            struct wifi_network *network = &wifi_networks[index];
            int y = WIFI_NETWORK_Y + i * 24;
            int selected = index == wifi_selected;
            int lit = tool_item_lit(TOOL_HOVER_NETWORK + i, selected);
            if (lit) fill(d, 8, y, 524, 24, PX.main);
            char signal_text[16], state[64];
            if (network->signal >= 0) snprintf(signal_text, sizeof(signal_text), "%d%%", network->signal);
            else snprintf(signal_text, sizeof(signal_text), "--");
            snprintf(state, sizeof(state), "%.63s", network->hosted ? "hosted hotspot" :
                     network->in_use ? "connected" : network->security);
            snprintf(line, sizeof(line), "%s %.200s  %.15s  %.7s  %.45s",
                     network->hosted ? "H" : network->in_use ? "*" : " ",
                     network->ssid[0] ? network->ssid : "<hidden>", network->device,
                     signal_text, state);
            clipped_text(d, 14, y + 6 + font->ascent, line, 512,
                         lit ? PX.title_text : network->in_use ? PX.main : PX.text);
        }
        text_out(d, 16, 327 + font->ascent, "Password", PX.main);
        int password_lit = tool_item_lit(TOOL_HOVER_WIFI_PASSWORD, wifi_password_editing);
        tool_draw_box(d, 100, 318, 230, 30, TOOL_HOVER_WIFI_PASSWORD, wifi_password_editing);
        if (wifi_password_editing) rect(d, 100, 318, 230, 30, PX.title_text);
        char masked[128];
        size_t password_n = strlen(wifi_password);
        if (password_n >= sizeof(masked)) password_n = sizeof(masked) - 1;
        memset(masked, '*', password_n); masked[password_n] = 0;
        clipped_text(d, 108, 327 + font->ascent,
                     wifi_password_editing ? masked : (password_n ? masked : "(stored/open)"), 214,
                     password_lit ? PX.title_text : PX.text);
        tool_draw_box(d, 340, 318, 84, 30, TOOL_HOVER_WIFI_CONNECT, 0);
        const char *connect_label = "Connect";
        if (wifi_selected >= 0 && wifi_selected < wifi_network_count) {
            if (wifi_networks[wifi_selected].hosted) connect_label = "Hosted";
            else if (wifi_networks[wifi_selected].in_use) connect_label = "Rejoin";
        }
        clipped_text(d, 346, 327 + font->ascent, connect_label, 72,
                     tool_item_lit(TOOL_HOVER_WIFI_CONNECT, 0) ? PX.title_text : PX.main);
        tool_draw_box(d, 434, 318, 90, 30, TOOL_HOVER_WIFI_SCAN, 0);
        text_out(d, 457, 327 + font->ascent, "Scan",
                 tool_item_lit(TOOL_HOVER_WIFI_SCAN, 0) ? PX.title_text : PX.main);
        clipped_text(d, 16, 360, wifi_status, 508, PX.text);
        clipped_text(d, 16, 380, "Joined networks and hosted hotspots stay separate by card; Esc closes", 508, PX.title_off);
        surface_present(tool_win, pm, TOOL_W, TOOL_H);
        return;
    }

    if (tool_kind == TOOL_SYSMON) {
        char value[64];
        snprintf(value, sizeof(value), "%.0f%%", sysmon_state.cpu_percent);
        draw_meter(d, "CPU", sysmon_state.cpu_percent / 100.0, value, 48);
        unsigned long used = sysmon_state.mem.total_kb > sysmon_state.mem.available_kb ?
            sysmon_state.mem.total_kb - sysmon_state.mem.available_kb : 0;
        double mem_frac = sysmon_state.mem.total_kb ? (double)used / sysmon_state.mem.total_kb : 0;
        snprintf(value, sizeof(value), "%.0f%%", mem_frac * 100.0);
        draw_meter(d, "Memory", mem_frac, value, 82);
        unsigned long swap_used = sysmon_state.mem.swap_total_kb > sysmon_state.mem.swap_free_kb ?
            sysmon_state.mem.swap_total_kb - sysmon_state.mem.swap_free_kb : 0;
        double swap_frac = sysmon_state.mem.swap_total_kb ? (double)swap_used / sysmon_state.mem.swap_total_kb : 0;
        snprintf(value, sizeof(value), "%.0f%%", swap_frac * 100.0);
        draw_meter(d, "Swap", swap_frac, value, 116);

        char line[512];
        int y = 164;
        snprintf(line, sizeof(line), "Host: %s", sysmon_state.hostname);
        clipped_text(d, 16, y, line, 508, PX.text); y += 26;
        snprintf(line, sizeof(line), "Kernel: %s", sysmon_state.kernel);
        clipped_text(d, 16, y, line, 508, PX.text); y += 26;
        unsigned long secs = (unsigned long)sysmon_state.uptime_seconds;
        snprintf(line, sizeof(line), "Uptime: %lud %02luh %02lum    Load: %.2f    Processes: %d",
                 secs / 86400, (secs / 3600) % 24, (secs / 60) % 60,
                 sysmon_state.load1, sysmon_state.processes);
        clipped_text(d, 16, y, line, 508, PX.text); y += 26;
        snprintf(line, sizeof(line), "RAM: %.1f / %.1f GiB    Swap: %.1f / %.1f GiB",
                 used / 1048576.0, sysmon_state.mem.total_kb / 1048576.0,
                 swap_used / 1048576.0, sysmon_state.mem.swap_total_kb / 1048576.0);
        clipped_text(d, 16, y, line, 508, PX.text); y += 26;
        snprintf(line, sizeof(line), "GPU: %s", sysmon_state.gpu);
        clipped_text(d, 16, y, line, 508, PX.text); y += 26;
        clipped_text(d, 16, y, sysmon_state.temperature, 508, PX.text);
        text_out(d, 16, TOOL_H - 18, "Live from /proc and /sys - refreshes only while open - Esc closes", PX.title_off);
        surface_present(tool_win, pm, TOOL_W, TOOL_H);
        return;
    }

    /* Settings */
    text_out(d, 16, 48 + font->ascent, "Theme", PX.main);
    for (int i = 0; i < scos_theme_count; i++) {
        int x = 16 + i * 128, y = 62;
        int selected = !strcmp(cfg.theme, scos_themes[i].id);
        int lit = tool_item_lit(TOOL_HOVER_THEME + i, selected);
        tool_draw_box(d, x, y, 120, 40, TOOL_HOVER_THEME + i, selected);
        int tw = text_w(scos_themes[i].name);
        text_out(d, x + (120 - tw) / 2, y + 14 + font->ascent, scos_themes[i].name,
                 lit ? PX.title_text : PX.text);
    }

    static const char *labels[5] = { "", "Terminal", "Files", "Taskbar pins", "Desktop pins" };
    for (int field = 1; field <= 4; field++) {
        int y = 116 + (field - 1) * 36;
        text_out(d, 16, y + 9 + font->ascent, labels[field], PX.main);
        int editing = settings_field == field;
        int lit = tool_item_lit(TOOL_HOVER_FIELD + field, editing);
        tool_draw_box(d, 100, y, 424, 30, TOOL_HOVER_FIELD + field, editing);
        if (editing) rect(d, 100, y, 424, 30, PX.title_text);
        char shown[1056];
        size_t cap = 0; char *target = settings_target(field, &cap); (void)cap;
        if (settings_field == field) snprintf(shown, sizeof(shown), "%s", settings_edit);
        else if (target && target[0]) snprintf(shown, sizeof(shown), "%s", target);
        else if (field == 1) snprintf(shown, sizeof(shown), "(auto: %s)", detect_terminal());
        else if (field == 2) {
            const char *e = detect_files_exec();
            snprintf(shown, sizeof(shown), "(auto: %s)", e ? e : "xdg-open");
        } else shown[0] = 0;
        clipped_text(d, 108, y + 9 + font->ascent, shown, 390, lit ? PX.title_text : PX.text);
        if (editing && (time(NULL) % 2) == 0) {
            int cx = 108 + text_w(settings_edit);
            if (cx < 518) fill(d, cx, y + 8, 2, 14, PX.title_text);
        }
    }

    text_out(d, 16, 277 + font->ascent, "Taskbar", PX.main);
    tool_draw_box(d, 100, 268, 34, 30, TOOL_HOVER_BAR_DEC, 0);
    text_out(d, 112, 277 + font->ascent, "-",
             tool_item_lit(TOOL_HOVER_BAR_DEC, 0) ? PX.title_text : PX.text);
    char bh[32]; snprintf(bh, sizeof(bh), "%d px", cfg.bar_height);
    text_out(d, 146, 277 + font->ascent, bh, PX.text);
    tool_draw_box(d, 204, 268, 34, 30, TOOL_HOVER_BAR_INC, 0);
    text_out(d, 216, 277 + font->ascent, "+",
             tool_item_lit(TOOL_HOVER_BAR_INC, 0) ? PX.title_text : PX.text);

    text_out(d, 250, 277 + font->ascent, "Icons", PX.main);
    tool_draw_box(d, 300, 268, 34, 30, TOOL_HOVER_ICON_DEC, 0);
    text_out(d, 312, 277 + font->ascent, "-",
             tool_item_lit(TOOL_HOVER_ICON_DEC, 0) ? PX.title_text : PX.text);
    char is[32]; snprintf(is, sizeof(is), "%d", cfg.desktop_icon_size);
    text_out(d, 346, 277 + font->ascent, is, PX.text);
    tool_draw_box(d, 386, 268, 34, 30, TOOL_HOVER_ICON_INC, 0);
    text_out(d, 398, 277 + font->ascent, "+",
             tool_item_lit(TOOL_HOVER_ICON_INC, 0) ? PX.title_text : PX.text);
    tool_draw_box(d, 440, 268, 84, 30, TOOL_HOVER_GRID, 0);
    text_out(d, 450, 277 + font->ascent, cfg.desktop_grid ? "Grid on" : "Grid off",
             tool_item_lit(TOOL_HOVER_GRID, 0) ? PX.title_text : PX.text);

    clipped_text(d, 16, 326, settings_status[0] ? settings_status :
                 "Click fields to edit. Drag shortcuts to rearrange or move between surfaces.", 508, PX.text);
    text_out(d, 16, TOOL_H - 18, "Single workspace by design - no pager or workspace daemon", PX.title_off);
    surface_present(tool_win, pm, TOOL_W, TOOL_H);
}

static int tool_button(XButtonEvent *e)
{
    if (e->type != ButtonPress) return 1;
    if (tool_kind == TOOL_WIFI && (e->button == Button4 || e->button == Button5)) {
        wifi_scroll_networks(e->button == Button4 ? -3 : 3); draw_tool(); return 1;
    }
    if (e->button != Button1) return 1;
    if (e->x >= TOOL_W - 30 && e->y < 28) { tool_close(); return 1; }
    if (tool_kind == TOOL_WIFI) {
        for (int i = 0; i < WIFI_VISIBLE_NETWORKS; i++) {
            int index = wifi_network_scroll + i;
            int y = WIFI_NETWORK_Y + i * 24;
            if (index < wifi_network_count && e->y >= y && e->y < y + 24) {
                wifi_selected = index; wifi_password_editing = 0; draw_tool(); return 1;
            }
        }
        if (e->x >= 100 && e->x < 330 && e->y >= 318 && e->y < 348) {
            wifi_password_editing = 1; draw_tool(); return 1;
        }
        if (e->x >= 340 && e->x < 424 && e->y >= 318 && e->y < 348) {
            wifi_connect_selected(); draw_tool(); return 1;
        }
        if (e->x >= 434 && e->x < 524 && e->y >= 318 && e->y < 348) {
            wifi_job_start(1, NULL, NULL); draw_tool(); return 1;
        }
        return 1;
    }
    if (tool_kind == TOOL_SETTINGS) {
        if (e->y >= 62 && e->y < 102) {
            for (int i = 0; i < scos_theme_count; i++) {
                int x = 16 + i * 128;
                if (e->x >= x && e->x < x + 120) { apply_theme_live(i); draw_tool(); return 1; }
            }
        }
        for (int field = 1; field <= 4; field++) {
            int y = 116 + (field - 1) * 36;
            if (e->x >= 100 && e->x < 524 && e->y >= y && e->y < y + 30) {
                start_settings_field(field); draw_tool(); return 1;
            }
        }
        if (e->y >= 268 && e->y < 298 && e->x >= 100 && e->x < 134) {
            apply_bar_height_live(cfg.bar_height - 4); draw_tool(); return 1;
        }
        if (e->y >= 268 && e->y < 298 && e->x >= 204 && e->x < 238) {
            apply_bar_height_live(cfg.bar_height + 4); draw_tool(); return 1;
        }
        if (e->y >= 268 && e->y < 298 && e->x >= 300 && e->x < 334) {
            int size = cfg.desktop_icon_size == 48 ? 32 : 24;
            apply_desktop_icon_size_live(size); draw_tool(); return 1;
        }
        if (e->y >= 268 && e->y < 298 && e->x >= 386 && e->x < 420) {
            int size = cfg.desktop_icon_size == 24 ? 32 : 48;
            apply_desktop_icon_size_live(size); draw_tool(); return 1;
        }
        if (e->y >= 268 && e->y < 298 && e->x >= 440 && e->x < 524) {
            cfg.desktop_grid = !cfg.desktop_grid;
            draw_desktop(); save_settings("Desktop grid"); draw_tool(); return 1;
        }
    }
    return 1;
}

static int tool_key(XKeyEvent *e)
{
    KeySym ks = XLookupKeysym(e, 0);
    if (tool_kind == TOOL_WIFI) {
        if (ks == XK_Escape) {
            if (wifi_password_editing) { wifi_password_editing = 0; draw_tool(); }
            else tool_close();
            return 1;
        }
        if (ks == XK_F5) { wifi_job_start(1, NULL, NULL); draw_tool(); return 1; }
        if (ks == XK_Up || ks == XK_Down) {
            if (wifi_network_count) {
                wifi_selected += ks == XK_Up ? -1 : 1;
                if (wifi_selected < 0) wifi_selected = 0;
                if (wifi_selected >= wifi_network_count) wifi_selected = wifi_network_count - 1;
                if (wifi_selected < wifi_network_scroll) wifi_network_scroll = wifi_selected;
                if (wifi_selected >= wifi_network_scroll + WIFI_VISIBLE_NETWORKS)
                    wifi_network_scroll = wifi_selected - WIFI_VISIBLE_NETWORKS + 1;
                draw_tool();
            }
            return 1;
        }
        if (ks == XK_Tab) { wifi_password_editing = !wifi_password_editing; draw_tool(); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { wifi_connect_selected(); draw_tool(); return 1; }
        if (wifi_password_editing && ks == XK_BackSpace) {
            size_t n = strlen(wifi_password); if (n) wifi_password[n - 1] = 0;
            draw_tool(); return 1;
        }
        if (wifi_password_editing) {
            char buffer[32];
            int n = XLookupString(e, buffer, sizeof(buffer), NULL, NULL);
            for (int i = 0; i < n; i++) {
                size_t used = strlen(wifi_password);
                if ((unsigned char)buffer[i] >= 32 && buffer[i] != 127 && used + 1 < sizeof(wifi_password)) {
                    wifi_password[used] = buffer[i]; wifi_password[used + 1] = 0;
                }
            }
            draw_tool(); return 1;
        }
        return 1;
    }
    if (tool_kind == TOOL_SYSMON) {
        if (ks == XK_Escape || ks == XK_q || ks == XK_Q) tool_close();
        return 1;
    }
    if (tool_kind != TOOL_SETTINGS) return 1;
    if (settings_field) {
        if (ks == XK_Return || ks == XK_KP_Enter) { commit_settings_field(); draw_tool(); return 1; }
        if (ks == XK_Escape) { settings_field = 0; snprintf(settings_status, sizeof(settings_status), "Edit cancelled"); draw_tool(); return 1; }
        if (ks == XK_BackSpace) {
            size_t n = strlen(settings_edit);
            if (n) settings_edit[n - 1] = 0;
            draw_tool(); return 1;
        }
        if (ks == XK_Tab) {
            int next = settings_field % 4 + 1;
            commit_settings_field(); start_settings_field(next); draw_tool(); return 1;
        }
        char buf[16];
        int m = XLookupString(e, buf, sizeof(buf), NULL, NULL);
        for (int i = 0; i < m; i++) {
            if (buf[i] >= 32 && buf[i] < 127 && strlen(settings_edit) < sizeof(settings_edit) - 1) {
                size_t n = strlen(settings_edit);
                settings_edit[n] = buf[i]; settings_edit[n + 1] = 0;
            }
        }
        draw_tool();
        return 1;
    }
    if (ks == XK_Escape) tool_close();
    else if (ks == XK_Tab) { start_settings_field(1); draw_tool(); }
    return 1;
}

int main(int argc, char **argv)
{
    saved_argv = argv;
    setlocale(LC_ALL, "");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("scoswm %s - SCos-styled X11 window manager\n"
                   "usage: scoswm [--config FILE] [--theme ID] [--version]\n"
                   "config: $XDG_CONFIG_HOME/scoswm/scoswm.conf\n"
                   "keys: Alt+Tab cycle, Alt+F4 close, Alt+F10 maximise,\n"
                   "      Alt+F9 minimise, Super+Enter terminal, Super+D launcher,\n"
                   "      Super+M SysMon, Super+comma Settings, Super+Q close,\n"
                   "      Alt+drag move/resize\n",
                   VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--version")) { printf("scoswm %s\n", VERSION); return 0; }
    }
    const char *theme_override = NULL, *cfg_override = NULL;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--theme")) theme_override = argv[i + 1];
        if (!strcmp(argv[i], "--config")) cfg_override = argv[i + 1];
    }

    load_config();
    if (cfg_override) { config_load_file(cfg_override, &cfg); }
    if (theme_override) {
        const struct scos_theme *t = theme_by_id(theme_override);
        if (t) theme = *t;
        else fprintf(stderr, "scoswm: unknown theme '%s'\n", theme_override);
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) die("cannot open display (is an X server running? Wayland-only sessions need XWayland)");
    fcntl(ConnectionNumber(dpy), F_SETFD, FD_CLOEXEC);
    XSetIOErrorHandler(xioerror);
    scr_no = DefaultScreen(dpy);
    root = RootWindow(dpy, scr_no);
    screen_w = DisplayWidth(dpy, scr_no);
    screen_h = DisplayHeight(dpy, scr_no);
    XSetErrorHandler(xerror);

    /* refuse to fight another WM for SubstructureRedirect:
     * a BadAccess on this select means someone else owns the root */
    {
        XSetErrorHandler(wm_conflict_error);
        XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask |
                     StructureNotifyMask | ButtonPressMask | ButtonReleaseMask | PropertyChangeMask |
                     PointerMotionMask | LeaveWindowMask | ExposureMask);
        XSync(dpy, False);
        XSetErrorHandler(xerror);
        if (wm_conflict) die("another window manager is already running");
    }

    if (cfg.font[0]) font = XLoadQueryFont(dpy, cfg.font);
    if (!font) font = XLoadQueryFont(dpy, "-misc-fixed-medium-r-normal--14-130-75-75-c-70-iso8859-1");
    if (!font) font = XLoadQueryFont(dpy, "-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso8859-1");
    if (!font) font = XLoadQueryFont(dpy, "9x15");
    if (!font) font = XLoadQueryFont(dpy, "10x20");
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (!font) die("no usable X core font found (is xfonts-base installed and in the X server font path?)");
    gc = XCreateGC(dpy, root, 0, NULL);
    XSetFont(dpy, gc, font->fid);

    for (int i = 0; i < A_COUNT; i++) atoms[i] = XInternAtom(dpy, atom_names[i], False);

    apply_palette();
    publish_cursor_settings();
    setup_cursor();

    /* _NET support handshake */
    wmcheck = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, wmcheck, atoms[A_NET_SUPPORTING_WM_CHECK], XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&wmcheck, 1);
    XChangeProperty(dpy, wmcheck, atoms[A_NET_WM_NAME], atoms[A_UTF8_STRING], 8,
                    PropModeReplace, (unsigned char *)"scoswm", 6);
    XChangeProperty(dpy, root, atoms[A_NET_SUPPORTING_WM_CHECK], XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&wmcheck, 1);
    Atom supported[] = {
        atoms[A_NET_CLIENT_LIST], atoms[A_NET_ACTIVE_WINDOW],
        atoms[A_NET_WM_DESKTOP], atoms[A_NET_NUMBER_OF_DESKTOPS],
        atoms[A_NET_CURRENT_DESKTOP], atoms[A_NET_WORKAREA], atoms[A_NET_FRAME_EXTENTS],
        atoms[A_NET_CLOSE_WINDOW], atoms[A_NET_MOVERESIZE_WINDOW], atoms[A_NET_WM_STATE],
        atoms[A_NET_WM_STATE_MAXIMIZED_VERT], atoms[A_NET_WM_STATE_MAXIMIZED_HORZ],
        atoms[A_NET_WM_STATE_FULLSCREEN], atoms[A_NET_WM_STATE_HIDDEN], atoms[A_NET_WM_ICON],
    };
    XChangeProperty(dpy, root, atoms[A_NET_SUPPORTED], XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)supported, (int)(sizeof(supported) / sizeof(supported[0])));
    long one = 1, zero = 0;
    XChangeProperty(dpy, root, atoms[A_NET_NUMBER_OF_DESKTOPS], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&one, 1);
    XChangeProperty(dpy, root, atoms[A_NET_CURRENT_DESKTOP], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&zero, 1);

    /* Application discovery for launcher + pins: the standard XDG
     * data dirs, plus Flatpak and Snap export dirs, whose apps are
     * invisible if only /usr/share/applications is scanned. */
    {
        const char *home = getenv("HOME") ? getenv("HOME") : "";
        static char dirbuf[24][640];
        const char *dirs[24];
        int ndirs = 0;
        const char *xdg_home = getenv("XDG_DATA_HOME");
        char xdg_home_buf[560];
        if (!xdg_home || !xdg_home[0]) {
            snprintf(xdg_home_buf, sizeof(xdg_home_buf), "%s/.local/share", home);
            xdg_home = xdg_home_buf;
        }
        if (ndirs < 24) {
            snprintf(dirbuf[ndirs], sizeof(dirbuf[0]), "%s/applications", xdg_home);
            dirs[ndirs] = dirbuf[ndirs]; ndirs++;
        }
        const char *xdg_dirs = getenv("XDG_DATA_DIRS");
        if (!xdg_dirs || !xdg_dirs[0]) xdg_dirs = "/usr/local/share:/usr/share";
        char xd[1024];
        snprintf(xd, sizeof(xd), "%s", xdg_dirs);
        for (char *tok = strtok(xd, ":"); tok && ndirs < 20; tok = strtok(NULL, ":")) {
            if (!*tok) continue;
            snprintf(dirbuf[ndirs], sizeof(dirbuf[0]), "%s/applications", tok);
            dirs[ndirs] = dirbuf[ndirs]; ndirs++;
        }
        char flat_local[640];
        snprintf(flat_local, sizeof(flat_local), "%s/.local/share/flatpak/exports/share/applications", home);
        const char *extra[] = { flat_local,
            "/var/lib/flatpak/exports/share/applications",
            "/var/lib/snapd/desktop/applications", NULL };
        for (int i = 0; extra[i] && ndirs < 24; i++) {
            snprintf(dirbuf[ndirs], sizeof(dirbuf[0]), "%s", extra[i]);
            dirs[ndirs] = dirbuf[ndirs]; ndirs++;
        }
        napps = desktop_scan(dirs, ndirs, apps, 1024);
    }
    build_pins();
    wifi_refresh_local();

    create_ui_windows();
    draw_desktop();
    draw_bar();
    grab_shortcuts();
    signal(SIGCHLD, SIG_IGN);

    /* adopt windows that were already mapped */
    {
        Window r, p, *kids = NULL; unsigned int nk = 0;
        if (XQueryTree(dpy, root, &r, &p, &kids, &nk)) {
            for (unsigned int i = 0; i < nk; i++) {
                XWindowAttributes at;
                if (XGetWindowAttributes(dpy, kids[i], &at) &&
                    at.map_state == IsViewable && !at.override_redirect)
                    manage(kids[i]);
            }
            if (kids) XFree(kids);
        }
    }
    recalc_struts();

    fprintf(stderr, "scoswm %s: %dx%d, theme %s, %d apps, %d taskbar pins, %d desktop pins\n",
            VERSION, screen_w, screen_h, theme.id, napps, npins, ndesk_pins);

    /* ------------------------------------------------------------ events */
    time_t last_clock = 0;
    while (running) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case MapRequest: {
                Window w = ev.xmaprequest.window;
                struct client *c = find_client(w);
                if (c) { if (c->minimized) activate(c, 1); }
                else manage(w);
                break; }
            case UnmapNotify: {
                struct client *c = find_client(ev.xunmap.window);
                if (c && ev.xunmap.window == c->win && !c->minimized)
                    /* Withdrawal is not destruction: return the client to
                     * the root before destroying only our frame. Apps that
                     * rapidly unmap/reuse a window must not lose that window. */
                    remove_client(c, 1);
                break; }
            case DestroyNotify: {
                struct client *c = find_client(ev.xdestroywindow.window);
                if (c) remove_client(c, 0);
                if (ev.xdestroywindow.window != root) recalc_struts();
                break; }
            case ConfigureRequest: {
                XConfigureRequestEvent *e = &ev.xconfigurerequest;
                struct client *c = find_client(e->window);
                if (!c) {
                    XWindowChanges wc;
                    wc.x = e->x; wc.y = e->y; wc.width = e->width; wc.height = e->height;
                    wc.border_width = e->border_width; wc.sibling = e->above; wc.stack_mode = e->detail;
                    XConfigureWindow(dpy, e->window, e->value_mask, &wc);
                } else if (!c->maximized && !c->fullscreen) {
                    if (e->value_mask & CWX) c->x = e->x - 1;
                    if (e->value_mask & CWY) c->y = e->y - TOP_H;
                    if (e->value_mask & CWWidth) c->w = e->width;
                    if (e->value_mask & CWHeight) c->h = e->height;
                    client_apply_hints(c, &c->w, &c->h);
                    client_place(c);
                    draw_frame(c);
                } else {
                    client_place(c);   /* refuse, but re-notify geometry */
                }
                break; }
            case ConfigureNotify:
                if (ev.xconfigure.window == root &&
                    (ev.xconfigure.width != screen_w || ev.xconfigure.height != screen_h)) {
                    screen_w = ev.xconfigure.width; screen_h = ev.xconfigure.height;
                    XMoveResizeWindow(dpy, edge_win, desktop_right_rule(), 0, DESK_RULE_W, screen_h - bar_h);
                    XMoveResizeWindow(dpy, bar_win, 0, screen_h - bar_h, screen_w, bar_h);
                    XMoveResizeWindow(dpy, overview_win, 0, 0, screen_w, overview_screen_height());
                    desktop_normalize_slots(); pins_sync_config();
                    draw_desktop(); draw_bar(); recalc_struts();
                    overview_clients_changed();
                }
                break;
            case PropertyNotify: {
                struct client *c = find_client(ev.xproperty.window);
                if (c && (ev.xproperty.atom == XA_WM_NAME || ev.xproperty.atom == atoms[A_NET_WM_NAME])) {
                    read_title(c); draw_frame(c); draw_bar();
                } else if (c && ev.xproperty.atom == XA_WM_NORMAL_HINTS) {
                    read_hints(c);
                } else if (c && ev.xproperty.atom == atoms[A_NET_WM_ICON]) {
                    xicon_destroy_owned(c->icon);
                    c->icon = client_load_icon(c); draw_bar();
                } else if (ev.xproperty.atom == atoms[A_NET_WM_STRUT_PARTIAL]) {
                    recalc_struts();
                }
                break; }
            case ClientMessage: {
                XClientMessageEvent *e = &ev.xclient;
                struct client *c = find_client(e->window);
                if (e->message_type == atoms[A_NET_ACTIVE_WINDOW] && c) activate(c, 1);
                else if (e->message_type == atoms[A_NET_CLOSE_WINDOW] && c) close_client(c);
                else if (e->message_type == atoms[A_NET_WM_STATE] && c) {
                    long action = e->data.l[0];   /* 0 remove 1 add 2 toggle */
                    for (int k = 1; k <= 2; k++) {
                        Atom a = (Atom)e->data.l[k];
                        if (!a) continue;
                        int want = action == 1 ? 1 : action == 2 ? -1 : 0;
                        if (a == atoms[A_NET_WM_STATE_FULLSCREEN])
                            set_fullscreen(c, want < 0 ? !c->fullscreen : want);
                        else if (a == atoms[A_NET_WM_STATE_MAXIMIZED_VERT] ||
                                 a == atoms[A_NET_WM_STATE_MAXIMIZED_HORZ])
                            set_maximized(c, want < 0 ? !c->maximized : want);
                        else if (a == atoms[A_NET_WM_STATE_HIDDEN]) {
                            if (want < 0 ? !c->minimized : want) minimize(c);
                            else activate(c, 1);
                        }
                    }
                } else if (e->message_type == atoms[A_NET_MOVERESIZE_WINDOW] && c) {
                    /* data.l[0]: gravity/source in high bits, x/y/w/h mask low */
                    unsigned long m = (unsigned long)e->data.l[0];
                    if (m & (1 << 8)) c->x = (int)e->data.l[1] - 1;
                    if (m & (1 << 9)) c->y = (int)e->data.l[2] - TOP_H;
                    if (m & (1 << 10)) c->w = (int)e->data.l[3];
                    if (m & (1 << 11)) c->h = (int)e->data.l[4];
                    client_apply_hints(c, &c->w, &c->h);
                    client_place(c); draw_frame(c);
                }
                break; }
            case ButtonPress: case ButtonRelease:
                on_button(&ev.xbutton);
                break;
            case LeaveNotify: {
                Window lw = ev.xcrossing.window;
                if (lw == root) {
                    if (desk_hover != -1) { desk_hover = -1; draw_desktop(); }
                } else if (lw == bar_win) {
                    if (bar_hover != -1) { bar_hover = -1; draw_bar(); }
                    if (bar_press != -1) { bar_press = -1; draw_bar(); }
                } else if (lw == menu_win) {
                    if (menu_hover != -1) { menu_hover = -1; draw_menu(); }
                } else if (hover_frame_client && lw == hover_frame_client->frame) {
                    struct client *old = hover_frame_client;
                    hover_frame_client = NULL; hover_title_btn = -1;
                    draw_frame(old);
                }
                break; }
            case MotionNotify: {
                /* compress motion */
                XEvent next;
                while (XCheckTypedEvent(dpy, MotionNotify, &next)) ev = next;
                on_motion(&ev.xmotion);
                break; }
            case KeyPress:
                on_key(&ev.xkey);
                break;
            case Expose:
                if (ev.xexpose.count == 0) {
                    if (ev.xexpose.window == root) expose_desktop();
                    else if (ev.xexpose.window == bar_win) draw_bar();
                    else if (ev.xexpose.window == edge_win) draw_right_rule();
                    else if (ev.xexpose.window == overview_win) draw_overview();
                    else if (ev.xexpose.window == launch_win) draw_launcher();
                    else if (ev.xexpose.window == menu_win) draw_menu();
                    else if (ev.xexpose.window == tool_win) draw_tool();
                    else {
                        struct client *c = find_client(ev.xexpose.window);
                        if (c) draw_frame(c);
                    }
                }
                break;
            default:
                break;
            }
        }
        /* 1 Hz clock/tick + asynchronous Wi-Fi output, without busy-waiting. */
        int fd = ConnectionNumber(dpy);
        fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
        int maxfd = fd;
        if (wifi_job.fd >= 0) {
            FD_SET(wifi_job.fd, &rf);
            if (wifi_job.fd > maxfd) maxfd = wifi_job.fd;
        }
        struct timeval tv = { 0, taskbar_launch_pending ? 50000 : 250000 };
        int selected = select(maxfd + 1, &rf, NULL, NULL, &tv);
        if (selected > 0 && wifi_job.fd >= 0 && FD_ISSET(wifi_job.fd, &rf)) wifi_job_read();
        if (taskbar_launch_pending && monotonic_msec() - taskbar_pending_since >= 400) {
            taskbar_launch_pending = 0;
            launch_pin_new(&taskbar_pending_pin);
        }
        time_t now = time(NULL);
        if (now != last_clock) {
            last_clock = now; draw_bar();
            if (tool_kind == TOOL_SYSMON) { refresh_sysmon(); draw_tool(); }
            else if (tool_kind == TOOL_SETTINGS && settings_field) draw_tool();
            if (now % 5 == 0) {
                wifi_refresh_local(); draw_bar();
                if (tool_kind == TOOL_WIFI) draw_tool();
            }
        }
        if (!running) break;
    }

    /* restore every client to the root before dying, so a restart (or
     * the next WM) finds plain top-level windows */
    overview_close();
    wifi_job_stop();
    while (nclients) remove_client(clients[0], 1);
    XChangeProperty(dpy, root, atoms[A_NET_SUPPORTED], XA_ATOM, 32, PropModeReplace, NULL, 0);
    XCloseDisplay(dpy);
    return 0;
}
