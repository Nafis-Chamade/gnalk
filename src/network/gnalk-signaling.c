#include "gnalk-signaling.h"
#include "gnalk-log.h"

struct _GnalkSignaling
{
  GObject parent_instance;

  SoupSession    *session;
  SoupWebsocketConnection *ws_connection;
  SoupMessage    *pending_ws_msg;  /* Keep message alive during async connect */
  char           *server_url;
  char           *backend_url;  /* Nextcloud server URL for auth */
  char           *user_id;
  char           *ticket;
  char           *session_id;           /* Room session ID (from Nextcloud) */
  char           *signaling_session_id; /* Signaling session ID (from hello response) */
  char           *current_room_token;   /* Currently joined room token */
  gboolean        connected;
  gboolean        has_mcu;  /* Server has MCU support */
  gboolean        pending_reconnect;  /* Reconnect after current disconnect completes */
  guint           reconnect_timeout_id;
  guint64         message_id;

  /* WebSocket signal handler IDs for proper cleanup */
  gulong          ws_message_handler_id;
  gulong          ws_closed_handler_id;
  gulong          ws_error_handler_id;

  /* Activity tracking for connection health */
  gint64          last_activity_time;   /* Monotonic time of last received message */

  GHashTable     *participants;      /* session_id -> display_name */
  GHashTable     *session_to_actor;  /* session_id -> actor_id */

  gboolean        is_federation;     /* TRUE when using federation auth */
  char           *fed_auth_token;    /* JWT token for federation auth */

  GCancellable   *connect_cancellable;  /* Cancels pending WebSocket connect on disconnect/finalize */

  GnalkSignalingMessageCallback message_callback;
  gpointer                       message_callback_data;
};

G_DEFINE_FINAL_TYPE (GnalkSignaling, gnalk_signaling, G_TYPE_OBJECT)

