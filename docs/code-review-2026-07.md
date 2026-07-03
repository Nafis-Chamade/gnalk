# Gnalk — Full Codebase Audit & Pre-release Work Plan (July 2026)

> **Audience: an AI model (or human) executing the fixes.** This document is
> self-contained: read it top to bottom before starting any chantier. Findings
> reference **file + function name** (line numbers drift — always locate with
> grep). Every chantier has step-by-step instructions, reference patterns to
> copy, known pitfalls, and a definition of done.
>
> Audit basis: full read of all 181 source files (~75k lines) by 6 parallel
> review passes + web research on GTK 4.12→4.20, libadwaita 1.5→1.8,
> GStreamer 1.24/1.26, and postmarketOS/OnePlus 6 (mid-2026 state).
> System at audit time: GTK 4.20.1, libadwaita 1.8.0. Project targets
> gtk4 >= 4.10, adwaita >= 1.4 (raise when adopting new APIs).

---

## 0. Overall verdict

**The foundations are good. No rewrite needed.** Macro-architecture matches
CLAUDE.md: session facade, controller/UI separation, DB-mediated data flow,
factory setup/bind discipline, clean GStreamer module decomposition, and
excellent deprecation hygiene (no GtkDialog / AdwMessageDialog /
GtkFileChooser anywhere — already AdwDialog / AdwAlertDialog / GtkFileDialog).

The weakness is **execution consistency**: the project invented the right
patterns (GnalkAsyncOp, generation counters, GWeakRef) but applies them in
~30% of the places that need them. Five systemic work streams ("chantiers")
fix this without touching the architecture.

### Chantier status

| # | Chantier | Status |
|---|----------|--------|
| 1 | HTTP cancellation contract + single dispatch helper | ✅ **DONE** (2026-07-02, see §2) |
| 2 | Generalize GnalkAsyncOp / async-lifetime safety | ⬜ TODO — §3 |
| 3 | Thread-safety: GStreamer→UI marshalling + async call teardown | ⬜ TODO — §4 |
| 4 | SQLite hardening (WAL, transactions, versioned migrations) | ⬜ TODO — §5 |
| 5 | Log hygiene (levels + zero secrets) | ⬜ TODO — §6 |
| 6 | message-row decomposition + scroll performance | ⬜ TODO — §7 |
| 7 | Zero-copy video (gtk4paintablesink + GraphicsOffload + VA-API) + call error recovery | ⬜ TODO — §8 |
| 8 | Mobile readiness (breakpoints, battery, camera abstraction) | ⬜ TODO — §9 |
| — | Bug backlog (independent point fixes) | ⬜ TODO — §10 |
| — | GTK/Adwaita modernization menu | §11 |

Chantiers 2–5 are ordered by leverage. 4 (SQLite) is independent of 2–3 and
can be done in any order. Within a chantier, follow the steps in order.

### Ground rules (from CLAUDE.md — apply to every chantier)

- Build with `meson compile -C build`. User runs the app via `./run.sh`;
  logs land in `build/gnalk.log`. **Never commit** — the user handles git.
- GTK4 factory rules: setup creates widgets + connects (via
  `g_signal_connect_object`), bind only calls setters (no widget creation,
  no syscalls, no `g_signal_connect`), unbind disconnects and resets.
- Never `g_list_store_sort`, never emit `items_changed(0,n,n)`.
- Mutate models in place + `notify`; never remove/reinsert rows.
- After any nontrivial change: compile, then smoke-test by launching the app
  briefly (`timeout 8 ./run.sh`) and reading `build/gnalk.log` for the
  startup sequence (auth chain → push connect → signaling → sync → poll).

---

## 1. Architecture map (what owns what)

```
GnalkApplication ──► GnalkSessionManager ──► GnalkSession (per account)
                                              ├── GnalkHttpClient      (network/)
                                              ├── GnalkSignaling       (WebSocket HPB)
                                              ├── GnalkPushClient      (notify_push WS)
                                              ├── GnalkSyncCoordinator (20 s timer)
                                              ├── GnalkMessageController (long-poll)
                                              ├── GnalkConversationController
                                              ├── GnalkParticipantController (60 s poll)
                                              ├── GnalkSyncManager     (offline queue)
                                              └── GnalkDataRepository ──► GnalkDatabase (SQLite)

GnalkWindow (ui/, 10 partials sharing gnalk-window-private.h, ~90-field struct)
GnalkCallWindow (call/, partials) ──► GnalkWebrtc ──► media/receive pipelines (GStreamer)
```

Data flow invariant: Session saves conversations to DB →
`DataRepository::data-changed` → Window reloads from DB. No in-memory list
stores in Session/AppState.

---

## 2. ✅ DONE — Chantier 1: HTTP cancellation contract (reference for everything else)

Completed 2026-07-02. **Read this section carefully: later chantiers rely on
the new contract.**

### The new contract (src/network/gnalk-http-client.c)

`dispatch_request()` is the single entry point for all requests. Its
guarantee, documented at the function definition:

> The user callback is invoked **exactly once for every outcome** — success,
> HTTP error, network error, **cancellation** (`G_IO_ERROR_CANCELLED`), or
> message-creation failure — as long as the client object is alive. Only
> client destruction swallows the callback.

Consequences every callback in the codebase must honor:

1. **CANCELLED is a normal event** (room switch → `cancel_room_requests`,
   account switch / disconnect → `cancel_pending`). Callbacks must free their
   `user_data` and reset in-flight flags, but emit **no** failure signals,
   toasts, retries, or state transitions.
2. The msg-creation-failure path invokes the callback **synchronously**
   (re-entrantly). Callers must set in-flight flags *before* calling the
   `gnalk_http_client_*` function.
3. The canonical callback prologue (use in all new/edited callbacks):

