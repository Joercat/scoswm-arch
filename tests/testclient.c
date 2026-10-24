/* tiny real X11 client used to prove scoswm manages real apps */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "client: no display\n"); return 1; }
    int s = DefaultScreen(d);
    Window w = XCreateSimpleWindow(d, RootWindow(d, s), 120, 90, 420, 280, 1,
                                   BlackPixel(d, s), 0x224488);
    XStoreName(d, w, argc > 1 ? argv[1] : "Test Window");
    XClassHint ch = { "testclient", "Testclient" };
    XSetClassHint(d, w, &ch);
    XSelectInput(d, w, ExposureMask | StructureNotifyMask);
    XMapWindow(d, w);
    XFlush(d);
    /* stay alive, honouring redraws, until killed */
    while (1) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type == Expose) {
            GC gc = XCreateGC(d, w, 0, NULL);
            XSetForeground(d, gc, 0x224488);
            XFillRectangle(d, w, gc, 0, 0, 4000, 4000);
            XSetForeground(d, gc, 0xFFFFFF);
            XDrawString(d, w, gc, 20, 30, "hello from a real X11 client", 29);
            XFreeGC(d, gc);
        }
    }
    return 0;
}
