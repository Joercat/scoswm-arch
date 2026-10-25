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

#ifndef VERSION
#define VERSION "0.5.2"
#endif

#define TITLE_H 26
#define BTN_SZ 18
#define TOP_H (TITLE_H + 1)          /* titlebar + top border */
#define DESK_COL_W 88
#define DESK_ROW_H 96
#define TOOL_W 540
#define TOOL_H 390

/* ------------------------------------------------------------------ state */
static Display *dpy;
static int scr_no;
static Window root, wmcheck, bar_win, launch_win, menu_win, tool_win, anim_win;
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

static struct desktop_app apps[1024];
static int napps;

enum core_action { CORE_NONE, CORE_TERMINAL, CORE_FILES, CORE_SYSMON, CORE_SETTINGS };
struct pin { char label[64]; char glyph[4]; char icon[160]; char command[512]; char match[96]; int action; };
static struct pin pins[32];
static int npins;

static unsigned long desk_sel_mask;
static int desk_hover = -1;
static int bar_hover = -1;          /* -1 none, >=0 bar_items idx, -2 start, -3 windows, -4 power */
static int bar_press = -1;          /* bar_items idx held down, -1 none */
static int bar_flash_pin = -1;      /* pin idx showing launch feedback */
static time_t bar_flash_until;
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
    static const char *const list[] = { "thunar", "pcmanfm", "pcmanfm-qt", "nautilus", "dolphin", "nemo", "caja", NULL };
    for (int i = 0; list[i]; i++)
        if (cmd_exists(list[i])) return list[i];
    return NULL;
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
static void draw_bar(void);
static void bar_layout(void);
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
enum { TOOL_NONE, TOOL_SETTINGS, TOOL_SYSMON };
static int tool_kind;
static int tool_release_pending;
static void tool_open(int kind);
static void tool_close(void);
static void draw_tool(void);
static int tool_key(XKeyEvent *e);
static int tool_button(XButtonEvent *e);
static void spawn_files_home(void);
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

static void raise_client_frame(struct client *c)
{
    if (c->fullscreen) {
        XRaiseWindow(dpy, c->frame);
    } else {
        /* Raise above applications but directly below the dock. A plain
         * XRaiseWindow made the next focused app cover the taskbar. */
        XWindowChanges wc;
        wc.sibling = bar_win;
        wc.stack_mode = Below;
        XConfigureWindow(dpy, c->frame, CWSibling | CWStackMode, &wc);
    }
}

static void activate(struct client *c, int raise)
{
    if (!c) return;
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
    int kind;               /* 0 none 1 move 2 resize 3 desktop-band */
    struct client *c;
    int start_x, start_y;   /* pointer at grab */
    int ox, oy, ow, oh;     /* frame/client geometry at grab */
    int edges;              /* resize edges: 1 left 2 right 4 top 8 bottom */
    int moved;
    int band_x, band_y, band_drawn;
} drag;

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
    if (win == bar_win || win == launch_win || win == menu_win || win == tool_win ||
        win == anim_win || win == wmcheck) return;
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
    { "Terminal", "Tr", "utilities-terminal", "terminal shell console command", CORE_TERMINAL },
    { "Files",    "Fi", "system-file-manager", "files file manager home documents downloads", CORE_FILES },
    { "SysMon",   "Sy", "utilities-system-monitor", "sysmon system monitor cpu memory gpu processes", CORE_SYSMON },
    { "Settings", "Se", "preferences-system", "settings theme configuration preferences", CORE_SETTINGS },
};

static void run_core_action(int action);
static void launch_cmd_for(const struct desktop_app *a, char *out, int cap);
static void build_pins(void);

/* Desktop/taskbar pins are deliberately just the existing "pins" list:
 * pinning an installed app adds its .desktop id there, which makes it a
 * first-class desktop icon (with that app's real icon) and taskbar pin,
 * and persists across restarts in the normal config file. */
static int app_is_pinned(int app_idx)
{
    if (app_idx < 0 || app_idx >= napps) return 0;
    for (int i = 0; i < npins; i++)
        if (pins[i].action == CORE_NONE && !strcasecmp(pins[i].match, apps[app_idx].id)) return 1;
    return 0;
}