```c
static void
on_foo_response (GnalkHttpClient *client, JsonNode *response,
                 GError *error, gpointer user_data)
{
  /* CANCELLED check FIRST, before casting user_data — safe even if the
   * owner is already freed (g_error_matches touches only `error`). */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    { /* free user_data if heap-allocated, reset flags via weak owner */ return; }
  ...
}
```

### What was changed (do not regress)

- `dispatch_request()` + `HttpCallbackKind` enum replace 59 copy-pasted
  request blocks; NULL `SoupMessage` (malformed server URL) now produces an
  error callback instead of a crash.
- All 4 response handlers (`on_json_response`, `on_bytes_response`,
  `on_head_response`, `on_upload_response`) use weak-ref-first ordering and
  **deferred** free (`http_callback_data_free_deferred`) — never free
  `HttpCallbackData` synchronously inside the libsoup callback stack.
- `get_server_status_with_timeout`'s private cancellable is chained to the
  client cancellable via `g_cancellable_connect` (see `TimeoutData.parent`),
  so `cancel_pending()` also aborts reconnect probes.
- Caller fixes applied: 9 CANCELLED-first guards in the session auth/guest
  chains (`gnalk-session.c`); `on_pending_message_sent`
  (`gnalk-sync-manager.c`) no longer consumes a retry on cancel (3 cancels
  used to silently DELETE a queued message); `loading_older` reset moved
  above the CANCELLED return in `on_older_messages_response`
  (`gnalk-message-controller.c`); spurious error signals/toasts/warnings
  gated in `gnalk-sync-coordinator.c`, `gnalk-call-window.c`
  (`on_http_join_call_response`), `gnalk-conversation-info-dialog.c`,
  `gnalk-window-file-handling.c` (`on_file_downloaded`, also resets
  `audio_loading`), `gnalk-window-conversation-list.c`.

---

## 3. Chantier 2 — Generalize async-lifetime safety (GnalkAsyncOp everywhere)

**Problem class**: raw owner pointers captured in async callbacks → use-after-
free when the owner dies with a request/timer in flight. The HTTP client can
outlive its session (avatar cache holds a strong ref in
`gnalk-avatar-cache.c`; an open call window holds one via `g_set_object` in
`gnalk-call-window.c` → `start_call`), so "client died ⇒ callback swallowed"
does NOT protect these.

### Reference patterns (copy these, don't invent new ones)

- **GnalkAsyncOp** (`src/utils/gnalk-async-op.[ch]`): GWeakRef to owner +
  generation counter. Exemplary usage: `src/controllers/gnalk-message-controller.c`
  (`on_poll_response`, `on_send_response`, …): CANCELLED-first → 
  `gnalk_async_op_is_valid` → `gnalk_async_op_get_owner` → free op on every path.
  Constraint: the generation pointer must point **inside the owner object**,
  never into a separately-freed struct.
- **Generation counter against ListView row recycling**:
  `src/ui/gnalk-file-preview.c` (`generation` field, checked in the async
  callback). A GWeakRef alone protects against destruction, NOT recycling.
- **Dialog-lifetime**: for AdwDialog HTTP callbacks, pass `g_object_ref (self)`
  as user_data and unref in the callback on all paths (AdwDialog self-destructs
  on close). Alternative: GWeakRef struct. Never a raw pointer.

### Step-by-step

1. **Session auth chain** (`src/gnalk-session.c`): convert the raw-`self`
   callbacks to GnalkAsyncOp: `on_server_status_response`,
   `on_user_info_response`, `on_conversations_response`,
   `on_capabilities_response`, `on_signaling_settings_response`, and the four
   `on_guest_*` callbacks. Model them on `on_room_join_response` /
   `on_fed_signaling_settings_response` in the same file (already AsyncOp).
   Keep the existing CANCELLED-first guards.
2. **Controllers**: replace raw owner pointers in callback structs with a
   GWeakRef (or embed a GnalkAsyncOp):
   - `gnalk-sync-coordinator.c`: `ConversationFetchData.coordinator`,
     `RoomSyncData.coordinator`, bare `self` in `on_threads_fetched`.
   - `gnalk-sync-manager.c`: `PendingSendData.manager`.
   - `gnalk-conversation-controller.c`: `OperationCallbackData.controller`,
     `FetchCallbackData.controller`, `SearchContactsCallbackData.controller`.
3. **`GnalkConversationData->session`** (`src/data/gnalk-conversation-data.c`):
   the field is a plain pointer commented "weak reference". Replace with a
   real `GWeakRef` (init/clear in init/finalize; getter returns a strong ref
   the caller unrefs, or NULL). Update `join_room_via_session()`
   (`gnalk-room-controller.c`) to handle NULL. This also fixes the layering
   inversion (data/ including gnalk-session.h can then be reconsidered).
4. **Dialogs** (`src/ui/gnalk-conversation-info-dialog.c`,
   `gnalk-new-conversation-dialog.c`): every HTTP callback currently gets raw
   `self`. Apply the ref-in/unref-out pattern to: `on_name_saved`,
   `on_description_saved`, `on_avatar_saved`, `on_notification_level_saved`,
   `on_call_notifications_saved`, `on_participants_response`,
   `on_shared_media_response`, `on_media_preview_loaded` (raw `data->image`
   widget pointer!), `on_create_room_response`, `on_add_participant_response`,
   `on_member_avatar_loaded`. In `on_member_avatar_loaded`
   (`gnalk-new-conversation-dialog.c`) also fix: `g_object_remove_weak_pointer`
   is skipped on the `texture == NULL` path while `data` is freed → dangling
   weak pointer → heap corruption. Remove the weak pointer on **every** path.
