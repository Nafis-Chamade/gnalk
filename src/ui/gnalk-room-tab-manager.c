/* gnalk-room-tab-manager.c
 *
 * Tab-based room message management implementation.
 */

#include "gnalk-room-tab-manager.h"
#include "gnalk-message-row.h"
#include "gnalk-log.h"

struct _GnalkRoomTabManager
{
  GObject parent_instance;

  GHashTable *tabs;             /* room_token -> GnalkRoomTab* */
  guint       max_tabs;         /* Configurable limit (0 = unlimited) */
  gdouble     height_multiplier; /* Viewports of messages to keep */
};

G_DEFINE_FINAL_TYPE (GnalkRoomTabManager, gnalk_room_tab_manager, G_TYPE_OBJECT)

/* Compare function for sorting messages in store - sorts by ID (chronological) */
static gint
compare_messages_by_id (gconstpointer a,
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

static GnalkRoomTab *
room_tab_new (const char *room_token)
{
  GNALK_TRACE_ROOM_SWITCH ("room_tab_new: creating tab for room %s", room_token);

  GnalkRoomTab *tab = g_new0 (GnalkRoomTab, 1);
  tab->state.room_token = g_strdup (room_token);
  tab->state.messages_store = g_list_store_new (GNALK_TYPE_MESSAGE_DATA);
  tab->state.oldest_loaded_id = 0;
  tab->state.last_known_message_id = 0;
  tab->state.anchor_message_id = 0;
  tab->state.anchor_offset = 0.0;
  tab->state.is_at_bottom = TRUE;
  tab->last_access_time = g_get_monotonic_time ();
  tab->state.has_more_history = TRUE;
  tab->state.initial_load_done = FALSE;
  tab->state.last_read_message_id = 0;

  GNALK_TRACE_STORE ("room_tab_new: created GListStore %p for room %s", (void*)tab->state.messages_store, room_token);
  return tab;
}

static void
room_tab_free (GnalkRoomTab *tab)
{
  if (!tab)
    return;

  GNALK_TRACE_ROOM_SWITCH ("room_tab_free: freeing tab for room %s", tab->state.room_token);
  GNALK_TRACE_STORE ("room_tab_free: clearing GListStore %p", (void*)tab->state.messages_store);

  g_free (tab->state.room_token);
  g_free (tab->state.room_name);
  g_free (tab->state.room_description);
  g_free (tab->state.session_id);
  g_clear_object (&tab->state.messages_store);
  g_free (tab);
}

static void
update_tab_tracking_ids (GnalkRoomTab *tab)
{
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (tab->state.messages_store));

  GNALK_TRACE_STORE ("update_tab_tracking_ids: room %s, %u items in store", tab->state.room_token, n_items);

  if (n_items == 0)
    {
      tab->state.oldest_loaded_id = 0;
      tab->state.last_known_message_id = 0;
      GNALK_TRACE_STORE ("update_tab_tracking_ids: store empty, reset IDs to 0");
      return;
    }

  /* Get oldest (first) message */
  GnalkMessageData *oldest = g_list_model_get_item (G_LIST_MODEL (tab->state.messages_store), 0);
  if (oldest)
    {
      tab->state.oldest_loaded_id = gnalk_message_data_get_id (oldest);
      g_object_unref (oldest);
    }

  /* Get newest (last non-pending) message */
  for (gint i = (gint) n_items - 1; i >= 0; i--)
    {
      GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (tab->state.messages_store), i);
      if (msg)
        {
          gint64 msg_id = gnalk_message_data_get_id (msg);
          if (msg_id > 0)  /* Skip pending messages */
            {
              tab->state.last_known_message_id = msg_id;
              g_object_unref (msg);
              break;
            }
          g_object_unref (msg);
        }
    }

  GNALK_TRACE_STORE ("update_tab_tracking_ids: oldest=%"G_GINT64_FORMAT", newest=%"G_GINT64_FORMAT,
                      tab->state.oldest_loaded_id, tab->state.last_known_message_id);
}

static guint
find_message_position (GListStore *store, gint64 message_id)
{
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (store));

  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (store), i);
      if (msg)
        {
          gint64 msg_id = gnalk_message_data_get_id (msg);
          g_object_unref (msg);
          if (msg_id == message_id)
            return i;
        }
    }

  return G_MAXUINT;  /* Not found */
}

