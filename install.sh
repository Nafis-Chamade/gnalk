#!/bin/sh
# Install gnalk on postmarketOS
# Usage: curl -sL https://codeberg.org/skilpa/gnalk/raw/branch/main/install.sh | doas sh
set -e

echo "Downloading gnalk..."
rm -f /tmp/gnalk.apk
curl -fL https://nc.skilpa.net/s/dFoG8HxqYXj453L/download -o /tmp/gnalk.apk

echo "Installing gnalk..."
apk add --allow-untrusted /tmp/gnalk.apk
rm -f /tmp/gnalk.apk

echo "Done! Run 'gnalk' to start."