5. **Window partials**: fix raw-`self` async holders:
   - `gnalk-window-read-status.c`: `mark_chat_read (..., self)` callbacks.
   - `gnalk-window-room-switch.c` `on_room_avatar_loaded`, `gnalk-window.c`
     `on_header_avatar_refetched`, `gnalk-window-message-actions.c`
     `on_quote_preview_image_loaded` → use GWeakRef like
     `gnalk-conversation-row.c` / `gnalk-participants-list.c` already do.
   - `gnalk-window-message-actions.c`: `highlight_clear_timer_id` is never
     cancelled on room close or in dispose, and its handler derefs
     `self->current_room` unguarded. Cancel in dispose + on room close; NULL-check.
   - `gnalk-window.c`: `g_signal_connect (call_window, "destroy", ...)` →
     `g_signal_connect_object`. Also the `app_actions` "about" action registered
     on GApplication captures the window raw — re-register per window or use
     a weak ref.
   - `gnalk-window-login.c`: module-static `GnalkGoa` connects
     `accounts-changed` to the first window raw and never disconnects → UAF
     on window re-creation (service mode). Use `g_signal_connect_object` and
     reconnect per window.
   - `gnalk-window.c` `attempt_autologin_idle`: 10 ms self-rescheduling raw
     timer, 100 Hz busy-poll → replace with a "ready" signal or at minimum
     guard with a weak ref and cancel in dispose.
6. **`current_room` NULL guards** (crash class): add NULL checks where
   `self->current_room` is dereferenced unguarded:
   `gnalk_window_on_session_typing` (gnalk-window.c — trivially reachable:
   peer types while no room open), `get_property`/`set_property` for
   CURRENT_ROOM_NAME (gnalk-window.c), `on_quote_closed` / `on_quote_requested` /
   `on_edit_requested` (gnalk-window-message-actions.c),
   `on_message_input_text_changed` (gnalk-window.c),
   `on_scroll_to_bottom_clicked_for_unread` (gnalk-window-read-status.c).
7. **Push client** (`src/network/gnalk-push-client.c`) — two UAF paths:
   - `websocket_connect_async` is issued with a NULL cancellable and finalize
     doesn't cancel → add a GCancellable, cancel in dispose.
   - `disconnect()` clears the connection **without disconnecting the
     "closed"/"error" handlers** first. Copy the choreography from
     `gnalk-signaling.c` (disconnect handlers before `g_clear_object`) — the
     hazard is documented in a comment there.
8. **Camera preview** (`src/call/gnalk-camera-preview.c`): `FrameUpdate.picture`
   is a raw pointer; every other frame-update struct uses a weak pointer.
   Align it (weak ref or strong ref on the GtkPicture).

**Definition of done**: `grep -rn "user_data = self\b\|->controller = self\|->manager = self\|->coordinator = self" src/` returns only sites that ref/weak-ref;
no dialog passes raw `self` to an HTTP call; app survives this torture
sequence without criticals/crash: open info dialog → close it immediately →
switch account → switch back → open room → switch room rapidly while
scrolling → remove account while it reconnects.

---

## 4. Chantier 3 — Threading: GStreamer→UI marshalling + async teardown

### 4.1 Unmarshalled callbacks (crash class — do first)

GstWebRTC/GStreamer emit on internal threads. The video-frame path is already
marshalled correctly (`g_idle_add` + weak refs — copy that pattern from the
tile video callbacks). These are NOT marshalled:

1. `src/network/gnalk-webrtc.c`: `notify::connection-state` →
   `connection_state_callback` fires synchronously on webrtcbin's PC thread;
   consumer `gnalk_call_signaling_on_publisher_connection_state`
   (`gnalk-call-signaling-manager.c`) calls `gtk_stack_set_visible_child_name`,
   creates tiles, sends WebSocket messages (libsoup is documented
   non-thread-safe in a comment near the top of gnalk-webrtc.c).
   → Marshal via the same `g_idle_add` + GWeakRef pattern used by
   `signal_callback` in the same file.
2. `src/call/gnalk-data-channel.c`: `on-message-string` / `on-open` /
   `on-close` handlers propagate into
   `gnalk_call_participant_on_tile_speaking` (g_free/g_strdup of
   `active_speaker_id`, `layout_idle_id`) on the webrtc thread. → Marshal at
   the data-channel boundary (one idle per message).
3. `src/call/gnalk-quality-controller.c`: `on_stats_promise_reply` runs on the
   promise thread, mutates fields the main-thread timer reads, `g_object_set`s
   the encoder, emits `stats-updated`, and holds a raw `self` with no ref
   (UAF if a reply lands after dispose). → ref `self` into the promise
   user_data with a GDestroyNotify unref; marshal the state mutation.
4. Streaming-thread counters (`rtp_video_probe_cb`, `on_remote_video_sample`
   writing `got_decoded_video` etc.): use `g_atomic_int_*` or marshal.
5. `gnalk-media-pipeline.c` / `gnalk-call-preview-manager.c`:
   `g_object_add_weak_pointer` called from streaming threads → replace with
   GWeakRef (thread-safe) taken on the main thread at setup.

### 4.2 Blocking teardown (UI freeze class)

- `gnalk-call-window.c` `end_call` chain: serial blocking
  `gst_element_get_state` waits (camera drain 2+2+3 s, ~2 s per pipeline,
  ~2 s per subscriber) — closing a 5-person call can freeze the UI 10+ s.
  → set pipelines to NULL asynchronously (`gst_element_set_state` without
  waiting), move any mandatory waits to a worker thread, or use bus-async
  state-change tracking.
- `gnalk-webrtc.c` `start_screen_share`: `g_usleep(200ms)` + two 3 s
  `gst_element_get_state` waits on the main thread; `gnalk_webrtc_end_call`
  blocks up to ~2 s. Same remedy.
- `gnalk-camera-preview.c` `stop_video_recording`: 10 s `g_usleep(50ms)`
  busy-poll for EOS on the main thread; `eos_mutex`/`eos_cond`/`bus_watch_id`
  are dead fields and no bus watch exists on the recording pipeline. → real
  bus watch + async EOS handling; surface encoder/filesink errors.

### 4.3 SQLite/model cross-thread (see also chantier 4)