enum {
  SIGNAL_TYPING,
  SIGNAL_INCOMING_CALL,
  SIGNAL_CALL_STATE_CHANGED,
  SIGNAL_CALL_ENDED,
  SIGNAL_READY,
  SIGNAL_SCREEN_SHARE_CHANGED,
  SIGNAL_ROOM_JOINED,                /* Emitted when server confirms room join */
  SIGNAL_ROOM_DISINVITED,            /* Emitted when kicked from a room */
  SIGNAL_CONNECTION_LOST,            /* Emitted when WebSocket closes unexpectedly */
  SIGNAL_CHAT_MESSAGE,               /* Emitted when HPB relays a chat message */
  SIGNAL_HAND_RAISED,                /* Emitted when a participant raises/lowers hand */
  SIGNAL_CALL_REACTION,              /* Emitted when a participant sends emoji reaction */
  SIGNAL_FEDERATION_INTERRUPTED,     /* Emitted when federation connection is lost */
  SIGNAL_FEDERATION_RESUMED,         /* Emitted when federation connection is restored */
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void gnalk_signaling_do_connect (GnalkSignaling *self);

static void
gnalk_signaling_finalize (GObject *object)
{
  GnalkSignaling *self = GNALK_SIGNALING (object);

  /* Cancel any pending WebSocket connect to prevent use-after-free */
  g_cancellable_cancel (self->connect_cancellable);
  g_clear_object (&self->connect_cancellable);

  if (self->reconnect_timeout_id > 0)
    g_source_remove (self->reconnect_timeout_id);

  /* Disconnect all signal handlers before object destruction.
   * This is critical because soup_websocket_connection_close is async
   * and we don't want the "closed" callback to fire on freed memory. */
  if (self->ws_connection)
    {
      if (self->ws_message_handler_id > 0)
        g_signal_handler_disconnect (self->ws_connection, self->ws_message_handler_id);
      if (self->ws_error_handler_id > 0)
        g_signal_handler_disconnect (self->ws_connection, self->ws_error_handler_id);
      if (self->ws_closed_handler_id > 0)
        g_signal_handler_disconnect (self->ws_connection, self->ws_closed_handler_id);

      self->ws_message_handler_id = 0;
      self->ws_error_handler_id = 0;
      self->ws_closed_handler_id = 0;

      /* Close if still open, then clear the object */
      SoupWebsocketState state = soup_websocket_connection_get_state (self->ws_connection);
      if (state == SOUP_WEBSOCKET_STATE_OPEN)
        soup_websocket_connection_close (self->ws_connection, 1000, "Finalize");

      g_clear_object (&self->ws_connection);
    }

  self->connected = FALSE;

  g_clear_object (&self->pending_ws_msg);
  g_clear_object (&self->session);
  g_clear_pointer (&self->participants, g_hash_table_unref);
  g_clear_pointer (&self->session_to_actor, g_hash_table_unref);
  g_free (self->server_url);
  g_free (self->backend_url);
  g_free (self->user_id);
  g_free (self->ticket);
  g_free (self->session_id);
  g_free (self->signaling_session_id);
  g_free (self->current_room_token);
  g_free (self->fed_auth_token);

  G_OBJECT_CLASS (gnalk_signaling_parent_class)->finalize (object);
}

static void
gnalk_signaling_class_init (GnalkSignalingClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_signaling_finalize;

  signals[SIGNAL_TYPING] =
    g_signal_new ("typing",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_STRING,   /* session_id */
                  G_TYPE_STRING,   /* display_name */
                  G_TYPE_BOOLEAN); /* is_typing */

  signals[SIGNAL_INCOMING_CALL] =
    g_signal_new ("incoming-call",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_STRING,   /* session_id */
                  G_TYPE_STRING,   /* actor_id (user_id) */
                  G_TYPE_INT64);   /* in_call flags */

  signals[SIGNAL_CALL_STATE_CHANGED] =
    g_signal_new ("call-state-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING,   /* actor_id (user_id) */
                  G_TYPE_INT64);   /* in_call flags */

  signals[SIGNAL_CALL_ENDED] =
    g_signal_new ("call-ended",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);  /* no parameters */

  signals[SIGNAL_READY] =
    g_signal_new ("ready",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);  /* no parameters */

  signals[SIGNAL_SCREEN_SHARE_CHANGED] =
    g_signal_new ("screen-share-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING,   /* session_id */
                  G_TYPE_BOOLEAN); /* is_sharing */

  /**
   * GnalkSignaling::room-joined:
   * @signaling: The signaling instance
   * @room_token: The room token that was joined
   *
   * Emitted when the server confirms that we have joined a signaling room.
   * This is the confirmation that call buttons should be enabled.
   */
  signals[SIGNAL_ROOM_JOINED] =
    g_signal_new ("room-joined",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);  /* room_token */

  /**
   * GnalkSignaling::room-disinvited:
   * @signaling: The signaling instance
   * @room_token: The room token we were kicked from
   *
   * Emitted when the server kicks us from a room (disinvite event).
   * The window should clear its room state and not try to rejoin.
   */
  signals[SIGNAL_ROOM_DISINVITED] =
    g_signal_new ("room-disinvited",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);  /* room_token */

  /**
   * GnalkSignaling::connection-lost:
   * @signaling: The signaling instance
   *
   * Emitted when the WebSocket connection is lost unexpectedly.
   * The ConnectionManager should trigger offline mode recovery.
   */
  signals[SIGNAL_CONNECTION_LOST] =
    g_signal_new ("connection-lost",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_CHAT_MESSAGE] =
    g_signal_new ("chat-message",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1,
                  JSON_TYPE_OBJECT);  /* chat_data */

  signals[SIGNAL_HAND_RAISED] =
    g_signal_new ("hand-raised",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_STRING,   /* session_id */
                  G_TYPE_BOOLEAN,  /* state */
                  G_TYPE_INT64);   /* timestamp */

  signals[SIGNAL_CALL_REACTION] =
    g_signal_new ("call-reaction",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING,   /* session_id */
                  G_TYPE_STRING);  /* emoji */

  signals[SIGNAL_FEDERATION_INTERRUPTED] =
    g_signal_new ("federation-interrupted",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_FEDERATION_RESUMED] =
    g_signal_new ("federation-resumed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_BOOLEAN);  /* resumed (TRUE if session was preserved) */
}

static void
gnalk_signaling_init (GnalkSignaling *self)
{
  self->session = soup_session_new ();
  self->participants = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->session_to_actor = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}

GnalkSignaling *
gnalk_signaling_new (void)
{
  return g_object_new (GNALK_TYPE_SIGNALING, NULL);
}

static void
on_ws_message (SoupWebsocketConnection *ws G_GNUC_UNUSED,
               SoupWebsocketDataType    type,
               GBytes                  *message,
               gpointer                 user_data)
{
  GnalkSignaling *self = GNALK_SIGNALING (user_data);

  if (type != SOUP_WEBSOCKET_DATA_TEXT)
    return;

  /* Update activity timestamp for connection health tracking */
  self->last_activity_time = g_get_monotonic_time () / G_USEC_PER_SEC;

  gsize size;
  const char *data = g_bytes_get_data (message, &size);

  g_autofree char *msg_copy = g_strndup (data, size);
  gnalk_log_ws_recv (self->user_id, msg_copy);

  g_autoptr(JsonParser) parser = json_parser_new ();
  g_autoptr(GError) error = NULL;

  if (!json_parser_load_from_data (parser, data, size, &error))
    {
      g_warning ("Failed to parse signaling message: %s", error->message);
      return;
    }

  JsonNode *root = json_parser_get_root (parser);

  /* Check for hello response to detect MCU support and get session ID */
  if (JSON_NODE_HOLDS_OBJECT (root))
    {
      JsonObject *msg_obj = json_node_get_object (root);
      const char *msg_type = json_object_get_string_member_with_default (msg_obj, "type", "");

      if (g_strcmp0 (msg_type, "hello") == 0)
        {
          JsonObject *hello = json_object_get_object_member (msg_obj, "hello");
          if (hello)
            {
              /* Store our signaling session ID */
              const char *session_id = json_object_get_string_member_with_default (hello, "sessionid", NULL);
              if (session_id)
                {
                  g_free (self->signaling_session_id);
                  self->signaling_session_id = g_strdup (session_id);
                }

              /* Check for MCU support in server features */
              JsonObject *server = json_object_get_object_member (hello, "server");
              if (server)
                {
                  JsonArray *features = json_object_get_array_member (server, "features");
                  if (features)
                    {
                      guint len = json_array_get_length (features);
                      for (guint i = 0; i < len; i++)
                        {
                          const char *feat = json_array_get_string_element (features, i);
                          if (g_strcmp0 (feat, "mcu") == 0)
                            {
                              self->has_mcu = TRUE;
                              break;
                            }
                        }
                    }
                }
            }

          /* Emit ready signal now that we're fully authenticated */
          g_signal_emit (self, signals[SIGNAL_READY], 0);
        }
      else if (g_strcmp0 (msg_type, "event") == 0)
        {
          /* Parse participant events to track who's in the room */
          JsonObject *event = json_object_get_object_member (msg_obj, "event");
          if (event)
            {
              const char *target = json_object_get_string_member_with_default (event, "target", "");
              /* Handle roomlist events (disinvite, etc.) */
              if (g_strcmp0 (target, "roomlist") == 0)
                {
                  const char *event_type = json_object_get_string_member_with_default (event, "type", "");
                  if (g_strcmp0 (event_type, "disinvite") == 0)
                    {
                      /* Server is kicking us from a room - this often precedes a connection close.
                       * Clear our room state immediately to prevent sending on a closing connection. */
                      JsonObject *disinvite = json_object_get_object_member (event, "disinvite");
                      const char *roomid = disinvite ?
                        json_object_get_string_member_with_default (disinvite, "roomid", "") : "";
                      g_message ("Received disinvite from room %s, clearing room state", roomid);

                      /* If this is our current room, clear it */
                      if (self->current_room_token && g_strcmp0 (self->current_room_token, roomid) == 0)
                        {
                          g_clear_pointer (&self->current_room_token, g_free);
                        }

                      /* Notify the window so it can clear its state too */
                      g_signal_emit (self, signals[SIGNAL_ROOM_DISINVITED], 0, roomid);
                    }
                }
              else if (g_strcmp0 (target, "room") == 0)
                {
                  const char *event_type = json_object_get_string_member_with_default (event, "type", "");
                  if (g_strcmp0 (event_type, "message") == 0)
                    {
                      /* HPB chat relay: event.message.data.type == "chat" */
                      JsonObject *message = json_object_get_object_member (event, "message");
                      if (message)
                        {
                          JsonObject *data = json_object_get_object_member (message, "data");
                          if (data)
                            {
                              const char *data_type = json_object_get_string_member_with_default (data, "type", "");
                              if (g_strcmp0 (data_type, "chat") == 0)
                                {
                                  JsonObject *chat = json_object_get_object_member (data, "chat");
                                  if (chat)
                                    g_signal_emit (self, signals[SIGNAL_CHAT_MESSAGE], 0, chat);
                                }
                            }
                        }
                    }
                }
              if (g_strcmp0 (target, "participants") == 0 || g_strcmp0 (target, "room") == 0)
                {
                  const char *event_type = json_object_get_string_member_with_default (event, "type", "");
                  if (g_strcmp0 (event_type, "join") == 0)
                    {
                      JsonArray *join_array = json_object_get_array_member (event, "join");
                      if (join_array)
                        {
                          guint len = json_array_get_length (join_array);
                          for (guint i = 0; i < len; i++)
                            {
                              JsonObject *p = json_array_get_object_element (join_array, i);
                              const char *sess_id = json_object_get_string_member_with_default (p, "sessionid", NULL);
                              if (sess_id)
                                {
                                  /* Regular users have a user object, guests may not */
                                  const char *name = "";
                                  if (json_object_has_member (p, "user"))
                                    {
                                      JsonObject *user = json_object_get_object_member (p, "user");
                                      if (user)
                                        name = json_object_get_string_member_with_default (user, "displayname", "");
                                    }
                                  g_hash_table_insert (self->participants,
                                                       g_strdup (sess_id),
                                                       g_strdup (name));
                                }
                            }
                        }
                    }
                  else if (g_strcmp0 (event_type, "update") == 0)
                    {
                      /* participants/update has a different structure: update.users[]
                       * Note: "end call for all" sends {"all":true,"incall":0} without users array */
                      JsonObject *update = json_object_get_object_member (event, "update");
                      if (update)
                        {
                          /* Check for "end call for all" broadcast */
                          if (json_object_has_member (update, "all") &&
                              json_object_get_boolean_member_with_default (update, "all", FALSE) &&
                              json_object_get_int_member_with_default (update, "incall", -1) == 0)
                            {
                              /* All participants left the call - emit signal */
                              g_signal_emit (self, signals[SIGNAL_CALL_ENDED], 0);
                            }

                          JsonArray *users = NULL;
                          if (json_object_has_member (update, "users"))
                            users = json_object_get_array_member (update, "users");
                          if (users)
                            {
                              guint len = json_array_get_length (users);
                              for (guint i = 0; i < len; i++)
                                {
                                  JsonObject *user = json_array_get_object_element (users, i);
                                  if (!user)
                                    continue;
                                  /* Note: update events use "sessionId" (camelCase) not "sessionid" */
                                  const char *sess_id = json_object_get_string_member_with_default (user, "sessionId", NULL);
                                  const char *actor_id = json_object_get_string_member_with_default (user, "actorId", NULL);
                                  gint64 in_call = json_object_get_int_member_with_default (user, "inCall", 0);

                                  if (actor_id)
                                    {
                                      /* Store session_id -> actor_id mapping for leave events */
                                      if (sess_id)
                                        g_hash_table_insert (self->session_to_actor,
                                                             g_strdup (sess_id),
                                                             g_strdup (actor_id));

                                      /* Emit call state changed — dedup is done by PC via DB */
                                      g_signal_emit (self, signals[SIGNAL_CALL_STATE_CHANGED], 0,
                                                     actor_id, in_call);

                                      /* Also emit incoming call signal for others with inCall > 0 */
                                      if (sess_id && in_call > 0)
                                        {
                                          /* Skip ourselves */
                                          if (self->signaling_session_id &&
                                              g_strcmp0 (sess_id, self->signaling_session_id) == 0)
                                            continue;

                                          g_signal_emit (self, signals[SIGNAL_INCOMING_CALL], 0,
                                                         sess_id, actor_id, in_call);
                                        }
                                    }
                                }
                            }
                        }
                    }
                  else if (g_strcmp0 (event_type, "federation_interrupted") == 0)
                    {
                      g_message ("Signaling: federation connection interrupted");
                      g_signal_emit (self, signals[SIGNAL_FEDERATION_INTERRUPTED], 0);
                    }
                  else if (g_strcmp0 (event_type, "federation_resumed") == 0)
                    {
                      gboolean resumed = FALSE;
                      if (json_object_has_member (event, "resumed"))
                        resumed = json_object_get_boolean_member_with_default (event, "resumed", FALSE);
                      g_message ("Signaling: federation connection resumed (session_preserved=%d)", resumed);
                      g_signal_emit (self, signals[SIGNAL_FEDERATION_RESUMED], 0, resumed);
                    }
                  else if (g_strcmp0 (event_type, "leave") == 0)
                    {
                      JsonArray *leave_array = json_object_get_array_member (event, "leave");
                      if (leave_array)
                        {
                          guint len = json_array_get_length (leave_array);
                          for (guint i = 0; i < len; i++)
                            {
                              const char *sess_id = json_array_get_string_element (leave_array, i);
                              if (sess_id)
                                {
                                  g_hash_table_remove (self->participants, sess_id);

                                  /* Emit call-state-changed(0) for leaving participant — PC dedup via DB */
                                  const char *actor_id = g_hash_table_lookup (self->session_to_actor, sess_id);
                                  if (actor_id)
                                    g_signal_emit (self, signals[SIGNAL_CALL_STATE_CHANGED], 0,
                                                   actor_id, (gint64) 0);
                                  g_hash_table_remove (self->session_to_actor, sess_id);
                                }
                            }
                        }
                    }
                }
            }
        }
      else if (g_strcmp0 (msg_type, "room") == 0)
        {
          /* Server confirms room join - parse room response */
          JsonObject *room = json_object_get_object_member (msg_obj, "room");
          if (room)
            {
              const char *roomid = json_object_get_string_member_with_default (room, "roomid", "");
              if (roomid && *roomid)
                {
                  /* Store current room token and emit room-joined signal */
                  g_free (self->current_room_token);
                  self->current_room_token = g_strdup (roomid);
                  g_message ("Signaling room joined: %s", roomid);
                  g_signal_emit (self, signals[SIGNAL_ROOM_JOINED], 0, roomid);
                }
            }
        }
      else if (g_strcmp0 (msg_type, "message") == 0)
        {
          /* Parse typing messages */
          JsonObject *msg_data = json_object_get_object_member (msg_obj, "message");
          if (msg_data)
            {
              const char *sender_session = NULL;
              JsonObject *sender_obj = json_object_get_object_member (msg_data, "sender");
              if (sender_obj)
                sender_session = json_object_get_string_member_with_default (sender_obj, "sessionid", NULL);

              JsonObject *inner_data = json_object_get_object_member (msg_data, "data");
              if (inner_data && sender_session)
                {
                  const char *msg_data_type = json_object_get_string_member_with_default (inner_data, "type", "");
                  if (g_strcmp0 (msg_data_type, "startedTyping") == 0 ||
                      g_strcmp0 (msg_data_type, "stoppedTyping") == 0)
                    {
                      gboolean is_typing = g_strcmp0 (msg_data_type, "startedTyping") == 0;
                      const char *display_name = g_hash_table_lookup (self->participants, sender_session);
                      g_signal_emit (self, signals[SIGNAL_TYPING], 0,
                                     sender_session,
                                     display_name ? display_name : "",
                                     is_typing);
                    }
                  else if (g_strcmp0 (msg_data_type, "shareScreen") == 0 ||
                           g_strcmp0 (msg_data_type, "unshareScreen") == 0)
                    {
                      gboolean is_sharing = g_strcmp0 (msg_data_type, "shareScreen") == 0;
                      g_message ("Screen share %s from session %s",
                                 is_sharing ? "started" : "stopped", sender_session);
                      g_signal_emit (self, signals[SIGNAL_SCREEN_SHARE_CHANGED], 0,
                                     sender_session, is_sharing);
                    }
                  else if (g_strcmp0 (msg_data_type, "raiseHand") == 0)
                    {
                      JsonObject *payload = json_object_get_object_member (inner_data, "payload");
                      if (payload)
                        {
                          gboolean state = json_object_get_boolean_member_with_default (payload, "state", FALSE);
                          gint64 timestamp = json_object_get_int_member_with_default (payload, "timestamp", 0);
                          g_signal_emit (self, signals[SIGNAL_HAND_RAISED], 0,
                                         sender_session, state, timestamp);
                        }
                    }
                  else if (g_strcmp0 (msg_data_type, "reaction") == 0)
                    {
                      JsonObject *payload = json_object_get_object_member (inner_data, "payload");
                      if (payload)
                        {
                          const char *reaction = json_object_get_string_member_with_default (payload, "reaction", "");
                          g_signal_emit (self, signals[SIGNAL_CALL_REACTION], 0,
                                         sender_session, reaction);
                        }
                    }
                }
            }
        }
    }

  if (self->message_callback)
    self->message_callback (self, root, self->message_callback_data);
}

static void
on_ws_closed (SoupWebsocketConnection *ws,
              gpointer                 user_data)
{
  GnalkSignaling *self = GNALK_SIGNALING (user_data);
  gboolean was_connected = self->connected;

  /* Check if this is the current connection or an old one being closed.
   * This handles the race condition where a new connection is started
   * before the old one finishes closing. */
  if (self->ws_connection != ws)
    {
      g_message ("Signaling: old WebSocket closed, ignoring (already reconnected)");
      return;
    }

  g_message ("Signaling WebSocket closed (was_connected=%d)", was_connected);

  /* Clear handler IDs since the connection is closing and they'll be invalid.
   * This prevents double-disconnect if gnalk_signaling_disconnect is called later. */
  self->ws_message_handler_id = 0;
  self->ws_closed_handler_id = 0;
  self->ws_error_handler_id = 0;

  self->connected = FALSE;
  g_clear_object (&self->ws_connection);
  g_clear_pointer (&self->current_room_token, g_free);

  /* Check if we need to reconnect (requested by gnalk_signaling_connect) */
  if (self->pending_reconnect)
    {
      self->pending_reconnect = FALSE;
      g_message ("Signaling: disconnect complete, now reconnecting");
      gnalk_signaling_do_connect (self);
      return;
    }

  /* Emit connection-lost signal if we were previously connected.
   * This allows the ConnectionManager to trigger offline mode recovery. */
  if (was_connected)
    g_signal_emit (self, signals[SIGNAL_CONNECTION_LOST], 0);
}

static void
on_ws_error (SoupWebsocketConnection *ws,
             GError                  *error,
             gpointer                 user_data)
{
  (void) ws;
  (void) user_data;

  /* Use g_message instead of g_warning since broken pipe errors
   * are expected when connection is lost - not a programming error */
  g_message ("WebSocket error: %s", error->message);
}

static char *
get_next_message_id (GnalkSignaling *self)
{
  self->message_id++;
  return g_strdup_printf ("%" G_GUINT64_FORMAT, self->message_id);
}

static void
send_hello (GnalkSignaling *self)
{
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  g_autofree char *msg_id = get_next_message_id (self);

  json_builder_begin_object (builder);

  json_builder_set_member_name (builder, "id");
  json_builder_add_string_value (builder, msg_id);

  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "hello");

  json_builder_set_member_name (builder, "hello");
  json_builder_begin_object (builder);

  json_builder_set_member_name (builder, "version");
  json_builder_add_string_value (builder, "1.0");

  json_builder_set_member_name (builder, "features");
  json_builder_begin_array (builder);
  json_builder_add_string_value (builder, "audio-video-permissions");
  json_builder_add_string_value (builder, "chat-v2");
  json_builder_add_string_value (builder, "dialout");
  json_builder_add_string_value (builder, "federation");
  json_builder_add_string_value (builder, "hello-v2");
  json_builder_add_string_value (builder, "incall-all");
  json_builder_add_string_value (builder, "join-features");
  json_builder_add_string_value (builder, "mcu");
  json_builder_add_string_value (builder, "offer-codecs");
  json_builder_add_string_value (builder, "recipient-call");
  json_builder_add_string_value (builder, "serverinfo");
  json_builder_add_string_value (builder, "simulcast");
  json_builder_add_string_value (builder, "switchto");
  json_builder_add_string_value (builder, "transient-data");
  json_builder_add_string_value (builder, "update-sdp");
  json_builder_add_string_value (builder, "virtual-sessions");
  json_builder_add_string_value (builder, "welcome");
  json_builder_end_array (builder);

  json_builder_set_member_name (builder, "auth");
  json_builder_begin_object (builder);

  if (self->is_federation)
    {
      /* Federation auth: type=federation, url=local Nextcloud, params.token=JWT */
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "federation");

      /* URL is our local Nextcloud server (the one that proxied the federation) */
      g_autofree char *nc_url = NULL;
      if (self->backend_url && self->backend_url[0])
        {
          /* Ensure trailing slash */
          if (g_str_has_suffix (self->backend_url, "/"))
            nc_url = g_strdup (self->backend_url);
          else
            nc_url = g_strdup_printf ("%s/", self->backend_url);
        }
      else
        {
          nc_url = g_strdup ("https://localhost/");
        }

      json_builder_set_member_name (builder, "url");
      json_builder_add_string_value (builder, nc_url);

      json_builder_set_member_name (builder, "params");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "token");
      json_builder_add_string_value (builder, self->fed_auth_token);
      json_builder_end_object (builder);
    }
  else
    {
      /* Normal auth: url=backend auth endpoint, params.userid+ticket */
      g_autofree char *auth_url = NULL;

      /* Use backend_url (the Nextcloud server URL) for auth, not the signaling server URL */
      g_autoptr(GUri) uri = g_uri_parse (self->backend_url ? self->backend_url : self->server_url, G_URI_FLAGS_NONE, NULL);
      if (uri)
        {
          const char *scheme = g_uri_get_scheme (uri);
          const char *host = g_uri_get_host (uri);
          int port = g_uri_get_port (uri);

          const char *http_scheme = "https";
          if (g_strcmp0 (scheme, "http") == 0)
            http_scheme = "http";

          if (port > 0 && port != 80 && port != 443)
            auth_url = g_strdup_printf ("%s://%s:%d/ocs/v2.php/apps/spreed/api/v3/signaling/backend?format=json",
                                         http_scheme, host, port);
          else
            auth_url = g_strdup_printf ("%s://%s/ocs/v2.php/apps/spreed/api/v3/signaling/backend?format=json",
                                         http_scheme, host);
        }
      else
        {
          auth_url = g_strdup ("https://localhost/ocs/v2.php/apps/spreed/api/v3/signaling/backend?format=json");
        }

      json_builder_set_member_name (builder, "url");
      json_builder_add_string_value (builder, auth_url);

      json_builder_set_member_name (builder, "params");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "userid");
      json_builder_add_string_value (builder, self->user_id);
      json_builder_set_member_name (builder, "ticket");
      json_builder_add_string_value (builder, self->ticket);
      json_builder_end_object (builder);
    }

  json_builder_end_object (builder);
  json_builder_end_object (builder);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  g_autoptr(JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, root);
  g_autofree char *json_str = json_generator_to_data (gen, NULL);

  gnalk_log_ws_send (self->user_id, json_str);

  soup_websocket_connection_send_text (self->ws_connection, json_str);
}

