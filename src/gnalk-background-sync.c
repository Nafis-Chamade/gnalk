#include "gnalk-background-sync.h"
#include "gnalk-message-data.h"
#include "gnalk-conversation-data.h"
#include <json-glib/json-glib.h>

struct _GnalkBackgroundSync
{
  GObject parent_instance;

  GnalkDatabase            *database;
  GnalkHttpClient          *http_client;
  GnalkNotificationService *notification_service;

  gint64  account_id;
  char   *user_id;
  char   *active_room_token;
  gboolean window_focused;
};

G_DEFINE_FINAL_TYPE (GnalkBackgroundSync, gnalk_background_sync, G_TYPE_OBJECT)

enum {
  SIGNAL_MESSAGES_SYNCED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_conversations_fetched (GnalkHttpClient *client,
                                       JsonNode         *response,
                                       GError           *error,
                                       gpointer          user_data);

/* Background sync data for message fetching */
typedef struct {
  GnalkBackgroundSync *sync;       /* Weak reference would be better but keep simple for now */
  char                 *room_token;
  char                 *room_name;  /* For notification display */
  gint64                last_known_id;
} BackgroundSyncData;

static void
background_sync_data_free (BackgroundSyncData *data)
{
  g_free (data->room_token);
  g_free (data->room_name);
  g_slice_free (BackgroundSyncData, data);
}

static gboolean
should_show_notification (GnalkBackgroundSync *self,
                          const char           *room_token)
{
  g_debug ("Notification decision: window_focused=%d, active_room=%s, msg_room=%s",
           self->window_focused,
           self->active_room_token ? self->active_room_token : "(none)",
           room_token);

  /* Always notify if window not focused (includes minimized) */
  if (!self->window_focused)
    {
      g_debug ("Notification: YES (window not focused)");
      return TRUE;
    }

  /* Window is focused - only notify if different room */
  if (self->active_room_token == NULL)
    {
      g_debug ("Notification: YES (no active room)");
      return TRUE;
    }

  gboolean different_room = g_strcmp0 (self->active_room_token, room_token) != 0;
  g_debug ("Notification: %s (focused on %s room)",
           different_room ? "YES" : "NO",
           different_room ? "different" : "same");
  return different_room;
}

static void
on_background_messages_synced (GnalkHttpClient *client G_GNUC_UNUSED,
                                JsonNode         *response,
                                GError           *error,
                                gpointer          user_data)
{
  BackgroundSyncData *data = user_data;
  GnalkBackgroundSync *self = data->sync;

  if (error)
    {
      g_debug ("Background sync failed for %s: %s", data->room_token, error->message);
      background_sync_data_free (data);
      return;
    }

  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    {
      background_sync_data_free (data);
      return;
    }

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    {
      background_sync_data_free (data);
      return;
    }

  JsonArray *messages_array = json_object_get_array_member (ocs, "data");
  if (!messages_array)
    {
      background_sync_data_free (data);
      return;
    }

  guint len = json_array_get_length (messages_array);
  if (len == 0)
    {
      g_debug ("Background sync: no new messages for %s", data->room_token);
      background_sync_data_free (data);
      return;
    }

  /* Track info for notification */
  gint64 newest_id = 0;
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

      GnalkMessageData *msg_data = gnalk_message_data_new_from_json (msg_obj, self->user_id);
      if (!msg_data)
        continue;

      gint64 msg_id = gnalk_message_data_get_id (msg_data);

      /* Only save messages newer than what we already have */
      if (msg_id > data->last_known_id)
        {
          if (msg_id > newest_id)
            newest_id = msg_id;

          /* Save to DB */
          gnalk_database_save_message (self->database, self->account_id,
                                         data->room_token, msg_data);
          saved_count++;

          /* Collect messages from others for notification */
          const char *author_id = gnalk_message_data_get_author_id (msg_data);
          if (g_strcmp0 (author_id, self->user_id) != 0)
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
      g_debug ("Background sync: saved %u new messages for %s",
               saved_count, data->room_token);

      /* Emit signal so window can update */
      g_signal_emit (self, signals[SIGNAL_MESSAGES_SYNCED], 0, data->room_token);

      /* Show notification if appropriate (not for initial sync when last_known_id was 0) */
      if (notification_msg_count > 0 &&
          data->last_known_id > 0 &&
          should_show_notification (self, data->room_token))
        {
          /* Use room name if we have it, otherwise use room token */
          const char *display_name = data->room_name ? data->room_name : data->room_token;

          /* Add summary if more messages were truncated */
          if (notification_msg_count > 5)
            g_string_append_printf (notification_body, "\n... and %u more",
                                    notification_msg_count - 5);

          g_message ("Showing notification for room %s: %u messages",
                     data->room_token, notification_msg_count);

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
    gnalk_database_set_last_known_message_id (self->database, self->account_id,
                                                 data->room_token, newest_id);

  background_sync_data_free (data);
}

static void
on_conversations_fetched (GnalkHttpClient *client G_GNUC_UNUSED,
                           JsonNode         *response,
                           GError           *error,
                           gpointer          user_data)
{
  GnalkBackgroundSync *self = GNALK_BACKGROUND_SYNC (user_data);

  if (error)
    {
      g_debug ("Background sync: failed to fetch conversations: %s", error->message);
      return;
    }

  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    return;

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    return;

  JsonArray *data = json_object_get_array_member (ocs, "data");
  if (!data)
    return;

  guint n_rooms = json_array_get_length (data);
  g_debug ("Background sync: checking %u conversations", n_rooms);

  for (guint i = 0; i < n_rooms; i++)
    {
      JsonObject *room = json_array_get_object_element (data, i);
      const char *token = json_object_get_string_member_with_default (room, "token", "");
      const char *display_name = json_object_get_string_member_with_default (room, "displayName", "");

      /* Skip the currently active room if window is focused */
      if (self->window_focused &&
          self->active_room_token &&
          g_strcmp0 (token, self->active_room_token) == 0)
        continue;

      /* Get server's last message ID from lastMessage object */
      JsonNode *last_message_node = json_object_get_member (room, "lastMessage");
      gint64 server_last_id = 0;
      if (last_message_node && JSON_NODE_HOLDS_OBJECT (last_message_node))
        {
          JsonObject *last_message = json_node_get_object (last_message_node);
          server_last_id = json_object_get_int_member_with_default (last_message, "id", 0);
        }
      else
        {
          g_debug ("Background sync: room %s has no lastMessage object", token);
        }

      if (server_last_id > 0)
        {
          gint64 db_last_id = gnalk_database_get_last_known_message_id (
              self->database, self->account_id, token);

          g_debug ("Background sync: room %s comparing server=%ld vs db=%ld",
                   token, server_last_id, db_last_id);

          if (server_last_id > db_last_id)
            {
              g_debug ("Background sync: room %s has new messages (server: %ld, db: %ld)",
                       token, server_last_id, db_last_id);

              /* Save updated conversation to DB (with new unreadMessages, lastMessage, etc.)
               * so the sidebar can show the changes immediately */
              GnalkConversationData *conv_data = gnalk_conversation_data_new_from_json (room, NULL);
              if (conv_data)
                {
                  gnalk_database_save_conversation (self->database, self->account_id, conv_data);
                  g_object_unref (conv_data);
                }

              BackgroundSyncData *sync_data = g_slice_new0 (BackgroundSyncData);
              sync_data->sync = self;
              sync_data->room_token = g_strdup (token);
              sync_data->room_name = g_strdup (display_name);
              sync_data->last_known_id = db_last_id;

              if (db_last_id == 0)
                {
                  /* No cached messages - fetch recent messages (last 50) */
                  gnalk_http_client_get_messages (self->http_client,
                                                    token,
                                                    0,
                                                    FALSE,  /* lookIntoFuture=FALSE to get recent */
                                                    0,
                                                    on_background_messages_synced,
                                                    sync_data);
                }
              else
                {
                  /* Have cached messages - fetch only NEW messages after last known ID */
                  gnalk_http_client_get_messages (self->http_client,
                                                    token,
                                                    db_last_id,
                                                    TRUE,   /* lookIntoFuture=TRUE */
                                                    0,      /* timeout=0 for immediate */
                                                    on_background_messages_synced,
                                                    sync_data);
                }
            }
        }
    }
}

static void
gnalk_background_sync_finalize (GObject *object)
{
  GnalkBackgroundSync *self = GNALK_BACKGROUND_SYNC (object);

  g_free (self->user_id);
  g_free (self->active_room_token);

  G_OBJECT_CLASS (gnalk_background_sync_parent_class)->finalize (object);
}

static void
gnalk_background_sync_class_init (GnalkBackgroundSyncClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_background_sync_finalize;

  /**
   * GnalkBackgroundSync::messages-synced:
   * @self: The background sync service
   * @room_token: The room that received new messages
   *
   * Emitted when new messages have been synced and saved to the database.
   * The window can connect to this to refresh its message view.
   */
  signals[SIGNAL_MESSAGES_SYNCED] =
    g_signal_new ("messages-synced",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);
}

static void
gnalk_background_sync_init (GnalkBackgroundSync *self)
{
  self->window_focused = FALSE;  /* Assume not focused in service mode */
}

GnalkBackgroundSync *
gnalk_background_sync_new (GnalkDatabase            *database,
                             GnalkHttpClient          *http_client,
                             GnalkNotificationService *notification_service)
{
  GnalkBackgroundSync *self = g_object_new (GNALK_TYPE_BACKGROUND_SYNC, NULL);

  self->database = database;
  self->http_client = http_client;
  self->notification_service = notification_service;

  return self;
}

void
gnalk_background_sync_set_account (GnalkBackgroundSync *self,
                                     gint64                account_id,
                                     const char           *user_id)
{
  g_return_if_fail (GNALK_IS_BACKGROUND_SYNC (self));

  self->account_id = account_id;
  g_free (self->user_id);
  self->user_id = g_strdup (user_id);
}

void
gnalk_background_sync_set_http_client (GnalkBackgroundSync *self,
                                         GnalkHttpClient      *http_client)
{
  g_return_if_fail (GNALK_IS_BACKGROUND_SYNC (self));
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (http_client));

  self->http_client = http_client;
}

void
gnalk_background_sync_trigger (GnalkBackgroundSync *self)
{
  g_return_if_fail (GNALK_IS_BACKGROUND_SYNC (self));

  if (self->account_id <= 0)
    {
      g_debug ("Background sync: no account set, skipping");
      return;
    }

  if (!self->http_client)
    {
      g_debug ("Background sync: no HTTP client set, skipping");
      return;
    }

  g_debug ("Background sync: triggered, fetching conversations");
  gnalk_http_client_get_conversations (self->http_client,
                                         on_conversations_fetched,
                                         self);
}

void
gnalk_background_sync_set_active_room (GnalkBackgroundSync *self,
                                         const char           *room_token)
{
  g_return_if_fail (GNALK_IS_BACKGROUND_SYNC (self));

  g_free (self->active_room_token);
  self->active_room_token = g_strdup (room_token);
}

void
gnalk_background_sync_set_window_focused (GnalkBackgroundSync *self,
                                            gboolean              focused)
{
  g_return_if_fail (GNALK_IS_BACKGROUND_SYNC (self));
  self->window_focused = focused;
}
