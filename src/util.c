#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

const struct scos_theme scos_themes[] = {
    { "matrix-1",        "Matrix",     0x39FF14, 0x031403, 0x000800, 0x1a1a1a, 0x39FF14, 0x001500, 0x141414 },
    { "blue-sky",        "Blue Sky",   0x00AAFF, 0x001933, 0x000a14, 0x002B4D, 0x00AAFF, 0x001020, 0x0d1b2a },
    { "midnight-purple", "Purple",     0xCC66FF, 0x1A0033, 0x0a0014, 0x330066, 0xCC66FF, 0x140028, 0x1c1024 },
    { "amber-tech",      "Matrix red", 0xFF0000, 0x1a0000, 0x080000, 0x4D0000, 0xFF0000, 0x1a0000, 0x201010 },
};
const int scos_theme_count = 4;

const struct scos_theme *theme_by_id(const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < scos_theme_count; i++)
        if (!strcmp(scos_themes[i].id, id)) return &scos_themes[i];
    return NULL;
}
const struct scos_theme *theme_by_index(int i)
{
    return i >= 0 && i < scos_theme_count ? &scos_themes[i] : NULL;
}

unsigned int color_blend_rgb(unsigned int a, unsigned int b, int t)
{
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    unsigned int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    unsigned int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    return (((ar * (255 - t) + br * t) / 255) << 16) |
           (((ag * (255 - t) + bg * t) / 255) << 8) |
           ((ab * (255 - t) + bb * t) / 255);
}

int parse_hex_color(const char *s, unsigned int *out)
{
    if (!s || !out) return 0;
    if (*s == '#') s++;
    if (strlen(s) != 6) return 0;
    unsigned int v = 0;
    for (int i = 0; i < 6; i++) {
        char c = s[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = v * 16 + (unsigned int)d;
    }
    *out = v;
    return 1;
}

/* ------------------------------------------------------------------ config */
void config_defaults(struct scos_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->theme, "matrix-1");
    strcpy(cfg->pins, "terminal,files,sysmon,settings");
    cfg->bar_height = 40;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static void set_str(char *dst, int cap, const char *v)
{
    snprintf(dst, cap, "%s", v);
}

int config_load_file(const char *path, struct scos_config *cfg)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    static const char *const color_keys[7] = {
        "main", "bg_top", "bg_bot", "win_bg", "text", "title_text", "taskbar_bg"
    };
    char line[1024];
    int applied = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        int done = 1;
        if (!strcmp(key, "theme")) set_str(cfg->theme, sizeof(cfg->theme), val);
        else if (!strcmp(key, "terminal")) set_str(cfg->terminal, sizeof(cfg->terminal), val);
        else if (!strcmp(key, "files")) set_str(cfg->files, sizeof(cfg->files), val);
        else if (!strcmp(key, "font")) set_str(cfg->font, sizeof(cfg->font), val);
        else if (!strcmp(key, "pins")) set_str(cfg->pins, sizeof(cfg->pins), val);
        else if (!strcmp(key, "bar_height")) {
            int v = atoi(val);
            if (v >= 28 && v <= 96) cfg->bar_height = v; else done = 0;
        } else {
            done = 0;
            for (int i = 0; i < 7; i++)
                if (!strcmp(key, color_keys[i])) {
                    unsigned int c;
                    if (parse_hex_color(val, &c)) { cfg->custom[i] = c; cfg->has_custom = 1; done = 1; }
                    break;
                }
            if (cfg->has_custom && !strcmp(cfg->theme, "matrix-1")) { /* custom wins once named */ }
        }
        if (done) applied++;
    }
    fclose(f);
    if (cfg->has_custom) strcpy(cfg->theme, "custom");
    return applied;
}

int config_save_file(const char *path, const struct scos_config *cfg)
{
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "# scoswm configuration\n");
    fprintf(f, "theme = %s\n", cfg->theme[0] ? cfg->theme : "matrix-1");
    if (cfg->has_custom) {
        static const char *const names[7] = {
            "main", "bg_top", "bg_bot", "win_bg", "text", "title_text", "taskbar_bg"
        };
        for (int i = 0; i < 7; i++)
            if (cfg->custom[i]) fprintf(f, "%s = #%06X\n", names[i], cfg->custom[i]);
    }
    fprintf(f, "terminal = %s\n", cfg->terminal);
    fprintf(f, "files = %s\n", cfg->files);
    fprintf(f, "font = %s\n", cfg->font);
    fprintf(f, "pins = %s\n", cfg->pins);
    fprintf(f, "bar_height = %d\n", cfg->bar_height);
    int ok = fclose(f) == 0;
    return ok;
}

