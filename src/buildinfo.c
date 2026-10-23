/*
 * SCos native - build provenance.
 *
 * This exists because a flashed image and the repository it is claimed to come from can silently be
 * different builds, and every surface that describes the OS then describes the wrong one.  The strings
 * below are the ones the boot stamp was generated from; `version`, `graphics`, the About panel, the TTY
 * and the log all quote them, so a stale or uncommitted build is visible without booting a debugger.
 */
#include "scos.h"

static void cat(char *out, int cap, int *n, const char *s)
{
    while (*s && *n + 1 < cap) out[(*n)++] = *s++;
    out[*n] = 0;
}

void scos_build_stamp(char *out, int cap)
{
    int n = 0;
    if (!out || cap < 2) { if (out && cap) out[0] = 0; return; }
    cat(out, cap, &n, SCOS_BUILD_COMMIT);
    cat(out, cap, &n, " ");
    cat(out, cap, &n, SCOS_BUILD_TREE);
    cat(out, cap, &n, ", ");
    cat(out, cap, &n, SCOS_BUILD_DATE);
}

int scos_build_modified(void)
{
    return SCOS_BUILD_TREE[0] == 'm';
}
