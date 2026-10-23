/* Headless backend: no window, no SDL.  Presents are counted (and, when
 * --screenshot is used, accumulated into a frame copy), synthetic input
 * from --click/--type is injected, and the run ends after --frames
 * presented frames.  This is what `make check` and CI run; it exercises
 * the exact WM code the SDL backend runs.
 */
#include "scos.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int bw, bh;
static u32 *frame;                 /* only allocated for --screenshot */
static long frames;
static int click_stage;            /* 0 idle, 1 down sent, 2 up sent, 3 typing */
static int type_idx;
static int cur_x, cur_y;           /* assumed cursor (WM starts it centred) */

int platform_backend_init(int width, int height, int fullscreen)
{
    (void)fullscreen;
    bw = width; bh = height;
    cur_x = width / 2; cur_y = height / 2;
    if (opt_screenshot) {
        frame = calloc((size_t)width * height, sizeof(u32));
        if (!frame) return -1;
    }
    return 0;
}

void platform_backend_shutdown(void) { free(frame); frame = NULL; }

void platform_present(const u32 *px, int sw, int x, int y, int w, int h)
{
    if (!frame) return;
    for (int r = 0; r < h; r++)
        memcpy(frame + (size_t)(y + r) * bw + x, px + (size_t)(y + r) * sw + x,
               (size_t)w * sizeof(u32));
}

void platform_note_present(void)
{
    frames++;
    if (opt_frames > 0 && frames >= opt_frames) platform_request_quit();
}

static void push_key(u16 keycode)
{
    struct key_event e = { 0 };
    e.keycode = keycode;
    e.pressed = 1;
    scos_input_push_key(&e);
}

static void push_mouse(u8 type, i16 dx, i16 dy, u8 buttons, u8 button, u8 down)
{
    struct mouse_event e = { 0 };
    e.type = type; e.dx = dx; e.dy = dy;
    e.buttons = buttons; e.button = button; e.down = down;
    scos_input_push_mouse(&e);
}

void platform_pump(void)
{
    platform_tick_update();
    if (platform_quit_requested()) return;

    /* Synthetic input, staged across pumps so the WM sees the same
     * move -> down -> up -> keys sequence a hand would produce.  The WM
     * starts its cursor at the screen centre and applies
     * (dx * sens) / 3 with the default sens of 2, hence the *3/2. */
    if (opt_click && click_stage == 0 && frames >= 2) {
        int tx = cur_x, ty = cur_y;
        if (sscanf(opt_click, "%d,%d", &tx, &ty) == 2) {
            push_mouse(MEV_MOVE, (i16)((tx - cur_x) * 3 / 2), (i16)(-(ty - cur_y) * 3 / 2),
                       0, 0, 0);
            cur_x = tx; cur_y = ty;
            push_mouse(MEV_BUTTON, 0, 0, MBTN_LEFT, MBTN_LEFT, 1);
            click_stage = 1;
        } else click_stage = 3;
    } else if (opt_click && click_stage == 1) {
        push_mouse(MEV_BUTTON, 0, 0, 0, MBTN_LEFT, 0);
        click_stage = 2;
    } else if (click_stage == 2 || (!opt_click && opt_type && click_stage == 0)) {
        click_stage = 3;
    } else if (opt_type && click_stage == 3 && frames >= 4) {
        if (opt_type[type_idx]) push_key((u16)(u8)opt_type[type_idx++]);
        else { push_key('\n'); click_stage = 4; }
    }
}

void platform_wait_idle(void)
{
    usleep(4000);
    platform_tick_update();
}

int platform_screenshot(const char *path)
{
    if (!frame || !path) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", bw, bh);
    for (int i = 0; i < bw * bh; i++) {
        u8 rgb[3] = { (u8)(frame[i] >> 16), (u8)(frame[i] >> 8), (u8)frame[i] };
        if (fwrite(rgb, 1, 3, f) != 3) { fclose(f); return 0; }
    }
    return fclose(f) == 0;
}