void theme_apply_custom(struct scos_theme *out, const struct scos_config *cfg)
{
    /* Unset custom fields inherit the Matrix base, as SCos custom
     * palettes started from editable copies of a preset. */
    *out = scos_themes[0];
    out->id = "custom"; out->name = "Custom";
    if (!cfg->has_custom) return;
    const unsigned int defs[7] = { out->main, out->bg_top, out->bg_bot, out->win_bg,
                                   out->text, out->title_text, out->taskbar_bg };
    unsigned int v[7];
    for (int i = 0; i < 7; i++) v[i] = cfg->custom[i] ? cfg->custom[i] : defs[i];
    out->main = v[0]; out->bg_top = v[1]; out->bg_bot = v[2]; out->win_bg = v[3];
    out->text = v[4]; out->title_text = v[5]; out->taskbar_bg = v[6];
}

/* -------------------------------------------------------------- .desktop */
int desktop_app_cmp(const void *a, const void *b)
{
    return strcasecmp(((const struct desktop_app *)a)->name,
                      ((const struct desktop_app *)b)->name);
}

static void unquote(char *s)
{
    /* .desktop Exec/Name are unquoted strings; only strip outer spaces */
    char *t = trim(s);
    if (t != s) memmove(s, t, strlen(t) + 1);
}

int desktop_parse_file(const char *path, struct desktop_app *app)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    memset(app, 0, sizeof(*app));
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(app->id, sizeof(app->id), "%s", base);
    char *dot = strstr(app->id, ".desktop");
    if (dot) *dot = 0;

    char line[1024];
    int in_entry = 0, have_exec = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') { in_entry = !strncmp(s, "[Desktop Entry]", 15); continue; }
        if (!in_entry) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        /* Only the untranslated keys; Name[xx] etc. fall through. */
        if (!strcmp(key, "Name")) { if (!app->name[0]) snprintf(app->name, sizeof(app->name), "%s", val); }
        else if (!strcmp(key, "Exec")) { snprintf(app->exec, sizeof(app->exec), "%s", val); have_exec = 1; }
        else if (!strcmp(key, "Icon")) snprintf(app->icon, sizeof(app->icon), "%s", val);
        else if (!strcmp(key, "NoDisplay")) app->nodisplay = !strcasecmp(val, "true");
        else if (!strcmp(key, "Terminal")) app->is_terminal = !strcasecmp(val, "true");
        else if (!strcmp(key, "Type")) { if (strcasecmp(val, "Application")) { fclose(f); return 0; } }
    }
    fclose(f);
    unquote(app->name); unquote(app->exec);
    return have_exec && app->name[0] && !app->nodisplay;
}

int desktop_scan(const char *const *dirs, int ndirs, struct desktop_app *out, int max)
{
    int n = 0;
    for (int d = 0; d < ndirs; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *e;
        while ((e = readdir(dir)) != NULL && n < max) {
            const char *name = e->d_name;
            size_t len = strlen(name);
            if (len < 9 || strcmp(name + len - 8, ".desktop")) continue;
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", dirs[d], name);
            struct desktop_app app;
            if (!desktop_parse_file(path, &app)) continue;
            int dup = 0;
            for (int i = 0; i < n; i++) if (!strcmp(out[i].id, app.id)) { dup = 1; break; }
            if (!dup) out[n++] = app;
        }
        closedir(dir);
    }
    qsort(out, n, sizeof(*out), desktop_app_cmp);
    return n;
}

void exec_strip_field_codes(const char *in, char *out, int cap)
{
    int o = 0;
    for (int i = 0; in[i] && o + 1 < cap; i++) {
        if (in[i] == '%' && in[i + 1]) {
            if (in[i + 1] == '%') { out[o++] = '%'; i++; }
            else i++;                     /* drop %f, %u, ... entirely */
            continue;
        }
        out[o++] = in[i];
    }
    out[o] = 0;
    /* a stripped code often leaves a double space behind */
    for (int i = 0; out[i]; i++)
        if (out[i] == ' ' && out[i + 1] == ' ') { memmove(out + i, out + i + 1, strlen(out + i)); i--; }
    char *t = trim(out);
    if (t != out) memmove(out, t, strlen(t) + 1);
}