/* Measure message height using gtk_widget_measure on an offscreen row */
static int
measure_message_height (GnalkMessageRow *measure_row, int available_width)
{
  int min_height, natural_height;
  gtk_widget_measure (GTK_WIDGET (measure_row),
                      GTK_ORIENTATION_VERTICAL,
                      available_width,
                      &min_height,
                      &natural_height,
                      NULL, NULL);
  return natural_height;
}

/* Trim messages outside the height window around anchor */
static void
trim_tab_messages (GnalkRoomTab *tab,
                   int            target_height,
                   int            viewport_width)
{
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (tab->state.messages_store));

  GNALK_TRACE_STORE ("trim_tab_messages: room %s, %u items, target_height=%d, viewport_width=%d",
                      tab->state.room_token, n_items, target_height, viewport_width);

  if (n_items == 0 || target_height <= 0 || viewport_width <= 0)
    {
      GNALK_TRACE_STORE ("trim_tab_messages: skipping (empty or no dimensions)");
      return;
    }

  /* Create measure row for height calculation */
  GnalkMessageRow *measure_row = gnalk_message_row_new ();

  /* Find anchor position */
  guint anchor_pos = G_MAXUINT;
  if (tab->state.anchor_message_id > 0)
    anchor_pos = find_message_position (tab->state.messages_store, tab->state.anchor_message_id);

  if (anchor_pos == G_MAXUINT)
    anchor_pos = n_items - 1;  /* Default to end if anchor not found */

  /* Measure outward from anchor until height budget exhausted */
  int above_height = 0, below_height = 0;
  int half_budget = target_height / 2;
  guint keep_start = anchor_pos, keep_end = anchor_pos;

  /* Expand upward from anchor */
  for (gint i = (gint) anchor_pos; i >= 0 && above_height < half_budget; i--)
    {
      GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (tab->state.messages_store), i);
      if (msg)
        {
          gnalk_message_row_set_from_data (measure_row, msg, NULL, NULL);
          above_height += measure_message_height (measure_row, viewport_width);
          keep_start = i;
          g_object_unref (msg);
        }
    }

  /* Expand downward from anchor */
  for (guint i = anchor_pos + 1; i < n_items && below_height < half_budget; i++)
    {
      GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (tab->state.messages_store), i);
      if (msg)
        {
          gnalk_message_row_set_from_data (measure_row, msg, NULL, NULL);
          below_height += measure_message_height (measure_row, viewport_width);
          keep_end = i;
          g_object_unref (msg);
        }
    }

  g_object_ref_sink (measure_row);
  g_object_unref (measure_row);

  /* Remove messages outside the keep window */
  /* Remove from end first to avoid index shifting issues */
  if (keep_end + 1 < n_items)
    {
      guint remove_count = n_items - keep_end - 1;
      g_list_store_splice (tab->state.messages_store, keep_end + 1, remove_count, NULL, 0);
    }

  if (keep_start > 0)
    g_list_store_splice (tab->state.messages_store, 0, keep_start, NULL, 0);

  /* Update tracking IDs */
  update_tab_tracking_ids (tab);

  guint remaining = g_list_model_get_n_items (G_LIST_MODEL (tab->state.messages_store));
  GNALK_TRACE_STORE ("trim_tab_messages: trimmed from %u to %u messages, kept range [%u, %u]",
                      n_items, remaining, keep_start, keep_end);
  g_debug ("Trimmed tab %s: kept %u messages around anchor",
           tab->state.room_token, remaining);
}

/* Find tab with oldest last_access_time */
static const char *
find_oldest_tab (GnalkRoomTabManager *self)
{
  const char *oldest_token = NULL;
  gint64 oldest_time = G_MAXINT64;

  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, self->tabs);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      GnalkRoomTab *tab = value;
      if (tab->last_access_time < oldest_time)
        {
          oldest_time = tab->last_access_time;
          oldest_token = key;
        }
    }

  return oldest_token;
}

static void
gnalk_room_tab_manager_dispose (GObject *object)
{
  GnalkRoomTabManager *self = GNALK_ROOM_TAB_MANAGER (object);

  g_clear_pointer (&self->tabs, g_hash_table_destroy);

  G_OBJECT_CLASS (gnalk_room_tab_manager_parent_class)->dispose (object);
}

static void
gnalk_room_tab_manager_class_init (GnalkRoomTabManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gnalk_room_tab_manager_dispose;
}

static void
gnalk_room_tab_manager_init (GnalkRoomTabManager *self)
{
  self->tabs = g_hash_table_new_full (g_str_hash, g_str_equal,
                                       g_free, (GDestroyNotify) room_tab_free);
  self->max_tabs = 5;
  self->height_multiplier = 3.0;
}