`save_messages_thread` (`gnalk-message-processor.c`) reads GnalkMessageData
strings (bound `SQLITE_STATIC`) on a worker while the main thread can
g_free/g_strdup them (`set_message`, `confirm_sent`). Fix as part of
chantier 4 step 3 (snapshot rows on the main thread before handing to the
worker), or bind with `SQLITE_TRANSIENT` after copying under one read.

**Definition of done**: `GST_DEBUG=2` run of a 2-party call with join/leave
churn shows no GTK criticals; closing the call window returns to the main
window in <300 ms; a `helgrind`/TSAN spot-check on the quality controller is
a bonus, not required.

---

## 5. Chantier 4 — SQLite hardening

All in `src/data/gnalk-database.c` + `src/data/gnalk-data-repository.c`
unless noted. Current state: no WAL, no busy_timeout, no transactions outside
the one migration, every `sqlite3_step` result ignored, migrations are ~25
blind `ALTER TABLE`s with errors discarded + `strstr` on `sqlite_master.sql`.

1. **Pragmas at open** (`gnalk_database_open`): `PRAGMA journal_mode=WAL;`,
   `PRAGMA synchronous=NORMAL;`, `sqlite3_busy_timeout (db, 5000);` (keep
   existing `PRAGMA foreign_keys`).
2. **Transactions around bulk writes**: wrap in `BEGIN IMMEDIATE`/`COMMIT`:
   - `save_conversations` loop (`gnalk-data-repository.c`, called from the
     20 s sync tick — currently N autocommit fsyncs on the main thread);
   - the per-message `save_message` loop in `on_room_messages_synced`
     (`gnalk-sync-coordinator.c`) — batch the whole sync batch into ONE
     transaction and ONE `data-changed` emission, not one per message;
   - `save_participants` (also: prepare the INSERT **once** outside the loop);
   - `save_messages_thread` (`gnalk-message-processor.c`);
   - `delete_conversation` and `delete_account` (3–5 dependent DELETEs,
     currently non-atomic).
3. **Single-writer discipline**: route ALL writes through one place. Simplest
   robust design: a dedicated writer GThread with a GAsyncQueue of
   ready-made bound statements or value structs (snapshot GObject field
   values on the main thread before enqueueing — fixes §4.3). Reads may stay
   where they are (WAL allows concurrent readers). Remove
   `gnalk_data_repository_get_database()` escape hatch usage in
   `gnalk-message-processor.c` and `GnalkAccountManager` (writes bypassing
   the repository never emit `data-changed`).
4. **Versioned migrations**: replace the blind-ALTER scheme with
   `PRAGMA user_version`. `user_version==0` → create current schema, stamp N.
   Otherwise run numbered migration steps in one transaction each, stamping
   after each. Delete the `strstr (schema_sql, ...)` checks. Pitfall found in
   the current code: the composite-PK migration (`messages_new` + `INSERT INTO
   ... SELECT *, 'users'`) assumes exactly 16 columns and fails forever on any
   old DB with extra columns, silently leaving the single-column PK →
   cross-account message overwrites. The versioned scheme must name columns
   explicitly in the copy.
5. **Check every `sqlite3_step`**: at minimum
   `if (rc != SQLITE_DONE) g_warning ("db: %s failed: %s", tag, sqlite3_errmsg (db));`
   A small helper macro keeps it terse.
6. **Fix `WHERE id = ?` without account_id** in `update_message`,
   `update_message_reactions`, `update_message_status` (currently dead code,
   but booby-trapped: PK is `(id, account_id)`). Add the account_id predicate
   or delete the functions.
7. **Corruption fallback**: if `sqlite3_open` or the initial pragma fails with
   SQLITE_CORRUPT/SQLITE_NOTADB → rename the file to `gnalk.db.corrupt-<ts>`,
   recreate, log prominently.
8. Smaller fixes while in there: stale-conversation cleanup deletes
   messages/sync_state without the `invitation_id = 0` guard used for
   conversations; the `IN (...)` list breaks past 999 variables (chunk it);
   `load_messages` paginates `id < ?` but orders by `timestamp DESC`
   (paginate on `(timestamp, id)` keyset instead);
   `AccountManager::add_account` returns owned ref on one branch, borrowed on
   the other — pick owned, fix callers.

**Definition of done**: sync tick with 100+ rooms produces one transaction;
`ls` shows `-wal`/`-shm` files; a fresh DB and an old-schema DB both migrate
to the same `user_version`; kill -9 during a sync leaves a consistent DB.

---

## 6. Chantier 5 — Log hygiene

Secrets currently in `build/gnalk.log` (all confirmed):

1. `src/network/gnalk-push-client.c` `send_text_message` /
   `authenticate`: logs every outgoing WS frame → **the account app-password
   goes to the log in cleartext**. Redact auth frames entirely.
2. `src/network/gnalk-http-client.c` `on_json_response`: logs the **full
   response body** of every request (private conversations) via
   `gnalk_log_http_response`. Body logging must become opt-in
   (`GNALK_LOG_HTTP_BODY=1`) and default off.
3. `gnalk_http_client_start_call`: debug block dumps **session cookie values**.
   Delete it or log names only.
4. `join_room_as_guest`: request body containing the guest room password is
   logged. Redact `"password"` fields before logging JSON bodies.
5. `src/network/gnalk-signaling.c` `send_hello`: logs the signaling
   ticket/JWT. Redact.
6. Also: session_id values are logged in room-join messages (`gnalk-session.c`,
   window logs) — shorten to a prefix.

Then performance: introduce levels in `src/utils/gnalk-log.[ch]`
(`GNALK_LOG_LEVEL` env, default = warnings+). Demote to debug: the per-scroll
`g_message`s in `gnalk-message-view.c` (`value-changed` handler — hottest
handler in the app), the per-frame/per-keyframe/per-300-packets logs in
`call/` probes (~170 `g_message` calls in that directory), the
`update_conversation_last_message` info log, the `g_print`s in the
conversation-list search path.

