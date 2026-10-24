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
#include <X11/keysym.h>

#include <ctype.h>
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
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

#ifndef VERSION
#define VERSION "0.2.0"
#endif

#define TITLE_H 26
#define BTN_SZ 18
#define TOP_H (TITLE_H + 1)          /* titlebar + top border */
#define DESK_COL_W 88
#define DESK_ROW_H 96

/* ------------------------------------------------------------------ state */
static Display *dpy;
static int scr_no;
static Window root, wmcheck, bar_win, launch_win, menu_win;
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
       A_NET_WM_PID, A_COUNT };
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
    "_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_STRUT_PARTIAL", "_NET_WM_PID",
};

struct client {
    Window win, frame;
    int x, y, w, h;                /* frame position, client size */
    int sx, sy, sw, sh;            /* saved geometry for max/fullscreen */
    int minimized, maximized, fullscreen;
    int skip_taskbar;
    Window transient_for;
    XSizeHints hints; int has_hints;
    char title[256];
    char wclass[128];              /* WM_CLASS name/class, lower case */
};

static struct client **clients;
static int nclients, clients_cap;
static struct client *active_client;

static struct desktop_app apps[512];
static int napps;

struct pin { char label[64]; char command[512]; char match[96]; };
static struct pin pins[32];
static int npins;

static unsigned long desk_sel_mask;
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
        "alacritty", "kitty", "st", "xfce4-terminal", "gnome-terminal",
        "konsole", "foot", "xterm", NULL
    };
    for (int i = 0; list[i]; i++)
        if (cmd_exists(list[i])) { snprintf(found, sizeof(found), "%s", list[i]); return found; }
    return "xterm";
}

static const char *detect_files(void)
{
    static char found[160];
    if (cfg.files[0]) return cfg.files;
    static const char *const list[] = { "thunar", "pcmanfm", "nautilus", "dolphin", "nemo", NULL };
    for (int i = 0; list[i]; i++)
        if (cmd_exists(list[i])) {
            snprintf(found, sizeof(found), "%s", list[i]); return found;
        }
    snprintf(found, sizeof(found), "xdg-open %s", getenv("HOME") ? getenv("HOME") : "/");
    return found;
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
    XMoveResizeWindow(dpy, c->frame, c->x, c->y, frame_w(c), frame_h(c));
    XMoveResizeWindow(dpy, c->win,
                      c->fullscreen ? 0 : 1, c->fullscreen ? 0 : TOP_H, c->w, c->h);
    /* synthetic ConfigureNotify so the client knows its real geometry */
    XEvent ev; memset(&ev, 0, sizeof(ev));
    ev.type = ConfigureNotify;
    ev.xconfigure.display = dpy; ev.xconfigure.event = c->win; ev.xconfigure.window = c->win;
    ev.xconfigure.x = c->x + (c->fullscreen ? 0 : 1);
    ev.xconfigure.y = c->y + (c->fullscreen ? 0 : TOP_H);
    ev.xconfigure.width = c->w; ev.xconfigure.height = c->h;
    ev.xconfigure.border_width = 0; ev.xconfigure.above = None;
    ev.xconfigure.override_redirect = False;
    XSendEvent(dpy, c->win, False, StructureNotifyMask, &ev);
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
static void draw_bar(void);
static void update_ewmh_client_list(void);
static void launcher_toggle(int open);
static void wm_notify_no_windows(void);
static void menu_open(int kind, int x, int y, Window target);

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

static void activate(struct client *c, int raise)
{
    if (!c) return;
    if (c->minimized) {
        c->minimized = 0;
        XMapWindow(dpy, c->win);
        XMapWindow(dpy, c->frame);
        set_wm_state(c, NormalState);
    }
    /* move to the top of the manage list = stacking/focus order */
    for (int i = 0; i < nclients; i++)
        if (clients[i] == c) {
            memmove(&clients[i], &clients[i + 1], (nclients - i - 1) * sizeof(*clients));
            clients[nclients - 1] = c;
            break;
        }
    if (raise) XRaiseWindow(dpy, c->frame);
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
        XRaiseWindow(dpy, c->frame);
    } else {
        c->x = c->sx; c->y = c->sy; c->w = c->sw; c->h = c->sh;
        c->fullscreen = 0;
    }
    set_wm_state(c, NormalState);
    frame_position_for_restore(c);
}

