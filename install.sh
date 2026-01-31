#!/bin/sh
# Install gnalk on postmarketOS
# Usage: doas ./install.sh
set -e

echo "Installing gnalk dependencies..."
apk add \
    gtk4.0 \
    libadwaita \
    libsoup3 \
    json-glib \
    sqlite-libs \
    libsecret \
    gstreamer \
    gst-plugins-base \
    gst-plugins-good \
    pipewire

echo "Downloading gnalk..."
rm -f /tmp/gnalk.apk
curl -fL https://nc.skilpa.net/s/GC3543yykXKfByt/download -o /tmp/gnalk.apk

echo "Installing gnalk..."
# Extract directly since APK has wrong dep name
cd /
tar -xzf /tmp/gnalk.apk --exclude='.SIGN*' --exclude='.PKGINFO'
rm -f /tmp/gnalk.apk

# Compile schemas
glib-compile-schemas /opt/gnalk/share/glib-2.0/schemas/ 2>/dev/null || true

echo "Done! Run 'gnalk' to start."
