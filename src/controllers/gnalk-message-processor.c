/* gnalk-message-processor.c
 *
 * Message data processing controller.
 * Handles JSON parsing, GListStore manipulation, DB save/load,
 * date separators, unread tracking. Emits signals for UI actions.
 *
 * NO GTK widget dependencies - only GObject, GListStore, json-glib, DB.
 */

#include "gnalk-message-processor.h"
#include "gnalk-database.h"
#include "gnalk-message-data.h"
#include "gnalk-message-controller.h"
#include "gnalk-room-state.h"
#include "gnalk-conversation-data.h"
#include "gnalk-log.h"
#include <json-glib/json-glib.h>
#include <glib/gi18n.h>

struct _GnalkMessageProcessor
{
  GObject parent_instance;

  /* Dependencies (not owned) */
  GnalkDatabase          *database;
  GnalkMessageController *message_controller;
  GListStore             *conversations_store;
  GnalkRoomState         *current_room;

  /* State */
  gint64    account_id;
  char     *current_user_id;
  gboolean  loading_older;

  /* Indexes for O(1) lookups (non-owning values, cleared on room change) */
  GHashTable *msg_index;  /* gint64* (msg_id) → GnalkMessageData* */
  GHashTable *ref_index;  /* const char* (reference_id) → GnalkMessageData* */
};

G_DEFINE_FINAL_TYPE (GnalkMessageProcessor, gnalk_message_processor, G_TYPE_OBJECT)

/* Forward declarations for index helpers */
static void msg_index_add   (GnalkMessageProcessor *self, GnalkMessageData *data);
static void msg_index_clear (GnalkMessageProcessor *self);

