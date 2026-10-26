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
| Taskbar: launcher grid button, "Windows N" picker, pins, power button, clock | Same layout in `draw_bar()` plus a core-font numeric date and lightweight Wi-Fi button; only permanent pins occupy the main bar. Left-click lists real running windows; right-click opens an on-demand MRU visual overview with captured tiles, wallpaper, green borders and a clean outer margin. The taskbar remains above the overview and ordinary taskbar actions close it before proceeding |
| Launcher grid + `user/apps` | Launcher panel over the taskbar indexing freedesktop `.desktop` files, type-to-search — the Linux-native equivalent |
| Desktop icons 88×96 snap grid, selection, context menus | Same grid on the root window; default icons are Terminal, Files, SysMon and Settings; right-click menu opens only those core apps, the launcher, or WM controls |
| Power menu | Real Linux actions: locker if installed, WM restart (clients reparented first, so nothing closes), `systemctl` suspend/reboot/poweroff. Every disruptive action has a Cancel-default confirmation; Lock remains immediate |
| Settings app | Built-in lightweight panel: live theme/taskbar changes, Terminal/Files/Pins editing, saved to `~/.config/scoswm/scoswm.conf` |
| SCos core apps | Only Terminal, Files, SysMon and Settings are retained. Terminal is the standalone `scos-terminal` Xlib/PTY application running real Bash; Files is the standalone original-style `scos-files` X11/POSIX application (with an explicit external-command override); SysMon is a built-in `/proc` + `/sys` panel with NVIDIA/nouveau detection; Settings is a built-in live config panel. Games and other non-normal-use SCos apps are intentionally absent. |
| No original network stack | Native per-adapter wireless status plus optional asynchronous `nmcli` scan/connect. Rows retain interface names and simultaneous active connections; same-interface/nonempty-SSID BSSID duplicates merge while different adapters and hidden SSIDs stay distinct. Hosted AP mode is detected separately from client connections and active rows sort first. No network daemon is embedded. |

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
via its ConfigureNotify. The two-pixel right desktop rule uses a small
WM-owned override-redirect surface above ordinary frames, with an empty
X SHAPE input region so it cannot intercept application clicks. If SHAPE is
unavailable, the overlay stays unmapped and the wallpaper fallback remains.

Cursor consistency uses the bundled SCos Xcursor theme, explicit session
`XCURSOR_THEME`/`XCURSOR_SIZE`, root `RESOURCE_MANAGER` entries for Qt, and
a small WM-owned `_XSETTINGS_SETTINGS` manager containing GTK's cursor name
and size. This is startup/publication code in the WM, not another daemon.

By design: a single workspace only (no pager/workspace daemon) and no
compositing effects. `_NET_WM_ICON` and freedesktop icon images are used
for clients and pins. Text uses core fonts rather than Xft/fontconfig;
WM titles are converted UTF-8 → Latin-1, while the terminal uses an
ISO10646 font when available for its Unicode cell grid.

Overview capture dynamically uses libXcomposite when available: an existing
compositor's named pixmap is read directly; otherwise a window is redirected
only around the synchronous capture and immediately unredirected. Channel
masks are converted between 32-bit client visuals and the thumbnail visual.
This is short-lived thumbnail capture, not a persistent compositor or idle
rendering loop. Hardware XF86 audio/playback/brightness keys and
Super+F1/F2/F3 volume fallbacks run configurable external commands, with
wpctl/pactl/amixer, playerctl and brightnessctl/light auto-detection.

## Verification

- `make check`: unit tests for everything X-independent, including the
  SCos theme constants, terminal parser/screen behavior and a real
  interactive-Bash PTY session — a theme or terminal regression fails CI.
- `tests/run_integration.sh`: under Xvfb, boots scoswm and checks a
  real Xlib client is framed, focused and listed.
- `tests/run_graphical.sh`: uses a pure-JavaScript X11 server to drive
  the real Xlib WM and terminal without Xvfb. Set `SCOSWM_NODE_X11` to a
  `sidorares/node-x11` checkout containing `lib/xserver`. Coverage includes
  buffered drawing, compact date/Wi-Fi geometry, all five power-action
  confirmation cancellations, high-contrast selection states, taskbar
  stacking, panel hover interiors, configurable media commands, and the
  two-pixel right rule both above a covering application and click-through
  via its empty SHAPE input region. Three real clients check MRU overview
  tiles, wallpaper/borders, taskbar stacking/actions, repeated dismissal and
  tile activation. Retained checks
  cover the pinned-only bar, resize-free minimise/restore, 20 window-reuse
  cycles, built-in images, Qt/GTK cursor publication, terminal repaint and
  long-title Close behavior, standalone Files navigation/create/rename,
  delayed fake-`nmcli` active-first multi-device interaction, shortcut
  multi-instance/MRU activation, drag/drop and mouse-opened Settings.
  Headless geometry is not presented as proof of physical-display
  visibility, real wireless/power behavior, or physical input latency.
- Earlier development runs also verified adoption, maximise/restore,
  Alt+Tab, desktop-icon and launcher launching, and config-file theming;
  `docs/scoswm.png` was captured from such a run.
