/* SCos application/desktop API — Linux port.
 *
 * Adapted from kernel/include/scos.h of SCos (bare-metal x86-64 OS).
 * The app-facing surface (windows, surfaces, themes, VFS, events) is kept
 * source-compatible so the original desktop/app code compiles unchanged
 * wherever possible.  Kernel-only services (PS/2, PIT, CMOS, PCI, GOP,
 * ACPI, page allocator) are replaced by the platform layer in
 * platform_linux.c / platform.h, which is documented in PORTING.md.
 */
#ifndef SCOS_H
#define SCOS_H

#include <stdint.h>
#include <stddef.h>

#define SCOS_BUILD_TAG "linux-port"
#if defined(__has_include)
#  if __has_include("scosbuild.h")
#    include "scosbuild.h"
#  endif
#endif
#ifndef SCOS_BUILD_COMMIT
#  define SCOS_BUILD_COMMIT "unstamped"
#  define SCOS_BUILD_BRANCH "unstamped"
#  define SCOS_BUILD_TREE "unknown"
#  define SCOS_BUILD_DATE "unknown"
#endif
void scos_build_stamp(char *out, int cap);
int scos_build_modified(void);

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
#ifndef true
#define true 1
#define false 0
#endif

/* 100 Hz tick, driven by the host monotonic clock (platform layer). */
extern volatile u64 timer_ticks;
#define tick_count timer_ticks
extern volatile int wm_in_idle;