GnalkRoomTabManager *
gnalk_room_tab_manager_new (guint   max_tabs,
                              gdouble height_multiplier)
{
  GnalkRoomTabManager *self = g_object_new (GNALK_TYPE_ROOM_TAB_MANAGER, NULL);
  self->max_tabs = max_tabs;
  self->height_multiplier = height_multiplier;
  return self;
}

GnalkRoomTab *
gnalk_room_tab_manager_get_tab (GnalkRoomTabManager *self,
                                  const char           *room_token)
{
  g_return_val_if_fail (GNALK_IS_ROOM_TAB_MANAGER (self), NULL);
  g_return_val_if_fail (room_token != NULL, NULL);

  GNALK_TRACE_ROOM_SWITCH ("get_tab: looking up room %s", room_token);

  GnalkRoomTab *tab = g_hash_table_lookup (self->tabs, room_token);

  if (!tab)
    {
      GNALK_TRACE_ROOM_SWITCH ("get_tab: tab not found, creating new tab for %s", room_token);
      tab = room_tab_new (room_token);
      g_hash_table_insert (self->tabs, g_strdup (room_token), tab);
      g_debug ("Created new tab for room %s", room_token);
    }
  else
    {
      GNALK_TRACE_ROOM_SWITCH ("get_tab: found existing tab for %s with %u messages",
                                room_token, g_list_model_get_n_items (G_LIST_MODEL (tab->state.messages_store)));
    }

  /* Update last access time */
  tab->last_access_time = g_get_monotonic_time ();

  return tab;
}

gboolean
gnalk_room_tab_manager_has_tab (GnalkRoomTabManager *self,
                                  const char           *room_token)
{
  g_return_val_if_fail (GNALK_IS_ROOM_TAB_MANAGER (self), FALSE);
  g_return_val_if_fail (room_token != NULL, FALSE);

  return g_hash_table_contains (self->tabs, room_token);
}

void
gnalk_room_tab_manager_background_tab (GnalkRoomTabManager *self,
                                         const char           *room_token,
                                         gint64                anchor_id,
                                         gdouble               anchor_offset,
                                         gboolean              at_bottom,
                                         int                   viewport_height,
                                         int                   viewport_width)
{
  g_return_if_fail (GNALK_IS_ROOM_TAB_MANAGER (self));
  g_return_if_fail (room_token != NULL);

  GNALK_TRACE_ROOM_SWITCH ("background_tab: backgrounding room %s", room_token);

  GnalkRoomTab *tab = g_hash_table_lookup (self->tabs, room_token);
  if (!tab)
    {
      GNALK_TRACE_ROOM_SWITCH ("background_tab: no tab found for %s, skipping", room_token);
      return;
    }

  /* Save scroll anchor */
  tab->state.anchor_message_id = anchor_id;
  tab->state.anchor_offset = anchor_offset;
  tab->state.is_at_bottom = at_bottom;

  GNALK_TRACE_SCROLL ("background_tab: saved anchor=%"G_GINT64_FORMAT", offset=%.1f, at_bottom=%d",
                       anchor_id, anchor_offset, at_bottom);
  g_debug ("Backgrounding tab %s: anchor=%"G_GINT64_FORMAT", offset=%.1f, at_bottom=%d",
           room_token, anchor_id, anchor_offset, at_bottom);

  /* Trim messages if we have viewport dimensions */
  /* When user was at bottom, skip trimming to preserve all recent messages.
   * This prevents returning to a room and finding messages missing. */
  if (at_bottom)
    {
      guint n_msgs = g_list_model_get_n_items (G_LIST_MODEL (tab->state.messages_store));
      GNALK_TRACE_STORE ("background_tab: at_bottom=TRUE, skipping trim to preserve %u messages", n_msgs);
      g_debug ("Skipping trim for tab %s (at bottom), preserving %u messages", room_token, n_msgs);
    }
  else if (viewport_height > 0 && viewport_width > 0 && self->height_multiplier > 0)
    {
      int target_height = (int) (viewport_height * self->height_multiplier);
      GNALK_TRACE_STORE ("background_tab: trimming messages to target_height=%d (multiplier=%.1f)",
                          target_height, self->height_multiplier);
      trim_tab_messages (tab, target_height, viewport_width);
    }
  else
    {
      GNALK_TRACE_STORE ("background_tab: skipping trim (viewport=%dx%d, multiplier=%.1f)",
                          viewport_width, viewport_height, self->height_multiplier);
    }
}