int ci_contains(const char *hay, const char *needle)
{
    if (!*needle) return 1;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

void utf8_to_latin1(const char *in, char *out, int cap)
{
    int o = 0;
    const unsigned char *s = (const unsigned char *)in;
    while (*s && o + 1 < cap) {
        unsigned int cp; int len;
        if (*s < 0x80) { cp = *s; len = 1; }
        else if ((*s & 0xE0) == 0xC0) { cp = *s & 0x1F; len = 2; }
        else if ((*s & 0xF0) == 0xE0) { cp = *s & 0x0F; len = 3; }
        else if ((*s & 0xF8) == 0xF0) { cp = *s & 0x07; len = 4; }
        else { s++; continue; }                       /* invalid lead byte */
        int ok = 1;
        for (int i = 1; i < len; i++) {
            if ((s[i] & 0xC0) != 0x80) { ok = 0; break; }
            cp = (cp << 6) | (s[i] & 0x3F);
        }
        if (!ok) { s++; continue; }
        s += len;
        if ((cp >= 32 && cp <= 126) || (cp >= 160 && cp <= 255)) out[o++] = (char)cp;
        else out[o++] = '?';
    }
    out[o] = 0;
}


/* ---------------------------------------------------------- Linux data */
static int meminfo_value(const char *text, const char *key, unsigned long *out)
{
    const char *p = strstr(text, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    char *end = NULL;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p) return 0;
    *out = v;
    return 1;
}

int parse_meminfo_text(const char *text, struct linux_meminfo *out)
{
    if (!text || !out) return 0;
    memset(out, 0, sizeof(*out));
    int ok = meminfo_value(text, "MemTotal", &out->total_kb);
    ok &= meminfo_value(text, "MemAvailable", &out->available_kb);
    meminfo_value(text, "SwapTotal", &out->swap_total_kb);
    meminfo_value(text, "SwapFree", &out->swap_free_kb);
    return ok && out->total_kb > 0;
}

int parse_cpu_stat_text(const char *text, unsigned long long *idle,
                        unsigned long long *total)
{
    if (!text || !idle || !total) return 0;
    const char *p = text;
    if (strncmp(p, "cpu ", 4) && strncmp(p, "cpu\t", 4)) {
        p = strstr(text, "\ncpu ");
        if (!p) return 0;
        p++;
    }
    unsigned long long v[10] = { 0 };
    int n = sscanf(p, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    if (n < 5) return 0;
    *idle = v[3] + v[4];
    *total = 0;
    for (int i = 0; i < 8 && i < n; i++) *total += v[i];
    return *total > 0;
}

int parse_loadavg_text(const char *text, double *load1, int *processes)
{
    if (!text) return 0;
    double one = 0, two = 0, three = 0;
    int running = 0, total = 0;
    if (sscanf(text, "%lf %lf %lf %d/%d", &one, &two, &three, &running, &total) < 4)
        return 0;
    if (load1) *load1 = one;
    if (processes) *processes = total;
    return 1;
}

/* ------------------------------------------------------------ app icons */
#include <stdint.h>
#include <zlib.h>

void rgba_image_free(struct rgba_image *img)
{
    if (!img) return;
    free(img->pixels);
    img->pixels = NULL;
    img->w = img->h = 0;
}

int rgba_image_scale_fit(struct rgba_image *img, int maxw, int maxh)
{
    if (!img || !img->pixels || img->w <= 0 || img->h <= 0) return 0;
    if (img->w <= maxw && img->h <= maxh) return 1;
    double scale = (double)maxw / img->w;
    double sy = (double)maxh / img->h;
    if (sy < scale) scale = sy;
    int nw = (int)(img->w * scale + 0.5), nh = (int)(img->h * scale + 0.5);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    unsigned char *out = malloc((size_t)nw * nh * 4);
    if (!out) return 0;
    for (int y = 0; y < nh; y++) {
        int src_y = y * img->h / nh;
        for (int x = 0; x < nw; x++) {
            int src_x = x * img->w / nw;
            memcpy(out + ((size_t)y * nw + x) * 4,
                   img->pixels + ((size_t)src_y * img->w + src_x) * 4, 4);
        }
    }
    free(img->pixels);
    img->pixels = out;
    img->w = nw; img->h = nh;
    return 1;
}

static unsigned int read_be32(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

static int png_load(FILE *f, struct rgba_image *out)
{
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    unsigned char header[8];
    if (fread(header, 1, 8, f) != 8 || memcmp(header, sig, 8)) return 0;

    unsigned int w = 0, h = 0;
    int bit_depth = 0, color_type = 0, interlace = 1;
    unsigned char palette[256][3];
    unsigned char trans_alpha[256];
    int have_palette = 0, trans_count = 0;
    unsigned char *idat = NULL;
    size_t idat_len = 0, idat_cap = 0;
    int have_ihdr = 0;

    for (;;) {
        unsigned char chunk_head[8];
        if (fread(chunk_head, 1, 8, f) != 8) break;
        unsigned int len = read_be32(chunk_head);
        if (len > 64u * 1024u * 1024u) { free(idat); return 0; }
        unsigned char *data = malloc(len ? len : 1);
        if (!data) { free(idat); return 0; }
        if (len && fread(data, 1, len, f) != len) { free(data); free(idat); return 0; }
        unsigned char crc[4];
        if (fread(crc, 1, 4, f) != 4) { free(data); break; }

        if (!memcmp(chunk_head + 4, "IHDR", 4) && len >= 13) {
            w = read_be32(data); h = read_be32(data + 4);
            bit_depth = data[8]; color_type = data[9]; interlace = data[12];
            have_ihdr = 1;
        } else if (!memcmp(chunk_head + 4, "PLTE", 4)) {
            unsigned int n = len / 3; if (n > 256) n = 256;
            for (unsigned int i = 0; i < n; i++) memcpy(palette[i], data + i * 3, 3);
            have_palette = (int)n;
        } else if (!memcmp(chunk_head + 4, "tRNS", 4)) {
            trans_count = len > 256 ? 256 : (int)len;
            memcpy(trans_alpha, data, trans_count);
        } else if (!memcmp(chunk_head + 4, "IDAT", 4)) {
            if (idat_len + len > idat_cap) {
                size_t cap = (idat_len + len) * 2 + 1024;
                unsigned char *next = realloc(idat, cap);
                if (!next) { free(data); free(idat); return 0; }
                idat = next; idat_cap = cap;
            }
            memcpy(idat + idat_len, data, len);
            idat_len += len;
        }
        int done = !memcmp(chunk_head + 4, "IEND", 4);
        free(data);
        if (done) break;
    }
    if (!have_ihdr || !idat || w == 0 || h == 0 || w > 4096 || h > 4096 || interlace != 0) {
        free(idat); return 0;
    }

    int channels = color_type == 0 ? 1 : color_type == 2 ? 3 : color_type == 3 ? 1 :
                   color_type == 4 ? 2 : color_type == 6 ? 4 : 0;
    if (!channels) { free(idat); return 0; }
    int bits_per_pixel = channels * bit_depth;
    size_t row_bytes = ((size_t)w * bits_per_pixel + 7) / 8;
    uint64_t raw_expected = (uint64_t)(row_bytes + 1) * h;
    if (raw_expected > 128u * 1024u * 1024u) { free(idat); return 0; }
    unsigned char *raw = malloc((size_t)raw_expected ? (size_t)raw_expected : 1);
    if (!raw) { free(idat); return 0; }
    uLongf raw_len = (uLongf)raw_expected;
    if (uncompress(raw, &raw_len, idat, (uLong)idat_len) != Z_OK || raw_len < raw_expected) {
        free(raw); free(idat); return 0;
    }
    free(idat);

    unsigned char *scan = malloc(row_bytes * h);
    unsigned char *rgba = malloc((size_t)w * h * 4);
    if (!scan || !rgba) { free(scan); free(rgba); free(raw); return 0; }
    int filter_bpp = bits_per_pixel / 8; if (filter_bpp < 1) filter_bpp = 1;
    for (unsigned int y = 0; y < h; y++) {
        const unsigned char *src = raw + (size_t)y * (row_bytes + 1);
        int filter = src[0];
        const unsigned char *line = src + 1;
        unsigned char *dst = scan + (size_t)y * row_bytes;
        const unsigned char *prev = y ? scan + (size_t)(y - 1) * row_bytes : NULL;
        for (size_t i = 0; i < row_bytes; i++) {
            int a = i >= (size_t)filter_bpp ? dst[i - filter_bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = prev && i >= (size_t)filter_bpp ? prev[i - filter_bpp] : 0;
            int v = line[i];
            switch (filter) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c;
                break; }
            default: free(scan); free(rgba); free(raw); return 0;
            }
            dst[i] = (unsigned char)v;
        }
    }
    free(raw);

    for (unsigned int y = 0; y < h; y++) {
        const unsigned char *row = scan + (size_t)y * row_bytes;
        for (unsigned int x = 0; x < w; x++) {
            unsigned char *px = rgba + ((size_t)y * w + x) * 4;
            px[0] = px[1] = px[2] = 0; px[3] = 255;
            if (color_type == 3) {
                int index;
                if (bit_depth == 8) index = row[x];
                else {
                    int bit = (int)x * bit_depth;
                    int byte = row[bit / 8];
                    int shift = 8 - bit_depth - (bit % 8);
                    index = (byte >> shift) & ((1 << bit_depth) - 1);
                }
                if (index < have_palette) memcpy(px, palette[index], 3);
                if (index < trans_count) px[3] = trans_alpha[index];
            } else {
                int samples[4] = { 0, 0, 0, 65535 };
                for (int ch = 0; ch < channels; ch++) {
                    if (bit_depth == 16) {
                        size_t off = ((size_t)x * channels + ch) * 2;
                        samples[ch] = (row[off] << 8) | row[off + 1];
                    } else if (bit_depth == 8) {
                        samples[ch] = row[(size_t)x * channels + ch] * 257;
                    } else {
                        int bit = ((int)x * channels + ch) * bit_depth;
                        int byte = row[bit / 8];
                        int shift = 8 - bit_depth - (bit % 8);
                        int maxv = (1 << bit_depth) - 1;
                        samples[ch] = ((byte >> shift) & maxv) * 65535 / maxv;
                    }
                }
                if (color_type == 0 || color_type == 4) {
                    px[0] = px[1] = px[2] = (unsigned char)(samples[0] >> 8);
                    if (color_type == 4) px[3] = (unsigned char)(samples[1] >> 8);
                } else {
                    px[0] = (unsigned char)(samples[0] >> 8);
                    px[1] = (unsigned char)(samples[1] >> 8);
                    px[2] = (unsigned char)(samples[2] >> 8);
                    if (color_type == 6) px[3] = (unsigned char)(samples[3] >> 8);
                }
            }
        }
    }
    free(scan);
    out->w = (int)w; out->h = (int)h; out->pixels = rgba;
    return 1;
}

struct xpm_color { char key[8]; unsigned char r, g, b, a; };

static int xpm_color_value(const char *s, struct xpm_color *c)
{
    if (!s || !strcasecmp(s, "None")) { c->r = c->g = c->b = 0; c->a = 0; return 1; }
    if (s[0] == '#') {
        unsigned long v = strtoul(s + 1, NULL, 16);
        size_t n = strlen(s + 1);
        if (n == 3) {
            c->r = (unsigned char)(((v >> 8) & 15) * 17);
            c->g = (unsigned char)(((v >> 4) & 15) * 17);
            c->b = (unsigned char)((v & 15) * 17);
        } else if (n >= 6) {
            if (n > 6) { /* 9/12-digit forms: keep the high bytes */
                c->r = (unsigned char)strtoul((char[]){ s[1], s[2], 0 }, NULL, 16);
                c->g = (unsigned char)strtoul((char[]){ s[n / 3 + 1], s[n / 3 + 2], 0 }, NULL, 16);
                c->b = (unsigned char)strtoul((char[]){ s[2 * (n / 3) + 1], s[2 * (n / 3) + 2], 0 }, NULL, 16);
            } else {
                c->r = (unsigned char)(v >> 16); c->g = (unsigned char)(v >> 8); c->b = (unsigned char)v;
            }
        } else return 0;
        c->a = 255; return 1;
    }
    static const struct { const char *name; unsigned char r, g, b; } names[] = {
        { "black", 0, 0, 0 }, { "white", 255, 255, 255 }, { "red", 255, 0, 0 },
        { "green", 0, 255, 0 }, { "blue", 0, 0, 255 }, { "yellow", 255, 255, 0 },
        { "cyan", 0, 255, 255 }, { "magenta", 255, 0, 255 }, { "gray", 128, 128, 128 },
        { "grey", 128, 128, 128 }, { "orange", 255, 165, 0 }, { "purple", 128, 0, 128 },
        { "brown", 165, 42, 42 }, { "pink", 255, 192, 203 },
    };
    for (unsigned int i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcasecmp(s, names[i].name)) {
            c->r = names[i].r; c->g = names[i].g; c->b = names[i].b; c->a = 255; return 1;
        }
    return 0;
}

static int xpm_load(FILE *f, struct rgba_image *out)
{
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0 || size > 16 * 1024 * 1024) return 0;
    fseek(f, 0, SEEK_SET);
    char *text = malloc(size + 1);
    if (!text) return 0;
    if (fread(text, 1, size, f) != (size_t)size) { free(text); return 0; }
    text[size] = 0;

    char **strings = NULL;
    int nstrings = 0, cap = 0;
    for (char *p = text; *p; p++) {
        if (*p != '"') continue;
        char *end = strchr(p + 1, '"');
        if (!end) break;
        if (nstrings == cap) {
            cap = cap ? cap * 2 : 64;
            strings = realloc(strings, cap * sizeof(*strings));
            if (!strings) { free(text); return 0; }
        }
        *end = 0;
        strings[nstrings++] = p + 1;
        p = end;
    }
    int w = 0, h = 0, ncolors = 0, cpp = 0;
    int ok = 0;
    struct xpm_color *colors = NULL;
    unsigned char *rgba = NULL;
    if (nstrings > 0 && sscanf(strings[0], "%d %d %d %d", &w, &h, &ncolors, &cpp) == 4 &&
        w > 0 && h > 0 && w <= 4096 && h <= 4096 && ncolors > 0 && cpp > 0 && cpp < 8 &&
        nstrings >= 1 + ncolors + h) {
        colors = calloc(ncolors, sizeof(*colors));
        rgba = malloc((size_t)w * h * 4);
        if (colors && rgba) {
            ok = 1;
            for (int i = 0; i < ncolors && ok; i++) {
                const char *line = strings[1 + i];
                if ((int)strlen(line) < cpp) { ok = 0; break; }
                memcpy(colors[i].key, line, cpp); colors[i].key[cpp] = 0;
                colors[i].a = 255;
                /* find the standalone "c" colour token */
                char copy[256]; snprintf(copy, sizeof(copy), "%s", line + cpp);
                char *save = NULL, *tok = strtok_r(copy, " \t", &save), *prev = NULL, *value = NULL;
                while (tok) {
                    if (prev && !strcmp(prev, "c")) { value = tok; break; }
                    prev = tok; tok = strtok_r(NULL, " \t", &save);
                }
                if (!value || !xpm_color_value(value, &colors[i])) { colors[i].r = 255; colors[i].b = 255; }
            }
            for (int y = 0; y < h && ok; y++) {
                const char *line = strings[1 + ncolors + y];
                if ((int)strlen(line) < w * cpp) { ok = 0; break; }
                for (int x = 0; x < w; x++) {
                    char key[8]; memcpy(key, line + x * cpp, cpp); key[cpp] = 0;
                    struct xpm_color *found = NULL;
                    for (int i = 0; i < ncolors; i++)
                        if (!strcmp(key, colors[i].key)) { found = &colors[i]; break; }
                    unsigned char *px = rgba + ((size_t)y * w + x) * 4;
                    if (found) { px[0] = found->r; px[1] = found->g; px[2] = found->b; px[3] = found->a; }
                    else { px[0] = 255; px[1] = 0; px[2] = 255; px[3] = 255; }
                }
            }
        }
    }
    free(colors);
    free(strings);
    free(text);
    if (!ok) { free(rgba); return 0; }
    out->w = w; out->h = h; out->pixels = rgba;
    return 1;
}

/* SVG is optional: use the desktop's gdk-pixbuf loader when present
 * (its normal librsvg loader handles SVG), without adding a toolkit or
 * a hard dependency to the WM. PNG and XPM stay on the small decoders
 * above. The scaled decode is bounded because icons are cached at small
 * sizes and never need a multi-thousand-pixel intermediate. */
static int pixbuf_image_load(const char *path, struct rgba_image *out)
{
    typedef void *(*new_scaled_fn)(const char *, int, int, int, void **);
    typedef int (*get_int_fn)(void *);
    typedef unsigned char *(*get_pixels_fn)(void *);
    typedef void (*unref_fn)(void *);
    void *gdk = dlopen("libgdk_pixbuf-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!gdk) return 0;
    void *gobj = dlopen("libgobject-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
    new_scaled_fn new_scaled = (new_scaled_fn)dlsym(gdk, "gdk_pixbuf_new_from_file_at_scale");
    get_int_fn get_width = (get_int_fn)dlsym(gdk, "gdk_pixbuf_get_width");
    get_int_fn get_height = (get_int_fn)dlsym(gdk, "gdk_pixbuf_get_height");
    get_int_fn get_channels = (get_int_fn)dlsym(gdk, "gdk_pixbuf_get_n_channels");
    get_int_fn get_rowstride = (get_int_fn)dlsym(gdk, "gdk_pixbuf_get_rowstride");
    get_pixels_fn get_pixels = (get_pixels_fn)dlsym(gdk, "gdk_pixbuf_get_pixels");
    unref_fn unref = gobj ? (unref_fn)dlsym(gobj, "g_object_unref") : NULL;
    if (!new_scaled || !get_width || !get_height || !get_channels ||
        !get_rowstride || !get_pixels || !unref) {
        if (gobj) dlclose(gobj);
        dlclose(gdk);
        return 0;
    }
    void *pixbuf = new_scaled(path, 256, 256, 1, NULL);
    int ok = 0;
    if (pixbuf) {
        int w = get_width(pixbuf), h = get_height(pixbuf);
        int channels = get_channels(pixbuf), stride = get_rowstride(pixbuf);
        unsigned char *src = get_pixels(pixbuf);
        if (w > 0 && h > 0 && w <= 256 && h <= 256 &&
            (channels == 3 || channels == 4) && stride >= w * channels && src) {
            unsigned char *rgba = malloc((size_t)w * h * 4);
            if (rgba) {
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        unsigned char *s = src + (size_t)y * stride + (size_t)x * channels;
                        unsigned char *d = rgba + ((size_t)y * w + x) * 4;
                        d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                        d[3] = channels == 4 ? s[3] : 255;
                    }
                }
                out->w = w; out->h = h; out->pixels = rgba;
                ok = 1;
            }
        }
        unref(pixbuf);
    }
    dlclose(gobj);
    dlclose(gdk);
    return ok;
}

static int has_supported_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".xpm") ||
                   !strcasecmp(dot, ".svg"));
}

