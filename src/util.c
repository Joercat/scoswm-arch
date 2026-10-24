#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
            char path[1024];
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