**Definition of done**: `grep -iE "password|authorization|cookie|ticket" build/gnalk.log`
after a full login+call session returns nothing; a normal chat session logs
< 50 lines at default level.

---

## 7. Chantier 6 — message-row decomposition + scroll performance

`src/ui/gnalk-message-row.c` is the hottest widget. Current sins, worst first:

1. **Sync GStreamer decode on bind**: `gnalk_audio_player_set_file` →
   `extract_waveform_data` (`gnalk-audio-player.c`) runs a full decode
   pipeline with pull loop **on the main thread**, triggered from
   `set_from_data` when a cached voice message scrolls into view. → Move
   waveform extraction to a GTask worker; cache the extracted waveform on
   disk next to the audio file (e.g. `<file>.waveform`) and on
   GnalkMessageData; the player shows a placeholder until the async result
   lands (guard with a generation counter against recycling).
2. **Per-row display-global CssProvider**: `set_bubble_color` /
   `apply_bubble_color` loads a display-level `GtkCssProvider` on nearly every
   bind (full-display style invalidation per row per scroll step) and the
   provider is **never removed** from the display (leak per destroyed row).
   Same anti-pattern in `gnalk-image-gallery.c` zoom. → Replace with a fixed
   palette of N CSS classes (`.bubble-color-0` … `.bubble-color-15`) defined
   once in the app stylesheet; pick class by hashing the author id. Delete
   the per-row provider entirely.
3. **Regex recompilation**: `gnalk_markdown_to_pango` (`src/utils/gnalk-markdown.c`)
   compiles ~10 GRegex per call, called 1–2× per bind. → compile once into
   `static GRegex *` under `g_once_init`. THEN: pre-compute the pango markup
   in MessageProcessor and cache it on GnalkMessageData (per CLAUDE.md
   "pre-compute in model"); bind just sets the cached markup.
   Also fix: on Pango parse failure (`gtk_label_set_markup` with unbalanced
   tags renders EMPTY) fall back to `g_markup_escape_text`-ed plain text;
   replace hardcoded `#e8e8e8`/`#0082c9` with CSS classes (dark-mode bug).
4. **Widget weight**: every recycled row instantiates a full GnalkAudioPlayer
   (playbin + 2 sinks!), GnalkVideoThumbnail, GnalkFilePreview,
   GnalkUploadPreview, GnalkQuoteWidget, a 12-item file-preview pool, and a
   15-button popover — ~120 widgets and a GStreamer pipeline per row, even
   for plain text. → Decompose per message type: a GtkStack (or per-type
   factories) with lightweight text row as the common case; create media
   players **lazily on first use**; share ONE context-menu popover across
   rows (pattern: `gnalk-participants-list.c`).
5. **Recycling races**: avatar/bubble-color/video-preview async callbacks use
   GWeakRef only → wrong avatar/color on recycled rows. Add the
   `gnalk-file-preview.c` generation-counter pattern. Same for
   `gnalk-window-file-handling.c` download completion (applies audio/video
   to a row that may have been rebound) and `gnalk-quote-widget.c`.
6. **reset() completeness** (`gnalk_message_row_reset`): currently leaks state
   across binds: `has_bubble_color` + `bubble-color-*` class, `is_bot` +
   `bot-avatar` class + 18px avatar size + 32×32 avatar_frame request,
   `edit_timeout`/`delete_timeout`/`threads_supported`. Reset everything the
   bind branches can set. (The per-type decomposition shrinks this list.)
7. **Dead code**: `set_data` (JsonObject path ~300 lines), `new_pending`,
   `add_reaction`, `get_date`, `set_reference_id`, `set_upload_progress`, and
   the 3 never-notified GObject properties have no external callers — delete
   (~500 lines).
8. **Syscalls in bind**: `g_file_test` for cache state on every bind of audio
   and non-cached files; group leaders stat up to 12 files per bind. → cache
   the result (incl. negative) on GnalkMessageData; refresh only on download
   events.
9. Related view fixes: `gnalk-message-view.c` hardcodes 60 px row height in
   `is_item_visible`/`get_first_visible_position` (wrong read-marking in
   media rooms) → use `gtk_list_view_scroll_to` (GTK 4.12) for jumps and
   compute visibility from the actual row widgets;
   `gnalk-window-message-display.c` uses `set_model(NULL)/set_model(store)`
   cycles for clear/batch-insert (full invalidation, forbidden by CLAUDE.md)
   → keep the model attached; batch via `g_list_store_splice`.
10. `trim_tab_messages` (`gnalk-room-tab-manager.c`) instantiates a real
    GnalkMessageRow + `gtk_widget_measure` per message on room background →
    estimate from message metadata instead, or trim by count only.

**Definition of done**: scrolling a 500-message room with images+audio at
120 Hz shows no frame drops in GTK inspector's frame profiler; `sysprof`
shows no `extract_waveform_data`, no `gtk_css_provider_load_from_string`,
no `g_regex_new` in the scroll path.

---

## 8. Chantier 7 — Zero-copy video + call error recovery

### 8.1 Rendering path (biggest CPU win)

Current receive path per participant:
`vp8dec (sw) → videoconvert (CPU→RGB) → appsink → g_bytes_new (memcpy) →
GdkMemoryTexture → g_idle_add → gtk_picture_set_paintable` = 2 full-frame CPU
copies + heap texture + idle per frame. Same pattern for local preview and
`gnalk-video-player.c`.

Target: `decodebin/vp8dec → gtk4paintablesink` (gst-plugins-rs), the sink's
paintable set on a GtkPicture wrapped in **GtkGraphicsOffload** (GTK 4.14+,
`gtk_graphics_offload_set_black_background (…, TRUE)` from 4.16). With
GStreamer 1.24+ and the sink's dmabuf support, frames go decoder → compositor
subsurface zero-copy on Wayland.

