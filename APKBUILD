# Maintainer: Skilpa <sylvain@skilpa.net>
pkgname=gnalk
pkgver=0.1.0
pkgrel=0
pkgdesc="Nextcloud Talk client with bundled WebRTC support"
url="https://codeberg.org/skilpa/gnalk"
arch="aarch64"
license="GPL-3.0-or-later"
options="!check"

# Runtime deps (system libs we still use)
depends="
	gtk4
	libadwaita
	libsoup3
	json-glib
	sqlite-libs
	libsecret
	gstreamer
	gst-plugins-base
	gst-plugins-good
	pipewire
	"

# Build deps
makedepends="
	meson
	gtk4-dev
	libadwaita-dev
	libsoup3-dev
	json-glib-dev
	sqlite-dev
	libsecret-dev
	gstreamer-dev
	gst-plugins-base-dev
	glib-dev
	gnutls-dev
	libsrtp-dev
	openssl-dev
	desktop-file-utils
	"

# Sources: gnalk + libnice + gst-plugins-bad
_libnice_ver=0.1.23
_gst_ver=1.24.10
source="
	$pkgname-$pkgver.tar.gz::https://codeberg.org/skilpa/gnalk/archive/v$pkgver.tar.gz
	https://libnice.freedesktop.org/releases/libnice-$_libnice_ver.tar.gz
	https://gstreamer.freedesktop.org/src/gst-plugins-bad/gst-plugins-bad-$_gst_ver.tar.xz
	"

builddir="$srcdir/gnalk"

build() {
	# 1. Build libnice to private prefix
	cd "$srcdir/libnice-$_libnice_ver"
	meson setup build --prefix=/opt/gnalk \
		-Dtests=disabled -Dexamples=disabled -Dgstreamer=enabled
	meson compile -C build

	# 2. Build gst-plugins-bad webrtc (using our libnice)
	cd "$srcdir/gst-plugins-bad-$_gst_ver"
	export PKG_CONFIG_PATH="$srcdir/libnice-$_libnice_ver/build/meson-private"
	meson setup build --prefix=/opt/gnalk \
		-Dauto_features=disabled \
		-Dwebrtc=enabled -Ddtls=enabled -Dsrtp=enabled
	meson compile -C build

	# 3. Build gnalk
	cd "$builddir"
	meson setup build --prefix=/opt/gnalk \
		--bindir=libexec \
		-Dlibdir=lib
	meson compile -C build
}

package() {
	# Install libnice libs
	cd "$srcdir/libnice-$_libnice_ver"
	DESTDIR="$pkgdir" meson install -C build --no-rebuild

	# Install only webrtc gst plugins (not full gst-plugins-bad)
	cd "$srcdir/gst-plugins-bad-$_gst_ver/build"
	install -Dm755 ext/webrtc/libgstwebrtc.so "$pkgdir/opt/gnalk/lib/gstreamer-1.0/libgstwebrtc.so"
	install -Dm755 ext/dtls/libgstdtls.so "$pkgdir/opt/gnalk/lib/gstreamer-1.0/libgstdtls.so"
	install -Dm755 ext/srtp/libgstsrtp.so "$pkgdir/opt/gnalk/lib/gstreamer-1.0/libgstsrtp.so"

	# Install gnalk
	cd "$builddir"
	DESTDIR="$pkgdir" meson install -C build --no-rebuild

	# Create wrapper script
	mkdir -p "$pkgdir/opt/gnalk/bin"
	cat > "$pkgdir/opt/gnalk/bin/gnalk" << 'EOF'
#!/bin/sh
export LD_LIBRARY_PATH="/opt/gnalk/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GST_PLUGIN_PATH="/opt/gnalk/lib/gstreamer-1.0${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
exec /opt/gnalk/libexec/gnalk-bin "$@"
EOF
	chmod 755 "$pkgdir/opt/gnalk/bin/gnalk"

	# Symlink to /usr/bin for PATH
	mkdir -p "$pkgdir/usr/bin"
	ln -s /opt/gnalk/bin/gnalk "$pkgdir/usr/bin/gnalk"

	# Fix .desktop file path
	sed -i 's|Exec=gnalk|Exec=/opt/gnalk/bin/gnalk|' \
		"$pkgdir/opt/gnalk/share/applications/net.skilpa.gnalk.desktop"

	# Move desktop file to standard location
	mkdir -p "$pkgdir/usr/share/applications"
	mv "$pkgdir/opt/gnalk/share/applications/net.skilpa.gnalk.desktop" \
		"$pkgdir/usr/share/applications/"
}

sha512sums=""
