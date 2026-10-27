/* dump the window tree + EWMH state for harness assertions */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Display *d;

static void dump(Window w, int depth)
{
    XWindowAttributes at;
    if (!XGetWindowAttributes(d, w, &at)) return;
    char *name = NULL; XFetchName(d, w, &name);
    Atom type; int fmt; unsigned long n = 0, after = 0; unsigned char *data = NULL;
    const char *state = "-";
    if (XGetWindowProperty(d, w, XInternAtom(d, "WM_STATE", 0), 0, 2, 0,
                           XInternAtom(d, "WM_STATE", 0), &type, &fmt, &n, &after, &data) == Success && data && n >= 1) {
        long s = ((long *)data)[0];
        state = s == 3 ? "Iconic" : s == 1 ? "Normal" : "?";
    }
    if (data) { XFree(data); data = NULL; }
    printf("%*s0x%lx \"%s\" %dx%d+%d+%d map=%d state=%s\n", depth * 2, "", w,
           name ? name : "", at.width, at.height, at.x, at.y, at.map_state, state);
    if (name) XFree(name);
    Window r, p, *kids = NULL; unsigned int nk = 0;
    if (XQueryTree(d, w, &r, &p, &kids, &nk)) {
        for (unsigned int i = 0; i < nk; i++) dump(kids[i], depth + 1);
        if (kids) XFree(kids);
    }
}

int main(void)
{
    d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "probe: no display\n"); return 1; }
    Window root = DefaultRootWindow(d);
    Atom type; int fmt; unsigned long n = 0, after = 0; unsigned char *data = NULL;
    if (XGetWindowProperty(d, root, XInternAtom(d, "_NET_ACTIVE_WINDOW", 0), 0, 1, 0, XA_WINDOW,
                           &type, &fmt, &n, &after, &data) == Success && data && n)
        printf("active=0x%lx\n", ((Window *)data)[0]);
    if (data) XFree(data);
    dump(root, 0);
    return 0;
}
