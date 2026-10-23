/* SCos logger. Linux: the ring buffer is kept verbatim; the COM1 serial
 * sink becomes stderr, and only when SCOSWM_DEBUG is set. */
#include <stdio.h>
#include <stdlib.h>
#include "scos.h"

static void serial_putc(char c)
{
    static int debug = -1;
    if (debug < 0) debug = getenv("SCOSWM_DEBUG") != NULL;
    if (debug) fputc(c, stderr);
}

static void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

/* on-screen ring so a held diagnostics screen (and `dmesg`) can show what
 * happened on machines without a serial console attached */
#define KLOG_LINES 128   /* ring 24: the boot log with full USB tracing no
                          * longer fits in 40 lines - diagnostics must show
                          * the whole enumeration story in one photo */
#define KLOG_LEN   120   /* room for the full usb status line + counters */
static char ring[KLOG_LINES][KLOG_LEN];
static int ring_head, ring_count;

static void klog_push(const char *line)
{
    strncpy(ring[ring_head], line, KLOG_LEN - 1);
    ring[ring_head][KLOG_LEN - 1] = 0;
    ring_head = (ring_head + 1) % KLOG_LINES;
    if (ring_count < KLOG_LINES) ring_count++;
}

int klog_ring_count(void) { return ring_count; }

int klog_ring(int i, char *out, int max)
{
    if (i < 0 || i >= ring_count || max < 2) return 0;
    int idx = (ring_head - ring_count + i + KLOG_LINES * 2) % KLOG_LINES;
    strncpy(out, ring[idx], max - 1);
    out[max - 1] = 0;
    return 1;
}

void klog_raw(const char *s)
{
    klog_push(s);
    serial_puts(s);
    serial_putc('\r');
    serial_putc('\n');
}

void klog(const char *fmt, ...)
{
    char buf[KLOG_LEN];
    int bp = 0;
    /* minimal varargs via builtin */
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            if (bp < KLOG_LEN - 1) buf[bp++] = *fmt;
            continue;
        }
        fmt++;
        int width = 0;
        if (*fmt == '0') fmt++;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        (void)width;
        switch (*fmt) {
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && bp < KLOG_LEN - 1) buf[bp++] = *s++;
            break;
        }
        case 'c':
            if (bp < KLOG_LEN - 1) buf[bp++] = (char)__builtin_va_arg(ap, int);
            break;
        case 'd': {
            char t[16]; int ti = 0; i32 v = __builtin_va_arg(ap, i32);
            u32 uv; int neg = 0;
            if (v < 0) { neg = 1; uv = 0u-(u32)v; } else uv = (u32)v;
            if (!uv) t[ti++] = '0';
            while (uv) { t[ti++] = (char)('0' + uv % 10); uv /= 10; }
            if (neg && bp < KLOG_LEN - 1) buf[bp++] = '-';
            while (ti-- && bp < KLOG_LEN - 1) buf[bp++] = t[ti];
            break;
        }
        case 'u': case 'x': {
            u32 v = __builtin_va_arg(ap, u32);
            const char *dg = "0123456789abcdef";
            char t[16]; int ti = 0; int base = (*fmt == 'x') ? 16 : 10;
            if (!v) t[ti++] = '0';
            while (v) { t[ti++] = dg[v % base]; v /= base; }
            while (ti-- && bp < KLOG_LEN - 1) buf[bp++] = t[ti];
            break;
        }
        case '%':
            if (bp < KLOG_LEN - 1) buf[bp++] = '%';
            break;
        default:
            if (bp < KLOG_LEN - 1) buf[bp++] = '%';
            if (bp < KLOG_LEN - 1) buf[bp++] = *fmt;
            break;
        }
    }
    __builtin_va_end(ap);
    buf[bp] = 0;
    klog_push(buf);
    serial_puts(buf);
    serial_putc('\r');
    serial_putc('\n');
}