Steps:
1. Check plugin availability at runtime (`gst_element_factory_find
   ("gtk4paintablesink")`); keep the appsink path as fallback.
2. Replace the appsink chain in `gnalk-receive-pipeline.c`, the local preview
   in `gnalk-media-pipeline.c`/`gnalk-call-preview-manager.c`/
   `gnalk-camera-preview.c`, and `gnalk-video-player.c`.
3. Raise gtk dep to >= 4.16 in `src/meson.build` when adopting.
4. **VA-API**: stop forcing software codecs. For decode, use `decodebin3` and
   let rank pick `vah264dec`/`vavp8dec` (PRIMARY+1 since GStreamer 1.24). For
   encode, probe `vah264enc`/`vavp8enc`/`vp8enc` in that order in
   `gnalk-media-pipeline.c` (H.264 requires offer/answer support — Talk
   supports H.264; keep vp8 fallback). Remove the redundant
   scale1/caps2/convert stages (3 videoconverts + 2 videorates in one path).

### 8.2 Error recovery (calls currently freeze forever on any blip)

1. `connection-state` handling (`gnalk-call-signaling-manager.c`): the only
   branch is `if (connected && !publisher_connected)`. Add FAILED/DISCONNECTED
   handling: subscriber → tear down + re-request offer; publisher → surface
   error state + attempt ICE restart (webrtcbin: set `ice-restart` on the
   next offer via `gst_promise` offer options), give up after N attempts with
   a visible error.
2. `gnalk-media-pipeline.c`: the `pipeline-error` signal is defined but never
   emitted — emit it from the bus ERROR watch; call window shows a toast and
   falls back (camera unplug → videotestsrc placeholder or camera-off state).
3. `gnalk-screen-capture.c`: subscribe to the portal `Session.Closed` D-Bus
   signal (compositor-side stop currently leaves a dead pipewiresrc streaming);
   also use the request path returned by each portal call instead of the
   predicted `build_request_path` (flow hangs silently when they differ), and
   add a response timeout.
4. `TileVideoCallbackData` (3 per tile, in `gnalk-call-participant-manager.c`,
   `gnalk-call-screen-share.c`, `gnalk-call-window.c`) is never freed: add a
   `GDestroyNotify` parameter to the `gnalk_webrtc_set_*_callback` setters and
   free on tile removal/end_call.
5. Misc correctness while in there: `call_ending` flag never reset after
   end_call (second call on the same window can never leave);
   `remove_tile` early-returns when `record->tile == NULL`, leaving ghost
   screen-share tiles + running subscribers after abrupt sharer leave;
   `has_mcu` is flipped by any *participant* advertising "mcu"
   (mis-indented `if` in `gnalk-call-window.c`) — MCU-ness must come only
   from the server hello; `local_tile` is never assigned (local user absent
   from grid — decide: implement or delete); `reaction_fade_timeout` leaks
   its data and one participant's reaction never clears if another reacts
   within 3 s (per-tile timeout, with GDestroyNotify);
   remote ICE candidates arriving before the answer are only buffered in
   receive-only mode (`gnalk-webrtc.c` — buffer in sender mode too);
   `gst_sdp_message_parse_buffer` result unchecked; `strip_rtx`
   (`gnalk-sdp-utils.c`) splits on `\r\n` only (LF-only SDP bypasses it);
   `gst_init` belongs in application startup, not per-GnalkWebrtc-instance;
   static per-process frame counters in receive-pipeline/media-pipeline lie
   in multi-party calls — make them instance fields.

**Definition of done**: 4-party call CPU < 1 core total on a VA-capable
desktop; pulling the network cable mid-call shows a reconnecting state and
either recovers or fails visibly within 30 s; camera unplug shows a toast,
not a frozen tile.

---

## 9. Chantier 8 — Mobile readiness (postmarketOS / OnePlus 6)

Ecosystem facts (mid-2026, verified): pmOS v26.06 stable (systemd default);
OnePlus 6 "enchilada" community device with official Phosh/GNOME-mobile
images; front camera (IMX371) works via libcamera since kernel 6.14
(black-level-only tuning, mediocre quality); **suspend is broken** (no
reliable push wakeup); call-audio routing is the riskiest area (callaudiod →
WirePlumber transition in progress; v25.12 notes OP6 call-audio problems).
GTK ≥ 4.16 defaults to the Vulkan renderer on Wayland — mature on turnip/
Adreno 630; escape hatch `GSK_RENDERER=ngl`. HW video (Venus VPU) via
stateful `v4l2h264dec`/`v4l2h264enc` — availability on the pmOS sdm845 kernel
unverified, test on device. Design for 360 px logical width; height can drop
to ~300 px with the OSK.

Priority order:
1. **AdwBreakpoint collapse**: the NavigationSplitView already has two
   code-created breakpoints — verify `collapsed=true` at ≤ 550 sp and audit
   every view at 360 px width with libadwaita's adaptive preview
   (GTK Inspector, Ctrl+Shift+M). AdwDialogs become bottom sheets for free.
2. **OSK survival**: message list + input usable at ~300 px height; keep the
   input visible and scroll-to-bottom on resize; set
   `input-purpose`/`input-hints` on entries.
3. **Battery discipline**: prefer the existing notify_push WebSocket; when
   `GtkWindow:is-active` is FALSE, stretch the 20 s sync timer and 60 s
   participant poll (e.g. ×6) and pause long-polling for non-visible rooms.
4. **Camera source abstraction** in `gnalk-media-pipeline.c` /
   `gnalk-camera-utils.c`: element name behind one function — `v4l2src`
   (desktop) / `pipewiresrc` via the camera portal (mobile; the path GNOME
   Snapshot uses). Mobile profile for the quality controller: 640×480@15-24.
