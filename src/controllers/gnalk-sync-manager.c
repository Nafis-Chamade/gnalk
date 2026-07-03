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
  guint    flush_timer_id;   /* Backoff timer between flush passes */
};

G_DEFINE_FINAL_TYPE (GnalkSyncManager, gnalk_sync_manager, G_TYPE_OBJECT)

/* A message that has failed this many times is kept in the DB (never deleted)
 * but no longer auto-retried — it stays for manual retry. */
#define GNALK_SYNC_MAX_RETRIES  3

/* Exponential-backoff bounds between flush passes (seconds). */
#define SYNC_FLUSH_BASE_SEC  2
#define SYNC_FLUSH_CAP_SEC  60

/* Jittered exponential backoff (seconds): MIN(base * 2^attempt, cap) + jitter. */
static guint
backoff_delay_seconds (guint attempt, guint base, guint cap)
{
  guint delay = base;
  for (guint i = 0; i < attempt && delay < cap; i++)
    delay *= 2;
  if (delay > cap)
    delay = cap;
  /* Up to ~25% jitter to de-synchronise retries (thundering herd). */
  return delay + (guint) g_random_int_range (0, (gint) (delay / 4) + 1);
}

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

  if (self->flush_timer_id > 0)
    {
      g_source_remove (self->flush_timer_id);
      self->flush_timer_id = 0;
    }

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
                                          reply_to, reference_id, 0);

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
  GWeakRef          manager_ref;  /* Weak ref to owner (use-after-free safe) */
  char              *reference_id;
  char              *conversation_token;
  int               retry_count;  /* Attempts already made for this message (for backoff) */
} PendingSendData;

static void
pending_send_data_free (PendingSendData *data)
{
  g_weak_ref_clear (&data->manager_ref);
  g_free (data->reference_id);
  g_free (data->conversation_token);
  g_slice_free (PendingSendData, data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PendingSendData, pending_send_data_free)

static gboolean
flush_retry_timeout (gpointer user_data)
{
  GnalkSyncManager *self = user_data;
  self->flush_timer_id = 0;
  flush_next_pending_message (self);
  return G_SOURCE_REMOVE;
}

/* Wait (jittered backoff) before the next flush pass instead of hammering the
 * server. Keeps flush_in_progress TRUE so no parallel flush starts meanwhile. */
static void
schedule_flush_retry (GnalkSyncManager *self, guint attempt)
{
  if (self->flush_timer_id != 0)
    return;

  guint delay = backoff_delay_seconds (attempt, SYNC_FLUSH_BASE_SEC, SYNC_FLUSH_CAP_SEC);
  g_debug ("SyncManager: backing off %us before next flush pass", delay);
  self->flush_timer_id = g_timeout_add_seconds (delay, flush_retry_timeout, self);
}

static void
on_pending_message_sent (GnalkHttpClient *client,
                          JsonNode         *response,
                          GError           *error,
                          gpointer          user_data)
{
  PendingSendData *data = user_data;
  /* Owner may have been destroyed while the request was in flight. */
  GnalkSyncManager *self = g_weak_ref_get (&data->manager_ref);

  /* Cancelled (disconnect/account switch): keep the message queued without
   * consuming a retry, and stop the flush — it restarts on reconnect. */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      pending_send_data_free (data);
      if (self)
        {
          self->flush_in_progress = FALSE;
          g_object_unref (self);  /* balance g_weak_ref_get */
        }
      return;
    }

  if (!self)
    {
      pending_send_data_free (data);
      return;
    }

  gboolean send_failed = (error != NULL);
  guint    failed_attempts = (guint) data->retry_count;

  if (error)
    {
      g_warning ("Failed to send pending message %s: %s",
                 data->reference_id, error->message);

      /* Update retry count and error in DB. The message row is preserved —
       * it is never deleted on failure, so it can be retried manually. */
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

  /* Continue flushing. On failure, back off before the next pass so a server
   * outage isn't hammered; on success, proceed immediately. */
  if (send_failed)
    schedule_flush_retry (self, failed_attempts);
  else
    flush_next_pending_message (self);

  g_object_unref (self);  /* balance g_weak_ref_get */
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

  /* Select the first message we can actually send. Messages that have exhausted
   * their retry budget are NEVER deleted — they stay in the DB (marked with a
   * retry count / last_error) for manual retry, and we simply skip past them.
   * File uploads aren't supported in offline flush yet, so they are likewise
   * left queued rather than dropped. */
  GnalkPendingMessage *pm = NULL;
  for (guint i = 0; i < pending->len; i++)
    {
      GnalkPendingMessage *cand = g_ptr_array_index (pending, i);

      if (cand->retry_count >= GNALK_SYNC_MAX_RETRIES)
        continue;  /* exhausted — keep for manual retry, don't auto-send */

      if (cand->file_path && *cand->file_path)
        continue;  /* file uploads not supported offline — keep queued */

      pm = cand;
      break;
    }

  if (!pm)
    {
      g_debug ("No more sendable pending messages "
               "(failed/file messages kept for manual retry)");
      self->flush_in_progress = FALSE;
      g_ptr_array_unref (pending);
      return;
    }

  /* Prepare callback data */
  PendingSendData *data = g_slice_new0 (PendingSendData);
  g_weak_ref_init (&data->manager_ref, self);
  data->reference_id = g_strdup (pm->reference_id);
  data->conversation_token = g_strdup (pm->conversation_token);
  data->retry_count = pm->retry_count;

  g_debug ("Sending pending message %s to room %s (attempt %d)",
           pm->reference_id, pm->conversation_token, pm->retry_count + 1);

  /* It's a text message */
  gnalk_http_client_send_message (self->http_client,
                                    pm->conversation_token,
                                    pm->message,
                                    pm->reply_to,
                                    pm->reference_id,
                                    on_pending_message_sent,
                                    data);

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
