# Gnalk

GTK4/libadwaita Nextcloud Talk client in C (Meson build).

See also: [`docs/call-connection-sequence.md`](docs/call-connection-sequence.md) — full HTTP/WebSocket sequence from login to active WebRTC call.

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
├── gnalk-session.[ch]                 # Per-account facade: owns controllers, relays signals
├── gnalk-session-manager.[ch]         # Multi-account management
├── gnalk-notification-service.[ch]    # Desktop notifications (libportal)
│
├── controllers/                        # HTTP, polling, caching (NO UI)
│   ├── gnalk-sync-coordinator         # Centralized sync: timer 20s, push, reconnect
│   ├── gnalk-message-controller       # Message ops, long-polling active room
│   ├── gnalk-message-processor        # Message store manipulation, date/timestamp/avatar computation
│   ├── gnalk-conversation-controller  # Conversation CRUD (create, leave, delete, search)
│   ├── gnalk-participant-controller   # Participant polling (60s), promote/demote/remove
│   ├── gnalk-room-controller          # Room open/close orchestration, tab management
│   ├── gnalk-app-state                # Centralized status (online, loading, sync)
│   └── gnalk-sync-manager             # Offline queue flush
│
├── data/                               # Models and persistence
│   ├── gnalk-database                 # SQLite (conversations, messages, pending)
│   ├── gnalk-conversation-data        # Room metadata model (cached subtitle, JsonObject* params)
│   ├── gnalk-message-data             # Message model (8 GObject properties for live updates)
│   ├── gnalk-account[-manager]        # Account model and persistence
│   └── gnalk-data-repository          # DB access layer for controllers
│
├── network/                            # Network layer
│   ├── gnalk-http-client              # REST API (single HttpCallbackData struct)
│   ├── gnalk-signaling                # WebSocket to signaling server
│   ├── gnalk-webrtc                   # WebRTC connection management
│   ├── gnalk-push-client              # notify_push WebSocket
│   ├── gnalk-network-monitor          # Connectivity detection
│   └── gnalk-sdp-utils                # SDP parsing
│
├── ui/                                 # GTK widgets and views
│   ├── gnalk-window.[ch]              # Main window (NavigationSplitView)
│   ├── gnalk-window-*.c               # Window partials (each includes own headers)
│   ├── gnalk-conversation-*           # Conversation list, row, sidebar, header
│   ├── gnalk-message-*                # Message view, row, input
│   └── ...                             # Dialogs, widgets
│
├── call/                               # Video/audio calls (GStreamer + WebRTC)
│   ├── gnalk-call-window              # Call UI (split into partials)
│   ├── gnalk-call-window-private.h    # CallParticipantRecord struct (consolidated)
│   ├── gnalk-media-pipeline           # GStreamer send (camera, audio, encoding)
│   ├── gnalk-receive-pipeline         # GStreamer receive (decode remote)
│   ├── gnalk-quality-controller       # Adaptive bitrate
│   ├── gnalk-data-channel             # WebRTC data channel (mute status)
│   └── gnalk-screen-capture           # Portal-based screen sharing
│
└── utils/                              # Helpers
    ├── gnalk-keyring                  # libsecret credential storage
    ├── gnalk-log                      # Logging
    ├── gnalk-avatar-cache             # Avatar image cache
    ├── gnalk-async-op                 # Safe async callbacks (GWeakRef + generation counter)
    └── gnalk-message-utils            # Centralized string replacement + JSON serialization
```

## Architecture

**Controller/UI separation**: Window never makes HTTP calls directly. Controllers handle HTTP, polling, and caching; Window displays data from DB and sends user intent to controllers.

**Cache-first**: Window loads from DB immediately, controllers sync in background, emit signals only when data changed.

**Session facade**: Each Session owns its controllers (MC, CC, PC) and relays their signals. Window connects to ~11 Session signals + controller signals via getters. At account switch, only Session signals need reconnection. Session parses `spreed.config.federation.outgoing-enabled` from capabilities to gate federated features.

**MessageProcessor**: Owns message store manipulation (insert, update, sort). Pre-computes display flags (show_date_header, show_timestamp, show_avatar). Maintains hash table indexes for O(1) lookups (msg_index by ID, ref_index by reference_id).

## GTK4 Patterns (MUST follow)

### Factory: setup creates widgets, bind only populates data
- **setup**: Create widgets + connect signals (via `g_signal_connect_object`). Called once.
- **bind**: Read pre-computed data, call setters. No widget creation, no syscalls, no `g_signal_connect`.
- **unbind**: Disconnect data notify, reset widget.
- Use `GtkStack` when a row can display different layouts (e.g. section header vs conversation row).

### GObject properties + notify for live updates
- All mutable display properties have `G_PARAM_EXPLICIT_NOTIFY`.
- Setters guard with `if (old == new) return;` then `g_object_notify_by_pspec()`.
- Multi-property changes: `g_object_freeze_notify()` / `g_object_thaw_notify()`.
- Widget connects `notify` in bind, handler dispatches per-property to update sub-widgets.
- **Never remove/reinsert** to force re-render — mutate in place + notify.

### GtkSortListModel for sorted lists
- Wrap `GListStore` with `GtkSortListModel` + `GtkCustomSorter`.
- On item change: `gtk_sorter_changed()`. On new item: `g_list_store_append()`.
- **Never call `g_list_store_sort()`** — emits `items_changed(0, n, n)` (full invalidation).
- **Never emit `items_changed(0, n, n)`** manually.

### Pre-compute in model, cache in Window
- Display flags (show_avatar, show_timestamp, show_date_header) computed in MessageProcessor.
- Capabilities (edit/delete timeouts) cached in Window struct at session switch.
- File cache state stored on data object, syscall only on first bind if not yet cached.

### Memory
- `g_intern_string()` for bounded-value fields (actor_type, system_message, last_message_type).
- `NULL` instead of `g_strdup("")` for often-empty fields.
- `JsonObject*` stored directly (parsed once), serialized on demand for DB.

## Key Patterns

- Async callbacks via GnalkAsyncOp (GWeakRef + generation counter)
- GObject signals for data binding
- Offline-first: queue operations, flush on reconnect
- Window partials share `gnalk-window-private.h` (minimal includes + forward declares, each .c adds its own)
- Hash table indexes in MessageProcessor for O(1) message lookups
- `g_signal_connect_object()` in factory setup for auto-disconnect safety
- Federated user search: `shareTypes[]=6` in autocomplete, remote avatars via unauthenticated SoupSession, group conversation creation (roomType=2) with deduplication
