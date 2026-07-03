#include "gnalk-message-controller.h"
#include "gnalk-message-data.h"
#include "gnalk-async-op.h"
#include <glib/gi18n.h>

struct _GnalkMessageController
{
  GObject parent_instance;

  GnalkHttpClient *http_client;

  /* Repository integration - all fetched messages are saved here */
  GnalkDataRepository *repository;
  gint64               account_id;
  char                *current_user;

  /* Current room */
  char *room_token;

  /* Polling state */
  gboolean polling;
  gint64   last_known_message_id;
  guint    poll_retry_timeout_id;

  /* Generation counter for stale response detection */
  guint generation;

  /* Track loading state */
  gboolean loading_older;

  /* Session-ready gate: defer polling until POST /participants/active
   * has completed (cookies established). Prevents PHP session race. */
  gboolean session_ready;
  gboolean polling_deferred;
  gint64   deferred_last_known_id;
};

G_DEFINE_FINAL_TYPE (GnalkMessageController, gnalk_message_controller, G_TYPE_OBJECT)

enum {
  SIGNAL_MESSAGES_LOADED,
  SIGNAL_POLL_RESPONSE,
  SIGNAL_OLDER_MESSAGES_LOADED,
  SIGNAL_MESSAGE_SENT,
  SIGNAL_MESSAGE_SEND_FAILED,
  SIGNAL_MESSAGE_EDITED,
  SIGNAL_MESSAGE_EDIT_FAILED,
  SIGNAL_MESSAGE_DELETED,
  SIGNAL_MESSAGE_DELETE_FAILED,
  SIGNAL_REACTION_ADDED,
  SIGNAL_REACTION_FAILED,
  SIGNAL_UPLOAD_STARTED,
  SIGNAL_UPLOAD_PUT_DONE,
  SIGNAL_UPLOAD_COMPLETED,
  SIGNAL_UPLOAD_FAILED,
  SIGNAL_READ_STATUS_UPDATED,
  SIGNAL_CONTEXT_LOADED,
  SIGNAL_SEARCH_RESULTS,
  SIGNAL_THREAD_MESSAGES_LOADED,
  SIGNAL_THREAD_OLDER_LOADED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_messages_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_poll_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_older_messages_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_send_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_edit_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_delete_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_thread_messages_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_thread_older_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_thread_send_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_create_thread_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
/* Thread support — forward declaration needed for on_create_thread_response */
void gnalk_message_controller_load_thread (GnalkMessageController *self, gint64 thread_id);
static void on_reaction_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_context_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_search_messages_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);

/* FileUploadData: uses its own GWeakRef pattern (multi-step chain with many fields) */
typedef struct {
  GWeakRef                 controller_ref;  /* weak ref to avoid use-after-free */
  char                    *remote_path;
  char                    *filename;
  GBytes                  *file_bytes;
  char                    *content_type;
  char                    *room_token;
  char                    *caption;
  char                    *reference_id;
  char                    *message_type;  /* "voice-message" for voice, NULL for regular files */
  gint64                   reply_to;
  int                      collision_count;
} FileUploadData;

static void
file_upload_data_free (FileUploadData *data)
{
  g_weak_ref_clear (&data->controller_ref);
  g_free (data->remote_path);
  g_free (data->filename);
  g_clear_pointer (&data->file_bytes, g_bytes_unref);
  g_free (data->content_type);
  g_free (data->room_token);
  g_free (data->caption);
  g_free (data->reference_id);
  g_free (data->message_type);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FileUploadData, file_upload_data_free)

static void
gnalk_message_controller_finalize (GObject *object)
{
  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (object);

  gnalk_message_controller_stop_polling (self);

  g_clear_pointer (&self->room_token, g_free);
  g_clear_pointer (&self->current_user, g_free);
  g_clear_object (&self->http_client);
  g_clear_object (&self->repository);

  G_OBJECT_CLASS (gnalk_message_controller_parent_class)->finalize (object);
}

static void
gnalk_message_controller_class_init (GnalkMessageControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_message_controller_finalize;

  signals[SIGNAL_MESSAGES_LOADED] =
    g_signal_new ("messages-loaded",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  JSON_TYPE_ARRAY,   /* messages */
                  G_TYPE_BOOLEAN);   /* is_initial */

  signals[SIGNAL_POLL_RESPONSE] =
    g_signal_new ("poll-response",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  JSON_TYPE_ARRAY);  /* messages */

  signals[SIGNAL_OLDER_MESSAGES_LOADED] =
    g_signal_new ("older-messages-loaded",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  JSON_TYPE_ARRAY,   /* messages */
                  G_TYPE_BOOLEAN);   /* has_more */

  signals[SIGNAL_MESSAGE_SENT] =
    g_signal_new ("message-sent",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_STRING,     /* reference_id */
                  G_TYPE_INT64,      /* message_id */
                  G_TYPE_INT64);     /* timestamp */

  signals[SIGNAL_MESSAGE_SEND_FAILED] =
    g_signal_new ("message-send-failed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING,     /* reference_id */
                  G_TYPE_STRING);    /* error */

  signals[SIGNAL_MESSAGE_EDITED] =
    g_signal_new ("message-edited",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT64);     /* message_id */

  signals[SIGNAL_MESSAGE_EDIT_FAILED] =
    g_signal_new ("message-edit-failed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_INT64,      /* message_id */
                  G_TYPE_STRING);    /* error */

  signals[SIGNAL_MESSAGE_DELETED] =
    g_signal_new ("message-deleted",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT64);     /* message_id */

  signals[SIGNAL_MESSAGE_DELETE_FAILED] =
    g_signal_new ("message-delete-failed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_INT64,      /* message_id */
                  G_TYPE_STRING);    /* error */

  signals[SIGNAL_REACTION_ADDED] =
    g_signal_new ("reaction-added",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_INT64,      /* message_id */
                  G_TYPE_STRING);    /* emoji */

  signals[SIGNAL_REACTION_FAILED] =
    g_signal_new ("reaction-failed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_INT64,      /* message_id */
                  G_TYPE_STRING,     /* emoji */
                  G_TYPE_STRING);    /* error */

  signals[SIGNAL_UPLOAD_STARTED] =
    g_signal_new ("upload-started",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);    /* reference_id */

  /* Emitted after the PUT (WebDAV upload) completes — tells the upload queue
   * to advance to the next file without waiting for the share POST response
   * (which can take 30+ seconds server-side). */
  signals[SIGNAL_UPLOAD_PUT_DONE] =
    g_signal_new ("upload-put-done",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);    /* reference_id */

  signals[SIGNAL_UPLOAD_COMPLETED] =
    g_signal_new ("upload-completed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);    /* reference_id */

  signals[SIGNAL_UPLOAD_FAILED] =
    g_signal_new ("upload-failed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING,     /* reference_id */
                  G_TYPE_STRING);    /* error */

  signals[SIGNAL_READ_STATUS_UPDATED] =
    g_signal_new ("read-status-updated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT64);     /* last_common_read_message */

  signals[SIGNAL_CONTEXT_LOADED] =
    g_signal_new ("context-loaded",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  JSON_TYPE_ARRAY,   /* messages */
                  G_TYPE_INT64);     /* target_message_id */

  signals[SIGNAL_SEARCH_RESULTS] =
    g_signal_new ("search-results",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 3,
                  JSON_TYPE_ARRAY,   /* entries */
                  G_TYPE_BOOLEAN,    /* has_more */
                  G_TYPE_INT);       /* cursor */

  signals[SIGNAL_THREAD_MESSAGES_LOADED] =
    g_signal_new ("thread-messages-loaded",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  JSON_TYPE_ARRAY,   /* messages */
                  G_TYPE_INT64);     /* thread_id */

  signals[SIGNAL_THREAD_OLDER_LOADED] =
    g_signal_new ("thread-older-loaded",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  JSON_TYPE_ARRAY,   /* messages */
                  G_TYPE_BOOLEAN);   /* has_more */
}

static void
gnalk_message_controller_init (GnalkMessageController *self)
{
  self->polling = FALSE;
  self->generation = 0;
  self->loading_older = FALSE;
}

GnalkMessageController *
gnalk_message_controller_new (GnalkHttpClient *http_client)
{
  GnalkMessageController *self = g_object_new (GNALK_TYPE_MESSAGE_CONTROLLER, NULL);

  self->http_client = http_client ? g_object_ref (http_client) : NULL;

  return self;
}

void
gnalk_message_controller_set_repository (GnalkMessageController *self,
                                         GnalkDataRepository    *repository,
                                         gint64                  account_id)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));

  g_set_object (&self->repository, repository);
  self->account_id = account_id;
}

