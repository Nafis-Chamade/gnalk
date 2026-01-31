#!/bin/sh
# Clean and prepare for rebuild

pkill -f gnalk 2>/dev/null || true
apk del gnalk 2>/dev/null || true
rm -rf /opt/gnalk
rm -f /usr/bin/gnalk
rm -f /usr/share/applications/net.skilpa.gnalk.desktop
rm -f /usr/share/dbus-1/services/net.skilpa.gnalk.service
sed -i '/gnalk/d' /etc/apk/world 2>/dev/null || true
rm -f /home/user/packages/claude/aarch64/gnalk*.apk
rm -f /home/user/packages/claude/aarch64/APKINDEX*

echo "Cleaned. Run: abuild -d -F"
