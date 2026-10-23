/* Host-backend interface for the SCos Linux port.
 *
 * Exactly one backend is linked into a binary:
 *   backend_sdl.c       - a real window on X11/Wayland (needs SDL2)
 *   backend_headless.c  - no display; used by `make check` / CI and for
 *                         scripted screenshots (--frames/--screenshot)
 *
 * The WM core only ever sees platform_linux.c, which forwards to these.
 */
#ifndef SCOS_PLATFORM_H
#define SCOS_PLATFORM_H

#include "scos.h"

/* main() lives in main.c and owns argument parsing plus the desktop
 * startup sequence; each backend only implements the hooks below. */
void platform_data_dir_init(const char *override);
void platform_tick_update(void);

/* Called once by scos_main() before fb_init(). */
int  platform_backend_init(int width, int height, int fullscreen);
void platform_backend_shutdown(void);

/* Enqueue host input (backends) / dequeue it (platform_linux.c). */
void scos_input_push_key(const struct key_event *e);
void scos_input_push_mouse(const struct mouse_event *e);

/* Headless/testing hooks (no-ops in the SDL backend). */
void platform_note_present(void);          /* one presented frame happened */
int  platform_screenshot(const char *path);/* dump last presented frame */

/* Options shared with backends (set by scos_main argument parsing helpers). */
extern int opt_frames;          /* headless: quit after N presented frames, 0 = run forever */
extern const char *opt_screenshot;
extern const char *opt_click;   /* headless: "x,y" synthetic click */
extern const char *opt_type;    /* headless: text typed after the click/open */

#endif
