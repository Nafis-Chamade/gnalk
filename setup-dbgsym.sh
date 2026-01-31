#!/bin/bash
# Setup debug symbols for Ubuntu 24.04 (noble)
# Run with: sudo ./setup-dbgsym.sh

set -e

if [ "$EUID" -ne 0 ]; then
    echo "Please run with sudo: sudo ./setup-dbgsym.sh"
    exit 1
fi

echo "=== Installing debug symbol keyring ==="
apt install -y ubuntu-dbgsym-keyring

echo "=== Adding ddebs repository ==="
cat > /etc/apt/sources.list.d/ddebs.list << 'EOF'
deb http://ddebs.ubuntu.com noble main restricted universe multiverse
deb http://ddebs.ubuntu.com noble-updates main restricted universe multiverse
EOF

echo "=== Importing signing key ==="
apt-key adv --keyserver keyserver.ubuntu.com --recv-keys F2EDC64DC5AEE1F6B9C621F0C8CAB6595FDFF622 2>/dev/null || true

echo "=== Updating package list ==="
apt update

echo "=== Installing debug symbols ==="
apt install -y libglib2.0-0t64-dbgsym libsoup-3.0-0-dbgsym || {
    echo "Trying alternative package names..."
    apt-cache search dbgsym | grep -E "glib2.0|soup-3.0"
    echo ""
    echo "Install manually with: sudo apt install <package-name>"
}

echo "=== Done ==="
echo "You can now run GDB and get full stack traces."
