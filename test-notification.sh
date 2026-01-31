#!/bin/bash
# Trigger opening a specific room via D-Bus action
ROOM_TOKEN="${1:-test-room}"

cd "$(dirname "$0")"

# Set schema dir for development
export GSETTINGS_SCHEMA_DIR="$PWD/data:${GSETTINGS_SCHEMA_DIR:-}"

echo "Opening room: $ROOM_TOKEN"

gdbus call --session --dest net.skilpa.gnalk \
    --object-path /net/skilpa/gnalk \
    --method org.gtk.Actions.Activate "open-room" "[<'$ROOM_TOKEN'>]" {}