int rgba_image_load(const char *path, struct rgba_image *out)
{
    if (!path || !out) return 0;
    out->w = out->h = 0; out->pixels = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char magic[8] = { 0 };
    size_t got = fread(magic, 1, 8, f);
    fseek(f, 0, SEEK_SET);
    int ok = 0;
    const char *dot = strrchr(path, '.');
    if (dot && !strcasecmp(dot, ".svg")) ok = pixbuf_image_load(path, out);
    else if (got == 8 && !memcmp(magic, "\x89PNG\r\n\x1a\n", 8)) ok = png_load(f, out);
    else ok = xpm_load(f, out);
    fclose(f);
    if (!ok) rgba_image_free(out);
    return ok;
}

static int file_exists(const char *path) { return access(path, R_OK) == 0; }

static int try_icon_file(const char *path, char *out, int cap)
{
    if (!file_exists(path)) return 0;
    if (has_supported_ext(path)) { snprintf(out, cap, "%s", path); return 1; }
    char buf[1024];
    const char *exts[] = { ".png", ".xpm", ".svg", NULL };
    for (int i = 0; exts[i]; i++) {
        snprintf(buf, sizeof(buf), "%s%s", path, exts[i]);
        if (file_exists(buf)) { snprintf(out, cap, "%s", buf); return 1; }
    }
    return 0;
}


