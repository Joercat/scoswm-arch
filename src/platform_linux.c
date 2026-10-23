/* Linux platform layer for the SCos window-manager port.
 *
 * Everything the bare-metal desktop expected from the kernel is provided
 * here in host terms:
 *   PIT 100 Hz tick        -> CLOCK_MONOTONIC, 1 tick = 10 ms
 *   CMOS RTC               -> localtime()
 *   page allocator         -> mmap()/munmap() (heap.c sits on top)
 *   PS/2 + USB HID queues  -> fixed ring buffers fed by the backend
 *   verified ATA fs image  -> snapshot file in the XDG data directory
 *   ACPI / 8042 reboot     -> a clean request to exit scoswm
 *   GPU engine             -> none; all calls decline, CPU compositor runs
 *   Lua/CAT apps, TTYs     -> explicit stubs (see PORTING.md)
 */
#include "scos.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>

/* ------------------------------------------------------------ tick/clock */
volatile u64 timer_ticks;
u64 cpu_tsc_hz = 1000000000ULL;          /* rdtsc() counts nanoseconds here */
int input_guard_armed;
volatile u32 input_last_tick;
u32 input_key_enqueued, input_mouse_enqueued, input_key_dropped, input_mouse_dropped;
int tty_request;
int fs_image_found;

static u64 start_ms;
static int quit_requested;

static u64 now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000ULL + (u64)ts.tv_nsec / 1000000ULL;
}

void platform_tick_update(void)
{
    if (!start_ms) start_ms = now_ms();
    timer_ticks = (now_ms() - start_ms) / 10;
}

u32 uptime_ms(void) { platform_tick_update(); return (u32)(timer_ticks * 10); }

void sleep_ms(u32 ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
    platform_tick_update();
}

void cpu_hlt(void) { platform_wait_idle(); }

u64 rdtsc(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

u64 cpu_busy_cycles(void) { return rdtsc(); }
void cpu_meter_init(void) { platform_tick_update(); }
void cpu_meter_tick(void) { }
void cpu_idle_begin(void) { }

u32 cpu_mhz(void)
{
    /* Host-reported frequency (for display); timing itself is ns-based. */
    static u32 cached;
    if (cached) return cached;
    cached = 1000;
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            double mhz = 0;
            if (sscanf(line, "cpu MHz : %lf", &mhz) == 1 && mhz > 0) {
                cached = (u32)mhz; break;
            }
        }
        fclose(f);
    }
    return cached;
}

u32 cpu_core_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (u32)n : 1;
}
u32 cpu_thread_count(void) { return cpu_core_count(); }

u32 cpu_usage_pct(void)
{
    /* This process's CPU time over wall time since the previous call. */
    static u64 last_cpu_us, last_wall_us;
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    u64 cpu = (u64)ru.ru_utime.tv_sec * 1000000ULL + (u64)ru.ru_utime.tv_usec
            + (u64)ru.ru_stime.tv_sec * 1000000ULL + (u64)ru.ru_stime.tv_usec;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    u64 wall = (u64)ts.tv_sec * 1000000ULL + (u64)ts.tv_nsec / 1000ULL;
    u32 pct = 0;
    if (last_wall_us && wall > last_wall_us)
        pct = (u32)((cpu - last_cpu_us) * 100 / (wall - last_wall_us));
    last_cpu_us = cpu; last_wall_us = wall;
    return pct > 100 ? 100 : pct;
}

/* ------------------------------------------------------------------ exit */
void platform_request_quit(void) { quit_requested = 1; }
int platform_quit_requested(void) { return quit_requested; }
void cpu_reboot_8042(void) { platform_request_quit(); }
void acpi_init(void) { }
int acpi_shutdown(void) { platform_request_quit(); return 1; }

_Noreturn void panic(const char *reason)
{
    fprintf(stderr, "scoswm: panic: %s\n", reason ? reason : "(null)");
    abort();
}
void kernel_panic(const char *reason) { panic(reason); }

