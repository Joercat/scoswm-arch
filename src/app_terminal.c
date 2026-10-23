/*
 * SCos Terminal.
 *
 * Ported from the web simulation and then made real: every command runs
 * against the live kernel (VFS, CMOS/RTC, PIT, page allocator, WM).
 * r27: in-window TABS (click the strip, Ctrl+T / Ctrl+Tab / Ctrl+W),
 * `appstrt` launching apps with their logs streaming into the launching
 * tab like a Linux console, a real process table (`procs`) and `kill`.
 * Nothing is simulated - `ping` honestly reports there is no TCP/IP
 * stack instead of inventing latency.
 */
#include "scos.h"

#define TERM_LINES 512
#define TERM_LINE 256
#define HIST 16
#define TERM_MAX_TABS 6
#define TERM_TAB_H 22
#define TAB_BTN_W 74

struct term {
    struct shell_confirm confirmation;
    char lines[TERM_LINES][TERM_LINE];
    int nlines;
    /* Display rows, not lines: a line the window is too narrow for becomes two rows, and everything that
     * scrolls has to count rows or the second half of a wrapped report is unreachable.  Cached per line
     * because recomputing it from the text on every frame would scan half a megabyte to paint a screen;
     * `rows_dirty' is set whenever the buffer changes and `rows_cols' records the width the counts belong
     * to, so a resize re-wraps everything at once. */
    short lrows[TERM_LINES];
    int rows_cols, rows_dirty;
    int total_rows;
    int scroll;              /* first visible display row */
    int follow;              /* stick to bottom while new output arrives */
    char input[TERM_LINE];
    int ipos;
    char hist[HIST][TERM_LINE];
    int hcount, hindex;
    char cwd[128];
    /* typewriter effect */
    char pending[2048];
    int pend_len, pend_shown, pending_active;
    char aliases[8][2][64];
    int nalias;
    int shutting_down;
    /* nano-style full screen editor */
    int edit_mode;
    char ed_path[192];
    char ed_buf[128][96];
    int ed_lines, ed_row, ed_col, ed_scroll, ed_modified;
    struct window *win;        /* back-pointer: tab ops + console redraws */
};

/* one terminal window = a strip of tabs, each an independent shell with
 * its own scrollback, cwd, history and aliases */
struct termwin {
    struct term *tabs[TERM_MAX_TABS];
    int ntabs;
    int active;
};

static inline struct termwin *TW(struct window *w)
{
    return (struct termwin *)w->data;
}
static inline struct term *WT(struct window *w)
{
    struct termwin *tw = TW(w);
    return tw->tabs[tw->active];
}

static void term_new_tab(struct window *w);
static void term_close_tab(struct window *w, int i);

static void term_push_line(struct term *t, const char *s)
{
    /* Split on \n, and split anything longer than one entry at a word boundary as well.  The second split is
     * what changed here: a 260-character report line used to have its tail dropped in silence - no mark that
     * anything had been lost - and the dropped part was the last clause of a diagnostic line.  A break the
     * terminal chose keeps the information; a truncation loses it.  The display re-wraps these rows to the
     * window's width anyway, so all this costs is a place to put text that has to go somewhere. */
    const char *p = s;
    while (*p) {
        const char *nl = p;
        while (*nl && *nl != '\n') nl++;
        int len = (int)(nl - p);
        char over[TERM_LINE];
        const char *entry = p;
        int whole = 1;                    /* 0 while one long line is still arriving row by row */
        if (len >= TERM_LINE) {
            const char *after = s_wrap_row(p, TERM_LINE - 1, over, sizeof over);
            entry = over;
            len = (int)strlen(over);
            if (after) { p = after; whole = 0; } else { p = nl; }
        }
        if (t->nlines < TERM_LINES) {
            memcpy(t->lines[t->nlines], entry, len);
            t->lines[t->nlines][len] = 0;
            t->nlines++;
        } else {
            for (int i = 1; i < TERM_LINES; i++)
                memcpy(t->lines[i - 1], t->lines[i], TERM_LINE);
            memcpy(t->lines[TERM_LINES - 1], entry, len);
            t->lines[TERM_LINES - 1][len] = 0;
        }
        t->rows_dirty = 1;
        if (whole) {
            if (!*nl) break;
            p = nl + 1;
        } else if (!*p) break;
    }
    if (t->follow) t->scroll = 1000000;      /* stay glued to the new output */
}

static void term_print(struct term *t, const char *s)   /* immediate */
{
    term_push_line(t, s);
}

static void term_type(struct term *t, const char *s)    /* typewriter */
{
    strncpy(t->pending, s, sizeof(t->pending) - 1);
    t->pending[sizeof(t->pending) - 1] = 0;
    t->pend_len = (int)strlen(t->pending);
    t->pend_shown = 0;
    t->pending_active = 1;
}


/* r39: bounded, ALWAYS-terminated copy into neofetch's info[] lines.
 * strncpy(dst, src, 40) leaves dst unterminated when src >= 40 chars -
 * the 45-char i5 brand string made the renderer read on into the next
 * line's bytes ("a bunch of random names after the CPU id" in the
 * field). */
static void info_set(char *line, int linesz, const char *prefix, const char *val)
{
    strcpy(line, prefix);
    int pl = (int)strlen(prefix);
    int vl = (int)strlen(val);
    if (pl + vl > linesz - 1) vl = linesz - 1 - pl;
    memcpy(line + pl, val, (u32)vl);
    line[pl + vl] = 0;
}

static void prompt_str(struct term *t, char *out)
{
    /* mirror the web simulation: home shows as "~" */
    char tmp[160];
    strcpy(tmp, t->cwd);
    int l = (int)strlen(tmp);
    while (l > 1 && tmp[l - 1] == '/') tmp[--l] = 0;
    char dispbuf[160];
    const char *disp = tmp;
    if (!strcmp(tmp, "/home")) disp = "~";
    else if (!strncmp(tmp, "/home/", 6)) {
        dispbuf[0] = '~';
        strcpy(dispbuf + 1, tmp + 5);
        disp = dispbuf;
    }
    strcpy(out, "user@scos:");
    strcat(out, disp);
    strcat(out, "$ ");
}

/* ------------------------------------------------------------ commands --- */
static const char *help_text =
    "Available commands:\n"
    "help      - Show this help message\n"
    "ls        - List directory contents\n"
    "cd        - Change directory\n"
    "pwd       - Print working directory\n"
    "cat       - Show file contents\n"
    "head      - Show first lines of a file\n"
    "wc        - Count lines, words, bytes of a file\n"
    "grep      - Print lines matching a string\n"
    "hexdump   - Dump file bytes in hex\n"
    "tree      - Recursive directory tree\n"
    "echo      - Display a message\n"
    "clear     - Clear terminal screen\n"
    "date      - Show current date and time (CMOS clock)\n"
    "cal       - Show this month's calendar\n"
    "mkdir     - Create directory\n"
    "touch     - Create an empty file\n"
    "cp        - Copy a file\n"
    "mv        - Move or rename a file\n"
    "rm [-s]   - Delete file (-s also allows system files)\n"
    "edit <f>  - Full-screen editor (nano-like: ^O save, ^X exit)\n"
    "appstrt   - Launch an app; its logs stream into this tab\n"
    "apps      - List installed apps and running instances\n"
    "appuninstall <id> - Remove a user app (confirmed; keeps data)\n"
    "graphics  - Detected GPUs and actual renderer status\n"
    "procs     - Process table with real per-task memory\n"
    "kill <pid>- Terminate an app task (see 'procs')\n"
    "tab       - new | close | <n>: terminal tabs (also Ctrl+T)\n"
    "whoami    - Show current user\n"
    "version   - Show system version and the commit + tree state it was built from\n"
    "uptime    - Time since boot (PIT)\n"
    "free      - Memory usage (real page allocator)\n"
    "cpu       - CPU brand, TSC reference rate and load\n"
    "df        - Filesystem usage\n"
    "disks     - Snapshot persistence target\n"
    "neofetch  - System summary with logo\n"
    "klog      - Kernel service/error log\n"
    "sysrq <a> - System request: a = panic|reboot|dump|time\n"
    "tty [1-6] - Switch to a text console; Ctrl+Alt+F7 returns to desktop\n"
    "theme     - List or switch themes\n"
    "calc      - Perform basic arithmetic\n"
    "ping      - Network status (TCP/IP unavailable)\n"
    "scinfo    - Display system information\n"
    "alias     - Create command aliases\n"
    "history   - Show command history\n"
    "save      - Write the filesystem image to disk\n"
    "kill --system <pid> - confirmed stop (scwm only)\n"
    "shutdown [--confirm] - Power the machine off\n"
    "reboot    - Restart the machine\n"
    "panic     - Trigger a kernel panic screen (for inspection)\n";

static const char *month_names[12] = { "Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec" };
static const char *day_names[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };

static void fmt_size(char *out, u32 bytes)
{
    if (bytes < 1024) { fmt_u32(out, bytes); strcat(out, " bytes"); }
    else if (bytes < 1024 * 1024) {
        fmt_u32(out, bytes / 1024); strcat(out, "."); fmt_u32(out + strlen(out), (bytes % 1024) * 100 / 1024);
        strcat(out, " KB");
    } else {
        fmt_u32(out, bytes / (1024 * 1024)); strcat(out, ".");
        fmt_u32(out + strlen(out), (bytes % (1024 * 1024)) * 100 / (1024 * 1024));
        strcat(out, " MB");
    }
}