enum {
  SIGNAL_BATCH_INSERT_BEGIN,
  SIGNAL_BATCH_INSERT_END,
  SIGNAL_SCROLL_TO_BOTTOM,
  SIGNAL_SCROLL_TO_ITEM,
  SIGNAL_CALL_STATE_LEFT,
  SIGNAL_CALL_ENDED,
  SIGNAL_LOADING_OLDER_CHANGED,
  SIGNAL_STATE_SYNCED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ============ GObject lifecycle ============ */

static void
gnalk_message_processor_finalize (GObject *object)
{
  GnalkMessageProcessor *self = GNALK_MESSAGE_PROCESSOR (object);
  g_clear_pointer (&self->msg_index, g_hash_table_unref);
  g_clear_pointer (&self->ref_index, g_hash_table_unref);
  g_free (self->current_user_id);
  G_OBJECT_CLASS (gnalk_message_processor_parent_class)->finalize (object);
}

static void
gnalk_message_processor_class_init (GnalkMessageProcessorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gnalk_message_processor_finalize;

  signals[SIGNAL_BATCH_INSERT_BEGIN] =
    g_signal_new ("batch-insert-begin",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_BATCH_INSERT_END] =
    g_signal_new ("batch-insert-end",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_SCROLL_TO_BOTTOM] =
    g_signal_new ("scroll-to-bottom",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_SCROLL_TO_ITEM] =
    g_signal_new ("scroll-to-item",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_UINT);

  signals[SIGNAL_CALL_STATE_LEFT] =
    g_signal_new ("call-state-left",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_CALL_ENDED] =
    g_signal_new ("call-ended",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_LOADING_OLDER_CHANGED] =
    g_signal_new ("loading-older-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  signals[SIGNAL_STATE_SYNCED] =
    g_signal_new ("state-synced",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_UINT);
}

static void
gnalk_message_processor_init (GnalkMessageProcessor *self)
{
  self->msg_index = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
  self->ref_index = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

/* ============ Constructor ============ */

GnalkMessageProcessor *
gnalk_message_processor_new (void)
{
  return g_object_new (GNALK_TYPE_MESSAGE_PROCESSOR, NULL);
}

/* ============ Dependency setters ============ */

void
gnalk_message_processor_set_database (GnalkMessageProcessor *self,
                                       GnalkDatabase          *db)
{
  self->database = db;
}

void
gnalk_message_processor_set_account_id (GnalkMessageProcessor *self,
                                         gint64                  account_id)
{
  self->account_id = account_id;
}

void
gnalk_message_processor_set_current_user (GnalkMessageProcessor *self,
                                            const char             *user_id)
{
  g_free (self->current_user_id);
  self->current_user_id = g_strdup (user_id);
}

void
gnalk_message_processor_set_room_state (GnalkMessageProcessor *self,
                                          GnalkRoomState         *state)
{
  if (self->current_room != state)
    msg_index_clear (self);
  self->current_room = state;
}

void
gnalk_message_processor_set_message_controller (GnalkMessageProcessor  *self,
                                                  GnalkMessageController *mc)
{
  self->message_controller = mc;
}

void
gnalk_message_processor_set_conversations_store (GnalkMessageProcessor *self,
                                                   GListStore             *store)
{
  self->conversations_store = store;
}

/* ============ State queries ============ */

gboolean
gnalk_message_processor_get_loading_older (GnalkMessageProcessor *self)
{
  return self->loading_older;
}

/* ============ Message sorting ============ */

gint
gnalk_message_processor_compare_by_id (gconstpointer a,
                                        gconstpointer b,
                                        gpointer      user_data)
{
  (void) user_data;
  GnalkMessageData *msg_a = GNALK_MESSAGE_DATA ((gpointer) a);
  GnalkMessageData *msg_b = GNALK_MESSAGE_DATA ((gpointer) b);

  gint64 id_a = gnalk_message_data_get_id (msg_a);
  gint64 id_b = gnalk_message_data_get_id (msg_b);

  /* Pending messages (ID=0) go at the end */
  if (id_a == 0 && id_b == 0)
    return 0;
  if (id_a == 0)
    return 1;
  if (id_b == 0)
    return -1;

  if (id_a < id_b)
    return -1;
  if (id_a > id_b)
    return 1;
  return 0;
}

/* ============ Index helpers ============ */

static void
msg_index_add (GnalkMessageProcessor *self, GnalkMessageData *data)
{
  gint64 id = gnalk_message_data_get_id (data);
  if (id > 0)
    {
      gint64 *key = g_new (gint64, 1);
      *key = id;
      g_hash_table_insert (self->msg_index, key, data);
    }

  const char *ref_id = gnalk_message_data_get_reference_id (data);
  if (ref_id && *ref_id)
    g_hash_table_insert (self->ref_index, g_strdup (ref_id), data);
}

static void
msg_index_clear (GnalkMessageProcessor *self)
{
  g_hash_table_remove_all (self->msg_index);
  g_hash_table_remove_all (self->ref_index);
}

void
gnalk_message_processor_index_message (GnalkMessageProcessor *self,
                                        GnalkMessageData      *data)
{
  g_return_if_fail (GNALK_IS_MESSAGE_PROCESSOR (self));
  g_return_if_fail (data != NULL);
  msg_index_add (self, data);
}

/* ============ Date separators and timestamps ============ */

void
gnalk_message_processor_update_separators (GnalkMessageProcessor *self)
{
  if (!self->current_room)
    return;

  GListStore *store = self->current_room->messages_store;
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
  GNALK_TRACE_UI ("update_date_separators_and_timestamps: processing %u messages", n_items);

  int prev_year = 0, prev_month = 0, prev_day = 0;
  const char *prev_author = NULL;
  gboolean prev_is_system = TRUE;  /* Treat "before first message" as system → first msg shows avatar */

  for (guint j = 0; j < n_items; j++)
    {
      GnalkMessageData *msg_data = g_list_model_get_item (G_LIST_MODEL (store), j);
      int year, month, day;
      gnalk_message_data_get_date (msg_data, &year, &month, &day);

      if (j == 0)
        gnalk_message_data_set_show_date_header (msg_data, TRUE);
      else
        {
          gboolean date_changed = (year != prev_year || month != prev_month || day != prev_day);
          gnalk_message_data_set_show_date_header (msg_data, date_changed);
        }

      /* Compute timestamp visibility */
      const char *curr_system = gnalk_message_data_get_system_message (msg_data);
      gboolean is_system = curr_system && *curr_system;

      /* Compute avatar grouping: show avatar for first message in a group */
      const char *curr_author = gnalk_message_data_get_author_id (msg_data);
      gboolean show_avatar = TRUE;
      if (!is_system && !prev_is_system &&
          g_strcmp0 (curr_author, prev_author) == 0)
        show_avatar = FALSE;
      gnalk_message_data_set_show_avatar (msg_data, show_avatar);
      prev_author = curr_author;  /* interned or stable within iteration */
      prev_is_system = is_system;
      gboolean show_timestamp = TRUE;

      if (!is_system && j + 1 < n_items)
        {
          GnalkMessageData *next = g_list_model_get_item (G_LIST_MODEL (store), j + 1);
          if (next)
            {
              const char *curr_author = gnalk_message_data_get_author_id (msg_data);
              const char *next_author = gnalk_message_data_get_author_id (next);
              const char *next_system = gnalk_message_data_get_system_message (next);
              gint64 curr_ts = gnalk_message_data_get_timestamp (msg_data);
              gint64 next_ts = gnalk_message_data_get_timestamp (next);
              JsonObject *curr_reactions = gnalk_message_data_get_reactions (msg_data);

              gboolean same_author = g_strcmp0 (curr_author, next_author) == 0;
              gboolean next_not_system = !next_system || !*next_system;
              gboolean same_minute = (curr_ts / 60) == (next_ts / 60);
              gboolean no_reactions = !curr_reactions || json_object_get_size (curr_reactions) == 0;

              if (same_author && next_not_system && same_minute && no_reactions)
                show_timestamp = FALSE;

              g_object_unref (next);
            }
        }
      gnalk_message_data_set_show_timestamp (msg_data, show_timestamp);

      prev_year = year;
      prev_month = month;
      prev_day = day;
      g_object_unref (msg_data);
    }
}

/* ============ Update is_own flags ============ */

void
gnalk_message_processor_update_is_own (GnalkMessageProcessor *self)
{
  if (!self->current_room)
    return;

  GListStore *store = self->current_room->messages_store;
  const char *current_user = self->current_user_id;

  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *data = g_list_model_get_item (G_LIST_MODEL (store), i);
      if (data)
        {
          const char *author_id = gnalk_message_data_get_author_id (data);
          gboolean is_own = current_user && g_strcmp0 (author_id, current_user) == 0;
          gnalk_message_data_set_is_own (data, is_own);
          g_object_unref (data);
        }
    }
  g_debug ("Updated is_own flags for %u messages (current_user=%s)",
           n_items, current_user ? current_user : "(null)");
}

/* ============ Save messages to database ============ */

typedef struct {
  GnalkDatabase *db;
  gint64          account_id;
  char           *room_token;
  GPtrArray      *messages;
} SaveMessagesData;

static void
save_messages_data_free (SaveMessagesData *data)
{
  g_free (data->room_token);
  g_ptr_array_free (data->messages, TRUE);
  g_slice_free (SaveMessagesData, data);
}

static void
save_messages_thread (GTask        *task,
                      gpointer      source_object G_GNUC_UNUSED,
                      gpointer      task_data,
                      GCancellable *cancellable G_GNUC_UNUSED)
{
  SaveMessagesData *data = task_data;

  for (guint i = 0; i < data->messages->len; i++)
    {
      GnalkMessageData *msg = g_ptr_array_index (data->messages, i);
      if (msg && gnalk_message_data_get_id (msg) > 0)
        gnalk_database_save_message (data->db, data->account_id, data->room_token, msg);
    }

  g_task_return_boolean (task, TRUE);
}

void
gnalk_message_processor_save_to_db (GnalkMessageProcessor *self)
{
  if (!self->database || self->account_id <= 0 || !self->current_room)
    return;

  GListStore *store = self->current_room->messages_store;
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
  if (n_items == 0)
    return;

  GPtrArray *messages = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *data = g_list_model_get_item (G_LIST_MODEL (store), i);
      if (data)
        g_ptr_array_add (messages, data);  /* Takes ownership */
    }

  SaveMessagesData *data = g_slice_new (SaveMessagesData);
  data->db = self->database;
  data->account_id = self->account_id;
  data->room_token = g_strdup (self->current_room->room_token);
  data->messages = messages;

  GTask *task = g_task_new (self, NULL, NULL, NULL);
  g_task_set_task_data (task, data, (GDestroyNotify) save_messages_data_free);
  g_task_run_in_thread (task, save_messages_thread);
  g_object_unref (task);
}

/* ============ Load messages from database ============ */

typedef struct {
  GnalkDatabase *db;
  gint64          account_id;
  char           *room_token;
} LoadMessagesData;

static void
load_messages_data_free (LoadMessagesData *data)
{
  g_free (data->room_token);
  g_slice_free (LoadMessagesData, data);
}

static void
load_messages_thread (GTask        *task,
                       gpointer      source_object G_GNUC_UNUSED,
                       gpointer      task_data,
                       GCancellable *cancellable G_GNUC_UNUSED)
{
  LoadMessagesData *data = task_data;

  GListStore *cached = gnalk_database_load_messages (data->db, data->account_id,
                                                       data->room_token, 15, 0);
  g_task_return_pointer (task, cached, g_object_unref);
}

static void
load_messages_from_db_finish (GObject      *source_object,
                               GAsyncResult *result,
                               gpointer      user_data G_GNUC_UNUSED)
{
  GnalkMessageProcessor *self = GNALK_MESSAGE_PROCESSOR (source_object);
  GTask *task = G_TASK (result);

  GNALK_TRACE_DATABASE ("load_messages_from_db_finish: callback started");
  g_message ("load_messages_from_db_finish: starting");

  /* Check if room changed while loading */
  const char *expected_token = g_task_get_name (task);
  if (!self->current_room || g_strcmp0 (self->current_room->room_token, expected_token) != 0)
    {
      GNALK_TRACE_DATABASE ("load_messages_from_db_finish: room changed (expected=%s, current=%s), discarding",
                             expected_token, self->current_room ? self->current_room->room_token : "(null)");
      g_message ("load_messages_from_db_finish: room changed, discarding");
      return;
    }

  GListStore *cached = g_task_propagate_pointer (task, NULL);
  if (!cached)
    {
      GNALK_TRACE_DATABASE ("load_messages_from_db_finish: no cached data returned");
      g_message ("load_messages_from_db_finish: no cached data");
      return;
    }

  guint n_cached = g_list_model_get_n_items (G_LIST_MODEL (cached));
  GNALK_TRACE_DATABASE ("load_messages_from_db_finish: got %u messages from cache", n_cached);
  g_message ("load_messages_from_db_finish: got %u messages from cache", n_cached);

  if (n_cached == 0)
    {
      g_object_unref (cached);
      GNALK_TRACE_DATABASE ("load_messages_from_db_finish: cache empty, triggering server fetch");
      g_message ("load_messages_from_db_finish: cache empty, triggering server fetch");
      if (self->message_controller)
        gnalk_message_controller_load_messages (self->message_controller);
      return;
    }

  const char *current_user = self->current_user_id;

  /* If last_read_message_id is not set, look it up from conversation data */
  if (self->current_room->last_read_message_id == 0 && self->conversations_store)
    {
      guint n_convos = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
      for (guint i = 0; i < n_convos; i++)
        {
          GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
          const char *conv_token = gnalk_conversation_data_get_token (conv);
          if (conv_token && g_strcmp0 (conv_token, self->current_room->room_token) == 0)
            {
              self->current_room->last_read_message_id = gnalk_conversation_data_get_last_read_message (conv);
              g_object_unref (conv);
              break;
            }
          g_object_unref (conv);
        }
    }

  /* Reset first_unread_message_id before populating */
  self->current_room->first_unread_message_id = 0;

  GListStore *store = self->current_room->messages_store;
  GNALK_TRACE_STORE ("load_messages_from_db_finish: populating store with %u messages", n_cached);

  /* Disconnect model from view to prevent visual updates during batch insert */
  g_signal_emit (self, signals[SIGNAL_BATCH_INSERT_BEGIN], 0);
  GNALK_TRACE_UI ("load_messages_from_db_finish: emitted batch-insert-begin");

  /* Populate store on main thread and mark unread messages */
  guint added_count = 0;
  guint skipped_count = 0;
  for (guint i = 0; i < n_cached; i++)
    {
      GnalkMessageData *data = g_list_model_get_item (G_LIST_MODEL (cached), i);
      if (data)
        {
          /* Skip transient system messages that were saved to DB by MessageController
           * but should not be displayed (they modify other messages instead) */
          const char *sys_msg = gnalk_message_data_get_system_message (data);
          if (sys_msg && *sys_msg &&
              (g_strcmp0 (sys_msg, "reaction") == 0 ||
               g_strcmp0 (sys_msg, "message_edited") == 0 ||
               g_strcmp0 (sys_msg, "message_deleted") == 0))
            {
              skipped_count++;
              g_clear_object (&data);
              continue;
            }

          gint64 msg_id = gnalk_message_data_get_id (data);

          /* Check if message already exists via index (O(1)) */
          gboolean already_exists = (msg_id > 0 &&
                                     g_hash_table_contains (self->msg_index, &msg_id));

          if (!already_exists)
            {
              /* Recalculate is_own based on current session's user_id */
              const char *author_id = gnalk_message_data_get_author_id (data);
              gboolean is_own = current_user && g_strcmp0 (author_id, current_user) == 0;
              gnalk_message_data_set_is_own (data, is_own);

              /* Mark as unread if message ID > last_read_message_id and not our own message */
              if (self->current_room->last_read_message_id > 0)
                {
                  if (msg_id > self->current_room->last_read_message_id && !is_own)
                    {
                      gnalk_message_data_set_is_unread (data, TRUE);

                      if (self->current_room->first_unread_message_id == 0 ||
                          msg_id < self->current_room->first_unread_message_id)
                        {
                          self->current_room->first_unread_message_id = msg_id;
                        }
                    }
                }

              g_list_store_insert_sorted (store, data,
                                           gnalk_message_processor_compare_by_id, NULL);
              msg_index_add (self, data);
              added_count++;
            }
          else
            {
              skipped_count++;
            }
        }
      g_clear_object (&data);
    }

  GNALK_TRACE_STORE ("load_messages_from_db_finish: added %u messages, skipped %u duplicates",
                       added_count, skipped_count);

  /* Update date separators and timestamp visibility */
  gnalk_message_processor_update_separators (self);

  /* Reconnect model to view - items will appear all at once */
  g_signal_emit (self, signals[SIGNAL_BATCH_INSERT_END], 0);
  GNALK_TRACE_UI ("load_messages_from_db_finish: emitted batch-insert-end with %u items", added_count);

  g_object_unref (cached);

  /* Track oldest loaded message for lazy loading */
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
  GNALK_TRACE_MESSAGES ("load_messages_from_db_finish: store now has %u messages total", n_items);

  if (n_items > 0)
    {
      GnalkMessageData *oldest = g_list_model_get_item (G_LIST_MODEL (store), 0);
      if (oldest)
        {
          self->current_room->oldest_loaded_id = gnalk_message_data_get_id (oldest);
          g_object_unref (oldest);
        }

      GnalkMessageData *newest = g_list_model_get_item (G_LIST_MODEL (store), n_items - 1);
      if (newest)
        {
          self->current_room->last_known_message_id = gnalk_message_data_get_id (newest);
          g_object_unref (newest);
        }

      GNALK_TRACE_MESSAGES ("load_messages_from_db_finish: oldest_id=%"G_GINT64_FORMAT", newest_id=%"G_GINT64_FORMAT,
                             self->current_room->oldest_loaded_id, self->current_room->last_known_message_id);
    }

  g_debug ("Loaded %u messages from database for conversation %s (last_known=%"G_GINT64_FORMAT")",
           n_cached, expected_token, self->current_room->last_known_message_id);

  g_message ("Messages displayed: %u (from cache)", n_items);
  GNALK_TRACE_SCROLL ("load_messages_from_db_finish: showing view at bottom (%u items)", n_items);
  self->current_room->scroll_to_bottom = TRUE;

  /* Tell UI to show messages (UI decides show_at_bottom vs show_messages based on count) */
  g_signal_emit (self, signals[SIGNAL_STATE_SYNCED], 0, (guint) GNALK_PROCESSOR_SYNC_DB_LOADED);

  /* Start message polling for new messages (cache-first: displayed cache, now poll for updates) */
  if (self->message_controller && !self->current_room->initial_load_done)
    {
      GNALK_TRACE_POLLING ("load_messages_from_db_finish: starting polling from message %"G_GINT64_FORMAT,
                            self->current_room->last_known_message_id);
      self->current_room->initial_load_done = TRUE;
      gnalk_message_controller_start_polling (self->message_controller,
                                                self->current_room->last_known_message_id);
    }
  else
    {
      GNALK_TRACE_POLLING ("load_messages_from_db_finish: polling already started, skipping");
    }
}

void
gnalk_message_processor_load_from_db (GnalkMessageProcessor *self,
                                        const char             *room_token)
{
  GNALK_TRACE_DATABASE ("load_from_db: starting for room %s", room_token);
  g_message ("load_from_db: starting for room %s", room_token);

  if (!self->database || self->account_id <= 0)
    {
      g_message ("load_from_db: no db or invalid account_id, returning");
      return;
    }

  LoadMessagesData *data = g_slice_new (LoadMessagesData);
  data->db = self->database;
  data->account_id = self->account_id;
  data->room_token = g_strdup (room_token);

  GTask *task = g_task_new (self, NULL, load_messages_from_db_finish, NULL);
  g_task_set_name (task, room_token);
  g_task_set_task_data (task, data, (GDestroyNotify) load_messages_data_free);
  g_task_run_in_thread (task, load_messages_thread);
  g_object_unref (task);
}

/* ============ Load older messages ============ */

typedef struct {
  GnalkDatabase *db;
  gint64          account_id;
  char           *room_token;
  gint64          before_id;
  guint           generation;
} LoadOlderMessagesData;

static void
load_older_messages_data_free (LoadOlderMessagesData *data)
{
  g_free (data->room_token);
  g_slice_free (LoadOlderMessagesData, data);
}

static void
load_older_messages_thread (GTask        *task,
                             gpointer      source_object G_GNUC_UNUSED,
                             gpointer      task_data,
                             GCancellable *cancellable G_GNUC_UNUSED)
{
  LoadOlderMessagesData *data = task_data;

  GListStore *older = gnalk_database_load_messages (data->db, data->account_id,
                                                      data->room_token, 20, data->before_id);
  g_task_return_pointer (task, older, g_object_unref);
}

static void
load_older_messages_from_db_finish (GObject      *source_object,
                                     GAsyncResult *result,
                                     gpointer      user_data)
{
  (void) user_data;
  GnalkMessageProcessor *self = GNALK_MESSAGE_PROCESSOR (source_object);
  GTask *task = G_TASK (result);
  LoadOlderMessagesData *data = g_task_get_task_data (task);

  /* Check if room changed while loading */
  if (!self->current_room ||
      data->generation != self->current_room->message_load_generation ||
      g_strcmp0 (self->current_room->room_token, data->room_token) != 0)
    {
      g_debug ("Room changed during older messages load, discarding results");
      self->loading_older = FALSE;
      g_signal_emit (self, signals[SIGNAL_LOADING_OLDER_CHANGED], 0, FALSE);
      return;
    }

  GListStore *older = g_task_propagate_pointer (task, NULL);
  if (!older)
    {
      self->loading_older = FALSE;
      g_signal_emit (self, signals[SIGNAL_LOADING_OLDER_CHANGED], 0, FALSE);
      return;
    }

  guint n_older = g_list_model_get_n_items (G_LIST_MODEL (older));
  g_debug ("Loaded %u older messages from DB", n_older);

  if (n_older == 0)
    {
      g_object_unref (older);
      /* No more in DB, try fetching from server via MessageController */
      if (self->message_controller && self->current_room && self->current_room->oldest_loaded_id > 0)
        {
          g_debug ("DB exhausted, fetching older messages from server via MessageController");
          gnalk_message_controller_load_older_messages (self->message_controller,
                                                          self->current_room->oldest_loaded_id);
        }
      else
        {
          self->current_room->has_more_history = FALSE;
          self->loading_older = FALSE;
          g_signal_emit (self, signals[SIGNAL_LOADING_OLDER_CHANGED], 0, FALSE);
        }
      return;
    }

  GListStore *store = self->current_room->messages_store;

  /* Collect messages into an array for efficient bulk insert */
  g_autoptr(GPtrArray) messages_to_insert = g_ptr_array_new_with_free_func (g_object_unref);

  for (guint i = 0; i < n_older; i++)
    {
      GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (older), i);
      if (msg)
        g_ptr_array_add (messages_to_insert, msg);
    }

