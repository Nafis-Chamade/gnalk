#include "gnalk-conversation-controller.h"
#include "gnalk-conversation-data.h"

#define CONVERSATION_POLL_INTERVAL_SECONDS 20

struct _GnalkConversationController
{
  GObject parent_instance;

  GnalkDatabase   *database;
  GnalkHttpClient *http_client;

  gint64   account_id;
  guint    poll_timer_id;
  gboolean has_active_room;
  gboolean polling_enabled;

  /* Generation counter for stale response detection */
  guint generation;
};

G_DEFINE_FINAL_TYPE (GnalkConversationController, gnalk_conversation_controller, G_TYPE_OBJECT)

enum {
  SIGNAL_CONVERSATIONS_UPDATED,
  SIGNAL_CONVERSATION_CREATED,
  SIGNAL_CONVERSATION_LEFT,
  SIGNAL_CONVERSATION_DELETED,
  SIGNAL_NEW_MESSAGES_DETECTED,
  SIGNAL_ERROR,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void do_poll (GnalkConversationController *self);

/* Callback data structures */
typedef struct {
  GnalkConversationController *controller;
  guint                          generation;
} PollCallbackData;

typedef struct {
  GnalkConversationController *controller;
  char                         *room_token;
} OperationCallbackData;

static void
poll_callback_data_free (PollCallbackData *data)
{
  g_slice_free (PollCallbackData, data);
}

static void
operation_callback_data_free (OperationCallbackData *data)
{
  g_free (data->room_token);
  g_slice_free (OperationCallbackData, data);
}

static void
gnalk_conversation_controller_finalize (GObject *object)
{
  GnalkConversationController *self = GNALK_CONVERSATION_CONTROLLER (object);

  gnalk_conversation_controller_stop_polling (self);

  g_clear_object (&self->database);
  g_clear_object (&self->http_client);

  G_OBJECT_CLASS (gnalk_conversation_controller_parent_class)->finalize (object);
}

static void
gnalk_conversation_controller_class_init (GnalkConversationControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_conversation_controller_finalize;

  signals[SIGNAL_CONVERSATIONS_UPDATED] =
    g_signal_new ("conversations-updated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_CONVERSATION_CREATED] =
    g_signal_new ("conversation-created",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);  /* room_token */

  signals[SIGNAL_CONVERSATION_LEFT] =
    g_signal_new ("conversation-left",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);  /* room_token */

  signals[SIGNAL_CONVERSATION_DELETED] =
    g_signal_new ("conversation-deleted",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);  /* room_token */

  signals[SIGNAL_NEW_MESSAGES_DETECTED] =
    g_signal_new ("new-messages-detected",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING,   /* room_token */
                  G_TYPE_INT64);   /* last_message_id */

  signals[SIGNAL_ERROR] =
    g_signal_new ("error",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);  /* error message */
}

static void
gnalk_conversation_controller_init (GnalkConversationController *self)
{
  self->generation = 0;
  self->polling_enabled = FALSE;
  self->has_active_room = FALSE;
}

GnalkConversationController *
gnalk_conversation_controller_new (GnalkDatabase   *database,
                                     GnalkHttpClient *http_client)
{
  GnalkConversationController *self = g_object_new (GNALK_TYPE_CONVERSATION_CONTROLLER, NULL);

  self->database = database ? g_object_ref (database) : NULL;
  self->http_client = http_client ? g_object_ref (http_client) : NULL;

  return self;
}

void
gnalk_conversation_controller_set_account_id (GnalkConversationController *self,
                                                gint64                         account_id)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));

  self->account_id = account_id;
  self->generation++;  /* Invalidate pending operations from old account */
}

/* ============ Polling ============ */

static JsonArray *
extract_conversations_array (JsonNode *response)
{
  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    return NULL;

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    return NULL;

  return json_object_get_array_member (ocs, "data");
}

