/* External file-manager-like X11 client used by test_wm_graphical.js.
 * It is launched through SCosWM's configured Files command, not by the
 * Node X server, and supplies multi-size icons like a real Linux app. */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void set_icons(Display *dpy, Window win, unsigned int phase)
{
    static const int sizes[] = { 16, 32, 48, 64 };
    size_t count = 0;
    for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        count += 2 + (size_t)sizes[i] * (size_t)sizes[i];
    unsigned long *icon = calloc(count, sizeof(*icon));
    if (!icon) return;
    size_t at = 0;
    for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int size = sizes[i];
        icon[at++] = (unsigned long)size;
        icon[at++] = (unsigned long)size;
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                unsigned int edge = x == 0 || y == 0 || x == size - 1 || y == size - 1;
                unsigned int r = edge ? 0x39 : (phase * 37 + (unsigned int)x * 3) & 255;
                unsigned int g = edge ? 0xff : (80 + (unsigned int)y * 2) & 255;
                unsigned int b = edge ? 0x14 : 0x30;
                icon[at++] = 0xff000000UL | (unsigned long)(r << 16) |
                             (unsigned long)(g << 8) | b;
            }
        }
    }
    Atom net_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
    XChangeProperty(dpy, win, net_icon, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)icon, (int)at);
    free(icon);
}

static void draw_content(Display *dpy, Window win, GC gc)
{
    XSetForeground(dpy, gc, 0x3465a4);
    XFillRectangle(dpy, win, gc, 0, 0, 460, 320);
    XSetForeground(dpy, gc, 0xc03030);
    XFillRectangle(dpy, win, gc, 20, 30, 120, 100);
    XSetForeground(dpy, gc, 0xd0c040);
    XFillRectangle(dpy, win, gc, 160, 30, 120, 100);
    XSetForeground(dpy, gc, 0x204020);
    XFillRectangle(dpy, win, gc, 20, 160, 260, 100);
}

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 2;
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Window win = XCreateSimpleWindow(dpy, root, 200, 150, 460, 320, 0,
                                     BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    if (!win) return 3;
    XStoreName(dpy, win, "Home - File Manager");
    XClassHint hint = { "thunar", "Thunar" };
    XSetClassHint(dpy, win, &hint);
    Atom protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    Atom delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &delete_window, 1);
    XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);
    set_icons(dpy, win, 0);
    GC gc = XCreateGC(dpy, win, 0, NULL);
    if (!gc) return 4;
    XMapWindow(dpy, win);
    draw_content(dpy, win, gc);
    XFlush(dpy);

    unsigned int phase = 0;
    for (;;) {
        while (XPending(dpy)) {
            XEvent event;
            XNextEvent(dpy, &event);
            if (event.type == Expose) draw_content(dpy, win, gc);
            if (event.type == DestroyNotify) {
                XFreeGC(dpy, gc);
                XCloseDisplay(dpy);
                return 0;
            }
            if (event.type == ClientMessage &&
                (Atom)event.xclient.message_type == protocols &&
                (Atom)event.xclient.data.l[0] == delete_window) {
                XDestroyWindow(dpy, win);
                XFreeGC(dpy, gc);
                XCloseDisplay(dpy);
                return 0;
            }
        }
        /* Real file managers can replace their icon while initializing.
         * Exercise WM PropertyNotify destruction/reload as well as manage. */
        set_icons(dpy, win, ++phase);
        XFlush(dpy);
        usleep(20000);
    }
}
