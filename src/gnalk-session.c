#include "gnalk-session.h"
#include "gnalk-application.h"
#include "gnalk-push-client.h"

struct _GnalkSession
{
  GObject parent_instance;

  /* Account this session is for */
  GnalkAccount *account;
  char *password;

  /* Shared resources (not owned) */
  GnalkDatabase       *database;
  GnalkNetworkMonitor *network_monitor;

  /* Owned components */
  GnalkHttpClient  *http_client;
  GnalkSignaling   *signaling;
  GnalkSyncManager *sync_manager;

  /* Connection state */
  GnalkSessionState state;
  guint reconnect_timer_id;
  int   reconnect_attempts;
  guint health_check_timer_id;
  guint heartbeat_timer_id;

  /* Signaling settings */
  char *signaling_server;
  char *signaling_ticket;

  /* ICE servers */
  char *stun_server;
  char *turn_server;
  char *turn_username;
  char *turn_credential;

  /* Room state */
  char *current_room_token;
  char *current_session_id;
  gboolean signaling_room_confirmed;
  guint room_join_generation;

  /* Conversations */
  GListStore *conversations;
  int total_unread;

  /* Signal handler IDs */
  gulong network_changed_handler_id;
  gulong signaling_ready_handler_id;
  gulong signaling_room_joined_handler_id;
  gulong signaling_connection_lost_handler_id;
  gulong signaling_typing_handler_id;
  gulong signaling_incoming_call_handler_id;
};

G_DEFINE_FINAL_TYPE (GnalkSession, gnalk_session, G_TYPE_OBJECT)

enum {
  SIGNAL_STATE_CHANGED,
  SIGNAL_CONVERSATIONS_UPDATED,
  SIGNAL_ROOM_JOINED,
  SIGNAL_ROOM_LEFT,
  SIGNAL_SIGNALING_ROOM_JOINED,
  SIGNAL_UNREAD_COUNT_CHANGED,
  SIGNAL_CALL_CAPABILITY_CHANGED,
  SIGNAL_MESSAGE_RECEIVED,
  SIGNAL_AUTH_FAILED,
  SIGNAL_SIGNALING_READY,
  SIGNAL_INCOMING_CALL,
  SIGNAL_TYPING,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Helper for readable state names in logging */
static const char *
session_state_to_string (GnalkSessionState state)
{
  switch (state)
    {
    case GNALK_SESSION_STATE_DISCONNECTED:
      return "DISCONNECTED";
    case GNALK_SESSION_STATE_CONNECTING:
      return "CONNECTING";
    case GNALK_SESSION_STATE_ONLINE:
      return "ONLINE";
    case GNALK_SESSION_STATE_OFFLINE:
      return "OFFLINE";
    case GNALK_SESSION_STATE_RECONNECTING:
      return "RECONNECTING";
    default:
      return "UNKNOWN";
    }
}

/* Forward declarations */
static void set_state (GnalkSession *self, GnalkSessionState new_state);
static void schedule_reconnect (GnalkSession *self);
static void attempt_reconnect (GnalkSession *self);
static void start_health_check (GnalkSession *self);
static void stop_health_check (GnalkSession *self);
static void start_heartbeat (GnalkSession *self);
static void stop_heartbeat (GnalkSession *self);
static void connect_signaling (GnalkSession *self);
static void update_call_capability (GnalkSession *self);

/* Auth chain callbacks */
static void on_server_status_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_user_info_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_conversations_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_capabilities_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_signaling_settings_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);

/* Cleanup */
static void
gnalk_session_finalize (GObject *object)
{
  GnalkSession *self = GNALK_SESSION (object);

  if (self->reconnect_timer_id > 0)
    g_source_remove (self->reconnect_timer_id);
  stop_health_check (self);
  stop_heartbeat (self);

  if (self->network_monitor && self->network_changed_handler_id > 0)
    g_signal_handler_disconnect (self->network_monitor, self->network_changed_handler_id);

  g_clear_object (&self->http_client);
  g_clear_object (&self->signaling);
  g_clear_object (&self->sync_manager);
  g_clear_object (&self->conversations);
  g_clear_object (&self->account);
  g_clear_object (&self->database);
  g_clear_object (&self->network_monitor);

  g_free (self->password);
  g_free (self->signaling_server);
  g_free (self->signaling_ticket);
  g_free (self->stun_server);
  g_free (self->turn_server);
  g_free (self->turn_username);
  g_free (self->turn_credential);
  g_free (self->current_room_token);
  g_free (self->current_session_id);

  G_OBJECT_CLASS (gnalk_session_parent_class)->finalize (object);
}

