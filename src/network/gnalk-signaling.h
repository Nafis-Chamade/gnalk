#pragma once

#include <glib-object.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define GNALK_TYPE_SIGNALING (gnalk_signaling_get_type ())
G_DECLARE_FINAL_TYPE (GnalkSignaling, gnalk_signaling, GNALK, SIGNALING, GObject)

typedef void (*GnalkSignalingMessageCallback) (GnalkSignaling *signaling,
                                                  JsonNode        *message,
                                                  gpointer         user_data);

GnalkSignaling *gnalk_signaling_new (void);

void gnalk_signaling_connect (GnalkSignaling *self,
                                const char      *server_url,
                                const char      *backend_url,
                                const char      *user_id,
                                const char      *ticket);

/* Connect to a remote signaling server with federation auth (JWT token).
 * Used for federated calls where the client connects to the host's signaling. */
void gnalk_signaling_connect_federation (GnalkSignaling *self,
                                          const char      *server_url,
                                          const char      *nextcloud_server_url,
                                          const char      *auth_token);

void gnalk_signaling_disconnect (GnalkSignaling *self);

gboolean gnalk_signaling_is_connected (GnalkSignaling *self);

void gnalk_signaling_send_message (GnalkSignaling *self,
                                     JsonNode        *message);

void gnalk_signaling_join_room (GnalkSignaling *self,
                                  const char      *room_token,
                                  const char      *session_id);

void gnalk_signaling_leave_room (GnalkSignaling *self,
                                   const char      *room_token);

void gnalk_signaling_set_message_callback (GnalkSignaling                *self,
                                             GnalkSignalingMessageCallback  callback,
                                             gpointer                        user_data);

gboolean gnalk_signaling_has_mcu (GnalkSignaling *self);

const char *gnalk_signaling_get_session_id (GnalkSignaling *self);

void gnalk_signaling_send_typing (GnalkSignaling *self,
                                    gboolean         typing);

const char *gnalk_signaling_get_display_name_for_session (GnalkSignaling *self,
                                                            const char      *session_id);

/* Update incall status via signaling WebSocket.
 * This tells the MCU we're ready to receive streams.
 * flags: 0=not in call, 1=audio, 3=audio+video, 7=audio+video+screenshare */
void gnalk_signaling_update_incall (GnalkSignaling *self,
                                      gint             flags);

/* Broadcast screen share start to all participants */
void gnalk_signaling_send_share_screen (GnalkSignaling *self);

/* Broadcast screen share stop to all participants */
void gnalk_signaling_send_unshare_screen (GnalkSignaling *self);

/* Broadcast hand raise/lower to all participants in the call */
void gnalk_signaling_send_raise_hand (GnalkSignaling *self,
                                       gboolean         raised);

/* Broadcast emoji reaction to all participants in the call */
void gnalk_signaling_send_call_reaction (GnalkSignaling *self,
                                          const char      *emoji);

/* Signals:
 * - "ready" ()
 *     Emitted when signaling is authenticated and ready
 * - "room-joined" (const char *room_token)
 *     Emitted when server confirms room join (call buttons can be enabled)
 * - "room-disinvited" (const char *room_token)
 *     Emitted when server kicks us from a room (window should clear room state)
 * - "connection-lost" ()
 *     Emitted when WebSocket closes unexpectedly (triggers offline mode)
 * - "screen-share-changed" (const char *session_id, gboolean is_sharing)
 *     Emitted when a participant starts or stops screen sharing
 * - "typing" (const char *session_id, const char *display_name, gboolean is_typing)
 *     Emitted when a participant starts or stops typing
 * - "incoming-call" (const char *session_id, const char *actor_id, gint64 in_call)
 *     Emitted when someone starts a call
 * - "call-state-changed" (const char *actor_id, gint64 in_call)
 *     Emitted when a participant's call state changes
 * - "call-ended" ()
 *     Emitted when all participants leave the call
 * - "chat-message" (JsonObject *chat_data)
 *     Emitted when HPB relays a chat message (contains "refresh" bool and optional "comment" object)
 * - "hand-raised" (const char *session_id, gboolean state, gint64 timestamp)
 *     Emitted when a participant raises or lowers their hand
 * - "call-reaction" (const char *session_id, const char *emoji)
 *     Emitted when a participant sends an emoji reaction during a call
 * - "federation-interrupted" ()
 *     Emitted when the federation connection to the remote server is lost
 * - "federation-resumed" (gboolean session_preserved)
 *     Emitted when the federation connection is restored
 */

/**
 * gnalk_signaling_get_last_activity_time:
 * @self: The signaling instance
 *
 * Returns the monotonic time (in seconds) of the last received message.
 * Used for connection health monitoring.
 *
 * Returns: Monotonic time in seconds, or 0 if no message received
 */
gint64 gnalk_signaling_get_last_activity_time (GnalkSignaling *self);

/**
 * gnalk_signaling_get_current_room_token:
 * @self: The signaling instance
 *
 * Returns the token of the currently joined room, or NULL if not in a room.
 *
 * Returns: (nullable): The current room token
 */
const char *gnalk_signaling_get_current_room_token (GnalkSignaling *self);

G_END_DECLS