static void close_client(struct client *c)
{
    if (!c) return;
    if (has_proto(c, atoms[A_WM_DELETE_WINDOW])) send_proto(c, atoms[A_WM_DELETE_WINDOW]);
    else XKillClient(dpy, c->win);
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
    XDestroyWindow(dpy, c->frame);
    memmove(&clients[idx], &clients[idx + 1], (nclients - idx - 1) * sizeof(*clients));
    nclients--;
    if (active_client == c) active_client = NULL;
    free(c);
    update_ewmh_client_list();
    if (!active_client) focus_top(); else draw_bar();
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
    if (win == bar_win || win == launch_win || win == menu_win || win == wmcheck) return;
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
    client_apply_hints(c, &c->w, &c->h);
    c->x = at.x - 1; c->y = at.y - TOP_H;
    int wx, wy, ww, wh; work_area(&wx, &wy, &ww, &wh);
    if (c->x + frame_w(c) > wx + ww) c->x = wx + ww - frame_w(c);
    if (c->y + frame_h(c) > wy + wh) c->y = wy + wh - frame_h(c);
    if (c->x < wx) c->x = wx;
    if (c->y < wy) c->y = wy;
    c->skip_taskbar = (wt == (Atom)XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False) ||
                       wt == (Atom)XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False));
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
    fa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                    PointerMotionMask | EnterWindowMask | StructureNotifyMask;
    fa.override_redirect = True;
    c->frame = XCreateWindow(dpy, root, c->x, c->y, frame_w(c), frame_h(c), 0,
                             CopyFromParent, InputOutput, CopyFromParent,
                             CWBackPixel | CWEventMask | CWOverrideRedirect, &fa);
    if (c->fullscreen) { c->x = 0; c->y = 0; c->w = screen_w; c->h = screen_h; }
    else if (c->maximized) {
        c->sx = c->x; c->sy = c->y; c->sw = c->w; c->sh = c->h;
        c->x = wx; c->y = wy; c->w = ww - 2; c->h = wh - TOP_H - 1;
    }

    XSetWindowBorderWidth(dpy, win, 0);
    XReparentWindow(dpy, win, c->frame, c->fullscreen ? 0 : 1, c->fullscreen ? 0 : TOP_H);
    XAddToSaveSet(dpy, win);
    XSelectInput(dpy, win, StructureNotifyMask | PropertyChangeMask | FocusChangeMask);
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
        XMapWindow(dpy, win);
        XMapWindow(dpy, c->frame);
        activate(c, 1);
    }
    recalc_struts();
    draw_bar();
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
static int launch_open, launch_offset, launch_sel;
static char launch_q[64];
static int launch_filtered[512];

static int launch_rebuild(void)
{
    int n = 0;
    for (int i = 0; i < napps; i++)
        if (ci_contains(apps[i].name, launch_q) || ci_contains(apps[i].exec, launch_q))
            launch_filtered[n++] = i;
    if (launch_sel >= n) launch_sel = n > 0 ? n - 1 : 0;
    if (launch_sel < 0) launch_sel = 0;
    return n;
}

static void launch_cmd_for(const struct desktop_app *a, char *out, int cap)
{
    char cmd[512];
    exec_strip_field_codes(a->exec, cmd, sizeof(cmd));
    if (a->is_terminal) snprintf(out, cap, "%s -e %s", detect_terminal(), cmd);
    else snprintf(out, cap, "%s", cmd);
}

static void launch_pin(int i)
{
    struct client *match = NULL;
    for (int k = 0; k < nclients; k++)
        if (pins[i].match[0] && strstr(clients[k]->wclass, pins[i].match)) match = clients[k];
    if (match) activate(match, 1);
    else spawn(pins[i].command);
}