static void
gnalk_session_class_init (GnalkSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gnalk_session_finalize;

  signals[SIGNAL_STATE_CHANGED] =
    g_signal_new ("state-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);

  signals[SIGNAL_CONVERSATIONS_UPDATED] =
    g_signal_new ("conversations-updated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  signals[SIGNAL_ROOM_JOINED] =
    g_signal_new ("room-joined", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIGNAL_ROOM_LEFT] =
    g_signal_new ("room-left", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  signals[SIGNAL_SIGNALING_ROOM_JOINED] =
    g_signal_new ("signaling-room-joined", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_UNREAD_COUNT_CHANGED] =
    g_signal_new ("unread-count-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);

  signals[SIGNAL_CALL_CAPABILITY_CHANGED] =
    g_signal_new ("call-capability-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  signals[SIGNAL_MESSAGE_RECEIVED] =
    g_signal_new ("message-received", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT64);

  signals[SIGNAL_AUTH_FAILED] =
    g_signal_new ("auth-failed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_SIGNALING_READY] =
    g_signal_new ("signaling-ready", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  signals[SIGNAL_INCOMING_CALL] =
    g_signal_new ("incoming-call", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIGNAL_TYPING] =
    g_signal_new ("typing", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
}

static void
gnalk_session_init (GnalkSession *self)
{
  self->state = GNALK_SESSION_STATE_DISCONNECTED;
  self->conversations = g_list_store_new (GNALK_TYPE_CONVERSATION_DATA);
}

/* Network change handler */
static void
on_network_changed (GnalkNetworkMonitor *monitor G_GNUC_UNUSED,
                    gboolean              is_online,
                    GnalkSession        *self)
{
  if (is_online)
    {
      if (self->state == GNALK_SESSION_STATE_OFFLINE)
        {
          g_debug ("Session %s: Network restored, attempting reconnect",
                   gnalk_account_get_user_id (self->account));
          self->reconnect_attempts = 0;
          set_state (self, GNALK_SESSION_STATE_RECONNECTING);
          attempt_reconnect (self);
        }
    }
  else
    {
      if (self->state == GNALK_SESSION_STATE_ONLINE ||
          self->state == GNALK_SESSION_STATE_RECONNECTING)
        {
          g_debug ("Session %s: Network lost, entering offline mode",
                   gnalk_account_get_user_id (self->account));
          set_state (self, GNALK_SESSION_STATE_OFFLINE);
        }
    }
}

/* Signaling handlers */
static void
on_signaling_ready (GnalkSignaling *signaling G_GNUC_UNUSED,
                    GnalkSession   *self)
{
  g_debug ("Session %s: Signaling ready", gnalk_account_get_user_id (self->account));
  g_signal_emit (self, signals[SIGNAL_SIGNALING_READY], 0);

  /* If we're in a room, join it on signaling */
  if (self->current_room_token && self->current_session_id)
    {
      gnalk_signaling_join_room (self->signaling,
                                   self->current_room_token,
                                   self->current_session_id);
    }
}

static void
on_signaling_room_joined (GnalkSignaling *signaling G_GNUC_UNUSED,
                          const char      *room_token,
                          GnalkSession   *self)
{
  g_debug ("Session %s: Signaling confirmed room join: %s",
           gnalk_account_get_user_id (self->account), room_token);

  if (g_strcmp0 (room_token, self->current_room_token) == 0)
    {
      self->signaling_room_confirmed = TRUE;
      g_signal_emit (self, signals[SIGNAL_SIGNALING_ROOM_JOINED], 0, room_token);
      update_call_capability (self);
    }
}

static void
on_signaling_connection_lost (GnalkSignaling *signaling G_GNUC_UNUSED,
                              GnalkSession   *self)
{
  g_debug ("Session %s: Signaling connection lost",
           gnalk_account_get_user_id (self->account));

  self->signaling_room_confirmed = FALSE;
  update_call_capability (self);

  if (self->state == GNALK_SESSION_STATE_ONLINE)
    {
      set_state (self, GNALK_SESSION_STATE_OFFLINE);
      schedule_reconnect (self);
    }
}

static void
on_signaling_typing (GnalkSignaling *signaling G_GNUC_UNUSED,
                     const char      *session_id,
                     const char      *display_name,
                     gboolean         is_typing,
                     GnalkSession   *self)
{
  g_signal_emit (self, signals[SIGNAL_TYPING], 0,
                 self->current_room_token, session_id, display_name, is_typing);
}

static void
on_signaling_incoming_call (GnalkSignaling *signaling G_GNUC_UNUSED,
                            const char      *session_id G_GNUC_UNUSED,
                            const char      *actor_id,
                            gint64           in_call,
                            GnalkSession   *self)
{
  if (in_call > 0 && self->current_room_token)
    {
      g_signal_emit (self, signals[SIGNAL_INCOMING_CALL], 0,
                     self->current_room_token, actor_id);
    }
}

/* Constructor */
GnalkSession *
gnalk_session_new (GnalkAccount        *account,
                     GnalkDatabase       *database,
                     GnalkNetworkMonitor *network_monitor)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT (account), NULL);
  g_return_val_if_fail (GNALK_IS_DATABASE (database), NULL);
  g_return_val_if_fail (GNALK_IS_NETWORK_MONITOR (network_monitor), NULL);

  GnalkSession *self = g_object_new (GNALK_TYPE_SESSION, NULL);

  self->account = g_object_ref (account);
  self->database = g_object_ref (database);
  self->network_monitor = g_object_ref (network_monitor);

  /* Create HTTP client */
  self->http_client = gnalk_http_client_new ();

  /* Create signaling */
  self->signaling = gnalk_signaling_new ();
  self->signaling_ready_handler_id =
    g_signal_connect (self->signaling, "ready",
                      G_CALLBACK (on_signaling_ready), self);
  self->signaling_room_joined_handler_id =
    g_signal_connect (self->signaling, "room-joined",
                      G_CALLBACK (on_signaling_room_joined), self);
  self->signaling_connection_lost_handler_id =
    g_signal_connect (self->signaling, "connection-lost",
                      G_CALLBACK (on_signaling_connection_lost), self);
  self->signaling_typing_handler_id =
    g_signal_connect (self->signaling, "typing",
                      G_CALLBACK (on_signaling_typing), self);
  self->signaling_incoming_call_handler_id =
    g_signal_connect (self->signaling, "incoming-call",
                      G_CALLBACK (on_signaling_incoming_call), self);

  /* Listen for network changes */
  self->network_changed_handler_id =
    g_signal_connect (network_monitor, "network-changed",
                      G_CALLBACK (on_network_changed), self);

  /* Load cached conversations */
  GListStore *cached = gnalk_database_load_conversations (database,
                                                            gnalk_account_get_id (account));
  if (cached)
    {
      guint n = g_list_model_get_n_items (G_LIST_MODEL (cached));
      for (guint i = 0; i < n; i++)
        {
          GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (cached), i);
          gnalk_conversation_data_set_session (conv, self);
          g_list_store_append (self->conversations, conv);
          g_object_unref (conv);
        }
      g_object_unref (cached);
    }

  return self;
}

/* State management */
static void
set_state (GnalkSession *self, GnalkSessionState new_state)
{
  if (self->state == new_state)
    return;

  GnalkSessionState old_state = self->state;
  self->state = new_state;

  g_debug ("Session %s: State %s -> %s",
           gnalk_account_get_user_id (self->account),
           session_state_to_string (old_state),
           session_state_to_string (new_state));

  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0, old_state, new_state);

  /* Start/stop health check and heartbeat based on state */
  if (new_state == GNALK_SESSION_STATE_ONLINE)
    {
      start_health_check (self);
      start_heartbeat (self);
    }
  else
    {
      stop_health_check (self);
      stop_heartbeat (self);
    }

  update_call_capability (self);
}

static void
update_call_capability (GnalkSession *self)
{
  gboolean can_call = (self->state == GNALK_SESSION_STATE_ONLINE &&
                       self->signaling_room_confirmed &&
                       gnalk_signaling_is_connected (self->signaling));

  g_signal_emit (self, signals[SIGNAL_CALL_CAPABILITY_CHANGED], 0, can_call);
}

/* Health check */
static gboolean
health_check_timeout (gpointer user_data)
{
  GnalkSession *self = GNALK_SESSION (user_data);

  if (self->state != GNALK_SESSION_STATE_ONLINE)
    {
      self->health_check_timer_id = 0;
      return G_SOURCE_REMOVE;
    }

  /* Check if signaling connection is actually lost.
   * We only trigger reconnect if the WebSocket is disconnected, not just idle.
   * The WebSocket protocol has its own ping/pong keepalive mechanism (handled by libsoup),
   * and the signaling server doesn't send application-level messages when idle.
   * If the WebSocket dies, on_ws_closed will fire and set connected=FALSE. */
  if (!gnalk_signaling_is_connected (self->signaling))
    {
      g_debug ("Session %s: Signaling not connected, triggering reconnect",
               gnalk_account_get_user_id (self->account));
      set_state (self, GNALK_SESSION_STATE_OFFLINE);
      schedule_reconnect (self);
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

static void
start_health_check (GnalkSession *self)
{
  if (self->health_check_timer_id > 0)
    return;

  self->health_check_timer_id =
    g_timeout_add_seconds (GNALK_SESSION_HEALTH_CHECK_INTERVAL,
                           health_check_timeout, self);
}

static void
stop_health_check (GnalkSession *self)
{
  if (self->health_check_timer_id > 0)
    {
      g_source_remove (self->health_check_timer_id);
      self->health_check_timer_id = 0;
    }
}

/* User status heartbeat - keeps online status active */
#define HEARTBEAT_INTERVAL_SECONDS 60

static void
on_heartbeat_response (GnalkHttpClient *client G_GNUC_UNUSED,
                       JsonNode         *response G_GNUC_UNUSED,
                       GError           *error,
                       gpointer          user_data G_GNUC_UNUSED)
{
  if (error)
    g_debug ("Heartbeat failed: %s", error->message);
}

static gboolean
heartbeat_timeout (gpointer user_data)
{
  GnalkSession *self = GNALK_SESSION (user_data);

  if (self->state != GNALK_SESSION_STATE_ONLINE)
    {
      self->heartbeat_timer_id = 0;
      return G_SOURCE_REMOVE;
    }

  gnalk_http_client_send_heartbeat (self->http_client,
                                     on_heartbeat_response,
                                     NULL);

  return G_SOURCE_CONTINUE;
}

static void
start_heartbeat (GnalkSession *self)
{
  if (self->heartbeat_timer_id > 0)
    return;

  /* Set initial user status to online */
  gnalk_http_client_set_user_status (self->http_client,
                                      "online",
                                      on_heartbeat_response,
                                      NULL);

  /* Start periodic heartbeat */
  self->heartbeat_timer_id =
    g_timeout_add_seconds (HEARTBEAT_INTERVAL_SECONDS,
                           heartbeat_timeout, self);
}

static void
stop_heartbeat (GnalkSession *self)
{
  if (self->heartbeat_timer_id > 0)
    {
      g_source_remove (self->heartbeat_timer_id);
      self->heartbeat_timer_id = 0;
    }
}

/* Reconnection */
static gboolean
reconnect_timeout (gpointer user_data)
{
  GnalkSession *self = GNALK_SESSION (user_data);
  self->reconnect_timer_id = 0;

  if (self->state == GNALK_SESSION_STATE_RECONNECTING ||
      self->state == GNALK_SESSION_STATE_OFFLINE)
    {
      attempt_reconnect (self);
    }

  return G_SOURCE_REMOVE;
}

static void
schedule_reconnect (GnalkSession *self)
{
  if (self->reconnect_timer_id > 0)
    return;

  if (self->reconnect_attempts >= GNALK_SESSION_MAX_RECONNECT_ATTEMPTS)
    {
      g_debug ("Session %s: Max reconnect attempts reached",
               gnalk_account_get_user_id (self->account));
      set_state (self, GNALK_SESSION_STATE_DISCONNECTED);
      return;
    }

  self->reconnect_timer_id =
    g_timeout_add_seconds (GNALK_SESSION_RECONNECT_INTERVAL_SEC,
                           reconnect_timeout, self);
}

static void
attempt_reconnect (GnalkSession *self)
{
  if (!gnalk_network_monitor_is_online (self->network_monitor))
    {
      g_debug ("Session %s: Network offline, deferring reconnect",
               gnalk_account_get_user_id (self->account));
      set_state (self, GNALK_SESSION_STATE_OFFLINE);
      return;
    }

  self->reconnect_attempts++;
  g_debug ("Session %s: Reconnect attempt %d/%d",
           gnalk_account_get_user_id (self->account),
           self->reconnect_attempts, GNALK_SESSION_MAX_RECONNECT_ATTEMPTS);

  set_state (self, GNALK_SESSION_STATE_RECONNECTING);

  /* Start auth chain */
  gnalk_http_client_get_server_status_with_timeout (
    self->http_client,
    GNALK_SESSION_AUTH_TIMEOUT_SEC,
    on_server_status_response,
    self);
}

/* Auth chain */
static void
auth_failed (GnalkSession *self, const char *message)
{
  g_debug ("Session %s: Auth failed: %s",
           gnalk_account_get_user_id (self->account), message);

  /* If network is down, go to offline state and wait for network-changed
   * signal to trigger reconnection. This prevents showing login errors
   * when connection fails due to network being unavailable. */
  if (!gnalk_network_monitor_is_online (self->network_monitor))
    {
      g_debug ("Session %s: Network offline during auth, waiting for reconnect",
               gnalk_account_get_user_id (self->account));
      set_state (self, GNALK_SESSION_STATE_OFFLINE);
      return;
    }

  if (self->state == GNALK_SESSION_STATE_CONNECTING)
    {
      set_state (self, GNALK_SESSION_STATE_DISCONNECTED);
      g_signal_emit (self, signals[SIGNAL_AUTH_FAILED], 0, message);
    }
  else if (self->state == GNALK_SESSION_STATE_RECONNECTING)
    {
      schedule_reconnect (self);
    }
}

static void
on_server_status_response (GnalkHttpClient *client G_GNUC_UNUSED,
                           JsonNode         *response G_GNUC_UNUSED,
                           GError           *error,
                           gpointer          user_data)
{
  GnalkSession *self = GNALK_SESSION (user_data);

  if (error)
    {
      auth_failed (self, error->message);
      return;
    }

  /* Proceed to get user info */
  gnalk_http_client_get_user_info (self->http_client,
                                     on_user_info_response,
                                     self);
}

static void
on_user_info_response (GnalkHttpClient *client G_GNUC_UNUSED,
                       JsonNode         *response,
                       GError           *error,
                       gpointer          user_data)
{
  GnalkSession *self = GNALK_SESSION (user_data);

  if (error)
    {
      auth_failed (self, error->message);
      return;
    }

  /* Extract display name */
  if (response && JSON_NODE_HOLDS_OBJECT (response))
    {
      JsonObject *obj = json_node_get_object (response);
      if (json_object_has_member (obj, "ocs"))
        {
          JsonObject *ocs = json_object_get_object_member (obj, "ocs");
          if (json_object_has_member (ocs, "data"))
            {
              JsonObject *data = json_object_get_object_member (ocs, "data");
              const char *display_name = json_object_get_string_member_with_default (data, "displayname", NULL);
              if (display_name)
                gnalk_account_set_display_name (self->account, display_name);
            }
        }
    }

  /* Proceed to get conversations */
  gnalk_http_client_get_conversations (self->http_client,
                                         on_conversations_response,
                                         self);
}

static void
on_conversations_response (GnalkHttpClient *client G_GNUC_UNUSED,
                           JsonNode         *response,
                           GError           *error,
                           gpointer          user_data)
{
  GnalkSession *self = GNALK_SESSION (user_data);

  if (error)
    {
      auth_failed (self, error->message);
      return;
    }

  /* Parse and store conversations */
  if (response && JSON_NODE_HOLDS_OBJECT (response))
    {
      JsonObject *obj = json_node_get_object (response);
      if (json_object_has_member (obj, "ocs"))
        {
          JsonObject *ocs = json_object_get_object_member (obj, "ocs");
          if (json_object_has_member (ocs, "data"))
            {
              JsonArray *data = json_object_get_array_member (ocs, "data");
              guint len = json_array_get_length (data);

              /* Build lookup of existing conversations by token */
              GHashTable *existing_by_token = g_hash_table_new (g_str_hash, g_str_equal);
              guint n_existing = g_list_model_get_n_items (G_LIST_MODEL (self->conversations));
              for (guint i = 0; i < n_existing; i++)
                {
                  GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (self->conversations), i);
                  if (conv)
                    {
                      const char *token = gnalk_conversation_data_get_token (conv);
                      if (token)
                        g_hash_table_insert (existing_by_token, (gpointer) token, conv);
                      /* Don't unref - hash table holds reference */
                    }
                }

              /* Build set of server tokens for stale detection */
              GHashTable *server_tokens = g_hash_table_new (g_str_hash, g_str_equal);
              GPtrArray *valid_tokens = g_ptr_array_new ();
              self->total_unread = 0;

              /* Update existing or add new conversations */
              for (guint i = 0; i < len; i++)
                {
                  JsonObject *conv_obj = json_array_get_object_element (data, i);
                  const char *token = json_object_get_string_member_with_default (conv_obj, "token", "");

                  if (!token || !*token)
                    continue;

                  g_hash_table_add (server_tokens, (gpointer) token);

                  GnalkConversationData *existing = g_hash_table_lookup (existing_by_token, token);
                  if (existing)
                    {
                      /* Update existing conversation in place */
                      gnalk_conversation_data_update_from_json (existing, conv_obj);
                      self->total_unread += gnalk_conversation_data_get_unread_messages (existing);
                      g_ptr_array_add (valid_tokens, (gpointer) token);
                    }
                  else
                    {
                      /* New conversation - create and append */
                      GnalkConversationData *conv = gnalk_conversation_data_new_from_json (conv_obj, self);
                      if (conv)
                        {
                          g_list_store_append (self->conversations, conv);
                          self->total_unread += gnalk_conversation_data_get_unread_messages (conv);
                          g_ptr_array_add (valid_tokens, (gpointer) gnalk_conversation_data_get_token (conv));
                          g_object_unref (conv);
                        }
                    }

                  /* Save to database (for both new and updated) */
                  GnalkConversationData *to_save = existing ? existing :
                    g_list_model_get_item (G_LIST_MODEL (self->conversations),
                                           g_list_model_get_n_items (G_LIST_MODEL (self->conversations)) - 1);
                  if (to_save)
                    {
                      gnalk_database_save_conversation (self->database,
                                                          gnalk_account_get_id (self->account),
                                                          to_save);
                      if (!existing)
                        g_object_unref (to_save);
                    }
                }

              /* Remove stale conversations (iterate backwards to avoid index shifts) */
              n_existing = g_list_model_get_n_items (G_LIST_MODEL (self->conversations));
              for (gint i = (gint) n_existing - 1; i >= 0; i--)
                {
                  GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (self->conversations), i);
                  if (conv)
                    {
                      const char *token = gnalk_conversation_data_get_token (conv);
                      if (token && !g_hash_table_contains (server_tokens, token))
                        g_list_store_remove (self->conversations, i);
                      g_object_unref (conv);
                    }
                }

              /* Clean up hash table references */
              GHashTableIter iter;
              gpointer value;
              g_hash_table_iter_init (&iter, existing_by_token);
              while (g_hash_table_iter_next (&iter, NULL, &value))
                g_object_unref (value);
              g_hash_table_destroy (existing_by_token);
              g_hash_table_destroy (server_tokens);

              /* Delete stale conversations from database */
              gnalk_database_delete_stale_conversations (self->database,
                                                           gnalk_account_get_id (self->account),
                                                           valid_tokens);
              g_ptr_array_unref (valid_tokens);

              g_signal_emit (self, signals[SIGNAL_CONVERSATIONS_UPDATED], 0);
              g_signal_emit (self, signals[SIGNAL_UNREAD_COUNT_CHANGED], 0, self->total_unread);
            }
        }
    }

  /* Proceed to get capabilities */
  gnalk_http_client_get_capabilities (self->http_client,
                                        on_capabilities_response,
                                        self);
}

static void
on_capabilities_response (GnalkHttpClient *client G_GNUC_UNUSED,
                          JsonNode         *response,
                          GError           *error,
                          gpointer          user_data)
{
  GnalkSession *self = GNALK_SESSION (user_data);

  /* Capabilities are optional - continue even on error */
  if (error)
    {
      g_debug ("Session %s: Capabilities fetch failed (continuing): %s",
               gnalk_account_get_user_id (self->account), error->message);
    }
  else if (response && JSON_NODE_HOLDS_OBJECT (response))
    {
      /* Parse notify_push WebSocket URL from capabilities */
      JsonObject *obj = json_node_get_object (response);
      if (json_object_has_member (obj, "ocs"))
        {
          JsonObject *ocs = json_object_get_object_member (obj, "ocs");
          if (json_object_has_member (ocs, "data"))
            {
              JsonObject *data = json_object_get_object_member (ocs, "data");
              if (json_object_has_member (data, "capabilities"))
                {
                  JsonObject *capabilities = json_object_get_object_member (data, "capabilities");

                  /* Extract notify_push WebSocket URL */
                  if (json_object_has_member (capabilities, "notify_push"))
                    {
                      JsonObject *notify_push = json_object_get_object_member (capabilities, "notify_push");
                      if (notify_push && json_object_has_member (notify_push, "endpoints"))
                        {
                          JsonObject *endpoints = json_object_get_object_member (notify_push, "endpoints");
                          if (endpoints)
                            {
                              const char *websocket_url = json_object_get_string_member_with_default (endpoints, "websocket", NULL);
                              if (websocket_url && *websocket_url)
                                {
                                  GApplication *gapp = g_application_get_default ();
                                  if (GNALK_IS_APPLICATION (gapp))
                                    {
                                      gnalk_application_set_push_websocket_url (GNALK_APPLICATION (gapp), websocket_url);
                                      g_message ("Session %s: Push WebSocket URL: %s",
                                                 gnalk_account_get_user_id (self->account), websocket_url);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

  /* Proceed to get signaling settings */
  gnalk_http_client_get_signaling_settings (self->http_client,
                                              on_signaling_settings_response,
                                              self);
}

static void
on_signaling_settings_response (GnalkHttpClient *client G_GNUC_UNUSED,
                                JsonNode         *response,
                                GError           *error,
                                gpointer          user_data)
{
  GnalkSession *self = GNALK_SESSION (user_data);

  if (error)
    {
      auth_failed (self, error->message);
      return;
    }

  /* Parse signaling settings */
  if (response && JSON_NODE_HOLDS_OBJECT (response))
    {
      JsonObject *obj = json_node_get_object (response);
      if (json_object_has_member (obj, "ocs"))
        {
          JsonObject *ocs = json_object_get_object_member (obj, "ocs");
          if (json_object_has_member (ocs, "data"))
            {
              JsonObject *data = json_object_get_object_member (ocs, "data");

              /* Signaling server and ticket */
              const char *server = json_object_get_string_member_with_default (data, "server", NULL);
              const char *ticket = json_object_get_string_member_with_default (data, "ticket", NULL);

              if (server && ticket)
                {
                  g_free (self->signaling_server);
                  g_free (self->signaling_ticket);
                  self->signaling_server = g_strdup (server);
                  self->signaling_ticket = g_strdup (ticket);
                }

              /* ICE servers */
              if (json_object_has_member (data, "stunservers"))
                {
                  JsonArray *stun = json_object_get_array_member (data, "stunservers");
                  if (json_array_get_length (stun) > 0)
                    {
                      JsonObject *s = json_array_get_object_element (stun, 0);
                      const char *url = NULL;

                      if (json_object_has_member (s, "urls"))
                        {
                          JsonNode *urls_node = json_object_get_member (s, "urls");
                          if (JSON_NODE_HOLDS_ARRAY (urls_node))
                            {
                              JsonArray *urls = json_node_get_array (urls_node);
                              if (json_array_get_length (urls) > 0)
                                url = json_array_get_string_element (urls, 0);
                            }
                          else if (JSON_NODE_HOLDS_VALUE (urls_node))
                            {
                              url = json_node_get_string (urls_node);
                            }
                        }
                      if (!url)
                        url = json_object_get_string_member_with_default (s, "url", NULL);

                      if (url)
                        {
                          g_free (self->stun_server);
                          self->stun_server = g_strdup (url);
                        }
                    }
                }

              if (json_object_has_member (data, "turnservers"))
                {
                  JsonArray *turn = json_object_get_array_member (data, "turnservers");
                  if (json_array_get_length (turn) > 0)
                    {
                      JsonObject *t = json_array_get_object_element (turn, 0);
                      const char *url = NULL;

                      if (json_object_has_member (t, "urls"))
                        {
                          JsonNode *urls_node = json_object_get_member (t, "urls");
                          if (JSON_NODE_HOLDS_ARRAY (urls_node))
                            {
                              JsonArray *urls = json_node_get_array (urls_node);
                              if (json_array_get_length (urls) > 0)
                                url = json_array_get_string_element (urls, 0);
                            }
                          else if (JSON_NODE_HOLDS_VALUE (urls_node))
                            {
                              url = json_node_get_string (urls_node);
                            }
                        }
                      if (!url)
                        url = json_object_get_string_member_with_default (t, "url", NULL);

                      if (url)
                        {
                          g_free (self->turn_server);
                          self->turn_server = g_strdup (url);
                        }

                      const char *username = json_object_get_string_member_with_default (t, "username", NULL);
                      const char *credential = json_object_get_string_member_with_default (t, "credential", NULL);

                      if (username)
                        {
                          g_free (self->turn_username);
                          self->turn_username = g_strdup (username);
                        }
                      if (credential)
                        {
                          g_free (self->turn_credential);
                          self->turn_credential = g_strdup (credential);
                        }
                    }
                }
            }
        }
    }

  /* Auth complete - connect signaling and go online */
  self->reconnect_attempts = 0;
  set_state (self, GNALK_SESSION_STATE_ONLINE);

  /* Connect push client at application level (works in service mode without window) */
  {
    GApplication *gapp = g_application_get_default ();
    if (GNALK_IS_APPLICATION (gapp))
      {
        GnalkApplication *app = GNALK_APPLICATION (gapp);
        GnalkPushClient *push_client = gnalk_application_get_push_client (app);
        const char *push_url = gnalk_application_get_push_websocket_url (app);
        const char *user_id = gnalk_account_get_user_id (self->account);

        if (push_client && push_url && *push_url && user_id && self->password &&
            !gnalk_push_client_is_connected (push_client))
          {
            g_message ("Session %s: Connecting push client to %s", user_id, push_url);
            gnalk_push_client_connect (push_client, push_url, user_id, self->password);

            /* Also set app credentials for background sync */
            gnalk_application_set_credentials (app,
                                                gnalk_account_get_server_url (self->account),
                                                user_id,
                                                self->password);
          }
      }
  }

  /* Create sync manager if not exists */
  if (!self->sync_manager)
    {
      self->sync_manager = gnalk_sync_manager_new (self->http_client,
                                                     self->database,
                                                     self->network_monitor,
                                                     gnalk_account_get_id (self->account));
    }

  /* Flush pending messages */
  gnalk_sync_manager_flush_pending_queue (self->sync_manager);

  /* Connect signaling */
  connect_signaling (self);
}

static void
connect_signaling (GnalkSession *self)
{
  if (!self->signaling_server || !self->signaling_ticket)
    {
      g_warning ("Session %s: No signaling settings",
                 gnalk_account_get_user_id (self->account));
      return;
    }

  gnalk_signaling_connect (self->signaling,
                             self->signaling_server,
                             gnalk_account_get_user_id (self->account),
                             self->signaling_ticket);
}

/* Public API */

GnalkSessionState
gnalk_session_get_state (GnalkSession *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), GNALK_SESSION_STATE_DISCONNECTED);
  return self->state;
}

gboolean
gnalk_session_is_online (GnalkSession *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), FALSE);
  return self->state == GNALK_SESSION_STATE_ONLINE;
}

gboolean
gnalk_session_can_call (GnalkSession *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), FALSE);
  gboolean online = (self->state == GNALK_SESSION_STATE_ONLINE);
  gboolean sig_connected = gnalk_signaling_is_connected (self->signaling);
  gboolean result = online && self->signaling_room_confirmed && sig_connected;
  g_message ("session_can_call: online=%d room_confirmed=%d sig_connected=%d => %d",
             online, self->signaling_room_confirmed, sig_connected, result);
  return result;
}

GnalkAccount *
gnalk_session_get_account (GnalkSession *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), NULL);
  return self->account;
}

gint64
gnalk_session_get_account_id (GnalkSession *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), 0);
  return gnalk_account_get_id (self->account);
}

void
gnalk_session_connect (GnalkSession *self,
                         const char    *password)
{
  g_return_if_fail (GNALK_IS_SESSION (self));

  if (self->state != GNALK_SESSION_STATE_DISCONNECTED)
    {
      g_debug ("Session %s: Already connected or connecting",
               gnalk_account_get_user_id (self->account));
      return;
    }

  /* Store password */
  g_free (self->password);
  self->password = g_strdup (password);

  /* Configure HTTP client */
  gnalk_http_client_set_credentials (self->http_client,
                                       gnalk_account_get_server_url (self->account),
                                       gnalk_account_get_user_id (self->account),
                                       password);

  /* Check network status before attempting connection.
   * If offline, go to OFFLINE state and wait for network-changed signal
   * to trigger reconnection. This prevents showing login errors when
   * autologin fails due to network being unavailable. */
  if (!gnalk_network_monitor_is_online (self->network_monitor))
    {
      g_debug ("Session %s: Network offline, deferring connection",
               gnalk_account_get_user_id (self->account));
      set_state (self, GNALK_SESSION_STATE_OFFLINE);
      return;
    }

  set_state (self, GNALK_SESSION_STATE_CONNECTING);

  /* Start auth chain */
  gnalk_http_client_get_server_status (self->http_client,
                                         on_server_status_response,
                                         self);
}

void
gnalk_session_disconnect (GnalkSession *self)
{
  g_return_if_fail (GNALK_IS_SESSION (self));

  /* Cancel pending timers */
  if (self->reconnect_timer_id > 0)
    {
      g_source_remove (self->reconnect_timer_id);
      self->reconnect_timer_id = 0;
    }
  stop_health_check (self);
  stop_heartbeat (self);

  /* Disconnect signaling */
  gnalk_signaling_disconnect (self->signaling);

  /* Cancel pending HTTP requests */
  gnalk_http_client_cancel_pending (self->http_client);

  /* Clear room state */
  g_clear_pointer (&self->current_room_token, g_free);
  g_clear_pointer (&self->current_session_id, g_free);
  self->signaling_room_confirmed = FALSE;

  set_state (self, GNALK_SESSION_STATE_DISCONNECTED);
}

/* Room join callback */
static void
on_room_join_response (GnalkHttpClient *client G_GNUC_UNUSED,
                       JsonNode         *response,
                       GError           *error,
                       gpointer          user_data)
{
  GnalkSession *self = GNALK_SESSION (user_data);

  if (error)
    {
      g_warning ("Session %s: Room join failed: %s",
                 gnalk_account_get_user_id (self->account), error->message);
      return;
    }

  /* Extract session ID */
  if (response && JSON_NODE_HOLDS_OBJECT (response))
    {
      JsonObject *obj = json_node_get_object (response);
      if (json_object_has_member (obj, "ocs"))
        {
          JsonObject *ocs = json_object_get_object_member (obj, "ocs");
          if (json_object_has_member (ocs, "data"))
            {
              JsonObject *data = json_object_get_object_member (ocs, "data");
              const char *session_id = json_object_get_string_member_with_default (data, "sessionId", NULL);

              if (session_id)
                {
                  g_free (self->current_session_id);
                  self->current_session_id = g_strdup (session_id);

                  g_signal_emit (self, signals[SIGNAL_ROOM_JOINED], 0,
                                 self->current_room_token, self->current_session_id);

                  /* Join room on signaling if connected */
                  gboolean sig_connected = gnalk_signaling_is_connected (self->signaling);
                  g_message ("Session %s: room join response, signaling connected=%d",
                             gnalk_account_get_user_id (self->account), sig_connected);
                  if (sig_connected)
                    {
                      gnalk_signaling_join_room (self->signaling,
                                                   self->current_room_token,
                                                   self->current_session_id);
                    }
                  else
                    {
                      g_warning ("Session %s: signaling not connected, cannot join room on signaling",
                                 gnalk_account_get_user_id (self->account));
                    }
                }
            }
        }
    }
}

void
gnalk_session_join_room (GnalkSession *self,
                           const char    *room_token)
{
  g_return_if_fail (GNALK_IS_SESSION (self));
  g_return_if_fail (room_token != NULL);

  /* If already in the same room, don't leave and re-join */
  if (g_strcmp0 (self->current_room_token, room_token) == 0)
    {
      g_debug ("Session %s: Already in room %s, skipping re-join",
               gnalk_account_get_user_id (self->account), room_token);

      /* If signaling not confirmed yet but we have a session_id, try to join signaling */
      if (!self->signaling_room_confirmed && self->current_session_id &&
          self->state == GNALK_SESSION_STATE_ONLINE &&
          gnalk_signaling_is_connected (self->signaling))
        {
          g_message ("Session %s: Re-joining signaling for room %s",
                     gnalk_account_get_user_id (self->account), room_token);
          gnalk_signaling_join_room (self->signaling, room_token, self->current_session_id);
        }
      return;
    }

  /* Leave current room first */
  if (self->current_room_token)
    gnalk_session_leave_room (self);

  self->room_join_generation++;
  g_free (self->current_room_token);
  self->current_room_token = g_strdup (room_token);
  g_clear_pointer (&self->current_session_id, g_free);  /* Clear old session_id */
  self->signaling_room_confirmed = FALSE;

  if (self->state != GNALK_SESSION_STATE_ONLINE)
    {
      g_debug ("Session %s: Deferring room join (not online)",
               gnalk_account_get_user_id (self->account));
      return;
    }

  gnalk_http_client_join_room (self->http_client,
                                 room_token,
                                 on_room_join_response,
                                 self);
}

void
gnalk_session_leave_room (GnalkSession *self)
{
  g_return_if_fail (GNALK_IS_SESSION (self));

  if (!self->current_room_token)
    return;

  /* Leave on signaling */
  if (gnalk_signaling_is_connected (self->signaling))
    gnalk_signaling_leave_room (self->signaling, self->current_room_token);

  /* Leave via HTTP */
  if (self->state == GNALK_SESSION_STATE_ONLINE)
    gnalk_http_client_leave_room (self->http_client, self->current_room_token, NULL, NULL);

  g_clear_pointer (&self->current_room_token, g_free);
  g_clear_pointer (&self->current_session_id, g_free);
  self->signaling_room_confirmed = FALSE;

  g_signal_emit (self, signals[SIGNAL_ROOM_LEFT], 0);
  update_call_capability (self);
}

const char *
gnalk_session_get_current_room_token (GnalkSession *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), NULL);
  return self->current_room_token;
}

const char *
gnalk_session_get_current_session_id (GnalkSession *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), NULL);
  return self->current_session_id;
}