static void pins_sync_config(void)
{
    cfg.pins[0] = 0;
    for (int i = 0; i < npins; i++) {
        const char *tok = pins[i].action == CORE_TERMINAL ? "terminal" :
                          pins[i].action == CORE_FILES ? "files" :
                          pins[i].action == CORE_SYSMON ? "sysmon" :
                          pins[i].action == CORE_SETTINGS ? "settings" :
                          pins[i].match[0] ? pins[i].match : pins[i].command;
        if (i) strncat(cfg.pins, ",", sizeof(cfg.pins) - strlen(cfg.pins) - 1);
        strncat(cfg.pins, tok, sizeof(cfg.pins) - strlen(cfg.pins) - 1);
    }
}

static void toggle_app_pin(int app_idx)
{
    if (app_idx < 0 || app_idx >= napps) return;
    if (app_is_pinned(app_idx)) {
        for (int i = 0; i < npins; i++) {
            if (pins[i].action == CORE_NONE && !strcasecmp(pins[i].match, apps[app_idx].id)) {
                memmove(&pins[i], &pins[i + 1], (npins - i - 1) * sizeof(pins[0]));
                npins--;
                break;
            }
        }
        pins_sync_config();
    } else {
        if (cfg.pins[0]) strncat(cfg.pins, ",", sizeof(cfg.pins) - strlen(cfg.pins) - 1);
        strncat(cfg.pins, apps[app_idx].id, sizeof(cfg.pins) - strlen(cfg.pins) - 1);
    }
    build_pins();
    desk_sel_mask = 0;
    save_settings("Desktop pins");
    redraw_everything();
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

static void launch_pin(int i)
{
    if (i < 0 || i >= npins) return;
    if (pins[i].action != CORE_NONE) { run_core_action(pins[i].action); return; }
    struct client *match = NULL;
    for (int k = 0; k < nclients; k++)
        if (pins[i].match[0] && strstr(clients[k]->wclass, pins[i].match)) match = clients[k];
    if (match) activate(match, 1);
    else spawn(pins[i].command);
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
enum { MENU_NONE, MENU_POWER, MENU_DESKTOP, MENU_TITLE, MENU_PICKER = 98, MENU_MESSAGE = 99 };
static int menu_kind, menu_n;
static int menu_hover = -1;
static char menu_labels[8][64];
static Window menu_target;           /* title menu: the client acted on */
static Window menu_item_targets[8];  /* stable window IDs for picker rows */
static int menu_bar_release;         /* ignore release from the opening click */
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
static int xicon_fill(struct xicon *xi, struct rgba_image *img)
{
    if (!img || !img->pixels) return 0;
    if (!rgba_image_scale_fit(img, xi->size, xi->size)) return 0;
    xi->w = img->w; xi->h = img->h;
    xi->pix = XCreatePixmap(dpy, root, xi->w, xi->h, DefaultDepth(dpy, scr_no));
    xi->mask = XCreatePixmap(dpy, root, xi->w, xi->h, 1);
    if (!xi->pix || !xi->mask) return 0;

    XImage *im = XCreateImage(dpy, DefaultVisual(dpy, scr_no), DefaultDepth(dpy, scr_no),
                              ZPixmap, 0, NULL, xi->w, xi->h, 32, 0);
    if (!im) return 0;
    im->data = malloc((size_t)im->bytes_per_line * xi->h);
    if (!im->data) { XFree(im); return 0; }
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
    if (xi->pix) XFreePixmap(dpy, xi->pix);
    if (xi->mask) XFreePixmap(dpy, xi->mask);
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
    if (!xicon_fill(xi, &img)) { free(xi); xi = NULL; }
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
    *x = 16 + (i % 8) * DESK_COL_W;
    *y = 16 + (i / 8) * DESK_ROW_H;
}

static void paint_desktop(Drawable d)
{
    /* Paint everything into the supplied drawable. When draw_desktop()
     * passes its wallpaper pixmap, the gradient, grid and icons must all
     * land in that pixmap: painting any layer straight onto the root is
     * then immediately covered by the pixmap copy, which both hid the
     * grid and made every repaint visibly flicker. */
    for (int y = 0; y < screen_h; y++) {
        unsigned int rgb = color_blend_rgb(theme.bg_top, theme.bg_bot,
                                           screen_h > 1 ? y * 255 / (screen_h - 1) : 0);
        XSetForeground(dpy, gc, alloc_rgb(rgb));
        XFillRectangle(dpy, d, gc, 0, y, screen_w, 1);
    }
    XSetForeground(dpy, gc, PX.grid);
    for (int x = 0; x < screen_w; x += 64) XDrawLine(dpy, d, gc, x, 0, x, screen_h);
    for (int y = 0; y < screen_h; y += 64) XDrawLine(dpy, d, gc, 0, y, screen_w, y);
    /* The grid alone only promised a line at the origin. Frame the actual
     * usable desktop explicitly so the right and bottom edges cannot
     * disappear merely because the screen size is not a multiple of 64. */
    rect(d, 0, 0, screen_w, screen_h - bar_h, PX.main);

    /* desktop icons on the SCos 88x96 snap grid */
    for (int i = 0; i < npins; i++) {
        int x, y; icon_rect(i, &x, &y);
        if (desk_sel_mask & (1ul << i)) {
            fill(d, x, y, 80, 88, alloc_rgb(color_blend_rgb(theme.bg_top, theme.main, 30)));
            rect(d, x, y, 80, 88, PX.main);
        } else if (i == desk_hover) {
            fill(d, x, y, 80, 88, alloc_rgb(color_blend_rgb(theme.bg_top, theme.main, 16)));
            rect(d, x, y, 80, 88, PX.title_off);
        }
        /* proper freedesktop image at the normal 24x24 SCos size;
         * the framed letter glyph remains only as a fallback */
        int gx = x + 28, gy = y + 8;
        struct xicon *xi = xicon_get(pins[i].icon, 24);
        if (xicon_drawn(xi)) {
            draw_xicon(d, xi, gx + (24 - xi->w) / 2, gy + (24 - xi->h) / 2);
        } else {
            rect(d, gx, gy, 24, 24, PX.main);
            int gw = text_w(pins[i].glyph);
            text_out(d, gx + (24 - gw) / 2, gy + 8 + font->ascent, pins[i].glyph, PX.main);
        }
        int lw = text_w(pins[i].label);
        if (lw > 72) lw = 72;
        clipped_text(d, x + (80 - lw) / 2, y + 40 + font->ascent,
                     pins[i].label, 72, PX.text);
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
}

static void expose_desktop(void)
{
    if (desk_pm && desk_pm_w == screen_w && desk_pm_h == screen_h)
        XCopyArea(dpy, desk_pm, root, gc, 0, 0, screen_w, screen_h, 0, 0);
    else
        draw_desktop();
}

/* ------------------------------------------------------------------- bar */
struct bar_item { int idx; int x, w; };
static struct bar_item bar_items[96];
static int bar_n;

static int bar_power_x(void)
{
    /* power button + clock strip reserved before any app buttons */
    return screen_w - 100 - 34 - 16;
}

static void bar_layout(void)
{
    bar_n = 0;
    int x = 168;
    int end = bar_power_x() - 8;
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
    fill(d, 8, by, 34, bh, launch_open || bar_hover == -2 ? PX.bar_hi : PX.taskbar_bg);
    rect(d, 8, by, 34, bh, PX.main);
    int cell_h = bh < 24 ? 4 : 6;
    int grid_h = cell_h * 2 + 3;
    int grid_x = 8 + (34 - 19) / 2, grid_y = by + (bh - grid_h) / 2;
    for (int q = 0; q < 4; q++)
        fill(d, grid_x + (q % 2) * 11, grid_y + (q / 2) * (cell_h + 3), 8, cell_h,
             launch_open ? PX.title_text : PX.main);
    /* Windows N picker */
    char wl[32]; snprintf(wl, sizeof(wl), "Windows %d", nclients);
    fill(d, 48, by, 110, bh, bar_hover == -3 ? PX.bar_hi : PX.taskbar_bg);
    rect(d, 48, by, 110, bh, PX.main);
    clipped_text(d, 56, centered_baseline(by, bh), wl, 94, PX.text);

    bar_layout();
    for (int i = 0; i < bar_n; i++) {
        struct bar_item *it = &bar_items[i];
        struct pin *p = &pins[it->idx];
        int lit = bar_press == i || bar_hover == i ||
                  (bar_flash_pin == it->idx && time(NULL) <= bar_flash_until);
        fill(d, it->x, by, 34, bh, lit ? PX.bar_hi : PX.taskbar_bg);
        rect(d, it->x, by, 34, bh, PX.main);
        struct xicon *xi = xicon_get(p->icon, bar_icon_size(20));
        if (xicon_drawn(xi))
            draw_xicon(d, xi, it->x + (34 - xi->w) / 2, by + (bh - xi->h) / 2);
        else {
            int gw = text_w(p->glyph);
            text_out(d, it->x + (34 - gw) / 2, centered_baseline(by, bh), p->glyph, PX.text);
        }
    }
    /* power button */
    int pw = 34, px = bar_power_x();
    fill(d, px, by, pw, bh, bar_hover == -4 ? PX.bar_hi : PX.taskbar_bg);
    rect(d, px, by, pw, bh, PX.main);
    XSetForeground(dpy, gc, PX.main);
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
    text_out(d, 10, 8 + font->ascent, launch_pin_mode ? "Add to Home: click app to pin/unpin" : launch_all && !launch_q[0] ? "All installed apps - Esc goes back" : launch_q[0] ? "Search apps - right-click pins to Home" : "Core apps - type to search all", PX.main);
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
        if (launch_offset + r == launch_sel) fill(d, 6, yy - 3, 288, 22, PX.bar_hi);
        struct xicon *xi = xicon_get(it->icon, 20);
        if (xicon_drawn(xi)) draw_xicon(d, xi, 10, yy - 1);
        else text_out(d, 12, yy + 4 + font->ascent, it->glyph, PX.main);
        /* SCos keeps launcher names in the normal text colour on hover;
         * title_text is for solid accent titlebars and was too dark here. */
        clipped_text(d, 44, yy + 4 + font->ascent, it->name, it->app_idx >= 0 && app_is_pinned(it->app_idx) ? 205 : 248,
                     PX.text);
        if (it->app_idx >= 0 && app_is_pinned(it->app_idx))
            text_out(d, 262, yy + 4 + font->ascent, "PIN", PX.main);
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
        static const char *items[] = { "Lock", "Restart WM", "Exit WM", "Suspend", "Reboot", "Power Off" };
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
}

static Pixmap menu_pm = None;
static int menu_pm_w, menu_pm_h;

static void draw_menu(void)
{
    if (menu_kind == MENU_NONE) return;
    int w = menu_w, h = menu_n * 24 + 6;
    Pixmap pm = surface_backing(menu_win, &menu_pm, &menu_pm_w, &menu_pm_h, w, h);
    Drawable d = pm ? pm : menu_win;
    fill(d, 0, 0, w, h, PX.win_bg);
    rect(d, 0, 0, w, h, PX.main);
    for (int i = 0; i < menu_n; i++) {
        int row_y = 3 + i * 24;
        if (i == menu_hover) fill(d, 2, row_y, w - 4, 24, PX.bar_hi);
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
        switch (idx) {
        case 0: run_core_action(CORE_TERMINAL); break;
        case 1: run_core_action(CORE_FILES); break;
        case 2: run_core_action(CORE_SYSMON); break;
        case 3: run_core_action(CORE_SETTINGS); break;
        case 4: launch_pin_mode = 1; launcher_toggle(1); break;
        case 5: launcher_toggle(1); break;
        case 6:
            while (nclients) remove_client(clients[0], 1);
            execvp("/proc/self/exe", saved_argv);
            break;
        case 7: running = 0; break;
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

static void pick_window(Window target)
{
    /* Rows keep their own window IDs: another client closing while the
     * menu is open must not make a click activate a different window. */
    struct client *c = find_client(target);
    if (c) activate(c, 1);
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
    if (e->type == ButtonPress && w != root && desk_sel_mask) {
        desk_sel_mask = 0;
        draw_desktop();
    }

    /* Taskbar menus open on press. Do not let the release from that very
     * same click immediately dismiss the menu before its next click. */
    if (menu_kind != MENU_NONE && menu_bar_release &&
        e->type == ButtonRelease && w == bar_win) {
        menu_bar_release = 0;
        return;
    }
    if (menu_kind != MENU_NONE && w != menu_win) {
        menu_close();
        if (passive_client_press) XAllowEvents(dpy, ReplayPointer, e->time);
        return;
    }
    if (w == menu_win) {
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
        XUngrabPointer(dpy, CurrentTime);
        if (drag.kind == 3) {
            drag.band_x = x; drag.band_y = y;
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
        if (e->x >= pw_x && e->x < pw_x + 34 && e->y >= 6 && e->y < bar_h - 6) {
            menu_open(MENU_POWER, pw_x - 140, screen_h - bar_h - (6 * 24 + 8), None);
            menu_bar_release = 1;
            return;
        }
        for (int i = 0; i < bar_n; i++) {
            struct bar_item *it = &bar_items[i];
            if (e->x >= it->x && e->x < it->x + it->w &&
                e->y >= 6 && e->y < bar_h - 6) {
                if (e->button == Button1) {
                    /* Instant visible feedback on mousedown, plus a
                     * short "starting" flash afterwards: launching a
                     * real app can take a second or two, and with no
                     * response at all a normal click looked like it
                     * had to be held down to register. */
                    bar_press = i;
                    bar_flash_pin = it->idx;
                    bar_flash_until = time(NULL) + 2;
                    draw_bar();
                    launch_pin(it->idx);
                    bar_press = -1; /* the two-second launch flash is not a held button */
                    draw_bar();
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
            idx >= 0 && idx < n && launch_items[idx].app_idx >= 0) {
            toggle_app_pin(launch_items[idx].app_idx);
            draw_launcher();
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
            for (int i = 0; i < npins; i++) {
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
        /* rubber band on empty desktop: clear any icon selection at once,
         * otherwise the old highlight stayed painted until button release
         * and looked like clicking off an icon never un-highlighted it */
        if (desk_sel_mask) { desk_sel_mask = 0; draw_desktop(); }
        drag.kind = 3; drag.start_x = drag.band_x = x; drag.start_y = drag.band_y = y;
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
    if (!drag.kind) {
        /* Hover tracking: nothing anywhere highlighted under the pointer,
         * which made the whole desktop feel unresponsive. Each surface
         * remembers what is hovered and repaints only when it changes. */
        if (menu_kind != MENU_NONE && e->window == menu_win) {
            int idx = (e->y - 3) / 24;
            if (idx < 0 || idx >= menu_n) idx = -1;
            if (idx != menu_hover) { menu_hover = idx; draw_menu(); }
        } else if (e->window == root) {
            int hit = -1;
            for (int i = 0; i < npins; i++) {
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

static void on_key(XKeyEvent *e)
{
    KeySym ks = XLookupKeysym(e, 0);
    unsigned int mods = e->state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);

    if (tool_kind != TOOL_NONE) { tool_key(e); return; }
    if (menu_kind != MENU_NONE) {
        if (ks == XK_Escape) menu_close();
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

        /* Removed non-core pseudo-apps are ignored rather than turned
         * into dead taskbar buttons. */
        if (!strcasecmp(tok, "web-browser") || !strcasecmp(tok, "browser")) continue;

        struct pin *p = &pins[npins];
        memset(p, 0, sizeof(*p));

        /* The four retained SCos core apps. */
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
            npins++;
            continue;
        }

        /* Then a user's real .desktop applications. */
        int found = -1;
        for (int i = 0; i < napps; i++)
            if (!strcasecmp(apps[i].id, tok)) found = i;
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
            npins++;
            continue;
        }

        /* Finally, an explicit bare command from the user's own config. */
        snprintf(p->label, sizeof(p->label), "%s", tok);
        snprintf(p->command, sizeof(p->command), "%s", tok);
        snprintf(p->match, sizeof(p->match), "%s", tok);
        snprintf(p->icon, sizeof(p->icon), "%s", tok);
        p->label[0] = (char)toupper((unsigned char)p->label[0]);
        pin_set_glyph(p, "");
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

/* Use the user's themed left_ptr on the desktop and WM chrome. The WM
 * never defined a cursor anywhere, so the root kept the bare X default
 * while applications showed their toolkit's themed cursor — two
 * different pointers on one desktop. libXcursor is loaded at runtime so
 * the WM keeps its libX11-only link line; without it we fall back to
 * the core font cursor. */
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
    at.background_pixel = PX.taskbar_bg;
    if (wm_cursor) at.cursor = wm_cursor;
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
    Window buffered[] = { bar_win, launch_win, menu_win, tool_win, anim_win };
    for (unsigned int i = 0; i < sizeof(buffered) / sizeof(buffered[0]); i++)
        XSetWindowBackgroundPixmap(dpy, buffered[i], None);
}


/* ------------------------------------------------------ core app panels */
/* Settings and SysMon are deliberately the only built-in windows.
 * They add no daemon and no background work: Settings only writes the
 * existing config file, and SysMon only samples /proc + /sys while it
 * is visible. Terminal and Files remain the user's real Linux programs. */
static int settings_field;              /* 0 none, 1 terminal, 2 files, 3 pins */
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
    for (int i = 0; i < nclients; i++) {
        XSetWindowBackground(dpy, clients[i]->frame, PX.win_bg);
        draw_frame(clients[i]);
    }
    draw_desktop();
    draw_bar();
    if (launch_open) draw_launcher();
    if (menu_kind != MENU_NONE) draw_menu();
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
    for (int i = 0; i < nclients; i++) {
        xicon_destroy_owned(clients[i]->icon);
        clients[i]->icon = client_load_icon(clients[i]);
    }
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

static char *settings_target(int field, size_t *cap)
{
    if (field == 1) { *cap = sizeof(cfg.terminal); return cfg.terminal; }
    if (field == 2) { *cap = sizeof(cfg.files); return cfg.files; }
    if (field == 3) { *cap = sizeof(cfg.pins); return cfg.pins; }
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
    int was_pins = settings_field == 3;
    settings_field = 0;
    if (was_pins) { build_pins(); desk_sel_mask = 0; redraw_everything(); }
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
            snprintf(sysmon_state.gpu, sizeof(sysmon_state.gpu), "GPU driver: %.230s (card%d)", driver, card);
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
            snprintf(sysmon_state.temperature, sizeof(sysmon_state.temperature), "%s: %.1f C", name, milli / 1000.0);
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

static void tool_close(void)
{
    if (tool_kind == TOOL_NONE) return;
    tool_kind = TOOL_NONE;
    tool_release_pending = 0;
    settings_field = 0;
    XUnmapWindow(dpy, tool_win);
    XUngrabKeyboard(dpy, CurrentTime);
}

static void tool_open(int kind)
{
    menu_close();
    if (launch_open) launcher_toggle(0);
    tool_kind = kind;
    tool_release_pending = 1;
    settings_field = 0;
    if (kind == TOOL_SYSMON) refresh_sysmon();
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

static void draw_tool(void)
{
    if (tool_kind == TOOL_NONE) return;
    Pixmap pm = surface_backing(tool_win, &tool_pm, &tool_pm_w, &tool_pm_h, TOOL_W, TOOL_H);
    Drawable d = pm ? pm : tool_win;
    fill(d, 0, 0, TOOL_W, TOOL_H, PX.win_bg);
    rect(d, 0, 0, TOOL_W, TOOL_H, PX.main);
    fill(d, 1, 1, TOOL_W - 2, 25, PX.main);
    text_out(d, 10, 6 + font->ascent, tool_kind == TOOL_SETTINGS ? "Settings" : "SysMon", PX.title_text);
    draw_button_glyph(d, TOOL_W - 27, 4, 2, PX.title_text);

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
        fill(d, x, y, 120, 40, selected ? PX.bar_hi : PX.win_bg);
        rect(d, x, y, 120, 40, PX.main);
        int tw = text_w(scos_themes[i].name);
        text_out(d, x + (120 - tw) / 2, y + 14 + font->ascent, scos_themes[i].name,
                 selected ? PX.title_text : PX.text);
    }

    static const char *labels[4] = { "", "Terminal", "Files", "Pins" };
    for (int field = 1; field <= 3; field++) {
        int y = 124 + (field - 1) * 44;
        text_out(d, 16, y + 9 + font->ascent, labels[field], PX.main);
        fill(d, 100, y, 424, 30, PX.win_bg);
        rect(d, 100, y, 424, 30, settings_field == field ? PX.title_text : PX.main);
        char shown[1056];
        size_t cap = 0; char *target = settings_target(field, &cap); (void)cap;
        if (settings_field == field) snprintf(shown, sizeof(shown), "%s", settings_edit);
        else if (target && target[0]) snprintf(shown, sizeof(shown), "%s", target);
        else if (field == 1) snprintf(shown, sizeof(shown), "(auto: %s)", detect_terminal());
        else if (field == 2) {
            const char *e = detect_files_exec();
            snprintf(shown, sizeof(shown), "(auto: %s)", e ? e : "xdg-open");
        } else snprintf(shown, sizeof(shown), "%s", cfg.pins);
        clipped_text(d, 108, y + 9 + font->ascent, shown, 390, PX.text);
        if (settings_field == field && (time(NULL) % 2) == 0) {
            int cx = 108 + text_w(settings_edit);
            if (cx < 518) fill(d, cx, y + 8, 2, 14, PX.main);
        }
    }

    text_out(d, 16, 268 + font->ascent, "Taskbar", PX.main);
    rect(d, 100, 256, 34, 30, PX.main);
    text_out(d, 112, 265 + font->ascent, "-", PX.text);
    char bh[32]; snprintf(bh, sizeof(bh), "%d px", cfg.bar_height);
    text_out(d, 146, 265 + font->ascent, bh, PX.text);
    rect(d, 204, 256, 34, 30, PX.main);
    text_out(d, 216, 265 + font->ascent, "+", PX.text);

    clipped_text(d, 16, 316, settings_status[0] ? settings_status :
                 "Theme and taskbar apply immediately. Click a field to edit it.", 508, PX.text);
    text_out(d, 16, TOOL_H - 18, "Single workspace by design - no pager or workspace daemon", PX.title_off);
    surface_present(tool_win, pm, TOOL_W, TOOL_H);
}

static int tool_button(XButtonEvent *e)
{
    if (e->type != ButtonPress || e->button != Button1) return 1;
    if (e->x >= TOOL_W - 30 && e->y < 28) { tool_close(); return 1; }
    if (tool_kind == TOOL_SETTINGS) {
        if (e->y >= 62 && e->y < 102) {
            for (int i = 0; i < scos_theme_count; i++) {
                int x = 16 + i * 128;
                if (e->x >= x && e->x < x + 120) { apply_theme_live(i); draw_tool(); return 1; }
            }
        }
        for (int field = 1; field <= 3; field++) {
            int y = 124 + (field - 1) * 44;
            if (e->x >= 100 && e->x < 524 && e->y >= y && e->y < y + 30) {
                start_settings_field(field); draw_tool(); return 1;
            }
        }
        if (e->y >= 256 && e->y < 286 && e->x >= 100 && e->x < 134) {
            apply_bar_height_live(cfg.bar_height - 4); draw_tool(); return 1;
        }
        if (e->y >= 256 && e->y < 286 && e->x >= 204 && e->x < 238) {
            apply_bar_height_live(cfg.bar_height + 4); draw_tool(); return 1;
        }
    }
    return 1;
}

static int tool_key(XKeyEvent *e)
{
    KeySym ks = XLookupKeysym(e, 0);
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
            int next = settings_field % 3 + 1;
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
                     StructureNotifyMask | ButtonPressMask | PropertyChangeMask |
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
        /* 1 Hz clock/tick + shutdown check, without busy-waiting */
        int fd = ConnectionNumber(dpy);
        fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
        struct timeval tv = { 0, 250000 };
        if (select(fd + 1, &rf, NULL, NULL, &tv) == 0) {
            time_t now = time(NULL);
            if (now != last_clock) {
                last_clock = now; draw_bar();
                if (tool_kind == TOOL_SYSMON) { refresh_sysmon(); draw_tool(); }
                else if (tool_kind == TOOL_SETTINGS && settings_field) draw_tool();
            }
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
