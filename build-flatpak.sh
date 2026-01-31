#!/bin/bash
# Build and install Flatpak locally
set -e

# Check if flatpak-builder is installed
if ! command -v flatpak-builder &> /dev/null; then
    echo "Error: flatpak-builder is not installed"
    echo "Install with: sudo apt install flatpak-builder"
    exit 1
fi

# Check if GNOME SDK is installed
if ! flatpak info org.gnome.Sdk//49 &> /dev/null; then
    echo "Installing GNOME SDK 49..."
    flatpak install -y flathub org.gnome.Sdk//49 org.gnome.Platform//49
fi

# Build and install
flatpak-builder --user --install --force-clean build-dir net.skilpa.gnalk.json

echo ""
echo "Build complete! Run with: flatpak run net.skilpa.gnalk"
