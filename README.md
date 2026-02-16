# Gnalk

A native GTK4/libadwaita client for [Nextcloud Talk](https://nextcloud.com/talk/), written in C.

Gnalk provides messaging and video/audio calling with a modern GNOME-style interface, designed for both desktop and mobile Linux devices.

> [!WARNING]
> This project was developed with AI assistance. If you prefer a purely human-written alternative, you may be interested in [Nutsy](https://codeberg.org/skilpa/nutsy), an earlier Nextcloud Talk client I started. However, please note that Nutsy is no longer maintained and remains incomplete.

## Quick Install (postmarketOS)

```bash
curl -sL https://codeberg.org/skilpa/gnalk/raw/branch/main/install.sh | doas sh
```

This downloads and installs a self-contained APK that bundles libnice and WebRTC GStreamer plugins, avoiding conflicts with system libraries. Uninstall with `doas apk del gnalk`.

## Features

### Messaging
- Send and receive text messages with markdown support
- Reply to messages (quoting)
- Edit and delete messages (time-limited based on server settings)
- Emoji reactions
- File sharing with upload progress
- Voice message recording and playback
- System messages (user joined, left, etc.)
- Read status tracking
- Message history with lazy loading

### Video/Audio Calls
- WebRTC-based calls with MCU (Janus) and P2P support
- Camera and microphone input via GStreamer
- Screen sharing (via xdg-desktop-portal)
- Adaptive bitrate quality control
- Multi-party calls with participant tiles
- Call controls (mute audio/video, hang up)
- Incoming call notifications

### Real-time Updates
- Push notifications via Nextcloud's notify_push
- Typing indicators
- Online/offline status
- Unread message counts with badges

### Multi-Account Support
- Manage multiple Nextcloud accounts
- Quick account switching
- Per-account session management
- Secure credential storage via libsecret

### Modern UI
- GTK4 + libadwaita for native GNOME experience
- Responsive split-view layout (adapts to mobile/desktop)
- Conversation list with search
- Dark/light/auto theme support
- Image gallery for media viewing
- Preferences dialog for camera, audio, and quality settings

### Offline Support
- Messages queued when offline
- Automatic sync on reconnect
- Local SQLite database for conversation/message caching
- Cache-first architecture for instant UI loading

### Desktop Integration
- Desktop notifications (via libportal)
- Background sync service mode
- Autostart support
- GNOME Online Accounts integration (optional)

## Requirements

- **Nextcloud server** with Talk app installed
- **External signaling server** (recommended for video calls) with Janus MCU


## Building

### Option 1: Classic Build (Meson)

#### Dependencies

**Required:**
- GTK 4 (>= 4.10)
- libadwaita 1 (>= 1.4)
- libsoup 3 (>= 3.0)
- json-glib 1.0
- sqlite3
- meson (>= 0.59.0)

**Optional:**
- libsecret-1 — Secure credential storage (keyring)
- GStreamer 1.0 — Media processing for calls:
  - gstreamer-video-1.0
  - gstreamer-app-1.0
  - gstreamer-sdp-1.0
  - gstreamer-webrtc-1.0
  - gstreamer-rtp-1.0
- libportal-gtk4 (>= 0.6) — Desktop portals (screen sharing, notifications)
- goa-1.0 — GNOME Online Accounts integration

**On Fedora/RHEL:**
```bash
sudo dnf install gtk4-devel libadwaita-devel libsoup3-devel json-glib-devel sqlite-devel \
    libsecret-devel gstreamer1-devel gstreamer1-plugins-base-devel gstreamer1-plugins-bad-free-devel \
    libportal-gtk4-devel gnome-online-accounts-devel
```

**On Debian/Ubuntu:**
```bash
sudo apt install libgtk-4-dev libadwaita-1-dev libsoup-3.0-dev libjson-glib-dev libsqlite3-dev \
    libsecret-1-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev \
    libportal-gtk4-dev libgoa-1.0-dev
```

#### Compile and Run

```bash
meson setup build
meson compile -C build
./build/src/gnalk
```

#### Install (optional)

```bash
sudo meson install -C build
gnalk
```

### Option 2: Flatpak Build

#### Prerequisites

```bash
# Install flatpak-builder
sudo apt install flatpak-builder  # Debian/Ubuntu
sudo dnf install flatpak-builder  # Fedora

# Install GNOME SDK
flatpak install flathub org.gnome.Sdk//49 org.gnome.Platform//49
```

#### Build and Install

```bash
./build-flatpak.sh
# Or manually:
flatpak-builder --user --install --force-clean build-dir net.skilpa.gnalk.json
```

#### Run

```bash
flatpak run net.skilpa.gnalk
```

### Option 3: Alpine/postmarketOS Build

For mobile devices (e.g., OnePlus 6 with postmarketOS).

#### Why Bundle Libraries?

Alpine's `gst-plugins-bad` package doesn't include WebRTC plugins, and the system `libnice` may conflict with GStreamer's requirements. The APKBUILD solves this by bundling:

- **libnice 0.1.23** — ICE/STUN/TURN implementation for NAT traversal
- **gst-plugins-bad (WebRTC subset)** — webrtc, dtls, srtp, sctp plugins

These are installed to `/opt/gnalk/lib/` with a wrapper script that sets `LD_LIBRARY_PATH` and `GST_PLUGIN_PATH`, isolating them from system libraries.

#### Install Build Dependencies

```bash
doas apk add abuild meson build-base glib-dev gtk4-dev libadwaita-dev libsoup3-dev \
    json-glib-dev sqlite-dev libsecret-dev gstreamer-dev gst-plugins-base-dev \
    gnutls-dev libsrtp-dev openssl-dev pipewire-dev desktop-file-utils
```

#### Build APK

```bash
abuild -d -F
```

#### Install

```bash
doas apk add --allow-untrusted ~/packages/claude/aarch64/gnalk-0.1.0-r0.apk
```

#### Development Scripts

| Script | Purpose |
|--------|---------|
| `clean.sh` | Dev cleanup: removes installed package, /opt/gnalk, and build artifacts. Run with `doas`. |
| `install.sh` | Downloads and installs the APK from the online repository. Run with `doas`. |
| `uninstall.sh` | User-friendly uninstall: handles APK or manual installs, prompts about user data. |

Full rebuild cycle:
```bash
doas ./clean.sh && abuild -d -F && doas apk add --allow-untrusted ~/packages/claude/aarch64/gnalk-0.1.0-r0.apk
```

#### Package Structure

```
/opt/gnalk/
├── bin/gnalk                    # Wrapper script (sets LD_LIBRARY_PATH, GST_PLUGIN_PATH)
├── libexec/gnalk                # Actual binary
├── lib/
│   ├── libnice.so.10            # Bundled libnice 0.1.23
│   ├── libgstwebrtcnice-1.0.so  # Bridge between libnice and GStreamer WebRTC
│   └── gstreamer-1.0/
│       ├── libgstnice.so        # libnice GStreamer plugin
│       ├── libgstwebrtc.so      # WebRTC plugin
│       ├── libgstdtls.so        # DTLS plugin
│       ├── libgstsrtp.so        # SRTP plugin
│       └── libgstsctp.so        # SCTP plugin (data channels)
└── share/...
/usr/bin/gnalk -> /opt/gnalk/bin/gnalk
/usr/share/applications/net.skilpa.gnalk.desktop
/usr/share/dbus-1/services/net.skilpa.gnalk.service
```

#### Verify WebRTC

```bash
GST_PLUGIN_PATH=/opt/gnalk/lib/gstreamer-1.0 gst-inspect-1.0 webrtcbin
```

#### Screen Sharing (Mobile)

Screen sharing requires `xdg-desktop-portal-wlr` configuration:

```bash
# Install required tools
sudo apk add slurp grim

# Configure for mobile (single screen, no interactive chooser)
mkdir -p ~/.config/xdg-desktop-portal-wlr
cat > ~/.config/xdg-desktop-portal-wlr/config << 'EOF'
[screencast]
chooser_type=none
max_fps=30
EOF

# Restart the portal service
systemctl --user restart xdg-desktop-portal-wlr
```

## Configuration

Gnalk stores settings in GSettings under `net.skilpa.gnalk`:

| Setting | Description | Default |
|---------|-------------|---------|
| `window-width` | Window width | 900 |
| `window-height` | Window height | 600 |
| `autologin-enabled` | Auto-login on startup | false |
| `preferred-video-device` | Camera device | (auto) |
| `preferred-audio-device` | Microphone device | (auto) |
| `camera-quality` | Video quality (0=Low, 1=Medium, 2=High) | 2 |
| `color-scheme` | Theme (0=Auto, 1=Light, 2=Dark) | 0 |
| `max-loaded-rooms` | Maximum rooms kept in memory | 5 |

Credentials are stored securely via libsecret (GNOME Keyring).

## Architecture

Gnalk follows a clean controller/UI separation:

- **Controllers** (`src/controllers/`) — Handle HTTP requests, polling, and caching. No UI code.
- **Data** (`src/data/`) — Models and SQLite persistence layer.
- **Network** (`src/network/`) — REST API client, WebSocket signaling, WebRTC management.
- **UI** (`src/ui/`) — GTK widgets and views. Displays data from DB, sends user intent to controllers.
- **Call** (`src/call/`) — GStreamer pipelines for video/audio calls.

Key patterns:
- **Cache-first**: UI loads from database immediately; controllers sync in background
- **Offline-first**: Operations queued when offline, flushed on reconnect
- **Session-per-account**: Each account has independent HttpClient, Signaling, and state machine

See `CLAUDE.md` for detailed architecture documentation.
See [`docs/call-connection-sequence.md`](docs/call-connection-sequence.md) for the full HTTP/WebSocket connection sequence (login → signaling → WebRTC call).

## License

[GPL-3.0-or-later](LICENSE)

## Links

- **Repository**: https://codeberg.org/skilpa/gnalk
- **Nextcloud Talk**: https://nextcloud.com/talk/
