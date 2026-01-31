#!/bin/bash
# Open UI window (simulates clicking app icon)
# This activates the running service instance, creating a window
# If no instance running, starts one with window

cd "$(dirname "$0")"

# Set schema dir for development
export GSETTINGS_SCHEMA_DIR="$PWD/data:${GSETTINGS_SCHEMA_DIR:-}"

gdbus call --session --dest net.skilpa.gnalk \
    --object-path /net/skilpa/gnalk \
    --method org.gtk.Application.Activate []

# Alternative: just run the binary (GApplication handles single-instance)
# ./build/src/gnalk