void
gnalk_message_controller_set_current_user (GnalkMessageController *self,
                                             const char              *user_id)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));

  g_free (self->current_user);
  self->current_user = g_strdup (user_id);
}

void
gnalk_message_controller_set_room (GnalkMessageController *self,
                                     const char              *room_token)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));

  if (g_strcmp0 (self->room_token, room_token) == 0)
    return;

  /* Stop polling and cancel pending requests when room changes */
  gnalk_message_controller_stop_polling (self);
  if (self->http_client)
    gnalk_http_client_cancel_room_requests (self->http_client);

  g_free (self->room_token);
  self->room_token = g_strdup (room_token);
  self->last_known_message_id = 0;
  self->polling_deferred = FALSE;
  self->loading_older = FALSE;

  /* Increment generation to discard stale responses */
  self->generation++;
}

const char *
gnalk_message_controller_get_room (GnalkMessageController *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self), NULL);
  return self->room_token;
}

void
gnalk_message_controller_cancel (GnalkMessageController *self)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));

  gnalk_message_controller_stop_polling (self);
  self->generation++;
}

/* ============ Message Loading ============ */

static JsonArray *
extract_messages_array (JsonNode *response)
{
  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    return NULL;

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    return NULL;

  return json_object_get_array_member (ocs, "data");
}

/* Save messages from JSON array to database.
 * Returns the highest message ID seen. */
static gint64
save_messages_to_db (GnalkMessageController *self,
                     JsonArray               *messages,
                     const char              *room_token)
{
  if (!self->repository || self->account_id <= 0 || !messages)
    return 0;

  guint len = json_array_get_length (messages);
  gint64 highest_id = 0;

  for (guint i = 0; i < len; i++)
    {
      JsonObject *msg_obj = json_array_get_object_element (messages, i);
      if (!msg_obj)
        continue;

      gint64 msg_id = json_object_get_int_member_with_default (msg_obj, "id", 0);
      if (msg_id > highest_id)
        highest_id = msg_id;

      /* Skip special system messages that modify other messages rather than
       * being displayed standalone (reaction, message_edited, message_deleted) */
      const char *system_message = json_object_get_string_member_with_default (msg_obj, "systemMessage", "");
      if (g_strcmp0 (system_message, "reaction") == 0 ||
          g_strcmp0 (system_message, "message_edited") == 0 ||
          g_strcmp0 (system_message, "message_deleted") == 0)
        continue;

      GnalkMessageData *msg_data = gnalk_message_data_new_from_json (msg_obj, self->current_user);
      if (!msg_data)
        continue;

      gnalk_data_repository_save_message (self->repository, self->account_id, room_token, msg_data);
      g_object_unref (msg_data);
    }

  /* Update last known message ID in sync state */
  if (highest_id > 0)
    gnalk_data_repository_set_last_known_message_id (self->repository, self->account_id, room_token, highest_id);

  return highest_id;
}

static void
on_messages_response (GnalkHttpClient *client G_GNUC_UNUSED,
                      JsonNode         *response,
                      GError           *error,
                      gpointer          user_data)
{
  GnalkAsyncOp *op = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      gnalk_async_op_free (op);
      return;
    }

  if (!gnalk_async_op_is_valid (op))
    {
      gnalk_async_op_free (op);
      return;
    }

  g_autoptr(GObject) owner = gnalk_async_op_get_owner (op);
  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (owner);
  gnalk_async_op_free (op);

  if (error)
    {
      g_warning ("Failed to get messages: %s", error->message);
      return;
    }

  JsonArray *messages = extract_messages_array (response);
  if (messages)
    {
      /* Save to database before emitting signal */
      save_messages_to_db (self, messages, self->room_token);
      g_signal_emit (self, signals[SIGNAL_MESSAGES_LOADED], 0, messages, TRUE);
    }
}

void
gnalk_message_controller_load_messages (GnalkMessageController *self)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (self->room_token != NULL);

  GnalkAsyncOp *op = gnalk_async_op_new (G_OBJECT (self), &self->generation, NULL, 0);

  gnalk_http_client_get_messages (self->http_client,
                                    self->room_token,
                                    0,
                                    FALSE,
                                    0,  /* timeout (unused for lookIntoFuture=FALSE) */
                                    on_messages_response,
                                    op);
}

static void
on_new_messages_response (GnalkHttpClient *client G_GNUC_UNUSED,
                          JsonNode         *response,
                          GError           *error,
                          gpointer          user_data)
{
  GnalkAsyncOp *op = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      gnalk_async_op_free (op);
      return;
    }

  if (!gnalk_async_op_is_valid (op))
    {
      gnalk_async_op_free (op);
      return;
    }

  g_autoptr(GObject) owner = gnalk_async_op_get_owner (op);
  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (owner);
  gnalk_async_op_free (op);

  if (error)
    {
      g_warning ("Failed to get new messages: %s", error->message);
      return;
    }

  JsonArray *messages = extract_messages_array (response);
  if (messages)
    {
      guint len = json_array_get_length (messages);
      for (guint i = 0; i < len; i++)
        {
          JsonObject *msg = json_array_get_object_element (messages, i);
          if (msg)
            {
              gint64 msg_id = json_object_get_int_member_with_default (msg, "id", 0);
              if (msg_id > self->last_known_message_id)
                self->last_known_message_id = msg_id;
            }
        }

      /* Save to database before emitting signal */
      save_messages_to_db (self, messages, self->room_token);

      /* Emit with is_initial=FALSE to indicate these are incremental updates */
      g_signal_emit (self, signals[SIGNAL_MESSAGES_LOADED], 0, messages, FALSE);
    }
}

