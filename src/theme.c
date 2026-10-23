/* SCos native - theme palettes (same four themes as the web simulation) */
#include "scos.h"

/* tiny json helpers */
static char *strstr_safe(const char *hay, const char *needle)
{
    int n = (int)strlen(needle);
    for (const char *p = hay; *p; p++)
        if (!strncmp(p, needle, n)) return (char *)p;
    return NULL;
}

static const char *json_value(const char *json, const char *key)
{
    const char *p = strstr_safe(json, key);
    if (!p) return NULL;
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static const struct theme themes[] = {
    { "matrix-1",       "Matrix",     0x39FF14, 0x031403, 0x000800, 0x1a1a1a, 0x39FF14, 0x001500, 0x141414 },
    { "blue-sky",       "Blue Sky",   0x00AAFF, 0x001933, 0x000a14, 0x002B4D, 0x00AAFF, 0x001020, 0x0d1b2a },
    { "midnight-purple","Purple",     0xCC66FF, 0x1A0033, 0x0a0014, 0x330066, 0xCC66FF, 0x140028, 0x1c1024 },
    { "amber-tech",     "Matrix red", 0xFF0000, 0x1a0000, 0x080000, 0x4D0000, 0xFF0000, 0x1a0000, 0x201010 },
};

static int current = 0;

int theme_count(void) { return 4+theme_custom_count(); }
const struct theme *theme_get(int i) { return i>=4?theme_custom_get(i-4):(i>=0?&themes[i]:NULL); }
const struct theme *theme_current(void) { return theme_get(current); }
void theme_set_index(int i)
{
    if (i >= 0 && i < theme_count()) current = i;
    wm_wallpaper_invalidate();
}
int theme_index_of_id(const char *id)
{
    for (int i = 0; i < theme_count(); i++)
        if (!strcmp(theme_get(i)->id, id)) return i;
    return -1;
}

static struct prefs prefs = { PREFS_MOUSE_DEFAULT, PREFS_DBL_DEFAULT };
const struct prefs *prefs_get(void) { return &prefs; }

void prefs_set_mouse(int sens)
{
    if (sens < 1) sens = 1;
    if (sens > 6) sens = 6;
    prefs.mouse_sens = sens;
    settings_save();
}

void prefs_set_dbl(int ms)
{
    if (ms < 200) ms = 200;
    if (ms > 900) ms = 900;
    prefs.dbl_ms = ms;
    settings_save();
}

static int json_int(const char *json, const char *key)
{
    const char *p = json_value(json, key);
    if (!p || *p < '0' || *p > '9') return -1;
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        int digit = *p++ - '0';
        if (v > (2147483647 - digit) / 10) return -1;
        v = v * 10 + digit;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ',' && *p != '}' && *p) return -1;
    return v;
}

void theme_load_from_settings(void)
{
    current=0;
    theme_custom_load();
    u32 len = 0;
    char *json = vfs_read("system/settings.json", &len);
    if (!json) return;
    int ms = json_int(json, "\"mouse_sens\"");
    if (ms >= 0) prefs.mouse_sens = ms < 1 ? 1 : (ms > 6 ? 6 : ms);
    int db = json_int(json, "\"dbl_ms\"");
    if (db >= 0) prefs.dbl_ms = db < 200 ? 200 : (db > 900 ? 900 : db);
    const char *p = json_value(json, "\"theme\"");
    if (!p || *p++ != '"') return;
    char id[32];
    int i = 0;
    while (p[i] && p[i] != '"' && i < 31) { id[i] = p[i]; i++; }
    if (p[i] != '"') return;
    id[i] = 0;
    int idx = theme_index_of_id(id);
    if (idx >= 0) theme_set_index(idx);
}

void settings_save(void)
{
    char buf[224];
    const struct theme *t = theme_current();
    /* manual format: no printf-to-buffer in the kernel */
    char *o = buf;
    char num[8];
    strcpy(o, "{\n  \"theme\": \""); o += strlen(o);
    strcpy(o, t->id); o += strlen(o);
    strcpy(o, "\",\n  \"mouse_sens\": "); o += strlen(o);
    fmt_u32(num, (u32)prefs.mouse_sens); strcpy(o, num); o += strlen(o);
    strcpy(o, ",\n  \"dbl_ms\": "); o += strlen(o);
    fmt_u32(num, (u32)prefs.dbl_ms); strcpy(o, num); o += strlen(o);
    strcpy(o, ",\n  \"version\": \"x64-dev\"\n}"); o += strlen(o);
    vfs_write("system/settings.json", buf, (u32)(o - buf));
}

void system_reset(void)
{
    prefs.mouse_sens = PREFS_MOUSE_DEFAULT;
    prefs.dbl_ms = PREFS_DBL_DEFAULT;
    theme_set_index(0);
    vfs_init_defaults();          /* rebuilds the tree from scratch */
    settings_save();
}
