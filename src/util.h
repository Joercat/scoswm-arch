/* Pure helpers for scoswm: themes, config, .desktop discovery.
 * Nothing here talks to X, so everything here is unit-tested by
 * tests/test_util.c. */
#ifndef SCOSWM_UTIL_H
#define SCOSWM_UTIL_H

/* The four SCos themes, values copied from SCos kernel/desktop/theme.c
 * (struct theme there): main accent, wallpaper gradient, window bg,
 * text, text-on-accent, taskbar bg. */
struct scos_theme {
    const char *id, *name;
    unsigned int main, bg_top, bg_bot, win_bg, text, title_text, taskbar_bg;
};
extern const struct scos_theme scos_themes[];
extern const int scos_theme_count;
const struct scos_theme *theme_by_id(const char *id);
const struct scos_theme *theme_by_index(int i);

/* Blend two 0xRRGGBB colours; t is 0..255, all of a at 0, all of b at 255.
 * Same arithmetic as SCos fb.c blend()/color_blend(). */
unsigned int color_blend_rgb(unsigned int a, unsigned int b, int t);
int parse_hex_color(const char *s, unsigned int *out);   /* "#39FF14" / "39FF14" */

/* --------------------------------------------------------------- config */
struct scos_config {
    char theme[32];        /* theme id, or "custom" with the fields below */
    unsigned int custom[7];/* main,bg_top,bg_bot,win_bg,text,title_text,taskbar_bg */
    int has_custom;
    char terminal[160];    /* "" = auto-detect */
    char files[160];       /* "" = auto-detect, used by the Files pin */
    char font[160];        /* X core font name, "" = WM default list */
    char pins[1024];       /* comma-separated .desktop ids or commands */
    int bar_height;        /* SCos used 40 */
};
void config_defaults(struct scos_config *cfg);
/* Parse a simple key=value file ("# comment", whitespace trimmed).
 * Unknown keys are ignored; returns number of keys applied, -1 if the
 * file could not be opened. */
int config_load_file(const char *path, struct scos_config *cfg);
/* Write the configuration back in scoswm.conf format. Comments and
 * unknown keys are not preserved. Returns 1 on success. */
int config_save_file(const char *path, const struct scos_config *cfg);
void theme_apply_custom(struct scos_theme *out, const struct scos_config *cfg);

/* ------------------------------------------------------- .desktop apps */
#define DESKTOP_APP_MAX_NAME 128
struct desktop_app {
    char id[96];           /* file basename without .desktop */
    char name[DESKTOP_APP_MAX_NAME];
    char exec[512];
    char icon[128];
    int is_terminal;       /* Terminal=true: run inside the terminal */
    int nodisplay;
};
int desktop_parse_file(const char *path, struct desktop_app *app);
/* Scan application dirs (later dirs lose to earlier ones on duplicate
 * id), skipping NoDisplay entries. Returns the app count, sorted by name. */
int desktop_scan(const char *const *dirs, int ndirs, struct desktop_app *out, int max);
int desktop_app_cmp(const void *a, const void *b);
/* Strip freedesktop Exec field codes (%f %F %u %U %d %D %n %N %i %c %k %v %m);
 * %% becomes %. */
void exec_strip_field_codes(const char *in, char *out, int cap);
/* Case-insensitive substring test (for launcher search). */
int ci_contains(const char *hay, const char *needle);
/* UTF-8 -> the Latin-1 subset an X core font can show; unrepresentable
 * characters become '?', invalid bytes are skipped. */
void utf8_to_latin1(const char *in, char *out, int cap);

/* ---------------------------------------------------------- Linux data */
struct linux_meminfo {
    unsigned long total_kb, available_kb, swap_total_kb, swap_free_kb;
};
/* Parsers accept complete file text so they can be unit-tested without
 * depending on the host's live /proc contents. */
int parse_meminfo_text(const char *text, struct linux_meminfo *out);
int parse_cpu_stat_text(const char *text, unsigned long long *idle,
                        unsigned long long *total);
int parse_loadavg_text(const char *text, double *load1, int *processes);

#endif
