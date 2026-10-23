/* Entry point and desktop startup sequence for the Linux port.
 *
 * Mirrors desktop_start() in the bare-metal kernel/desktop/platform.c,
 * with each step replaced by its host equivalent (see PORTING.md).
 */
#include "scos.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>

int opt_frames;
const char *opt_screenshot;
const char *opt_click;
const char *opt_type;

static void usage(const char *argv0)
{
    printf(
"usage: %s [options]\n"
"\n"
"SCos window manager (Linux port) - the SCos desktop in a host window.\n"
"\n"
"  --width N        desktop width in pixels (default 1024)\n"
"  --height N       desktop height in pixels (default 768)\n"
"  --fullscreen     open fullscreen (SDL backend)\n"
"  --open APP       open this app at startup (files, terminal, notepad,\n"
"                   calendar, settings, about, browser, blackjack,\n"
"                   solitaire, sysmon)\n"
"  --data-dir DIR   persistence directory (default: $XDG_DATA_HOME/scoswm\n"
"                   or ~/.local/share/scoswm; snapshot: DIR/vfs.snapshot)\n"
"  --no-boot        skip the boot splash screen\n"
"  --frames N       quit after N presented frames (headless backend)\n"
"  --screenshot F   write the final frame to F as a PPM image (headless)\n"
"  --click X,Y      synthetic click (headless testing)\n"
"  --type TEXT      type TEXT followed by Enter (headless testing)\n"
"  --help           this text\n"
"\n"
"Environment: SCOSWM_DEBUG=1 logs the SCos kernel log to stderr.\n",
        argv0);
}

int main(int argc, char **argv)
{
    int width = 1024, height = 768, fullscreen = 0, boot = 1;
    const char *open_app = NULL, *data_dir = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); return 0; }
        else if (!strcmp(a, "--width") && i + 1 < argc) width = atoi(argv[++i]);
        else if (!strcmp(a, "--height") && i + 1 < argc) height = atoi(argv[++i]);
        else if (!strcmp(a, "--fullscreen")) fullscreen = 1;
        else if (!strcmp(a, "--no-boot")) boot = 0;
        else if (!strcmp(a, "--open") && i + 1 < argc) open_app = argv[++i];
        else if (!strcmp(a, "--data-dir") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(a, "--frames") && i + 1 < argc) opt_frames = atoi(argv[++i]);
        else if (!strcmp(a, "--screenshot") && i + 1 < argc) opt_screenshot = argv[++i];
        else if (!strcmp(a, "--click") && i + 1 < argc) opt_click = argv[++i];
        else if (!strcmp(a, "--type") && i + 1 < argc) opt_type = argv[++i];
        else { fprintf(stderr, "scoswm: unknown option '%s' (try --help)\n", a); return 2; }
    }
    if (width < 640) width = 640;
    if (height < 480) height = 480;

    platform_data_dir_init(data_dir);
    if (platform_backend_init(width, height, fullscreen) != 0) {
        fprintf(stderr, "scoswm: could not initialise the display backend\n");
        return 1;
    }

    mm_init();
    fb_init(width, height);

    if (boot && !opt_frames) {
        boot_screen_init();
        boot_screen_step("core: host process, monotonic 100 Hz tick ready", 25);
        char line[128], cpu[49];
        cpu_brand(cpu, sizeof(cpu));
        snprintf(line, sizeof(line), "cpu: %s", cpu);
        boot_screen_step(line, 45);
        boot_screen_step("video: host window, CPU software compositor", 65);
    }

    if (!vfs_init_defaults()) panic("initial desktop filesystem allocation failed");
    int loaded = fs_image_load();
    system_files_init(fs_image_available());
    if (boot && !opt_frames)
        boot_screen_step(loaded ? "fs: saved snapshot loaded"
                                : "fs: defaults active; snapshot save available", 80);

    theme_load_from_settings();
    apps_register_all();
    cpu_meter_init();
    wm_init();
    if (boot && !opt_frames) boot_screen_step("wm: compositor ready", 100);

    if (open_app && !wm_open_app(open_app, NULL))
        fprintf(stderr, "scoswm: no such app '%s'\n", open_app);

    wm_run();

    if (opt_screenshot && !platform_screenshot(opt_screenshot))
        fprintf(stderr, "scoswm: could not write screenshot '%s'\n", opt_screenshot);

    /* Clean exit persists the RAM filesystem (theme, desktop layout,
     * documents) to the snapshot file; Terminal 'save' does the same
     * at any time.  Bare metal required an explicit verified save. */
    if (fs_image_available() && !fs_image_save())
        fprintf(stderr, "scoswm: warning: could not write snapshot '%s'\n", fs_image_target());

    platform_backend_shutdown();
    return 0;
}