/* ------------------------------------------------------------- page heap */
uintptr_t pages_allocate(size_t count)
{
    void *p = mmap(NULL, count * 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : (uintptr_t)p;
}
void pages_release(uintptr_t p, size_t count)
{
    if (p) munmap((void *)p, count * 4096);
}
u64 memory_free_pages(void)
{
    struct sysinfo si;
    if (sysinfo(&si) != 0) return 0;
    return (u64)si.freeram * si.mem_unit / 4096;
}
u64 memory_reserved_pages(void) { return 0; }

/* memcpy/memset: the desktop's own libc (lib.c) provides the rest, but the
 * bare-metal memcpy/memset lived in the kernel memory code, so they are
 * provided here.  They are exported (as in the original), which means SDL
 * and libc calls in this process also resolve to them; both are plain
 * correct implementations. */
void *memcpy(void *d, const void *s, size_t n)
{
    u8 *dd = d; const u8 *ss = s;
    while (n && ((uintptr_t)dd & 7)) { *dd++ = *ss++; n--; }
    u64 *dw = (u64 *)dd; const u64 *sw = (const u64 *)ss;
    while (n >= 8) { *dw++ = *sw++; n -= 8; }
    dd = (u8 *)dw; ss = (const u8 *)sw;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memset(void *d, int c, size_t n)
{
    u8 *dd = d;
    u64 w = (u8)c; w |= w << 8; w |= w << 16; w |= w << 32;
    while (n && ((uintptr_t)dd & 7)) { *dd++ = (u8)c; n--; }
    u64 *dw = (u64 *)dd;
    while (n >= 8) { *dw++ = w; n -= 8; }
    dd = (u8 *)dw;
    while (n--) *dd++ = (u8)c;
    return d;
}

/* ------------------------------------------------------------------ input */
#define KQ 512
#define MQ 512
static struct key_event kqueue[KQ];
static struct mouse_event mqueue[MQ];
static int khead, ktail, mhead, mtail;

void scos_input_push_key(const struct key_event *e)
{
    if (input_guard_armed) input_last_tick = (u32)timer_ticks;
    int next = (khead + 1) % KQ;
    if (next == ktail) { input_key_dropped++; return; }
    input_key_enqueued++;
    kqueue[khead] = *e; khead = next;
}
void scos_input_push_mouse(const struct mouse_event *e)
{
    if (input_guard_armed) input_last_tick = (u32)timer_ticks;
    int next = (mhead + 1) % MQ;
    if (next == mtail) { input_mouse_dropped++; return; }
    input_mouse_enqueued++;
    mqueue[mhead] = *e; mhead = next;
}
int kbd_poll(struct key_event *out)
{
    if (ktail == khead) return 0;
    *out = kqueue[ktail]; ktail = (ktail + 1) % KQ;
    return 1;
}
int mouse_poll(struct mouse_event *out)
{
    if (mtail == mhead) return 0;
    *out = mqueue[mtail]; mtail = (mtail + 1) % MQ;
    return 1;
}
int mouse_present(void) { return 1; }

/* ------------------------------------------------------- no-op services */
void usb_init(void) { }
void usb_poll(void) { platform_pump(); }
void usb_poll_events(void) { platform_pump(); }
void usb_status(char *out, int max) { snprintf(out, max, "input: host keyboard/mouse via SDL2"); }
void usb_kbd_leds_off(void) { }
u64 usb_dma_bytes(void) { return 0; }
void input_status(char *out, int max) { usb_status(out, max); }
void tty_run(int return_to_wm) { (void)return_to_wm; tty_request = 0; }
void tty_select(int number) { (void)number; }

/* GPU: nothing is bound, every engine call declines, the CPU paints. */
int gpu_engine_drives_output(void) { return 0; }
int gpu_engine_available(void) { return 0; }
int gpu_engine_fill_rectangle(int x, int y, int w, int h, uint32_t c)
{ (void)x; (void)y; (void)w; (void)h; (void)c; return -1; }
int gpu_engine_screen_to_screen_blit(int a, int b, int w, int h, int c, int d)
{ (void)a; (void)b; (void)w; (void)h; (void)c; (void)d; return -1; }
int gpu_engine_wait_idle(void) { return -1; }
void gpu_engine_note_work(uint32_t by_engine, uint32_t by_cpu)
{ (void)by_engine; (void)by_cpu; }

void graphics_report(char *out, size_t capacity)
{
    if (!out || !capacity) return;
    snprintf(out, capacity,
        "Renderer: CPU software compositor (SCos back buffer)\n"
        "Scanout: host window via SDL2 texture, %dx%dx32\n"
        "GPU acceleration: none - no GPU engine is ported to Linux; "
        "the CPU paints every pixel, exactly as on SCos hardware without "
        "a bound engine module\n",
        screen_w, screen_h);
}

/* Lua/CAT apps: not ported yet.  The registry calls these, so say so
 * honestly instead of failing silently. */
struct app *lua_app_install(const char *path) { (void)path; return NULL; }
void lua_apps_refresh(void) { }
int lua_source_check(const char *src, u32 len, char *err, size_t cap)
{
    (void)src; (void)len;
    if (err && cap) snprintf(err, cap, "Lua/CAT apps are not ported to Linux yet (see PORTING.md)");
    return 0;
}

/* ------------------------------------------------------------------- RTC */
void rtc_read(struct rtc_time *t)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    t->year = (u16)(tmv.tm_year + 1900);
    t->mon = (u8)(tmv.tm_mon + 1);
    t->day = (u8)tmv.tm_mday;
    t->hour = (u8)tmv.tm_hour;
    t->min = (u8)tmv.tm_min;
    t->sec = (u8)tmv.tm_sec;
    t->weekday = (u8)tmv.tm_wday;
}
u32 rtc_to_epoch(const struct rtc_time *t)
{
    int y = t->year - (t->mon <= 2 ? 1 : 0);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned mp = (t->mon + 9) % 12;
    unsigned doy = (153 * mp + 2) / 5 + t->day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097L + (long)doe - 719468L;
    return (u32)(days * 86400L + t->hour * 3600L + t->min * 60L + t->sec);
}

/* ------------------------------------------- snapshot persistence (VFS) */
static char data_dir[512];
static char snapshot_path[560];

static void mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *q = tmp + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(tmp, 0700); *q = '/'; }
    mkdir(tmp, 0700);
}

