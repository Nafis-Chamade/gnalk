/* gnalk-window-read-status.c
 *
 * Read status tracking extracted from gnalk-window.c.
 * Handles tracking unread messages, marking as read, and glow animation timing.
 */

#include "gnalk-window-private.h"
#include "gnalk-http-client.h"
#include <json-glib/json-glib.h>

/* Forward declaration for internal callback */
static void on_bound_unread_row_destroyed (gpointer data, GObject *where_the_object_was);

/* ============ Cleanup helper ============ */

/* Clear all read status tracking state (called when switching rooms or disposing) */
void
gnalk_window_clear_read_status_tracking (GnalkWindow *self)
{
  /* Remove weak refs from all tracked rows */
  if (self->bound_unread_rows)
    {
      GHashTableIter iter;
      gpointer key, value;
      g_hash_table_iter_init (&iter, self->bound_unread_rows);
      while (g_hash_table_iter_next (&iter, &key, &value))
        g_object_weak_unref (G_OBJECT (value), on_bound_unread_row_destroyed, self);
      g_hash_table_remove_all (self->bound_unread_rows);
    }

  /* Clear visible unread list */
  if (self->visible_unread_ids)
    g_array_set_size (self->visible_unread_ids, 0);

  /* Cancel timers */
  if (self->mark_read_debounce_id > 0)
    {
      g_source_remove (self->mark_read_debounce_id);
      self->mark_read_debounce_id = 0;
    }
  if (self->mark_read_timer_id > 0)
    {
      g_source_remove (self->mark_read_timer_id);
      self->mark_read_timer_id = 0;
    }
}

/* ============ Update message read status ============ */

/* Update message read status based on lastCommonReadMessage */
void
gnalk_window_update_read_status (GnalkWindow *self, gint64 last_common_read_message)
{
  if (last_common_read_message <= 0)
    return;

  self->last_common_read_message = last_common_read_message;

  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
      gint64 msg_id = gnalk_message_data_get_id (data);
      GnalkMessageDataStatus current_status = gnalk_message_data_get_status (data);

      /* Only update own messages that are in SENT status */
      if (current_status == GNALK_MESSAGE_DATA_STATUS_SENT && msg_id > 0)
        {
          if (msg_id <= last_common_read_message)
            gnalk_message_data_set_status (data, GNALK_MESSAGE_DATA_STATUS_READ);
        }
      g_object_unref (data);
    }
}

/* ============ Mark chat read API ============ */

/* Callback for mark_chat_read API response */
static void
on_mark_chat_read_response (GnalkHttpClient *client G_GNUC_UNUSED,
                            JsonNode         *response,
                            GError           *error,
                            gpointer          user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  g_debug ("read-status: mark_chat_read response received");

  if (error)
    {
      g_debug ("read-status: mark_chat_read failed: %s", error->message);
      return;
    }

  /* Parse response to update conversation's unread count
   * Response format: {"ocs":{"data":{...room data with unreadMessages, token, etc...}}} */
  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    {
      g_debug ("read-status: invalid response (no object)");
      return;
    }

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    {
      g_debug ("read-status: invalid response (no ocs)");
      return;
    }

  JsonObject *data = json_object_get_object_member (ocs, "data");
  if (!data)
    {
      g_debug ("read-status: invalid response (no data)");
      return;
    }

  const char *token = json_object_get_string_member_with_default (data, "token", NULL);
  gint64 unread = json_object_get_int_member_with_default (data, "unreadMessages", -1);
  g_debug ("read-status: response token=%s, unreadMessages=%ld",
           token ? token : "(null)", (long) unread);

  if (!token)
    {
      g_debug ("read-status: invalid response (no token)");
      return;
    }

  /* Find and update the conversation in our store */
  guint n_convos = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
  g_debug ("read-status: searching %u conversations for token=%s", n_convos, token);

  gboolean found = FALSE;
  for (guint i = 0; i < n_convos; i++)
    {
      GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
      const char *conv_token = gnalk_conversation_data_get_token (conv);

      if (conv_token && g_strcmp0 (conv_token, token) == 0)
        {
          g_debug ("read-status: found conversation at index %u, updating", i);
          /* Update the conversation data in-place (emits "updated" signal) */
          gnalk_conversation_data_update_from_json (conv, data);
          found = TRUE;
          g_object_unref (conv);
          break;
        }
      g_object_unref (conv);
    }

  if (!found)
    g_debug ("read-status: conversation with token=%s not found in store!", token);
}