static void
on_ws_connected (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  g_autoptr(GError) error = NULL;

  /* Finish the WebSocket connect using the source (SoupSession) directly,
   * because if the operation was cancelled, user_data (self) may already be freed. */
  SoupWebsocketConnection *ws = soup_session_websocket_connect_finish (
    SOUP_SESSION (source), result, &error);

  if (error)
    {
      g_clear_object (&ws);
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to connect WebSocket: %s", error->message);
      return;
    }

  /* If we got here without error, the cancellable was not cancelled,
   * so self is still alive. */
  GnalkSignaling *self = GNALK_SIGNALING (user_data);

  self->ws_connection = ws;

  /* Now safe to release the pending message - handshake is complete */
  g_clear_object (&self->pending_ws_msg);

  self->connected = TRUE;

  /* Store handler IDs so we can properly disconnect before cleanup */
  self->ws_message_handler_id =
    g_signal_connect (self->ws_connection, "message", G_CALLBACK (on_ws_message), self);
  self->ws_closed_handler_id =
    g_signal_connect (self->ws_connection, "closed", G_CALLBACK (on_ws_closed), self);
  self->ws_error_handler_id =
    g_signal_connect (self->ws_connection, "error", G_CALLBACK (on_ws_error), self);

  send_hello (self);
}

