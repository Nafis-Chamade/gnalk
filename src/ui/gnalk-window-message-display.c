/* gnalk-window-message-display.c
 *
 * Message display functionality extracted from gnalk-window.c.
 * Handles loading, displaying, and managing messages in the message list.
 */

#include "gnalk-window-private.h"
#include "gnalk-application.h"
#include "gnalk-database.h"
#include "gnalk-message-data.h"
#include "gnalk-message-controller.h"
#include "gnalk-log.h"
#include <json-glib/json-glib.h>
#include <glib/gi18n.h>

/* ============ Message sorting ============ */

/* Compare function for sorting messages in store - sorts by ID (chronological) */
gint
gnalk_window_compare_messages_by_id (gconstpointer a,
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

/* ============ Clear/refresh messages ============ */

/* Clear messages by disconnecting model first to avoid GTK internal state issues */
void
gnalk_window_clear_messages_store (GnalkWindow *self)
{
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  GNALK_TRACE_STORE ("clear_messages_store: clearing %u items from store %p for room %s",
                      n_items, (void*)self->messages_store,
                      self->current_room ? self->current_room->room_token : "(null)");

  /* Disconnect model from view first - this prevents GTK from trying to
   * unbind items during removal which can cause crashes during transitions */
  gtk_no_selection_set_model (self->messages_selection, NULL);

  /* Now safe to clear the store */
  g_list_store_remove_all (self->messages_store);

  /* Reconnect the model */
  gtk_no_selection_set_model (self->messages_selection, G_LIST_MODEL (self->messages_store));
  GNALK_TRACE_STORE ("clear_messages_store: cleared, store now has 0 items");
}

/* Update is_own flags for all messages based on current session's user.
 * Used when switching accounts to avoid full reload. */
void
gnalk_window_update_messages_is_own (GnalkWindow *self)
{
  const char *current_user = NULL;
  if (self->active_session)
    {
      GnalkAccount *account = gnalk_session_get_account (self->active_session);
      if (account)
        current_user = gnalk_account_get_user_id (account);
    }

  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
      if (data)
        {
          const char *author_id = gnalk_message_data_get_author_id (data);
          gboolean is_own = current_user && g_strcmp0 (author_id, current_user) == 0;
          gnalk_message_data_set_is_own (data, is_own);
          g_object_unref (data);
        }
    }
  g_debug ("Updated is_own flags for %u messages (current_user=%s)", n_items, current_user ? current_user : "(null)");
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

  /* Save all messages to database */
  for (guint i = 0; i < data->messages->len; i++)
    {
      GnalkMessageData *msg = g_ptr_array_index (data->messages, i);
      if (msg && gnalk_message_data_get_id (msg) > 0)
        gnalk_database_save_message (data->db, data->account_id, data->room_token, msg);
    }

  g_task_return_boolean (task, TRUE);
}

void
gnalk_window_save_messages_to_db (GnalkWindow *self)
{
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  if (!app)
    return;

  GnalkDatabase *db = gnalk_application_get_database (app);
  gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;

  if (!db || account_id <= 0 || !self->current_room)
    return;

  /* Copy messages to array for async saving */
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  if (n_items == 0)
    return;

  GPtrArray *messages = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
      if (data)
        g_ptr_array_add (messages, data);  /* Takes ownership */
    }

  SaveMessagesData *data = g_slice_new (SaveMessagesData);
  data->db = db;
  data->account_id = account_id;
  data->room_token = g_strdup (self->current_room->room_token);
  data->messages = messages;

  GTask *task = g_task_new (self, NULL, NULL, NULL);  /* No callback needed */
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

  /* Run DB query in background thread - load last 15 for fast initial display */
  GListStore *cached = gnalk_database_load_messages (data->db, data->account_id,
                                                       data->room_token, 15, 0);
  g_task_return_pointer (task, cached, g_object_unref);
}