  /* Prepend all older messages at once using splice - O(n) instead of O(n^2) */
  if (messages_to_insert->len > 0)
    {
      g_list_store_splice (store, 0, 0,
                           messages_to_insert->pdata, messages_to_insert->len);
      for (guint i = 0; i < messages_to_insert->len; i++)
        msg_index_add (self, g_ptr_array_index (messages_to_insert, i));
    }

  /* Update oldest_loaded_message_id */
  GnalkMessageData *oldest = g_list_model_get_item (G_LIST_MODEL (older), 0);
  if (oldest)
    {
      self->current_room->oldest_loaded_id = gnalk_message_data_get_id (oldest);
      g_object_unref (oldest);
    }

  g_object_unref (older);

  /* Update date separators and timestamp visibility for all messages */
  gnalk_message_processor_update_separators (self);

  self->loading_older = FALSE;
  g_signal_emit (self, signals[SIGNAL_LOADING_OLDER_CHANGED], 0, FALSE);
}

void
gnalk_message_processor_load_older (GnalkMessageProcessor *self)
{
  if (!self->current_room ||
      self->loading_older ||
      !self->current_room->has_more_history)
    return;

  if (self->current_room->oldest_loaded_id <= 0)
    return;

  self->loading_older = TRUE;
  g_signal_emit (self, signals[SIGNAL_LOADING_OLDER_CHANGED], 0, TRUE);

  if (!self->database || self->account_id <= 0)
    {
      /* No database or no session, try server directly via MessageController */
      if (self->message_controller && self->current_room)
        {
          gnalk_message_controller_load_older_messages (self->message_controller,
                                                          self->current_room->oldest_loaded_id);
        }
      else
        {
          self->loading_older = FALSE;
          g_signal_emit (self, signals[SIGNAL_LOADING_OLDER_CHANGED], 0, FALSE);
        }
      return;
    }

  LoadOlderMessagesData *data = g_slice_new (LoadOlderMessagesData);
  data->db = self->database;
  data->account_id = self->account_id;
  data->room_token = g_strdup (self->current_room->room_token);
  data->before_id = self->current_room->oldest_loaded_id;
  data->generation = self->current_room->message_load_generation;

  GTask *task = g_task_new (self, NULL, load_older_messages_from_db_finish, NULL);
  g_task_set_task_data (task, data, (GDestroyNotify) load_older_messages_data_free);
  g_task_run_in_thread (task, load_older_messages_thread);
  g_object_unref (task);
}

