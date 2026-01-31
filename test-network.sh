#!/bin/bash
# Test network loss for gnalk (works mid-conversation)
# Usage: ./test-network.sh [block|unblock|status]

COMMENT="gnalk-network-test"
SERVER_HOST="nc.skilpa.net"

block_network() {
    echo "Blocking network to $SERVER_HOST..."

    # Resolve hostname to IP
    SERVER_IP=$(getent hosts "$SERVER_HOST" | awk '{print $1}' | head -1)

    if [ -z "$SERVER_IP" ]; then
        echo "Error: Could not resolve $SERVER_HOST"
        exit 1
    fi

    echo "Blocking connections to $SERVER_IP ($SERVER_HOST)"

    # Use UFW to block (works with Ubuntu's firewall)
    sudo ufw deny out to "$SERVER_IP" comment "$COMMENT"
    sudo ufw deny in from "$SERVER_IP" comment "$COMMENT"

    echo "Network blocked. Run '$0 unblock' to restore."
}

unblock_network() {
    echo "Unblocking network..."

    # Resolve hostname to IP
    SERVER_IP=$(getent hosts "$SERVER_HOST" | awk '{print $1}' | head -1)

    if [ -n "$SERVER_IP" ]; then
        # Remove UFW rules
        sudo ufw delete deny out to "$SERVER_IP" 2>/dev/null
        sudo ufw delete deny in from "$SERVER_IP" 2>/dev/null
    fi

    echo "Network restored."
}

show_status() {
    echo "=== UFW rules for $SERVER_HOST ==="
    sudo ufw status numbered 2>/dev/null | grep -i "$COMMENT" || echo "No blocking rules found"
    echo ""
    echo "=== Server resolution ==="
    echo "$SERVER_HOST -> $(getent hosts "$SERVER_HOST" | awk '{print $1}' | head -1)"
}

start_offline_flatpak() {
    echo "Starting gnalk without network (Flatpak sandbox)..."
    flatpak run --unshare=network net.skilpa.gnalk
}

start_offline_classic() {
    echo "Starting gnalk without network (unshare)..."
    if [ -x "./build/src/gnalk" ]; then
        # Use sudo unshare to create isolated network namespace
        # Preserve display and dbus for GTK to work
        sudo unshare --net env \
            HOME="$HOME" \
            DISPLAY="$DISPLAY" \
            WAYLAND_DISPLAY="$WAYLAND_DISPLAY" \
            XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
            DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" \
            GSETTINGS_SCHEMA_DIR="$PWD/build/schemas" \
            G_MESSAGES_DEBUG=gnalk \
            ./build/src/gnalk
    else
        echo "Error: ./build/src/gnalk not found. Run 'meson compile -C build' first."
        exit 1
    fi
}

case "${1:-status}" in
    block|off|down)
        block_network
        ;;
    unblock|on|up)
        unblock_network
        ;;
    status)
        show_status
        ;;
    offline)
        start_offline_classic
        ;;
    offline-flatpak)
        start_offline_flatpak
        ;;
    *)
        echo "Usage: $0 [block|unblock|status|offline|offline-flatpak]"
        echo ""
        echo "Commands:"
        echo "  block          - Block connections to Nextcloud server (mid-conversation test)"
        echo "  unblock        - Restore network access"
        echo "  status         - Show current state"
        echo "  offline        - Start classic build with no network (unshare)"
        echo "  offline-flatpak - Start Flatpak with no network (sandbox)"
        echo ""
        echo "For mid-conversation test: block/unblock"
        echo "For startup without network: offline or offline-flatpak"
        exit 1
        ;;
esac
