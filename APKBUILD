# Maintainer: Skilpa <sylvain@skilpa.net>
pkgname=gnalk
pkgver=0.1.0
pkgrel=0
pkgdesc="Nextcloud Talk Client"
url="https://codeberg.org/skilpa/gnalk"
arch="all"
license="GPL-3.0-or-later"
# Note: Video calls require webrtcbin which is NOT included in Alpine's
# gst-plugins-bad (libnice version too old). See README.md for manual build.
depends="
	dbus:org.freedesktop.Secrets
	gst-plugins-bad
	gst-plugins-good
	gst-plugins-rs-gtk4
	gstreamer
	libcamera
	libcamera-gstreamer
	libnice-gstreamer
	pipewire
	"
makedepends="
	desktop-file-utils
	glib-dev
	gst-plugins-bad-dev
	gst-plugins-base-dev
	gst-plugins-good
	gstreamer-dev
	gtk4.0-dev
	json-glib-dev
	libadwaita-dev
	libcamera-dev
	libcamera-gstreamer
	libnice-dev
	libnice-gstreamer
	libsecret-dev
	libsoup3-dev
	meson
	pipewire-dev
	sqlite-dev
	"
subpackages="$pkgname-lang"
source="$pkgname-$pkgver.tar.gz::https://codeberg.org/skilpa/gnalk/archive/v$pkgver.tar.gz"
options="!check"

build() {
	abuild-meson . output
	meson compile -C output
}

package() {
	DESTDIR="$pkgdir" meson install --no-rebuild -C output
}

sha512sums=""