GListStore *
gnalk_session_get_conversations (GnalkSession *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), NULL);
  return self->conversations;
}

void
gnalk_session_refresh_conversations (GnalkSession *self)
{
  g_return_if_fail (GNALK_IS_SESSION (self));

  if (self->state != GNALK_SESSION_STATE_ONLINE)
    return;

  gnalk_http_client_get_conversations (self->http_client,
                                         on_conversations_response,
                                         self);
}

int
gnalk_session_get_total_unread (GnalkSession *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), 0);
  return self->total_unread;
}

void
gnalk_session_send_message (GnalkSession *self,
                              const char    *room_token,
                              const char    *message,
                              gint64         reply_to,
                              const char    *reference_id)
{
  g_return_if_fail (GNALK_IS_SESSION (self));

  if (self->state == GNALK_SESSION_STATE_ONLINE)
    {
      gnalk_http_client_send_message (self->http_client,
                                        room_token, message, reply_to, reference_id,
                                        NULL, NULL);
    }
  else if (self->sync_manager)
    {
      gnalk_sync_manager_queue_message (self->sync_manager,
                                          room_token, message, reply_to, reference_id);
    }
}

void
gnalk_session_send_file (GnalkSession *self,
                           const char    *room_token,
                           const char    *file_path,
                           const char    *file_name,
                           const char    *content_type,
                           const char    *caption,
                           gint64         reply_to,
                           const char    *reference_id)
{
  g_return_if_fail (GNALK_IS_SESSION (self));

  if (self->sync_manager)
    {
      gnalk_sync_manager_queue_file (self->sync_manager,
                                       room_token, file_path, file_name,
                                       content_type, caption, reply_to, reference_id);
    }
}