/* ============ Mark messages read up to ID ============ */

/* Forward declarations for internal callbacks */
static gboolean on_mark_read_delay_timer (gpointer user_data);

/* Mark messages as read up to the given message ID */
void
gnalk_window_mark_messages_read_up_to (GnalkWindow *self, gint64 message_id)
{
  if (message_id <= self->last_read_message_id)
    return;

  self->last_read_message_id = message_id;

  /* Update local message states */
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
      gint64 msg_id = gnalk_message_data_get_id (data);

      if (msg_id <= message_id && gnalk_message_data_get_is_unread (data))
        gnalk_message_data_set_is_unread (data, FALSE);

      g_object_unref (data);
    }

  /* Update first_unread_message_id */
  if (self->first_unread_message_id > 0 && self->first_unread_message_id <= message_id)
    {
      /* Find new first unread */
      self->first_unread_message_id = 0;
      for (guint i = 0; i < n_items; i++)
        {
          GnalkMessageData *data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
          if (gnalk_message_data_get_is_unread (data))
            {
              gint64 msg_id = gnalk_message_data_get_id (data);
              if (self->first_unread_message_id == 0 || msg_id < self->first_unread_message_id)
                self->first_unread_message_id = msg_id;
            }
          g_object_unref (data);
        }
    }

  /* Call API to mark as read on server */
  if (self->current_room_token && self->http_client)
    {
      gnalk_http_client_mark_chat_read (self->http_client,
                                          self->current_room_token,
                                          message_id,
                                          on_mark_chat_read_response,
                                          self);
    }
}

/* ============ Viewport visibility checking ============ */

/* Check if a row widget is actually visible in the viewport (not just bound) */
static gboolean
is_row_visible_in_viewport (GnalkWindow *self, GtkWidget *row)
{
  GtkScrolledWindow *scroll = gnalk_message_view_get_scrolled_window (self->message_view);
  if (!scroll || !row)
    return FALSE;

  /* Get viewport page size */
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (scroll);
  gdouble page_size = gtk_adjustment_get_page_size (vadj);

  /* Get row bounds relative to scrolled window */
  graphene_rect_t bounds;
  if (!gtk_widget_compute_bounds (row, GTK_WIDGET (scroll), &bounds))
    return FALSE;

  gdouble row_top = graphene_rect_get_y (&bounds);
  gdouble row_height = graphene_rect_get_height (&bounds);
  gdouble row_bottom = row_top + row_height;

  /* Filter out placeholder/recycled rows that haven't been laid out yet.
   * Real message rows are at least 40px tall. Rows with tiny heights are
   * pre-bound but not yet rendered by GTK ListView. */
  if (row_height < 40.0)
    return FALSE;

  /* Row is visible if it overlaps with the viewport (0 to page_size) */
  return (row_bottom > 0) && (row_top < page_size);
}

/* ============ Timer callbacks ============ */

