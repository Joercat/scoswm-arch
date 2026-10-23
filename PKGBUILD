# Unofficial packaging for the SCos window manager (Linux port).
# This builds from a local git clone placed in ./pkgbuild-src so the
# recipe works from a checkout of this repository:
#
#     git clone https://github.com/Joercat/scoswm-arch.git pkgbuild-src
#     makepkg -si
#
# To package a tagged release instead, point `source` at the release
# tarball on GitHub and drop the local-directory source line.
pkgname=scoswm
pkgver=0.1.0
pkgrel=1
pkgdesc="The SCos window manager/desktop, ported from the SCos OS to Linux (SDL2)"
arch=('x86_64' 'aarch64')
url="https://github.com/Joercat/scoswm-arch"
license=('custom')
depends=('sdl2')
makedepends=('pkgconf')
source=("pkgbuild-src::git+file://$PWD/pkgbuild-src")
sha256sums=('SKIP')

build() {
    cd "$srcdir/pkgbuild-src"
    make scoswm CFLAGS="$CFLAGS"
}

check() {
    cd "$srcdir/pkgbuild-src"
    make check
}

package() {
    cd "$srcdir/pkgbuild-src"
    make install DESTDIR="$pkgdir" PREFIX=/usr
}
