/* SCos native - minimal libc (freestanding) */
#include "scos.h"





int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (u8)*a - (u8)*b : 0;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) ;
    return r;
}

char *strcat(char *d, const char *s)
{
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++)) ;
    return r;
}

char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

/* ---------------------------------------------------------- editing ------ */
int edit_line(char *buf, int *pos, int maxlen, struct key_event *e)
{
    if (!e->pressed) return 0;
    int len = (int)strlen(buf);
    if (e->keycode == '\b' || e->keycode == 127) {
        if (*pos > 0) {
            for (int i = *pos; i <= len; i++) buf[i - 1] = buf[i];
            (*pos)--;
            return 1;
        }
        return 0;
    }
    if (e->keycode == KEY_LEFT)  { if (*pos > 0) (*pos)--; return 0; }
    if (e->keycode == KEY_RIGHT) { if (*pos < len) (*pos)++; return 0; }
    if (e->keycode == KEY_HOME)  { *pos = 0; return 0; }
    if (e->keycode == KEY_END)   { *pos = len; return 0; }
    if (e->keycode >= 32 && e->keycode < 127 && len < maxlen - 1) {
        for (int i = len; i >= *pos; i--) buf[i + 1] = buf[i];
        buf[*pos] = (char)e->keycode;
        (*pos)++;
        return 1;
    }
    return 0;
}

void fmt_u32(char *out, u32 v)
{
    char tmp[12];
    int i = 0;
    if (!v) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0;
    while (i--) out[j++] = tmp[i];
    out[j] = 0;
}

void fmt_i32(char *out, i32 v)
{
    u32 magnitude=(u32)v;
    if (v < 0) { *out++ = '-'; magnitude=0u-magnitude; }
    fmt_u32(out, magnitude);
}

void fmt_pad2(char *out, u32 v)
{
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
    out[2] = 0;
}

char *str_str(const char *hay, const char *needle)
{
    int n = (int)strlen(needle);
    if (!n) return (char *)hay;
    for (const char *p = hay; *p; p++)
        if (!strncmp(p, needle, n)) return (char *)p;
    return NULL;
}

double strtod_simple(const char *s, char **end)
{
    double sign = 1, val = 0;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') { val += (*s - '0') * frac; frac *= 0.1; s++; }
    }
    if (end) *end = (char *)s;
    return sign * val;
}

void fmt_double(char *out, double v)
{
    if(!(v<=999999999999999.0&&v>=-999999999999999.0)){strcpy(out,"out of range");return;}
    if (v < 0) { *out++ = '-'; v = -v; }
    u64 ip=(u64)v;
    u32 fr=(u32)((v-(double)ip)*1000000.0+0.5);
    if(fr==1000000){ip++;fr=0;}
    fmt_u64(out,ip);out+=strlen(out);
    if (fr) {
        *out++ = '.';
        char digits[7];
        for (int i = 5; i >= 0; i--) { digits[i] = (char)('0' + fr % 10); fr /= 10; }
        int last = 5;
        while (last > 0 && digits[last] == '0') last--;
        for (int i = 0; i <= last; i++) *out++ = digits[i];
    }
    *out = 0;
}

#if defined(__x86_64__) || defined(__i386__)
u32 cpu_signature(void)
{
    u32 ax, bx, cx, dx;
    __asm__ volatile("movl $1, %%eax; cpuid"
                     : "=a"(ax), "=b"(bx), "=c"(cx), "=d"(dx));
    return ax;
}

#else
u32 cpu_signature(void) { return 0; }
#endif

char *str_chr(const char *s, char c)
{
    while (*s && *s != c) s++;
    return *s == c ? (char *)s : 0;
}

#if defined(__x86_64__) || defined(__i386__)
void cpu_brand(char *out, int max)
{
    if (!out || max <= 0) return;
    u32 ax, bx, cx, dx;
    out[0] = 0;
    __asm__ volatile("movl $0x80000000, %%eax; cpuid"
                     : "=a"(ax), "=b"(bx), "=c"(cx), "=d"(dx));
    if (ax < 0x80000004) {
        /* no brand string leaf: fall back to vendor + family/model */
        __asm__ volatile("movl $0, %%eax; cpuid"
                         : "=a"(ax), "=b"(bx), "=c"(cx), "=d"(dx));
        char vend[13];
        *(u32 *)(vend + 0) = bx; *(u32 *)(vend + 4) = dx; *(u32 *)(vend + 8) = cx;
        vend[12] = 0;
        u32 fam, mod, f1;
        __asm__ volatile("movl $1, %%eax; cpuid"
                         : "=a"(f1), "=b"(bx), "=c"(cx), "=d"(dx));
        fam = ((f1 >> 8) & 0xF);
        if (fam == 0xF) fam += (f1 >> 20) & 0xFF;
        mod = ((f1 >> 4) & 0xF);
        if (fam == 0x6 || fam == 0xF) mod += ((f1 >> 16) & 0xF) << 4;
        strncpy(out, vend, max - 1);
        out[max - 1] = 0;
        char tail[40];
        strcpy(tail, " family ");
        char n[8];
        fmt_u32(n, fam); strcat(tail, n);
        strcat(tail, " model "); fmt_u32(n, mod); strcat(tail, n);
        strncat(out, tail, max - strlen(out) - 1);
        return;
    }
    /* r39: collect into a local 48-byte block first, then copy out
     * bounded by max - the old code wrote out[48] and u32-aliased the
     * caller buffer regardless of max, and unaligned u32 stores through
     * a char * invited aliasing trouble. Also trims BOTH ends: brand
     * strings are space-padded inside the 48 bytes. */
    u32 words[12];
    for (u32 leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
        __asm__ volatile("cpuid"
                         : "=a"(ax), "=b"(bx), "=c"(cx), "=d"(dx)
                         : "a"(leaf));
        int idx = (int)(leaf - 0x80000002) * 4;
        words[idx] = ax; words[idx + 1] = bx;
        words[idx + 2] = cx; words[idx + 3] = dx;
    }
    char tmp[49];
    memcpy(tmp, words, 48);
    tmp[48] = 0;
    const char *b = tmp;
    while (*b == ' ') b++;
    int len = (int)strlen(b);
    while (len > 0 && b[len - 1] == ' ') len--;
    if (len > max - 1) len = max - 1;
    memcpy(out, b, (u32)len);
    out[len] = 0;
}

#else
void cpu_brand(char *out, int max)
{
    if (!out || max <= 0) return;
    strncpy(out, "host CPU (non-x86 build)", max - 1);
    out[max - 1] = 0;
}
#endif

u32 str_to_u32(const char *s)
{
    u32 v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (u32)(*s - '0'); s++; }
    return v;
}

char *strncat(char *d, const char *s, size_t n)
{
    char *o = d;
    while (*d) d++;
    while (n-- && *s) *d++ = *s++;
    *d = 0;
    return o;
}

char *strstr(const char *h, const char *n)
{
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return NULL;
}
void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *sr = src;
    if (d == sr) return dst;
    if (d < sr) {
        for (size_t i = 0; i < n; i++) d[i] = sr[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = sr[i - 1];
    }
    return dst;
}

void fmt_u64(char *out,u64 v){char reverse[20];size_t n=0;do{reverse[n++]=(char)('0'+v%10);v/=10;}while(v);while(n)*out++=reverse[--n];*out=0;}
