# scoswm — the SCos desktop experience as a real Linux window manager

![scoswm with its core Terminal, Files, SysMon and Settings apps, managing a real X11 application](docs/scoswm.png)

`scoswm` is a small, non-compositing **X11 window manager** that manages
your ordinary Linux applications — terminals, browsers, editors, games —
while looking and behaving like the [SCos](https://github.com/Joercat/SCos)
desktop: the bottom taskbar with its 2×2-grid launcher, "Windows N"
picker, pinned shortcuts, power button and clock; desktop icons with
real freedesktop app images on a snap grid over a gradient+grid
wallpaper; 26 px titlebars with
minimise/maximise/close buttons; and the four SCos themes
(**Matrix** `matrix-1`, **Blue Sky** `blue-sky`, **Purple**
`midnight-purple`, **Matrix red** `amber-tech`) with the exact palettes
from SCos's `kernel/desktop/theme.c`.

An earlier version of this project ran the SCos desktop itself inside
one SDL window. That could only ever run SCos's built-in apps, so it
has been replaced wholesale by this window manager: same look, same
interaction model, real applications.

The app set is intentionally small. The retained SCos core is
**Terminal, Files, SysMon and Settings** — no games, demos or duplicate
utilities. Terminal is the included `scos-terminal` application: an
SCos-styled, tabbed terminal running a real interactive Bash on a Linux
PTY. Files opens your configured/detected Linux file manager; SysMon and
Settings are tiny built-in panels. SysMon reads only `/proc` and `/sys`
(including NVIDIA proprietary vs nouveau detection) and only samples
while open. Settings changes the theme,
taskbar height, program choices and pins, applies them live and saves
the normal config file.

## Why X11 (and why it is the lightweight choice)

- **One code path for your GPU.** Xorg runs on the NVIDIA proprietary
  driver and on nouveau with identical behaviour — no per-driver
  compositor quirks. Intel/AMD work the same way for when you need them.
- **GPU acceleration stays on.** There is no software compositor to
  bypass or accelerate: OpenGL/Vulkan clients render directly through
  Xorg and the GPU driver (GLX/DRI on NVIDIA proprietary and nouveau),
  while `scoswm` itself only draws window decorations and the taskbar.
  Idle cost is a few MB of RAM and ~0% CPU (it blocks on the X
  connection and wakes once a second for the clock).
- **Wayland programs still work** through XWayland, which every major
  toolkit supports.

The hard shared-library runtime dependencies are `libX11` and zlib (the
latter is used to decode standard PNG app icons) — on Debian those are
the `libx11-6`/`zlib1g` packages, pulled in automatically by the `.deb`.
SVG icon support is optional through the recommended gdk-pixbuf/librsvg
packages. The terminal uses the system's PTY support and Bash; no Konsole,
VTE, or other terminal-emulation library is embedded or required.

## Install

Debian 12/13 (and derivatives) — build a quick test package:

```sh
sudo apt install build-essential libx11-dev zlib1g-dev
./packaging/build-deb.sh
sudo apt install ./scoswm_*.deb
```

or build the proper Debian source package in `debian/`:

```sh
sudo apt install debhelper libx11-dev zlib1g-dev
dpkg-buildpackage -us -uc -b
sudo apt install ../scoswm_*.deb
```

Arch (this repo ships a `PKGBUILD`):

```sh
makepkg -si
```

Any distro:

```sh
make
sudo make install          # WM + terminal + icons/cursor theme + session entries
```

### Selecting SCosWM in SDDM

Installation puts `scoswm.desktop` in `/usr/share/xsessions/`, which is
where SDDM looks for X11 sessions. No SDDM configuration is needed:
log out, click the session menu at the SDDM greeter, choose **SCosWM**,
and log in. (SDDM starts its X server via Xorg, so make sure
`xserver-xorg-core` is installed — the Debian package recommends it.)
Verify the session is registered with:

```sh
grep -H '^Name=\|^Exec=\|^TryExec=' /usr/share/xsessions/scoswm.desktop
```

### Troubleshooting a failed SDDM login

If selecting SCosWM flashes and drops you back to the greeter, the WM
exited during startup and logged why. Check, in order:

```sh
cat ~/.cache/scoswm/session.log          # scoswm's own messages + exit status
cat ~/.local/share/sddm/xorg-session.log # SDDM/Xorg side of the attempt
which Xorg                               # an X11 session needs an X server
```

The usual causes are a missing Xorg (`sudo apt install
xserver-xorg-core`, plus your GPU's Xorg driver, e.g.
`xserver-xorg-video-nvidia` on proprietary NVIDIA), missing core fonts
(`xfonts-base`, a hard dependency of the `.deb`), or another window
manager already owning the display. To isolate SDDM entirely, switch
to a TTY (`Ctrl+Alt+F3`), log in, and run
`startx /usr/bin/scoswm-session` — startup errors then print directly
on that console.

Then pick **SCosWM** in your display manager, or from a TTY:

```sh
echo 'exec scoswm-session' >> ~/.xinitrc
startx
```

## Using it

| Action | How |
|---|---|
| Open the app launcher | Click the 2×2 grid button, or `Super+D`. The normal view shows only core/pinned apps; typing searches installed apps too. Choose **All installed apps...** to browse without typing; the mouse wheel scrolls the list |
| Launch pinned apps | Taskbar pin squares, or double-click desktop icons |
| Add/remove Home apps | Right-click the desktop and choose **Add/Remove App...**, or right-click any searched app in the launcher. Pinned apps appear on the desktop and taskbar and are saved in `pins` |
| Window list | "Windows N" taskbar button. Running windows appear only here, with their real icons; the main taskbar keeps only your pinned shortcuts |
| Move / resize | Drag the titlebar / any window edge or corner, or `Alt+drag` / `Alt+right-drag` anywhere. Dragging a maximised titlebar restores the window under the pointer |
| Minimise / maximise / close | Titlebar buttons, double-click titlebar to maximise, or right-click the titlebar for a menu. Restore minimised windows from **Windows N**; minimise/restore transitions target that picker and never resize the real application |
| Cycle windows | `Alt+Tab` / `Alt+Shift+Tab`, or use the mouse wheel over the desktop, taskbar or a titlebar |
| Terminal | `Super+Enter` |
| SysMon / Settings | `Super+M` / `Super+,`, or their desktop/taskbar icons |
| Close focused window | `Alt+F4` or `Super+Q` |
| Minimise / maximise focused | `Alt+F9` / `Alt+F10` |
| Desktop menu | Right-click the desktop |
| Power menu | Taskbar power button: lock, restart/exit the WM, suspend, reboot, power off (via `systemctl`) |

Terminal, Files, SysMon and Settings use their bundled `scos-terminal`,
`scos-files`, `scos-sysmon` and `scos-settings` images directly; they do
not depend on generic theme icon names. Other desktop, taskbar and
launcher icons are the apps' real `Icon=` images, resolved from the
freedesktop icon theme and `/usr/share/pixmaps` at the normal small
sizes — 24 px on
the desktop, up to 20 px in taskbar pins and 16 px beside titles in the
Windows picker (smaller automatically when the taskbar is shortened). PNG and XPM
are decoded directly; SVG themes such as Breeze use the recommended
`gdk-pixbuf`/`librsvg` loader when installed. Icons are decoded once and
cached as X pixmaps; only an app with no readable image keeps the SCos
letter-glyph fallback rather than growing the layout.

### Consistent SCos cursors

The package installs the **SCos** Xcursor theme (24/32/48 px images plus
common GTK/Qt/CSS cursor aliases) under `/usr/share/icons/SCos`. The
session explicitly selects it instead of inheriting KDE's Breeze choice,
and the WM publishes the same name and size through root `RESOURCE_MANAGER`
resources and the standard GTK `Gtk/CursorThemeName` /
`Gtk/CursorThemeSize` XSETTINGS properties. This covers the desktop, WM
frames and application interiors, including Qt 6 and GTK clients, without
running a settings daemon. Root/frame cursor inheritance alone is not
relied upon.

Set `SCOS_CURSOR_THEME` and/or `SCOS_CURSOR_SIZE` before
`scoswm-session` only if you deliberately want a different theme or size.
The generated cursor and application images are shipped in the repository;
Python is needed only to regenerate them with
`packaging/generate_artwork.py`, never to build or run SCosWM.

The launcher's normal view is deliberately curated to the four core
apps plus your pins. Once you type, it searches installed apps too,
and the **All installed apps...** entry at the bottom of the launcher
browses everything without typing. Applications are discovered in the
standard XDG data dirs (`~/.local/share/applications`,
`/usr/local/share/applications`, `/usr/share/applications` and the
rest of `XDG_DATA_DIRS`) plus the Flatpak and Snap export directories
(`~/.local/share/flatpak/exports/share/applications`,
`/var/lib/flatpak/exports/share/applications`,
`/var/lib/snapd/desktop/applications`), so Firefox, Steam, VS Code,
Flatpaks and Snaps are all available without cluttering the
default view.

## SCos Terminal

`Super+Enter`, the Terminal icon, and the launcher open **SCos Terminal**
by default. It starts `/bin/bash` on a real pseudo-terminal, so Bash,
Readline, SSH, Vim, htop, tmux, and ordinary Linux command-line programs
run normally; it is not a simulated SCos command parser. The frontend
provides SCos-styled tabs, 1024 lines of scrollback, UTF-8 decoding and
Unicode core-font output, ANSI/256/true-color attributes, alternate
screens, application cursor keys and mouse reports, terminal resize
notifications, selection, and clipboard paste.

| Terminal action | How |
|---|---|
| New / close tab | `Ctrl+Shift+T` / `Ctrl+Shift+W`, or the tab `+` / `x` buttons |
| Cycle tabs | `Ctrl+Tab` / `Ctrl+Shift+Tab` |
| Copy / paste | Select with the mouse and `Ctrl+Shift+C` / `Ctrl+Shift+V`; middle-click pastes the primary selection |
| Scroll | Mouse wheel or `Shift+PageUp` / `Shift+PageDown`; typing returns to the live prompt |

Bash's normal `Ctrl+T` and `Ctrl+W` editing keys are deliberately left
for the shell. Fullscreen applications receive their mouse input;
holding Shift bypasses that for terminal selection/scrolling.

The package also installs `/usr/share/applications/scos-terminal.desktop`
and dedicated `scos-terminal` hicolor icons (16–256 px), with a KDE
`StartupWMClass` mapping, so **SCos Terminal** appears with its own identity
in KDE, GNOME, and other application menus and works independently of
SCosWM. It inherits the session's themed cursor. Direct command-line use is:

```sh
scos-terminal                         # interactive Bash
scos-terminal -e htop                 # run one command instead of Bash
scos-terminal -hold -e bash -c 'ls -l; read'
```

An explicit `terminal = ...` in `scoswm.conf` still takes priority for
SCosWM's Terminal action. Set `SCOS_SHELL` only if you intentionally
want a shell other than the default Bash.

## Browser titlebars and window controls

SCosWM draws the SCos titlebar and controls for managed X11 windows.
The session asks Firefox and ordinary GTK applications to use those
system decorations (`MOZ_GTK_TITLEBAR_DECORATION=system` and `GTK_CSD=0`)
without overriding values you explicitly exported. Restart the browser
after logging into SCosWM so it sees those values.

Chromium/Chrome's custom frame is its own preference: enable **Use system
title bar and borders** in Settings → Appearance or from the tab-strip
context menu. Applications with an intentionally custom titlebar can
still draw one; an X11 WM cannot remove buttons from inside a client's
own pixels, and SCosWM does not modify browser profiles behind your back.

## Configuration

`~/.config/scoswm/scoswm.conf` (see `packaging/scoswm.conf.example`):

```ini
theme = blue-sky
terminal =                 # empty = SCos Terminal; or name another command
pins = terminal, files, sysmon, settings, firefox, code
bar_height = 40
```

The built-in **Settings** panel applies themes and taskbar height
live and saves this file; editing it by hand also works. The
power/desktop menus have a **Restart WM** entry that keeps your
windows open. An optional `~/.config/scoswm/autostart.sh` is run by
`scoswm-session`.

There are deliberately **no multiple workspaces**: one workspace, no
pager, no workspace daemon and no extra background process.

With `terminal =` empty, SCosWM prefers its installed `scos-terminal`.
External terminals remain supported as an explicit custom command and as
a fallback if the bundled executable is unavailable.

`scoswm --theme blue-sky` overrides the theme for one run;
`scoswm --config FILE` loads a specific config. `SCOSWM_DEBUG=1`
logs X errors.

## Development

```sh
make check
```

runs unit tests for the pure logic (SCos theme values, config and
`.desktop` parsing, Exec field-code stripping, UTF-8 titles), terminal
emulation checks, and a real interactive-Bash PTY integration test — all
without needing an X server. When Xvfb is available, it also boots the
WM, manages a real Xlib client
(`tests/testclient.c`) and verifies adoption through the window tree
(`tests/xprobe.c`). Without any X server the integration step reports
SKIP. `tests/run_graphical.sh` additionally runs headless graphical
regressions when a `node-x11` checkout with `lib/xserver` is available:
set `SCOSWM_NODE_X11=/path/to/node-x11`. It checks desktop geometry and
both right-border columns, 28 px taskbar bounds, the upward power-symbol
opening, high-contrast launcher/menu/Settings selections, close hover, dock
stacking, the pinned-only taskbar, picker-based resize-free
minimise/restore, 20 rapid window-withdrawal/reuse cycles, real images for
all four built-ins, Qt/GTK cursor settings and cursor inheritance, buffered
terminal resize/move behavior, and mouse-opened Settings with live persisted
28/32 px taskbar scaling.

`PORTING.md` documents how each piece of the SCos desktop maps onto
this WM and where real-application behaviour necessarily differs.

## Layout

- `src/scoswm.c` — the window manager (Xlib, EWMH/ICCCM)
- `src/scos-terminal.c` — standalone Xlib terminal frontend and PTY sessions
- `src/term_emulation.c` — display-independent terminal screen and VT parser
- `src/util.c` — X-independent helpers: themes, config, `.desktop` discovery, Linux `/proc` parsers
- `packaging/` — display-manager session, example config, built-in artwork, SCos cursor theme and deterministic artwork generator
- `tests/` — unit tests, integration harness client/probe
