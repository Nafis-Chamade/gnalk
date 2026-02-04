#include "gnalk-sync-coordinator.h"
#include "gnalk-message-data.h"
#include "gnalk-conversation-data.h"
#include <json-glib/json-glib.h>

#define SYNC_INTERVAL_SECONDS 20

struct _GnalkSyncCoordinator
{
  GObject parent_instance;

  GnalkDataRepository      *repository;
  GnalkHttpClient          *http_client;
  GnalkNotificationService *notification_service;

  gint64    account_id;
  char     *user_id;
  char     *active_room_token;
  gboolean  window_focused;

  guint     periodic_timer_id;
  gboolean  periodic_enabled;
  gboolean  is_syncing;
};

G_DEFINE_FINAL_TYPE (GnalkSyncCoordinator, gnalk_sync_coordinator, G_TYPE_OBJECT)

enum {
  SIGNAL_CONVERSATIONS_SYNCED,
  SIGNAL_ROOM_MESSAGES_SYNCED,
  SIGNAL_SYNC_STARTED,
  SIGNAL_SYNC_FINISHED,
  SIGNAL_ERROR,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_conversations_fetched (GnalkHttpClient *client,
                                      JsonNode        *response,
                                      GError          *error,
                                      gpointer         user_data);

/* ============ Callback Data Structures ============ */

/* Data for conversation fetching - captures account context at trigger time */
typedef struct {
  GnalkSyncCoordinator *coordinator;
  GnalkHttpClient      *http_client;  /* Borrowed reference */
  gint64                account_id;
  char                 *user_id;
  GnalkSyncTrigger      trigger;
} ConversationFetchData;

static void
conversation_fetch_data_free (ConversationFetchData *data)
{
  g_free (data->user_id);
  g_slice_free (ConversationFetchData, data);
}

/* Data for message fetching */
typedef struct {
  GnalkSyncCoordinator *coordinator;
  GnalkHttpClient      *http_client;  /* Borrowed reference */
  gint64                account_id;
  char                 *user_id;
  char                 *room_token;
  char                 *room_name;    /* For notification display */
  gint64                last_known_id;
} RoomSyncData;

static void
room_sync_data_free (RoomSyncData *data)
{
  g_free (data->user_id);
  g_free (data->room_token);
  g_free (data->room_name);
  g_slice_free (RoomSyncData, data);
}

/* ============ Notification Logic ============ */

static gboolean
should_show_notification (GnalkSyncCoordinator *self,
                          const char           *room_token)
{
  g_debug ("SyncCoordinator: notification decision - window_focused=%d, active_room=%s, msg_room=%s",
           self->window_focused,
           self->active_room_token ? self->active_room_token : "(none)",
           room_token);

  /* Always notify if window not focused (includes minimized) */
  if (!self->window_focused)
    {
      g_debug ("SyncCoordinator: notification YES (window not focused)");
      return TRUE;
    }

  /* Window is focused - only notify if different room */
  if (self->active_room_token == NULL)
    {
      g_debug ("SyncCoordinator: notification YES (no active room)");
      return TRUE;
    }

  gboolean different_room = g_strcmp0 (self->active_room_token, room_token) != 0;
  g_debug ("SyncCoordinator: notification %s (focused on %s room)",
           different_room ? "YES" : "NO",
           different_room ? "different" : "same");
  return different_room;
}

/* ============ Message Sync Callbacks ============ */

static void
on_room_messages_synced (GnalkHttpClient *client G_GNUC_UNUSED,
                         JsonNode        *response,
                         GError          *error,
                         gpointer         user_data)
{
  RoomSyncData *data = user_data;
  GnalkSyncCoordinator *self = data->coordinator;

  if (error)
    {
      g_debug ("[%s] SyncCoordinator: message sync failed for %s: %s",
               data->user_id, data->room_token, error->message);
      room_sync_data_free (data);
      return;
    }

  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    {
      room_sync_data_free (data);
      return;
    }

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    {
      room_sync_data_free (data);
      return;
    }

  JsonArray *messages_array = json_object_get_array_member (ocs, "data");
  if (!messages_array)
    {
      room_sync_data_free (data);
      return;
    }

  guint len = json_array_get_length (messages_array);
  if (len == 0)
    {
      g_debug ("[%s] SyncCoordinator: no new messages for %s", data->user_id, data->room_token);
      room_sync_data_free (data);
      return;
    }

  /* Track info for notification and conversation subtitle */
  gint64 newest_id = 0;
  gint64 newest_timestamp = 0;
  JsonObject *newest_msg_obj = NULL;
  guint saved_count = 0;
  gboolean has_mention = FALSE;
  GString *notification_body = g_string_new (NULL);
  guint notification_msg_count = 0;

  /* Process messages */
  for (guint i = 0; i < len; i++)
    {
      JsonObject *msg_obj = json_array_get_object_element (messages_array, i);
      if (!msg_obj)
        continue;

      GnalkMessageData *msg_data = gnalk_message_data_new_from_json (msg_obj, data->user_id);
      if (!msg_data)
        continue;

      gint64 msg_id = gnalk_message_data_get_id (msg_data);

      /* Only save messages newer than what we already have */
      if (msg_id > data->last_known_id)
        {
          if (msg_id > newest_id)
            {
              newest_id = msg_id;
              newest_timestamp = gnalk_message_data_get_timestamp (msg_data);
              newest_msg_obj = msg_obj;
            }

          /* Save to DB - use captured account_id */
          gnalk_data_repository_save_message (self->repository, data->account_id,
                                              data->room_token, msg_data);
          saved_count++;

          /* Collect messages from others for notification */
          const char *author_id = gnalk_message_data_get_author_id (msg_data);
          if (g_strcmp0 (author_id, data->user_id) != 0)
            {
              const char *sender = json_object_get_string_member_with_default (
                  msg_obj, "actorDisplayName", "Someone");
              const char *text = json_object_get_string_member_with_default (
                  msg_obj, "message", "");

              /* Add to notification body (limit to avoid huge notifications) */
              if (notification_msg_count < 5)
                {
                  if (notification_body->len > 0)
                    g_string_append (notification_body, "\n");
                  g_string_append_printf (notification_body, "%s: %s", sender, text);
                }
              notification_msg_count++;

              /* TODO: Parse messageParameters for mention-user with our ID */
            }
        }

      g_object_unref (msg_data);
    }

  if (saved_count > 0)
    {
      g_message ("[%s] SyncCoordinator: saved %u new messages for %s",
                 data->user_id, saved_count, data->room_token);

      /* Update conversation's last message data for subtitle display */
      if (newest_msg_obj)
        {
          const char *message_text = json_object_get_string_member_with_default (
              newest_msg_obj, "message", "");
          const char *actor_id = json_object_get_string_member_with_default (
              newest_msg_obj, "actorId", "");
          const char *actor_name = json_object_get_string_member_with_default (
              newest_msg_obj, "actorDisplayName", "");
          const char *system_message = json_object_get_string_member_with_default (
              newest_msg_obj, "systemMessage", "");
          const char *message_type = (system_message && *system_message) ? "system" : "comment";

          /* Serialize message parameters if present */
          g_autofree char *params_json = NULL;
          if (json_object_has_member (newest_msg_obj, "messageParameters"))
            {
              JsonNode *params_node = json_object_get_member (newest_msg_obj, "messageParameters");
              if (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
                {
                  JsonGenerator *gen = json_generator_new ();
                  json_generator_set_root (gen, params_node);
                  params_json = json_generator_to_data (gen, NULL);
                  g_object_unref (gen);
                }
            }

          gnalk_data_repository_update_conversation_last_message (
              self->repository,
              data->account_id,
              data->room_token,
              newest_timestamp,
              newest_id,
              message_text,
              actor_id,
              actor_name,
              message_type,
              params_json);
        }

      /* Emit signal so window can update */
      g_signal_emit (self, signals[SIGNAL_ROOM_MESSAGES_SYNCED], 0,
                     data->room_token, saved_count);

      /* Show notification if appropriate (not for initial sync when last_known_id was 0) */
      if (notification_msg_count > 0 &&
          data->last_known_id > 0 &&
          should_show_notification (self, data->room_token))
        {
          const char *display_name = data->room_name ? data->room_name : data->room_token;

          /* Add summary if more messages were truncated */
          if (notification_msg_count > 5)
            g_string_append_printf (notification_body, "\n... and %u more",
                                    notification_msg_count - 5);

          g_message ("[%s] SyncCoordinator: showing notification for room %s: %u messages",
                     data->user_id, data->room_token, notification_msg_count);

          gnalk_notification_service_notify_message (self->notification_service,
                                                     data->room_token,
                                                     display_name,
                                                     NULL,  /* sender_name - included in body */
                                                     notification_body->str,
                                                     NULL,  /* avatar */
                                                     has_mention);
        }
    }

  g_string_free (notification_body, TRUE);

  /* Update last known message ID so we don't re-fetch these */
  if (newest_id > 0)
    gnalk_data_repository_set_last_known_message_id (self->repository, data->account_id,
                                                     data->room_token, newest_id);

  room_sync_data_free (data);
}

/* ============ Conversation Sync ============ */

static void
sync_room_messages (GnalkSyncCoordinator *self,
                    GnalkHttpClient      *http_client,
                    gint64                account_id,
                    const char           *user_id,
                    const char           *room_token,
                    const char           *room_name,
                    gint64                db_last_id)
{
  RoomSyncData *sync_data = g_slice_new0 (RoomSyncData);
  sync_data->coordinator = self;
  sync_data->http_client = http_client;
  sync_data->account_id = account_id;
  sync_data->user_id = g_strdup (user_id);
  sync_data->room_token = g_strdup (room_token);
  sync_data->room_name = g_strdup (room_name);
  sync_data->last_known_id = db_last_id;

  if (db_last_id == 0)
    {
      /* No cached messages - fetch recent messages (last 50) */
      gnalk_http_client_get_messages (http_client,
                                      room_token,
                                      0,
                                      FALSE,  /* lookIntoFuture=FALSE to get recent */
                                      0,
                                      on_room_messages_synced,
                                      sync_data);
    }
  else
    {
      /* Have cached messages - fetch only NEW messages after last known ID */
      gnalk_http_client_get_messages (http_client,
                                      room_token,
                                      db_last_id,
                                      TRUE,   /* lookIntoFuture=TRUE */
                                      0,      /* timeout=0 for immediate */
                                      on_room_messages_synced,
                                      sync_data);
    }
}

static void
on_conversations_fetched (GnalkHttpClient *client G_GNUC_UNUSED,
                          JsonNode        *response,
                          GError          *error,
                          gpointer         user_data)
{
  ConversationFetchData *fetch_data = user_data;
  GnalkSyncCoordinator *self = fetch_data->coordinator;

  self->is_syncing = FALSE;

  if (error)
    {
      g_debug ("SyncCoordinator: failed to fetch conversations: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
      g_signal_emit (self, signals[SIGNAL_SYNC_FINISHED], 0, FALSE);
      conversation_fetch_data_free (fetch_data);
      return;
    }

  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    {
      g_signal_emit (self, signals[SIGNAL_SYNC_FINISHED], 0, FALSE);
      conversation_fetch_data_free (fetch_data);
      return;
    }

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    {
      g_signal_emit (self, signals[SIGNAL_SYNC_FINISHED], 0, FALSE);
      conversation_fetch_data_free (fetch_data);
      return;
    }

  JsonArray *data = json_object_get_array_member (ocs, "data");
  if (!data)
    {
      g_signal_emit (self, signals[SIGNAL_SYNC_FINISHED], 0, FALSE);
      conversation_fetch_data_free (fetch_data);
      return;
    }

  gint64 account_id = fetch_data->account_id;
  GnalkHttpClient *http_client = fetch_data->http_client;

  guint n_rooms = json_array_get_length (data);
  g_message ("[%s] SyncCoordinator: checking %u conversations",
             fetch_data->user_id, n_rooms);

  gboolean has_changes = FALSE;
  GPtrArray *valid_tokens = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *conversations_to_save = g_ptr_array_new_with_free_func (g_object_unref);

  for (guint i = 0; i < n_rooms; i++)
    {
      JsonObject *room = json_array_get_object_element (data, i);
      const char *token = json_object_get_string_member_with_default (room, "token", "");
      const char *display_name = json_object_get_string_member_with_default (room, "displayName", "");

      g_ptr_array_add (valid_tokens, g_strdup (token));

      /* Collect conversation for batch save */
      GnalkConversationData *conv_data = gnalk_conversation_data_new_from_json_with_user (room, fetch_data->user_id);
      if (conv_data)
        {
          g_ptr_array_add (conversations_to_save, conv_data);  /* Takes ownership */
          has_changes = TRUE;
        }

      /* Skip the currently active room if window is focused
       * (MessageController handles it via long-polling) */
      if (self->window_focused &&
          self->active_room_token &&
          g_strcmp0 (token, self->active_room_token) == 0)
        {
          g_debug ("[%s] SyncCoordinator: skipping active room %s", fetch_data->user_id, token);
          continue;
        }

      /* Get server's last message ID from lastMessage object */
      JsonNode *last_message_node = json_object_get_member (room, "lastMessage");
      gint64 server_last_id = 0;
      if (last_message_node && JSON_NODE_HOLDS_OBJECT (last_message_node))
        {
          JsonObject *last_message = json_node_get_object (last_message_node);
          server_last_id = json_object_get_int_member_with_default (last_message, "id", 0);
        }

      if (server_last_id > 0)
        {
          gint64 db_last_id = gnalk_data_repository_get_last_known_message_id (
              self->repository, account_id, token);

          g_debug ("[%s] SyncCoordinator: room %s comparing server=%ld vs db=%ld",
                   fetch_data->user_id, token, server_last_id, db_last_id);

          if (server_last_id > db_last_id)
            {
              g_message ("[%s] SyncCoordinator: room %s has new messages (server: %ld, db: %ld)",
                         fetch_data->user_id, token, server_last_id, db_last_id);

              /* Sync messages for this room */
              sync_room_messages (self, http_client, account_id,
                                  fetch_data->user_id, token, display_name, db_last_id);
            }
        }
    }

  /* Batch save all conversations (emits single data-changed signal) */
  if (conversations_to_save->len > 0)
    {
      g_message ("[%s] SyncCoordinator: batch saving %u conversations", fetch_data->user_id, conversations_to_save->len);
      gnalk_data_repository_save_conversations (self->repository, account_id, conversations_to_save);
    }
  g_ptr_array_unref (conversations_to_save);

  /* Delete conversations no longer on server */
  int deleted = gnalk_data_repository_delete_stale_conversations (self->repository,
                                                                  account_id,
                                                                  valid_tokens);
  if (deleted > 0)
    {
      g_message ("[%s] SyncCoordinator: deleted %d stale conversations", fetch_data->user_id, deleted);
      has_changes = TRUE;
    }

  g_ptr_array_unref (valid_tokens);

  /* Emit conversations-synced signal if data changed */
  if (has_changes)
    g_signal_emit (self, signals[SIGNAL_CONVERSATIONS_SYNCED], 0);

  g_signal_emit (self, signals[SIGNAL_SYNC_FINISHED], 0, TRUE);
  conversation_fetch_data_free (fetch_data);
}

/* ============ Periodic Timer ============ */

static gboolean
on_periodic_timer (gpointer user_data)
{
  GnalkSyncCoordinator *self = GNALK_SYNC_COORDINATOR (user_data);

  g_debug ("SyncCoordinator: periodic sync triggered");
  gnalk_sync_coordinator_trigger (self, GNALK_SYNC_TRIGGER_TIMER);

  return G_SOURCE_CONTINUE;
}

/* ============ GObject Implementation ============ */

static void
gnalk_sync_coordinator_finalize (GObject *object)
{
  GnalkSyncCoordinator *self = GNALK_SYNC_COORDINATOR (object);

  gnalk_sync_coordinator_stop_periodic_sync (self);

  g_free (self->user_id);
  g_free (self->active_room_token);

  G_OBJECT_CLASS (gnalk_sync_coordinator_parent_class)->finalize (object);
}

static void
gnalk_sync_coordinator_class_init (GnalkSyncCoordinatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_sync_coordinator_finalize;

  /**
   * GnalkSyncCoordinator::conversations-synced:
   * @self: The sync coordinator
   *
   * Emitted when conversations have been synced and saved to the database.
   */
  signals[SIGNAL_CONVERSATIONS_SYNCED] =
    g_signal_new ("conversations-synced",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);

  /**
   * GnalkSyncCoordinator::room-messages-synced:
   * @self: The sync coordinator
   * @room_token: The room that received new messages
   * @count: Number of messages synced
   *
   * Emitted when messages for a room have been synced and saved to the database.
   */
  signals[SIGNAL_ROOM_MESSAGES_SYNCED] =
    g_signal_new ("room-messages-synced",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING,   /* room_token */
                  G_TYPE_UINT);    /* count */

  /**
   * GnalkSyncCoordinator::sync-started:
   * @self: The sync coordinator
   * @trigger: What triggered the sync
   *
   * Emitted when a sync operation starts.
   */
  signals[SIGNAL_SYNC_STARTED] =
    g_signal_new ("sync-started",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT);     /* GnalkSyncTrigger */

  /**
   * GnalkSyncCoordinator::sync-finished:
   * @self: The sync coordinator
   * @success: Whether the sync completed successfully
   *
   * Emitted when a sync operation completes.
   */
  signals[SIGNAL_SYNC_FINISHED] =
    g_signal_new ("sync-finished",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_BOOLEAN); /* success */

  /**
   * GnalkSyncCoordinator::error:
   * @self: The sync coordinator
   * @message: Error message
   *
   * Emitted when a sync error occurs.
   */
  signals[SIGNAL_ERROR] =
    g_signal_new ("error",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);  /* message */
}

static void
gnalk_sync_coordinator_init (GnalkSyncCoordinator *self)
{
  self->window_focused = FALSE;  /* Assume not focused in service mode */
  self->periodic_enabled = FALSE;
  self->is_syncing = FALSE;
}

/* ============ Public API ============ */

GnalkSyncCoordinator *
gnalk_sync_coordinator_new (GnalkDataRepository      *repository,
                            GnalkNotificationService *notification_service)
{
  GnalkSyncCoordinator *self = g_object_new (GNALK_TYPE_SYNC_COORDINATOR, NULL);

  self->repository = repository;
  self->notification_service = notification_service;

  return self;
}

void
gnalk_sync_coordinator_set_account (GnalkSyncCoordinator *self,
                                    gint64                account_id,
                                    const char           *user_id)
{
  g_return_if_fail (GNALK_IS_SYNC_COORDINATOR (self));

  self->account_id = account_id;
  g_free (self->user_id);
  self->user_id = g_strdup (user_id);
}

void
gnalk_sync_coordinator_set_http_client (GnalkSyncCoordinator *self,
                                        GnalkHttpClient      *http_client)
{
  g_return_if_fail (GNALK_IS_SYNC_COORDINATOR (self));
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (http_client));

  self->http_client = http_client;
}

void
gnalk_sync_coordinator_trigger (GnalkSyncCoordinator *self,
                                GnalkSyncTrigger      trigger)
{
  g_return_if_fail (GNALK_IS_SYNC_COORDINATOR (self));

  if (self->account_id <= 0)
    {
      g_debug ("SyncCoordinator: no account set, skipping");
      return;
    }

  if (!self->http_client)
    {
      g_debug ("SyncCoordinator: no HTTP client set, skipping");
      return;
    }

  if (self->is_syncing)
    {
      g_debug ("SyncCoordinator: sync already in progress, skipping");
      return;
    }

  self->is_syncing = TRUE;
  g_signal_emit (self, signals[SIGNAL_SYNC_STARTED], 0, (int)trigger);

  /* Capture account context at trigger time */
  ConversationFetchData *fetch_data = g_slice_new0 (ConversationFetchData);
  fetch_data->coordinator = self;
  fetch_data->http_client = self->http_client;
  fetch_data->account_id = self->account_id;
  fetch_data->user_id = g_strdup (self->user_id);
  fetch_data->trigger = trigger;

  g_debug ("SyncCoordinator: trigger=%d, fetching conversations for account %ld",
           trigger, (long)fetch_data->account_id);

  gnalk_http_client_get_conversations (self->http_client,
                                       on_conversations_fetched,
                                       fetch_data);
}

void
gnalk_sync_coordinator_sync_room (GnalkSyncCoordinator *self,
                                  const char           *room_token)
{
  g_return_if_fail (GNALK_IS_SYNC_COORDINATOR (self));
  g_return_if_fail (room_token != NULL);

  if (self->account_id <= 0 || !self->http_client)
    {
      g_debug ("SyncCoordinator: cannot sync room - no account or client");
      return;
    }

  gint64 db_last_id = gnalk_data_repository_get_last_known_message_id (
      self->repository, self->account_id, room_token);

  sync_room_messages (self, self->http_client, self->account_id,
                      self->user_id, room_token, NULL, db_last_id);
}

void
gnalk_sync_coordinator_start_periodic_sync (GnalkSyncCoordinator *self)
{
  g_return_if_fail (GNALK_IS_SYNC_COORDINATOR (self));

  self->periodic_enabled = TRUE;

  if (self->periodic_timer_id > 0)
    return;  /* Already running */

  g_debug ("SyncCoordinator: starting periodic sync (every %ds)", SYNC_INTERVAL_SECONDS);
  self->periodic_timer_id = g_timeout_add_seconds (SYNC_INTERVAL_SECONDS,
                                                   on_periodic_timer, self);
}

void
gnalk_sync_coordinator_stop_periodic_sync (GnalkSyncCoordinator *self)
{
  g_return_if_fail (GNALK_IS_SYNC_COORDINATOR (self));

  self->periodic_enabled = FALSE;

  if (self->periodic_timer_id > 0)
    {
      g_debug ("SyncCoordinator: stopping periodic sync");
      g_source_remove (self->periodic_timer_id);
      self->periodic_timer_id = 0;
    }
}

void
gnalk_sync_coordinator_set_active_room (GnalkSyncCoordinator *self,
                                        const char           *room_token)
{
  g_return_if_fail (GNALK_IS_SYNC_COORDINATOR (self));

  g_free (self->active_room_token);
  self->active_room_token = g_strdup (room_token);
}

void
gnalk_sync_coordinator_set_window_focused (GnalkSyncCoordinator *self,
                                           gboolean              focused)
{
  g_return_if_fail (GNALK_IS_SYNC_COORDINATOR (self));
  self->window_focused = focused;
}

gboolean
gnalk_sync_coordinator_is_syncing (GnalkSyncCoordinator *self)
{
  g_return_val_if_fail (GNALK_IS_SYNC_COORDINATOR (self), FALSE);
  return self->is_syncing;
}