static void
gnalk_signaling_do_connect (GnalkSignaling *self)
{
  if (!self->server_url)
    return;

  g_message ("Signaling: connecting to %s", self->server_url);

  /* Clear any previous pending message */
  g_clear_object (&self->pending_ws_msg);

  /* Cancel any previous pending connect and create a fresh cancellable */
  g_cancellable_cancel (self->connect_cancellable);
  g_clear_object (&self->connect_cancellable);
  self->connect_cancellable = g_cancellable_new ();

  self->pending_ws_msg = soup_message_new ("GET", self->server_url);

  soup_session_websocket_connect_async (self->session, self->pending_ws_msg,
                                         NULL, NULL, G_PRIORITY_DEFAULT,
                                         self->connect_cancellable,
                                         on_ws_connected, self);
  /* Don't unref msg here - keep it alive until on_ws_connected finishes */
}

void
gnalk_signaling_connect (GnalkSignaling *self,
                           const char      *server_url,
                           const char      *backend_url,
                           const char      *user_id,
                           const char      *ticket)
{
  g_return_if_fail (GNALK_IS_SIGNALING (self));
  g_return_if_fail (server_url != NULL);

  gboolean was_connected = (self->ws_connection != NULL);

  /* Store new credentials first */
  g_free (self->server_url);
  g_free (self->backend_url);
  g_free (self->user_id);
  g_free (self->ticket);

  /* Store the Nextcloud server URL for backend auth */
  self->backend_url = g_strdup (backend_url);

  g_autofree char *ws_url = g_strdup (server_url);
  if (g_str_has_prefix (ws_url, "https://"))
    {
      self->server_url = g_strdup_printf ("wss://%s", ws_url + 8);
    }
  else if (g_str_has_prefix (ws_url, "http://"))
    {
      self->server_url = g_strdup_printf ("ws://%s", ws_url + 7);
    }
  else
    {
      self->server_url = g_strdup (ws_url);
    }

  if (!g_str_has_suffix (self->server_url, "/spreed"))
    {
      char *old = self->server_url;
      self->server_url = g_strdup_printf ("%s/spreed", old);
      g_free (old);
    }

  self->user_id = g_strdup (user_id);
  self->ticket = g_strdup (ticket);
  self->is_federation = FALSE;
  g_clear_pointer (&self->fed_auth_token, g_free);

  if (was_connected)
    {
      /* Wait for disconnect to complete before reconnecting */
      self->pending_reconnect = TRUE;
      gnalk_signaling_disconnect (self);
    }
  else
    {
      gnalk_signaling_do_connect (self);
    }
}