/* ============ Process older messages from server ============ */

void
gnalk_message_processor_process_older_from_server (GnalkMessageProcessor *self,
                                                     JsonArray             *messages,
                                                     gboolean               has_more)
{
  GNALK_TRACE_MESSAGES ("process_older_from_server: received response, has_more=%d", has_more);

  self->loading_older = FALSE;
  g_signal_emit (self, signals[SIGNAL_LOADING_OLDER_CHANGED], 0, FALSE);

  guint len = json_array_get_length (messages);
  if (len == 0)
    {
      if (self->current_room)
        self->current_room->has_more_history = FALSE;
      GNALK_TRACE_MESSAGES ("process_older_from_server: no messages, history exhausted");
      g_debug ("No more messages from server, history exhausted");
      return;
    }

  if (!self->current_room)
    return;

  GNALK_TRACE_MESSAGES ("process_older_from_server: processing %u older messages", len);
  g_debug ("Received %u older messages via MessageController", len);

  const char *current_user = self->current_user_id;
  GListStore *store = self->current_room->messages_store;

  /* Build list of messages (server returns newest first for lookIntoFuture=0) */
  GPtrArray *new_messages = g_ptr_array_new_with_free_func (g_object_unref);

  for (guint i = 0; i < len; i++)
    {
      JsonObject *msg_obj = json_array_get_object_element (messages, i);
      if (!msg_obj)
        continue;

      GnalkMessageData *msg_data = gnalk_message_data_new_from_json (msg_obj, current_user);
      if (msg_data)
        g_ptr_array_add (new_messages, msg_data);
    }

  if (new_messages->len == 0)
    {
      g_ptr_array_free (new_messages, TRUE);
      return;
    }

  /* Reverse array to get oldest-first order (server returns newest-first) */
  for (guint i = 0; i < new_messages->len / 2; i++)
    {
      gpointer tmp = new_messages->pdata[i];
      new_messages->pdata[i] = new_messages->pdata[new_messages->len - 1 - i];
      new_messages->pdata[new_messages->len - 1 - i] = tmp;
    }

  /* Prepend all messages at once using splice */
  GNALK_TRACE_STORE ("process_older_from_server: prepending %u messages to store", new_messages->len);
  g_list_store_splice (store, 0, 0,
                       new_messages->pdata, new_messages->len);
  for (guint j = 0; j < new_messages->len; j++)
    msg_index_add (self, g_ptr_array_index (new_messages, j));

  /* Update oldest_loaded_message_id from the oldest message received (now at index 0) */
  GnalkMessageData *oldest = g_ptr_array_index (new_messages, 0);
  self->current_room->oldest_loaded_id = gnalk_message_data_get_id (oldest);
  GNALK_TRACE_MESSAGES ("process_older_from_server: updated oldest_id=%"G_GINT64_FORMAT,
                         self->current_room->oldest_loaded_id);

  g_ptr_array_free (new_messages, TRUE);

  /* Update has_more_history flag */
  self->current_room->has_more_history = has_more;

  /* Update date separators and timestamp visibility */
  gnalk_message_processor_update_separators (self);
}