GnalkHttpClient *
gnalk_session_get_http_client (GnalkSession *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), NULL);
  return self->http_client;
}

GnalkSignaling *
gnalk_session_get_signaling (GnalkSession *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), NULL);
  return self->signaling;
}

GnalkSyncManager *
gnalk_session_get_sync_manager (GnalkSession *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), NULL);
  return self->sync_manager;
}

void
gnalk_session_set_signaling_settings (GnalkSession *self,
                                         const char    *server,
                                         const char    *ticket)
{
  g_return_if_fail (GNALK_IS_SESSION (self));

  g_free (self->signaling_server);
  g_free (self->signaling_ticket);
  self->signaling_server = g_strdup (server);
  self->signaling_ticket = g_strdup (ticket);
}

void
gnalk_session_set_ice_servers (GnalkSession *self,
                                 const char    *stun_server,
                                 const char    *turn_server,
                                 const char    *turn_username,
                                 const char    *turn_credential)
{
  g_return_if_fail (GNALK_IS_SESSION (self));

  g_free (self->stun_server);
  g_free (self->turn_server);
  g_free (self->turn_username);
  g_free (self->turn_credential);

  self->stun_server = g_strdup (stun_server);
  self->turn_server = g_strdup (turn_server);
  self->turn_username = g_strdup (turn_username);
  self->turn_credential = g_strdup (turn_credential);
}

