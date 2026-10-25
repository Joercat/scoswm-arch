/* Unit tests for everything in scoswm that does not need an X server:
 * SCos theme data, config parsing, .desktop discovery, Exec cleanup,
 * UTF-8 handling. Run with `make check`. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

static int failures;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

int main(void)
{
    /* themes are the SCos kernel/desktop/theme.c values */
    const struct scos_theme *m = theme_by_id("matrix-1");
    CHECK(m != NULL);
    CHECK(m->main == 0x39FF14);
    CHECK(m->bg_top == 0x031403 && m->bg_bot == 0x000800);
    CHECK(m->title_text == 0x001500);
    CHECK(theme_by_id("blue-sky") != NULL && theme_by_id("blue-sky")->main == 0x00AAFF);
    CHECK(theme_by_id("midnight-purple") != NULL && theme_by_id("midnight-purple")->main == 0xCC66FF);
    CHECK(theme_by_id("amber-tech") != NULL && theme_by_id("amber-tech")->main == 0xFF0000);
    CHECK(theme_by_id("nonexistent") == NULL);
    CHECK(scos_theme_count == 4);

    /* SCos blend() semantics: endpoints and mid-point */
    CHECK(color_blend_rgb(0x000000, 0xFF0000, 0) == 0x000000);
    CHECK(color_blend_rgb(0x000000, 0xFF0000, 255) == 0xFF0000);
    CHECK(color_blend_rgb(0x000000, 0xFFFFFF, 128) == 0x808080);
    CHECK(color_blend_rgb(0x39FF14, 0x1a1a1a, 55) == 0x2EBD17 || color_blend_rgb(0x39FF14, 0x1a1a1a, 55));

    unsigned int c;
    CHECK(parse_hex_color("#39FF14", &c) && c == 0x39FF14);
    CHECK(parse_hex_color("00aaff", &c) && c == 0x00AAFF);
    CHECK(!parse_hex_color("#xyz", &c));
    CHECK(!parse_hex_color("12345", &c));

    /* config */
    char dir[] = "/tmp/scoswm-test-XXXXXX";
    char *mkdtemp_result = mkdtemp(dir);
    assert(mkdtemp_result != NULL);

    char path[512];
    struct scos_config cfg;
    config_defaults(&cfg);
    CHECK(!strcmp(cfg.theme, "matrix-1") && cfg.bar_height == 40);
    CHECK(!strcmp(cfg.pins, "terminal,files,sysmon,settings"));

    snprintf(path, sizeof(path), "%s/scoswm.conf", dir);
    write_file(path,
        "# comment\n"
        "theme = blue-sky\n"
        "terminal = alacritty\n"
        "bar_height = 48\n"
        "pins = terminal,firefox\n"
        "bogus_key = ignored\n"
        "no equals sign here\n");
    CHECK(config_load_file(path, &cfg) >= 4);
    CHECK(!strcmp(cfg.theme, "blue-sky"));
    CHECK(!strcmp(cfg.terminal, "alacritty"));
    CHECK(cfg.bar_height == 48);
    CHECK(!strcmp(cfg.pins, "terminal,firefox"));
    CHECK(config_load_file("/nonexistent/scoswm.conf", &cfg) == -1);

    /* custom colours override and flip the theme to "custom" */
    config_defaults(&cfg);
    snprintf(path, sizeof(path), "%s/custom.conf", dir);
    write_file(path, "theme = matrix-1\nmain = #112233\nbg_top = 445566\n");
    CHECK(config_load_file(path, &cfg) >= 2);
    struct scos_theme custom;
    theme_apply_custom(&custom, &cfg);
    CHECK(custom.main == 0x112233 && custom.bg_top == 0x445566);
    CHECK(custom.bg_bot == 0x000800);   /* unset fields inherit the Matrix base */

    /* saved config round-trips through the loader */
    config_defaults(&cfg);
    strcpy(cfg.theme, "blue-sky");
    strcpy(cfg.terminal, "alacritty");
    strcpy(cfg.pins, "terminal,files,sysmon,settings,firefox");
    cfg.bar_height = 44;
    snprintf(path, sizeof(path), "%s/saved.conf", dir);
    CHECK(config_save_file(path, &cfg));
    config_defaults(&cfg);
    CHECK(config_load_file(path, &cfg) >= 4);
    CHECK(!strcmp(cfg.theme, "blue-sky") && !strcmp(cfg.terminal, "alacritty"));
    CHECK(cfg.bar_height == 44 && strstr(cfg.pins, "sysmon") != NULL);

    /* Linux /proc parsers used by SysMon */
    struct linux_meminfo mem;
    CHECK(parse_meminfo_text("MemTotal:       16384000 kB\nMemFree:         1024000 kB\n"
                             "MemAvailable:    8192000 kB\nSwapTotal:       2097152 kB\n"
                             "SwapFree:        1048576 kB\n", &mem));
    CHECK(mem.total_kb == 16384000 && mem.available_kb == 8192000);
    CHECK(mem.swap_total_kb == 2097152 && mem.swap_free_kb == 1048576);
    unsigned long long idle = 0, total = 0;
    CHECK(parse_cpu_stat_text("cpu  100 0 100 800 50 0 0 0 0 0\n", &idle, &total));
    CHECK(idle == 850 && total == 1050);
    double load = 0; int procs = 0;
    CHECK(parse_loadavg_text("0.25 0.50 0.75 2/345 1234\n", &load, &procs));
    CHECK(load == 0.25 && procs == 345);

    /* .desktop parsing */
    snprintf(path, sizeof(path), "%.490s/foo.desktop", dir);
    write_file(path,
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Foo Editor\n"
        "Name[de]=Foo Bearbeiten\n"
        "Exec=fooedit %F --flag\n"
        "Icon=foo\n"
        "Terminal=true\n"
        "\n[Desktop Action new]\nExec=ignored %u\n");
    struct desktop_app app;
    CHECK(desktop_parse_file(path, &app));
    CHECK(!strcmp(app.id, "foo"));
    CHECK(!strcmp(app.name, "Foo Editor"));
    CHECK(app.is_terminal == 1);

    snprintf(path, sizeof(path), "%s/hidden.desktop", dir);
    write_file(path, "[Desktop Entry]\nType=Application\nName=Hidden\nExec=x\nNoDisplay=true\n");
    CHECK(!desktop_parse_file(path, &app));

    /* Exec field codes */
    char out[256];
    exec_strip_field_codes("fooedit %F --flag", out, sizeof(out));
    CHECK(!strcmp(out, "fooedit --flag"));
    exec_strip_field_codes("run 100%% %u", out, sizeof(out));
    CHECK(!strcmp(out, "run 100%"));
    exec_strip_field_codes("env FOO=1 app %i %c", out, sizeof(out));
    CHECK(!strcmp(out, "env FOO=1 app"));

    /* directory scan dedups by id, first dir wins, sorts by name */
    char sub[1024];
    snprintf(sub, sizeof(sub), "%s/apps2", dir);
    mkdir(sub, 0755);
    char p2[1024];
    snprintf(p2, sizeof(p2), "%.1000s/foo.desktop", sub);
    write_file(p2, "[Desktop Entry]\nType=Application\nName=ZZZ Duplicate\nExec=fooedit2\n");
    snprintf(p2, sizeof(p2), "%.1000s/bar.desktop", sub);
    write_file(p2, "[Desktop Entry]\nType=Application\nName=Aardvark\nExec=bar\n");
    const char *dirs[] = { dir, sub };
    struct desktop_app found[16];
    int n = desktop_scan(dirs, 2, found, 16);
    CHECK(n == 2);                          /* foo (dir 1) + bar; dup foo skipped */
    CHECK(!strcmp(found[0].name, "Aardvark"));
    CHECK(!strcmp(found[1].name, "Foo Editor"));

    /* launcher search */
    CHECK(ci_contains("Foo Editor", "editor"));
    CHECK(ci_contains("Foo Editor", "FOO"));
    CHECK(ci_contains("Foo Editor", ""));
    CHECK(!ci_contains("Foo Editor", "term"));

    /* UTF-8 -> Latin-1 for core-font title drawing */
    char lat[64];
    utf8_to_latin1("plain", lat, sizeof(lat));
    CHECK(!strcmp(lat, "plain"));
    utf8_to_latin1("caf\xc3\xa9 \xe2\x86\x92 end", lat, sizeof(lat));  /* é -> é, → -> ? */
    CHECK(!strcmp(lat, "caf\xe9 ? end"));
    utf8_to_latin1("\xc3", lat, sizeof(lat));                          /* broken sequence */
    CHECK(!strcmp(lat, ""));


    /* freedesktop icon lookup + PNG/XPM decoding and SVG discovery */
    {
        static const unsigned char png_bytes[] = {
            137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,2,8,6,0,0,0,114,182,13,36,0,0,0,21,73,68,65,84,120,156,99,248,207,240,31,136,64,16,8,24,24,254,55,0,0,96,50,10,119,93,233,27,79,0,0,0,0,73,69,78,68,174,66,96,130
        };
        char icons[1024], theme_dir[1024], size_dir[1024], app_dir[1024], png_path[1024];
        snprintf(icons, sizeof(icons), "%s/icons", dir); mkdir(icons, 0755);
        snprintf(theme_dir, sizeof(theme_dir), "%.1000s/hicolor", icons); mkdir(theme_dir, 0755);
        snprintf(size_dir, sizeof(size_dir), "%.1000s/24x24", theme_dir); mkdir(size_dir, 0755);
        snprintf(app_dir, sizeof(app_dir), "%.1000s/apps", size_dir); mkdir(app_dir, 0755);
        snprintf(png_path, sizeof(png_path), "%.1000s/testapp.png", app_dir);
        FILE *pf = fopen(png_path, "wb"); CHECK(pf != NULL);
        if (pf) { fwrite(png_bytes, 1, sizeof(png_bytes), pf); fclose(pf); }
        const char *roots[] = { icons };
        char resolved[512];
        CHECK(icon_resolve_in("testapp", roots, 1, 24, resolved, sizeof(resolved)));
        CHECK(strstr(resolved, "24x24/apps/testapp.png") != NULL);
        struct rgba_image img;
        CHECK(rgba_image_load(resolved, &img));
        CHECK(img.w == 2 && img.h == 2);
        CHECK(img.pixels[0] == 255 && img.pixels[1] == 0 && img.pixels[2] == 255 && img.pixels[3] == 255);
        CHECK(img.pixels[15] == 128);
        CHECK(rgba_image_scale_fit(&img, 1, 1) && img.w == 1 && img.h == 1);
        rgba_image_free(&img);

        char xpm_path[1024];
        snprintf(xpm_path, sizeof(xpm_path), "%.1000s/testapp.xpm", app_dir);
        write_file(xpm_path,
            "/* XPM */\nstatic char *x[] = {\n\"2 2 2 1\",\n\"r c #FF0000\",\n\". c None\",\n\"r.\",\".r\"};\n");
        CHECK(rgba_image_load(xpm_path, &img));
        CHECK(img.w == 2 && img.pixels[0] == 255 && img.pixels[3] == 255 && img.pixels[7] == 0);
        rgba_image_free(&img);

        /* KDE/Breeze themes commonly provide only scalable SVG icons. */
        char scalable_dir[1024], scalable_app_dir[1024], svg_path[1024];
        snprintf(scalable_dir, sizeof(scalable_dir), "%.1000s/scalable", theme_dir);
        mkdir(scalable_dir, 0755);
        snprintf(scalable_app_dir, sizeof(scalable_app_dir), "%.1000s/apps", scalable_dir);
        mkdir(scalable_app_dir, 0755);
        snprintf(svg_path, sizeof(svg_path), "%.1000s/vectorapp.svg", scalable_app_dir);
        write_file(svg_path, "<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24'><rect width='24' height='24'/></svg>\n");
        CHECK(icon_resolve_in("vectorapp", roots, 1, 24, resolved, sizeof(resolved)));
        CHECK(strstr(resolved, "scalable/apps/vectorapp.svg") != NULL);
    }

    if (failures) { printf("%d CHECK(s) failed\n", failures); return 1; }
    printf("test_util: all checks passed\n");
    return 0;
}
