#!/bin/sh
# Uninstall gnalk
set -e

APP_ID="net.skilpa.gnalk"

echo "Uninstalling gnalk..."

# Stop running processes
if pgrep -f gnalk >/dev/null 2>&1; then
    echo "Stopping gnalk processes..."
    pkill -f gnalk 2>/dev/null || true
    sleep 1
fi

# Check if installed as APK
if apk info -e gnalk >/dev/null 2>&1; then
    echo "Removing gnalk APK package..."
    doas apk del gnalk
else
    # Manual uninstall from /opt/gnalk/
    echo "Removing /opt/gnalk/..."
    doas rm -rf /opt/gnalk
    doas rm -f /usr/bin/gnalk
    doas rm -f /usr/share/applications/$APP_ID.desktop
fi

# Recompile schemas if needed
if [ -f "/usr/share/glib-2.0/schemas/$APP_ID.gschema.xml" ]; then
    doas rm -f "/usr/share/glib-2.0/schemas/$APP_ID.gschema.xml"
    doas glib-compile-schemas /usr/share/glib-2.0/schemas/ 2>/dev/null || true
fi

# Update icon cache
doas gtk4-update-icon-cache -q -t -f /usr/share/icons/hicolor 2>/dev/null || true

# User data (optional)
if [ -d "$HOME/.local/share/gnalk" ]; then
    printf "Remove user data (~/.local/share/gnalk)? [y/N] "
    read -r answer
    if [ "$answer" = "y" ] || [ "$answer" = "Y" ]; then
        rm -rf "$HOME/.local/share/gnalk"
        echo "User data removed."
    fi
fi

echo "gnalk uninstalled."
