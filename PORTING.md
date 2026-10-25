# How SCos maps onto scoswm

The goal: the SCos desktop's look and interaction model, managing real
Linux applications. The previous incarnation of this repo (the SCos
desktop running in-process inside an SDL window) was discarded because
it could only run SCos's built-in apps.

## Backend choice

| Considered | Verdict |
|---|---|
| SDL/nested framebuffer (old port) | Rejected by requirements: runs no real apps. |
| Wayland compositor (wlroots) | Proprietary NVIDIA needs GBM/EGLStreams care per version; a from-scratch compositor is also the largest, hungriest option — against SCos's reason to exist. |
| **X11 + Xlib, no compositor** | **Chosen.** NVIDIA proprietary and nouveau both drive Xorg identically; clients render directly through the driver; the WM only draws decorations/taskbar. XWayland covers Wayland-only clients. |

## Feature mapping (SCos `kernel/desktop/*` → scoswm)

| SCos | scoswm |
|---|---|
| `theme.c` four palettes | Copied verbatim into `src/util.c` (`scos_themes[]`), unit-tested; config can also define a custom palette field-by-field |
| `wm.c` 26 px titlebar, 18 px min/max/close, accent when focused, `color_blend` when not | Same geometry and colours in `src/scoswm.c: draw_frame()`; buttons act on the real client window (ICCCM `WM_DELETE_WINDOW`, EWMH state for maximise/minimise) |
| Taskbar: launcher grid button, "Windows N" picker, pins, power button, clock | Same layout in `draw_bar()`; only permanent pins occupy the main bar, while the picker lists real running windows with their icons and is the animation target for minimise/restore |
| Launcher grid + `user/apps` | Launcher panel over the taskbar indexing freedesktop `.desktop` files, type-to-search — the Linux-native equivalent |
| Desktop icons 88×96 snap grid, selection, context menus | Same grid on the root window; default icons are Terminal, Files, SysMon and Settings; right-click menu opens only those core apps, the launcher, or WM controls |
| Power menu | Real Linux actions: locker if installed, WM restart (clients reparented first, so nothing closes), `systemctl` suspend/reboot/poweroff |
| Settings app | Built-in lightweight panel: live theme/taskbar changes, Terminal/Files/Pins editing, saved to `~/.config/scoswm/scoswm.conf` |
| SCos core apps | Only Terminal, Files, SysMon and Settings are retained. Terminal is the standalone `scos-terminal` Xlib/PTY application running real Bash; Files delegates to a real Linux file manager; SysMon is a built-in `/proc` + `/sys` panel with NVIDIA/nouveau detection; Settings is a built-in live config panel. Games and other non-normal-use SCos apps are intentionally absent. |

## Protocol coverage

ICCCM: `WM_NAME`, `WM_CLASS`, `WM_NORMAL_HINTS` (min/max size, resize
increments), `WM_TRANSIENT_FOR`, `WM_PROTOCOLS`
(`WM_DELETE_WINDOW`, `WM_TAKE_FOCUS`), `WM_STATE`. An ordinary unmap is
handled as withdrawal, not window destruction: the client is reparented
to the root before only its SCosWM frame is destroyed, so it can map again.
EWMH: `_NET_SUPPORTED`, `_NET_CLIENT_LIST`, `_NET_ACTIVE_WINDOW`,
`_NET_SUPPORTING_WM_CHECK`, `_NET_WM_NAME`, `_NET_WM_DESKTOP`,
`_NET_NUMBER_OF_DESKTOPS`, `_NET_CURRENT_DESKTOP`, `_NET_WORKAREA`,
`_NET_WM_STRUT_PARTIAL` (respected from other panels too),
`_NET_FRAME_EXTENTS`, `_NET_CLOSE_WINDOW`, `_NET_MOVERESIZE_WINDOW`,
`_NET_WM_STATE` (maximized/fullscreen/hidden), window types
(dock/desktop/notification left unmanaged; ordinary and utility windows
are reachable from the Windows picker). Root supports RandR-driven resize
via its ConfigureNotify.

Cursor consistency uses the bundled SCos Xcursor theme, explicit session
`XCURSOR_THEME`/`XCURSOR_SIZE`, root `RESOURCE_MANAGER` entries for Qt, and
a small WM-owned `_XSETTINGS_SETTINGS` manager containing GTK's cursor name
and size. This is startup/publication code in the WM, not another daemon.

By design: a single workspace only (no pager/workspace daemon) and no
compositing effects. `_NET_WM_ICON` and freedesktop icon images are used
for clients and pins. Text uses core fonts rather than Xft/fontconfig;
WM titles are converted UTF-8 → Latin-1, while the terminal uses an
ISO10646 font when available for its Unicode cell grid.

## Verification

- `make check`: unit tests for everything X-independent, including the
  SCos theme constants, terminal parser/screen behavior and a real
  interactive-Bash PTY session — a theme or terminal regression fails CI.
- `tests/run_integration.sh`: under Xvfb, boots scoswm and checks a
  real Xlib client is framed, focused and listed.
- `tests/run_graphical.sh`: uses a pure-JavaScript X11 server to drive
  the real Xlib WM and terminal without Xvfb. Set `SCOSWM_NODE_X11` to a
  `sidorares/node-x11` checkout containing `lib/xserver`. The saved
  regression covers buffered drawing, desktop/taskbar geometry including
  both right-border columns, power and high-contrast selection colours,
  close hover, taskbar stacking, the pinned-only bar, picker activation and
  resize-free minimise/restore, 20 window-reuse cycles, all four built-in
  image sets, Qt/GTK cursor publication and inheritance, pointer-driven
  terminal resize/move, and mouse-opened Settings with live persisted
  taskbar scaling.
- Earlier development runs also verified adoption, maximise/restore,
  Alt+Tab, desktop-icon and launcher launching, and config-file theming;
  `docs/scoswm.png` was captured from such a run.
