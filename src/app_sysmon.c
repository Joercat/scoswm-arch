/*
 * SCos native - System Monitor.
 *
 * Everything shown here is measured, not simulated:
 *   - CPU brand from CPUID (x86) or the host, speed host-reported,
 *   - CPU load from this process's CPU time over wall time,
 *   - memory from the kernel page allocator,
 *   - the task table is the live window/app table of the WM plus the
 *     always-present kernel services; "End Task" really closes the window.
 */
#include "scos.h"

/* r27: the service table with proper names - every entry is a subsystem
 * that really exists and really runs on this machine:
 *   sckern  the kernel core (mm, idt, panic, drivers' home)
 *   intsck  init: the boot path that starts every service, then the WM
 *   scwm    window compositor (owns the framebuffer + wallpaper cache)
 *   vfsd    RAM filesystem
 *   inputd  input server: host event queues
 *   pitclk  100 Hz tick clock, uptime and CPU-load accounting */
#define SYS_TASKS 6
static const char *sys_names[SYS_TASKS] = {
    "sckern", "intsck", "scwm", "vfsd", "inputd", "pitclk",
};

int proc_sys_count(void) { return SYS_TASKS; }
const char *proc_sys_name(int i)
{
    return (i >= 0 && i < SYS_TASKS) ? sys_names[i] : "?";
}

/* Allocator ownership includes page rounding, allocation headers, surfaces,
 * app state and dynamically added tabs/documents. Static state stays in sckern. */
u64 proc_win_mem_kb(struct window *w){return heap_owner_bytes((uintptr_t)w)/1024;}
u64 proc_wm_mem_kb(void){return heap_owner_bytes(HEAP_WM)/1024;}
u64 proc_kernel_mem_kb(void){return memory_reserved_pages()*4+(heap_owner_bytes(0)+usb_dma_bytes())/1024;}

struct smon {
    int sel;              /* selected row, -1 = none; >= SYS_TASKS = app */
    int hover_btn;
    int rows,top,visible,scroll,selected_id;
};

static void sm_tick(struct window *w)
{
    if ((tick_count % 50) == 0) wm_redraw(w);
}