5. **Call audio routing**: talk to callaudiod D-Bus
   (`org.mobian_project.CallAudio`, `SelectMode` VoIP) when present, for
   earpiece/speaker switching.
6. Touch: 44 px minimum targets; every hover-only action (message-row hover
   buttons) needs a long-press path (`GtkGestureLongPress`).
7. Mark screen share desktop-only initially (phone portal screencast immature).

Reference app: Fractal (adaptive chat done right). Cautionary: Dino A/V calls
still failed on OnePlus 6 as of late 2025 — treat mobile calls as
experimental; chat-first.

---

## 10. Bug backlog (independent point fixes, any order)

| Sev | Where | Issue → Fix |
|-----|-------|-------------|
| high | `gnalk-application.c` | `self->account_id` never assigned; app-level http_client never gets credentials → notification actions ("mark read", reply) are dead. Wire through SessionManager or delete the vestigial fields. |
| high | 8 sites: `gnalk-window-read-status.c`, `gnalk-conversation-info-dialog.c`, `gnalk-participants-list.c`, `gnalk-conversation-data.c` (thread ids) | 64-bit ids truncated via `GINT_TO_POINTER((gint)…)` → wrong row marked read / wrong attendee promoted past 2^31. Use `g_hash_table` with int64 keys (`g_int64_hash`) or heap-allocated keys. |
| high | `gnalk-avatar-cache.c` | No eviction (memory + disk grow unboundedly; keys multiplied per size). Add LRU cap (e.g. 256 textures) + disk GC; negative-cache failed fetches (a 404 avatar currently re-fetches per bind). Also: first-load disk read+decode is sync on main thread → GTask. |
| high | `gnalk-window-file-handling.c` `add_file_as_attachment` | `g_file_get_contents` + sync texture decode of the full attachment on the main thread (seconds of freeze for a video; unbounded RAM). → async read, stream upload, thumbnail via `gdk_texture_new_from_file` at reduced size on a worker. |
| high | `gnalk-session.c` | Session gives up after 10×5 s reconnect attempts, state DISCONNECTED, UI uninformed, never retries (a 1-min outage permanently kills the session). → exponential backoff with jitter, no attempt cap while OFFLINE-network events can rearm; surface state in UI. |
| high | network layer | No backoff anywhere: push client retries every 5 s forever incl. on auth failure (`gnalk-push-client.c` — stop on 401/403); signaling has NO reconnect at all (`reconnect_timeout_id` dead) — recovery relies on Session; add jittered backoff helper shared by push/signaling/session. |
| med | `gnalk-sync-manager.c` | Offline queue: immediate recursive retry without backoff, then message DELETED after 3 attempts (data loss on a 30 s hiccup). → backoff between flush passes; never delete — mark failed + keep for manual retry. |
| med | `gnalk-message-controller.c` `resume_polling` | Refuses when `last_known_message_id <= 0` → uploading into an empty room permanently stops live updates. Allow resume with id 0. |
| med | `gnalk-session.c` online `send_message` | Passes NULL callback → failed sends silently dropped (no retry/queue/UI). Route through MessageController's send path (single send path). |
| med | `gnalk-session.c` `total_unread` | Computed only in the auth-chain conversations response; frozen after login. Recompute in SyncCoordinator's conversations pass. |
| med | `gnalk-http-client.c` WebDAV paths | `file_path`/`remote_path`/`user_id` concatenated unescaped → "My File (1).png" breaks. Escape path segments with `g_uri_escape_string (…, "/", FALSE)`. |
| med | `gnalk-http-client.c` `add_reaction`/`set_user_status` | JSON built with printf (injectable, breaks on `"`/`\`). Use JsonBuilder like the other endpoints. |
| med | `gnalk-video-thumbnail.c` | `gdk_texture_download` gives premultiplied BGRA; fed to `gdk_pixbuf_new_from_data` as RGBA → red/blue swapped thumbnails. Use `gdk_texture_save_to_png_bytes`/`gdk_texture_downloader` with explicit format, drop GdkPixbuf. Also make thumbnail decode/encode async. |
| med | `gnalk-message-input.c` | Enter during IM preedit sends mid-composition (CJK) → check `gtk_text_view_im_context_filter_keypress` ordering / preedit state; Ctrl+V only matches lowercase `GDK_KEY_v`. |
| med | `gnalk-window-account-session.c` + login | `switch_to_session` runs twice on startup (double signal wiring + double loads). Deduplicate (only via `active-session-changed`). |
| med | `gnalk-window.c` `on_session_room_disinvited` | Nulls `current_room` without informing RoomController/processor/tab manager → stale polling. Route through the normal room-close path. |
| med | `gnalk-application.c` `ensure_window` | Can pick a call window from `gtk_application_get_windows` and drop notification navigation. Iterate for `GNALK_IS_WINDOW`. |
| med | `gnalk-session-manager.c` | `gnalk_keyring_lookup_password_sync` per account on the main thread at startup (UI freeze on slow Secret Service). Use the async API. |
| med | `gnalk-participants-list.c` `set_participants` | Full widget rebuild + N avatar fetches on every 60 s poll. Diff in place (id-keyed), reuse rows. |
| med | reactions UI (`gnalk-message-row.c`) | Reactions rendered as one concatenated emoji label: counts discarded, not clickable, `strstr` dedup wrongly prefix-matches skin-tone variants. Rebuild with AdwWrapBox chips (see §11). |
| low | `gnalk-connection-dialog.c` | 867 lines dead (never instantiated; stale include in gnalk-window.c). **Delete the file** + include. |
| low | `gnalk-avatar-cache.c` `create_round_avatar` | Draw-func signature mismatch (`GtkSnapshot*` cast to `GtkDrawingAreaDrawFunc`) — dead code, delete or fix before use. |
| low | `gnalk-database.c` pending_messages | `INSERT OR REPLACE` resets created_at/retry_count on requeue. Use UPSERT preserving them. |
| low | `gnalk-window-conversation-list.c` | Empty "Conversations" section header when all rooms are favorites (missing `has_non_favorites` check). |
| low | `gnalk-date-separator.c` | "Yesterday" via `tm_yday - 1` breaks across year boundary. Compare julian days (GDate). |
| low | i18n | Untranslated: message context-menu items, media tooltips, recording tooltips, "Today/Yesterday", typing indicator (also non-plural-aware). Wrap in `_()` / `ngettext`. |
| low | `gnalk-window.c` | Move all `g_source_remove` from finalize to dispose (criticals window between dispose and finalize). |
| low | controllers | `g_slice_new/free` deprecated → `g_new0`/`g_free`. |
| low | `gnalk-session.c` | Signaling-settings/ICE parsing duplicated between normal and guest chains (guest copy misses `urls`-as-string + legacy `url`) → extract one parser. Guest chain proceeds on missing `sessionId` → emits `room-joined` with NULL session id; bail instead. |
| low | `gnalk-sync-coordinator.c` | `set_active_room` broadcast to all sessions' coordinators + notification keys are room-token-only → cross-account suppression collisions. Key by (account, token). |
| low | window O(n) scans | `on_message_controller_sent/deleted/reaction_added/...` linear-scan the store although MessageProcessor keeps `msg_index`/`ref_index` hash indexes. Expose lookup API and use it (7 call sites). Also `append/prepend_messages` in `gnalk-room-tab-manager.c` do O(n×m) dup detection. |
| low | `gnalk-preferences-dialog.c` | 3 synchronous GstDeviceMonitor enumerations at dialog open; device identity by mutable list index. Enumerate once, async; key devices by persistent id. Wrap the required `GValueArray` (GStreamer level message) in `G_GNUC_(BEGIN|END)_IGNORE_DEPRECATIONS` (also in `gnalk-voice-recorder.c`). |
| low | `gnalk-markdown.c` | Guessable `<<<GNALKFENCEDn>>>` placeholder can be spoofed by message content (cosmetic). Use a per-call random nonce in the token. |
| low | `gnalk-network-monitor.c` `is_metered` | Derefs `self->monitor` after dispose NULLs it. Guard. |
| low | `gnalk-webrtc.c` | `g_strstr_len (debug, …)` where debug may be NULL (GLib critical). Guard. |
| low | `gnalk-background-generator.c` | Leaks a new `mosaic-bg-<ts>.png` in cache dir per generation. Delete the previous file. |
| low | typing/reaction signaling | One unicast WS frame per participant per event (O(N) per keystroke transition in large rooms) — acceptable for now, note for scale. |

---

## 11. GTK / libadwaita modernization menu (adopt opportunistically)

Verified against official NEWS/blogs. Raise meson deps when adopting
(target: gtk4 >= 4.16, libadwaita >= 1.6; >= 4.20/1.8 where noted).

**High value:**
- `GtkGraphicsOffload` (4.14) + `black-background` (4.16) + gtk4paintablesink
  dmabuf (GStreamer 1.24) — see chantier 7.
- `gtk_list_view_scroll_to()` (4.12) — replaces adjustment hacks for
  jump-to-message / scroll-to-bottom (chantier 6).
- CSS variables + `color-mix()` (4.16), `light-dark()` + media queries
  (4.20): rewrite custom CSS; use `var(--accent-bg-color)` (Adw 1.6 accent
  colors) for own-message bubbles; fixes dark-mode hardcodes. The old
  `@named_color` + `alpha()/mix()/shade()` are deprecated in 4.16.
- `AdwWrapBox` (1.7) — reaction chips + attachment chip rows.
- `GtkSectionModel`/`GtkListHeader` (4.12) — optional: native date headers in
  the message list instead of `show_date_header` flags.

**Nice to have:** AdwSpinner (1.6), AdwButtonRow (1.6, details sidebar),
AdwToggleGroup + AdwInlineViewSwitcher (1.7), AdwMultiLayoutView (1.6,
adaptive call layouts), AdwBottomSheet standalone (1.6) + `:reveal-bottom-bar`
(1.8) for a "return to call" bar, AdwShortcutsDialog (1.8, GtkShortcutsWindow
is deprecated in 4.18), `AdwPreferencesGroup.bind_model` (1.8, accounts list),
`.document`/`.body` typography classes (1.8) for message text,
GtkPopoverBin (4.20) for row context menus, `gtk_uri_launcher` /
`GtkFileLauncher` for links and downloaded files (gtk_show_uri deprecated),
emoji chooser improvements (4.14) are free.

**Deprecation greps to run** (currently near-clean):
`.dim-label` → `.dimmed` (Adw 1.7); `GdkPixbuf` remnants → GdkTexture
(avatar cache, video thumbnails — 4.20 deprecates remaining pixbuf APIs);
`gtk_widget_show/hide` → `set_visible`; `G_TYPE_VALUE_ARRAY` (wrap, see §10).

**GStreamer 1.24/1.26 notes**: VA decoders auto-ranked PRIMARY+1; ICE consent
freshness (RFC 7675, needs libnice ≥ 0.1.22) helps detect dead peers; DRM
modifier negotiation is what makes zero-copy work.

---

## 12. Verification checklist (run after each chantier)

1. `meson compile -C build` — zero warnings in touched files.
2. `timeout 8 ./run.sh` → `build/gnalk.log` shows: auth chain → push
   connected/authenticated → signaling ready → SyncCoordinator conversations
   → room joined → `get_messages … lookIntoFuture=1` long-poll.
3. Torture: rapid room switching while scrolling; open/close info dialog with
   slow network; account switch mid-sync; kill network mid-call.
4. `grep -iE "password|authorization|cookie|ticket" build/gnalk.log` → empty
   (after chantier 5).
5. GTK inspector (GTK_DEBUG=interactive): no criticals in console during the
   torture pass.