void platform_data_dir_init(const char *override)
{
    if (override && override[0]) {
        snprintf(data_dir, sizeof(data_dir), "%s", override);
    } else {
        const char *xdg = getenv("XDG_DATA_HOME");
        const char *home = getenv("HOME");
        if (xdg && xdg[0]) snprintf(data_dir, sizeof(data_dir), "%s/scoswm", xdg);
        else if (home && home[0]) snprintf(data_dir, sizeof(data_dir), "%s/.local/share/scoswm", home);
        else snprintf(data_dir, sizeof(data_dir), ".scoswm");
    }
    mkdir_p(data_dir);
    snprintf(snapshot_path, sizeof(snapshot_path), "%s/vfs.snapshot", data_dir);
}

const char *ata_model(void) { return NULL; }
int ata_present(void) { return 0; }
const char *fs_image_target(void) { return snapshot_path; }
int fs_image_available(void)
{
    if (!snapshot_path[0]) return 0;
    return access(data_dir, W_OK) == 0;
}

/* Snapshot format: "SCOSLNX1\n" then per node "D <path>\n" or
 * "F <path> <len>\n" followed by exactly <len> raw bytes. */
static FILE *snap;
static int snap_fail;
static void save_node(struct vfs_node *n, const char *path)
{
    if (snap_fail) return;
    if (n != vfs_root) {
        if (n->is_dir) {
            if (fprintf(snap, "D %s\n", path) < 0) { snap_fail = 1; return; }
        } else {
            if (fprintf(snap, "F %s %u\n", path, n->size) < 0) { snap_fail = 1; return; }
            if (n->size && fwrite(n->data, 1, n->size, snap) != n->size) { snap_fail = 1; return; }
        }
    }
    for (struct vfs_node *c = n->child; c; c = c->sibling) {
        char child[256];
        snprintf(child, sizeof(child), "%s%s%s", path, path[0] ? "/" : "", c->name);
        save_node(c, child);
    }
}

int fs_image_save(void)
{
    if (!fs_image_available()) return 0;
    char tmp[580];
    snprintf(tmp, sizeof(tmp), "%s.tmp", snapshot_path);
    snap = fopen(tmp, "wb");
    if (!snap) return 0;
    snap_fail = 0;
    if (fprintf(snap, "SCOSLNX1\n") < 0) snap_fail = 1;
    save_node(vfs_root, "");
    if (fclose(snap) != 0) snap_fail = 1;
    snap = NULL;
    if (snap_fail) { remove(tmp); return 0; }
    if (rename(tmp, snapshot_path) != 0) { remove(tmp); return 0; }
    return 1;
}

static void vfs_mkdirs_for(const char *path, int is_file)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    /* create every prefix directory ("a/b/c" -> a, a/b [, a/b/c]) */
    for (char *q = tmp + 1; ; q++) {
        if (*q == '/' || (*q == 0 && !is_file)) {
            char save = *q; *q = 0;
            if (!vfs_lookup(tmp)) vfs_mkdir(tmp);
            *q = save;
            if (!save) break;
        } else if (!*q) break;
    }
}

int fs_image_load(void)
{
    fs_image_found = 0;
    FILE *f = fopen(snapshot_path, "rb");
    if (!f) return 0;
    char magic[16];
    if (!fgets(magic, sizeof(magic), f) || strcmp(magic, "SCOSLNX1\n") != 0) {
        fclose(f); return 0;
    }
    char line[320];
    int ok = 1;
    while (fgets(line, sizeof(line), f)) {
        size_t ln = strlen(line);
        while (ln && (line[ln - 1] == '\n' || line[ln - 1] == '\r')) line[--ln] = 0;
        if (line[0] == 'D' && line[1] == ' ') {
            vfs_mkdirs_for(line + 2, 0);
        } else if (line[0] == 'F' && line[1] == ' ') {
            /* the length follows the LAST space (paths may contain spaces) */
            char *last = line + 2;
            for (char *q = line + 2; *q; q++) if (*q == ' ') last = q;
            if (last == line + 2) { ok = 0; break; }
            *last = 0;
            long len = strtol(last + 1, NULL, 10);
            if (len < 0 || len > (64 << 20)) { ok = 0; break; }
            char *buf = malloc((size_t)len + 1);
            if (!buf) { ok = 0; break; }
            if (len && fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); ok = 0; break; }
            vfs_mkdirs_for(line + 2, 1);
            vfs_write(line + 2, buf, (u32)len);
            free(buf);
        } else if (ln == 0) {
            continue;
        } else { ok = 0; break; }
    }
    fclose(f);
    fs_image_found = ok;
    return ok;
}

int app_uninstall(const char *id, char *message, size_t capacity)
{
    (void)id;
    if (message && capacity)
        snprintf(message, capacity,
                 "External (Lua/CAT) apps are not ported to Linux yet; "
                 "there is nothing to uninstall.");
    return 0;
}