/* Timer callback for removing glow after animation completes (HTTP already sent) */
static gboolean
on_mark_read_delay_timer (gpointer user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  self->mark_read_timer_id = 0;

  if (self->visible_unread_ids->len == 0)
    return G_SOURCE_REMOVE;

  /* Remove glow from visible messages (HTTP was already sent in debounce callback) */
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));

  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
      gint64 msg_id = gnalk_message_data_get_id (data);

      /* Check if this message ID is in our visible unread list */
      for (guint j = 0; j < self->visible_unread_ids->len; j++)
        {
          gint64 visible_id = g_array_index (self->visible_unread_ids, gint64, j);
          if (msg_id == visible_id)
            {
              gnalk_message_data_set_is_unread (data, FALSE);
              break;
            }
        }

      g_object_unref (data);
    }

  /* Update first_unread_message_id by finding new first unread */
  self->first_unread_message_id = 0;
  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
      if (gnalk_message_data_get_is_unread (data))
        {
          gint64 msg_id = gnalk_message_data_get_id (data);
          if (self->first_unread_message_id == 0 || msg_id < self->first_unread_message_id)
            self->first_unread_message_id = msg_id;
        }
      g_object_unref (data);
    }

  /* Clear the visible list - glow removed */
  g_array_set_size (self->visible_unread_ids, 0);

  return G_SOURCE_REMOVE;
}

/* Debounce callback - fires after list settles, checks actual visibility */
static gboolean
on_mark_read_debounce_timer (gpointer user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  self->mark_read_debounce_id = 0;

  g_debug ("read-status: debounce timer fired, %u bound unread rows",
           g_hash_table_size (self->bound_unread_rows));

  if (g_hash_table_size (self->bound_unread_rows) == 0)
    return G_SOURCE_REMOVE;

  /* Clear previous visible list */
  g_array_set_size (self->visible_unread_ids, 0);

  /* Check each bound row to see if it's actually visible in viewport */
  gint64 highest_id = 0;
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, self->bound_unread_rows);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      gint64 msg_id = GPOINTER_TO_INT (key);
      GtkWidget *row = GTK_WIDGET (value);

      if (is_row_visible_in_viewport (self, row))
        {
          g_array_append_val (self->visible_unread_ids, msg_id);
          if (msg_id > highest_id)
            highest_id = msg_id;
        }
    }

  g_debug ("read-status: %u visible unread messages, highest_id=%ld",
           self->visible_unread_ids->len, (long) highest_id);

  if (self->visible_unread_ids->len == 0)
    return G_SOURCE_REMOVE;

  /* Fire HTTP call immediately to mark as read on server */
  if (highest_id > self->last_read_message_id &&
      self->current_room_token && self->http_client)
    {
      g_debug ("read-status: calling mark_chat_read for token=%s, msg_id=%ld (last_read=%ld)",
               self->current_room_token, (long) highest_id, (long) self->last_read_message_id);
      self->last_read_message_id = highest_id;
      gnalk_http_client_mark_chat_read (self->http_client,
                                          self->current_room_token,
                                          highest_id,
                                          on_mark_chat_read_response,
                                          self);
    }
  else
    {
      g_debug ("read-status: skipping HTTP call (highest_id=%ld <= last_read=%ld, token=%s, client=%p)",
               (long) highest_id, (long) self->last_read_message_id,
               self->current_room_token ? self->current_room_token : "(null)",
               (void *) self->http_client);
    }

  /* Cancel any existing glow timer and start fresh - glow continues for 4s */
  if (self->mark_read_timer_id > 0)
    g_source_remove (self->mark_read_timer_id);

  self->mark_read_timer_id = g_timeout_add (4000, on_mark_read_delay_timer, self);

  return G_SOURCE_REMOVE;
}

/* ============ Bound row tracking ============ */

/* Called when a tracked unread row widget is destroyed (e.g., ListView recycling) */
static void
on_bound_unread_row_destroyed (gpointer data, GObject *where_the_object_was)
{
  GnalkWindow *self = GNALK_WINDOW (data);

  /* Find and remove the entry with this widget pointer */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, self->bound_unread_rows);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (value == where_the_object_was)
        {
          g_hash_table_iter_remove (&iter);
          break;
        }
    }
}

