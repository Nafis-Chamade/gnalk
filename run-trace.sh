#!/bin/bash
# Run gnalk with all trace logging enabled

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Enable all trace categories
export GNALK_TRACE="1"

# Forward to main run script
exec "$SCRIPT_DIR/run.sh" "$@"
