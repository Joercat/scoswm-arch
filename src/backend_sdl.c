/* SDL2 backend: the SCos desktop in a host window.
 *
 * SDL2 is used (rather than native X11/Wayland code) so the same binary
 * works on X11 and Wayland sessions and on other Unix-like systems:
 * SDL presents one streaming texture (the WM's RAM back buffer) and
 * host keyboard/mouse events are translated into the SCos event queues
 * the WM already speaks.
 */
#include "scos.h"
#include "platform.h"

#include <SDL.h>
#include <stdio.h>

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int bw, bh;
static u8 button_mask;

int platform_backend_init(int width, int height, int fullscreen)
{
    bw = width; bh = height;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "scoswm: SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    win = SDL_CreateWindow("SCos - scoswm",
                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           width, height,
                           SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
                           (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) { fprintf(stderr, "scoswm: SDL_CreateWindow: %s\n", SDL_GetError()); return -1; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "scoswm: SDL_CreateRenderer: %s\n", SDL_GetError()); return -1; }
    /* Host-window resizes scale (letterboxed) instead of resizing the
     * SCos desktop, whose layout is fixed at startup like the GOP mode
     * was on bare metal. */
    SDL_RenderSetLogicalSize(ren, width, height);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888,
                            SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!tex) { fprintf(stderr, "scoswm: SDL_CreateTexture: %s\n", SDL_GetError()); return -1; }
    SDL_StartTextInput();
    return 0;
}

void platform_backend_shutdown(void)
{
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
}

void platform_present(const u32 *px, int sw, int x, int y, int w, int h)
{
    if (!tex) return;
    SDL_Rect r = { x, y, w, h };
    SDL_UpdateTexture(tex, &r, px + (size_t)y * sw + x, sw * 4);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

void platform_note_present(void) { }

int platform_screenshot(const char *path)
{
    (void)path;   /* screenshots are a headless-backend feature */
    return 0;
}

/* ------------------------------------------------------------ translation */
static u8 sdl_buttons(void)
{
    u32 s = SDL_GetMouseState(NULL, NULL);
    u8 m = 0;
    if (s & SDL_BUTTON_LMASK) m |= MBTN_LEFT;
    if (s & SDL_BUTTON_RMASK) m |= MBTN_RIGHT;
    if (s & SDL_BUTTON_MMASK) m |= MBTN_MIDDLE;
    return m;
}

static void push_key(u16 keycode, int pressed, SDL_Keymod mod)
{
    struct key_event e = { 0 };
    e.keycode = keycode;
    e.pressed = pressed ? 1 : 0;
    e.shift = (mod & KMOD_SHIFT) ? 1 : 0;
    e.ctrl  = (mod & KMOD_CTRL) ? 1 : 0;
    e.alt   = (mod & KMOD_ALT) ? 1 : 0;
    scos_input_push_key(&e);
}

/* Returns a SCos keycode for non-printable keys, 0 for "not special". */
static u16 special_key(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UP: return KEY_UP;         case SDLK_DOWN: return KEY_DOWN;
    case SDLK_LEFT: return KEY_LEFT;     case SDLK_RIGHT: return KEY_RIGHT;
    case SDLK_HOME: return KEY_HOME;     case SDLK_END: return KEY_END;
    case SDLK_PAGEUP: return KEY_PGUP;   case SDLK_PAGEDOWN: return KEY_PGDN;
    case SDLK_DELETE: return KEY_DELETE; case SDLK_INSERT: return KEY_INSERT;
    case SDLK_F1: return KEY_F1;   case SDLK_F2: return KEY_F2;
    case SDLK_F3: return KEY_F3;   case SDLK_F4: return KEY_F4;
    case SDLK_F5: return KEY_F5;   case SDLK_F6: return KEY_F6;
    case SDLK_F7: return KEY_F7;   case SDLK_F8: return KEY_F8;
    case SDLK_F9: return KEY_F9;   case SDLK_F10: return KEY_F10;
    case SDLK_F11: return KEY_F11; case SDLK_F12: return KEY_F12;
    case SDLK_RETURN: case SDLK_KP_ENTER: return '\n';
    case SDLK_ESCAPE: return 27;
    case SDLK_BACKSPACE: return '\b';
    case SDLK_TAB: return '\t';
    default: return 0;
    }
}

void platform_pump(void)
{
    platform_tick_update();
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            platform_request_quit();
            break;
        case SDL_KEYDOWN: case SDL_KEYUP: {
            int pressed = ev.type == SDL_KEYDOWN;
            SDL_Keymod mod = SDL_GetModState();
            u16 sp = special_key(ev.key.keysym.sym);
            if (sp) { push_key(sp, pressed, mod); break; }
            if (!pressed) break;
            if (mod & (KMOD_CTRL | KMOD_ALT)) {
                /* Ctrl/Alt combos (Ctrl+T, Ctrl+Alt+Del, ...) never reach
                 * SDL_TEXTINPUT, so deliver them from the key event. */
                SDL_Keycode k = ev.key.keysym.sym;
                if (k >= 'a' && k <= 'z') push_key((u16)k, 1, mod);
                else if (k == ' ') push_key(' ', 1, mod);
            }
            /* plain printable keys arrive via SDL_TEXTINPUT below */
            break;
        }
        case SDL_TEXTINPUT:
            /* one event, usually one ASCII char on this desktop */
            for (const char *q = ev.text.text; *q; q++)
                if ((u8)*q >= 32 && (u8)*q < 127) push_key((u16)(u8)*q, 1, SDL_GetModState());
            break;
        case SDL_MOUSEMOTION: {
            struct mouse_event e = { 0 };
            e.type = MEV_MOVE;
            e.dx = (i16)ev.motion.xrel;
            e.dy = (i16)-ev.motion.yrel;  /* WM expects PS/2-style: +dy is up */
            e.buttons = sdl_buttons();
            scos_input_push_mouse(&e);
            break;
        }
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            u8 bit = ev.button.button == SDL_BUTTON_LEFT ? MBTN_LEFT
                   : ev.button.button == SDL_BUTTON_RIGHT ? MBTN_RIGHT : MBTN_MIDDLE;
            if (ev.type == SDL_MOUSEBUTTONDOWN) button_mask |= bit; else button_mask &= (u8)~bit;
            struct mouse_event e = { 0 };
            e.type = MEV_BUTTON;
            e.buttons = button_mask;
            e.button = bit;
            e.down = ev.type == SDL_MOUSEBUTTONDOWN ? 1 : 0;
            scos_input_push_mouse(&e);
            break;
        }
        case SDL_MOUSEWHEEL: {
            struct mouse_event e = { 0 };
            e.type = MEV_WHEEL;
            e.wheel = (i8)(ev.wheel.y ? ev.wheel.y : ev.wheel.x);
            e.buttons = button_mask;
            scos_input_push_mouse(&e);
            break;
        }
        default:
            break;
        }
    }
}

void platform_wait_idle(void)
{
    SDL_Delay(4);
    platform_tick_update();
}