void
gnalk_message_controller_load_new_messages (GnalkMessageController *self,
                                              gint64                   since_id)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (self->room_token != NULL);
  g_return_if_fail (since_id > 0);

  self->last_known_message_id = since_id;

  GnalkAsyncOp *op = gnalk_async_op_new (G_OBJECT (self), &self->generation, NULL, 0);

  /* Use lookIntoFuture=TRUE to get messages after the given ID */
  gnalk_http_client_get_messages (self->http_client,
                                    self->room_token,
                                    since_id,
                                    TRUE,
                                    0,  /* timeout=0 for non-blocking request */
                                    on_new_messages_response,
                                    op);
}

/* ============ Older Messages (History) ============ */

static void
on_older_messages_response (GnalkHttpClient *client G_GNUC_UNUSED,
                            JsonNode         *response,
                            GError           *error,
                            gpointer          user_data)
{
  GnalkAsyncOp *op = user_data;

  /* Always clear loading_older if owner is alive, even for stale or
   * cancelled responses — otherwise pause_polling() mid-load leaves the
   * flag stuck and history loading is dead until the next room switch. */
  GObject *owner_check = gnalk_async_op_get_owner (op);
  if (owner_check)
    {
      GNALK_MESSAGE_CONTROLLER (owner_check)->loading_older = FALSE;
      g_object_unref (owner_check);
    }

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      gnalk_async_op_free (op);
      return;
    }

  if (!gnalk_async_op_is_valid (op))
    {
      gnalk_async_op_free (op);
      return;
    }

  g_autoptr(GObject) owner = gnalk_async_op_get_owner (op);
  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (owner);
  gnalk_async_op_free (op);

  if (error)
    {
      g_warning ("Failed to get older messages: %s", error->message);
      /* Emit with empty array so the processor resets its loading_older flag.
       * has_more=TRUE preserves has_more_history so the user can retry. */
      g_autoptr(JsonArray) empty = json_array_new ();
      g_signal_emit (self, signals[SIGNAL_OLDER_MESSAGES_LOADED], 0, empty, TRUE);
      return;
    }

  JsonArray *messages = extract_messages_array (response);
  if (messages)
    {
      guint len = json_array_get_length (messages);
      gboolean has_more = (len >= 50);

      /* Save to database before emitting signal */
      save_messages_to_db (self, messages, self->room_token);

      /* Update has_more_history in DB */
      if (self->repository && self->account_id > 0)
        gnalk_data_repository_set_has_more_history (self->repository, self->account_id,
                                                     self->room_token, has_more);

      g_signal_emit (self, signals[SIGNAL_OLDER_MESSAGES_LOADED], 0, messages, has_more);
    }
}

void
gnalk_message_controller_load_older_messages (GnalkMessageController *self,
                                                gint64                   before_id)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (self->room_token != NULL);

  if (self->loading_older)
    return;

  self->loading_older = TRUE;

  GnalkAsyncOp *op = gnalk_async_op_new (G_OBJECT (self), &self->generation, NULL, 0);

  /* Use look_into_future=FALSE to get messages before the given ID */
  gnalk_http_client_get_messages (self->http_client,
                                    self->room_token,
                                    before_id,
                                    FALSE,  /* look_into_future=FALSE means get older */
                                    0,      /* timeout (unused for lookIntoFuture=FALSE) */
                                    on_older_messages_response,
                                    op);
}

/* ============ Long Polling ============ */

static gboolean
poll_retry_timeout (gpointer user_data)
{
  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (user_data);

  self->poll_retry_timeout_id = 0;

  if (self->polling && self->room_token)
    gnalk_message_controller_start_polling (self, self->last_known_message_id);

  return G_SOURCE_REMOVE;
}

static void
on_poll_response (GnalkHttpClient *client G_GNUC_UNUSED,
                  JsonNode         *response,
                  GError           *error,
                  gpointer          user_data)
{
  GnalkAsyncOp *op = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      gnalk_async_op_free (op);
      return;
    }

  if (!gnalk_async_op_is_valid (op))
    {
      gnalk_async_op_free (op);
      return;
    }

  g_autoptr(GObject) owner = gnalk_async_op_get_owner (op);
  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (owner);
  gnalk_async_op_free (op);

  if (error)
    {
      g_warning ("Poll error: %s", error->message);
      /* Retry after delay */
      if (self->polling)
        self->poll_retry_timeout_id = g_timeout_add_seconds (5, poll_retry_timeout, self);
      return;
    }

  JsonArray *messages = extract_messages_array (response);
  if (messages)
    {
      /* Update last known ID from response */
      guint len = json_array_get_length (messages);
      for (guint i = 0; i < len; i++)
        {
          JsonObject *msg = json_array_get_object_element (messages, i);
          if (msg)
            {
              gint64 msg_id = json_object_get_int_member_with_default (msg, "id", 0);
              if (msg_id > self->last_known_message_id)
                self->last_known_message_id = msg_id;
            }
        }

      /* Save to database before emitting signal */
      save_messages_to_db (self, messages, self->room_token);

      g_signal_emit (self, signals[SIGNAL_POLL_RESPONSE], 0, messages);

      /* Update conversation subtitle with the latest message */
      if (self->repository && self->account_id > 0 && len > 0)
        {
          /* Find the message with highest ID (most recent) */
          JsonObject *latest_msg = NULL;
          gint64 latest_id = 0;
          for (guint i = 0; i < len; i++)
            {
              JsonObject *msg = json_array_get_object_element (messages, i);
              if (msg)
                {
                  gint64 msg_id = json_object_get_int_member_with_default (msg, "id", 0);
                  if (msg_id > latest_id)
                    {
                      latest_id = msg_id;
                      latest_msg = msg;
                    }
                }
            }

          if (latest_msg)
            {
              const char *message_text = json_object_get_string_member_with_default (latest_msg, "message", "");
              const char *actor_id = json_object_get_string_member_with_default (latest_msg, "actorId", "");
              const char *actor_name = json_object_get_string_member_with_default (latest_msg, "actorDisplayName", "");
              const char *system_message = json_object_get_string_member_with_default (latest_msg, "systemMessage", "");
              gint64 timestamp = json_object_get_int_member_with_default (latest_msg, "timestamp", 0);

              /* Determine message type */
              const char *message_type = (system_message && *system_message) ? "system" : "comment";

              /* Serialize message parameters if present */
              g_autofree char *params_json = NULL;
              if (json_object_has_member (latest_msg, "messageParameters"))
                {
                  JsonNode *params_node = json_object_get_member (latest_msg, "messageParameters");
                  if (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
                    {
                      JsonGenerator *gen = json_generator_new ();
                      json_generator_set_root (gen, params_node);
                      params_json = json_generator_to_data (gen, NULL);
                      g_object_unref (gen);
                    }
                }

              gnalk_data_repository_update_conversation_last_message (self->repository,
                                                                        self->account_id,
                                                                        self->room_token,
                                                                        timestamp,
                                                                        latest_id,
                                                                        message_text,
                                                                        actor_id,
                                                                        actor_name,
                                                                        message_type,
                                                                        params_json);
            }
        }
    }

  /* Continue polling if still active - directly start next request
   * (don't call start_polling which would return early since polling=TRUE) */
  if (self->polling && self->room_token)
    {
      GnalkAsyncOp *poll_op = gnalk_async_op_new (G_OBJECT (self), &self->generation, NULL, 0);

      gnalk_http_client_get_messages (self->http_client,
                                        self->room_token,
                                        self->last_known_message_id,
                                        TRUE,  /* long poll */
                                        30,    /* 30 second timeout */
                                        on_poll_response,
                                        poll_op);
    }
}

