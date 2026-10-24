# Maintainer: scoswm contributors
pkgname=scoswm
pkgver=0.4.1
pkgrel=1
pkgdesc="SCos-styled lightweight X11 window manager for real Linux applications"
arch=('x86_64' 'aarch64')
url="https://github.com/Joercat/scoswm-arch"
license=('MIT')
depends=('libx11' 'zlib')
makedepends=('gcc' 'make')
optdepends=('xorg-server: X server (Xorg or XWayland host)'
            'xterm: fallback terminal for Super+Enter'
            'slock: screen locker for the power menu')
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/v$pkgver.tar.gz")
sha256sums=('SKIP')

build() {
    cd "$pkgname-$pkgver"
    make
}

check() {
    cd "$pkgname-$pkgver"
    make check
}

package() {
    cd "$pkgname-$pkgver"
    make DESTDIR="$pkgdir" PREFIX=/usr install
}