/* Process conversations response: detect new messages and save to DB */
static gboolean
process_conversations_response (GnalkConversationController *self,
                                 JsonArray                    *conversations)
{
  if (!self->database || self->account_id <= 0 || !conversations)
    return FALSE;

  guint len = json_array_get_length (conversations);
  gboolean has_changes = FALSE;
  GPtrArray *valid_tokens = g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; i < len; i++)
    {
      JsonObject *conv_obj = json_array_get_object_element (conversations, i);
      if (!conv_obj)
        continue;

      const char *token = json_object_get_string_member_with_default (conv_obj, "token", NULL);
      if (!token)
        continue;

      g_ptr_array_add (valid_tokens, g_strdup (token));

      /* Check for new messages by comparing lastMessage.id with DB */
      JsonObject *last_message = json_object_get_object_member (conv_obj, "lastMessage");
      if (last_message)
        {
          gint64 server_last_msg_id = json_object_get_int_member_with_default (last_message, "id", 0);
          gint64 db_last_msg_id = gnalk_database_get_last_known_message_id (self->database,
                                                                               self->account_id,
                                                                               token);

          if (server_last_msg_id > db_last_msg_id)
            {
              g_debug ("ConversationController: Room %s has new messages (server=%ld, db=%ld)",
                       token, (long) server_last_msg_id, (long) db_last_msg_id);

              /* Emit signal for background sync */
              g_signal_emit (self, signals[SIGNAL_NEW_MESSAGES_DETECTED], 0,
                             token, db_last_msg_id);
              has_changes = TRUE;
            }
        }

      /* Save/update conversation in database */
      GnalkConversationData *conv_data = gnalk_conversation_data_new_from_json (conv_obj, NULL);
      if (conv_data)
        {
          gnalk_database_save_conversation (self->database, self->account_id, conv_data);
          g_object_unref (conv_data);
        }
    }

  /* Delete conversations no longer on server */
  int deleted = gnalk_database_delete_stale_conversations (self->database,
                                                              self->account_id,
                                                              valid_tokens);
  if (deleted > 0)
    {
      g_debug ("ConversationController: Deleted %d stale conversations", deleted);
      has_changes = TRUE;
    }

  g_ptr_array_unref (valid_tokens);

  return has_changes;
}

static void
on_poll_response (GnalkHttpClient *client G_GNUC_UNUSED,
                  JsonNode         *response,
                  GError           *error,
                  gpointer          user_data)
{
  PollCallbackData *cb_data = user_data;

  /* Check for cancellation before accessing controller */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      poll_callback_data_free (cb_data);
      return;
    }

  GnalkConversationController *self = cb_data->controller;
  guint expected_generation = cb_data->generation;
  poll_callback_data_free (cb_data);

  /* Discard stale response */
  if (expected_generation != self->generation)
    {
      g_debug ("ConversationController: Discarding stale poll response");
      return;
    }

  if (error)
    {
      g_warning ("ConversationController: Poll failed: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
      return;
    }

  JsonArray *conversations = extract_conversations_array (response);
  if (!conversations)
    return;

  /* Process and save conversations */
  gboolean has_changes = process_conversations_response (self, conversations);

  /* Only emit update signal if data actually changed */
  if (has_changes)
    {
      g_debug ("ConversationController: Conversations changed, emitting signal");
      g_signal_emit (self, signals[SIGNAL_CONVERSATIONS_UPDATED], 0);
    }
}

static void
do_poll (GnalkConversationController *self)
{
  if (!self->http_client || !gnalk_http_client_is_ready (self->http_client))
    return;

  PollCallbackData *cb_data = g_slice_new0 (PollCallbackData);
  cb_data->controller = self;
  cb_data->generation = self->generation;

  gnalk_http_client_get_conversations (self->http_client, on_poll_response, cb_data);
}

static gboolean
on_poll_timer (gpointer user_data)
{
  GnalkConversationController *self = GNALK_CONVERSATION_CONTROLLER (user_data);

  /* Don't poll if a room is active (message polling handles it) */
  if (self->has_active_room)
    {
      g_debug ("ConversationController: Room active, skipping poll");
      self->poll_timer_id = 0;
      return G_SOURCE_REMOVE;
    }

  g_debug ("ConversationController: Polling conversations");
  do_poll (self);

  return G_SOURCE_CONTINUE;
}