static void on_initial_messages_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);

void
gnalk_message_controller_start_polling (GnalkMessageController *self,
                                          gint64                   last_known_id)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (self->room_token != NULL);

  /* Defer polling until session cookies are established */
  if (!self->session_ready)
    {
      self->polling_deferred = TRUE;
      self->deferred_last_known_id = last_known_id;
      g_debug ("MC: deferring polling until session ready");
      return;
    }

  /* Don't restart polling if already running */
  if (self->polling)
    return;

  self->polling = TRUE;
  self->last_known_message_id = last_known_id;

  GnalkAsyncOp *op = gnalk_async_op_new (G_OBJECT (self), &self->generation, NULL, 0);

  if (last_known_id == 0)
    {
      /* No cached messages - first fetch recent messages, then start polling */
      gnalk_http_client_get_messages (self->http_client,
                                        self->room_token,
                                        0,
                                        FALSE,  /* lookIntoFuture=FALSE to get recent messages */
                                        0,      /* no timeout for non-polling request */
                                        on_initial_messages_response,
                                        op);
    }
  else
    {
      /* Have cached messages - start long-polling for new ones */
      gnalk_http_client_get_messages (self->http_client,
                                        self->room_token,
                                        last_known_id,
                                        TRUE,  /* wait (long poll) */
                                        30,    /* 30 second timeout */
                                        on_poll_response,
                                        op);
    }
}

static void
on_initial_messages_response (GnalkHttpClient *client G_GNUC_UNUSED,
                               JsonNode         *response,
                               GError           *error,
                               gpointer          user_data)
{
  GnalkAsyncOp *op = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      gnalk_async_op_free (op);
      return;
    }

  if (!gnalk_async_op_is_valid (op))
    {
      gnalk_async_op_free (op);
      return;
    }

  g_autoptr(GObject) owner = gnalk_async_op_get_owner (op);
  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (owner);
  gnalk_async_op_free (op);

  if (error)
    {
      g_warning ("Failed to get initial messages: %s", error->message);
      return;
    }

  JsonArray *messages = extract_messages_array (response);
  if (messages)
    {
      /* Find highest message ID */
      gint64 highest_id = 0;
      guint len = json_array_get_length (messages);
      for (guint i = 0; i < len; i++)
        {
          JsonObject *msg = json_array_get_object_element (messages, i);
          gint64 msg_id = json_object_get_int_member_with_default (msg, "id", 0);
          if (msg_id > highest_id)
            highest_id = msg_id;
        }

      /* Emit signal with messages for display */
      g_signal_emit (self, signals[SIGNAL_MESSAGES_LOADED], 0, messages, TRUE);

      /* Now start long-polling from the highest message ID */
      if (highest_id > 0 && self->polling)
        {
          self->last_known_message_id = highest_id;

          GnalkAsyncOp *poll_op = gnalk_async_op_new (G_OBJECT (self), &self->generation, NULL, 0);

          gnalk_http_client_get_messages (self->http_client,
                                            self->room_token,
                                            highest_id,
                                            TRUE,  /* long poll */
                                            30,
                                            on_poll_response,
                                            poll_op);
        }
    }
}

void
gnalk_message_controller_stop_polling (GnalkMessageController *self)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));

  self->polling = FALSE;

  if (self->poll_retry_timeout_id > 0)
    {
      g_source_remove (self->poll_retry_timeout_id);
      self->poll_retry_timeout_id = 0;
    }

  /* Cancel pending HTTP requests - caller should handle this via http_client */
}

gboolean
gnalk_message_controller_is_polling (GnalkMessageController *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self), FALSE);
  return self->polling;
}

void
gnalk_message_controller_pause_polling (GnalkMessageController *self)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));

  if (!self->polling)
    return;

  self->polling = FALSE;

  /* Cancel in-flight long-poll to release the server-side PHP session lock */
  if (self->http_client)
    gnalk_http_client_cancel_room_requests (self->http_client);
}

void
gnalk_message_controller_resume_polling (GnalkMessageController *self)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));

  if (self->polling)
    return;

  /* Allow id 0: an empty room (e.g. just uploaded the first file) still needs
   * live polling — get_messages with lastKnownId=0, lookIntoFuture=1 polls for
   * everything after the start. Only a genuinely invalid (<0) id blocks. */
  if (!self->http_client || !self->room_token || self->last_known_message_id < 0)
    return;

  self->polling = TRUE;

  GnalkAsyncOp *op = gnalk_async_op_new (G_OBJECT (self), &self->generation, NULL, 0);

  gnalk_http_client_get_messages (self->http_client,
                                    self->room_token,
                                    self->last_known_message_id,
                                    TRUE,  /* long poll */
                                    30,
                                    on_poll_response,
                                    op);
}

/* ============ Send Message ============ */

