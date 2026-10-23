/*
 * SCos native - non-fatal error screen.
 *
 * Two tiers of failure reporting:
 *   fatal    -> kernel_panic_info() in panic.c: register + stack dump, halt
 *   non-fatal-> here: a held screen with the subsystem name, the message,
 *               caller-supplied related state (register/IO/dump lines) and
 *               the recent kernel log; the OS continues afterwards.
 *
 * The hold loop deliberately avoids sleep_ms(): a subsystem that froze the
 * PIT would otherwise freeze the error screen too. Timing comes from the
 * RTC (CMOS reads need no interrupts) and the loop self-heals a stuck PIT
 * by re-arming the PIC, mirroring the WM irq watchdog.
 */
#include "scos.h"

static const char *warn_art[] = {
    "             ________             ",
    "            /        \\            ",
    "           /          \\           ",
    "          /    !!!!    \\          ",
    "         /     !!!!     \\         ",
    "        /                \\        ",
    "       /__________________\\       ",
};

static u32 last_show_tick;
static int have_shown, shown_count;

/* pending error: err_notify stores it, the WM loop presents it at a safe
 * point so an error raised inside an atomic flow (dialog callback, disk
 * write, reboot path) can never block that flow */
static char pend_subsys[32], pend_msg[96];
static char pend_dump[4][72];
static int pend_ndump, pend_active;

int err_pending(void) { return pend_active; }

void err_show_pending(void)
{
    if (!pend_active) return;
    pend_active = 0;
    const char *dp[4];
    for (int i = 0; i < pend_ndump; i++) dp[i] = pend_dump[i];
    error_screen(pend_subsys, pend_msg, pend_ndump ? dp : 0, pend_ndump);
}

static u32 rtc_sec_now(void)
{
    struct rtc_time rt;
    rtc_read(&rt);
    return (u32)rt.sec + rt.min * 60u;
}

void error_screen(const char *subsys, const char *msg,
                  const char *const *dump, int ndump)
{
    const struct theme *t = theme_current();
    fb_clear(0x000000);
    int y = 36;
    for (unsigned i = 0; i < sizeof(warn_art) / sizeof(warn_art[0]); i++) {
        s_text(&screen, 60, y, warn_art[i], 0xFFCC33);
        y += 18;
    }
    s_text_scaled(&screen, 420, 48, "SYSTEM", 0xFFCC33, 3);
    s_text_scaled(&screen, 420, 96, "ERROR", 0xFFCC33, 3);
    char head[96];
    strcpy(head, "subsystem: ");
    strncat(head, subsys ? subsys : "?", 40);
    s_text(&screen, 420, 150, head, t->text);

    y = 200;
    /* Wrapped, not run off the edge: a panic that says where it stopped is the entire reason this screen
       exists, and the message used to be drawn with no bound at all - past a screen width it left the
       display and was simply gone. */
    y += s_text_wrap(&screen, 60, y, screen_w - 120, msg ? msg : "unspecified error", 0xFFFFFF) * FONT_H;
    y += 14;
    if (dump && ndump > 0) {
        s_text(&screen, 60, y, "related state:", 0xFFCC33);
        y += 20;
        /* The state lines wrap for the same reason: they carry addresses, and an address cut in half is
           worse than no address at all. */
        for (int i = 0; i < ndump && y < screen_h - 220; i++) {
            y += s_text_wrap(&screen, 60, y, screen_w - 120, dump[i], 0xCCCCCC) * FONT_H + 2;
        }
        y += 12;
    }
    s_text(&screen, 60, y, "recent kernel log:", 0xFFCC33);
    y += 20;
    int n = klog_ring_count();
    int start = n > 8 ? n - 8 : 0;
    for (int i = start; i < n && y < screen_h - 60; i++) {
        char ln[96];
        if (klog_ring(i, ln, sizeof(ln))) {
            s_text(&screen, 60, y, ln, 0x999999);
            y += 18;
        }
    }
    s_text(&screen, 60, screen_h - 30,
           "non-fatal: the OS continues. any key or click dismisses - auto in 20 s",
           0xFFCC33);
    fb_flip();

    /* hold: RTC-timed, interrupt-independent, self-healing */
    u32 t0 = rtc_sec_now(), prev = t0;
    int elapsed = 0, stuck = 0;
    u32 last_tick = tick_count;
    for (;;) {
        platform_pump(); platform_wait_idle(); /* host input, not hotplug probes */
        struct key_event ke;
        struct mouse_event me;
        if (kbd_poll(&ke)) break;
        if (mouse_poll(&me)) break;
        if (tick_count != last_tick) { last_tick = tick_count; stuck = 0; }
        else if (++stuck > 2000000) stuck = 0;  /* bare metal re-armed the PIC here */
        u32 s = rtc_sec_now();
        if (s != prev) {
            prev = s;
            if (++elapsed >= 20) break;
        }
    }
    wm_request_full();      /* dismissed: WM must repaint over this screen */
}

void err_notify(const char *subsys, const char *msg,
                const char *const *dump, int ndump)
{
    char line[128];
    strcpy(line, "err: ");
    strncat(line, subsys ? subsys : "?", 24);
    strcat(line, ": ");
    strncat(line, msg ? msg : "?", 88);
    klog("%s", line);
    if (!input_guard_armed) return;      /* pre-WM: log only, never hold boot */
    if (shown_count >= 5 || pend_active) return;
    if (have_shown && tick_count - last_show_tick < 1000)
        return;                                      /* 10 s cooldown */
    last_show_tick = tick_count;
    have_shown = 1;
    shown_count++;
    strncpy(pend_subsys, subsys ? subsys : "?", sizeof(pend_subsys) - 1);
    strncpy(pend_msg, msg ? msg : "?", sizeof(pend_msg) - 1);
    pend_ndump = 0;
    if (dump)
        for (int i = 0; i < ndump && i < 4; i++) {
            strncpy(pend_dump[i], dump[i] ? dump[i] : "", sizeof(pend_dump[i]) - 1);
            pend_dump[i][sizeof(pend_dump[i]) - 1] = 0;
            pend_ndump++;
        }
    pend_active = 1;
}
