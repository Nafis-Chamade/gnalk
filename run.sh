#!/bin/bash
# Run gnalk without installing

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

# Always update and compile schemas (source may have changed)
mkdir -p "$BUILD_DIR/schemas"
cp "$SCRIPT_DIR/data/net.skilpa.gnalk.gschema.xml" "$BUILD_DIR/schemas/"
glib-compile-schemas "$BUILD_DIR/schemas/"

# Set environment variables
export GSETTINGS_SCHEMA_DIR="$BUILD_DIR/schemas"
export G_MESSAGES_DEBUG=gnalk
#export GST_DEBUG=rtpvp8depay:5,vp8dec:4

# Set up log files
LOG_FILE="$BUILD_DIR/gnalk.log"
HTTP_LOG="$BUILD_DIR/http.log"
WS_LOG="$BUILD_DIR/websocket.log"
NOTIF_LOG="$BUILD_DIR/notifications.log"

# HTTP, WebSocket, and notification logs go to separate files
export GNALK_HTTP_LOG="$HTTP_LOG"
export GNALK_WS_LOG="$WS_LOG"
export GNALK_NOTIF_LOG="$NOTIF_LOG"

# Enable trace logging (set to "1" for all, or comma-separated categories)
# Categories: messages, room_switch, scroll, store, polling, database, ui, network
# Example: export GNALK_TRACE="messages,room_switch,scroll"
# Uncomment the following line to enable all trace logs:
#export GNALK_TRACE="1"

# Clear log files for fresh run
> "$LOG_FILE"
> "$HTTP_LOG"
> "$WS_LOG"
> "$NOTIF_LOG"

echo "Logging to:"
echo "  Main log: $LOG_FILE"
echo "  HTTP log: $HTTP_LOG"
echo "  WebSocket log: $WS_LOG"
echo "  Notifications log: $NOTIF_LOG"
if [ -n "$GNALK_TRACE" ]; then
    echo "  Trace enabled: $GNALK_TRACE"
fi
echo ""

exec "$BUILD_DIR/src/gnalk" "$@" 2>&1 | tee "$LOG_FILE"
