# Gnalk: HTTP/WebSocket Connection Sequence (Login → Call)

Complete trace of every HTTP request and WebSocket message from app startup through an active WebRTC call.

---

## 1. Authentication Chain

Sequential requests orchestrated by `gnalk_session_connect()` in `gnalk-session.c`.
All HTTP requests use **Basic Auth** (`Authorization: Basic <base64(user:pass)>`) + `OCS-APIREQUEST: true`.

### 1.1 Server Status Check

```
GET /status.php
```
- **Function**: `gnalk_http_client_get_server_status()` → `on_server_status_response()`
- **Timeout**: 35s
- **Purpose**: Verify server reachable, validate credentials

### 1.2 Get User Info

```
GET /ocs/v2.php/cloud/user?format=json
```
- **Function**: `gnalk_http_client_get_user_info()` → `on_user_info_response()`
- **Response**: `ocs.data.displayname`
- **Purpose**: Retrieve display name for account

### 1.3 Get Conversations

```
GET /ocs/v2.php/apps/spreed/api/v4/room?format=json&includeStatus=true
```
- **Function**: `gnalk_http_client_get_conversations()` → `on_conversations_response()`
- **Response**: `ocs.data[]` array of room objects
- **Purpose**: Load all conversations, save to DB, emit `conversations-updated`

### 1.4 Get Capabilities

```
GET /ocs/v2.php/cloud/capabilities?format=json
```
- **Function**: `gnalk_http_client_get_capabilities()` → `on_capabilities_response()`
- **Response**: `ocs.data.capabilities.{spreed.features[], notify_push.endpoints.websocket}`
- **Purpose**: Detect federation (`federation-v1`/`v2`), get push WebSocket URL

### 1.5 Get Signaling Settings

```
GET /ocs/v2.php/apps/spreed/api/v3/signaling/settings?format=json
```
- **Function**: `gnalk_http_client_get_signaling_settings()` → `on_signaling_settings_response()`
- **Response**: `ocs.data.{server, ticket, stunservers[].urls[], turnservers[].{urls[], username, credential}}`
- **Purpose**: Get signaling WS URL + auth ticket, ICE servers (STUN/TURN)
- **State change**: session → `GNALK_SESSION_STATE_ONLINE`

---

## 2. Push WebSocket (notify_push)

Connected after capabilities provide the WebSocket URL.
File: `gnalk-push-client.c`

### Connection

```
WebSocket connect → {capabilities.notify_push.endpoints.websocket}
```

### Authentication (plain text messages)

```
→ "{user_id}"
→ "{password}"
← "authenticated"
```

State machine: `DISCONNECTED → CONNECTING → CONNECTED → SENT_USER → SENT_PASSWORD → AUTHENTICATED`

### Notifications

Server sends plain strings: `"newmessage"`, `"call"`, etc.
Auto-reconnects every 5s on disconnect.

---

## 3. Signaling WebSocket

Connected from `on_signaling_settings_response()`.
File: `gnalk-signaling.c`

### Connection

```
WebSocket connect → {signaling_server}?token={ticket}
```

### Hello (server response)

```json
← {
    "type": "hello",
    "hello": {
      "sessionid": "<signaling_session_id>",
      "server": { "features": ["mcu", ...] }
    }
  }
```

Stores `signaling_session_id`, detects MCU support, emits `ready` signal.

---

## 4. Periodic Sync (Background)

`GnalkSyncCoordinator` — 20s timer, also triggered by push notifications.

### Conversation Refresh

```
GET /ocs/v2.php/apps/spreed/api/v4/room?format=json&includeStatus=true
```

### Room Message Sync (per room with new messages)

First time:
```
GET /ocs/v2.php/apps/spreed/api/v1/chat/{token}?format=json&lookIntoFuture=0&lastKnownMessageId=0&limit=50
```

Subsequent:
```
GET /ocs/v2.php/apps/spreed/api/v1/chat/{token}?format=json&lookIntoFuture=1&lastKnownMessageId={id}&timeout=0
```

---

## 5. Opening a Room

User clicks conversation. `gnalk_room_controller_open_room()` → `gnalk_session_join_room()`.

