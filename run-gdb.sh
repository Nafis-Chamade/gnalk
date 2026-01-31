#!/bin/bash
# Run gnalk under GDB for debugging crashes

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

# Always update and compile schemas (source may have changed)
mkdir -p "$BUILD_DIR/schemas"
cp "$SCRIPT_DIR/data/net.skilpa.gnalk.gschema.xml" "$BUILD_DIR/schemas/"
glib-compile-schemas "$BUILD_DIR/schemas/"

# Set environment variables
export GSETTINGS_SCHEMA_DIR="$BUILD_DIR/schemas"
export G_MESSAGES_DEBUG=gnalk

# Make GLib criticals fatal so GDB catches them (not warnings, to avoid CSS parser noise)
export G_DEBUG=fatal-criticals

# Enable debuginfod for automatic debug symbol download (Ubuntu 24.04+)
export DEBUGINFOD_URLS="https://debuginfod.ubuntu.com"

# Run under GDB
# - "run" starts the app
# - "bt" prints backtrace when it crashes
# - saves output to crash.log
LOG_FILE="$BUILD_DIR/crash.log"
echo "Running under GDB. Output will be saved to $LOG_FILE"
echo "When it crashes, GDB will show a backtrace."
echo "Debug symbols will be downloaded automatically from debuginfod.ubuntu.com"
echo "Type 'quit' to exit GDB."
echo ""

gdb -ex "set debuginfod enabled on" \
    -ex "set pagination off" \
    -ex "run" \
    -ex "bt full" \
    -ex "info registers" \
    "$BUILD_DIR/src/gnalk" "$@" 2>&1 | tee "$LOG_FILE"