void
gnalk_signaling_connect_federation (GnalkSignaling *self,
                                    const char      *server_url,
                                    const char      *nextcloud_server_url,
                                    const char      *auth_token)
{
  g_return_if_fail (GNALK_IS_SIGNALING (self));
  g_return_if_fail (server_url != NULL);
  g_return_if_fail (auth_token != NULL);

  gboolean was_connected = (self->ws_connection != NULL);

  g_free (self->server_url);
  g_free (self->backend_url);
  g_free (self->user_id);
  g_free (self->ticket);
  g_free (self->fed_auth_token);

  /* Store the local Nextcloud server URL for auth */
  self->backend_url = g_strdup (nextcloud_server_url);
  self->fed_auth_token = g_strdup (auth_token);
  self->is_federation = TRUE;
  self->user_id = NULL;
  self->ticket = NULL;

  /* Convert URL to WebSocket scheme */
  if (g_str_has_prefix (server_url, "https://"))
    self->server_url = g_strdup_printf ("wss://%s", server_url + 8);
  else if (g_str_has_prefix (server_url, "http://"))
    self->server_url = g_strdup_printf ("ws://%s", server_url + 7);
  else
    self->server_url = g_strdup (server_url);

  if (!g_str_has_suffix (self->server_url, "/spreed"))
    {
      char *old = self->server_url;
      self->server_url = g_strdup_printf ("%s/spreed", old);
      g_free (old);
    }

  g_message ("Federation signaling: connecting to %s (nc=%s)",
             self->server_url, nextcloud_server_url);

  if (was_connected)
    {
      self->pending_reconnect = TRUE;
      gnalk_signaling_disconnect (self);
    }
  else
    {
      gnalk_signaling_do_connect (self);
    }
}