const char *
gnalk_session_get_stun_server (GnalkSession *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), NULL);
  return self->stun_server;
}

const char *
gnalk_session_get_turn_server (GnalkSession *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), NULL);
  return self->turn_server;
}

const char *
gnalk_session_get_turn_username (GnalkSession *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), NULL);
  return self->turn_username;
}

const char *
gnalk_session_get_turn_credential (GnalkSession *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), NULL);
  return self->turn_credential;
}

void
gnalk_session_attempt_reconnect (GnalkSession *self)
{
  g_return_if_fail (GNALK_IS_SESSION (self));

  if (self->state == GNALK_SESSION_STATE_DISCONNECTED ||
      self->state == GNALK_SESSION_STATE_OFFLINE)
    {
      self->reconnect_attempts = 0;
      set_state (self, GNALK_SESSION_STATE_RECONNECTING);
      attempt_reconnect (self);
    }
}

gint64
gnalk_session_get_last_known_message_id (GnalkSession *self,
                                            const char    *room_token)
{
  g_return_val_if_fail (GNALK_IS_SESSION (self), 0);
  return gnalk_database_get_last_known_message_id (self->database,
                                                     gnalk_account_get_id (self->account),
                                                     room_token);
}

void
gnalk_session_set_last_known_message_id (GnalkSession *self,
                                            const char    *room_token,
                                            gint64         message_id)
{
  g_return_if_fail (GNALK_IS_SESSION (self));
  gnalk_database_set_last_known_message_id (self->database,
                                              gnalk_account_get_id (self->account),
                                              room_token, message_id);
}
