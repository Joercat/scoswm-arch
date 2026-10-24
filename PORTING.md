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
| Taskbar: launcher grid button, "Windows N" picker, pins, power button, clock | Same layout in `draw_bar()`; the picker is a real window list, pins launch/focus real apps |
| Launcher grid + `user/apps` | Launcher panel over the taskbar indexing freedesktop `.desktop` files, type-to-search — the Linux-native equivalent |
| Desktop icons 88×96 snap grid, selection, context menus | Same grid on the root window; icons are your pins, double-click launches/focuses the real app; right-click menu opens Terminal/Files/Browser/launcher/config |
| Power menu | Real Linux actions: locker if installed, WM restart (clients reparented first, so nothing closes), `systemctl` suspend/reboot/poweroff |
| Settings app theme editing | Config file (`~/.config/scoswm/scoswm.conf`) + Restart WM; `--theme` for one-off runs |
| SCos built-in apps (Terminal, Files, Settings, SysMon, …) | **Deliberately not ported.** Your real terminal/file manager/settings tools take their place via pins and auto-detection. |

## Protocol coverage

ICCCM: `WM_NAME`, `WM_CLASS`, `WM_NORMAL_HINTS` (min/max size, resize
increments), `WM_TRANSIENT_FOR`, `WM_PROTOCOLS`
(`WM_DELETE_WINDOW`, `WM_TAKE_FOCUS`), `WM_STATE`.
EWMH: `_NET_SUPPORTED`, `_NET_CLIENT_LIST`, `_NET_ACTIVE_WINDOW`,
`_NET_SUPPORTING_WM_CHECK`, `_NET_WM_NAME`, `_NET_WM_DESKTOP`,
`_NET_NUMBER_OF_DESKTOPS`, `_NET_CURRENT_DESKTOP`, `_NET_WORKAREA`,
`_NET_WM_STRUT_PARTIAL` (respected from other panels too),
`_NET_FRAME_EXTENTS`, `_NET_CLOSE_WINDOW`, `_NET_MOVERESIZE_WINDOW`,
`_NET_WM_STATE` (maximized/fullscreen/hidden), window types
(dock/desktop/notification left unmanaged; utility/splash kept off the
taskbar). Root supports RandR-driven resize via its ConfigureNotify.

Not yet: multiple desktops/workspaces, `_NET_WM_ICON` taskbar icons
(pins show the SCos-style initial glyph), Xft/fontconfig text
(core fonts only, titles converted UTF-8 → Latin-1), compositing
effects of any kind (by design).

## Verification

- `make check`: unit tests for everything X-independent, including the
  SCos theme constants — a theme edit that diverges from SCos fails CI.
- `tests/run_integration.sh`: under Xvfb, boots scoswm and checks a
  real Xlib client is framed, focused and listed.
- During development the WM was additionally exercised against a
  pure-JavaScript X11 server (node `x11` package) driving real Xlib
  binaries: adoption, drag, maximise/restore, Alt+Tab, minimise,
  desktop-icon and launcher launching, config-file theming, and the
  close button were all verified, with `docs/scoswm.png` captured from
  such a run.