void
gnalk_room_tab_manager_foreground_tab (GnalkRoomTabManager *self,
                                         const char           *room_token,
                                         gint64               *anchor_id,
                                         gdouble              *anchor_offset,
                                         gboolean             *was_at_bottom)
{
  g_return_if_fail (GNALK_IS_ROOM_TAB_MANAGER (self));
  g_return_if_fail (room_token != NULL);

  GNALK_TRACE_ROOM_SWITCH ("foreground_tab: foregrounding room %s", room_token);

  GnalkRoomTab *tab = gnalk_room_tab_manager_get_tab (self, room_token);

  /* Update access time */
  tab->last_access_time = g_get_monotonic_time ();

  /* Return anchor info */
  if (anchor_id)
    *anchor_id = tab->state.anchor_message_id;
  if (anchor_offset)
    *anchor_offset = tab->state.anchor_offset;
  if (was_at_bottom)
    *was_at_bottom = tab->state.is_at_bottom;

  guint n_messages = g_list_model_get_n_items (G_LIST_MODEL (tab->state.messages_store));
  GNALK_TRACE_SCROLL ("foreground_tab: restoring anchor=%"G_GINT64_FORMAT", offset=%.1f, at_bottom=%d",
                       tab->state.anchor_message_id, tab->state.anchor_offset, tab->state.is_at_bottom);
  GNALK_TRACE_STORE ("foreground_tab: tab has %u messages, initial_load_done=%d",
                      n_messages, tab->state.initial_load_done);
  g_debug ("Foregrounding tab %s: anchor=%"G_GINT64_FORMAT", at_bottom=%d, messages=%u",
           room_token, tab->state.anchor_message_id, tab->state.is_at_bottom, n_messages);
}

void
gnalk_room_tab_manager_enforce_limit (GnalkRoomTabManager *self)
{
  g_return_if_fail (GNALK_IS_ROOM_TAB_MANAGER (self));

  if (self->max_tabs == 0)
    {
      GNALK_TRACE_ROOM_SWITCH ("enforce_limit: unlimited tabs, skipping");
      return;  /* Unlimited */
    }

  guint current_count = g_hash_table_size (self->tabs);
  GNALK_TRACE_ROOM_SWITCH ("enforce_limit: %u tabs, max=%u", current_count, self->max_tabs);

  while (g_hash_table_size (self->tabs) > self->max_tabs)
    {
      const char *oldest_token = find_oldest_tab (self);
      if (oldest_token)
        {
          GNALK_TRACE_ROOM_SWITCH ("enforce_limit: evicting oldest tab %s", oldest_token);
          g_debug ("Evicting oldest tab: %s", oldest_token);
          g_hash_table_remove (self->tabs, oldest_token);
        }
      else
        {
          break;  /* No tabs to remove */
        }
    }
}