void
gnalk_signaling_disconnect (GnalkSignaling *self)
{
  g_return_if_fail (GNALK_IS_SIGNALING (self));

  /* Cancel any pending async WebSocket connect */
  g_cancellable_cancel (self->connect_cancellable);
  g_clear_object (&self->connect_cancellable);

  if (self->ws_connection)
    {
      /* Disconnect message and error handlers to stop processing new data,
       * but keep the closed handler so it can do proper cleanup when
       * the connection actually closes (soup_websocket_connection_close is async) */
      if (self->ws_message_handler_id > 0)
        {
          g_signal_handler_disconnect (self->ws_connection, self->ws_message_handler_id);
          self->ws_message_handler_id = 0;
        }
      if (self->ws_error_handler_id > 0)
        {
          g_signal_handler_disconnect (self->ws_connection, self->ws_error_handler_id);
          self->ws_error_handler_id = 0;
        }

      /* Close the connection - this is async, the "closed" signal will fire
       * when it's actually closed and on_ws_closed will do final cleanup.
       * Do NOT g_clear_object here - libsoup still needs the object!
       * Only close if still open - calling close twice causes assertion failure */
      SoupWebsocketState state = soup_websocket_connection_get_state (self->ws_connection);
      if (state == SOUP_WEBSOCKET_STATE_OPEN)
        soup_websocket_connection_close (self->ws_connection, 1000, "Disconnect");
    }

  g_clear_pointer (&self->current_room_token, g_free);
  self->connected = FALSE;
}

gboolean
gnalk_signaling_is_connected (GnalkSignaling *self)
{
  g_return_val_if_fail (GNALK_IS_SIGNALING (self), FALSE);
  return self->connected;
}

