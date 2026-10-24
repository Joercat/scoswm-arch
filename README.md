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
utilities. Terminal and Files open your configured/detected Linux
programs; SysMon and Settings are tiny built-in panels. SysMon reads
only `/proc` and `/sys` (including NVIDIA proprietary vs nouveau
detection) and only samples while open. Settings changes the theme,
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

The only runtime dependencies are `libX11` and zlib (the latter is
used to decode standard PNG app icons) — on Debian those are the
`libx11-6`/`zlib1g` packages, pulled in automatically by the `.deb`.

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
sudo make install          # /usr/bin/scoswm, session files under /usr/share
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
| Window list | "Windows N" taskbar button |
| Move / resize | Drag the titlebar / any window edge or corner, or `Alt+drag` / `Alt+right-drag` anywhere. Dragging a maximised titlebar restores the window under the pointer |
| Minimise / maximise / close | Titlebar buttons, double-click titlebar to maximise, or right-click titlebar/taskbar button for a menu. Minimise and restore have a short transition |
| Cycle windows | `Alt+Tab` / `Alt+Shift+Tab`, or use the mouse wheel over the desktop, taskbar or a titlebar |
| Terminal | `Super+Enter` |
| SysMon / Settings | `Super+M` / `Super+,`, or their desktop/taskbar icons |
| Close focused window | `Alt+F4` or `Super+Q` |
| Minimise / maximise focused | `Alt+F9` / `Alt+F10` |
| Desktop menu | Right-click the desktop |
| Power menu | Taskbar power button: lock, restart/exit the WM, suspend, reboot, power off (via `systemctl`) |

Desktop, taskbar and launcher icons are the apps' real
`Icon=` images, resolved from the freedesktop icon theme and
`/usr/share/pixmaps` (PNG/XPM) at the normal small sizes — 24 px on
the desktop, 20 px in taskbar pins and 16 px beside running-window
titles. Icons are decoded once and cached as X pixmaps; an app with no
readable PNG/XPM icon keeps the SCos letter-glyph fallback rather than
growing the layout.

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

## Configuration

`~/.config/scoswm/scoswm.conf` (see `packaging/scoswm.conf.example`):

```ini
theme = blue-sky
terminal = alacritty
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

On Debian the default terminal honours the `x-terminal-emulator`
alternative (`sudo update-alternatives --config x-terminal-emulator`).

`scoswm --theme blue-sky` overrides the theme for one run;
`scoswm --config FILE` loads a specific config. `SCOSWM_DEBUG=1`
logs X errors.

## Development

```sh
make check
```

runs unit tests for the pure logic (SCos theme values, config and
`.desktop` parsing, Exec field-code stripping, UTF-8 titles — all in
`src/util.c`, no X server needed) and, when Xvfb is available, an
integration test that boots the WM, manages a real Xlib client
(`tests/testclient.c`) and verifies adoption through the window tree
(`tests/xprobe.c`). Without any X server the integration step reports
SKIP.

`PORTING.md` documents how each piece of the SCos desktop maps onto
this WM and where real-application behaviour necessarily differs.

## Layout

- `src/scoswm.c` — the window manager (Xlib, EWMH/ICCCM)
- `src/util.c` — X-independent helpers: themes, config, `.desktop` discovery, Linux `/proc` parsers
- `packaging/` — display-manager session, example config, session script
- `tests/` — unit tests, integration harness client/probe
