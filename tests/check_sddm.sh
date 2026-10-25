#!/bin/sh
# SDDM session check: SDDM lists an X11 session when a .desktop file in
# /usr/share/xsessions (or /usr/local/share/xsessions) has an Exec
# command and a TryExec that resolves to an installed executable.
# Install into a scratch root exactly like the Debian/Arch packages do
# and verify SCosWM would be offered and launchable at the greeter.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

make -C "$root" install DESTDIR="$tmp" PREFIX=/usr >/dev/null

entry="$tmp/usr/share/xsessions/scoswm.desktop"
[ -f "$entry" ] || { echo "FAIL: no /usr/share/xsessions/scoswm.desktop installed"; exit 1; }

get() { sed -n "s/^$1=//p" "$entry" | head -1; }
[ "$(get Name)" = "SCosWM" ] || { echo "FAIL: session Name is not SCosWM"; exit 1; }
[ "$(get Type)" = "Application" ] || { echo "FAIL: session Type is not Application"; exit 1; }

exec_cmd=$(get Exec | awk '{print $1}')
try_cmd=$(get TryExec | awk '{print $1}')
[ -n "$exec_cmd" ] || { echo "FAIL: missing Exec"; exit 1; }
[ -n "$try_cmd" ] || { echo "FAIL: missing TryExec"; exit 1; }

# SDDM's SessionModel: a relative TryExec must be found executable on
# the session PATH; here that means inside the installed /usr/bin.
[ -x "$tmp/usr/bin/$try_cmd" ] || { echo "FAIL: TryExec '$try_cmd' is not an installed executable"; exit 1; }
[ -x "$tmp/usr/bin/$exec_cmd" ] || { echo "FAIL: Exec '$exec_cmd' is not an installed executable"; exit 1; }
[ -x "$tmp/usr/bin/scoswm" ] || { echo "FAIL: scoswm binary not installed"; exit 1; }
sh -n "$tmp/usr/bin/scoswm-session"

app="$tmp/usr/share/applications/scos-terminal.desktop"
[ -f "$app" ] || { echo "FAIL: no standalone SCos Terminal desktop entry installed"; exit 1; }
[ -x "$tmp/usr/bin/scos-terminal" ] || { echo "FAIL: scos-terminal binary not installed"; exit 1; }
grep -qx 'Name=SCos Terminal' "$app" || { echo "FAIL: terminal desktop entry has the wrong name"; exit 1; }
grep -qx 'Exec=scos-terminal' "$app" || { echo "FAIL: terminal desktop entry does not launch scos-terminal"; exit 1; }
grep -qx 'Terminal=false' "$app" || { echo "FAIL: graphical terminal must not be nested in another terminal"; exit 1; }
grep -qx 'Icon=scos-terminal' "$app" || { echo "FAIL: terminal uses a generic icon"; exit 1; }
grep -qx 'StartupWMClass=SCosTerminal' "$app" || { echo "FAIL: terminal KDE window class is not mapped"; exit 1; }
for size in 16x16 22x22 24x24 32x32 48x48 64x64 128x128 256x256; do
    for name in scos-terminal scos-files scos-sysmon scos-settings; do
        icon="$tmp/usr/share/icons/hicolor/$size/apps/$name.png"
        [ -s "$icon" ] || { echo "FAIL: missing built-in application icon ($icon)"; exit 1; }
    done
done

cursor_dir="$tmp/usr/share/icons/SCos/cursors"
for name in left_ptr default arrow xterm text hand2 pointer watch wait move size_all \
            hresize ew-resize e-resize vresize ns-resize n-resize bd_double_arrow \
            fd_double_arrow ne-resize nw-resize not-allowed dnd-no-drop; do
    [ -e "$cursor_dir/$name" ] || { echo "FAIL: missing SCos cursor or alias ($name)"; exit 1; }
done
[ "$(od -An -tx1 -N4 "$cursor_dir/left_ptr" | tr -d ' \n')" = "58637572" ] || {
    echo "FAIL: SCos left_ptr is not an Xcursor file"; exit 1;
}
grep -qxF 'export XCURSOR_THEME=${SCOS_CURSOR_THEME:-SCos}' "$tmp/usr/bin/scoswm-session" || {
    echo "FAIL: session does not select the SCos cursor theme"; exit 1;
}
grep -qxF 'export XCURSOR_SIZE=${SCOS_CURSOR_SIZE:-24}' "$tmp/usr/bin/scoswm-session" || {
    echo "FAIL: session does not select a consistent cursor size"; exit 1;
}

echo "SDDM session check: SCosWM will be listed and launchable ($entry)"
echo "Standalone application check: SCos Terminal is available to other desktops ($app)"
echo "Application identity check: four built-in icon sets and terminal KDE window class installed"
echo "Cursor check: SCos Xcursor theme, aliases and session selection installed"