void
gnalk_signaling_send_message (GnalkSignaling *self,
                                JsonNode        *message)
{
  g_return_if_fail (GNALK_IS_SIGNALING (self));

  if (self->ws_connection == NULL ||
      soup_websocket_connection_get_state (self->ws_connection) != SOUP_WEBSOCKET_STATE_OPEN)
    {
      g_warning ("Cannot send signaling message: WebSocket not connected or not open");
      return;
    }

  g_autofree char *msg_id = get_next_message_id (self);

  g_autoptr(JsonBuilder) wrapper = json_builder_new ();
  json_builder_begin_object (wrapper);
  json_builder_set_member_name (wrapper, "id");
  json_builder_add_string_value (wrapper, msg_id);

  if (JSON_NODE_HOLDS_OBJECT (message))
    {
      JsonObject *msg_obj = json_node_get_object (message);
      GList *members = json_object_get_members (msg_obj);
      for (GList *l = members; l; l = l->next)
        {
          const char *name = l->data;
          JsonNode *value = json_object_get_member (msg_obj, name);
          json_builder_set_member_name (wrapper, name);
          json_builder_add_value (wrapper, json_node_copy (value));
        }
      g_list_free (members);
    }

  json_builder_end_object (wrapper);

  g_autoptr(JsonNode) wrapped = json_builder_get_root (wrapper);
  g_autoptr(JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, wrapped);
  g_autofree char *json_str = json_generator_to_data (gen, NULL);

  gnalk_log_ws_send (self->user_id, json_str);

  soup_websocket_connection_send_text (self->ws_connection, json_str);
}

void
gnalk_signaling_join_room (GnalkSignaling *self,
                             const char      *room_token,
                             const char      *session_id)
{
  g_return_if_fail (GNALK_IS_SIGNALING (self));
  g_return_if_fail (room_token != NULL);

  if (!self->ws_connection ||
      soup_websocket_connection_get_state (self->ws_connection) != SOUP_WEBSOCKET_STATE_OPEN)
    {
      g_message ("signaling_join_room: WebSocket not ready, skipping join for %s", room_token);
      return;
    }

  /* Don't explicitly leave the old room - the signaling server handles room
   * switching automatically when we join a new room. Sending an explicit leave
   * causes a disinvite event and the server may close the connection before
   * we can join the new room. */
  if (self->current_room_token != NULL &&
      g_strcmp0 (self->current_room_token, room_token) != 0)
    {
      g_message ("signaling_join_room: switching from %s to %s",
                 self->current_room_token, room_token);
    }

  g_free (self->session_id);
  self->session_id = g_strdup (session_id);
  g_free (self->current_room_token);
  self->current_room_token = g_strdup (room_token);

  g_autoptr(JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "room");
  json_builder_set_member_name (builder, "room");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "roomid");
  json_builder_add_string_value (builder, room_token);
  if (session_id)
    {
      json_builder_set_member_name (builder, "sessionid");
      json_builder_add_string_value (builder, session_id);
    }
  json_builder_end_object (builder);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  gnalk_signaling_send_message (self, root);
}

void
gnalk_signaling_leave_room (GnalkSignaling *self,
                              const char      *room_token G_GNUC_UNUSED)
{
  g_return_if_fail (GNALK_IS_SIGNALING (self));

  if (!self->ws_connection ||
      soup_websocket_connection_get_state (self->ws_connection) != SOUP_WEBSOCKET_STATE_OPEN)
    return;

  g_clear_pointer (&self->current_room_token, g_free);

  g_autoptr(JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "room");
  json_builder_set_member_name (builder, "room");
  json_builder_begin_object (builder);
  json_builder_end_object (builder);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  gnalk_signaling_send_message (self, root);
}

void
gnalk_signaling_set_message_callback (GnalkSignaling                *self,
                                        GnalkSignalingMessageCallback  callback,
                                        gpointer                        user_data)
{
  g_return_if_fail (GNALK_IS_SIGNALING (self));

  self->message_callback = callback;
  self->message_callback_data = user_data;
}

gboolean
gnalk_signaling_has_mcu (GnalkSignaling *self)
{
  g_return_val_if_fail (GNALK_IS_SIGNALING (self), FALSE);
  return self->has_mcu;
}

const char *
gnalk_signaling_get_session_id (GnalkSignaling *self)
{
  g_return_val_if_fail (GNALK_IS_SIGNALING (self), NULL);
  return self->signaling_session_id;
}

void
gnalk_signaling_send_typing (GnalkSignaling *self,
                               gboolean         typing)
{
  g_return_if_fail (GNALK_IS_SIGNALING (self));

  if (!self->ws_connection || !self->signaling_session_id)
    return;

  const char *typing_type = typing ? "startedTyping" : "stoppedTyping";

  /* Send typing message to all participants in the room */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, self->participants);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *session_id = key;

      /* Don't send to ourselves */
      if (g_strcmp0 (session_id, self->signaling_session_id) == 0)
        continue;

      g_autoptr(JsonBuilder) builder = json_builder_new ();

      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "message");
      json_builder_set_member_name (builder, "message");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "recipient");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "session");
      json_builder_set_member_name (builder, "sessionid");
      json_builder_add_string_value (builder, session_id);
      json_builder_end_object (builder);
      json_builder_set_member_name (builder, "data");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, typing_type);
      json_builder_end_object (builder);
      json_builder_end_object (builder);
      json_builder_end_object (builder);

      g_autoptr(JsonNode) root = json_builder_get_root (builder);
      gnalk_signaling_send_message (self, root);
    }
}

const char *
gnalk_signaling_get_display_name_for_session (GnalkSignaling *self,
                                                const char      *session_id)
{
  g_return_val_if_fail (GNALK_IS_SIGNALING (self), NULL);

  if (!session_id)
    return NULL;

  return g_hash_table_lookup (self->participants, session_id);
}