/* Called when an unread message becomes bound (may or may not be visible) */
void
gnalk_window_track_bound_unread_row (GnalkWindow *self, gint64 msg_id, GtkWidget *row)
{
  g_debug ("read-status: tracking unread msg_id=%ld", (long) msg_id);

  /* Remove any existing weak ref if this msg_id was already tracked */
  gpointer old_row = g_hash_table_lookup (self->bound_unread_rows, GINT_TO_POINTER (msg_id));
  if (old_row != NULL)
    g_object_weak_unref (G_OBJECT (old_row), on_bound_unread_row_destroyed, self);

  g_object_weak_ref (G_OBJECT (row), on_bound_unread_row_destroyed, self);
  g_hash_table_insert (self->bound_unread_rows, GINT_TO_POINTER (msg_id), row);

  /* Reset the debounce timer - wait for list to settle before checking visibility */
  if (self->mark_read_debounce_id > 0)
    g_source_remove (self->mark_read_debounce_id);

  self->mark_read_debounce_id = g_timeout_add (500, on_mark_read_debounce_timer, self);
}

/* Called when an unread message row is unbound */
void
gnalk_window_untrack_bound_unread_row (GnalkWindow *self, gint64 msg_id)
{
  /* Remove weak ref before removing from hash table */
  gpointer row = g_hash_table_lookup (self->bound_unread_rows, GINT_TO_POINTER (msg_id));
  if (row != NULL)
    g_object_weak_unref (G_OBJECT (row), on_bound_unread_row_destroyed, self);

  g_hash_table_remove (self->bound_unread_rows, GINT_TO_POINTER (msg_id));

  /* Also remove from visible list if present */
  for (guint i = 0; i < self->visible_unread_ids->len; i++)
    {
      if (g_array_index (self->visible_unread_ids, gint64, i) == msg_id)
        {
          g_array_remove_index_fast (self->visible_unread_ids, i);
          break;
        }
    }

  /* If no more bound unread messages, cancel all timers */
  if (g_hash_table_size (self->bound_unread_rows) == 0)
    {
      if (self->mark_read_debounce_id > 0)
        {
          g_source_remove (self->mark_read_debounce_id);
          self->mark_read_debounce_id = 0;
        }
      if (self->mark_read_timer_id > 0)
        {
          g_source_remove (self->mark_read_timer_id);
          self->mark_read_timer_id = 0;
        }
    }
}

/* ============ Signal handlers ============ */

/* Called when user scrolls - re-check visibility for bound unread messages */
void
gnalk_window_on_message_scroll_changed (GnalkMessageView *view,
                                         gpointer           user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  (void) view;

  /* Only re-check if we have bound unread messages that haven't been marked read yet */
  if (g_hash_table_size (self->bound_unread_rows) == 0)
    return;

  /* Don't re-trigger if glow timer is already running (messages already detected as visible) */
  if (self->mark_read_timer_id > 0)
    return;

  /* Reset debounce timer to re-check visibility after scroll settles */
  if (self->mark_read_debounce_id > 0)
    g_source_remove (self->mark_read_debounce_id);

  self->mark_read_debounce_id = g_timeout_add (500, on_mark_read_debounce_timer, self);
}

/* Handle scroll-to-bottom button click - mark all as read */
void
gnalk_window_on_scroll_to_bottom_clicked_for_unread (GnalkMessageView *view G_GNUC_UNUSED,
                                                      GnalkWindow      *self)
{
  /* Mark all messages as read when user clicks scroll-to-bottom */
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  if (n_items > 0)
    {
      GnalkMessageData *last = g_list_model_get_item (G_LIST_MODEL (self->messages_store), n_items - 1);
      gint64 last_id = gnalk_message_data_get_id (last);
      g_object_unref (last);

      if (last_id > self->last_read_message_id)
        gnalk_window_mark_messages_read_up_to (self, last_id);
    }
}