void
gnalk_conversation_controller_start_polling (GnalkConversationController *self)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));

  self->polling_enabled = TRUE;

  /* Don't start timer if a room is active */
  if (self->has_active_room)
    return;

  /* Already running */
  if (self->poll_timer_id > 0)
    return;

  g_debug ("ConversationController: Starting polling (every %ds)", CONVERSATION_POLL_INTERVAL_SECONDS);
  self->poll_timer_id = g_timeout_add_seconds (CONVERSATION_POLL_INTERVAL_SECONDS,
                                                 on_poll_timer, self);
}

void
gnalk_conversation_controller_stop_polling (GnalkConversationController *self)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));

  self->polling_enabled = FALSE;

  if (self->poll_timer_id > 0)
    {
      g_debug ("ConversationController: Stopping polling");
      g_source_remove (self->poll_timer_id);
      self->poll_timer_id = 0;
    }
}

void
gnalk_conversation_controller_set_room_active (GnalkConversationController *self,
                                                  gboolean                       has_active_room)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));

  gboolean was_active = self->has_active_room;
  self->has_active_room = has_active_room;

  if (has_active_room)
    {
      /* Room became active - stop conversation polling */
      if (self->poll_timer_id > 0)
        {
          g_debug ("ConversationController: Room active, pausing polling");
          g_source_remove (self->poll_timer_id);
          self->poll_timer_id = 0;
        }
    }
  else if (was_active && self->polling_enabled)
    {
      /* Room closed - resume polling if enabled */
      if (self->poll_timer_id == 0)
        {
          g_debug ("ConversationController: Room closed, resuming polling");
          self->poll_timer_id = g_timeout_add_seconds (CONVERSATION_POLL_INTERVAL_SECONDS,
                                                         on_poll_timer, self);
        }
    }
}

void
gnalk_conversation_controller_refresh (GnalkConversationController *self)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));

  g_debug ("ConversationController: Manual refresh requested");
  do_poll (self);
}

gboolean
gnalk_conversation_controller_is_polling (GnalkConversationController *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self), FALSE);
  return self->poll_timer_id > 0;
}

/* ============ Conversation Operations ============ */

static void
on_create_response (GnalkHttpClient *client G_GNUC_UNUSED,
                    JsonNode         *response,
                    GError           *error,
                    gpointer          user_data)
{
  OperationCallbackData *cb_data = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      operation_callback_data_free (cb_data);
      return;
    }

  GnalkConversationController *self = cb_data->controller;
  operation_callback_data_free (cb_data);

  if (error)
    {
      g_warning ("ConversationController: Create failed: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
      return;
    }

  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    return;

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    return;

  JsonObject *data = json_object_get_object_member (ocs, "data");
  if (!data)
    return;

  const char *token = json_object_get_string_member_with_default (data, "token", NULL);
  if (!token)
    return;

  /* Save the new conversation to database */
  if (self->database && self->account_id > 0)
    {
      GnalkConversationData *conv_data = gnalk_conversation_data_new_from_json (data, NULL);
      if (conv_data)
        {
          gnalk_database_save_conversation (self->database, self->account_id, conv_data);
          g_object_unref (conv_data);
        }
    }

  g_debug ("ConversationController: Created conversation %s", token);
  g_signal_emit (self, signals[SIGNAL_CONVERSATION_CREATED], 0, token);

  /* Refresh to get updated list */
  do_poll (self);
}

void
gnalk_conversation_controller_create (GnalkConversationController *self,
                                         int                           room_type,
                                         const char                   *invite,
                                         const char                   *room_name)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);

  OperationCallbackData *cb_data = g_slice_new0 (OperationCallbackData);
  cb_data->controller = self;
  cb_data->room_token = NULL;

  gnalk_http_client_create_conversation (self->http_client,
                                            room_type,
                                            invite,
                                            room_name,
                                            on_create_response,
                                            cb_data);
}