### 5.1 Join Room (HTTP)

```
POST /ocs/v2.php/apps/spreed/api/v4/room/{token}/participants/active?format=json
```
- **Response**: Room metadata + Nextcloud `sessionId`

### 5.2 Join Room (Signaling WS)

```json
→ { "type": "room", "room": { "roomid": "{token}" } }
← { "type": "room", "room": { "roomid": "{token}" } }
```

Emits `room-joined(token)`.

### 5.3 Participant Polling (60s interval)

```
GET /ocs/v2.php/apps/spreed/api/v4/room/{token}/participants?format=json&includeStatus=true
```

### 5.4 Message Long-Polling (active room)

```
GET /ocs/v2.php/apps/spreed/api/v1/chat/{token}?format=json&lookIntoFuture=1&lastKnownMessageId={id}&timeout=30&setReadMarker=0
```
- **Server timeout**: 30s, **HTTP timeout**: 35s
- Blocks until new messages or timeout, then re-polls continuously

---

## 6. Starting/Joining a Call

### 6.1 Join Call (HTTP)

```
POST /ocs/v2.php/apps/spreed/api/v4/call/{token}?format=json
```
- **Body**: `flags` bitmask: `0x1` audio, `0x3` audio+video, `0x7` audio+video+screen

### 6.2 Update Incall (Signaling WS)

```json
→ { "type": "incall", "incall": { "incall": 3 } }
```

---

## 7. WebRTC Signaling Exchange

File: `gnalk-call-signaling-manager.c`

### 7.1 Publishing (send own stream to MCU)

**Send offer:**
```json
→ {
    "type": "message",
    "message": {
      "recipient": { "type": "session", "sessionid": "{signaling_session_id}" },
      "data": {
        "type": "offer",
        "payload": { "nick": "...", "type": "offer", "sdp": "v=0\r\n..." }
      }
    }
  }
```

**Send ICE candidates** (repeated per candidate):
```json
→ {
    "type": "message",
    "message": {
      "recipient": { "type": "session", "sessionid": "{signaling_session_id}" },
      "data": {
        "type": "candidate",
        "payload": { "candidate": { "foundation": "...", "component": 1, ... } }
      }
    }
  }
```

**Receive answer from MCU:**
```json
← {
    "type": "message",
    "message": {
      "sender": { "sessionid": "{mcu_session_id}" },
      "data": { "type": "answer", "payload": { "type": "answer", "sdp": "..." } }
    }
  }
```

### 7.2 Subscribing (receive remote participants)

**Request offer from peer:**
```json
→ {
    "type": "message",
    "message": {
      "recipient": { "type": "session", "sessionid": "{peer_session_id}" },
      "data": { "type": "requestoffer", "roomType": "video" }
    }
  }
```

**Receive offer:**
```json
← {
    "type": "message",
    "message": {
      "sender": { "sessionid": "{peer_session_id}" },
      "data": { "type": "offer", "payload": { "nick": "...", "sdp": "..." } }
    }
  }
```

**Send answer:**
```json
→ {
    "type": "message",
    "message": {
      "recipient": { "type": "session", "sessionid": "{peer_session_id}" },
      "data": {
        "type": "answer",
        "payload": { "nick": "...", "sdp": "...", "sid": "{subscriber_sid}" }
      }
    }
  }
```

Then ICE candidate exchange with peer (same format as MCU).

---

## 8. Screen Sharing

**Offer** (same as publish, with `roomType: "screen"`):
```json
→ { ..., "data": { "type": "offer", "roomType": "screen", "payload": { "sdp": "..." } } }
```

**Broadcast to room:**
```json
→ { ..., "data": { "type": "shareScreen" } }
```

---

## 9. Room Events (Signaling WS, server-initiated)

### Participant Join
```json
← { "type": "event", "event": { "target": "participants", "type": "join",
    "join": [{ "sessionid": "...", "user": { "displayname": "..." } }] } }
```

### Room Update (call state changes)
```json
← { "type": "event", "event": { "target": "room", "type": "update",
    "update": { "users": [{ "sessionId": "...", "inCall": 3 }] } } }
```

