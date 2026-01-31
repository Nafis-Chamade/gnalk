#include "gnalk-sync-manager.h"

struct _GnalkSyncManager
{
  GObject parent_instance;

  GnalkHttpClient     *http_client;
  GnalkDatabase       *database;
  GnalkNetworkMonitor *network_monitor;
  gint64                account_id;

  gulong network_changed_handler;
  gboolean flush_in_progress;
};

G_DEFINE_FINAL_TYPE (GnalkSyncManager, gnalk_sync_manager, G_TYPE_OBJECT)

enum {
  SIGNAL_NETWORK_CHANGED,
  SIGNAL_MESSAGE_SENT,
  SIGNAL_MESSAGE_FAILED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void flush_next_pending_message (GnalkSyncManager *self);

static void
on_network_changed (GnalkNetworkMonitor *monitor,
                    gboolean              is_online,
                    GnalkSyncManager    *self)
{
  g_debug ("Sync manager: network changed, online=%d", is_online);

  /* Forward signal */
  g_signal_emit (self, signals[SIGNAL_NETWORK_CHANGED], 0, is_online);

  /* Flush pending queue when coming online */
  if (is_online && !self->flush_in_progress)
    gnalk_sync_manager_flush_pending_queue (self);
}

static void
gnalk_sync_manager_dispose (GObject *object)
{
  GnalkSyncManager *self = GNALK_SYNC_MANAGER (object);

  if (self->network_monitor && self->network_changed_handler > 0)
    {
      g_signal_handler_disconnect (self->network_monitor, self->network_changed_handler);
      self->network_changed_handler = 0;
    }

  g_clear_object (&self->http_client);
  g_clear_object (&self->database);
  g_clear_object (&self->network_monitor);

  G_OBJECT_CLASS (gnalk_sync_manager_parent_class)->dispose (object);
}

static void
gnalk_sync_manager_class_init (GnalkSyncManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gnalk_sync_manager_dispose;

  signals[SIGNAL_NETWORK_CHANGED] =
    g_signal_new ("network-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  signals[SIGNAL_MESSAGE_SENT] =
    g_signal_new ("message-sent",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT64);

  signals[SIGNAL_MESSAGE_FAILED] =
    g_signal_new ("message-failed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
}

static void
gnalk_sync_manager_init (GnalkSyncManager *self)
{
  self->flush_in_progress = FALSE;
}

GnalkSyncManager *
gnalk_sync_manager_new (GnalkHttpClient     *http_client,
                          GnalkDatabase       *database,
                          GnalkNetworkMonitor *network_monitor,
                          gint64                account_id)
{
  GnalkSyncManager *self = g_object_new (GNALK_TYPE_SYNC_MANAGER, NULL);

  self->http_client = http_client ? g_object_ref (http_client) : NULL;
  self->database = database ? g_object_ref (database) : NULL;
  self->network_monitor = network_monitor ? g_object_ref (network_monitor) : NULL;
  self->account_id = account_id;

  if (self->network_monitor)
    {
      self->network_changed_handler =
        g_signal_connect (self->network_monitor, "network-changed",
                          G_CALLBACK (on_network_changed), self);
    }

  return self;
}

void
gnalk_sync_manager_set_account_id (GnalkSyncManager *self,
                                     gint64             account_id)
{
  g_return_if_fail (GNALK_IS_SYNC_MANAGER (self));
  self->account_id = account_id;
}

gboolean
gnalk_sync_manager_is_online (GnalkSyncManager *self)
{
  g_return_val_if_fail (GNALK_IS_SYNC_MANAGER (self), FALSE);

  if (self->network_monitor)
    return gnalk_network_monitor_is_online (self->network_monitor);

  return TRUE;  /* Assume online if no monitor */
}

void
gnalk_sync_manager_queue_message (GnalkSyncManager *self,
                                    const char        *conversation_token,
                                    const char        *message,
                                    gint64             reply_to,
                                    const char        *reference_id)
{
  g_return_if_fail (GNALK_IS_SYNC_MANAGER (self));
  g_return_if_fail (conversation_token != NULL);
  g_return_if_fail (reference_id != NULL);

  if (!self->database || self->account_id <= 0)
    {
      g_warning ("SyncManager: Cannot queue message %s (database=%p, account_id=%ld)",
                 reference_id, (void *) self->database, (long) self->account_id);
      return;
    }

  gnalk_database_queue_pending_message (self->database, self->account_id,
                                          conversation_token, message,
                                          reply_to, reference_id);

  g_debug ("SyncManager: Queued pending message %s for %s",
           reference_id, conversation_token);
}

void
gnalk_sync_manager_queue_file (GnalkSyncManager *self,
                                 const char        *conversation_token,
                                 const char        *file_path,
                                 const char        *file_name,
                                 const char        *content_type,
                                 const char        *caption,
                                 gint64             reply_to,
                                 const char        *reference_id)
{
  g_return_if_fail (GNALK_IS_SYNC_MANAGER (self));
  g_return_if_fail (conversation_token != NULL);
  g_return_if_fail (reference_id != NULL);

  if (!self->database || self->account_id <= 0)
    {
      g_warning ("SyncManager: Cannot queue file %s (database=%p, account_id=%ld)",
                 reference_id, (void *) self->database, (long) self->account_id);
      return;
    }

  gnalk_database_queue_pending_file (self->database, self->account_id,
                                       conversation_token, file_path,
                                       file_name, content_type, caption,
                                       reply_to, reference_id);

  g_debug ("SyncManager: Queued pending file %s (%s) for %s",
           reference_id, file_name, conversation_token);
}

/* Data for pending message send callback */
typedef struct {
  GnalkSyncManager *manager;
  char              *reference_id;
  char              *conversation_token;
} PendingSendData;

static void
pending_send_data_free (PendingSendData *data)
{
  g_free (data->reference_id);
  g_free (data->conversation_token);
  g_slice_free (PendingSendData, data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PendingSendData, pending_send_data_free)

static void
on_pending_message_sent (GnalkHttpClient *client,
                          JsonNode         *response,
                          GError           *error,
                          gpointer          user_data)
{
  PendingSendData *data = user_data;
  GnalkSyncManager *self = data->manager;

  if (error)
    {
      g_warning ("Failed to send pending message %s: %s",
                 data->reference_id, error->message);

      /* Update retry count and error in DB */
      if (self->database)
        gnalk_database_update_pending_error (self->database,
                                               data->reference_id,
                                               error->message);

      /* Emit failed signal */
      g_signal_emit (self, signals[SIGNAL_MESSAGE_FAILED], 0,
                     data->reference_id, error->message);
    }
  else
    {
      /* Success - extract message ID from response */
      gint64 message_id = 0;

      if (response && JSON_NODE_HOLDS_OBJECT (response))
        {
          JsonObject *root = json_node_get_object (response);
          JsonObject *ocs = json_object_get_object_member (root, "ocs");
          if (ocs)
            {
              JsonObject *msg_data = json_object_get_object_member (ocs, "data");
              if (msg_data)
                message_id = json_object_get_int_member_with_default (msg_data, "id", 0);
            }
        }

      g_debug ("Pending message %s sent successfully, id=%ld",
               data->reference_id, message_id);

      /* Remove from pending queue */
      if (self->database)
        gnalk_database_remove_pending_message (self->database, data->reference_id);

      /* Emit sent signal */
      g_signal_emit (self, signals[SIGNAL_MESSAGE_SENT], 0,
                     data->reference_id, message_id);
    }

  pending_send_data_free (data);

  /* Continue flushing queue */
  flush_next_pending_message (self);
}

static void
flush_next_pending_message (GnalkSyncManager *self)
{
  if (!self->database || self->account_id <= 0 || !self->http_client)
    {
      self->flush_in_progress = FALSE;
      return;
    }

  /* Check if still online */
  if (!gnalk_sync_manager_is_online (self))
    {
      g_debug ("Went offline during flush, stopping");
      self->flush_in_progress = FALSE;
      return;
    }

  /* Get pending messages */
  GPtrArray *pending = gnalk_database_get_pending_messages (self->database,
                                                              self->account_id);
  if (!pending || pending->len == 0)
    {
      g_debug ("No more pending messages to flush");
      self->flush_in_progress = FALSE;
      if (pending)
        g_ptr_array_unref (pending);
      return;
    }

  /* Get first pending message */
  GnalkPendingMessage *pm = g_ptr_array_index (pending, 0);

  /* Skip if too many retries */
  if (pm->retry_count >= 3)
    {
      g_warning ("Pending message %s exceeded retry limit, marking as failed",
                 pm->reference_id);
      g_signal_emit (self, signals[SIGNAL_MESSAGE_FAILED], 0,
                     pm->reference_id, "Exceeded retry limit");
      gnalk_database_remove_pending_message (self->database, pm->reference_id);
      g_ptr_array_unref (pending);
      /* Continue with next message */
      flush_next_pending_message (self);
      return;
    }

  /* Prepare callback data */
  PendingSendData *data = g_slice_new0 (PendingSendData);
  data->manager = self;
  data->reference_id = g_strdup (pm->reference_id);
  data->conversation_token = g_strdup (pm->conversation_token);

  g_debug ("Sending pending message %s to room %s",
           pm->reference_id, pm->conversation_token);

  /* Send message or file */
  if (pm->file_path && *pm->file_path)
    {
      /* File uploads are complex (multi-step), skip for now and mark as failed */
      g_warning ("Pending file uploads not yet supported, skipping %s", pm->reference_id);
      g_signal_emit (self, signals[SIGNAL_MESSAGE_FAILED], 0,
                     pm->reference_id, "File uploads not supported in offline mode");
      gnalk_database_remove_pending_message (self->database, pm->reference_id);
      pending_send_data_free (data);
      g_ptr_array_unref (pending);
      flush_next_pending_message (self);
      return;
    }
  else
    {
      /* It's a text message */
      gnalk_http_client_send_message (self->http_client,
                                        pm->conversation_token,
                                        pm->message,
                                        pm->reply_to,
                                        pm->reference_id,
                                        on_pending_message_sent,
                                        data);
    }

  g_ptr_array_unref (pending);
}

void
gnalk_sync_manager_flush_pending_queue (GnalkSyncManager *self)
{
  g_return_if_fail (GNALK_IS_SYNC_MANAGER (self));

  if (self->flush_in_progress)
    {
      g_debug ("Flush already in progress");
      return;
    }

  if (!gnalk_sync_manager_is_online (self))
    {
      g_debug ("Cannot flush pending queue - offline");
      return;
    }

  g_debug ("Starting pending message queue flush");
  self->flush_in_progress = TRUE;
  flush_next_pending_message (self);
}