static void resolve_path(struct term *t, const char *arg, char *out)
{
    if (arg[0] == '/') strcpy(out, arg);
    else {
        strcpy(out, t->cwd);
        int l = (int)strlen(out);
        if (l && out[l - 1] != '/') { out[l] = '/'; out[l + 1] = 0; }
        strcat(out, arg);
    }
}


static void run_command(struct term *t, const char *command)
{
    char cmdbuf[TERM_LINE];
    strncpy(cmdbuf, command, sizeof(cmdbuf) - 1);
    cmdbuf[sizeof(cmdbuf) - 1] = 0;

    int confirmed = t->confirmation.command[0] != 0;
    char confirmation_message[192];
    if (confirmed && confirm_command(&t->confirmation, cmdbuf, sizeof(cmdbuf), confirmation_message)) {
        term_print(t, confirmation_message);
        return;
    }

    char prompt[160];
    prompt_str(t, prompt);
    char echo[TERM_LINE + 160];
    strcpy(echo, prompt); strcat(echo, cmdbuf);
    term_print(t, echo);

    if (cmdbuf[0]) {
        if (t->hcount < HIST) strcpy(t->hist[t->hcount++], cmdbuf);
        else {
            for (int i = 1; i < HIST; i++) strcpy(t->hist[i - 1], t->hist[i]);
            strcpy(t->hist[HIST - 1], cmdbuf);
        }
        t->hindex = t->hcount;
    }

    /* alias expansion */
    char effective[TERM_LINE];
    strcpy(effective, cmdbuf);
    char *sp = effective;
    while (*sp == ' ') sp++;
    char first[64];
    int i = 0;
    while (sp[i] && sp[i] != ' ' && i < 63) { first[i] = sp[i]; i++; }
    first[i] = 0;
    for (int a = 0; !confirmed && a < t->nalias; a++)
        if (!strcmp(t->aliases[a][0], first)) {
            char rest[TERM_LINE];
            strcpy(rest, sp + i);
            strcpy(effective, t->aliases[a][1]);
            strcat(effective, rest);
            break;
        }

    if (!confirmed && confirm_command(&t->confirmation,effective,sizeof(effective),confirmation_message)) {
        term_print(t,confirmation_message); return;
    }
    /* tokenize (naive, space separated like the web version) */
    char *args[24];
    int nargs = 0;
    char *p = effective;
    while (*p && nargs < 24) {
        while (*p == ' ') p++;
        if (!*p) break;
        args[nargs++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    if (!nargs) return;

    const char *cmd = args[0];
    char response[2560];   /* holds the GPU detection report, which lists every adapter */
    response[0] = 0;
    int typed = 1;

    if (!strcmp(cmd, "help")) {
        int page = 1;
        if (nargs > 1 && !strncmp(args[1], "--p", 3)) page = (int)str_to_u32(args[1] + 3);
        else if (nargs > 1) page = (int)str_to_u32(args[1]);
        if (page < 1) page = 1;
        const int PER = 18;
        int total_lines = 1;
        for (const char *q = help_text; *q; q++) if (*q == '\n') total_lines++;
        int pages = (total_lines + PER - 1) / PER;
        if (page > pages) page = pages;
        response[0] = 0;
        int line = 0;
        for (const char *q = help_text; *q; ) {
            const char *nl = str_chr(q, '\n');
            int len = nl ? (int)(nl - q) : (int)strlen(q);
            if (line >= (page - 1) * PER && line < page * PER) {
                strncat(response, q, (u32)len);
                strcat(response, "\n");
            }
            line++;
            if (!nl) break;
            q = nl + 1;
        }
        char foot[64];
        strcpy(foot, "-- page ");
        char n[8];
        fmt_u32(n, (u32)page); strcat(foot, n); strcat(foot, "/");
        fmt_u32(n, (u32)pages); strcat(foot, n);
        if (page < pages) { strcat(foot, " : 'help --p");
            fmt_u32(n, (u32)(page + 1)); strcat(foot, n); strcat(foot, "' for more --"); }
        else strcat(foot, " --");
        strcat(response, foot);
    }
    else if (!strcmp(cmd, "ls")) {
        char path[256];
        resolve_path(t, nargs > 1 ? args[1] : "", path);
        if (!strncmp(path, "/system", 7))
            strcpy(response, "Warning: /system holds OS files - view with care, "
                             "edits can break SCos\n");
        struct vfs_node *n = vfs_lookup(path);
        if (!n || !vfs_is_dir(n)) strcat(response, "Error: Invalid path or permission denied.");
        else {
            char names[64][VFS_NAME];
            int c = vfs_list(n, names, 64);
            if (!c) strcpy(response, "(empty directory)");
            else {
                for (int j = 0; j < c; j++) {
                    if (j) strcat(response, "  ");
                    strcat(response, names[j]);
                }
            }
        }
    }
    else if (!strcmp(cmd, "cd")) {
        char target[256];
        if (nargs < 2) strcpy(target, "/home/");
        else if (!strcmp(args[1], "..")) {
            char tmp[256];
            vfs_parent_path(t->cwd, tmp);
            strcpy(target, tmp);
        } else resolve_path(t, args[1], target);
        int len = (int)strlen(target);
        if (len && target[len - 1] != '/') strcat(target, "/");
        struct vfs_node *n = vfs_lookup(target);
        if (n && vfs_is_dir(n)) {
            strcpy(t->cwd, target);
            strcpy(response, "Changed directory to "); strcat(response, target);
        } else {
            strcpy(response, "Error: Directory not found: "); strcat(response, target);
        }
    }
    else if (!strcmp(cmd, "cat")) {
        if (nargs < 2) strcpy(response, "Error: No filename specified");
        else {
            char path[256];
            resolve_path(t, args[1], path);
            u32 len = 0;
            char *data = vfs_read(path, &len);
            if (!data) strcpy(response, "Error: File not found");
            else {
                /* binary file? show a hex preview instead of raw bytes */
                int weird = 0, probe = len < 256 ? (int)len : 256;
                for (int i = 0; i < probe; i++)
                    if ((unsigned char)data[i] != '\n' && (unsigned char)data[i] != '\t' &&
                        ((unsigned char)data[i] < 32 || (unsigned char)data[i] > 126)) weird++;
                if (probe && weird * 5 > probe) {
                    static const char hx[] = "0123456789abcdef";
                    char *o = response;
                    char num[12];
                    strcpy(o, "Binary file ("); o += strlen(o);
                    fmt_u32(num, len); strcpy(o, num); o += strlen(o);
                    strcpy(o, " bytes) - hex preview, first "); o += strlen(o);
                    fmt_u32(num, (u32)(probe < 64 ? probe : 64)); strcpy(o, num); o += strlen(o);
                    strcpy(o, " bytes.\nUse 'hexdump "); o += strlen(o);
                    strncat(o, path, 100); o += strlen(o);
                    strcpy(o, "' for a full dump.\n\n"); o += strlen(o);
                    int nb = probe < 64 ? probe : 64;
                    for (int i = 0; i < nb; i++) {
                        if (i % 16 == 0 && o + 8 < response + sizeof(response)) {
                            *o++ = hx[(i >> 12) & 15]; *o++ = hx[(i >> 8) & 15];
                            *o++ = hx[(i >> 4) & 15]; *o++ = hx[i & 15];
                            *o++ = ':'; *o++ = ' ';
                        }
                        if (o + 4 < response + sizeof(response)) {
                            *o++ = hx[(data[i] >> 4) & 15];
                            *o++ = hx[data[i] & 15];
                            *o++ = (i % 16 == 15 || i == nb - 1) ? '\n' : ' ';
                        }
                    }
                    *o = 0;
                    if (!strncmp(path, "/system", 7))
                        strcat(response, "\n(this is a real copy of a system file - the disk original is untouched)");
                } else {
                    char *o = response;
                    if (!strncmp(path, "/system", 7)) {
                        strcpy(o, "Warning: system file - editing it can break SCos\n");
                        o += strlen(o);
                    }
                    strncpy(o, data, sizeof(response) - 1 - (u32)(o - response));
                    response[sizeof(response) - 1] = 0;
                }
            }
        }
    }
    else if (!strcmp(cmd, "edit") || !strcmp(cmd, "nano")) {
        if (nargs < 2) strcpy(response, "Error: usage: edit <file>");
        else {
            char path[256];
            resolve_path(t, args[1], path);
            t->edit_mode = 1;
            t->ed_row = t->ed_col = t->ed_scroll = 0;
            t->ed_modified = 0;
            t->ed_lines = 0;
            strncpy(t->ed_path, path, sizeof(t->ed_path) - 1);
            u32 len = 0;
            char *data = vfs_read(path, &len);
            if (data) {
                char *start = data;
                while (t->ed_lines < 128) {
                    char *nl = str_chr(start, '\n');
                    int ln = nl ? (int)(nl - start) : (int)strlen(start);
                    if (ln > 95) ln = 95;
                    memcpy(t->ed_buf[t->ed_lines], start, ln);
                    t->ed_buf[t->ed_lines][ln] = 0;
                    t->ed_lines++;
                    if (!nl) break;
                    start = nl + 1;
                    if (!*start) {
                        if (t->ed_lines < 128) {
                            t->ed_buf[t->ed_lines][0] = 0;
                            t->ed_lines++;
                        }
                        break;
                    }
                }
            }
            if (!t->ed_lines) { t->ed_buf[0][0] = 0; t->ed_lines = 1; }
            strcpy(response, "");
        }
    }
    else if (!strcmp(cmd, "echo")) {
        for (int j = 1; j < nargs; j++) {
            if (j > 1) strcat(response, " ");
            strcat(response, args[j]);
        }
    }
    else if (!strcmp(cmd, "clear")) {
        t->nlines = 0;
        /* The row cache belongs to the buffer, so emptying the buffer empties it: a stale total_rows would
         * leave the scroll range pointing at rows that no longer exist. */
        t->total_rows = 0;
        t->rows_dirty = 1;
        t->scroll = -1;
    t->follow = 1;
        t->follow = 1;
        return;
    }
    else if (!strcmp(cmd, "sysrq")) {
        /* Linux-style system request: one command, several real actions */
        if (nargs < 2) {
            strcpy(response,
                "sysrq - system request key. Actions:\n"
                "  sysrq panic [msg]  trigger a real kernel panic\n"
                "  sysrq reboot       reboot the machine now\n"
                "  sysrq dump         dump recent kernel log to this terminal\n"
                "  sysrq time         PIT uptime, tick rate and RTC clock");
        } else if (!strcmp(args[1], "panic")) {
            char msg[128];
            msg[0] = 0;
            for (int j = 2; j < nargs; j++) { if (j > 2) strncat(msg," ",sizeof(msg)-strlen(msg)-1); strncat(msg,args[j],sizeof(msg)-strlen(msg)-1); }
            term_print(t, "Triggering kernel panic as requested...");
            sleep_ms(600);
            kernel_panic(msg[0] ? msg : "panic requested via sysrq");
            return;
        } else if (!strcmp(args[1], "reboot")) {
            term_print(t, "Rebooting SCos... Please wait.");
            t->shutting_down = 2;
            return;
        } else if (!strcmp(args[1], "dump")) {
            int n = klog_ring_count();
            int start = n > 14 ? n - 14 : 0;
            response[0] = 0;
            for (int i = start; i < n; i++) {
                char ln[96];
                if (klog_ring(i, ln, sizeof(ln))) {
                    if (response[0]) strcat(response, "\n");
                    strncat(response, ln, sizeof(response) - strlen(response) - 2);
                }
            }
            if (!response[0]) strcpy(response, "(log empty)");
        } else if (!strcmp(args[1], "time")) {
            struct rtc_time rt;
            rtc_read(&rt);
            char a[16], b[16];
            fmt_u32(a, uptime_ms());
            fmt_pad2(b, rt.hour);
            strcpy(response, "uptime: "); strcat(response, a); strcat(response, " ms (PIT ticks)\n");
            char c[8];
            fmt_pad2(c, rt.min); strcat(response, "rtc:    ");
            strcat(response, b); strcat(response, ":"); strcat(response, c);
            strcat(response, " local CMOS clock");
        } else {
            strcpy(response, "sysrq: unknown action '");
            strncat(response, args[1], 32);
            strcat(response, "' - run 'sysrq' for the list");
        }
    }
    else if (!strcmp(cmd, "klog") || !strcmp(cmd, "dmesg")) {
        int n = klog_ring_count();
        int start = n > 14 ? n - 14 : 0;
        response[0] = 0;
        for (int i = start; i < n; i++) {
            char ln[96];
            if (klog_ring(i, ln, sizeof(ln))) {
                if (response[0]) strcat(response, "\n");
                strncat(response, ln, sizeof(response) - strlen(response) - 2);
            }
        }
        if (!response[0]) strcpy(response, "(log empty)");
    }
    else if (!strcmp(cmd, "date")) {
        struct rtc_time rt;
        rtc_read(&rt);
        char a[4], b[4], c[4], d[8];
        fmt_pad2(a, rt.hour); fmt_pad2(b, rt.min); fmt_pad2(c, rt.sec);
        fmt_u32(d, rt.year);
        strcpy(response, day_names[(rt.weekday ? rt.weekday : 7) - 1]);
        strcat(response, " "); strcat(response, month_names[rt.mon - 1]);
        strcat(response, " "); fmt_pad2(a, rt.day); strcat(response, a);
        strcat(response, " "); strcat(response, d);
        strcat(response, " "); fmt_pad2(a, rt.hour); strcat(response, a);
        strcat(response, ":"); fmt_pad2(a, rt.min); strcat(response, a);
        strcat(response, ":"); fmt_pad2(a, rt.sec); strcat(response, a);
        strcat(response, " CMOS");
    }
    else if (!strcmp(cmd, "mkdir")) {
        if (nargs < 2) strcpy(response, "Error: No directory name specified");
        else {
            char path[256];
            resolve_path(t, args[1], path);
            if (vfs_mkdir(path)) {
                strcpy(response, "Directory created: "); strcat(response, path); strcat(response, "/");
            } else strcpy(response, "Error: Failed to create directory");
        }
    }
    else if (!strcmp(cmd, "touch")) {
        if (nargs < 2) strcpy(response, "Error: No filename specified");
        else {
            char path[256];
            resolve_path(t, args[1], path);
            if (vfs_write(path, "", 0)) {
                strcpy(response, "File created: "); strcat(response, path);
            } else strcpy(response, "Error: Failed to create file");
        }
    }
    else if (!strcmp(cmd, "rm")) {
        int force = 0, invalid = 0;
        const char *target = NULL;
        for (int j = 1; j < nargs; j++) {
            if (!strcmp(args[j], "-s") || !strcmp(args[j], "-f")) force = 1;
            else if (!strcmp(args[j], "-i")) { }
            else if (args[j][0]=='-' || target) invalid=1;
            else target = args[j];
        }
        if (invalid) strcpy(response,"Usage: rm [-i] [-s|-f] <path>; -s/-f allow system files and always confirm.");
        else if (!target) strcpy(response, "Error: No file or directory specified. (rm [-s] <path>)");
        else {
            char path[256];
            resolve_path(t, target, path);
            if (!force && !strncmp(path, "/system/", 8))
                strcpy(response, "Error: system file - use 'rm -s <path>' to delete it anyway");
            else if (vfs_delete(path)) {
                strcpy(response, "Removed: "); strcat(response, path);
            } else strcpy(response, "Error: File or directory not found.");
        }
    }
    else if (!strcmp(cmd, "whoami")) strcpy(response, "user");
    else if (!strcmp(cmd, "version")) {
        strcpy(response, "SCos unnumbered x64 development - build " SCOS_BUILD_TAG " from ");
        scos_build_stamp(response + strlen(response), sizeof(response) - strlen(response));
        if (scos_build_modified())
            strncat(response, "  [WARNING: built with uncommitted changes]", sizeof(response) - strlen(response) - 1);
    }
    else if (!strcmp(cmd, "calc")) {
        if (nargs != 4) strcpy(response, "Usage: calc <number1> <operator> <number2>");
        else {
            char *e1, *e2;
            double n1 = strtod_simple(args[1], &e1);
            double n2 = strtod_simple(args[3], &e2);
            if (*e1 || *e2) strcpy(response, "Error: Invalid numbers. Please use numeric values.");
            else {
                double r = 0;
                int ok = 1;
                if (!strcmp(args[2], "+")) r = n1 + n2;
                else if (!strcmp(args[2], "-")) r = n1 - n2;
                else if (!strcmp(args[2], "*")) r = n1 * n2;
                else if (!strcmp(args[2], "/")) {
                    if (n2 == 0) { strcpy(response, "Error: Division by zero is not allowed."); ok = 0; }
                    else r = n1 / n2;
                } else { strcpy(response, "Error: Invalid operator. Use +, -, *, or /. Check \"help calc\""); ok = 0; }
                if (ok) {
                    strcpy(response, "Result: ");
                    fmt_double(response + strlen(response), r);
                }
            }
        }
    }
    else if (!strcmp(cmd, "ping")) {
        strcpy(response, "ping: this kernel has no TCP/IP stack - network unreachable.\n"
                         "No network interface driver is present (see 'disks'/'cpu' for real hardware).");
    }
    else if (!strcmp(cmd, "scinfo") || !strcmp(cmd, "sysinfo")) {
        char a[32], b[32];
        struct rtc_time rt;
        rtc_read(&rt);
        fmt_size(a, vfs_usage_bytes());
        fmt_pad2(b, rt.hour);
        strcpy(response, "SCos System Information:\n");
        strcat(response, "OS Version: unnumbered x64 development\n");
        strcat(response, "Kernel: sckern (SCos x64 development)\n");
        strcat(response, "Architecture: x86-64 (native UEFI)\n");
        strcat(response, "Uptime: "); fmt_u32(b, uptime_ms() / 60000); strcat(response, b); strcat(response, " minutes\n");
        strcat(response, "Managed Memory: "); fmt_u64(b, mm_total_kb() / 1024); strcat(response, b); strcat(response, " MB\n");
        strcat(response, "Free Memory: "); fmt_u64(b, mm_free_kb() / 1024); strcat(response, b); strcat(response, " MB\n");
        { char cc[8];
          strcat(response, "CPU Cores: "); fmt_u32(cc, cpu_core_count());
          strcat(response, cc); strcat(response, " (");
          fmt_u32(cc, cpu_thread_count()); strcat(response, cc);
          strcat(response, " threads)\n"); }
        strcat(response, "Storage Used: "); strcat(response, a); strcat(response, "\n");
        strcat(response, "Disk: "); strcat(response, fs_image_available() ? "snapshot persistence available" : "RAM-only (no snapshot)");
        strcat(response, fs_image_found ? " (SCos image loaded)" : "");
        strcat(response, "\nCurrent Time: ");
        fmt_pad2(b, rt.hour); strcat(response, b); strcat(response, ":");
        fmt_pad2(b, rt.min); strcat(response, b); strcat(response, ":");
        fmt_pad2(b, rt.sec); strcat(response, b);
    }
    else if (!strcmp(cmd, "alias")) {
        if (nargs == 1) {
            if (!t->nalias) strcpy(response, "No aliases set.");
            else {
                strcpy(response, "Current aliases:\n");
                for (int a2 = 0; a2 < t->nalias; a2++) {
                    strcat(response, t->aliases[a2][0]);
                    strcat(response, "='");
                    strcat(response, t->aliases[a2][1]);
                    strcat(response, "'\n");
                }
            }
        } else if (nargs >= 3 && t->nalias < 8) {
            strcpy(t->aliases[t->nalias][0], args[1]);
            strcpy(t->aliases[t->nalias][1], args[2]);
            t->nalias++;
            strcpy(response, "Alias '"); strcat(response, args[1]);
            strcat(response, "' set to '"); strcat(response, args[2]); strcat(response, "'");
        } else strcpy(response, "Usage: alias <name> '<command>'");
    }
    else if (!strcmp(cmd, "history")) {
        if (!t->hcount) strcpy(response, "No commands in history.");
        else {
            for (int j = 0; j < t->hcount; j++) {
                char num[8];
                fmt_u32(num, j + 1);
                strcat(response, num); strcat(response, "  ");
                strcat(response, t->hist[j]);
                strcat(response, "\n");
            }
        }
    }
    else if(!strcmp(cmd,"appuninstall")){
        if(nargs!=2||!strcmp(args[1],"--help"))strcpy(response,"Usage: appuninstall <app-id> (confirmed; projects/data kept)");
        else app_uninstall(args[1],response,sizeof(response));
    }
    else if(!strcmp(cmd,"graphics")){if(nargs!=1)strcpy(response,"Usage: graphics");else graphics_report(response,sizeof(response));}
    else if (!strcmp(cmd, "appstrt")) {
        lua_apps_refresh();
        /* r27: THE way to launch apps from the terminal. Like running a
         * program from a Linux shell: the window opens, this tab becomes
         * its console, and the app's real events stream in here. */
        if (nargs < 2) {
            strcpy(response, "Usage: appstrt <app|file.cat> [file]\nInstalled:");
            for (int i2 = 0; i2 < app_count(); i2++) {
                struct app *a = app_at(i2);
                if (!a || a->id[0] == '_') continue;
                if (strlen(response)+strlen(a->id)+6 >= sizeof(response)) {
                    strcat(response,"\n...");break;
                }
                strcat(response, " ");
                strcat(response, a->id);
            }
        } else {
            struct app *a = app_find(args[1]);
            if (!a) { char source[256]; resolve_path(t,args[1],source); a=lua_app_install(source); }
            if (!a || a->id[0] == '_') {
                strcpy(response, "app "); strcat(response, args[1]);
                strcat(response, " failed to launch: unknown app");
            } else {
                int already = wm_app_running(a->id);
                wm_set_pending_console(t);     /* open()-time logs land here */
                char path_arg[256];void *arg=NULL;
                if(nargs>2){resolve_path(t,args[2],path_arg);arg=path_arg;}
                struct window *nw = wm_open_app(a->id,arg);
                if (!nw) {
                    strcpy(response, "app "); strcat(response, args[1]);
                    strcat(response, " failed to launch");
                } else {
                    strcpy(response, "app "); strcat(response, args[1]);
                    const char *failure=nw->app->failure ? nw->app->failure(nw) : NULL;
                    if (failure) {
                        strcat(response," stopped: ");
                        strncat(response,failure,256);
                    } else strcat(response, already ? " already running - focused"
                                                    : " started successfully");
                }
            }
        }
    }
    else if (!strcmp(cmd, "blackjack") || !strcmp(cmd, "solitaire") ||
             !strcmp(cmd, "sysmon")) {
        strcpy(response,"Use appstrt ");strcat(response,cmd);
    }
    else if (!strcmp(cmd, "save")) {
        if (nargs != 1) strcpy(response, "Usage: save");
        else if (!fs_image_available()) strcpy(response, "No writable snapshot location. Files are RAM-only; no file was written.");
        else if (fs_image_save()) strcpy(response, "Filesystem image written to disk.");
        else strcpy(response, "Save failed. See klog; previous disk save may be incomplete.");
    }
    else if (!strcmp(cmd, "tty")) {
        int number=1;
        if (nargs>2 || (nargs==2 && (!parse_pid(args[1],&number) || number<1 || number>6)))
            strcpy(response,"Usage: tty [1-6]");
        else { (void)number; strcpy(response,"Text consoles (tty) are a bare-metal SCos feature; not available in the Linux port."); }
    }
    else if (!strcmp(cmd, "shutdown")) {
        if (nargs > 1 && (nargs != 2 || strcmp(args[1],"--confirm"))) {
            term_print(t,"Usage: shutdown [--confirm]"); return;
        }
        term_print(t, "Shutting down SCos... Goodbye!");
        t->shutting_down = 1;
        return;
    }
    else if (!strcmp(cmd, "reboot")) {
        if (nargs > 1 && (nargs != 2 || strcmp(args[1],"--confirm"))) {
            term_print(t,"Usage: reboot [--confirm]"); return;
        }
        term_print(t, "Rebooting SCos... Please wait.");
        t->shutting_down = 2;
        return;
    }
    else if (!strcmp(cmd, "pwd")) {
        strcpy(response, t->cwd);
    }
    else if (!strcmp(cmd, "cp")) {
        if (nargs < 3) strcpy(response, "Usage: cp <src> <dst>");
        else {
            char a[256], b[256];
            resolve_path(t, args[1], a); resolve_path(t, args[2], b);
            u32 len = 0;
            char *data = vfs_read(a, &len);
            if (!data) strcpy(response, "Error: cannot read source");
            else if (vfs_write(b, data, len)) { strcpy(response, "Copied to "); strcat(response, b); }
            else strcpy(response, "Error: cannot write destination");
        }
    }
    else if (!strcmp(cmd, "mv")) {
        if (nargs < 3) strcpy(response, "Usage: mv <src> <dst>");
        else {
            char a[256], b[256];
            resolve_path(t, args[1], a); resolve_path(t, args[2], b);
            if (vfs_rename(a, b)) { strcpy(response, "Moved to "); strcat(response, b); }
            else strcpy(response, "Error: rename failed (missing path or name taken)");
        }
    }
    else if (!strcmp(cmd, "head")) {
        if (nargs < 2) strcpy(response, "Usage: head <file> [lines]");
        else {
            char a[256];
            resolve_path(t, args[1], a);
            u32 len = 0;
            char *data = vfs_read(a, &len);
            if (!data) strcpy(response, "Error: File not found");
            else {
                int n = nargs > 2 ? (int)str_to_u32(args[2]) : 10;
                if (n < 1) n = 1;
                response[0] = 0;
                const char *p = data;
                for (int i = 0; i < n && *p; i++) {
                    const char *nl = str_chr(p, '\n');
                    int l = nl ? (int)(nl - p) : (int)strlen(p);
                    if ((int)strlen(response) + l + 2 > (int)sizeof(response) - 1) break;
                    strncat(response, p, l);
                    strcat(response, "\n");
                    if (!nl) break;
                    p = nl + 1;
                }
            }
        }
    }
    else if (!strcmp(cmd, "wc")) {
        if (nargs < 2) strcpy(response, "Usage: wc <file>");
        else {
            char a[256];
            resolve_path(t, args[1], a);
            u32 len = 0;
            char *data = vfs_read(a, &len);
            if (!data) strcpy(response, "Error: File not found");
            else {
                u32 lines = 0, words = 0;
                int inw = 0;
                for (u32 i = 0; i < len; i++) {
                    char c = data[i];
                    if (c == '\n') lines++;
                    if (c == ' ' || c == '\n' || c == '\t') inw = 0;
                    else if (!inw) { inw = 1; words++; }
                }
                char n[12];
                response[0] = 0;
                fmt_u32(n, lines); strcat(response, n); strcat(response, " lines, ");
                fmt_u32(n, words); strcat(response, n); strcat(response, " words, ");
                fmt_u32(n, len); strcat(response, n); strcat(response, " bytes");
            }
        }
    }
    else if (!strcmp(cmd, "grep")) {
        if (nargs < 3) strcpy(response, "Usage: grep <text> <file>");
        else {
            char a[256];
            resolve_path(t, args[2], a);
            u32 len = 0;
            char *data = vfs_read(a, &len);
            if (!data) strcpy(response, "Error: File not found");
            else {
                response[0] = 0;
                const char *p = data;
                while (*p) {
                    const char *nl = str_chr(p, '\n');
                    int l = nl ? (int)(nl - p) : (int)strlen(p);
                    char one[256];
                    if (l > (int)sizeof(one) - 1) l = sizeof(one) - 1;
                    memcpy(one, p, l);
                    one[l] = 0;
                    if (str_str(one, args[1])) {
                        if ((int)strlen(response) + l + 2 < (int)sizeof(response) - 1) {
                            strcat(response, one); strcat(response, "\n");
                        }
                    }
                    if (!nl) break;
                    p = nl + 1;
                }
                if (!response[0]) strcpy(response, "(no matches)");
            }
        }
    }
    else if (!strcmp(cmd, "hexdump")) {
        if (nargs < 2) strcpy(response, "Usage: hexdump <file>");
        else {
            char a[256];
            resolve_path(t, args[1], a);
            u32 len = 0;
            char *data = vfs_read(a, &len);
            if (!data) strcpy(response, "Error: File not found");
            else {
                if (len > 128) len = 128;
                response[0] = 0;
                static const char hx[] = "0123456789abcdef";
                for (u32 i = 0; i < len; i += 16) {
                    char line[80];
                    int o = 0;
                    for (u32 j = i; j < i + 16 && j < len; j++) {
                        line[o++] = hx[(u8)data[j] >> 4];
                        line[o++] = hx[data[j] & 15];
                        line[o++] = ' ';
                    }
                    line[o++] = '|';
                    for (u32 j = i; j < i + 16 && j < len; j++)
                        line[o++] = (data[j] >= 32 && data[j] < 127) ? data[j] : '.';
                    line[o++] = '|'; line[o] = 0;
                    if ((int)strlen(response) + o + 2 < (int)sizeof(response) - 1) {
                        strcat(response, line); strcat(response, "\n");
                    }
                }
            }
        }
    }
    else if (!strcmp(cmd, "tree")) {
        char path[256];
        resolve_path(t, nargs > 1 ? args[1] : "", path);
        struct vfs_node *n = vfs_lookup(path);
        if (!n || !vfs_is_dir(n)) strcpy(response, "Error: Invalid path");
        else {
            response[0] = 0;
            char names[64][VFS_NAME];
            /* iterative deep walk with indentation */
            struct { struct vfs_node *n; int depth; } stack[16];
            int sp = 0;
            stack[sp].n = n; stack[sp].depth = 0; sp++;
            while (sp && (int)strlen(response) < 1200) {
                sp--;
                struct vfs_node *cur = stack[sp].n;
                int d = stack[sp].depth;
                int c = vfs_list(cur, names, 64);
                for (int i = c - 1; i >= 0; i--) {
                    struct vfs_node *ch = vfs_child(cur, names[i]);
                    if (sp < 16 && ch && ch->is_dir) { stack[sp].n = ch; stack[sp].depth = d + 1; sp++; }
                }
                for (int i = 0; i < c; i++) {
                    for (int k = 0; k < d; k++) strcat(response, "  ");
                    strcat(response, names[i]);
                    struct vfs_node *ch = vfs_child(cur, names[i]);
                    if (ch && ch->is_dir) strcat(response, "/");
                    strcat(response, "\n");
                }
            }
        }
    }
    else if (!strcmp(cmd, "uptime")) {
        u32 up = uptime_ms() / 1000;
        char n[12];
        strcpy(response, "up ");
        fmt_u32(n, up / 3600); strcat(response, n); strcat(response, "h ");
        fmt_u32(n, (up / 60) % 60); strcat(response, n); strcat(response, "m ");
        fmt_u32(n, up % 60); strcat(response, n); strcat(response, "s");
    }
    else if (!strcmp(cmd, "free")) {
        u64 tot = 0, fre = 0;
        mm_stats(&tot, &fre);
        char n[16];
        strcpy(response, "total: ");
        fmt_u64(n, tot); strcat(response, n); strcat(response, " KB\nfree:  ");
        fmt_u64(n, fre); strcat(response, n); strcat(response, " KB\nused:  ");
        fmt_u64(n, tot - fre); strcat(response, n); strcat(response, " KB");
    }
    else if (!strcmp(cmd, "cpu")) {
        char cpu[49];
        cpu_brand(cpu, sizeof(cpu));
        strcpy(response, cpu[0] ? cpu : "x86 processor (no CPUID brand string)");
        char n[16];
        strcat(response, "\nTSC:     ");
        fmt_u32(n, cpu_mhz()); strcat(response, n); strcat(response, " MHz (startup TSC reference)");
        strcat(response, "\nload:    ");
        fmt_u32(n, cpu_usage_pct()); strcat(response, n); strcat(response, "% (TSC idle accounting)");
        strcat(response, "\nmode:    Linux user process, 100 Hz tick");
    }
    else if (!strcmp(cmd, "df")) {
        char n[16];
        strcpy(response, "vfs:   ");
        fmt_u32(n, vfs_usage_bytes()); strcat(response, n); strcat(response, " bytes used (in memory)\n");
        strcat(response, "disk:  ");
        if (fs_image_available()) { strcat(response, "snapshot file: "); strcat(response, fs_image_target()); strcat(response, " ('save' writes it)"); }
        else strcat(response, "no writable snapshot location");
    }
    else if (!strcmp(cmd, "disks")) {
        if (fs_image_available()) { strcpy(response, "snapshot: "); strcat(response, fs_image_target()); }
        else strcpy(response, "No writable snapshot location (RAM-only session).");
    }
    else if (!strcmp(cmd, "theme")) {
        if (nargs < 2) {
            strcpy(response, "Current theme: ");
            strcat(response, theme_current()->name);
            strcat(response, "\nAvailable:");
            for (int i = 0; i < theme_count(); i++) {
                strcat(response, "\n  "); strcat(response, theme_get(i)->id);
                strcat(response, "  - "); strcat(response, theme_get(i)->name);
            }
            strcat(response, "\nUsage: theme <id>");
        } else {
            int found = -1;
            for (int i = 0; i < theme_count(); i++)
                if (!strcmp(theme_get(i)->id, args[1])) found = i;
            if (found < 0) { strcpy(response, "Unknown theme: "); strcat(response, args[1]); }
            else { theme_set_index(found); wm_theme_changed(); settings_save();
                   strcpy(response, "Theme switched to "); strcat(response, theme_current()->name); }
        }
    }
    else if (!strcmp(cmd, "cal")) {
        struct rtc_time rt;
        rtc_read(&rt);
        response[0] = 0;
        static const char *mn[] = { "January", "February", "March", "April", "May", "June",
            "July", "August", "September", "October", "November", "December" };
        strcat(response, mn[rt.mon - 1]); strcat(response, " ");
        char n[8];
        fmt_u32(n, (u32)rt.year); strcat(response, n); strcat(response, "\n");
        strcat(response, "Su Mo Tu We Th Fr Sa\n");
        static const u8 md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int days = md[rt.mon - 1];
        if (rt.mon == 2 && ((rt.year % 4 == 0 && rt.year % 100 != 0) || rt.year % 400 == 0)) days = 29;
        /* weekday of day 1 from rtc weekday of today */
        int wd1 = (rt.weekday - (rt.day - 1) % 7 + 7) % 7;
        for (int i = 0; i < wd1; i++) strcat(response, "   ");
        for (int d = 1; d <= days; d++) {
            char cell[8];
            if (d == rt.day) { cell[0] = '['; fmt_u32(cell + 1, (u32)d);
                strcat(cell, "]"); }
            else { fmt_u32(cell, (u32)d); if (d < 10) { cell[1] = cell[0]; cell[0] = ' '; cell[2] = 0; } }
            strcat(response, cell);
            strcat(response, (d == rt.day) ? "" : " ");
            if ((wd1 + d) % 7 == 0) strcat(response, "\n");
        }
    }
    else if (!strcmp(cmd, "neofetch")) {
        char cpu[49];
        cpu_brand(cpu, sizeof(cpu));
        if (!cpu[0]) strcpy(cpu, "unknown x86 processor");
        u64 tot = 0, fre = 0;
        mm_stats(&tot, &fre);
        char n[16];
        char info[14][64];
        int ni = 0;
        strcpy(info[ni++], "user@scos");
        strcpy(info[ni++], "---------------------");
        strcpy(info[ni++], "OS:      SCos x64 development (build " SCOS_BUILD_TAG ")");
        info_set(info[ni++], (int)sizeof(info[0]), "CPU:     ", cpu);
        strcpy(info[ni],   "Speed:   ");
        fmt_u32(n, cpu_mhz()); strcat(info[ni], n); strcat(info[ni], " MHz (TSC reference)"); ni++;
        strcpy(info[ni], "         ");
        fmt_u32(n, cpu_core_count()); strcat(info[ni], n); strcat(info[ni], " core(s), ");
        fmt_u32(n, cpu_thread_count()); strcat(info[ni], n); strcat(info[ni], " thread(s) (CPUID)"); ni++;
        strcpy(info[ni],   "Load:    ");
        fmt_u32(n, cpu_usage_pct()); strcat(info[ni], n); strcat(info[ni], "% (idle-time meter)"); ni++;
        strcpy(info[ni],   "Memory:  ");
        fmt_u64(n, (tot - fre) / 1024); strcat(info[ni], n);
        strcat(info[ni], " / "); fmt_u64(n, tot / 1024); strcat(info[ni], n);
        strcat(info[ni], " MB"); ni++;
        { const char *m = ata_model();
          info_set(info[ni++], (int)sizeof(info[0]), "Disk:    ",
                   m && m[0] ? m : "none"); }
        strcpy(info[ni],   "Video:   ");
        fmt_u32(n, (u32)screen_w); strcat(info[ni], n); strcat(info[ni], "x");
        fmt_u32(n, (u32)screen_h); strcat(info[ni], n); strcat(info[ni], "x");
        fmt_u32(n, fb_bpp()); strcat(info[ni], n); ni++;
        strcpy(info[ni],   "Uptime:  ");
        fmt_u32(n, uptime_ms() / 1000); strcat(info[ni], n); strcat(info[ni], " s"); ni++;
        info_set(info[ni++], (int)sizeof(info[0]), "Theme:   ",
                 theme_current()->name);
        strcpy(info[ni++], "Shell:   scos-sh");
        strcpy(info[ni++], "WM:      scwm (GOP framebuffer compositor)");
        response[0] = 0;
        int rows = neofetch_art_lines > ni ? neofetch_art_lines : ni;
        for (int i = 0; i < rows; i++) {
            char line[128];
            const char *a = i < neofetch_art_lines ? neofetch_art[i] : "";
            strcpy(line, a);
            int l = (int)strlen(line);
            while (l < neofetch_art_width + 2) line[l++] = ' ';
            line[l] = 0;
            if (i < ni) strcat(line, info[i]);
            strcat(response, line);
            strcat(response, "\n");
        }
        response[strlen(response) - 1] = 0;
    }
    else if (!strcmp(cmd, "panic")) {
        /* was listed in help since v1 but never implemented - now real */
        term_print(t, "Triggering kernel panic as requested...");
        sleep_ms(600);
        kernel_panic("panic requested from terminal");
        return;
    }
    else if (!strcmp(cmd, "make")) {
        /* inside joke from the os.html days - deliberately not in help */
        if (nargs == 2 && !strcmp(args[1], "real")) strcpy(response, "real!!!");
        else strcpy(response, "make: nothing to be done");
    }
    else if (!strcmp(cmd, "tab")) {
        struct termwin *tw = TW(t->win);
        if (nargs < 2) {
            char n[8];
            strcpy(response, "tabs: ");
            fmt_u32(n, (u32)tw->ntabs); strcat(response, n);
            strcat(response, ", active #");
            fmt_u32(n, (u32)(tw->active + 1)); strcat(response, n);
            strcat(response, "\nusage: tab new | tab close | tab <n>");
        } else if (!strcmp(args[1], "new")) {
            term_new_tab(t->win);
            return;
        } else if (!strcmp(args[1], "close")) {
            if (tw->ntabs <= 1) strcpy(response, "cannot close the last tab");
            else {
                term_close_tab(t->win, tw->active);
                term_print(WT(t->win), "tab closed");
                wm_redraw(t->win);
                return;              /* this command's own tab may be gone */
            }
        } else {
            int n2 = (int)str_to_u32(args[1]);
            if (n2 >= 1 && n2 <= tw->ntabs) { tw->active = n2 - 1; return; }
            strcpy(response, "no such tab: ");
            strncat(response, args[1], 8);
        }
    }
    else if (!strcmp(cmd, "apps")) {
        lua_apps_refresh();
        strcpy(response, "ID          TITLE                 RUNNING\n");
        for (int i2 = 0; i2 < app_count(); i2++) {
            struct app *a = app_at(i2);
            if (!a || a->id[0] == '_') continue;
            char row[80];
            strcpy(row, a->id);
            while ((int)strlen(row) < 12) strcat(row, " ");
            strncat(row, a->title, 21);
            while ((int)strlen(row) < 34) strcat(row, " ");
            char n[8];
            fmt_u32(n, (u32)wm_app_running(a->id));
            strcat(row, n); strcat(row, "\n");
            if (strlen(response)+strlen(row) >= sizeof(response)-40) {
                strcat(response,"... use launcher search for more\n");break;
            }
            strcat(response,row);
        }
        response[strlen(response) - 1] = 0;
    }
    else if (!strcmp(cmd, "procs")) {
        strcpy(response, "PID  NAME        TYPE    STATE      MEM\n");
        for (int i2 = 0; i2 < proc_sys_count(); i2++) {
            char row[96], n[16];
            fmt_u32(n, (u32)i2);
            strcpy(row, n);
            while ((int)strlen(row) < 5) strcat(row, " ");
            strcat(row, proc_sys_name(i2));
            while ((int)strlen(row) < 16) strcat(row, " ");
            strcat(row, "system  running  ");
            u64 kb = 0;
            int show = 0;
            if (i2 == 0) { kb = proc_kernel_mem_kb(); show = 1; }
            else if (i2 == 2) { kb = proc_wm_mem_kb(); show = 1; }
            else if (i2 == 3) { kb = heap_owner_bytes(HEAP_VFS) / 1024; show = 1; }
            if (show) { fmt_u64(n, kb); strcat(row, n); strcat(row, " KB"); }
            else strcat(row, "-");
            strcat(row, "\n");
            if (strlen(response) + strlen(row) < sizeof(response) - 64)
                strcat(response, row);
        }
        int nw = wm_win_count();
        for (int i2 = 0; i2 < nw && i2 < 20; i2++) {
            struct window *aw = wm_win_at(i2);
            char row[112], n[16];
            fmt_u32(n, (u32)(10 + i2));
            strcpy(row, n);
            while ((int)strlen(row) < 5) strcat(row, " ");
            strncat(row, aw->app ? aw->app->id : "?", 11);
            while ((int)strlen(row) < 16) strcat(row, " ");
            strcat(row, "app     ");
            strcat(row, aw->state == WIN_STATE_MIN ? "minimized  "
                                                   : "running    ");
            fmt_u64(n, proc_win_mem_kb(aw));
            strcat(row, n); strcat(row, " KB\n");
            if (strlen(response) + strlen(row) < sizeof(response) - 64)
                strcat(response, row);
        }
        if (nw > 20 && strlen(response) < sizeof(response) - 40)
            strcat(response, "(more windows - see sysmon)\n");
        response[strlen(response) - 1] = 0;
    }

    else if (!strcmp(cmd, "kill")) {
        int pid = -1;
        int system = nargs == 3 && !strcmp(args[1], "--system");
        if (nargs == 2 && !strcmp(args[1], "--help"))
            strcpy(response,"kill <pid> | kill --system <pid> (always asks y/n). Only scwm (2) supports a real system stop.");
        else if ((!system && nargs != 2) || !parse_pid(args[system ? 2 : 1], &pid))
            strcpy(response,"Usage: kill <pid> | kill --system <pid>; PID must be decimal.");
        else if (system) {
            if (pid == 2) { wm_stop_requested = 1; strcpy(response,"Terminating scoswm and its applications."); }
            else strcpy(response,"This subsystem has no independent stop operation; nothing was terminated.");
        }
        else {
            if (pid >= 0 && pid < proc_sys_count()) {
                strcpy(response, "cannot kill system task ");
                strcat(response, proc_sys_name(pid));
            } else if (pid >= 10 && pid - 10 < wm_win_count()) {
                struct window *v = wm_win_at(pid - 10);
                if (v == t->win)
                    strcpy(response, "refusing to kill this terminal - "
                                     "close the window instead");
                else if (v->app && v->app->id[0] == '_')
                    strcpy(response, "cannot kill a system dialog");
                else {
                    strcpy(response, "terminated ");
                    strcat(response, v->app ? v->app->id : "?");
                    strcat(response, " (pid ");
                    char n[8];
                    fmt_u32(n, (u32)pid);
                    strcat(response, n);
                    strcat(response, ")");
                    wm_close_window(v);
                }
            } else {
                strcpy(response, "no such pid: ");
                strncat(response, args[1], 8);
            }
        }
    }
    else {
        strcpy(response, "Command not found: "); strcat(response, cmd);
        strcat(response, ". Type 'help' for available commands.");
    }

    if (response[0]) {
        if (typed) term_type(t, response);
        else term_print(t, response);
    }
}

/* ------------------------------------------------------------- app glue -- */
static void term_init_shell(struct term *t, struct window *w, const char *banner)
{
    memset(t, 0, sizeof(*t));
    strcpy(t->cwd, "/");
    t->scroll = -1;
    t->hindex = 0;
    t->win = w;
    if (banner) term_print(t, banner);
    term_print(t, "Type 'help' for commands.  Tabs: [+] or Ctrl+T, cycle "
                  "Ctrl+Tab, close Ctrl+W.");
}

static void term_open(struct window *w, void *arg)
{
    (void)arg;
    struct termwin *tw = palloc(sizeof(*tw));
    if (!tw) return;             /* OOM: wm_open_app reports it centrally */
    memset(tw, 0, sizeof(*tw));
    struct term *t = palloc(sizeof(struct term));
    if (!t) { pfree(tw, sizeof(*tw)); return; }
    w->data = tw;
    term_init_shell(t, w, "SCos Terminal - x64-dev");
    tw->tabs[0] = t;
    tw->ntabs = 1;
    tw->active = 0;
    /* real attribution so sysmon/procs show this window's true footprint */
    wm_track_mem(w, (int)(sizeof(*tw) + sizeof(struct term)));
}

static void term_close(struct window *w)
{
    struct termwin *tw = TW(w);
    if (tw) {
        for (int i = 0; i < tw->ntabs; i++) {
            /* apps launched from this tab must not log into freed memory */
            wm_clear_console(tw->tabs[i]);
            pfree(tw->tabs[i], sizeof(struct term));
        }
        pfree(tw, sizeof(*tw));
    }
    w->data = NULL;
}

static void term_new_tab(struct window *w)
{
    struct termwin *tw = TW(w);
    if (tw->ntabs >= TERM_MAX_TABS) {
        term_print(WT(w), "tab limit reached (6 per window) - close one first");
        wm_redraw(w);
        return;
    }
    struct term *t = palloc(sizeof(struct term));
    if (!t) {
        term_print(WT(w), "out of memory - cannot open a new tab");
        wm_redraw(w);
        return;
    }
    char banner[48];
    strcpy(banner, "SCos Terminal - x64-dev - tab ");
    char n[4];
    fmt_u32(n, (u32)(tw->ntabs + 1));
    strcat(banner, n);
    term_init_shell(t, w, banner);
    tw->tabs[tw->ntabs++] = t;
    tw->active = tw->ntabs - 1;
    wm_track_mem(w, (int)sizeof(struct term));
    wm_redraw(w);
}

static void term_close_tab(struct window *w, int i)
{
    struct termwin *tw = TW(w);
    if (i < 0 || i >= tw->ntabs || tw->ntabs <= 1) return;
    struct term *t = tw->tabs[i];
    wm_clear_console(t);
    wm_track_mem(w, -(int)sizeof(struct term));
    pfree(t, sizeof(struct term));
    for (int k = i; k < tw->ntabs - 1; k++) tw->tabs[k] = tw->tabs[k + 1];
    tw->ntabs--;
    if (tw->active >= tw->ntabs) tw->active = tw->ntabs - 1;
    else if (tw->active > i) tw->active--;
    wm_redraw(w);
}

/* console sink for apps launched with `appstrt` (called by the WM) */
void term_console_line(void *term, const char *line)
{
    struct term *t = (struct term *)term;
    if (!t || !t->win || !TW(t->win)) return;
    term_push_line(t, line);
    if (WT(t->win) == t) wm_redraw(t->win);
}

void term_console_exit(void *term, const char *app_id)
{
    char buf[96];
    strcpy(buf, "[");
    strncat(buf, app_id ? app_id : "app", 24);
    strcat(buf, "] exited");
    term_console_line(term, buf);
}

static int term_visible_rows(struct window *w)
{
    return (wm_content_h(w) - 24 - TERM_TAB_H) / (FONT_H + 2);
}

/* How many characters a row of this window holds, and the row count that goes with them. */
static int term_cols(struct window *w)
{
    return s_text_cols(w->surf.w - 12);
}

static void term_rows_sync(struct term *t, int cols)
{
    if (!t->rows_dirty && t->rows_cols == cols) return;
    t->rows_dirty = 0;
    t->rows_cols = cols;
    t->total_rows = 0;
    for (int i = 0; i < t->nlines; i++) {
        int n = s_wrap_rows(t->lines[i], cols);
        /* An empty entry still occupies a row: `echo ""' prints a blank line, and a terminal that ate it
         * would make the output above and below it look adjacent. */
        if (n < 1) n = 1;
        t->lrows[i] = (short)n;
        t->total_rows += n;
    }
}

static int term_display_rows(struct term *t, struct window *w)
{
    term_rows_sync(t, term_cols(w));
    return t->total_rows + 1;                 /* +1 for the prompt row, which never wraps */
}

/* The `index'th wrapped row of `text', painted at (x,y).  Each row is found by walking the line from its
 * start instead of carrying a cursor between rows, so painting stays idempotent under resize, scroll and
 * partial damage - three things that change independently here. */
static void term_paint_row(struct surface *s, const char *text, int cols, int index, u32 color,
                           int x, int y)
{
    char row[TERM_LINE];
    const char *p = text;

    row[0] = 0;
    for (int i = 0; i <= index && p; i++) p = s_wrap_row(p, cols, row, sizeof row);
    if (row[0]) s_text(s, x, y, row, color);
}

static void ed_clamp_cursor(struct term *t)
{
    if (t->ed_row < 0) t->ed_row = 0;
    if (t->ed_row > t->ed_lines - 1) t->ed_row = t->ed_lines - 1;
    int ln = (int)strlen(t->ed_buf[t->ed_row]);
    if (t->ed_col > ln) t->ed_col = ln;
    if (t->ed_col < 0) t->ed_col = 0;
}

static void ed_save(struct term *t)
{
    char out[128 * 97];
    int o = 0;
    for (int i = 0; i < t->ed_lines; i++) {
        int ln = (int)strlen(t->ed_buf[i]);
        memcpy(out + o, t->ed_buf[i], ln);
        o += ln;
        out[o++] = '\n';
    }
    if(vfs_write(t->ed_path,out,(u32)o))t->ed_modified=0;
    else err_notify("editor","Save failed. Buffer remains modified; check path and memory.",NULL,0);
}

static void ed_key(struct window *w, struct key_event *e)
{
    struct term *t = WT(w);
    if (!e->pressed) return;
    char c = (char)e->keycode;
    if (e->ctrl && (c == 15 || c == 'o' || c == 'O')) { ed_save(t); wm_redraw(w); return; }
    if (e->ctrl && (c == 24 || c == 'x' || c == 'X')) {
        t->edit_mode = 0;
        char m[224];
        strcpy(m, "[editor] closed ");
        strcat(m, t->ed_path);
        term_push_line(t, m);
        wm_redraw(w);
        return;
    }
    char *line = t->ed_buf[t->ed_row];
    int ln = (int)strlen(line);
    switch (e->keycode) {
    case KEY_UP:    t->ed_row--; break;
    case KEY_DOWN:  t->ed_row++; break;
    case KEY_LEFT:
        if (t->ed_col) t->ed_col--;
        else if (t->ed_row) { t->ed_row--; t->ed_col = (int)strlen(t->ed_buf[t->ed_row]); }
        break;
    case KEY_RIGHT:
        if (t->ed_col < ln) t->ed_col++;
        else if (t->ed_row < t->ed_lines - 1) { t->ed_row++; t->ed_col = 0; }
        break;
    case KEY_HOME:  t->ed_col = 0; break;
    case KEY_END:   t->ed_col = ln; break;
    case KEY_PGUP:  t->ed_row -= 12; break;
    case KEY_PGDN:  t->ed_row += 12; break;
    case 8: case 127:
        if (t->ed_col) {
            memmove(line + t->ed_col - 1, line + t->ed_col, ln - t->ed_col + 1);
            t->ed_col--;
            t->ed_modified = 1;
        } else if (t->ed_row) {
            int plen = (int)strlen(t->ed_buf[t->ed_row - 1]);
            if (plen + ln < 96) {
                memcpy(t->ed_buf[t->ed_row - 1] + plen, line, ln + 1);
                for (int i = t->ed_row; i < t->ed_lines - 1; i++)
                    memcpy(t->ed_buf[i], t->ed_buf[i + 1], sizeof(t->ed_buf[i]));
                t->ed_lines--;
                t->ed_row--;
                t->ed_col = plen;
                t->ed_modified = 1;
            }
        }
        break;
    case 10: case 13:
        if (t->ed_lines < 128) {
            for (int i = t->ed_lines; i > t->ed_row + 1; i--)
                memcpy(t->ed_buf[i], t->ed_buf[i - 1], sizeof(t->ed_buf[i]));
            int tail = ln - t->ed_col;
            memcpy(t->ed_buf[t->ed_row + 1], line + t->ed_col, tail);
            t->ed_buf[t->ed_row + 1][tail] = 0;
            line[t->ed_col] = 0;
            t->ed_lines++;
            t->ed_row++;
            t->ed_col = 0;
            t->ed_modified = 1;
        }
        break;
    default:
        if (c >= 32 && c < 127 && ln < 95) {
            memmove(line + t->ed_col + 1, line + t->ed_col, ln - t->ed_col + 1);
            line[t->ed_col++] = c;
            t->ed_modified = 1;
        }
        break;
    }
    ed_clamp_cursor(t);
    int rows = term_visible_rows(w) - 2;
    if (rows < 4) rows = 4;
    if (t->ed_row < t->ed_scroll) t->ed_scroll = t->ed_row;
    if (t->ed_row >= t->ed_scroll + rows) t->ed_scroll = t->ed_row - rows + 1;
    wm_redraw(w);
}

static void term_paint_editor(struct window *w)
{
    struct term *t = WT(w);
    struct surface *s = &w->surf;
    const struct theme *th = theme_current();
    s_fill(s, 0, 0, s->w, s->h, th->win_bg);
    s_fill(s, 0, 0, s->w, 20, th->taskbar_bg);
    char hdr[160];
    strcpy(hdr, "  edit: ");
    strcat(hdr, t->ed_path);
    if (t->ed_modified) strcat(hdr, "  [modified]");
    s_clip_text(s, 0, 3, hdr, th->main, s->w - 8);
    int rows = (s->h - 24 - 20) / (FONT_H + 2);
    for (int i = 0; i < rows; i++) {
        int r = t->ed_scroll + i;
        int y = 24 + i * (FONT_H + 2);
        if (r < t->ed_lines)
            s_clip_text(s, 6, y, t->ed_buf[r], th->text, s->w - 12);
        else
            s_text(s, 6, y, "~", color_blend(th->text,th->win_bg,55));
    }
    int cy = 24 + (t->ed_row - t->ed_scroll) * (FONT_H + 2);
    int cx = 6 + t->ed_col * FONT_W;
    char under = (t->ed_col < (int)strlen(t->ed_buf[t->ed_row]))
                 ? t->ed_buf[t->ed_row][t->ed_col] : ' ';
    s_fill(s, cx, cy, FONT_W, FONT_H, th->main);
    char one[2] = { under, 0 };
    s_text(s, cx, cy, one, th->title_text);
    s_fill(s, 0, s->h - 18, s->w, 18, th->taskbar_bg);
    s_text(s, 6, s->h - 15, "^O save   ^X exit   arrows move   type to insert",
           th->text);
}

static void term_paint_tabstrip(struct window *w)
{
    struct termwin *tw = TW(w);
    struct surface *s = &w->surf;
    const struct theme *th = theme_current();
    s_fill(s, 0, 0, s->w, TERM_TAB_H, th->taskbar_bg);
    int x = 4;
    for (int i = 0; i < tw->ntabs && x + TAB_BTN_W <= s->w - 8; i++) {
        int act = (i == tw->active);
        s_fill(s, x, 2, TAB_BTN_W - 3, TERM_TAB_H - 4, act ? th->win_bg : color_blend(th->win_bg,th->main,12));
        s_frame_rect(s, x, 2, TAB_BTN_W - 3, TERM_TAB_H - 4,
                     act ? th->main : th->main);
        char lbl[12];
        fmt_u32(lbl, (u32)(i + 1));
        strcat(lbl, ":term");
        s_text(s, x + 5, 5, lbl, act ? th->main : th->text);
        s_text(s, x + TAB_BTN_W - 16, 5, "x", th->text);
        x += TAB_BTN_W;
    }
    if (x + 26 <= s->w - 4) {
        s_fill(s, x, 2, 24, TERM_TAB_H - 4, color_blend(th->win_bg,th->main,12));
        s_frame_rect(s, x, 2, 24, TERM_TAB_H - 4, th->main);
        s_text(s, x + 7, 4, "+", th->main);
    }
}

static void term_paint(struct window *w)
{
    if (WT(w)->edit_mode) { term_paint_editor(w); return; }
    struct term *t = WT(w);
    struct surface *s = &w->surf;
    const struct theme *th = theme_current();
    s_fill(s, 0, 0, s->w, s->h, th->win_bg);
    term_paint_tabstrip(w);      /* after the background clear */

    int rows = term_visible_rows(w);
    /* pending typewriter text may span several lines */
    char pbuf[512];
    int plines = 0;
    if (t->pending_active) {
        int n = t->pend_shown;
        if (n > t->pend_len) n = t->pend_len;
        if (n > (int)sizeof(pbuf) - 1) n = sizeof(pbuf) - 1;
        memcpy(pbuf, t->pending, n);
        pbuf[n] = 0;
        plines = 1;
        for (int i = 0; i < n; i++) if (pbuf[i] == '\n') plines++;
    }
    int cols = term_cols(w);
    term_rows_sync(t, cols);
    int total = t->total_rows + (t->pending_active ? s_wrap_rows(pbuf, cols) : 0) + 1;
    if (t->scroll < 0 || t->scroll > total - rows) t->scroll = total - rows;
    if (t->scroll < 0) t->scroll = 0;
    t->follow = (t->scroll >= total - rows);

    int y = TERM_TAB_H + 4;
    int li = 0, line_base = 0;             /* display row where t->lines[li] begins */
    while (li < t->nlines && line_base + t->lrows[li] <= t->scroll) {
        line_base += t->lrows[li];
        li++;
    }
    for (int r = t->scroll; r < total && r < t->scroll + rows; r++, y += FONT_H + 2) {
        if (li < t->nlines) {
            term_paint_row(s, t->lines[li], cols, r - line_base, th->text, 6, y);
            if (r + 1 >= line_base + t->lrows[li]) {
                line_base += t->lrows[li];
                li++;
            }
        } else if (t->pending_active && r < t->total_rows + plines) {
            term_paint_row(s, pbuf, cols, r - t->total_rows, th->main, 6, y);
        } else {
            char prompt[160];
            prompt_str(t, prompt);
            int prompt_width=s_text_width(prompt),limit=((s->w-80)/2/FONT_W)*FONT_W;if(prompt_width>limit)prompt_width=limit;
            s_clip_text(s,6,y,prompt,th->main,prompt_width);
            int px=6+prompt_width;
            int cols = (s->w - px - 8) / FONT_W;
            int start = cols > 0 && t->ipos >= cols ? t->ipos - cols + 1 : 0;
            s_clip_text(s, px, y, t->input + start, th->main, s->w - px - 8);
            /* Keep the insertion cell visible when a command exceeds the row. */
            int cx = px + (t->ipos - start) * FONT_W;
            if (cx >= 0 && cx + FONT_W <= s->w - 6)
                s_fill(s, cx, y + FONT_H - 2, FONT_W, 2, th->main);
        }
    }
}

static void term_key(struct window *w, struct key_event *e)
{
    if (WT(w)->edit_mode) { ed_key(w, e); return; }
    struct term *t = WT(w);
    if (!e->pressed) return;
    if (t->confirmation.command[0] && (e->keycode == 3 || e->keycode == 27)) {
        t->confirmation.command[0]=0; t->input[0]=0; t->ipos=0;
        term_print(t,"Cancelled."); wm_redraw(w); return;
    }
    /* tab shortcuts (before line editing swallows the keys) */
    if (e->ctrl) {
        struct termwin *tw = TW(w);
        if (e->keycode == 20 || e->keycode == 't' || e->keycode == 'T') { term_new_tab(w); return; }
        if ((e->keycode == 23 || e->keycode == 'w' || e->keycode == 'W') && tw->ntabs > 1) {
            term_close_tab(w, tw->active);
            return;
        }
        if (e->keycode == '\t') {
            tw->active = e->shift
                ? (tw->active + tw->ntabs - 1) % tw->ntabs
                : (tw->active + 1) % tw->ntabs;
            wm_redraw(w);
            return;
        }
    }
    /* never swallow keystrokes: finish any typewriter output instantly */
    if (t->pending_active) {
        term_push_line(t, t->pending);
        t->pending_active = 0;
    }

    if (e->keycode == '\n') {
        char cmd[TERM_LINE];
        strcpy(cmd, t->input);
        t->input[0] = 0;
        t->ipos = 0;
        run_command(t, cmd);
        wm_redraw(w);          /* commands with empty output (edit, clear...)
                                * still change what must be on screen */
        return;
    }
    /* r36: keyboard scrollback - matches every terminal the user knows
     * and hedges mice whose reports never carry a wheel byte */
    if (e->keycode == KEY_PGUP || e->keycode == KEY_PGDN) {
        int rows = term_visible_rows(w);
        int total = term_display_rows(t, w);
        t->scroll += (e->keycode == KEY_PGUP) ? -(rows - 1) : (rows - 1);
        if (t->scroll > total - rows) t->scroll = total - rows;
        if (t->scroll < 0) t->scroll = 0;
        t->follow = (t->scroll >= total - rows);
        wm_redraw(w);
        return;
    }
    if (e->keycode == KEY_UP) {
        if (t->hcount && t->hindex > 0) {
            t->hindex--;
            strcpy(t->input, t->hist[t->hindex]);
            t->ipos = (int)strlen(t->input);
        }
        wm_redraw(w);        /* r36: recalled history must SHOW */
        return;
    }
    if (e->keycode == KEY_DOWN) {
        if (t->hindex < t->hcount - 1) {
            t->hindex++;
            strcpy(t->input, t->hist[t->hindex]);
        } else { t->hindex = t->hcount; t->input[0] = 0; }
        t->ipos = (int)strlen(t->input);
        wm_redraw(w);        /* r36: recalled history must SHOW */
        return;
    }
    edit_line(t->input, &t->ipos, sizeof(t->input), e);
    /* r36 ECHO ROOT FIX: typed characters landed in the line buffer but
     * NOTHING repainted the window - the field saw "text I type is
     * invisible until I press Enter" because Enter was the only path
     * that called wm_redraw.  Every line-editing key now redraws. */
    wm_redraw(w);
}

static void term_mouse(struct window *w, struct mouse_event *e, int x, int y)
{
    struct termwin *tw = TW(w);
    /* tab strip: click selects, [x] closes, [+] opens (not in editor mode -
     * the nano-style editor owns the whole surface like real nano) */
    if (e->type == MEV_BUTTON && e->down && e->button == MBTN_LEFT &&
        y < TERM_TAB_H && !WT(w)->edit_mode) {
        if (x >= 4) {
            int i = (x - 4) / TAB_BTN_W;
            int inx = (x - 4) % TAB_BTN_W;
            if (i < tw->ntabs && inx < TAB_BTN_W - 3) {
                if (inx >= TAB_BTN_W - 18) term_close_tab(w, i);
                else { tw->active = i; wm_redraw(w); }
                return;
            }
            int px = 4 + tw->ntabs * TAB_BTN_W;
            if (x >= px && x <= px + 24) { term_new_tab(w); return; }
        }
        return;
    }
    struct term *t = WT(w);
    if (e->type == MEV_WHEEL) {
        int rows = term_visible_rows(w);
        t->scroll -= e->wheel;
        if (t->scroll < 0) t->scroll = 0;
        int total = term_display_rows(t, w);
        if (t->scroll > total - rows) t->scroll = total - rows;
        if (t->scroll < 0) t->scroll = 0;
        t->follow = (t->scroll >= total - rows);
    }
}

static void term_tick(struct window *w)
{
    struct term *t = WT(w);
    if (t->pending_active) {
        t->pend_shown += 40;                     /* typewriter speed */
        if (t->pend_shown >= t->pend_len) {
            term_push_line(t, t->pending);
            t->pending_active = 0;
        }
        wm_redraw(w);
    }
    if (t->shutting_down == 1 || t->shutting_down == 2) {
        /* Linux: shutdown/reboot end the scoswm session (see PORTING.md);
         * they never power off or reboot the host. */
        sleep_ms(1200);
        platform_request_quit();
        t->shutting_down = 0;
    }
}

struct app app_terminal = {
    .desktop_label = "Terminal",
    .uses_data = 1, .id = "terminal", .title = "Terminal", .icon = ICON_TERMINAL, .single = 0,
    .def_w = 700, .def_h = 450,
    .open = term_open, .paint = term_paint, .key = term_key,
    .mouse = term_mouse, .tick = term_tick, .close = term_close,
};

SCOS_APP(app_terminal, 001);