static void
on_send_response (GnalkHttpClient *client G_GNUC_UNUSED,
                  JsonNode         *response,
                  GError           *error,
                  gpointer          user_data)
{
  GnalkAsyncOp *op = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      gnalk_async_op_free (op);
      return;
    }

  g_autoptr(GObject) owner = gnalk_async_op_get_owner (op);
  if (!owner)
    {
      gnalk_async_op_free (op);
      return;
    }

  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (owner);
  const char *reference_id = op->context_data;

  if (error)
    {
      g_warning ("Failed to send message: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_MESSAGE_SEND_FAILED], 0,
                     reference_id, error->message);
      gnalk_async_op_free (op);
      return;
    }

  /* Extract message details from response */
  if (response && JSON_NODE_HOLDS_OBJECT (response))
    {
      JsonObject *root = json_node_get_object (response);
      JsonObject *ocs = json_object_get_object_member (root, "ocs");
      if (ocs)
        {
          JsonObject *msg_data = json_object_get_object_member (ocs, "data");
          if (msg_data)
            {
              const char *ref_id = json_object_get_string_member_with_default (msg_data, "referenceId", "");
              gint64 msg_id = json_object_get_int_member_with_default (msg_data, "id", 0);
              gint64 timestamp = json_object_get_int_member_with_default (msg_data, "timestamp", 0);

              if (ref_id && *ref_id && msg_id > 0)
                {
                  g_signal_emit (self, signals[SIGNAL_MESSAGE_SENT], 0,
                                 ref_id, msg_id, timestamp);
                }

              /* Update conversation subtitle with sent message data */
              const char *message_text = json_object_get_string_member_with_default (msg_data, "message", "");
              const char *actor_id = json_object_get_string_member_with_default (msg_data, "actorId", "");
              const char *actor_name = json_object_get_string_member_with_default (msg_data, "actorDisplayName", "");
              const char *system_message = json_object_get_string_member_with_default (msg_data, "systemMessage", "");

              /* Determine message type */
              const char *message_type = (system_message && *system_message) ? "system" : "comment";

              /* Serialize message parameters if present */
              g_autofree char *params_json = NULL;
              if (json_object_has_member (msg_data, "messageParameters"))
                {
                  JsonNode *params_node = json_object_get_member (msg_data, "messageParameters");
                  if (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
                    {
                      JsonGenerator *gen = json_generator_new ();
                      json_generator_set_root (gen, params_node);
                      params_json = json_generator_to_data (gen, NULL);
                      g_object_unref (gen);
                    }
                }

              gnalk_data_repository_update_conversation_last_message (self->repository,
                                                                        self->account_id,
                                                                        self->room_token,
                                                                        timestamp,
                                                                        msg_id,
                                                                        message_text,
                                                                        actor_id,
                                                                        actor_name,
                                                                        message_type,
                                                                        params_json);

              /* Update local DB read status - server already updates it when sending,
               * but we need to update locally so conversation list shows 0 unread */
              gnalk_data_repository_update_conversation_read_status (self->repository,
                                                                       self->account_id,
                                                                       self->room_token,
                                                                       msg_id);
            }
        }
    }

  gnalk_async_op_free (op);
}

void
gnalk_message_controller_send_message (GnalkMessageController *self,
                                         const char              *message,
                                         gint64                   reply_to,
                                         const char              *reference_id)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (self->room_token != NULL);
  g_return_if_fail (message != NULL && *message);

  GnalkAsyncOp *op = gnalk_async_op_new (G_OBJECT (self), NULL, reference_id, 0);

  gnalk_http_client_send_message (self->http_client,
                                    self->room_token,
                                    message,
                                    reply_to,
                                    reference_id,
                                    on_send_response,
                                    op);
}

/* ============ Edit Message ============ */

static void
on_edit_response (GnalkHttpClient *client G_GNUC_UNUSED,
                  JsonNode         *response G_GNUC_UNUSED,
                  GError           *error,
                  gpointer          user_data)
{
  GnalkAsyncOp *op = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      gnalk_async_op_free (op);
      return;
    }

  g_autoptr(GObject) owner = gnalk_async_op_get_owner (op);
  if (!owner)
    {
      gnalk_async_op_free (op);
      return;
    }

  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (owner);
  gint64 message_id = op->context_id;
  gnalk_async_op_free (op);

  if (error)
    {
      g_warning ("Failed to edit message: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_MESSAGE_EDIT_FAILED], 0,
                     message_id, error->message);
      return;
    }

  g_signal_emit (self, signals[SIGNAL_MESSAGE_EDITED], 0, message_id);
}

void
gnalk_message_controller_edit_message (GnalkMessageController *self,
                                         gint64                   message_id,
                                         const char              *new_text)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (self->room_token != NULL);
  g_return_if_fail (new_text != NULL && *new_text);

  GnalkAsyncOp *op = gnalk_async_op_new (G_OBJECT (self), NULL, NULL, message_id);

  gnalk_http_client_edit_message (self->http_client,
                                    self->room_token,
                                    message_id,
                                    new_text,
                                    on_edit_response,
                                    op);
}

/* ============ Delete Message ============ */

static void
on_delete_response (GnalkHttpClient *client G_GNUC_UNUSED,
                    JsonNode         *response G_GNUC_UNUSED,
                    GError           *error,
                    gpointer          user_data)
{
  GnalkAsyncOp *op = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      gnalk_async_op_free (op);
      return;
    }

  g_autoptr(GObject) owner = gnalk_async_op_get_owner (op);
  if (!owner)
    {
      gnalk_async_op_free (op);
      return;
    }

  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (owner);
  gint64 message_id = op->context_id;
  gnalk_async_op_free (op);

  if (error)
    {
      g_warning ("Failed to delete message: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_MESSAGE_DELETE_FAILED], 0,
                     message_id, error->message);
      return;
    }

  g_signal_emit (self, signals[SIGNAL_MESSAGE_DELETED], 0, message_id);
}

void
gnalk_message_controller_delete_message (GnalkMessageController *self,
                                           gint64                   message_id)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (self->room_token != NULL);

  GnalkAsyncOp *op = gnalk_async_op_new (G_OBJECT (self), NULL, NULL, message_id);

  gnalk_http_client_delete_message (self->http_client,
                                      self->room_token,
                                      message_id,
                                      on_delete_response,
                                      op);
}

/* ============ Reactions ============ */

static void
on_reaction_response (GnalkHttpClient *client G_GNUC_UNUSED,
                      JsonNode         *response G_GNUC_UNUSED,
                      GError           *error,
                      gpointer          user_data)
{
  GnalkAsyncOp *op = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      gnalk_async_op_free (op);
      return;
    }

  g_autoptr(GObject) owner = gnalk_async_op_get_owner (op);
  if (!owner)
    {
      gnalk_async_op_free (op);
      return;
    }

  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (owner);
  gint64 message_id = op->context_id;
  const char *emoji = op->context_data;

  if (error)
    {
      g_warning ("Failed to add reaction: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_REACTION_FAILED], 0,
                     message_id, emoji, error->message);
    }
  else
    {
      g_signal_emit (self, signals[SIGNAL_REACTION_ADDED], 0, message_id, emoji);
    }

  gnalk_async_op_free (op);
}

void
gnalk_message_controller_add_reaction (GnalkMessageController *self,
                                         gint64                   message_id,
                                         const char              *emoji)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (self->room_token != NULL);
  g_return_if_fail (emoji != NULL && *emoji);

  GnalkAsyncOp *op = gnalk_async_op_new (G_OBJECT (self), NULL, emoji, message_id);

  gnalk_http_client_add_reaction (self->http_client,
                                    self->room_token,
                                    message_id,
                                    emoji,
                                    on_reaction_response,
                                    op);
}

/* ============ File Upload ============ */

static void start_file_upload_check_exists (FileUploadData *upload_data);

static void
on_file_share_response (GnalkHttpClient *client G_GNUC_UNUSED,
                        JsonNode         *response G_GNUC_UNUSED,
                        GError           *error,
                        gpointer          user_data)
{
  FileUploadData *data = user_data;

  /* Check for cancellation BEFORE accessing controller */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      file_upload_data_free (data);
      return;
    }

  GnalkMessageController *self = g_weak_ref_get (&data->controller_ref);
  if (!self)
    {
      /* Controller was destroyed, just free the data */
      file_upload_data_free (data);
      return;
    }

  if (error)
    {
      g_warning ("Failed to share file to conversation: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_UPLOAD_FAILED], 0,
                     data->reference_id, error->message);
    }
  else
    {
      g_debug ("MC: file shared successfully ref=%.12s…", data->reference_id);
      g_signal_emit (self, signals[SIGNAL_UPLOAD_COMPLETED], 0, data->reference_id);
    }

  g_object_unref (self);
  file_upload_data_free (data);
}