static void sm_paint(struct window *w)
{
    struct smon *m = w->data;
    struct surface *s = &w->surf;
    const struct theme *t = theme_current();
    s_fill(s, 0, 0, s->w, s->h, t->win_bg);

    char line[128], n[16];
    int y = 10;
    s_text(s, 12, y, "CPU", t->main); y += 20;
    char cpu[49];
    cpu_brand(cpu, sizeof(cpu));
    strcpy(line, "  model:  "); strcat(line, cpu[0] ? cpu : "x86 processor (no brand leaf)");
    s_clip_text(s,12,y,line,t->text,s->w-24); y += 18;
    u32 sig = cpu_signature();
    u32 fam = (sig >> 8) & 0xF;
    if (fam == 0xF) fam += (sig >> 20) & 0xFF;
    u32 mod = (sig >> 4) & 0xF;
    if (fam == 6 || fam == 0xF) mod += ((sig >> 16) & 0xF) << 4;
    strcpy(line, "  cpuid:  family ");
    fmt_u32(n, fam); strcat(line, n);
    strcat(line, " model "); fmt_u32(n, mod); strcat(line, n);
    strcat(line, " stepping "); fmt_u32(n, sig & 0xF); strcat(line, n);
    s_clip_text(s,12,y,line,t->text,s->w-24); y += 18;
    strcpy(line, "  clock:  ");
    fmt_u32(n, cpu_mhz()); strcat(line, n); strcat(line, " MHz (host-reported)");
    s_clip_text(s,12,y,line,t->text,s->w-24);   /* r31: this row was built but never
                                        * drawn - the MHz line silently
                                        * vanished from the CPU panel */
    y += 18;
    strcpy(line, "cpu cores: ");
    fmt_u32(n, cpu_core_count()); strcat(line, n);
    strcat(line, "  threads: ");
    fmt_u32(n, cpu_thread_count()); strcat(line, n);
    strcat(line, " (host-reported)");
    s_clip_text(s,12,y,line,t->text,s->w-24); y += 18;
    u32 load = cpu_usage_pct();
    strcpy(line, "  load:   ");
    fmt_u32(n, load); strcat(line, n); strcat(line, "% (this process, 1s sample)");
    s_clip_text(s,12,y,line,t->text,s->w-24); y += 18;
    int bw = 300;
    s_frame_rect(s, 30, y, bw, 12, t->main);
    s_fill(s, 31, y + 1, (int)((bw - 2) * load) / 100, 10, t->main);
    y += 22;

    u64 tot = 0, fre = 0;
    mm_stats(&tot, &fre);
    s_text(s, 12, y, "Memory", t->main); y += 20;
    strcpy(line, "  used:   ");
    fmt_u64(n, (tot - fre) / 1024); strcat(line, n); strcat(line, " MB of ");
    fmt_u64(n, tot / 1024); strcat(line, n); strcat(line, " MB managed");
    s_clip_text(s,12,y,line,t->text,s->w-24); y += 18;
    s_frame_rect(s, 30, y, bw, 12, t->main);
    s_fill(s, 31, y + 1, (int)((u64)(bw - 2) * (tot - fre) / (tot ? tot : 1)), 10, t->main);
    y += 22;

    /* r31: live allocator counters ride on the Uptime row (no layout
     * shift): proof the heap numbers are REAL and moving - every window
     * open/resize/save bumps these, and allocs tracking frees is the
     * leak check the user can watch in real time. */
    u32 aops = 0, fops = 0;
    mm_ops(&aops, &fops);
    strcpy(line, "Uptime: ");
    fmt_u32(n, uptime_ms() / 1000); strcat(line, n); strcat(line, " s");
    strcat(line, "    heap: ");
    fmt_u32(n, aops); strcat(line, n); strcat(line, " allocs / ");
    fmt_u32(n, fops); strcat(line, n); strcat(line, " frees (live)");
    s_clip_text(s,12,y,line,t->text,s->w-24); y += 24;

    s_text(s, 12, y, "Tasks", t->main); y += 20;
    s_text(s, 16, y, "PID", t->text);
    s_text(s, 60, y, "NAME", t->text);
    s_text(s, 260, y, "TYPE", t->text);
    s_text(s, 360, y, "STATE", t->text);
    s_text(s, 470, y, "MEM", t->text);
    if (s->w >= 600) s_text(s, 560, y, "CPU", t->text);   /* r39 */
    y += FONT_H+4;
    s_fill(s, 12, y, s->w - 24, 1, t->main);
    y += 6;

    int nwin = wm_win_count();
    m->rows = SYS_TASKS + nwin;
    m->top=y;m->visible=(s->h-46-y)/18;
    if(m->visible<0)m->visible=0;
    if(m->selected_id){
        m->sel=-1;
        for(int i=0;i<nwin;i++)if(wm_win_at(i)->id==m->selected_id)m->sel=SYS_TASKS+i;
        if(m->sel<0)m->selected_id=0;
    }
    int maxscroll=m->rows-m->visible;if(maxscroll<0)maxscroll=0;
    if(m->scroll>maxscroll)m->scroll=maxscroll;
    if(m->scroll<0)m->scroll=0;
    for (int i = m->scroll; i < m->rows && i < m->scroll+m->visible; i++) {
        int rh = 18;
        u32 bg = 0;
        if (i == m->sel) bg = color_blend(t->win_bg, t->main, 30);
        else if (i >= SYS_TASKS && (i - SYS_TASKS) % 2) bg = color_blend(t->win_bg, t->main, 8);
        if (bg) s_fill(s, 12, y - 2, s->w - 24, rh, bg);
        if (i < SYS_TASKS) {
            fmt_u32(n, (u32)i);
            s_text(s, 16, y, n, t->text);
            s_text(s, 60, y, sys_names[i], t->text);
            s_text(s, 260, y, "system", ((t->main >> 1) & 0x7F7F7F));
            s_text(s, 360, y, "running", t->text);
            /* measured footprints where attribution is honest:
             * sckern = allocator remainder, scwm = framebuffers,
             * vfsd = live file data; the rest run inside sckern's
             * address space and have no separate allocation */
            char mb[24];
            u64 kb = 0;
            int show = 0;
            if (i == 0) { kb = proc_kernel_mem_kb(); show = 1; }
            else if (i == 2) { kb = proc_wm_mem_kb(); show = 1; }
            else if (i == 3) { kb = heap_owner_bytes(HEAP_VFS) / 1024; show = 1; }
            if (show) { fmt_u64(mb, kb); strcat(mb, " KB"); }
            else strcpy(mb, "-");
            s_text(s, 470, y, mb, t->text);
            /* r39: per-task CPU is honestly attributable to app windows
             * only (their callbacks are the instrumented code paths);
             * system rows show "-" rather than inventing a split. */
            if (s->w >= 600) s_text(s, 560, y, "-", t->text);
        } else {
            struct window *aw = wm_win_at(i - SYS_TASKS);
            fmt_u32(n, (u32)(10 + i - SYS_TASKS));
            s_text(s, 16, y, n, t->text);
            s_clip_text(s,60,y,aw->app?aw->app->id:"?",t->text,190);
            s_text(s, 260, y, "app", t->main);
            s_text(s, 360, y, aw->state == WIN_STATE_MIN ? "minimized" : "running", t->text);
            char mb[24];
            fmt_u64(mb, proc_win_mem_kb(aw));
            strcat(mb, " KB");
            s_text(s, 470, y, mb, t->text);
            if (s->w >= 600) {          /* r39: TSC-measured app CPU% */
                char cb[8];
                fmt_u32(cb, wm_win_cpu_pct(aw));
                strcat(cb, "%");
                s_text(s, 560, y, cb, t->text);
            }
        }
        y += rh;
    }

    /* End Task button */
    int by = s->h - 36;
    int enabled = m->sel >= SYS_TASKS;
    u32 bg = (m->hover_btn && enabled) ? t->main : color_blend(t->win_bg,t->main,15);
    u32 fg = (m->hover_btn && enabled) ? t->title_text : (enabled ? t->main : color_blend(t->text,t->win_bg,55));
    s_fill(s, 12, by, 110, 26, bg);
    s_frame_rect(s, 12, by, 110, 26, fg);
    s_text(s, 26, by + 5, "End Task", fg);
    s_clip_text(s, 140, by + 6, m->sel >= SYS_TASKS
             ? "click a task row, then End Task (system tasks cannot be ended)"
             : "select an app task to enable End Task",
           ((t->main >> 1) & 0x7F7F7F),s->w-152);
}

