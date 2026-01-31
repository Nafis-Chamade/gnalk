# Maintainer: Skilpa <sylvain@skilpa.net>
pkgname=gnalk
pkgver=0.1.0
pkgrel=0
pkgdesc="Nextcloud Talk client with bundled WebRTC support"
url="https://codeberg.org/skilpa/gnalk"
arch="aarch64"
license="GPL-3.0-or-later"
options="!check !fhs !tracedeps"

# Runtime deps (system libs we still use)
depends="
	gtk4.0
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
	local _staging="$srcdir/staging"

	# 1. Build and install libnice to staging dir
	cd "$srcdir/libnice-$_libnice_ver"
	meson setup build --prefix=/opt/gnalk \
		-Dtests=disabled -Dexamples=disabled -Dgstreamer=enabled
	meson compile -C build
	DESTDIR="$_staging" meson install -C build --no-rebuild

	# Fix .pc files to point to staging paths
	sed -i "s|/opt/gnalk|$_staging/opt/gnalk|g" "$_staging"/opt/gnalk/lib/pkgconfig/*.pc

	# 2. Build gst-plugins-bad webrtc (using staged libnice)
	cd "$srcdir/gst-plugins-bad-$_gst_ver"
	export PKG_CONFIG_PATH="$_staging/opt/gnalk/lib/pkgconfig:$PKG_CONFIG_PATH"
	export LDFLAGS="-L$_staging/opt/gnalk/lib"
	meson setup build --prefix=/opt/gnalk \
		-Dauto_features=disabled \
		-Dwebrtc=enabled -Ddtls=enabled -Dsrtp=enabled -Dsctp=enabled
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

	# Install webrtc gst plugins and libraries from gst-plugins-bad
	cd "$srcdir/gst-plugins-bad-$_gst_ver/build"
	# Plugins
	install -Dm755 ext/webrtc/libgstwebrtc.so "$pkgdir/opt/gnalk/lib/gstreamer-1.0/libgstwebrtc.so"
	install -Dm755 ext/dtls/libgstdtls.so "$pkgdir/opt/gnalk/lib/gstreamer-1.0/libgstdtls.so"
	install -Dm755 ext/srtp/libgstsrtp.so "$pkgdir/opt/gnalk/lib/gstreamer-1.0/libgstsrtp.so"
	install -Dm755 ext/sctp/libgstsctp.so "$pkgdir/opt/gnalk/lib/gstreamer-1.0/libgstsctp.so"
	# Libraries (libgstwebrtcnice bridges libnice to gstreamer-webrtc)
	install -Dm755 gst-libs/gst/webrtc/nice/libgstwebrtcnice-1.0.so* "$pkgdir/opt/gnalk/lib/" 2>/dev/null || \
		cp -a gst-libs/gst/webrtc/nice/libgstwebrtcnice-1.0.so* "$pkgdir/opt/gnalk/lib/"

	# Install gnalk
	cd "$builddir"
	DESTDIR="$pkgdir" meson install -C build --no-rebuild

	# Create wrapper script
	mkdir -p "$pkgdir/opt/gnalk/bin"
	cat > "$pkgdir/opt/gnalk/bin/gnalk" << 'EOF'
#!/bin/sh
export LD_LIBRARY_PATH="/opt/gnalk/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GST_PLUGIN_PATH="/opt/gnalk/lib/gstreamer-1.0${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
export GSETTINGS_SCHEMA_DIR="/opt/gnalk/share/glib-2.0/schemas${GSETTINGS_SCHEMA_DIR:+:$GSETTINGS_SCHEMA_DIR}"
export XDG_DATA_DIRS="/opt/gnalk/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
exec /opt/gnalk/libexec/gnalk "$@"
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

	# Compile GSettings schemas
	glib-compile-schemas "$pkgdir/opt/gnalk/share/glib-2.0/schemas/"
}

sha512sums="
23ae86d03d5ca7af6517b1a0140a46dbc0fa860bcbc0a5de607ec6982aafaf31629e65f4066ce668882d98ebfb89cd0b7574170582ba0c73fbc17e1f001518c3  gnalk-0.1.0.tar.gz
2533d3759117ab8fde7cdeb041193324dac7604599a891d47e07a37287af9989be1522c56af52c947598f93b18e60e2cb1b1d9c7ff4cf2bb3e21ce948907f076  libnice-0.1.23.tar.gz
b4f0ca2899052ba48aca2b4364479cfe580f6150e2b2b043e2a0e687409f5529f548fa06dedd73733d857c1243871426668664228d74bbdfcbd7ed35aea24189  gst-plugins-bad-1.24.10.tar.xz
"