/* Helper function to update date separators and timestamp visibility */
static void
update_date_separators_and_timestamps (GnalkWindow *self)
{
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  GNALK_TRACE_UI ("update_date_separators_and_timestamps: processing %u messages", n_items);

  int prev_year = 0, prev_month = 0, prev_day = 0;

  for (guint j = 0; j < n_items; j++)
    {
      GnalkMessageData *msg_data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), j);
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
      gboolean show_timestamp = TRUE;

      if (!is_system && j + 1 < n_items)
        {
          GnalkMessageData *next = g_list_model_get_item (G_LIST_MODEL (self->messages_store), j + 1);
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

static void
load_messages_from_db_finish (GObject      *source_object,
                               GAsyncResult *result,
                               gpointer      user_data G_GNUC_UNUSED)
{
  GnalkWindow *self = GNALK_WINDOW (source_object);
  GTask *task = G_TASK (result);

  GNALK_TRACE_DATABASE ("load_messages_from_db_finish: callback started");
  g_message ("load_messages_from_db_finish: starting");

  /* Check if room changed while loading */
  const char *expected_token = g_task_get_name (task);
  if (!self->current_room || g_strcmp0 (self->current_room->room_token, expected_token) != 0)
    {
      GNALK_TRACE_DATABASE ("load_messages_from_db_finish: room changed (expected=%s, current=%s), discarding",
                             expected_token, self->current_room ? self->current_room->room_token : "(null)");
      g_message ("load_messages_from_db_finish: room changed (expected=%s, current=%s), discarding",
                 expected_token, self->current_room ? self->current_room->room_token : "(null)");
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
      /* Cache empty - need to fetch initial messages from server */
      GNALK_TRACE_DATABASE ("load_messages_from_db_finish: cache empty, triggering server fetch");
      g_message ("load_messages_from_db_finish: cache empty, triggering server fetch");
      if (self->message_controller)
        gnalk_message_controller_load_messages (self->message_controller);
      return;
    }

  /* Get current user ID from active session (not application, for multi-account support) */
  const char *current_user = NULL;
  if (self->active_session)
    {
      GnalkAccount *account = gnalk_session_get_account (self->active_session);
      if (account)
        current_user = gnalk_account_get_user_id (account);
    }

  /* If last_read_message_id is not set (e.g., immediate restore at startup),
   * look it up from the conversation data now that conversations are loaded */
  if (self->current_room && self->current_room->last_read_message_id == 0)
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

  GNALK_TRACE_STORE ("load_messages_from_db_finish: populating store with %u messages", n_cached);

  /* Disconnect model from view to prevent visual updates during batch insert.
   * This eliminates the "movement" when loading many messages. */
  gtk_no_selection_set_model (self->messages_selection, NULL);
  GNALK_TRACE_UI ("load_messages_from_db_finish: disconnected model for batch insert");

  /* Populate store on main thread and mark unread messages */
  guint added_count = 0;
  guint skipped_count = 0;
  for (guint i = 0; i < n_cached; i++)
    {
      GnalkMessageData *data = g_list_model_get_item (G_LIST_MODEL (cached), i);
      if (data)
        {
          gint64 msg_id = gnalk_message_data_get_id (data);

          /* Check if message already exists in store (avoid duplicates) */
          gboolean already_exists = FALSE;
          guint n_existing = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
          for (guint j = 0; j < n_existing; j++)
            {
              GnalkMessageData *existing = g_list_model_get_item (G_LIST_MODEL (self->messages_store), j);
              if (gnalk_message_data_get_id (existing) == msg_id)
                {
                  already_exists = TRUE;
                  g_object_unref (existing);
                  break;
                }
              g_object_unref (existing);
            }

          if (!already_exists)
            {
              /* Recalculate is_own based on current session's user_id
               * (DB may have stored wrong value for multi-account scenarios) */
              const char *author_id = gnalk_message_data_get_author_id (data);
              gboolean is_own = current_user && g_strcmp0 (author_id, current_user) == 0;
              gnalk_message_data_set_is_own (data, is_own);

              /* Mark as unread if message ID > last_read_message_id and not our own message */
              if (self->current_room->last_read_message_id > 0)
                {
                  if (msg_id > self->current_room->last_read_message_id && !is_own)
                    {
                      gnalk_message_data_set_is_unread (data, TRUE);

                      /* Track first unread message for scrolling */
                      if (self->current_room->first_unread_message_id == 0 ||
                          msg_id < self->current_room->first_unread_message_id)
                        {
                          self->current_room->first_unread_message_id = msg_id;
                        }
                    }
                }

              /* Insert sorted by message ID to maintain correct order */
              g_list_store_insert_sorted (self->messages_store, data,
                                           gnalk_window_compare_messages_by_id, NULL);
              added_count++;
            }
          else
            {
              skipped_count++;
            }
        }
      g_clear_object (&data);
    }

  GNALK_TRACE_STORE ("load_messages_from_db_finish: added %u messages, skipped %u duplicates", added_count, skipped_count);

  /* Update date separators and timestamp visibility for all messages */
  update_date_separators_and_timestamps (self);

  /* Reconnect model to view - items will appear all at once */
  gtk_no_selection_set_model (self->messages_selection, G_LIST_MODEL (self->messages_store));
  GNALK_TRACE_UI ("load_messages_from_db_finish: reconnected model with %u items", added_count);

  g_object_unref (cached);

  /* Track oldest loaded message for lazy loading */
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  GNALK_TRACE_MESSAGES ("load_messages_from_db_finish: store now has %u messages total", n_items);

  if (n_items > 0)
    {
      /* Track oldest for history loading */
      GnalkMessageData *oldest = g_list_model_get_item (G_LIST_MODEL (self->messages_store), 0);
      if (oldest)
        {
          self->current_room->oldest_loaded_id = gnalk_message_data_get_id (oldest);
          g_object_unref (oldest);
        }

      /* Track newest for polling - messages are sorted oldest first */
      GnalkMessageData *newest = g_list_model_get_item (G_LIST_MODEL (self->messages_store), n_items - 1);
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

  /* Update tab state */
  if (self->tab_manager)
    {
      GnalkRoomTab *tab = gnalk_room_tab_manager_get_tab (self->tab_manager, expected_token);
      if (tab)
        {
          tab->state.oldest_loaded_id = self->current_room->oldest_loaded_id;
          tab->state.last_known_message_id = self->current_room->last_known_message_id;
          tab->state.initial_load_done = TRUE;
          tab->state.has_more_history = self->current_room->has_more_history;
        }
    }

  /* Show the view positioned at bottom (no visual jump).
   * This hides the view, positions at bottom, then reveals it. */
  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  g_message ("Messages displayed: %u (from cache)", n_items);
  GNALK_TRACE_SCROLL ("load_messages_from_db_finish: showing view at bottom (%u items)", n_items);
  self->current_room->scroll_to_bottom = TRUE;

  if (n_items > 0)
    gnalk_message_view_show_messages_at_bottom (self->message_view);
  else
    gnalk_message_view_show_messages (self->message_view);

  /* Start message polling for new messages (cache-first: we displayed cache, now poll for updates) */
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

/* Load messages from database asynchronously */
void
gnalk_window_load_messages_from_db (GnalkWindow *self,
                                      const char   *room_token)
{
  GNALK_TRACE_DATABASE ("load_messages_from_db: starting for room %s, current_room=%s",
                          room_token, self->current_room ? self->current_room->room_token : "(null)");
  g_message ("load_messages_from_db: starting for room %s", room_token);

  /* Clear old messages before reloading */
  GNALK_TRACE_STORE ("load_messages_from_db: clearing messages store");
  gnalk_window_clear_messages_store (self);

  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  if (!app)
    {
      g_message ("load_messages_from_db: no app, returning");
      return;
    }

  GnalkDatabase *db = gnalk_application_get_database (app);
  gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;

  /* If account_id not set yet (offline startup, no session), look it up from settings */
  if (account_id <= 0 && db)
    {
      GSettings *settings = gnalk_application_get_settings (app);
      g_autofree char *server_url = g_settings_get_string (settings, "last-server-url");
      g_autofree char *user_id = g_settings_get_string (settings, "last-user-id");

      if (server_url && *server_url && user_id && *user_id)
        {
          account_id = gnalk_database_get_or_create_account (db, server_url, user_id);
          g_message ("load_messages_from_db: looked up account_id=%" G_GINT64_FORMAT " from settings", account_id);
        }
    }

  g_message ("load_messages_from_db: db=%p, account_id=%" G_GINT64_FORMAT, (void*)db, account_id);

  if (!db || account_id <= 0)
    {
      g_message ("load_messages_from_db: no db or invalid account_id, returning");
      return;
    }

  /* Prepare async task data */
  LoadMessagesData *data = g_slice_new (LoadMessagesData);
  data->db = db;
  data->account_id = account_id;
  data->room_token = g_strdup (room_token);

  GTask *task = g_task_new (self, NULL, load_messages_from_db_finish, NULL);
  g_task_set_name (task, room_token);  /* Store token to verify on completion */
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

  /* Load older messages from DB - 20 before the given ID */
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
  GnalkWindow *self = GNALK_WINDOW (source_object);
  GTask *task = G_TASK (result);
  LoadOlderMessagesData *data = g_task_get_task_data (task);

  /* Check if room changed while loading */
  if (!self->current_room ||
      data->generation != self->current_room->message_load_generation ||
      g_strcmp0 (self->current_room->room_token, data->room_token) != 0)
    {
      g_debug ("Room changed during older messages load, discarding results");
      gnalk_message_view_set_loading_older (self->message_view, FALSE);
      return;
    }

  GListStore *older = g_task_propagate_pointer (task, NULL);
  if (!older)
    {
      gnalk_message_view_set_loading_older (self->message_view, FALSE);
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
          gnalk_message_view_set_loading_older (self->message_view, FALSE);
        }
      return;
    }

  GtkScrolledWindow *scroll = gnalk_message_view_get_scrolled_window (self->message_view);

  /* Collect messages into an array for efficient bulk insert */
  g_autoptr(GPtrArray) messages_to_insert = g_ptr_array_new_with_free_func (g_object_unref);

  for (guint i = 0; i < n_older; i++)
    {
      GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (older), i);
      if (msg)
        g_ptr_array_add (messages_to_insert, msg);  /* Takes ownership */
    }

  /* Prepend all older messages at once using splice - O(n) instead of O(n^2) */
  if (messages_to_insert->len > 0)
    g_list_store_splice (self->messages_store, 0, 0,
                         messages_to_insert->pdata, messages_to_insert->len);

  /* Update oldest_loaded_message_id */
  GnalkMessageData *oldest = g_list_model_get_item (G_LIST_MODEL (older), 0);
  if (oldest)
    {
      self->current_room->oldest_loaded_id = gnalk_message_data_get_id (oldest);
      g_object_unref (oldest);
    }

  g_object_unref (older);

  /* Update date separators and timestamp visibility for all messages */
  update_date_separators_and_timestamps (self);

  /* Restore scroll position after UI updates */
  g_idle_add_once ((GSourceOnceFunc) (void (*)(void)) gtk_widget_queue_allocate, GTK_WIDGET (scroll));

  gnalk_message_view_set_loading_older (self->message_view, FALSE);
}

/* Load older messages when scrolled near top */
void
gnalk_window_load_older_messages (GnalkWindow *self)
{
  if (!self->current_room ||
      gnalk_message_view_get_loading_older (self->message_view) ||
      !self->current_room->has_more_history)
    return;

  if (self->current_room->oldest_loaded_id <= 0)
    return;

  gnalk_message_view_set_loading_older (self->message_view, TRUE);

  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  if (!app)
    {
      gnalk_message_view_set_loading_older (self->message_view, FALSE);
      return;
    }

  GnalkDatabase *db = gnalk_application_get_database (app);
  gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;

  if (!db || account_id <= 0)
    {
      /* No database or no session, try server directly via MessageController */
      if (self->message_controller && self->current_room)
        {
          gnalk_message_controller_load_older_messages (self->message_controller,
                                                          self->current_room->oldest_loaded_id);
        }
      else
        {
          gnalk_message_view_set_loading_older (self->message_view, FALSE);
        }
      return;
    }

  /* Prepare async task data */
  LoadOlderMessagesData *data = g_slice_new (LoadOlderMessagesData);
  data->db = db;
  data->account_id = account_id;
  data->room_token = g_strdup (self->current_room->room_token);
  data->before_id = self->current_room->oldest_loaded_id;
  data->generation = self->current_room->message_load_generation;

  GTask *task = g_task_new (self, NULL, load_older_messages_from_db_finish, NULL);
  g_task_set_task_data (task, data, (GDestroyNotify) load_older_messages_data_free);
  g_task_run_in_thread (task, load_older_messages_thread);
  g_object_unref (task);
}

/* ============ Process messages from JSON ============ */

/* Helper function to process messages from JsonArray - shared by initial load and polling */
void
gnalk_window_process_messages_array (GnalkWindow *self,
                                       JsonArray    *messages,
                                       gboolean      is_initial_load)
{
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));

  /* Get current user ID from active session (not application, for multi-account support) */
  const char *current_user = NULL;
  if (self->active_session)
    {
      GnalkAccount *account = gnalk_session_get_account (self->active_session);
      if (account)
        current_user = gnalk_account_get_user_id (account);
    }
  guint len = json_array_get_length (messages);

  gboolean initial_load = is_initial_load &&
                          (g_list_model_get_n_items (G_LIST_MODEL (self->messages_store)) == 0);
  gboolean was_at_bottom = initial_load || self->current_room->scroll_to_bottom ||
                           gnalk_message_view_is_at_bottom (self->message_view);

  /* For initial load from server, disconnect model to prevent visual movement */
  if (initial_load && len > 0)
    {
      gtk_no_selection_set_model (self->messages_selection, NULL);
      GNALK_TRACE_UI ("process_messages_array: disconnected model for batch insert (initial_load, %u msgs)", len);
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
      GNALK_TRACE_MESSAGES ("process_messages_array: processing msg %"G_GINT64_FORMAT" systemMessage='%s'",
                            msg_id, system_message);
      if (g_strcmp0 (system_message, "reaction") == 0)
        {
          /* Get the emoji and parent message ID */
          const char *emoji = json_object_get_string_member_with_default (msg, "message", "");
          JsonNode *parent_node = json_object_get_member (msg, "parent");
          if (parent_node && JSON_NODE_HOLDS_OBJECT (parent_node) && *emoji)
            {
              JsonObject *parent = json_node_get_object (parent_node);
              if (parent)
                {
                  gint64 parent_id = json_object_get_int_member_with_default (parent, "id", 0);
                  /* Find the parent message in our store and add the reaction */
                  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
                  for (guint j = 0; j < n_items; j++)
                    {
                      GnalkMessageData *msg_data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), j);
                      if (gnalk_message_data_get_id (msg_data) == parent_id)
                        {
                          gnalk_message_data_add_reaction (msg_data, emoji);
                          g_object_unref (msg_data);
                          break;
                        }
                      g_object_unref (msg_data);
                    }
                }
            }
          continue;  /* Don't create a data entry for reaction messages */
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
                  const char *new_message = json_object_get_string_member_with_default (parent, "message", "");
                  gint64 edit_timestamp = json_object_get_int_member_with_default (parent, "lastEditTimestamp", 0);

                  /* Find and update the original message in our store */
                  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
                  for (guint j = 0; j < n_items; j++)
                    {
                      GnalkMessageData *msg_data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), j);
                      if (gnalk_message_data_get_id (msg_data) == parent_id)
                        {
                          gnalk_message_data_set_message (msg_data, new_message);
                          gnalk_message_data_set_last_edit_timestamp (msg_data, edit_timestamp);

                          /* Force list view to rebind by remove/insert */
                          g_object_ref (msg_data);
                          g_list_store_remove (self->messages_store, j);
                          g_list_store_insert (self->messages_store, j, msg_data);
                          g_object_unref (msg_data);

                          g_object_unref (msg_data);
                          break;
                        }
                      g_object_unref (msg_data);
                    }
                }
            }
          continue;  /* Don't create a data entry for message_edited system messages */
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
                  const char *deleted_message = json_object_get_string_member_with_default (parent, "message", "");

                  /* Find and update the original message in our store */
                  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
                  for (guint j = 0; j < n_items; j++)
                    {
                      GnalkMessageData *msg_data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), j);
                      if (gnalk_message_data_get_id (msg_data) == parent_id)
                        {
                          gnalk_message_data_set_message (msg_data, deleted_message);

                          /* Force list view to rebind by remove/insert */
                          g_object_ref (msg_data);
                          g_list_store_remove (self->messages_store, j);
                          g_list_store_insert (self->messages_store, j, msg_data);
                          g_object_unref (msg_data);

                          g_object_unref (msg_data);
                          break;
                        }
                      g_object_unref (msg_data);
                    }
                }
            }
          continue;  /* Don't create a data entry for message_deleted system messages */
        }

      /* Handle call_left system message - clear call state for actor
       * Skip during initial load - call state is already set from participants API */
      if (!self->current_room->skip_historical_call_messages && g_strcmp0 (system_message, "call_left") == 0)
        {
          const char *actor_id = json_object_get_string_member_with_default (msg, "actorId", "");
          if (actor_id && *actor_id)
            {
              gnalk_signaling_set_call_state (self->signaling, actor_id, 0);
              /* Update participant icon via component */
              gnalk_participants_list_update_call_state (self->participants_list, actor_id, 0);
              /* Clear responded state if this caller left */
              gnalk_incoming_call_banner_clear_responded (self->incoming_call_banner, actor_id);
            }
        }

      /* Handle call_ended system message - clear all call states
       * Skip during initial load - call state is already set from participants API */
      if (!self->current_room->skip_historical_call_messages && g_strcmp0 (system_message, "call_ended") == 0)
        {
          gnalk_signaling_clear_call_states (self->signaling);
          /* Clear all participant call icons via component */
          gnalk_participants_list_clear (self->participants_list);
          /* Clear responded state since call ended */
          gnalk_incoming_call_banner_clear_all_responded (self->incoming_call_banner);
        }

      /* Check if this message matches a pending message by referenceId */
      const char *ref_id = json_object_get_string_member_with_default (msg, "referenceId", "");
      gboolean found_pending = FALSE;

      if (ref_id && *ref_id)
        {
          guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
          for (guint j = 0; j < n_items; j++)
            {
              GnalkMessageData *existing = g_list_model_get_item (G_LIST_MODEL (self->messages_store), j);
              const char *existing_ref = gnalk_message_data_get_reference_id (existing);

              if (existing_ref && g_strcmp0 (existing_ref, ref_id) == 0)
                {
                  /* Found matching pending message - update it */
                  gint64 timestamp = json_object_get_int_member_with_default (msg, "timestamp", 0);
                  gnalk_message_data_confirm_sent (existing, msg_id, timestamp);

                  /* Update message text from server (may differ from local placeholder) */
                  const char *server_message = json_object_get_string_member_with_default (msg, "message", NULL);
                  if (server_message)
                    gnalk_message_data_set_message (existing, server_message);

                  /* Copy messageParameters from server (contains file info) */
                  if (json_object_has_member (msg, "messageParameters"))
                    {
                      JsonNode *params_node = json_object_get_member (msg, "messageParameters");
                      if (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
                        {
                          JsonObject *params = json_node_get_object (params_node);
                          gnalk_message_data_set_message_parameters (existing, params);
                        }
                    }

                  /* If this was a file upload, copy the local file to cache with correct file_id */
                  const char *local_path = gnalk_message_data_get_upload_file_path (existing);
                  if (local_path && *local_path && json_object_has_member (msg, "messageParameters"))
                    {
                      JsonObject *params = json_object_get_object_member (msg, "messageParameters");
                      if (params && json_object_has_member (params, "file"))
                        {
                          JsonObject *file = json_object_get_object_member (params, "file");
                          if (file)
                            {
                              const char *file_id = json_object_get_string_member_with_default (file, "id", NULL);
                              const char *file_name = json_object_get_string_member_with_default (file, "name", NULL);
                              if (file_id && file_name)
                                {
                                  /* Copy local file to cache with correct naming */
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
                  {
                    GnalkDatabase *db = gnalk_application_get_database (app);
                    gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;
                    if (db && account_id > 0 && self->current_room)
                      gnalk_database_save_message (db, account_id, self->current_room->room_token, existing);
                  }

                  /* Force UI update by remove/insert at same position (preserve order while pending) */
                  g_object_ref (existing);
                  g_list_store_remove (self->messages_store, j);
                  g_list_store_insert (self->messages_store, j, existing);
                  g_object_unref (existing);

                  /* Check if any pending messages remain - if not, re-sort the list */
                  {
                    gboolean has_pending = FALSE;
                    guint n = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
                    for (guint k = 0; k < n; k++)
                      {
                        GnalkMessageData *m = g_list_model_get_item (G_LIST_MODEL (self->messages_store), k);
                        if (gnalk_message_data_get_id (m) == 0 || gnalk_message_data_get_is_uploading (m))
                          {
                            has_pending = TRUE;
                            g_object_unref (m);
                            break;
                          }
                        g_object_unref (m);
                      }

                    if (!has_pending)
                      g_list_store_sort (self->messages_store, gnalk_window_compare_messages_by_id, NULL);
                  }

                  found_pending = TRUE;
                  g_object_unref (existing);
                  break;
                }
              g_object_unref (existing);
            }
        }

      if (!found_pending)
        {
          /* Check if message with this ID already exists (avoid duplicates) */
          guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
          gboolean already_exists = FALSE;
          for (guint j = 0; j < n_items; j++)
            {
              GnalkMessageData *existing = g_list_model_get_item (G_LIST_MODEL (self->messages_store), j);
              if (gnalk_message_data_get_id (existing) == msg_id)
                {
                  /* Check if the message has been edited (newer lastEditTimestamp) */
                  gint64 new_edit_ts = json_object_get_int_member_with_default (msg, "lastEditTimestamp", 0);
                  gint64 existing_edit_ts = gnalk_message_data_get_last_edit_timestamp (existing);
                  if (new_edit_ts > existing_edit_ts)
                    {
                      /* Message was edited - update it */
                      const char *new_message = json_object_get_string_member_with_default (msg, "message", "");
                      gnalk_message_data_set_message (existing, new_message);
                      gnalk_message_data_set_last_edit_timestamp (existing, new_edit_ts);

                      /* Force rebind by removing and re-inserting */
                      g_object_ref (existing);
                      g_list_store_remove (self->messages_store, j);
                      g_list_store_insert (self->messages_store, j, existing);
                      g_object_unref (existing);
                    }
                  already_exists = TRUE;
                  g_object_unref (existing);
                  break;
                }
              g_object_unref (existing);
            }

          if (!already_exists)
            {
              GnalkMessageData *msg_data = gnalk_message_data_new_from_json (msg, current_user);

              /* Mark as unread if message ID > last_read_message_id and not our own message */
              if (self->current_room->last_read_message_id > 0 &&
                  msg_id > self->current_room->last_read_message_id &&
                  !gnalk_message_data_get_is_own (msg_data))
                {
                  gnalk_message_data_set_is_unread (msg_data, TRUE);

                  /* Track first unread message for scrolling */
                  if (self->current_room->first_unread_message_id == 0 ||
                      msg_id < self->current_room->first_unread_message_id)
                    {
                      self->current_room->first_unread_message_id = msg_id;
                    }
                }

              guint before_count = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
              g_list_store_insert_sorted (self->messages_store, msg_data,
                                           gnalk_window_compare_messages_by_id, NULL);
              guint after_count = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
              GNALK_TRACE_MESSAGES ("process_messages_array: inserted msg %"G_GINT64_FORMAT" (store %u -> %u)",
                                      msg_id, before_count, after_count);
              g_object_unref (msg_data);
            }
          else
            {
              GNALK_TRACE_MESSAGES ("process_messages_array: msg %"G_GINT64_FORMAT" already exists, skipped", msg_id);
            }
        }
    }

  /* Update date separators and timestamp visibility for all messages after batch processing */
  update_date_separators_and_timestamps (self);

  /* Reconnect model if we disconnected for batch insert */
  if (initial_load && len > 0)
    {
      gtk_no_selection_set_model (self->messages_selection, G_LIST_MODEL (self->messages_store));
      GNALK_TRACE_UI ("process_messages_array: reconnected model with %u items",
                       g_list_model_get_n_items (G_LIST_MODEL (self->messages_store)));
    }

  /* Update read status for messages based on lastCommonReadMessage */
  if (self->current_room->last_common_read_message > 0)
    gnalk_window_update_read_status (self, self->current_room->last_common_read_message);

  self->current_room->last_known_message_id = max_id;

  /* Update tab's newest_loaded_id */
  if (self->tab_manager && self->current_room && max_id > 0)
    {
      GnalkRoomTab *tab = gnalk_room_tab_manager_get_tab (self->tab_manager, self->current_room->room_token);
      if (tab)
        tab->state.last_known_message_id = max_id;
    }

  /* Track oldest message for lazy loading - only on initial load */
  if (initial_load)
    {
      guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
      if (n_items > 0)
        {
          GnalkMessageData *oldest = g_list_model_get_item (G_LIST_MODEL (self->messages_store), 0);
          if (oldest)
            {
              self->current_room->oldest_loaded_id = gnalk_message_data_get_id (oldest);
              g_object_unref (oldest);
            }
        }
      /* If we got fewer messages than expected, history might be exhausted */
      if (len < 50)
        self->current_room->has_more_history = FALSE;
    }

  if (len > 0 && was_at_bottom)
    {
      /* On initial load, scroll to first unread message if there are any */
      if (initial_load && self->current_room->first_unread_message_id > 0)
        {
          /* Find the position of the first unread message */
          guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
          for (guint i = 0; i < n_items; i++)
            {
              GnalkMessageData *data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
              gint64 msg_id = gnalk_message_data_get_id (data);
              g_object_unref (data);

              if (msg_id == self->current_room->first_unread_message_id)
                {
                  /* Show view at unread position (no visual jump) */
                  gnalk_message_view_show_messages_at_item (self->message_view, i);
                  break;
                }
            }
        }
      else
        {
          guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
          if (initial_load && n_items > 0)
            {
              /* Show view at bottom (no visual jump) */
              gnalk_message_view_show_messages_at_item (self->message_view, n_items - 1);
            }
          else
            {
              /* Not initial load - just scroll */
              gnalk_message_view_scroll_to_bottom (self->message_view);
            }
        }
    }

  self->current_room->scroll_to_bottom = FALSE;

  /* Clear the flag after processing first HTTP messages */
  self->current_room->skip_historical_call_messages = FALSE;
}

/* ============ Message Controller Signal Handlers ============ */

void
gnalk_window_on_message_controller_messages_loaded (GnalkMessageController *controller G_GNUC_UNUSED,
                                                      JsonArray               *messages,
                                                      gboolean                 is_initial G_GNUC_UNUSED,
                                                      GnalkWindow            *self)
{
  gnalk_window_process_messages_array (self, messages, TRUE);

  /* Save ALL messages to database for offline access (before trimming display) */
  gnalk_window_save_messages_to_db (self);

  /* Trim display to last 15 messages for initial load (keep full cache in DB) */
  guint total = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  if (total > 15)
    {
      guint to_remove = total - 15;

      /* Disconnect model before trimming to avoid GTK scroll issues */
      gtk_no_selection_set_model (self->messages_selection, NULL);
      g_list_store_splice (self->messages_store, 0, to_remove, NULL, 0);
      gtk_no_selection_set_model (self->messages_selection, G_LIST_MODEL (self->messages_store));

      total = 15;

      /* Update oldest_loaded_message_id to reflect displayed messages */
      GnalkMessageData *oldest = g_list_model_get_item (G_LIST_MODEL (self->messages_store), 0);
      if (oldest)
        {
          self->current_room->oldest_loaded_id = gnalk_message_data_get_id (oldest);
          g_object_unref (oldest);
        }
    }
  g_message ("Messages displayed: %u (after server fetch)", total);

  /* Scroll to bottom after any changes */
  gnalk_message_view_scroll_to_bottom (self->message_view);

  /* Update tab state after server load */
  if (self->tab_manager && self->current_room)
    {
      GnalkRoomTab *tab = gnalk_room_tab_manager_get_tab (self->tab_manager, self->current_room->room_token);
      if (tab)
        {
          tab->state.oldest_loaded_id = self->current_room->oldest_loaded_id;
          tab->state.last_known_message_id = self->current_room->last_known_message_id;
          tab->state.initial_load_done = TRUE;
          tab->state.has_more_history = self->current_room->has_more_history;
        }
    }

  /* Start polling after initial messages are loaded */
  if (!self->current_room->initial_load_done)
    {
      self->current_room->initial_load_done = TRUE;
      gnalk_message_controller_start_polling (self->message_controller,
                                                self->current_room->last_known_message_id);
    }
}

void
gnalk_window_on_message_controller_poll_response (GnalkMessageController *controller G_GNUC_UNUSED,
                                                    JsonArray               *messages,
                                                    GnalkWindow            *self)
{
  guint len = json_array_get_length (messages);
  GNALK_TRACE_POLLING ("poll_response: received %u messages", len);

  /* Verify model is connected before processing */
  GListModel *connected_model = gtk_no_selection_get_model (self->messages_selection);
  GNALK_TRACE_STORE ("poll_response: selection model=%p, messages_store=%p, connected=%d",
                       (void*)connected_model, (void*)self->messages_store,
                       connected_model == G_LIST_MODEL (self->messages_store));

  gnalk_window_process_messages_array (self, messages, FALSE);

  /* Save new messages to database for offline access */
  if (len > 0)
    {
      guint total = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
      g_message ("Messages displayed: %u (+%u from poll)", total, len);
      GNALK_TRACE_DATABASE ("poll_response: saving %u messages to database", len);
      gnalk_window_save_messages_to_db (self);
    }
}

void
gnalk_window_on_message_controller_older_messages_loaded (GnalkMessageController *controller G_GNUC_UNUSED,
                                                            JsonArray               *messages,
                                                            gboolean                 has_more,
                                                            GnalkWindow            *self)
{
  GNALK_TRACE_MESSAGES ("older_messages_loaded: received response, has_more=%d", has_more);
  gnalk_message_view_set_loading_older (self->message_view, FALSE);

  guint len = json_array_get_length (messages);
  if (len == 0)
    {
      self->current_room->has_more_history = FALSE;
      GNALK_TRACE_MESSAGES ("older_messages_loaded: no messages, history exhausted");
      g_debug ("No more messages from server, history exhausted");
      return;
    }

  GNALK_TRACE_MESSAGES ("older_messages_loaded: processing %u older messages", len);
  g_debug ("Received %u older messages via MessageController", len);

  /* Messages are already saved to DB by MessageController.
   * Now process and display them in the UI. */
  /* Get current user ID from active session (not application, for multi-account support) */
  const char *current_user = NULL;
  if (self->active_session)
    {
      GnalkAccount *account = gnalk_session_get_account (self->active_session);
      if (account)
        current_user = gnalk_account_get_user_id (account);
    }

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

  /* Prepend all messages at once using splice - O(n) instead of O(n^2) */
  GNALK_TRACE_STORE ("older_messages_loaded: prepending %u messages to store", new_messages->len);
  g_list_store_splice (self->messages_store, 0, 0,
                       new_messages->pdata, new_messages->len);

  /* Update oldest_loaded_message_id from the oldest message received (now at index 0) */
  GnalkMessageData *oldest = g_ptr_array_index (new_messages, 0);
  self->current_room->oldest_loaded_id = gnalk_message_data_get_id (oldest);
  GNALK_TRACE_MESSAGES ("older_messages_loaded: updated oldest_id=%"G_GINT64_FORMAT, self->current_room->oldest_loaded_id);

  g_ptr_array_free (new_messages, TRUE);

  /* Update has_more_history flag */
  self->current_room->has_more_history = has_more;

  /* Update tab state */
  if (self->tab_manager && self->current_room)
    {
      GnalkRoomTab *tab = gnalk_room_tab_manager_get_tab (self->tab_manager, self->current_room->room_token);
      if (tab)
        {
          tab->state.oldest_loaded_id = self->current_room->oldest_loaded_id;
          tab->state.has_more_history = has_more;
        }
    }

  /* Update date separators and timestamp visibility */
  update_date_separators_and_timestamps (self);
}