/* ------------------------------------------------------------- string ---- */
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strcat(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strncat(char *d, const char *s, size_t n);
char *strstr(const char *h, const char *n);
char *str_chr(const char *s, char c);
char *str_str(const char *hay, const char *needle);

/* ------------------------------------------------------------ platform --- */
/* Implemented in platform_linux.c + the selected backend; see platform.h. */
void platform_pump(void);          /* update ticks, enqueue host input */
void platform_wait_idle(void);     /* sleep until next frame/input */
void platform_request_quit(void);
int  platform_quit_requested(void);
void platform_present(const u32 *px, int sw, int x, int y, int w, int h);
void platform_note_present(void);

void cpu_hlt(void);
void cpu_reboot_8042(void);        /* Linux: asks scoswm to exit (see PORTING.md) */
u64  rdtsc(void);                  /* monotonic ns counter (name kept for callers) */
extern u64 cpu_tsc_hz;             /* == 1e9: rdtsc() counts nanoseconds here */
u64  cpu_busy_cycles(void);
void cpu_meter_init(void);
void cpu_meter_tick(void);
void cpu_idle_begin(void);
u32  cpu_mhz(void);
u32  cpu_core_count(void);
u32  cpu_thread_count(void);
u32  cpu_signature(void);
void cpu_brand(char *out, int max);
u32  cpu_usage_pct(void);

/* No-op host stubs for bare-metal services the desktop code still names. */
void usb_init(void);
void usb_poll(void);
void usb_poll_events(void);
void usb_status(char *out, int max);
void usb_kbd_leds_off(void);
u64  usb_dma_bytes(void);
int  mouse_present(void);
void input_status(char *out, int max);
extern volatile u32 input_last_tick;
extern int input_guard_armed;
extern u32 input_key_enqueued, input_mouse_enqueued, input_key_dropped, input_mouse_dropped;

void error_screen(const char *subsys, const char *msg,
                  const char *const *dump, int ndump);
void err_notify(const char *subsys, const char *msg,
                const char *const *dump, int ndump);
int  err_pending(void);
void err_show_pending(void);
int  klog_ring_count(void);
int  klog_ring(int i, char *out, int max);
void klog(const char *fmt, ...);
void klog_raw(const char *s);

_Noreturn void panic(const char *reason);
void kernel_panic(const char *reason);

/* ----------------------------------------------------------------- pit ---- */
u32  uptime_ms(void);
void sleep_ms(u32 ms);

/* ------------------------------------------------------------ keyboard ---- */
enum {
    KEY_NONE = 0,
    KEY_UP = 128, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_DELETE, KEY_INSERT,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL, KEY_LALT, KEY_RALT,
    KEY_CAPS, KEY_NUM, KEY_SCROLL
};

struct key_event {
    u8   scancode;      /* unused on Linux (kept for source compatibility) */
    u16  keycode;       /* ascii or KEY_* code */
    u8   pressed;
    u8   shift, ctrl, alt;
};

int  kbd_poll(struct key_event *out);      /* 1 if an event was dequeued */

/* --------------------------------------------------------------- mouse ---- */
enum { MEV_MOVE = 1, MEV_BUTTON, MEV_WHEEL };
enum { MBTN_LEFT = 1, MBTN_RIGHT = 2, MBTN_MIDDLE = 4 };

struct mouse_event {
    u8  type;           /* MEV_* */
    i16 dx, dy;         /* relative motion (host convention converted by backend) */
    i8  wheel;
    u8  buttons;        /* button state bitmask after event */
    u8  button;         /* which button changed (for MEV_BUTTON) */
    u8  down;           /* 1 pressed / 0 released */
};

int  mouse_poll(struct mouse_event *out);

/* --------------------------------------------------------------- frame ---- */
struct surface {
    u32 *px;
    int  w, h;
};

extern struct surface screen;              /* back buffer, full screen */
extern int screen_w, screen_h;

u32 fb_bpp(void);
void fb_init(int width, int height);       /* Linux: size chosen at startup */
void fb_flip(void);                        /* back buffer -> host window */
void fb_scene_clip(int x,int y,int w,int h);
void fb_scene_unclip(void);
void fb_flip_rect(int x, int y, int w, int h);   /* damaged region only */
void fb_clear(u32 color);
int fb_scroll_output(u32 x,u32 y,u32 w,u32 h,u32 step,u32 color);
void fb_scroll_cpu(u32 x,u32 y,u32 w,u32 h,u32 step,u32 color);

/* GPU engine boundary: no GPU driver is ported; every call declines and
 * the CPU compositor path (the SCos default) is always used. */
int gpu_engine_drives_output(void);
int gpu_engine_available(void);
int gpu_engine_fill_rectangle(int x, int y, int width, int height, uint32_t color);
int gpu_engine_screen_to_screen_blit(int dst_x, int dst_y, int width, int height, int src_x, int src_y);
int gpu_engine_wait_idle(void);
void gpu_engine_note_work(uint32_t by_engine, uint32_t by_cpu);
void graphics_report(char *out, size_t capacity);

void s_pixel(struct surface *s, int x, int y, u32 c);
void s_fill(struct surface *s, int x, int y, int w, int h, u32 c);
void s_frame_rect(struct surface *s, int x, int y, int w, int h, u32 c);
void s_line(struct surface *s, int x0, int y0, int x1, int y1, u32 c);
void s_circle(struct surface *s, int cx, int cy, int r, u32 c);
void s_disc(struct surface *s, int cx, int cy, int r, u32 c);
void s_vgrad(struct surface *s, int x, int y, int w, int h, u32 top, u32 bot);

#include "text.h"

extern const u8 font8x16[256][16];

void s_char(struct surface *s, int x, int y, char ch, u32 fg);
void s_char_bg(struct surface *s, int x, int y, char ch, u32 fg, u32 bg);
void s_text(struct surface *s, int x, int y, const char *str, u32 fg);
void s_text_bg(struct surface *s, int x, int y, const char *str, u32 fg, u32 bg);
void s_scos_logo(struct surface *s, int x, int y, u32 color, int scale, int phase);
void s_text_scaled(struct surface *s, int x, int y, const char *str, u32 fg, int scale);
int  s_text_width(const char *str);
void s_clip_text(struct surface *s, int x, int y, const char *str, u32 fg, int max_w);

int  s_text_cols(int width);
int  s_wrap_next(const char *text, int cols, int *rowlen, int *advance);
int  s_wrap_rows(const char *text, int cols);
const char *s_wrap_row(const char *text, int cols, char *out, int out_size);
int  s_text_wrap(struct surface *s, int x, int y, int width, const char *text, u32 fg);
void s_blit(struct surface *d, struct surface *s, int dx, int dy);

int edit_line(char *buf, int *pos, int maxlen, struct key_event *e);
double strtod_simple(const char *s, char **end);
void fmt_double(char *out, double v);
void fmt_u32(char *out, u32 v);
void fmt_u64(char *out, u64 v);
void fmt_i32(char *out, i32 v);
void fmt_pad2(char *out, u32 v);

/* icon drawing (procedural, 24x24 logical box at x,y) */
enum { ICON_FOLDER, ICON_TERMINAL, ICON_NOTEPAD, ICON_BROWSER,
       ICON_CALENDAR, ICON_SETTINGS, ICON_INFO, ICON_CARDS, ICON_CHART, ICON_SOL, ICON_COUNT };
void s_icon(struct surface *s, int id, int x, int y, u32 c);
u32  color_blend(u32 a, u32 b, int t);
int  card_w(void);
int  card_h(void);
void card_draw(struct surface *s, int x, int y, u8 card, int face_down, u32 back_color);
const char *card_rank_str(int rank);

/* ------------------------------------------------------------------ mm ---- */
void  mm_init(void);
void mm_stats(u64 *total_kb, u64 *free_kb);
void mm_ops(u32 *allocs, u32 *frees);
void *palloc(size_t bytes);
#define HEAP_WM 1
#define HEAP_VFS 2
void *palloc_owned(size_t bytes,uintptr_t owner);
uintptr_t heap_set_owner(uintptr_t owner);
u64 heap_owner_bytes(uintptr_t owner);
void  pfree(void *p, size_t bytes);
u64   mm_total_kb(void);
u64   mm_free_kb(void);
size_t allocation_bytes(const void *p);
u64 memory_reserved_pages(void);
uintptr_t pages_allocate(size_t count);
void pages_release(uintptr_t p,size_t count);
u64 memory_free_pages(void);

/* ----------------------------------------------------------------- vfs ---- */
#define VFS_NAME 48

struct vfs_node {
    char name[VFS_NAME];
    int  is_dir;
    char *data;
    u32  size, cap;
    struct vfs_node *child, *sibling, *parent;
};

extern struct vfs_node *vfs_root;

int vfs_init_defaults(void);
struct vfs_node *vfs_lookup(const char *path);
int   vfs_list(struct vfs_node *dir, char names[][VFS_NAME], int max);
char *vfs_read(const char *path, u32 *len);
int   vfs_write(const char *path, const char *data, u32 len);
int   vfs_mkdir(const char *path);
int   vfs_delete(const char *path);
struct vfs_node *vfs_child(struct vfs_node *dir, const char *name);
int   vfs_rename(const char *oldp, const char *newp);
int   vfs_factory_reset(void);
void  system_files_init(int have_disk);
int   vfs_is_dir(struct vfs_node *n);
u32   vfs_usage_bytes(void);
u32   str_to_u32(const char *s);
extern const char *const neofetch_art[];
extern const int neofetch_art_lines, neofetch_art_width;
extern const char *const panic_art[];
extern const int panic_art_lines, panic_art_width;
char *vfs_parent_path(const char *path, char *out);
void  vfs_free_tree(struct vfs_node *n);

/* ------------------------------------------------- persistence (Linux) --- */
/* On bare metal these were the verified-ATA image.  On Linux they are the
 * snapshot file under the XDG data dir; see platform_linux.c + PORTING.md. */
const char *ata_model(void);
int  ata_present(void);
int  fs_image_save(void);
int  fs_image_load(void);
int  fs_image_available(void);
const char *fs_image_target(void);
extern int fs_image_found;

/* ----------------------------------------------------------------- rtc ---- */
struct rtc_time { u16 year; u8 mon, day, hour, min, sec, weekday; };
void rtc_read(struct rtc_time *t);
u32  rtc_to_epoch(const struct rtc_time *t);

/* ---------------------------------------------------------------- acpi ---- */
void acpi_init(void);
int  acpi_shutdown(void);          /* Linux: requests a clean scoswm exit */

/* --------------------------------------------------------------- themes --- */
struct theme {
    const char *id;
    const char *name;
    u32 main;
    u32 bg_top, bg_bot;
    u32 win_bg;
    u32 text;
    u32 title_text;
    u32 taskbar_bg;
};

int theme_custom_count(void);
const struct theme *theme_custom_get(int);
int theme_id_valid(const char *);
int theme_custom_store(const char *,const u32[10],int);
void theme_custom_load(void);
void theme_values(const struct theme *,u32[10]);
int  theme_count(void);
const struct theme *theme_get(int i);
const struct theme *theme_current(void);
void theme_set_index(int i);
int  theme_index_of_id(const char *id);
void theme_load_from_settings(void);

/* -------------------------------------------------------------- windows --- */
struct window;
struct app;

struct app {
    const char *id;
    const char *title;
    int  icon;
    int  single;
    int  def_w, def_h;
    int  min_w, min_h;
    int  uses_data;
    void (*open)(struct window *w, void *arg);
    void (*paint)(struct window *w);
    void (*key)(struct window *w, struct key_event *e);
    void (*mouse)(struct window *w, struct mouse_event *e, int x, int y);
    void (*tick)(struct window *w);
    void (*close)(struct window *w);
    const char *desktop_label;
    int external;
    int file_editor;
    const char *file_suffix;
    void (*document)(struct window *,const char *);
    const char *(*failure)(struct window *w);
    int (*drop)(struct window *w, const char *path, int x, int y);
};

#define WIN_TITLEBAR 26
#define WIN_STATE_NORMAL 0
#define WIN_STATE_MIN 1
#define WIN_STATE_MAX 2

struct window {
    int  id;
    char title[64];
    struct app *app;
    void *data;
    int  x, y, w, h;
    int  px, py, pw, ph;
    int  z;
    int  state;
    struct surface surf;
    int  dirty;
    int  closing;
    int chrome_dirty;
    size_t data_bytes;
    int restore_state;
    void *console;
};

int  wm_win_count(void);
struct window *wm_win_at(int i);
u32  wm_win_cpu_pct(struct window *w);
int  wm_app_running(const char *app_id);
void wm_track_mem(struct window *w, ptrdiff_t delta);
void wm_set_console(struct window *w, void *term);
void wm_set_pending_console(void *term);
void wm_clear_console(void *term);
void app_log(struct window *w, const char *line);
void term_console_line(void *term, const char *line);
void term_console_exit(void *term, const char *app_id);

int  proc_sys_count(void);
const char *proc_sys_name(int i);
u64 proc_win_mem_kb(struct window *w);
u64 proc_wm_mem_kb(void);
u64 proc_kernel_mem_kb(void);

void wm_init(void);
void wm_run(void);
extern int tty_request;
void tty_run(int return_to_wm);
void tty_select(int number);
struct window *wm_open_app(const char *app_id, void *arg);
void wm_close_window(struct window *w);
void wm_set_title(struct window *w, const char *title);
int wm_resize_window(struct window *,int,int);
int wm_move_window(struct window *,int,int);
void wm_redraw(struct window *w);
void wm_request_full(void);
void wm_theme_changed(void);
void wm_focus(struct window *w);
void wm_minimize_window(struct window *w);
struct window *wm_focused(void);
int  wm_content_w(struct window *w);
static inline int mx_abs(struct window *w, int x) { return w->x + 1 + x; }
static inline int my_abs(struct window *w, int y) { return w->y + WIN_TITLEBAR + y; }
int  wm_content_h(struct window *w);

typedef void (*dialog_cb)(int ok, const char *text, void *ud);
void wm_dialog(const char *title, const char *message, const char *input,
               dialog_cb cb, void *ud);
int  wm_dialog_active(void);

typedef void (*menu_cb)(int item, void *ud);
void wm_menu(int x, int y, const char **items, int n, menu_cb cb, void *ud);

void wm_error_popup(const char *text);
void wm_fatal_screen(const char *line1, const char *line2);
void wm_poweroff_screen(void);

/* apps registry */
#define SCOS_APP(symbol, order) \
    static struct app * const scos_register_##symbol \
    __attribute__((used,section(".scos_apps." #order),aligned(8))) = &(symbol)
int app_register(struct app *app);
struct window *app_open_document(const char *path);
struct app *lua_app_install(const char *path);
int  lua_source_check(const char *src, u32 len, char *err, size_t cap);
void lua_apps_refresh(void);
void apps_register_all(void);
struct app *app_find(const char *id);
void wm_desktop_pin_file(const char *path);
int  app_count(void);
struct app *app_at(int i);

/* boot screen */
void boot_screen_step(const char *msg, int progress_pct);
void boot_screen_init(void);

/* persistence hooks */
void settings_save(void);
#define PREFS_MOUSE_DEFAULT 2
#define PREFS_DBL_DEFAULT 500
struct prefs { int mouse_sens; int dbl_ms; };
const struct prefs *prefs_get(void);
void prefs_set_mouse(int sens);
void prefs_set_dbl(int ms);

/* ------------------------------------------------------ drag and drop ---- */
#define DND_APP 0
#define DND_FILE 1
int wm_dnd_begin(int kind, const char *payload, const char *label);
int wm_dnd_active(void);
int wm_dnd_cancel(void);
int wm_dnd_drop(int px, int py);
int wm_dnd_target(int px, int py, char *out, int cap);
int wm_desk_actions(int item, const char **labels, int max);
int wm_desk_invoke(int item, int action);
int wm_desk_app_state(const char *app_id);
int wm_desk_show_app(const char *app_id);
void wm_desktop_restore(void);
void wm_desktop_install(const char *id);
int wm_desk_vis_count(void);
int wm_desk_vis_get(int idx, char *app, char *path, char *label, int *kind);
u32  wm_desk_sel_mask(void);
int  wm_desk_pos(int vis_idx, int *gx, int *gy);
int  wm_desk_action_index(int item, const char *label);
int  wm_app_minimized(const char *app_id);
void wm_wallpaper_invalidate(void);
void system_reset(void);

struct shell_confirm { char command[256]; };
int confirm_answer(const char *s);
int confirm_command(struct shell_confirm *c, char *line, unsigned cap, char *message);
int parse_pid(const char *s, int *pid);
extern int wm_stop_requested;

int app_unregister(struct app *app);
int app_uninstall(const char *id,char *message,size_t capacity);
void app_request_uninstall(const char *id);
void wm_desktop_remove_app(const char *id);
int wm_taskbar_pin(const char *id);
int wm_taskbar_pinned(const char *id);
int wm_taskbar_capacity(void);
void wm_notify(const char *title,const char *text,int warning);

#endif /* SCOS_H */