static void
on_leave_response (GnalkHttpClient *client G_GNUC_UNUSED,
                   JsonNode         *response G_GNUC_UNUSED,
                   GError           *error,
                   gpointer          user_data)
{
  OperationCallbackData *cb_data = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      operation_callback_data_free (cb_data);
      return;
    }

  GnalkConversationController *self = cb_data->controller;
  char *room_token = g_strdup (cb_data->room_token);
  operation_callback_data_free (cb_data);

  if (error)
    {
      g_warning ("ConversationController: Leave failed: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
      g_free (room_token);
      return;
    }

  /* Delete from database */
  if (self->database && self->account_id > 0 && room_token)
    gnalk_database_delete_conversation (self->database, self->account_id, room_token);

  g_debug ("ConversationController: Left conversation %s", room_token);
  g_signal_emit (self, signals[SIGNAL_CONVERSATION_LEFT], 0, room_token);

  /* Refresh to get updated list */
  do_poll (self);

  g_free (room_token);
}

void
gnalk_conversation_controller_leave (GnalkConversationController *self,
                                        const char                   *room_token)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (room_token != NULL);

  OperationCallbackData *cb_data = g_slice_new0 (OperationCallbackData);
  cb_data->controller = self;
  cb_data->room_token = g_strdup (room_token);

  gnalk_http_client_leave_conversation (self->http_client,
                                           room_token,
                                           on_leave_response,
                                           cb_data);
}

static void
on_delete_response (GnalkHttpClient *client G_GNUC_UNUSED,
                    JsonNode         *response G_GNUC_UNUSED,
                    GError           *error,
                    gpointer          user_data)
{
  OperationCallbackData *cb_data = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      operation_callback_data_free (cb_data);
      return;
    }

  GnalkConversationController *self = cb_data->controller;
  char *room_token = g_strdup (cb_data->room_token);
  operation_callback_data_free (cb_data);

  if (error)
    {
      g_warning ("ConversationController: Delete failed: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
      g_free (room_token);
      return;
    }

  /* Delete from database */
  if (self->database && self->account_id > 0 && room_token)
    gnalk_database_delete_conversation (self->database, self->account_id, room_token);

  g_debug ("ConversationController: Deleted conversation %s", room_token);
  g_signal_emit (self, signals[SIGNAL_CONVERSATION_DELETED], 0, room_token);

  /* Refresh to get updated list */
  do_poll (self);

  g_free (room_token);
}

void
gnalk_conversation_controller_delete (GnalkConversationController *self,
                                         const char                   *room_token)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (room_token != NULL);

  OperationCallbackData *cb_data = g_slice_new0 (OperationCallbackData);
  cb_data->controller = self;
  cb_data->room_token = g_strdup (room_token);

  gnalk_http_client_delete_conversation (self->http_client,
                                            room_token,
                                            on_delete_response,
                                            cb_data);
}

static void
on_favorite_response (GnalkHttpClient *client G_GNUC_UNUSED,
                      JsonNode         *response G_GNUC_UNUSED,
                      GError           *error,
                      gpointer          user_data)
{
  OperationCallbackData *cb_data = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      operation_callback_data_free (cb_data);
      return;
    }

  GnalkConversationController *self = cb_data->controller;
  operation_callback_data_free (cb_data);

  if (error)
    {
      g_warning ("ConversationController: Set favorite failed: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
      return;
    }

  /* Refresh to get updated list with new favorite status */
  do_poll (self);
}

void
gnalk_conversation_controller_set_favorite (GnalkConversationController *self,
                                               const char                   *room_token,
                                               gboolean                       favorite)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (room_token != NULL);

  OperationCallbackData *cb_data = g_slice_new0 (OperationCallbackData);
  cb_data->controller = self;
  cb_data->room_token = g_strdup (room_token);

  gnalk_http_client_set_conversation_favorite (self->http_client,
                                                  room_token,
                                                  favorite,
                                                  on_favorite_response,
                                                  cb_data);
}