static void
on_file_upload_response (GnalkHttpClient *client,
                         gboolean          success,
                         GError           *error,
                         gpointer          user_data)
{
  FileUploadData *data = user_data;

  /* Check for cancellation BEFORE accessing controller */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      file_upload_data_free (data);
      return;
    }

  GnalkMessageController *self = g_weak_ref_get (&data->controller_ref);
  if (!self)
    {
      /* Controller was destroyed, just free the data */
      file_upload_data_free (data);
      return;
    }

  if (error || !success)
    {
      g_warning ("Failed to upload file: %s", error ? error->message : "Unknown error");
      /* Signal queue to advance even on failure */
      g_signal_emit (self, signals[SIGNAL_UPLOAD_PUT_DONE], 0, data->reference_id);
      g_signal_emit (self, signals[SIGNAL_UPLOAD_FAILED], 0,
                     data->reference_id, error ? error->message : "Upload failed");
      g_object_unref (self);
      file_upload_data_free (data);
      return;
    }

  g_debug ("MC: PUT done, sharing to conversation '%s' ref=%.12s…", data->room_token, data->reference_id);

  /* Signal upload queue to advance immediately — the share POST can take 30+
   * seconds server-side, so don't block subsequent uploads on it. */
  g_signal_emit (self, signals[SIGNAL_UPLOAD_PUT_DONE], 0, data->reference_id);

  /* Unref self before continuing - the next callback will re-acquire it */
  g_object_unref (self);

  gnalk_http_client_share_to_conversation (client,
                                             data->remote_path,
                                             data->room_token,
                                             data->caption,
                                             data->reply_to,
                                             data->reference_id,
                                             data->message_type,
                                             on_file_share_response,
                                             data);
}

static void
on_file_exists_response (GnalkHttpClient *client G_GNUC_UNUSED,
                         gboolean          exists,
                         GError           *error,
                         gpointer          user_data)
{
  FileUploadData *data = user_data;

  /* Check for cancellation BEFORE accessing controller */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      file_upload_data_free (data);
      return;
    }

  GnalkMessageController *self = g_weak_ref_get (&data->controller_ref);
  if (!self)
    {
      /* Controller was destroyed, just free the data */
      file_upload_data_free (data);
      return;
    }

  if (error)
    {
      g_warning ("Failed to check file existence: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_UPLOAD_PUT_DONE], 0, data->reference_id);
      g_signal_emit (self, signals[SIGNAL_UPLOAD_FAILED], 0,
                     data->reference_id, error->message);
      g_object_unref (self);
      file_upload_data_free (data);
      return;
    }

  if (exists)
    {
      /* File exists, rename with (2), (3), etc. */
      data->collision_count++;

      const char *ext_pos = g_strrstr (data->filename, ".");
      g_autofree char *new_name = NULL;

      if (ext_pos)
        {
          gsize base_len = ext_pos - data->filename;
          g_autofree char *base = g_strndup (data->filename, base_len);
          new_name = g_strdup_printf ("%s (%d)%s", base, data->collision_count, ext_pos);
        }
      else
        {
          new_name = g_strdup_printf ("%s (%d)", data->filename, data->collision_count);
        }

      g_free (data->remote_path);
      data->remote_path = g_strdup_printf ("/Talk/%s", new_name);

      /* Unref before recursive call - will re-acquire in callback */
      g_object_unref (self);
      start_file_upload_check_exists (data);
      return;
    }

  g_debug ("MC: check_exists done, uploading to %s", data->remote_path);

  /* Unref before starting upload - the next callback will re-acquire it */
  GnalkHttpClient *http_client = self->http_client;
  g_object_unref (self);

  gnalk_http_client_upload_file (http_client,
                                   data->remote_path,
                                   data->file_bytes,
                                   data->content_type,
                                   on_file_upload_response,
                                   data);
}

static void
start_file_upload_check_exists (FileUploadData *upload_data)
{
  GnalkMessageController *self = g_weak_ref_get (&upload_data->controller_ref);
  if (!self)
    {
      /* Controller was destroyed, just free the data */
      file_upload_data_free (upload_data);
      return;
    }

  GnalkHttpClient *http_client = self->http_client;
  g_object_unref (self);

  gnalk_http_client_check_file_exists (http_client,
                                         upload_data->remote_path,
                                         on_file_exists_response,
                                         upload_data);
}

void
gnalk_message_controller_upload_file (GnalkMessageController *self,
                                        const char              *filename,
                                        GBytes                  *file_bytes,
                                        const char              *content_type,
                                        const char              *caption,
                                        gint64                   reply_to,
                                        const char              *reference_id)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (self->room_token != NULL);
  g_return_if_fail (filename != NULL && *filename);
  g_return_if_fail (file_bytes != NULL);

  FileUploadData *data = g_new0 (FileUploadData, 1);
  g_weak_ref_init (&data->controller_ref, self);
  data->filename = g_strdup (filename);
  data->remote_path = g_strdup_printf ("/Talk/%s", filename);
  data->file_bytes = g_bytes_ref (file_bytes);
  data->content_type = g_strdup (content_type);
  data->room_token = g_strdup (self->room_token);
  data->caption = g_strdup (caption);
  data->reply_to = reply_to;
  data->reference_id = g_strdup (reference_id);
  data->message_type = NULL;  /* Regular file upload */
  data->collision_count = 1;

  g_debug ("MC: upload_file starting for '%s' ref=%.12s…", filename, reference_id);

  /* Pause long-polling to avoid PHP session lock contention on the server.
   * The server's long-poll holds the PHP session lock for up to 30 seconds,
   * which blocks the share POST. Window resumes polling after all uploads complete. */
  if (self->polling)
    gnalk_message_controller_pause_polling (self);

  g_signal_emit (self, signals[SIGNAL_UPLOAD_STARTED], 0, reference_id);

  start_file_upload_check_exists (data);
}

void
gnalk_message_controller_upload_voice_message (GnalkMessageController *self,
                                                 const char              *filename,
                                                 GBytes                  *file_bytes,
                                                 const char              *reference_id)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (self->room_token != NULL);
  g_return_if_fail (filename != NULL && *filename);
  g_return_if_fail (file_bytes != NULL);

  FileUploadData *data = g_new0 (FileUploadData, 1);
  g_weak_ref_init (&data->controller_ref, self);
  data->filename = g_strdup (filename);
  data->remote_path = g_strdup_printf ("/Talk/%s", filename);
  data->file_bytes = g_bytes_ref (file_bytes);
  data->content_type = g_strdup ("audio/ogg");
  data->room_token = g_strdup (self->room_token);
  data->caption = NULL;
  data->reply_to = 0;
  data->reference_id = g_strdup (reference_id);
  data->message_type = g_strdup ("voice-message");
  data->collision_count = 1;

  g_signal_emit (self, signals[SIGNAL_UPLOAD_STARTED], 0, reference_id);

  start_file_upload_check_exists (data);
}

