#!/bin/bash
# Start/restart gnalk in service mode (for development)

cd "$(dirname "$0")"

# Kill any existing instance
gdbus call --session --dest net.skilpa.gnalk \
    --object-path /net/skilpa/gnalk \
    --method org.gtk.Actions.Activate "quit" [] {} 2>/dev/null || true
pkill -f "gnalk.*service" 2>/dev/null || true

# Wait for process to exit
sleep 0.5

# Build
meson compile -C build || exit 1

# Compile schemas for development
glib-compile-schemas data/

# Set schema dir for development (not installed)
export GSETTINGS_SCHEMA_DIR="$PWD/data:${GSETTINGS_SCHEMA_DIR:-}"

# Enable debug messages
export G_MESSAGES_DEBUG=all

# Log file for service mode
LOG_FILE="$PWD/build/gnalk-service.log"

# Start in service mode (background, no window) with logging
./build/src/gnalk --gapplication-service > "$LOG_FILE" 2>&1 &
echo "Service started (PID: $!)"
echo "Logs: tail -f $LOG_FILE"
echo "Test with: ./run-ui.sh (open window) or ./test-notification.sh <room-token>"