static void sm_mouse(struct window *w, struct mouse_event *e, int x, int y)
{
    struct smon *m = w->data;
    struct surface *s = &w->surf;
    int old_sel = m->sel, old_h = m->hover_btn;
    m->hover_btn = (x >= 12 && x < 122 && y >= s->h - 36 && y < s->h - 10);

    if(e->type==MEV_WHEEL){m->scroll-=e->wheel;wm_redraw(w);return;}
    int y0=m->top;
    if (e->type == MEV_BUTTON && e->down && e->button == MBTN_LEFT) {
        if (y >= y0 && y < y0 + m->visible * 18) {
            int row = m->scroll+(y - y0) / 18;
            if (row < m->rows) {m->sel=(m->sel==row)?-1:row;
                m->selected_id=m->sel>=SYS_TASKS?wm_win_at(m->sel-SYS_TASKS)->id:0;}
        } else if (m->hover_btn && m->sel >= SYS_TASKS) {
            struct window *victim = wm_win_at(m->sel - SYS_TASKS);
            if (victim && victim != w) {
                char lg[64];
                strcpy(lg, "ended task ");
                strncat(lg, victim->app ? victim->app->id : "?", 32);
                app_log(w, lg);
                wm_close_window(victim);
                m->sel = -1;m->selected_id=0;
            } else if (victim == w) {
                m->sel = -1;m->selected_id=0;   /* refusing to saw off our own branch */
            }
        } else if (m->hover_btn) {
            m->sel = -1;
        }
    }
    if (old_sel != m->sel || old_h != m->hover_btn) wm_redraw(w);
}

static void sm_key(struct window *w, struct key_event *e)
{
    struct smon *m = w->data;
    if (!e->pressed) return;
    if (e->keycode == KEY_UP && m->sel > 0) m->sel--;
    else if (e->keycode == KEY_DOWN && m->sel < m->rows - 1) m->sel++;
    else return;
    m->selected_id=m->sel>=SYS_TASKS?wm_win_at(m->sel-SYS_TASKS)->id:0;
    if(m->sel<m->scroll)m->scroll=m->sel;
    if(m->sel>=m->scroll+m->visible)m->scroll=m->sel-m->visible+1;
    wm_redraw(w);
}

static void sm_open(struct window *w, void *arg)
{
    (void)arg;
    struct smon *m = palloc(sizeof(*m));
    if (!m) return;              /* OOM: wm_open_app reports it centrally */
    memset(m, 0, sizeof(*m));
    m->sel = -1;
    m->hover_btn = 0;
    w->data = m;
    wm_track_mem(w, (int)sizeof(*m));
}

static void sm_close(struct window *w)
{
    if (w->data) pfree(w->data, sizeof(struct smon));
}

struct app app_sysmon = {
    .desktop_label = "SysMon",
    .uses_data = 1, .id = "sysmon", .title = "System Monitor", .icon = ICON_CHART, .single = 1,
    .def_w = 640, .def_h = 540,.min_w=600,.min_h=500,
    .open = sm_open, .paint = sm_paint, .key = sm_key,
    .mouse = sm_mouse, .tick = sm_tick, .close = sm_close,
};

SCOS_APP(app_sysmon, 008);