/* ---------------------------------------------------------------- menus */
enum { MENU_NONE, MENU_POWER, MENU_DESKTOP, MENU_TITLE, MENU_PICKER = 98, MENU_MESSAGE = 99 };
static int menu_kind, menu_n;
static char menu_labels[8][64];
static Window menu_target;           /* title menu: the client acted on */
static int menu_x, menu_y, menu_w = 190;

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
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", s ? s : "");
    if (text_w(buf) > max_w) {
        int len = (int)strlen(buf);
        while (len > 0) {
            buf[len] = 0;
            char tmp[260];
            snprintf(tmp, sizeof(tmp), "%s...", buf);
            if (text_w(tmp) <= max_w) { snprintf(buf, sizeof(buf), "%s", tmp); break; }
            len--;
        }
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

static void draw_frame(struct client *c)
{
    if (!c || c->minimized) return;
    Window f = c->frame;
    int fw = frame_w(c), fh = frame_h(c);
    if (c->fullscreen) return;
    int focused = active_client == c;
    fill(f, 0, 0, fw, fh, PX.win_bg);
    rect(f, 0, 0, fw, fh, PX.main);
    fill(f, 1, 1, fw - 2, TITLE_H - 1, focused ? PX.main : PX.title_off);
    clipped_text(f, 8, 5 + font->ascent, c->title, fw - 96,
                 focused ? PX.title_text : PX.text);
    unsigned long fg = focused ? PX.title_text : PX.text;
    for (int b = 0; b < 3; b++)
        draw_button_glyph(f, fw - 22 - (2 - b) * 22, 4, b, fg);
    /* resize grip, like SCos's diagonal dots */
    XSetForeground(dpy, gc, PX.bar_hi);
    for (int i = 0; i < 4; i++)
        XFillRectangle(dpy, f, gc, fw - 12 + i * 3, fh - 12 + i * 3, 2, 2);
}

/* ---------------------------------------------------------------- desktop */
static void icon_rect(int i, int *x, int *y)
{
    *x = 16 + (i % 8) * DESK_COL_W;
    *y = 16 + (i / 8) * DESK_ROW_H;
}

static void draw_desktop(void)
{
    /* Painted straight onto the root window (Expose events repaint it)
     * rather than via a background pixmap, so the desktop also works
     * on minimal X servers that ignore root pixmaps. */
    for (int y = 0; y < screen_h; y++) {
        unsigned int rgb = color_blend_rgb(theme.bg_top, theme.bg_bot,
                                           screen_h > 1 ? y * 255 / (screen_h - 1) : 0);
        XSetForeground(dpy, gc, alloc_rgb(rgb));
        XFillRectangle(dpy, root, gc, 0, y, screen_w, 1);
    }
    XSetForeground(dpy, gc, PX.grid);
    for (int x = 0; x < screen_w; x += 64) XDrawLine(dpy, root, gc, x, 0, x, screen_h);
    for (int y = 0; y < screen_h; y += 64) XDrawLine(dpy, root, gc, 0, y, screen_w, y);

    /* desktop icons on the SCos 88x96 snap grid */
    for (int i = 0; i < npins; i++) {
        int x, y; icon_rect(i, &x, &y);
        if (desk_sel_mask & (1ul << i)) {
            fill(root, x, y, 80, 88, alloc_rgb(color_blend_rgb(theme.bg_top, theme.main, 30)));
            rect(root, x, y, 80, 88, PX.main);
        }
        /* generic SCos-ish 24x24 glyph: a framed box with the initial */
        int gx = x + 28, gy = y + 8;
        rect(root, gx, gy, 24, 24, PX.main);
        char one[2] = { pins[i].label[0] ? pins[i].label[0] : '?', 0 };
        text_out(root, gx + 8, gy + 8 + font->ascent, one, PX.main);
        clipped_text(root, x + 4, y + 40 + font->ascent, pins[i].label, 72, PX.text);
    }
}

/* ------------------------------------------------------------------- bar */
struct bar_item { int kind; int idx; int x, w; };   /* kind 0=client 1=pin */
static struct bar_item bar_items[96];
static int bar_n;

static void bar_layout(void)
{
    bar_n = 0;
    int x = 168;
    int avail = screen_w - x - 34 - 16 - 100;        /* power + clock+padding */
    if (avail < 100) avail = 100;
    /* pinned squares first (34 px, SCos taskbar pins) */
    int squares = 0;
    for (int i = 0; i < npins; i++) {
        int running = 0;
        for (int k = 0; k < nclients; k++)
            if (pins[i].match[0] && strstr(clients[k]->wclass, pins[i].match)) running = 1;
        if (!running) squares++;
    }
    int sq_w = squares * 40;
    if (sq_w > avail / 3) sq_w = avail / 3;
    for (int i = 0; i < npins && bar_n < 96; i++) {
        int running = 0;
        for (int k = 0; k < nclients; k++)
            if (pins[i].match[0] && strstr(clients[k]->wclass, pins[i].match)) running = 1;
        if (running) continue;
        if (x + 40 > 168 + sq_w + 200 && squares > 4) break;
        bar_items[bar_n].kind = 1; bar_items[bar_n].idx = i;
        bar_items[bar_n].x = x; bar_items[bar_n].w = 34; bar_n++;
        x += 40;
    }
    /* running windows share what is left */
    int nrun = 0;
    for (int i = 0; i < nclients; i++) if (!clients[i]->skip_taskbar) nrun++;
    if (nrun) {
        int rest = screen_w - x - 34 - 16 - 100;
        int bw = rest / nrun;
        if (bw > 170) bw = 170;
        if (bw < 46) bw = 46;
        for (int i = 0; i < nclients && bar_n < 96; i++) {
            if (clients[i]->skip_taskbar) continue;
            bar_items[bar_n].kind = 0; bar_items[bar_n].idx = i;
            bar_items[bar_n].x = x; bar_items[bar_n].w = bw; bar_n++;
            x += bw + 6;
        }
    }
}

static void draw_bar(void)
{
    if (!bar_win) return;
    fill(bar_win, 0, 0, screen_w, bar_h, PX.taskbar_bg);
    fill(bar_win, 0, 0, screen_w, 1, PX.main);
    int by = 6, bh = bar_h - 12;
    /* launcher button: the 2x2 SCos grid */
    fill(bar_win, 8, by, 34, bh, launch_open ? PX.bar_hi : PX.taskbar_bg);
    rect(bar_win, 8, by, 34, bh, PX.main);
    for (int q = 0; q < 4; q++)
        fill(bar_win, 15 + (q % 2) * 11, by + 6 + (q / 2) * 9, 8, 6,
             launch_open ? PX.title_text : PX.main);
    /* Windows N picker */
    char wl[32]; snprintf(wl, sizeof(wl), "Windows %d", nclients);
    fill(bar_win, 48, by, 110, bh, PX.taskbar_bg);
    rect(bar_win, 48, by, 110, bh, PX.main);
    clipped_text(bar_win, 56, by + 6 + font->ascent, wl, 94, PX.text);

    bar_layout();
    for (int i = 0; i < bar_n; i++) {
        struct bar_item *it = &bar_items[i];
        if (it->kind == 1) {
            struct pin *p = &pins[it->idx];
            fill(bar_win, it->x, by, 34, bh, PX.taskbar_bg);
            rect(bar_win, it->x, by, 34, bh, PX.main);
            char one[2] = { p->label[0] ? p->label[0] : '?', 0 };
            text_out(bar_win, it->x + 13, by + 8 + font->ascent, one, PX.text);
        } else {
            struct client *c = clients[it->idx];
            int foc = active_client == c;
            fill(bar_win, it->x, by, it->w, bh,
                 foc ? PX.bar_hi : PX.taskbar_bg);
            rect(bar_win, it->x, by, it->w, bh, PX.main);
            clipped_text(bar_win, it->x + 6, by + 6 + font->ascent, c->title,
                         it->w - 12, c->minimized ? PX.title_off : PX.text);
            if (c->minimized)             /* SCos showed put-away state */
                text_out(bar_win, it->x + 6, by + 6 + font->ascent, "[-] ", PX.main);
        }
    }
    /* power button */
    int pw = 34, px = screen_w - 100 - pw - 16;
    fill(bar_win, px, by, pw, bh, PX.taskbar_bg);
    rect(bar_win, px, by, pw, bh, PX.main);
    XSetForeground(dpy, gc, PX.main);
    XDrawArc(dpy, bar_win, gc, px + pw / 2 - 7, by + bh / 2 - 5, 14, 14, 45 * 64, 270 * 64);
    XDrawLine(dpy, bar_win, gc, px + pw / 2, by + bh / 2 - 8, px + pw / 2, by + bh / 2);
    /* clock */
    time_t now = time(NULL);
    struct tm tmv; localtime_r(&now, &tmv);
    char clock[16];
    snprintf(clock, sizeof(clock), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    text_out(bar_win, screen_w - text_w(clock) - 10, by + 6 + font->ascent, clock, PX.main);
}

/* ------------------------------------------------------ launcher window */
static void draw_launcher(void)
{
    if (!launch_open) return;
    int h = 308, y = screen_h - bar_h - h - 4;
    XMoveResizeWindow(dpy, launch_win, 8, y, 300, h);
    fill(launch_win, 0, 0, 300, h, PX.win_bg);
    rect(launch_win, 0, 0, 300, h, PX.main);
    text_out(launch_win, 10, 8 + font->ascent, "Apps: type to search / scroll", PX.main);
    rect(launch_win, 10, 26, 280, 20, PX.main);
    text_out(launch_win, 14, 30 + font->ascent, launch_q, PX.text);
    int caret_x = 14 + text_w(launch_q);
    if ((time(NULL) % 2) == 0) fill(launch_win, caret_x, 30, 2, 12, PX.main);
    int n = launch_rebuild();
    int rows = (h - 64) / 24; if (rows > 10) rows = 10;
    if (launch_sel < launch_offset) launch_offset = launch_sel;
    if (launch_sel >= launch_offset + rows) launch_offset = launch_sel - rows + 1;
    int yy = 56;
    for (int r = 0; r < rows && launch_offset + r < n; r++) {
        int ai = launch_filtered[launch_offset + r];
        if (launch_offset + r == launch_sel) fill(launch_win, 6, yy - 3, 288, 22, PX.bar_hi);
        char one[2] = { apps[ai].name[0], 0 };
        text_out(launch_win, 12, yy + 4 + font->ascent, one, PX.main);
        clipped_text(launch_win, 44, yy + 4 + font->ascent, apps[ai].name, 248,
                     launch_offset + r == launch_sel ? PX.title_text : PX.text);
        yy += 24;
    }
    if (!n) text_out(launch_win, 14, yy + font->ascent, "(no matching app)", PX.title_off);
    XRaiseWindow(dpy, launch_win);
}

static void launcher_toggle(int open)
{
    if (open && !launch_open) {
        launch_q[0] = 0; launch_offset = 0; launch_sel = 0;
        launch_rebuild();
        XMapWindow(dpy, launch_win);
        launch_open = 1;
        XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime);
        draw_launcher();
    } else if (!open && launch_open) {
        launch_open = 0;
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
    XUnmapWindow(dpy, menu_win);
    XUngrabKeyboard(dpy, CurrentTime);
}

static void menu_open(int kind, int x, int y, Window target)
{
    menu_close();
    menu_kind = kind; menu_target = target;
    if (kind == MENU_POWER) {
        static const char *items[] = { "Lock", "Restart WM", "Exit WM", "Suspend", "Reboot", "Power Off" };
        menu_n = 6;
        for (int i = 0; i < menu_n; i++) snprintf(menu_labels[i], 64, "%s", items[i]);
    } else if (kind == MENU_DESKTOP) {
        static const char *items[] = { "Open Terminal", "Open Files", "Open Web Browser",
                                       "App Launcher", "Edit WM Config", "Restart WM", "Exit WM" };
        menu_n = 7;
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
}

static void draw_menu(void)
{
    if (menu_kind == MENU_NONE) return;
    int w = menu_w, h = menu_n * 24 + 6;
    fill(menu_win, 0, 0, w, h, PX.win_bg);
    rect(menu_win, 0, 0, w, h, PX.main);
    for (int i = 0; i < menu_n; i++)
        clipped_text(menu_win, 10, 3 + i * 24 + 4 + font->ascent, menu_labels[i], w - 20, PX.text);
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
static struct {
    int kind;               /* 0 none 1 move 2 resize 3 desktop-band */
    struct client *c;
    int start_x, start_y;   /* pointer at grab */
    int ox, oy, ow, oh;     /* frame/client geometry at grab */
    int moved;
    int band_x, band_y, band_drawn;
} drag;

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

static void menu_action(int idx)
{
    int kind = menu_kind;
    Window target = menu_target;
    menu_close();
    if (kind == MENU_POWER) {
        switch (idx) {
        case 0: {   /* Lock: first locker that exists wins */
            static const char *const lockers[] = { "slock", "i3lock", "xlock", "xscreensaver-command -lock", NULL };
            for (int i = 0; lockers[i]; i++) {
                char first[64]; snprintf(first, sizeof(first), "%s", lockers[i]);
                char *sp = strchr(first, ' '); if (sp) *sp = 0;
                if (cmd_exists(first)) { spawn(lockers[i]); break; }
            }
            break; }
        case 1:   /* restart scoswm, clients survive (reparented on exit path) */
            while (nclients) remove_client(clients[0], 1);
            execvp("/proc/self/exe", saved_argv);
            break;
        case 2: running = 0; break;
        case 3: spawn("systemctl suspend"); break;
        case 4: spawn("systemctl reboot"); break;
        case 5: spawn("systemctl poweroff"); break;
        }
    } else if (kind == MENU_DESKTOP) {
        char cmd[640];
        switch (idx) {
        case 0: spawn(detect_terminal()); break;
        case 1: spawn(detect_files()); break;
        case 2: spawn("xdg-open https://duckduckgo.com"); break;
        case 3: launcher_toggle(1); break;
        case 4:
            snprintf(cmd, sizeof(cmd), "%s -e %s ~/.config/scoswm/scoswm.conf",
                     detect_terminal(), getenv("EDITOR") ? getenv("EDITOR") : "vi");
            spawn(cmd); break;
        case 5:
            while (nclients) remove_client(clients[0], 1);
            execvp("/proc/self/exe", saved_argv);
            break;
        case 6: running = 0; break;
        }
    } else if (kind == MENU_TITLE) {
        struct client *c = find_client(target);
        if (c) {
            if (idx == 0) minimize(c);
            else if (idx == 1) set_maximized(c, !c->maximized);
            else if (idx == 2) close_client(c);
        }
    }
}

static void pick_window(int idx)
{
    /* the "Windows" picker: SCos listed "[+]/[-]/title" rows in a menu */
    if (idx < 0 || idx >= nclients) return;
    activate(clients[idx], 1);
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

    if (menu_kind != MENU_NONE && w != menu_win) { menu_close(); return; }
    if (w == menu_win) {
        if (e->type != ButtonRelease || e->button != Button1) return;
        int idx = (e->y - 3) / 24;
        if (menu_kind == MENU_MESSAGE) menu_close();
        else if (menu_kind == MENU_PICKER) {
            if (idx >= 0 && idx < menu_n) { int ci = nclients - 1 - idx; menu_close(); pick_window(ci); }
            else menu_close();
        }
        else if (idx >= 0 && idx < menu_n) menu_action(idx);
        else menu_close();
        return;
    }
    if (launch_open && w != launch_win && w != bar_win) launcher_toggle(0);

    if (w == bar_win) {
        if (e->type != ButtonPress) return;
        if (e->x >= 8 && e->x < 42 && e->y >= 6) { launcher_toggle(!launch_open); return; }
        if (e->x >= 48 && e->x < 158 && e->y >= 6) {
            /* Windows picker as a SCos-style menu of open windows */
            if (!nclients) { wm_notify_no_windows(); return; }
            menu_kind = MENU_PICKER;
            menu_n = nclients > 8 ? 8 : nclients;
            for (int i = 0; i < menu_n; i++) {
                struct client *c = clients[nclients - 1 - i];
                snprintf(menu_labels[i], 64, "%s %s", c->minimized ? "[-]" :
                         (active_client == c ? "[+]" : "   "), c->title);
            }
            menu_x = 48; menu_y = screen_h - bar_h - (menu_n * 24 + 8);
            menu_w = 300;
            XMoveResizeWindow(dpy, menu_win, menu_x, menu_y, menu_w, menu_n * 24 + 6);
            XMapWindow(dpy, menu_win);
            XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime);
            draw_menu();
            return;
        }
        int pw_x = screen_w - 100 - 34 - 16;
        if (e->x >= pw_x && e->x < pw_x + 34 && e->y >= 6) {
            menu_open(MENU_POWER, pw_x - 140, screen_h - bar_h - (6 * 24 + 8), None);
            return;
        }
        for (int i = 0; i < bar_n; i++) {
            struct bar_item *it = &bar_items[i];
            if (e->x >= it->x && e->x < it->x + it->w && e->y >= 6) {
                if (it->kind == 1) {
                    if (e->button == Button1) launch_pin(it->idx);
                } else {
                    struct client *c = clients[it->idx];
                    if (e->button == Button1) {
                        if (active_client == c && !c->minimized) minimize(c);
                        else activate(c, 1);
                    } else if (e->button == Button3) {
                        menu_open(MENU_TITLE, it->x, screen_h - bar_h - (3 * 24 + 8), c->win);
                    }
                }
                return;
            }
        }
        return;
    }

    if (w == launch_win) {
        if (e->type != ButtonPress) return;
        if (e->button == Button4) { if (launch_offset > 0) launch_offset--; draw_launcher(); return; }
        if (e->button == Button5) { launch_offset++; draw_launcher(); return; }
        int row = (e->y - 53) / 24;
        int n = launch_rebuild();
        int idx = launch_offset + row;
        if (e->button == Button1 && idx >= 0 && idx < n) {
            char cmd[1024];
            launch_cmd_for(&apps[launch_filtered[idx]], cmd, sizeof(cmd));
            launcher_toggle(0);
            spawn(cmd);
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
            for (int i = 0; i < npins; i++) {
                int ix, iy; icon_rect(i, &ix, &iy);
                if (ix < bx2 && ix + 80 > bx && iy < by2 && iy + 88 > byy)
                    desk_sel_mask |= 1ul << i;
            }
            draw_desktop();
            return;
        }
        if (e->type != ButtonPress) return;
        if (e->button == Button3) {
            menu_open(MENU_DESKTOP, x, y, None);
            return;
        }
        if (e->button != Button1) return;
        int hit = -1;
        for (int i = 0; i < npins; i++) {
            int ix, iy; icon_rect(i, &ix, &iy);
            if (x >= ix && x < ix + 80 && y >= iy && y < iy + 88) hit = i;
        }
        if (hit >= 0) {
            int dbl = (e->button == last_click.button &&
                       (long)(e->time - last_click.t) < 400 &&
                       x - last_click.x < 6 && x - last_click.x > -6 &&
                       y - last_click.y < 6 && y - last_click.y > -6 &&
                       hit == last_click.icon);
            last_click.button = e->button; last_click.t = e->time;
            last_click.x = x; last_click.y = y; last_click.icon = hit;
            desk_sel_mask = 1ul << hit;
            draw_desktop();
            if (dbl) launch_pin(hit);
            return;
        }
        last_click.icon = -1;
        /* rubber band on empty desktop */
        drag.kind = 3; drag.start_x = drag.band_x = x; drag.start_y = drag.band_y = y;
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
    activate(c, 1);

    int rx = is_frame ? e->x : e->x + 1;
    int ry = is_frame ? e->y : e->y + TOP_H;
    /* Alt+drag anywhere: move (left) / resize (right) */
    if (e->state & Mod1Mask) {
        drag.kind = e->button == Button3 ? 2 : 1;
        drag.c = c; drag.start_x = x; drag.start_y = y;
        drag.ox = c->x; drag.oy = c->y; drag.ow = c->w; drag.oh = c->h;
        return;
    }
    if (!is_frame) return;

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
        if (c->maximized || c->fullscreen) return;
        drag.kind = 1; drag.c = c; drag.start_x = x; drag.start_y = y;
        drag.ox = c->x; drag.oy = c->y; drag.ow = c->w; drag.oh = c->h;
        return;
    }
    if (ry < TITLE_H && e->button == Button3) {
        menu_open(MENU_TITLE, x, y, c->win);
        return;
    }
    /* bottom-right resize grip */
    if (e->button == Button1 && rx >= frame_w(c) - 18 && ry >= frame_h(c) - 18 &&
        !c->maximized && !c->fullscreen) {
        drag.kind = 2; drag.c = c; drag.start_x = x; drag.start_y = y;
        drag.ox = c->x; drag.oy = c->y; drag.ow = c->w; drag.oh = c->h;
    }
}

static void on_motion(XMotionEvent *e)
{
    if (!drag.kind) return;
    if (drag.kind == 3) {
        drag.band_x = e->x_root; drag.band_y = e->y_root;
        return;
    }
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
        int nw = drag.ow + (e->x_root - drag.start_x);
        int nh = drag.oh + (e->y_root - drag.start_y);
        client_apply_hints(c, &nw, &nh);
        c->w = nw; c->h = nh;
        client_place(c);
    }
}

static void on_key(XKeyEvent *e)
{
    KeySym ks = XLookupKeysym(e, 0);
    unsigned int mods = e->state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);

    if (menu_kind != MENU_NONE) {
        if (ks == XK_Escape) menu_close();
        return;
    }
    if (launch_open) {
        int n = launch_rebuild();
        if (ks == XK_Escape) launcher_toggle(0);
        else if (ks == XK_Return) {
            if (n > 0) {
                char cmd[1024];
                launch_cmd_for(&apps[launch_filtered[launch_sel]], cmd, sizeof(cmd));
                launcher_toggle(0);
                spawn(cmd);
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
    else if (mods == Mod4Mask && (ks == XK_Return || ks == XK_KP_Enter)) spawn(detect_terminal());
    else if (mods == Mod4Mask && (ks == XK_d || ks == XK_D)) launcher_toggle(1);
    else if (mods == Mod4Mask && (ks == XK_q || ks == XK_Q) && active_client) close_client(active_client);
}

static void grab_shortcuts(void)
{
    static const struct { KeySym ks; unsigned int mod; } grabs[] = {
        { XK_Tab, Mod1Mask }, { XK_Tab, Mod1Mask | ShiftMask },
        { XK_F4, Mod1Mask }, { XK_F10, Mod1Mask }, { XK_F9, Mod1Mask },
        { XK_Return, Mod4Mask }, { XK_d, Mod4Mask }, { XK_D, Mod4Mask },
        { XK_q, Mod4Mask }, { XK_Q, Mod4Mask },
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
static void build_pins(void)
{
    npins = 0;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", cfg.pins);
    for (char *tok = strtok(buf, ","); tok && npins < 32; tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        char *end = tok + strlen(tok);
        while (end > tok && end[-1] == ' ') *--end = 0;
        if (!*tok) continue;
        struct pin *p = &pins[npins];
        memset(p, 0, sizeof(*p));
        /* 1) exact .desktop id match */
        int found = -1;
        for (int i = 0; i < napps; i++)
            if (!strcasecmp(apps[i].id, tok)) found = i;
        if (found < 0)       /* 2) substring match on id or name */
            for (int i = 0; i < napps; i++)
                if (ci_contains(apps[i].id, tok) || ci_contains(apps[i].name, tok)) { found = i; break; }
        if (found >= 0) {
            char cmd[1024];
            launch_cmd_for(&apps[found], cmd, sizeof(cmd));
            snprintf(p->command, sizeof(p->command), "%s", cmd);
            snprintf(p->label, sizeof(p->label), "%s", apps[found].name);
            snprintf(p->match, sizeof(p->match), "%s", apps[found].id);
            npins++;
            continue;
        }
        /* 3) built-in pseudo-pins */
        if (!strcasecmp(tok, "terminal")) {
            snprintf(p->label, sizeof(p->label), "Terminal");
            snprintf(p->command, sizeof(p->command), "%s", detect_terminal());
            snprintf(p->match, sizeof(p->match), "%s", "terminal");
        } else if (!strcasecmp(tok, "files")) {
            snprintf(p->label, sizeof(p->label), "Files");
            snprintf(p->command, sizeof(p->command), "%s", detect_files());
            snprintf(p->match, sizeof(p->match), "%s", "files");
        } else if (!strcasecmp(tok, "web-browser") || !strcasecmp(tok, "browser")) {
            snprintf(p->label, sizeof(p->label), "Web Browser");
            snprintf(p->command, sizeof(p->command), "xdg-open https://duckduckgo.com");
            snprintf(p->match, sizeof(p->match), "%s", "browser");
        } else {             /* 4) a bare command */
            snprintf(p->label, sizeof(p->label), "%s", tok);
            snprintf(p->command, sizeof(p->command), "%s", tok);
            snprintf(p->match, sizeof(p->match), "%s", tok);
            p->label[0] = (char)toupper((unsigned char)p->label[0]);
        }
        npins++;
    }
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

static void create_ui_windows(void)
{
    XSetWindowAttributes at;
    at.override_redirect = True;
    at.background_pixel = PX.taskbar_bg;
    at.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    bar_win = XCreateWindow(dpy, root, 0, screen_h - bar_h, screen_w, bar_h, 0,
                            CopyFromParent, InputOutput, CopyFromParent,
                            CWBackPixel | CWEventMask | CWOverrideRedirect, &at);
    XChangeProperty(dpy, bar_win, atoms[A_NET_WM_WINDOW_TYPE], XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&atoms[A_NET_WM_WINDOW_TYPE_DOCK], 1);
    long strut[12] = { 0 };
    strut[3] = bar_h;                       /* bottom */
    strut[11] = screen_w;                   /* bottom_end_x */
    XChangeProperty(dpy, bar_win, atoms[A_NET_WM_STRUT_PARTIAL], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)strut, 12);
    XMapWindow(dpy, bar_win);

    at.background_pixel = PX.win_bg;
    at.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    launch_win = XCreateWindow(dpy, root, 8, screen_h - bar_h - 312, 300, 308, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWBackPixel | CWEventMask | CWOverrideRedirect, &at);
    menu_win = XCreateWindow(dpy, root, 100, 100, 190, 100, 0,
                             CopyFromParent, InputOutput, CopyFromParent,
                             CWBackPixel | CWEventMask | CWOverrideRedirect, &at);
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
                   "      Super+Q close, Alt+drag move/resize\n",
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
                     StructureNotifyMask | ButtonPressMask | PropertyChangeMask |
                     ExposureMask);
        XSync(dpy, False);
        XSetErrorHandler(xerror);
        if (wm_conflict) die("another window manager is already running");
    }

    if (cfg.font[0]) font = XLoadQueryFont(dpy, cfg.font);
    if (!font) font = XLoadQueryFont(dpy, "-misc-fixed-medium-r-normal--14-130-75-75-c-70-iso8859-1");
    if (!font) font = XLoadQueryFont(dpy, "-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso8859-1");
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (!font) die("no usable core font found");
    gc = XCreateGC(dpy, root, 0, NULL);
    XSetFont(dpy, gc, font->fid);

    for (int i = 0; i < A_COUNT; i++) atoms[i] = XInternAtom(dpy, atom_names[i], False);

    apply_palette();

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
        atoms[A_NET_WM_STATE_FULLSCREEN], atoms[A_NET_WM_STATE_HIDDEN],
    };
    XChangeProperty(dpy, root, atoms[A_NET_SUPPORTED], XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)supported, (int)(sizeof(supported) / sizeof(supported[0])));
    long one = 1, zero = 0;
    XChangeProperty(dpy, root, atoms[A_NET_NUMBER_OF_DESKTOPS], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&one, 1);
    XChangeProperty(dpy, root, atoms[A_NET_CURRENT_DESKTOP], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&zero, 1);

    /* application discovery for launcher + pins */
    {
        const char *home = getenv("HOME");
        char local[512];
        snprintf(local, sizeof(local), "%s/.local/share/applications", home ? home : "");
        const char *dirs[] = { local, "/usr/local/share/applications", "/usr/share/applications" };
        napps = desktop_scan(dirs, 3, apps, 512);
    }
    build_pins();

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

    fprintf(stderr, "scoswm %s: %dx%d, theme %s, %d apps, %d pins\n",
            VERSION, screen_w, screen_h, theme.id, napps, npins);

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
                    remove_client(c, 0);
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
                    XMoveResizeWindow(dpy, bar_win, 0, screen_h - bar_h, screen_w, bar_h);
                    draw_desktop(); draw_bar(); recalc_struts();
                }
                break;
            case PropertyNotify: {
                struct client *c = find_client(ev.xproperty.window);
                if (c && (ev.xproperty.atom == XA_WM_NAME || ev.xproperty.atom == atoms[A_NET_WM_NAME])) {
                    read_title(c); draw_frame(c); draw_bar();
                } else if (c && ev.xproperty.atom == XA_WM_NORMAL_HINTS) {
                    read_hints(c);
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
                    if (ev.xexpose.window == root) { if (!drag.kind) draw_desktop(); }
                    else if (ev.xexpose.window == bar_win) draw_bar();
                    else if (ev.xexpose.window == launch_win) draw_launcher();
                    else if (ev.xexpose.window == menu_win) draw_menu();
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
        /* 1 Hz clock/tick + shutdown check, without busy-waiting */
        int fd = ConnectionNumber(dpy);
        fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
        struct timeval tv = { 0, 250000 };
        if (select(fd + 1, &rf, NULL, NULL, &tv) == 0) {
            time_t now = time(NULL);
            if (now != last_clock) { last_clock = now; draw_bar(); }
        }
        if (!running) break;
    }

    /* restore every client to the root before dying, so a restart (or
     * the next WM) finds plain top-level windows */
    while (nclients) remove_client(clients[0], 1);
    XChangeProperty(dpy, root, atoms[A_NET_SUPPORTED], XA_ATOM, 32, PropModeReplace, NULL, 0);
    XCloseDisplay(dpy);
    return 0;
}
