# Gnalk

GTK4/libadwaita Nextcloud Talk client in C (Meson build).

## Build & Run

```bash
meson setup build && meson compile -C build
```

User starts the app with `./run.sh`. Logs: `build/gnalk.log`.

## Git

Never commit on your own. Wait for the user to handle commits.

## Source Structure

```
src/
├── main.c, gnalk-application.[ch]     # App singleton, GApplication
├── gnalk-session.[ch]                 # Per-account: HttpClient, Signaling, state machine
├── gnalk-session-manager.[ch]         # Multi-account management
├── gnalk-notification-service.[ch]    # Desktop notifications (libportal)
│
├── controllers/                        # HTTP, polling, caching (NO UI)
│   ├── gnalk-sync-coordinator         # Centralized sync: timer 20s, push, reconnect
│   ├── gnalk-message-controller       # Message ops, long-polling active room
│   ├── gnalk-conversation-controller  # Conversation CRUD (create, leave, delete)
│   ├── gnalk-participant-controller   # Participant polling (60s)
│   ├── gnalk-room-controller          # Room open/close orchestration
│   ├── gnalk-app-state                # Centralized state
│   └── gnalk-sync-manager             # Offline queue flush
│
├── data/                               # Models and persistence
│   ├── gnalk-database                 # SQLite (conversations, messages, pending)
│   ├── gnalk-conversation-data        # Room metadata model
│   ├── gnalk-message-data             # Message model (replies, reactions, status)
│   ├── gnalk-account[-manager]        # Account model and persistence
│   └── gnalk-data-repository          # DB access layer for controllers
│
├── network/                            # Network layer
│   ├── gnalk-http-client              # REST API (Nextcloud Talk endpoints)
│   ├── gnalk-signaling                # WebSocket to signaling server
│   ├── gnalk-webrtc                   # WebRTC connection management
│   ├── gnalk-push-client              # notify_push WebSocket
│   ├── gnalk-network-monitor          # Connectivity detection
│   └── gnalk-sdp-utils                # SDP parsing
│
├── ui/                                 # GTK widgets and views
│   ├── gnalk-window.[ch]              # Main window (NavigationSplitView)
│   ├── gnalk-window-*.c               # Window partials (login, messages, etc.)
│   ├── gnalk-conversation-*           # Conversation list, row, sidebar, header
│   ├── gnalk-message-*                # Message view, row, input
│   └── ...                             # Dialogs, widgets
│
├── call/                               # Video/audio calls (GStreamer + WebRTC)
│   ├── gnalk-call-window              # Call UI (split into partials)
│   ├── gnalk-media-pipeline           # GStreamer send (camera, audio, encoding)
│   ├── gnalk-receive-pipeline         # GStreamer receive (decode remote)
│   ├── gnalk-quality-controller       # Adaptive bitrate
│   ├── gnalk-data-channel             # WebRTC data channel (mute status)
│   └── gnalk-screen-capture           # Portal-based screen sharing
│
└── utils/                              # Helpers
    ├── gnalk-keyring                  # libsecret credential storage
    ├── gnalk-log                      # Logging
    └── gnalk-avatar-cache             # Avatar image cache
```

## Architecture

**Controller/UI separation**: Window never makes HTTP calls directly. Controllers handle HTTP, polling, and caching; Window displays data from DB and sends user intent to controllers.

**Cache-first**: Window loads from DB immediately, controllers sync in background, emit signals only when data changed.

**Session owns resources**: Each Session has its own HttpClient, Signaling, and state machine. SessionManager coordinates multiple accounts.

## Key Patterns

- Async callbacks (not async/await)
- GObject signals for data binding
- Offline-first: queue operations, flush on reconnect
- Generation counters: discard stale responses