/* Append s to out (current length *len, capacity cap); 0 if too long. */
static int append_str(char *out, size_t cap, size_t *len, const char *s)
{
    size_t n = strlen(s);
    if (*len + n >= cap) return 0;
    memcpy(out + *len, s, n + 1);
    *len += n;
    return 1;
}

int icon_resolve_in(const char *icon, const char *const *roots, int nroots,
                    int preferred_size, char *out, int cap)
{
    if (!icon || !*icon || !out || cap <= 0) return 0;
    out[0] = 0;
    if (icon[0] == '/' || !strncmp(icon, "./", 2) || !strncmp(icon, "../", 3))
        return try_icon_file(icon, out, cap);

    char name[256];
    snprintf(name, sizeof(name), "%s", icon);
    int has_ext = has_supported_ext(name);
    static const char *const contexts[] = {
        "apps", "categories", "devices", "places", "mimetypes", "actions", "status", "panel", NULL
    };
    int sizes[9];
    int nsizes = 0;
    const int wanted[] = { preferred_size, 48, 32, 24, 16, 64, 96, 128 };
    for (unsigned int i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
        int seen = 0;
        for (int j = 0; j < nsizes; j++) if (sizes[j] == wanted[i]) seen = 1;
        if (!seen) sizes[nsizes++] = wanted[i];
    }
    const char *exts[] = { ".png", ".xpm", ".svg", NULL };

    for (int r = 0; r < nroots; r++) {
        const char *rootp = roots[r];
        if (!rootp || !*rootp) continue;
        const char *base_name = strrchr(rootp, '/');
        base_name = base_name ? base_name + 1 : rootp;
        char path[4096];
        if (!strcmp(base_name, "pixmaps")) {
            for (int e = 0; exts[e]; e++) {
                size_t len = 0; path[0] = 0;
                if (append_str(path, sizeof(path), &len, rootp) &&
                    append_str(path, sizeof(path), &len, "/") &&
                    append_str(path, sizeof(path), &len, name) &&
                    (has_ext || append_str(path, sizeof(path), &len, exts[e])) &&
                    file_exists(path)) { snprintf(out, cap, "%s", path); return 1; }
                if (has_ext) break;
            }
            continue;
        }
        /* Theme roots: hicolor first, then any sibling theme dirs. */
        char themes[64][256];
        int nthemes = 0;
        snprintf(themes[nthemes++], sizeof(themes[0]), "hicolor");
        DIR *d = opendir(rootp);
        if (d) {
            struct dirent *entry;
            while ((entry = readdir(d)) != NULL && nthemes < 64) {
                if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "hicolor")) continue;
                snprintf(themes[nthemes++], sizeof(themes[0]), "%s", entry->d_name);
            }
            closedir(d);
        }
        for (int t = 0; t < nthemes; t++) {
            for (int si = 0; si < nsizes; si++) {
                for (int ci = 0; contexts[ci]; ci++) {
                    for (int e = 0; exts[e]; e++) {
                        char sizedir[32];
                        snprintf(sizedir, sizeof(sizedir), "%dx%d", sizes[si], sizes[si]);
                        size_t len = 0; path[0] = 0;
                        if (append_str(path, sizeof(path), &len, rootp) &&
                            append_str(path, sizeof(path), &len, "/") &&
                            append_str(path, sizeof(path), &len, themes[t]) &&
                            append_str(path, sizeof(path), &len, "/") &&
                            append_str(path, sizeof(path), &len, sizedir) &&
                            append_str(path, sizeof(path), &len, "/") &&
                            append_str(path, sizeof(path), &len, contexts[ci]) &&
                            append_str(path, sizeof(path), &len, "/") &&
                            append_str(path, sizeof(path), &len, name) &&
                            (has_ext || append_str(path, sizeof(path), &len, exts[e])) &&
                            file_exists(path)) { snprintf(out, cap, "%s", path); return 1; }
                        if (has_ext) break;
                    }
                }
            }
            for (int ci = 0; contexts[ci]; ci++) {
                for (int e = 0; exts[e]; e++) {
                    size_t len = 0; path[0] = 0;
                    if (append_str(path, sizeof(path), &len, rootp) &&
                        append_str(path, sizeof(path), &len, "/") &&
                        append_str(path, sizeof(path), &len, themes[t]) &&
                        append_str(path, sizeof(path), &len, "/scalable/") &&
                        append_str(path, sizeof(path), &len, contexts[ci]) &&
                        append_str(path, sizeof(path), &len, "/") &&
                        append_str(path, sizeof(path), &len, name) &&
                        (has_ext || append_str(path, sizeof(path), &len, exts[e])) &&
                        file_exists(path)) { snprintf(out, cap, "%s", path); return 1; }
                    if (has_ext) break;
                }
            }
        }
    }
    return 0;
}

int icon_resolve(const char *icon, int preferred_size, char *out, int cap)
{
    const char *home = getenv("HOME");
    char local_icons[512], local_pixmaps[512];
    snprintf(local_icons, sizeof(local_icons), "%s/.local/share/icons", home ? home : "");
    snprintf(local_pixmaps, sizeof(local_pixmaps), "%s/.local/share/pixmaps", home ? home : "");
    const char *roots[] = {
        local_icons, "/usr/local/share/icons", "/usr/share/icons",
        local_pixmaps, "/usr/local/share/pixmaps", "/usr/share/pixmaps",
    };
    return icon_resolve_in(icon, roots, 6, preferred_size, out, cap);
}
