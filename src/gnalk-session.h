#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "gnalk-account.h"
#include "gnalk-http-client.h"
#include "gnalk-signaling.h"
#include "gnalk-sync-manager.h"
#include "gnalk-database.h"
#include "gnalk-network-monitor.h"
#include "gnalk-conversation-data.h"

G_BEGIN_DECLS

#define GNALK_TYPE_SESSION (gnalk_session_get_type ())
G_DECLARE_FINAL_TYPE (GnalkSession, gnalk_session, GNALK, SESSION, GObject)

/**
 * GnalkSessionState:
 * @GNALK_SESSION_STATE_DISCONNECTED: Not connected (initial state, or after logout)
 * @GNALK_SESSION_STATE_CONNECTING: Authentication in progress
 * @GNALK_SESSION_STATE_ONLINE: Fully connected and operational
 * @GNALK_SESSION_STATE_OFFLINE: Detected offline, waiting for network
 * @GNALK_SESSION_STATE_RECONNECTING: Actively attempting reconnection
 *
 * Session connection state machine.
 */
typedef enum {
  GNALK_SESSION_STATE_DISCONNECTED,
  GNALK_SESSION_STATE_CONNECTING,
  GNALK_SESSION_STATE_ONLINE,
  GNALK_SESSION_STATE_OFFLINE,
  GNALK_SESSION_STATE_RECONNECTING
} GnalkSessionState;

/* Timeout constants */
#define GNALK_SESSION_DEFAULT_TIMEOUT_SEC     35
#define GNALK_SESSION_AUTH_TIMEOUT_SEC         5
#define GNALK_SESSION_RECONNECT_INTERVAL_SEC   5
#define GNALK_SESSION_MAX_RECONNECT_ATTEMPTS  10
#define GNALK_SESSION_HEALTH_CHECK_INTERVAL   30
#define GNALK_SESSION_STALE_THRESHOLD_SEC     60

/**
 * GnalkSession:
 *
 * Self-contained connection context for a single account. Owns:
 * - HTTP client (configured with account credentials)
 * - Signaling WebSocket
 * - Sync manager (offline queue)
 * - Conversation list
 * - Connection state machine
 * - Reconnection logic
 *
 * Signals:
 * - "state-changed" (GnalkSessionState old_state, GnalkSessionState new_state)
 *     Emitted when connection state changes
 * - "conversations-updated" ()
 *     Emitted when conversation list is refreshed
 * - "room-joined" (const char *room_token, const char *session_id)
 *     Emitted when HTTP room join succeeds
 * - "room-left" ()
 *     Emitted when leaving current room
 * - "signaling-room-joined" (const char *room_token)
 *     Emitted when signaling confirms room join (call buttons can be enabled)
 * - "unread-count-changed" (int total_unread)
 *     Emitted when total unread message count changes
 * - "call-capability-changed" (gboolean can_call)
 *     Emitted when ability to make calls changes
 * - "message-received" (const char *room_token, gint64 message_id)
 *     Emitted when a new message arrives (for notifications)
 * - "auth-failed" (const char *error_message)
 *     Emitted when authentication fails
 * - "signaling-ready" ()
 *     Emitted when signaling WebSocket is authenticated and ready
 * - "incoming-call" (const char *room_token, const char *caller_id)
 *     Emitted when someone starts a call
 * - "typing" (const char *room_token, const char *session_id, const char *display_name, gboolean is_typing)
 *     Emitted when typing indicator changes
 */

GnalkSession *gnalk_session_new (GnalkAccount        *account,
                                     GnalkDatabase       *database,
                                     GnalkNetworkMonitor *network_monitor);

GnalkSession *gnalk_session_new_guest (const char          *server_url,
                                            const char          *room_token,
                                            const char          *display_name,
                                            const char          *room_password,
                                            GnalkDatabase       *database,
                                            GnalkNetworkMonitor *network_monitor);

/* State queries */
GnalkSessionState gnalk_session_get_state (GnalkSession *self);
gboolean           gnalk_session_is_online (GnalkSession *self);
gboolean           gnalk_session_is_guest  (GnalkSession *self);
gboolean           gnalk_session_can_call  (GnalkSession *self);

/* Account */
GnalkAccount *gnalk_session_get_account (GnalkSession *self);
gint64         gnalk_session_get_account_id (GnalkSession *self);

/* Connection lifecycle */
void gnalk_session_connect       (GnalkSession *self,
                                    const char    *password);
void gnalk_session_connect_guest (GnalkSession *self);
void gnalk_session_disconnect    (GnalkSession *self);

/* Room management */
void        gnalk_session_join_room  (GnalkSession *self,
                                        const char    *room_token);
void        gnalk_session_leave_room (GnalkSession *self);
const char *gnalk_session_get_current_room_token (GnalkSession *self);
const char *gnalk_session_get_current_session_id (GnalkSession *self);

/* Conversations */
GListStore *gnalk_session_get_conversations   (GnalkSession *self);
void        gnalk_session_refresh_conversations (GnalkSession *self);
int         gnalk_session_get_total_unread    (GnalkSession *self);

/* Messaging (delegates to sync manager when offline) */
void gnalk_session_send_message (GnalkSession *self,
                                   const char    *room_token,
                                   const char    *message,
                                   gint64         reply_to,
                                   const char    *reference_id);

void gnalk_session_send_file (GnalkSession *self,
                                const char    *room_token,
                                const char    *file_path,
                                const char    *file_name,
                                const char    *content_type,
                                const char    *caption,
                                gint64         reply_to,
                                const char    *reference_id);

/* Internal component access (for call window, etc.) */
GnalkHttpClient  *gnalk_session_get_http_client  (GnalkSession *self);
GnalkSignaling   *gnalk_session_get_signaling    (GnalkSession *self);
GnalkSyncManager *gnalk_session_get_sync_manager (GnalkSession *self);

/* Signaling settings (set after auth, used for connecting signaling) */
void gnalk_session_set_signaling_settings (GnalkSession *self,
                                              const char    *server,
                                              const char    *ticket);

/* ICE servers (set after auth, needed for WebRTC) */
void        gnalk_session_set_ice_servers (GnalkSession *self,
                                             const char    *stun_server,
                                             const char    *turn_server,
                                             const char    *turn_username,
                                             const char    *turn_credential);
const char *gnalk_session_get_stun_server     (GnalkSession *self);
const char *gnalk_session_get_turn_server     (GnalkSession *self);
const char *gnalk_session_get_turn_username   (GnalkSession *self);
const char *gnalk_session_get_turn_credential (GnalkSession *self);

/* Manual reconnection trigger */
void gnalk_session_attempt_reconnect (GnalkSession *self);

/* Message sync state */
gint64 gnalk_session_get_last_known_message_id (GnalkSession *self,
                                                   const char    *room_token);
void   gnalk_session_set_last_known_message_id (GnalkSession *self,
                                                   const char    *room_token,
                                                   gint64         message_id);

G_END_DECLS