void
gnalk_message_controller_set_session_ready (GnalkMessageController *self,
                                            gboolean                ready)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));

  self->session_ready = ready;
  if (ready && self->polling_deferred)
    {
      self->polling_deferred = FALSE;
      gnalk_message_controller_start_polling (self, self->deferred_last_known_id);
    }
}

/* ============ Context Loading (Jump-to-message) ============ */

static void
on_context_response (GnalkHttpClient *client G_GNUC_UNUSED,
                     JsonNode         *response,
                     GError           *error,
                     gpointer          user_data)
{
  GnalkAsyncOp *op = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      gnalk_async_op_free (op);
      return;
    }

  if (!gnalk_async_op_is_valid (op))
    {
      gnalk_async_op_free (op);
      return;
    }

  g_autoptr(GObject) owner = gnalk_async_op_get_owner (op);
  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (owner);
  gint64 target_id = op->context_id;
  gnalk_async_op_free (op);

  if (error)
    {
      g_warning ("Failed to get message context: %s", error->message);
      return;
    }

  JsonArray *messages = extract_messages_array (response);
  if (messages)
    {
      save_messages_to_db (self, messages, self->room_token);
      g_signal_emit (self, signals[SIGNAL_CONTEXT_LOADED], 0, messages, target_id);
    }
}

void
gnalk_message_controller_load_context (GnalkMessageController *self,
                                       gint64                  message_id)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (self->room_token != NULL);
  g_return_if_fail (message_id > 0);

  GnalkAsyncOp *op = gnalk_async_op_new (G_OBJECT (self), &self->generation,
                                          NULL, message_id);

  gnalk_http_client_get_message_context (self->http_client,
                                          self->room_token,
                                          message_id,
                                          30,
                                          on_context_response,
                                          op);
}

/* ============ Message Search ============ */

static void
on_search_messages_response (GnalkHttpClient *client G_GNUC_UNUSED,
                             JsonNode         *response,
                             GError           *error,
                             gpointer          user_data)
{
  GnalkAsyncOp *op = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      gnalk_async_op_free (op);
      return;
    }

  if (!gnalk_async_op_is_valid (op))
    {
      gnalk_async_op_free (op);
      return;
    }

  g_autoptr(GObject) owner = gnalk_async_op_get_owner (op);
  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (owner);
  gnalk_async_op_free (op);

  if (error)
    {
      g_warning ("Failed to search messages: %s", error->message);
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

  JsonArray *entries = json_object_get_array_member (data, "entries");
  if (!entries)
    return;

  /* Check for pagination: cursor is in ocs.data.cursor */
  gint cursor = (gint) json_object_get_int_member_with_default (data, "cursor", 0);
  gboolean has_more = json_object_has_member (data, "cursor") && cursor > 0;

  g_signal_emit (self, signals[SIGNAL_SEARCH_RESULTS], 0, entries, has_more, cursor);
}

void
gnalk_message_controller_search_messages (GnalkMessageController *self,
                                          const char             *search_term,
                                          gint                    cursor)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (self->room_token != NULL);
  g_return_if_fail (search_term != NULL);

  GnalkAsyncOp *op = gnalk_async_op_new (G_OBJECT (self), &self->generation,
                                          NULL, 0);

  gnalk_http_client_search_messages (self->http_client,
                                      self->room_token,
                                      search_term,
                                      20,
                                      cursor,
                                      on_search_messages_response,
                                      op);
}

/* ============ HPB Chat Relay ============ */

void
gnalk_message_controller_handle_relayed_chat (GnalkMessageController *self,
                                               JsonObject             *chat_data)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (chat_data != NULL);

  if (!self->room_token)
    return;

  /* Check if full comment is present */
  if (json_object_has_member (chat_data, "comment"))
    {
      JsonObject *comment = json_object_get_object_member (chat_data, "comment");
      if (!comment)
        return;

      /* Dedup: skip if we already have this message */
      gint64 msg_id = json_object_get_int_member_with_default (comment, "id", 0);
      if (msg_id > 0 && msg_id <= self->last_known_message_id)
        {
          g_debug ("MC: chat relay dedup - message %ld already known", (long) msg_id);
          return;
        }

      /* Wrap in a single-element array (same format as poll-response) */
      g_autoptr(JsonArray) messages = json_array_new ();
      json_array_add_object_element (messages, json_object_ref (comment));

      /* Save to DB */
      save_messages_to_db (self, messages, self->room_token);

      /* Update last known ID */
      if (msg_id > self->last_known_message_id)
        self->last_known_message_id = msg_id;

      /* Emit poll-response signal so MessageProcessor handles it identically */
      g_signal_emit (self, signals[SIGNAL_POLL_RESPONSE], 0, messages);

      /* Update conversation last message */
      if (self->repository && self->account_id > 0)
        {
          const char *message_text = json_object_get_string_member_with_default (comment, "message", "");
          const char *actor_id = json_object_get_string_member_with_default (comment, "actorId", "");
          const char *actor_name = json_object_get_string_member_with_default (comment, "actorDisplayName", "");
          const char *system_message = json_object_get_string_member_with_default (comment, "systemMessage", "");
          gint64 timestamp = json_object_get_int_member_with_default (comment, "timestamp", 0);

          const char *message_type = (system_message && *system_message) ? "system" : "comment";

          g_autofree char *params_json = NULL;
          if (json_object_has_member (comment, "messageParameters"))
            {
              JsonNode *params_node = json_object_get_member (comment, "messageParameters");
              if (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
                {
                  JsonGenerator *gen = json_generator_new ();
                  json_generator_set_root (gen, params_node);
                  params_json = json_generator_to_data (gen, NULL);
                  g_object_unref (gen);
                }
            }

          gnalk_data_repository_update_conversation_last_message (self->repository,
                                                                    self->account_id,
                                                                    self->room_token,
                                                                    timestamp,
                                                                    msg_id,
                                                                    message_text,
                                                                    actor_id,
                                                                    actor_name,
                                                                    message_type,
                                                                    params_json);
        }

      g_debug ("MC: chat relay delivered message %ld via HPB", (long) msg_id);
    }
  else if (json_object_get_boolean_member_with_default (chat_data, "refresh", FALSE))
    {
      /* No full message data - fetch via REST */
      g_debug ("MC: chat relay refresh, fetching new messages via REST");
      if (self->last_known_message_id > 0)
        gnalk_message_controller_load_new_messages (self, self->last_known_message_id);
    }
}

/* ============ Thread support ============ */