/* ============ Process messages from JSON ============ */

void
gnalk_message_processor_process_json (GnalkMessageProcessor *self,
                                        JsonArray             *messages,
                                        gboolean               is_initial_load,
                                        gboolean               view_is_at_bottom)
{
  if (!self->current_room)
    return;

  /* In context mode, skip non-initial messages (poll responses).
   * They are already saved to DB by the MC and will appear on restore_live. */
  if (self->current_room->in_context_mode && !is_initial_load)
    return;

  const char *current_user = self->current_user_id;
  GListStore *store = self->current_room->messages_store;
  guint len = json_array_get_length (messages);

  gboolean initial_load = is_initial_load &&
                          (g_list_model_get_n_items (G_LIST_MODEL (store)) == 0);
  gboolean was_at_bottom = initial_load || self->current_room->scroll_to_bottom ||
                           view_is_at_bottom;

  /* For initial load from server, disconnect model to prevent visual movement */
  if (initial_load && len > 0)
    {
      g_signal_emit (self, signals[SIGNAL_BATCH_INSERT_BEGIN], 0);
      GNALK_TRACE_UI ("process_json: emitted batch-insert-begin (initial_load, %u msgs)", len);
    }

  gint64 max_id = self->current_room->last_known_message_id;

  for (guint i = 0; i < len; i++)
    {
      JsonObject *msg = json_array_get_object_element (messages, i);
      if (!msg)
        continue;

      gint64 msg_id = json_object_get_int_member_with_default (msg, "id", 0);
      if (msg_id > max_id)
        max_id = msg_id;

      /* Check if this is a reaction message */
      const char *system_message = json_object_get_string_member_with_default (msg, "systemMessage", "");
      GNALK_TRACE_MESSAGES ("process_json: processing msg %"G_GINT64_FORMAT" systemMessage='%s'",
                            msg_id, system_message);
      if (g_strcmp0 (system_message, "reaction") == 0)
        {
          const char *emoji = json_object_get_string_member_with_default (msg, "message", "");
          JsonNode *parent_node = json_object_get_member (msg, "parent");
          if (parent_node && JSON_NODE_HOLDS_OBJECT (parent_node) && *emoji)
            {
              JsonObject *parent = json_node_get_object (parent_node);
              if (parent)
                {
                  gint64 parent_id = json_object_get_int_member_with_default (parent, "id", 0);
                  GnalkMessageData *target = g_hash_table_lookup (self->msg_index, &parent_id);
                  if (target)
                    gnalk_message_data_add_reaction (target, emoji);
                }
            }
          continue;
        }

      /* Handle message_edited system message - update original message */
      if (g_strcmp0 (system_message, "message_edited") == 0)
        {
          JsonNode *parent_node = json_object_get_member (msg, "parent");
          if (parent_node && JSON_NODE_HOLDS_OBJECT (parent_node))
            {
              JsonObject *parent = json_node_get_object (parent_node);
              if (parent)
                {
                  gint64 parent_id = json_object_get_int_member_with_default (parent, "id", 0);
                  GnalkMessageData *target = g_hash_table_lookup (self->msg_index, &parent_id);
                  if (target)
                    {
                      const char *new_message = json_object_get_string_member_with_default (parent, "message", "");
                      gint64 edit_timestamp = json_object_get_int_member_with_default (parent, "lastEditTimestamp", 0);
                      gnalk_message_data_set_message (target, new_message);
                      gnalk_message_data_set_last_edit_timestamp (target, edit_timestamp);
                    }
                }
            }
          continue;
        }

      /* Handle message_deleted system message - update original message */
      if (g_strcmp0 (system_message, "message_deleted") == 0)
        {
          JsonNode *parent_node = json_object_get_member (msg, "parent");
          if (parent_node && JSON_NODE_HOLDS_OBJECT (parent_node))
            {
              JsonObject *parent = json_node_get_object (parent_node);
              if (parent)
                {
                  gint64 parent_id = json_object_get_int_member_with_default (parent, "id", 0);
                  GnalkMessageData *target = g_hash_table_lookup (self->msg_index, &parent_id);
                  if (target)
                    {
                      const char *deleted_message = json_object_get_string_member_with_default (parent, "message", "");
                      gnalk_message_data_set_message (target, deleted_message);
                    }
                }
            }
          continue;
        }

      /* Handle call_left system message - emit signal for UI to update call state */
      if (!self->current_room->skip_historical_call_messages && g_strcmp0 (system_message, "call_left") == 0)
        {
          const char *actor_id = json_object_get_string_member_with_default (msg, "actorId", "");
          if (actor_id && *actor_id)
            g_signal_emit (self, signals[SIGNAL_CALL_STATE_LEFT], 0, actor_id);
        }

      /* Handle call_ended system message - emit signal for UI to clear all call states */
      if (!self->current_room->skip_historical_call_messages && g_strcmp0 (system_message, "call_ended") == 0)
        {
          g_signal_emit (self, signals[SIGNAL_CALL_ENDED], 0);
        }

      /* Check if this message matches a pending message by referenceId */
      const char *ref_id = json_object_get_string_member_with_default (msg, "referenceId", "");
      gboolean found_pending = FALSE;

      if (ref_id && *ref_id)
        {
          GnalkMessageData *existing = g_hash_table_lookup (self->ref_index, ref_id);
          if (existing)
            {
              /* Found matching pending message - update it.
               * Set parameters before message so notify::message handler
               * renders with current parameters. freeze/thaw batches all notifications. */
              gint64 timestamp = json_object_get_int_member_with_default (msg, "timestamp", 0);

              g_object_freeze_notify (G_OBJECT (existing));

              /* Copy messageParameters from server first (contains file info) */
              if (json_object_has_member (msg, "messageParameters"))
                {
                  JsonNode *params_node = json_object_get_member (msg, "messageParameters");
                  if (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
                    {
                      JsonObject *params = json_node_get_object (params_node);
                      gnalk_message_data_set_message_parameters (existing, params);
                    }
                }

              gnalk_message_data_confirm_sent (existing, msg_id, timestamp);

              const char *server_message = json_object_get_string_member_with_default (msg, "message", NULL);
              if (server_message)
                gnalk_message_data_set_message (existing, server_message);

              g_object_thaw_notify (G_OBJECT (existing));

              /* If this was a file upload, copy the local file to cache with correct file_id */
              const char *local_path = gnalk_message_data_get_upload_file_path (existing);
              if (local_path && *local_path && json_object_has_member (msg, "messageParameters"))
                {
                  JsonNode *params_node = json_object_get_member (msg, "messageParameters");
                  JsonObject *params = (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
                    ? json_node_get_object (params_node) : NULL;
                  if (params && json_object_has_member (params, "file"))
                    {
                      JsonObject *file = json_object_get_object_member (params, "file");
                      if (file)
                        {
                          const char *file_id = json_object_get_string_member_with_default (file, "id", NULL);
                          const char *file_name = json_object_get_string_member_with_default (file, "name", NULL);
                          if (file_id && file_name)
                            {
                              g_autofree char *cache_dir = g_build_filename (g_get_user_cache_dir (), "gnalk", NULL);
                              g_mkdir_with_parents (cache_dir, 0700);
                              g_autofree char *safe_name = g_strdup_printf ("%s_%s", file_id, file_name);
                              g_autofree char *cache_path = g_build_filename (cache_dir, safe_name, NULL);

                              g_autoptr(GFile) src = g_file_new_for_path (local_path);
                              g_autoptr(GFile) dst = g_file_new_for_path (cache_path);
                              g_autoptr(GError) copy_error = NULL;
                              if (!g_file_copy (src, dst, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &copy_error))
                                g_debug ("Failed to copy uploaded file to cache: %s", copy_error->message);
                              else
                                g_debug ("Copied uploaded file to cache: %s", cache_path);
                            }
                        }
                    }
                }

              /* Save confirmed message to database */
              if (self->database && self->account_id > 0 && self->current_room)
                gnalk_database_save_message (self->database, self->account_id,
                                              self->current_room->room_token, existing);

              /* Update indexes: remove from ref_index, add to msg_index */
              g_hash_table_remove (self->ref_index, ref_id);
              if (msg_id > 0)
                {
                  gint64 *key = g_new (gint64, 1);
                  *key = msg_id;
                  g_hash_table_insert (self->msg_index, key, existing);
                }

              /* Check if any pending messages remain - if not, re-sort the list */
              {
                gboolean has_pending = FALSE;
                guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
                for (guint k = 0; k < n; k++)
                  {
                    GnalkMessageData *m = g_list_model_get_item (G_LIST_MODEL (store), k);
                    if (gnalk_message_data_get_id (m) == 0 || gnalk_message_data_get_is_uploading (m))
                      {
                        has_pending = TRUE;
                        g_object_unref (m);
                        break;
                      }
                    g_object_unref (m);
                  }

                if (!has_pending)
                  g_list_store_sort (store, gnalk_message_processor_compare_by_id, NULL);
              }

              found_pending = TRUE;
            }
        }

      if (!found_pending)
        {
          /* Check if message with this ID already exists via index (O(1)) */
          GnalkMessageData *existing = (msg_id > 0)
            ? g_hash_table_lookup (self->msg_index, &msg_id) : NULL;

          if (existing)
            {
              gint64 new_edit_ts = json_object_get_int_member_with_default (msg, "lastEditTimestamp", 0);
              gint64 existing_edit_ts = gnalk_message_data_get_last_edit_timestamp (existing);
              if (new_edit_ts > existing_edit_ts)
                {
                  const char *new_message = json_object_get_string_member_with_default (msg, "message", "");
                  gnalk_message_data_set_message (existing, new_message);
                  gnalk_message_data_set_last_edit_timestamp (existing, new_edit_ts);
                }
            }

          if (!existing)
            {
              GnalkMessageData *msg_data = gnalk_message_data_new_from_json (msg, current_user);

              if (self->current_room->last_read_message_id > 0 &&
                  msg_id > self->current_room->last_read_message_id &&
                  !gnalk_message_data_get_is_own (msg_data))
                {
                  gnalk_message_data_set_is_unread (msg_data, TRUE);

                  if (self->current_room->first_unread_message_id == 0 ||
                      msg_id < self->current_room->first_unread_message_id)
                    {
                      self->current_room->first_unread_message_id = msg_id;
                    }
                }

              guint before_count = g_list_model_get_n_items (G_LIST_MODEL (store));
              g_list_store_insert_sorted (store, msg_data,
                                           gnalk_message_processor_compare_by_id, NULL);
              msg_index_add (self, msg_data);
              guint after_count = g_list_model_get_n_items (G_LIST_MODEL (store));
              GNALK_TRACE_MESSAGES ("process_json: inserted msg %"G_GINT64_FORMAT" (store %u -> %u)",
                                      msg_id, before_count, after_count);
              g_object_unref (msg_data);
            }
          else
            {
              GNALK_TRACE_MESSAGES ("process_json: msg %"G_GINT64_FORMAT" already exists, skipped", msg_id);
            }
        }
    }

  /* Update date separators and timestamp visibility after batch processing */
  gnalk_message_processor_update_separators (self);

  /* Reconnect model if we disconnected for batch insert */
  if (initial_load && len > 0)
    {
      g_signal_emit (self, signals[SIGNAL_BATCH_INSERT_END], 0);
      GNALK_TRACE_UI ("process_json: emitted batch-insert-end with %u items",
                       g_list_model_get_n_items (G_LIST_MODEL (store)));
    }

  self->current_room->last_known_message_id = max_id;

  /* Track oldest message for lazy loading - only on initial load */
  if (initial_load)
    {
      guint n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
      if (n_items > 0)
        {
          GnalkMessageData *oldest = g_list_model_get_item (G_LIST_MODEL (store), 0);
          if (oldest)
            {
              self->current_room->oldest_loaded_id = gnalk_message_data_get_id (oldest);
              g_object_unref (oldest);
            }
        }
      if (len < 50)
        self->current_room->has_more_history = FALSE;
    }

  /* Emit scroll signals */
  if (len > 0 && was_at_bottom)
    {
      if (initial_load && self->current_room->first_unread_message_id > 0)
        {
          /* Find the position of the first unread message */
          guint n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
          for (guint i = 0; i < n_items; i++)
            {
              GnalkMessageData *data = g_list_model_get_item (G_LIST_MODEL (store), i);
              gint64 mid = gnalk_message_data_get_id (data);
              g_object_unref (data);

              if (mid == self->current_room->first_unread_message_id)
                {
                  g_signal_emit (self, signals[SIGNAL_SCROLL_TO_ITEM], 0, i);
                  break;
                }
            }
        }
      else
        {
          guint n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
          if (initial_load && n_items > 0)
            {
              g_signal_emit (self, signals[SIGNAL_SCROLL_TO_ITEM], 0, n_items - 1);
            }
          else
            {
              g_signal_emit (self, signals[SIGNAL_SCROLL_TO_BOTTOM], 0);
            }
        }
    }

  self->current_room->scroll_to_bottom = FALSE;
  self->current_room->skip_historical_call_messages = FALSE;
}

/* ============ Context mode (jump-to-message) ============ */

void
gnalk_message_processor_load_context (GnalkMessageProcessor *self,
                                      JsonArray             *messages,
                                      gint64                 target_message_id)
{
  if (!self->current_room || !messages)
    return;

  guint len = json_array_get_length (messages);
  if (len == 0)
    return;

  const char *current_user = self->current_user_id;
  GListStore *store = self->current_room->messages_store;

  /* Enter context mode */
  self->current_room->in_context_mode = TRUE;
  self->current_room->context_target_message_id = target_message_id;

  /* Disconnect model, clear store and indexes */
  g_signal_emit (self, signals[SIGNAL_BATCH_INSERT_BEGIN], 0);
  g_list_store_remove_all (store);
  msg_index_clear (self);

  /* Parse and insert messages */
  gint64 oldest_id = G_MAXINT64;
  gint64 newest_id = 0;

  for (guint i = 0; i < len; i++)
    {
      JsonObject *msg_obj = json_array_get_object_element (messages, i);
      if (!msg_obj)
        continue;

      /* Skip transient system messages */
      const char *sys_msg = json_object_get_string_member_with_default (msg_obj, "systemMessage", "");
      if (g_strcmp0 (sys_msg, "reaction") == 0 ||
          g_strcmp0 (sys_msg, "message_edited") == 0 ||
          g_strcmp0 (sys_msg, "message_deleted") == 0)
        continue;

      GnalkMessageData *msg_data = gnalk_message_data_new_from_json (msg_obj, current_user);
      if (!msg_data)
        continue;

      gint64 msg_id = gnalk_message_data_get_id (msg_data);
      if (msg_id < oldest_id)
        oldest_id = msg_id;
      if (msg_id > newest_id)
        newest_id = msg_id;

      g_list_store_insert_sorted (store, msg_data,
                                   gnalk_message_processor_compare_by_id, NULL);
      msg_index_add (self, msg_data);
      g_object_unref (msg_data);
    }

  /* Update loading state */
  self->current_room->oldest_loaded_id = (oldest_id < G_MAXINT64) ? oldest_id : 0;
  self->current_room->context_newest_id = newest_id;
  self->current_room->has_more_history = TRUE;
  self->current_room->last_known_message_id = newest_id;

  /* Update separators */
  gnalk_message_processor_update_separators (self);

  /* Reconnect model */
  g_signal_emit (self, signals[SIGNAL_BATCH_INSERT_END], 0);

  /* Find target message and scroll to it */
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
  guint target_pos = n_items / 2; /* fallback: scroll to middle */
  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *data = g_list_model_get_item (G_LIST_MODEL (store), i);
      if (data)
        {
          if (gnalk_message_data_get_id (data) == target_message_id)
            {
              target_pos = i;
              g_object_unref (data);
              break;
            }
          g_object_unref (data);
        }
    }

  g_signal_emit (self, signals[SIGNAL_SCROLL_TO_ITEM], 0, target_pos);
  g_signal_emit (self, signals[SIGNAL_STATE_SYNCED], 0,
                 (guint) GNALK_PROCESSOR_SYNC_CONTEXT_LOADED);
}

void
gnalk_message_processor_restore_live (GnalkMessageProcessor *self)
{
  if (!self->current_room)
    return;

  /* Exit context mode */
  self->current_room->in_context_mode = FALSE;
  self->current_room->context_target_message_id = 0;
  self->current_room->context_newest_id = 0;

  /* Clear store and indexes */
  g_signal_emit (self, signals[SIGNAL_BATCH_INSERT_BEGIN], 0);
  g_list_store_remove_all (self->current_room->messages_store);
  msg_index_clear (self);
  g_signal_emit (self, signals[SIGNAL_BATCH_INSERT_END], 0);

  /* Reload from DB (same as opening a room fresh) */
  gnalk_message_processor_load_from_db (self, self->current_room->room_token);
}