void
gnalk_signaling_update_incall (GnalkSignaling *self,
                                 gint             flags)
{
  g_return_if_fail (GNALK_IS_SIGNALING (self));

  if (!self->ws_connection)
    return;

  g_message ("Sending incall update via signaling: flags=%d", flags);

  /* Send room message with incall flag to notify MCU.
   * This is required for the MCU to know we're ready to receive streams. */
  g_autoptr(JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "room");
  json_builder_set_member_name (builder, "room");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "incall");
  json_builder_add_int_value (builder, flags);
  json_builder_end_object (builder);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  gnalk_signaling_send_message (self, root);
}

void
gnalk_signaling_send_share_screen (GnalkSignaling *self)
{
  g_return_if_fail (GNALK_IS_SIGNALING (self));

  if (!self->ws_connection || !self->signaling_session_id)
    return;

  g_message ("Sending shareScreen message to all participants");

  /* Send shareScreen message to all participants in the room */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, self->participants);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *session_id = key;

      /* Don't send to ourselves */
      if (g_strcmp0 (session_id, self->signaling_session_id) == 0)
        continue;

      g_autoptr(JsonBuilder) builder = json_builder_new ();

      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "message");
      json_builder_set_member_name (builder, "message");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "recipient");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "session");
      json_builder_set_member_name (builder, "sessionid");
      json_builder_add_string_value (builder, session_id);
      json_builder_end_object (builder);
      json_builder_set_member_name (builder, "data");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "shareScreen");
      json_builder_end_object (builder);
      json_builder_end_object (builder);
      json_builder_end_object (builder);

      g_autoptr(JsonNode) root = json_builder_get_root (builder);
      gnalk_signaling_send_message (self, root);
    }
}

void
gnalk_signaling_send_unshare_screen (GnalkSignaling *self)
{
  g_return_if_fail (GNALK_IS_SIGNALING (self));

  if (!self->ws_connection || !self->signaling_session_id)
    return;

  g_message ("Sending unshareScreen message to all participants");

  /* Send unshareScreen message to all participants in the room */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, self->participants);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *session_id = key;

      /* Don't send to ourselves */
      if (g_strcmp0 (session_id, self->signaling_session_id) == 0)
        continue;

      g_autoptr(JsonBuilder) builder = json_builder_new ();

      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "message");
      json_builder_set_member_name (builder, "message");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "recipient");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "session");
      json_builder_set_member_name (builder, "sessionid");
      json_builder_add_string_value (builder, session_id);
      json_builder_end_object (builder);
      json_builder_set_member_name (builder, "data");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "unshareScreen");
      json_builder_end_object (builder);
      json_builder_end_object (builder);
      json_builder_end_object (builder);

      g_autoptr(JsonNode) root = json_builder_get_root (builder);
      gnalk_signaling_send_message (self, root);
    }
}

void
gnalk_signaling_send_raise_hand (GnalkSignaling *self,
                                  gboolean         raised)
{
  g_return_if_fail (GNALK_IS_SIGNALING (self));

  if (!self->ws_connection || !self->signaling_session_id)
    return;

  gint64 timestamp = g_get_real_time () / G_USEC_PER_SEC;

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, self->participants);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *session_id = key;

      if (g_strcmp0 (session_id, self->signaling_session_id) == 0)
        continue;

      g_autoptr(JsonBuilder) builder = json_builder_new ();

      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "message");
      json_builder_set_member_name (builder, "message");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "recipient");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "session");
      json_builder_set_member_name (builder, "sessionid");
      json_builder_add_string_value (builder, session_id);
      json_builder_end_object (builder);
      json_builder_set_member_name (builder, "data");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "raiseHand");
      json_builder_set_member_name (builder, "payload");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "state");
      json_builder_add_boolean_value (builder, raised);
      json_builder_set_member_name (builder, "timestamp");
      json_builder_add_int_value (builder, timestamp);
      json_builder_end_object (builder);
      json_builder_end_object (builder);
      json_builder_end_object (builder);
      json_builder_end_object (builder);

      g_autoptr(JsonNode) root = json_builder_get_root (builder);
      gnalk_signaling_send_message (self, root);
    }
}

void
gnalk_signaling_send_call_reaction (GnalkSignaling *self,
                                     const char      *emoji)
{
  g_return_if_fail (GNALK_IS_SIGNALING (self));
  g_return_if_fail (emoji != NULL);

  if (!self->ws_connection || !self->signaling_session_id)
    return;

  gint64 timestamp = g_get_real_time () / G_USEC_PER_SEC;

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, self->participants);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *session_id = key;

      if (g_strcmp0 (session_id, self->signaling_session_id) == 0)
        continue;

      g_autoptr(JsonBuilder) builder = json_builder_new ();

      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "message");
      json_builder_set_member_name (builder, "message");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "recipient");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "session");
      json_builder_set_member_name (builder, "sessionid");
      json_builder_add_string_value (builder, session_id);
      json_builder_end_object (builder);
      json_builder_set_member_name (builder, "data");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "reaction");
      json_builder_set_member_name (builder, "payload");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "reaction");
      json_builder_add_string_value (builder, emoji);
      json_builder_set_member_name (builder, "timestamp");
      json_builder_add_int_value (builder, timestamp);
      json_builder_end_object (builder);
      json_builder_end_object (builder);
      json_builder_end_object (builder);
      json_builder_end_object (builder);

      g_autoptr(JsonNode) root = json_builder_get_root (builder);
      gnalk_signaling_send_message (self, root);
    }
}

gint64
gnalk_signaling_get_last_activity_time (GnalkSignaling *self)
{
  g_return_val_if_fail (GNALK_IS_SIGNALING (self), 0);
  return self->last_activity_time;
}

const char *
gnalk_signaling_get_current_room_token (GnalkSignaling *self)
{
  g_return_val_if_fail (GNALK_IS_SIGNALING (self), NULL);
  return self->current_room_token;
}