### Chat Message via HPB
```json
← { "type": "event", "event": { "target": "room", "type": "message",
    "message": { "data": { "type": "chat", "chat": { "refresh": true, "comment": {...} } } } } }
```

### Participant Leave
```json
← { "type": "event", "event": { "target": "room", "type": "leave",
    "leave": ["sessionid1", "sessionid2"] } }
```

---

## 10. Ending a Call

### Leave Call (HTTP)
```
DELETE /ocs/v2.php/apps/spreed/api/v4/call/{token}?format=json
```

### Update Incall (Signaling WS)
```json
→ { "type": "incall", "incall": { "incall": 0 } }
```

Then close WebRTC peer connections and GStreamer pipelines.

---

## 11. Federation Calls

### Accept Invitation
```
POST /ocs/v2.php/apps/spreed/api/v1/federation/invitation/{id}?format=json
```

### Get Remote Signaling Settings
```
GET /ocs/v2.php/apps/spreed/api/v3/signaling/settings?format=json  (room context)
```
Response: `ocs.data.federation.{signaling_server, auth_token, nextcloud_server}`

### Connect Remote Signaling
`gnalk_signaling_connect_federation(server_url, nextcloud_server, jwt_token)` — JWT auth instead of ticket.

### Start Federated Call
```
POST /ocs/v2.php/apps/spreed/api/v4/call/{token}/federation?format=json
```

### Federation Events
```json
← { "type": "event", "event": { "target": "room", "type": "federation_interrupted" } }
← { "type": "event", "event": { "target": "room", "type": "federation_resumed", "resumed": true } }
```

---

## Endpoint Summary

| Step | Method | Endpoint |
|------|--------|----------|
| Auth | `GET` | `/status.php` |
| User info | `GET` | `/ocs/v2.php/cloud/user` |
| Conversations | `GET` | `/ocs/v2.php/apps/spreed/api/v4/room` |
| Capabilities | `GET` | `/ocs/v2.php/cloud/capabilities` |
| Signaling settings | `GET` | `/ocs/v2.php/apps/spreed/api/v3/signaling/settings` |
| Join room | `POST` | `/ocs/v2.php/apps/spreed/api/v4/room/{token}/participants/active` |
| Leave room | `DELETE` | `/ocs/v2.php/apps/spreed/api/v4/room/{token}/participants/active` |
| Participants | `GET` | `/ocs/v2.php/apps/spreed/api/v4/room/{token}/participants` |
| Messages | `GET` | `/ocs/v2.php/apps/spreed/api/v1/chat/{token}` |
| Send message | `POST` | `/ocs/v2.php/apps/spreed/api/v1/chat/{token}` |
| Join call | `POST` | `/ocs/v2.php/apps/spreed/api/v4/call/{token}` |
| Leave call | `DELETE` | `/ocs/v2.php/apps/spreed/api/v4/call/{token}` |
| Federation invitations | `GET` | `/ocs/v2.php/apps/spreed/api/v1/federation/invitation` |
| Accept invitation | `POST` | `/ocs/v2.php/apps/spreed/api/v1/federation/invitation/{id}` |
| Federated call join | `POST` | `/ocs/v2.php/apps/spreed/api/v4/call/{token}/federation` |

## WebSocket Connections

| Name | URL Source | Auth |
|------|-----------|------|
| **Signaling** | `signaling/settings` → `server` | Ticket (URL param) |
| **Push (notify_push)** | `capabilities` → `notify_push.endpoints.websocket` | user_id + password (plain text) |
| **Federation signaling** | `signaling/settings` → `federation.signaling_server` | JWT token |

## Key Source Files

| File | Role |
|------|------|
| `gnalk-session.c` | Auth chain orchestration |
| `gnalk-http-client.c` | All HTTP endpoints |
| `gnalk-signaling.c` | Signaling WebSocket protocol |
| `gnalk-push-client.c` | Push WebSocket |
| `gnalk-sync-coordinator.c` | Periodic sync (20s) |
| `gnalk-room-controller.c` | Room open/close |
| `gnalk-call-signaling-manager.c` | WebRTC signaling messages |
| `gnalk-call-window.c` | Call UI + WebRTC setup |