void
gnalk_room_tab_manager_append_messages (GnalkRoomTabManager *self,
                                          const char           *room_token,
                                          GPtrArray            *messages)
{
  g_return_if_fail (GNALK_IS_ROOM_TAB_MANAGER (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (messages != NULL);

  GNALK_TRACE_MESSAGES ("append_messages: appending %u messages to room %s", messages->len, room_token);

  if (messages->len == 0)
    {
      GNALK_TRACE_MESSAGES ("append_messages: no messages to append");
      return;
    }

  GnalkRoomTab *tab = gnalk_room_tab_manager_get_tab (self, room_token);
  guint before_count = g_list_model_get_n_items (G_LIST_MODEL (tab->state.messages_store));
  guint added = 0;

  for (guint i = 0; i < messages->len; i++)
    {
      GnalkMessageData *msg = g_ptr_array_index (messages, i);
      if (!GNALK_IS_MESSAGE_DATA (msg))
        continue;

      gint64 msg_id = gnalk_message_data_get_id (msg);

      /* Check if message already exists */
      gboolean exists = FALSE;
      guint n_items = g_list_model_get_n_items (G_LIST_MODEL (tab->state.messages_store));
      for (guint j = 0; j < n_items; j++)
        {
          GnalkMessageData *existing = g_list_model_get_item (G_LIST_MODEL (tab->state.messages_store), j);
          if (existing)
            {
              if (gnalk_message_data_get_id (existing) == msg_id)
                {
                  exists = TRUE;
                  g_object_unref (existing);
                  break;
                }
              g_object_unref (existing);
            }
        }

      if (!exists)
        {
          g_list_store_insert_sorted (tab->state.messages_store, msg, compare_messages_by_id, NULL);
          added++;
        }
    }

  guint after_count = g_list_model_get_n_items (G_LIST_MODEL (tab->state.messages_store));
  GNALK_TRACE_STORE ("append_messages: added %u messages (before=%u, after=%u)", added, before_count, after_count);

  update_tab_tracking_ids (tab);
}

void
gnalk_room_tab_manager_prepend_messages (GnalkRoomTabManager *self,
                                           const char           *room_token,
                                           GPtrArray            *messages)
{
  g_return_if_fail (GNALK_IS_ROOM_TAB_MANAGER (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (messages != NULL);

  GNALK_TRACE_MESSAGES ("prepend_messages: prepending %u messages to room %s", messages->len, room_token);

  if (messages->len == 0)
    {
      GNALK_TRACE_MESSAGES ("prepend_messages: no messages to prepend");
      return;
    }

  GnalkRoomTab *tab = gnalk_room_tab_manager_get_tab (self, room_token);
  guint before_count = g_list_model_get_n_items (G_LIST_MODEL (tab->state.messages_store));

  /* Filter out messages that already exist */
  GPtrArray *new_messages = g_ptr_array_new ();

  for (guint i = 0; i < messages->len; i++)
    {
      GnalkMessageData *msg = g_ptr_array_index (messages, i);
      if (!GNALK_IS_MESSAGE_DATA (msg))
        continue;

      gint64 msg_id = gnalk_message_data_get_id (msg);

      /* Check if message already exists */
      gboolean exists = FALSE;
      guint n_items = g_list_model_get_n_items (G_LIST_MODEL (tab->state.messages_store));
      for (guint j = 0; j < n_items; j++)
        {
          GnalkMessageData *existing = g_list_model_get_item (G_LIST_MODEL (tab->state.messages_store), j);
          if (existing)
            {
              if (gnalk_message_data_get_id (existing) == msg_id)
                {
                  exists = TRUE;
                  g_object_unref (existing);
                  break;
                }
              g_object_unref (existing);
            }
        }

      if (!exists)
        g_ptr_array_add (new_messages, msg);
    }

  /* Prepend all new messages at position 0 using splice */
  guint added = new_messages->len;
  if (new_messages->len > 0)
    g_list_store_splice (tab->state.messages_store, 0, 0, new_messages->pdata, new_messages->len);

  g_ptr_array_free (new_messages, TRUE);

  guint after_count = g_list_model_get_n_items (G_LIST_MODEL (tab->state.messages_store));
  GNALK_TRACE_STORE ("prepend_messages: added %u messages (before=%u, after=%u)", added, before_count, after_count);

  update_tab_tracking_ids (tab);
}

void
gnalk_room_tab_manager_remove_tab (GnalkRoomTabManager *self,
                                     const char           *room_token)
{
  g_return_if_fail (GNALK_IS_ROOM_TAB_MANAGER (self));
  g_return_if_fail (room_token != NULL);

  GNALK_TRACE_ROOM_SWITCH ("remove_tab: removing tab for room %s", room_token);
  g_hash_table_remove (self->tabs, room_token);
  g_debug ("Removed tab for room %s", room_token);
}

guint
gnalk_room_tab_manager_get_tab_count (GnalkRoomTabManager *self)
{
  g_return_val_if_fail (GNALK_IS_ROOM_TAB_MANAGER (self), 0);

  return g_hash_table_size (self->tabs);
}

void
gnalk_room_tab_manager_set_max_tabs (GnalkRoomTabManager *self,
                                       guint                 max_tabs)
{
  g_return_if_fail (GNALK_IS_ROOM_TAB_MANAGER (self));

  self->max_tabs = max_tabs;
  gnalk_room_tab_manager_enforce_limit (self);
}

void
gnalk_room_tab_manager_set_height_multiplier (GnalkRoomTabManager *self,
                                                gdouble               multiplier)
{
  g_return_if_fail (GNALK_IS_ROOM_TAB_MANAGER (self));

  self->height_multiplier = multiplier;
}

void
gnalk_room_tab_manager_clear_all (GnalkRoomTabManager *self)
{
  g_return_if_fail (GNALK_IS_ROOM_TAB_MANAGER (self));

  guint count = g_hash_table_size (self->tabs);
  g_debug ("Clearing all %u tabs from tab manager", count);
  g_hash_table_remove_all (self->tabs);
}
