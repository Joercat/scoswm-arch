# How the SCos window manager was ported to Linux

## What "the WM" is in SCos

In SCos the window manager is not a separate program talking to a
kernel: `kernel/desktop/wm.c` is a compositor, desktop shell and window
manager in one, and the "applications" (Files, Terminal, Notepad, …)
are C translation units in the same address space that register a
`struct app` (open/paint/key/mouse/tick/close callbacks) and draw into a
per-window RAM surface with the `s_*` primitives in `fb.c`. Input
arrives as `key_event`/`mouse_event` structs from PS/2 (or USB HID)
queues; output is a `u32` back buffer copied to the firmware
framebuffer.

That shape is what made a faithful port possible: keep `wm.c`, the app
sources and the drawing code, and replace only the kernel services
underneath them. This port therefore runs the original WM code — the
same damage-rectangle compositor, icon grid, taskbar, launcher, menus,
dialogs and notifications — with a Linux platform layer in place of the
SCos kernel. It is a port of that desktop (its windows manage *SCos
apps*), not an X11/Wayland window manager for native Linux programs;
making it manage foreign windows would mean replacing the app model
the SCos desktop is built on.

## Provenance

All files in `src/` whose header says "SCos native" are copied from
[SCos](https://github.com/Joercat/SCos) at commit
`33edc51f31ababd19bc0b3d072018b40555a1773` (branch
`arena/01a09dfd-scos`), mostly from `kernel/desktop/`, plus
`kernel/src/font.c`. Changes to them are the minimal adaptations listed
below and are marked in place. New files are `scos.h` (trimmed),
`platform_linux.c`, `backend_sdl.c`, `backend_headless.c`, `main.c`,
`platform.h` and `scos_apps.ld`.

## Service-by-service mapping

| SCos (bare metal) | Linux port |
|---|---|
| GOP framebuffer, `fb_flip*` scanout copy | `platform_present()` → SDL streaming texture (XRGB8888); damage rects are presented individually, as before |
| PS/2 + xHCI HID event queues | SDL keyboard/mouse events translated to the same `key_event`/`mouse_event` queues (`platform_linux.c` rings, `backend_sdl.c` translation; SDL `yrel` is inverted to the PS/2 "+dy is up" convention) |
| PIT 100 Hz `tick_count` | `CLOCK_MONOTONIC`; 1 tick = 10 ms, updated in `platform_pump()` |
| CMOS RTC | `localtime()` |
| Kernel page allocator under `palloc/pfree` (`heap.c`) | `mmap`/`munmap` in 4 KiB pages — `heap.c` is used verbatim, ownership accounting and SysMon numbers included |
| Host memory size/free | `sysinfo()`; CPU brand CPUID on x86 (the original `lib.c` code), `/proc/cpuinfo` MHz, `sysconf` core count, `getrusage` process load (labels reworded where the old text claimed TSC/PIT measurements) |
| Verified ATA filesystem image (`save`) | Snapshot file `$XDG_DATA_HOME/scoswm/vfs.snapshot` (`--data-dir` overrides), format `SCOSLNX1` + per-node records, written via a temp file + rename; loaded at startup and auto-saved on clean exit |
| ACPI power-off, 8042 reboot | Clean exit of the `scoswm` process (taskbar power menu, Terminal `shutdown`/`reboot`, Settings factory reset, Ctrl+Alt+Del). The host is never rebooted or powered off |
| PCI GPU detection + loadable engine modules (`graphics`) | Not ported. `graphics_report()` says the renderer is the CPU software compositor; every `gpu_engine_*` call declines, which is exactly the fallback path SCos itself takes with no bound module |
| COM1 serial `klog` | The on-screen ring buffer is kept; the serial sink becomes stderr under `SCOSWM_DEBUG=1` |
| Kernel maintenance TTYs (Ctrl+Alt+F1–F6, `tty`) | Not ported; the key chord and command answer that there are no text consoles in the Linux port |
| `panic()` | Message to stderr + `abort()` (including Terminal `panic`/`sysrq panic`) |
| Lua runtime + `.cat` packages + App Studio | **Not ported.** `lua_app_install()` returns NULL, `lua_source_check()` reports why, `appuninstall` explains. Studio is not linked, because its only product was `.cat` packages. This is the biggest remaining piece of work |
| Boot splash (`boot_screen.c`) | Kept, with shortened steps describing the host actually in use |

## Source adaptations to the copied files

Kept verbatim except as noted:

- `wm.c` — power menu and Ctrl+Alt+Del exit via
  `platform_request_quit()`; the PIC-rearming watchdog notes stalled
  ticks instead; the main loop calls `platform_pump()` /
  `platform_wait_idle()` and honours the quit flag, instead of polling
  PS/2/USB and executing `hlt`.
- `fb.c` — `fb_init(width, height)`; the GOP scanout copy becomes
  `platform_present()`; GPU-engine scroll helpers decline.
- `vfs.c` — the generated Lua demo packages/notices are no longer
  seeded (no Lua loader), and the `system/README.txt` text describes
  snapshot persistence.
- `klog.c`, `err.c`, `boot_screen.c` — serial/PIC/USB calls replaced
  as in the table above.
- `apps.c`, `cat.c` — unchanged; they link against the Lua stubs.
- `lib.c` — CPUID code guarded to x86 (other architectures report the
  host generically); `strstr` returns `char *` to match the hosted libc
  declaration.
- `app_terminal.c`, `app_settings.c`, `app_about.c`, `app_sysmon.c`,
  `confirm.c` — shutdown/reboot/reset paths exit the process; labels
  that named ATA/GOP/PIT/TSC now name the snapshot file, the host
  window and host-reported values. No displays were removed and no
  numbers are invented: SysMon still reports the real allocator
  ledger, host RAM and this process's measured CPU share.
- `scos.h` — the app-facing API is unchanged; kernel-only declarations
  (port I/O, IRQ, PCI, GOP structs) were dropped and the platform
  hooks added.

App registration deserves a note: on bare metal, built-ins
self-register with `SCOS_APP(sym, order)` into `.scos_apps.<order>`
and the kernel linker script gathers them between
`__scos_apps_start/__scos_apps_end`. The port keeps the macro and the
symbol names; `src/scos_apps.ld` is a GNU-ld `INSERT` fragment that
builds the same sorted section, so `apps.c` walks the same table.

## Backends

- `backend_sdl.c` — the one you run. SDL2 is the only dependency, so
  the same binary works in X11 and Wayland sessions (and elsewhere
  SDL2 runs). The desktop size is fixed at startup, as the firmware
  mode was; resizing the host window scales/letterboxes it.
- `backend_headless.c` — no display at all. It counts presented
  frames, optionally accumulates them for `--screenshot`, injects
  `--click`/`--type`, and quits after `--frames`. `make check`
  (`tests/test_port.py`) drives the whole desktop through it,
  including a theme-change persistence round-trip and opening every
  built-in app. The SDL binary was additionally verified against a
  real SDL2 library under SDL's dummy video driver.

## Deliberate divergences from bare-metal behaviour

1. The snapshot is saved automatically on clean exit (bare metal
   required an explicit verified `save`; the command still exists).
2. "Restart/Power Off" mean "exit scoswm".
3. The desktop session has one fixed resolution chosen at startup
   (`--width/--height/--fullscreen`).
4. Ten built-in apps, not eleven: App Studio is absent until the Lua
   app system is ported.