static void
on_thread_messages_response (GnalkHttpClient *client G_GNUC_UNUSED,
                             JsonNode         *response,
                             GError           *error,
                             gpointer          user_data)
{
  GnalkAsyncOp *op = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      gnalk_async_op_free (op);
      return;
    }
  if (!gnalk_async_op_is_valid (op))
    {
      gnalk_async_op_free (op);
      return;
    }

  g_autoptr(GObject) owner = gnalk_async_op_get_owner (op);
  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (owner);
  gint64 thread_id = op->context_id;
  gnalk_async_op_free (op);

  if (error)
    {
      g_warning ("Failed to load thread messages: %s", error->message);
      return;
    }

  JsonArray *messages = extract_messages_array (response);
  if (messages)
    {
      save_messages_to_db (self, messages, self->room_token);
      g_signal_emit (self, signals[SIGNAL_THREAD_MESSAGES_LOADED], 0, messages, thread_id);
    }
}

static void
on_thread_older_response (GnalkHttpClient *client G_GNUC_UNUSED,
                          JsonNode         *response,
                          GError           *error,
                          gpointer          user_data)
{
  GnalkAsyncOp *op = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      gnalk_async_op_free (op);
      return;
    }
  if (!gnalk_async_op_is_valid (op))
    {
      gnalk_async_op_free (op);
      return;
    }

  g_autoptr(GObject) owner = gnalk_async_op_get_owner (op);
  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (owner);
  gnalk_async_op_free (op);

  if (error)
    {
      g_warning ("Failed to load older thread messages: %s", error->message);
      return;
    }

  JsonArray *messages = extract_messages_array (response);
  if (messages)
    {
      save_messages_to_db (self, messages, self->room_token);
      guint len = json_array_get_length (messages);
      g_signal_emit (self, signals[SIGNAL_THREAD_OLDER_LOADED], 0,
                     messages, (gboolean) (len >= 50));
    }
}

static void
on_thread_send_response (GnalkHttpClient *client G_GNUC_UNUSED,
                         JsonNode         *response,
                         GError           *error,
                         gpointer          user_data)
{
  GnalkAsyncOp *op = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      gnalk_async_op_free (op);
      return;
    }
  if (!gnalk_async_op_is_valid (op))
    {
      gnalk_async_op_free (op);
      return;
    }

  g_autoptr(GObject) owner = gnalk_async_op_get_owner (op);
  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (owner);
  gnalk_async_op_free (op);

  if (error)
    {
      g_warning ("Thread message send failed: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_MESSAGE_SEND_FAILED], 0, "", error->message);
      return;
    }

  /* POST response: ocs.data is a single message object, not an array */
  if (response && JSON_NODE_HOLDS_OBJECT (response))
    {
      JsonObject *root = json_node_get_object (response);
      JsonObject *ocs = json_object_get_object_member (root, "ocs");
      if (ocs)
        {
          JsonObject *msg_data = json_object_get_object_member (ocs, "data");
          if (msg_data)
            {
              const char *ref = json_object_get_string_member_with_default (msg_data, "referenceId", "");
              gint64 msg_id = json_object_get_int_member_with_default (msg_data, "id", 0);
              gint64 ts = json_object_get_int_member_with_default (msg_data, "timestamp", 0);
              if (ref && *ref && msg_id > 0)
                g_signal_emit (self, signals[SIGNAL_MESSAGE_SENT], 0, ref, msg_id, ts);
            }
        }
    }
}

static void
on_create_thread_response (GnalkHttpClient *client G_GNUC_UNUSED,
                           JsonNode         *response,
                           GError           *error,
                           gpointer          user_data)
{
  GnalkAsyncOp *op = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      gnalk_async_op_free (op);
      return;
    }
  if (!gnalk_async_op_is_valid (op))
    {
      gnalk_async_op_free (op);
      return;
    }

  g_autoptr(GObject) owner = gnalk_async_op_get_owner (op);
  GnalkMessageController *self = GNALK_MESSAGE_CONTROLLER (owner);
  gnalk_async_op_free (op);

  if (error)
    {
      g_warning ("Thread creation failed: %s", error->message);
      return;
    }

  /* POST response: ocs.data is a single message object (the thread root) */
  if (response && JSON_NODE_HOLDS_OBJECT (response))
    {
      JsonObject *root = json_node_get_object (response);
      JsonObject *ocs = json_object_get_object_member (root, "ocs");
      if (ocs)
        {
          JsonObject *msg_data = json_object_get_object_member (ocs, "data");
          if (msg_data)
            {
              /* The response is the root message; its threadId field
               * holds the actual thread ID (which may differ from the
               * message id for system messages like "thread_created"). */
              gint64 thread_id = json_object_get_int_member_with_default (msg_data, "threadId", 0);
              if (thread_id <= 0)
                thread_id = json_object_get_int_member_with_default (msg_data, "id", 0);
              if (thread_id > 0)
                gnalk_message_controller_load_thread (self, thread_id);
            }
        }
    }
}

void
gnalk_message_controller_load_thread (GnalkMessageController *self,
                                      gint64                  thread_id)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (self->room_token != NULL);
  g_return_if_fail (thread_id > 0);

  GnalkAsyncOp *op = gnalk_async_op_new (G_OBJECT (self), &self->generation,
                                          NULL, thread_id);

  gnalk_http_client_get_thread_messages (self->http_client,
                                          self->room_token,
                                          thread_id,
                                          0,   /* latest messages */
                                          100, /* limit */
                                          on_thread_messages_response,
                                          op);
}

void
gnalk_message_controller_load_older_thread (GnalkMessageController *self,
                                            gint64                  thread_id,
                                            gint64                  before_id)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (self->room_token != NULL);
  g_return_if_fail (thread_id > 0);
  g_return_if_fail (before_id > 0);

  GnalkAsyncOp *op = gnalk_async_op_new (G_OBJECT (self), &self->generation,
                                          NULL, 0);

  gnalk_http_client_get_thread_messages (self->http_client,
                                          self->room_token,
                                          thread_id,
                                          before_id,
                                          50,
                                          on_thread_older_response,
                                          op);
}

void
gnalk_message_controller_send_thread_message (GnalkMessageController *self,
                                              const char             *message,
                                              gint64                  thread_id,
                                              gint64                  reply_to,
                                              const char             *reference_id)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (self->room_token != NULL);
  g_return_if_fail (message != NULL);
  g_return_if_fail (thread_id > 0);

  GnalkAsyncOp *op = gnalk_async_op_new (G_OBJECT (self), &self->generation,
                                          NULL, 0);

  gnalk_http_client_send_thread_message (self->http_client,
                                          self->room_token,
                                          message,
                                          thread_id,
                                          reply_to,
                                          reference_id,
                                          on_thread_send_response,
                                          op);
}

void
gnalk_message_controller_create_thread (GnalkMessageController *self,
                                        const char             *message,
                                        const char             *thread_title,
                                        const char             *reference_id)
{
  g_return_if_fail (GNALK_IS_MESSAGE_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (self->room_token != NULL);
  g_return_if_fail (message != NULL && *message);

  GnalkAsyncOp *op = gnalk_async_op_new (G_OBJECT (self), &self->generation,
                                          NULL, 0);

  gnalk_http_client_create_thread (self->http_client,
                                    self->room_token,
                                    message,
                                    thread_title,
                                    reference_id,
                                    on_create_thread_response,
                                    op);
}
