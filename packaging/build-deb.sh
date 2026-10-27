#!/bin/sh
# Build a quick .deb for testing on Debian, without needing debhelper.
# (For a proper source-package build use debian/ + dpkg-buildpackage.)
#
#   sudo apt install build-essential libx11-dev libxext-dev zlib1g-dev
#   ./packaging/build-deb.sh
#   sudo apt install ./scoswm_<version>_<arch>.deb
#
# The package installs /usr/share/xsessions/scoswm.desktop, so SDDM
# offers SCosWM at the login screen immediately after installation.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

command -v dpkg-deb >/dev/null 2>&1 || { echo "dpkg-deb not found (install dpkg-dev)"; exit 1; }
command -v cc >/dev/null 2>&1 || { echo "no C compiler (install build-essential)"; exit 1; }

version=$(awk -F'"' '/DVERSION/{print $2; exit}' Makefile)
[ -n "$version" ] || { echo "could not read VERSION from Makefile"; exit 1; }
arch=$(dpkg --print-architecture)

make clean >/dev/null
make

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT INT TERM
make install DESTDIR="$stage" PREFIX=/usr >/dev/null
chmod 755 "$stage"

mkdir -p "$stage/DEBIAN"
# Real shared-library dependencies from the built binary.
if [ -f debian/control ] && command -v dpkg-shlibdeps >/dev/null 2>&1; then
    shlibs=$(dpkg-shlibdeps -O "$stage/usr/bin/scoswm" "$stage/usr/bin/scos-terminal" "$stage/usr/bin/scos-files" | sed 's/^shlibs:Depends=//')
else
    shlibs="libc6, libx11-6, libxext6, zlib1g"
fi
installed_size=$(du -sk "$stage/usr" | cut -f1)

cat > "$stage/DEBIAN/control" <<EOF
Package: scoswm
Version: $version
Section: x11
Priority: optional
Architecture: $arch
Maintainer: Joe <167563398+Joercat@users.noreply.github.com>
Depends: $shlibs, libxcomposite1, bash, xfonts-base
Recommends: xserver-xorg-core, xinit, xdg-utils, dbus-x11, libxcursor1, libgdk-pixbuf-2.0-0, librsvg2-common
Suggests: sddm, networkmanager, slock | i3lock | suckless-tools
Installed-Size: $installed_size
Homepage: https://github.com/Joercat/scoswm-arch
Description: SCos-styled lightweight X11 window manager
 scoswm is a small, non-compositing X11 window manager that manages
 ordinary Linux applications while looking and behaving like the SCos
 desktop. It installs /usr/share/xsessions/scoswm.desktop, so display
 managers such as SDDM offer "SCosWM" as a selectable X11 session.
 The included scos-terminal runs a real interactive Bash on a PTY and
 scos-files browses real POSIX directories. Both install standalone
 desktop entries for KDE and other desktops. The taskbar Wi-Fi panel
 preserves multiple adapters and simultaneous connections.
EOF

out="$root/scoswm_${version}_${arch}.deb"
dpkg-deb --build --root-owner-group "$stage" "$out" >/dev/null
echo "built $out"
