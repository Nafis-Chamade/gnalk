/* gnalk-window-conversation-list.c
 *
 * Conversation list management extracted from gnalk-window.c.
 * Handles loading conversations from DB, search, list factories,
 * and conversation controller signal handlers.
 */

#include "gnalk-window-private.h"
#include "gnalk-application.h"
#include "gnalk-session.h"
#include "gnalk-account-manager.h"
#include "gnalk-database.h"
#include "gnalk-data-repository.h"
#include "gnalk-sync-coordinator.h"
#include "gnalk-conversation-row.h"
#include "gnalk-conversation-sidebar.h"
#include "gnalk-thread-row.h"
#include "gnalk-thread-data.h"
#include "gnalk-conversation-header.h"
#include "gnalk-participants-list.h"
#include "gnalk-message-view.h"
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>

/* ============ Tree Model Children Callback ============ */

GListModel *
gnalk_window_create_conversation_children (gpointer item,
                                           gpointer user_data G_GNUC_UNUSED)
{
  if (!GNALK_IS_CONVERSATION_DATA (item))
    return NULL;

  GnalkConversationData *data = GNALK_CONVERSATION_DATA (item);

  /* Section headers and invitations have no children */
  if (gnalk_conversation_data_is_section_header (data))
    return NULL;
  if (gnalk_conversation_data_is_invitation (data))
    return NULL;

  /* Always return the threads store so GtkTreeListModel observes items_changed.
   * When threads arrive via sync, the tree model picks them up automatically.
   * The expand button visibility is controlled in bind based on item count. */
  GListStore *threads = gnalk_conversation_data_get_threads_store (data);
  return G_LIST_MODEL (g_object_ref (threads));
}

/* ============ Sorting Helper ============ */

/* Return section priority: invitations (0) > favorites (1) > regular (2) */
static gint
get_section_priority (GnalkConversationData *data)
{
  if (gnalk_conversation_data_is_invitation (data))
    return 0;
  if (gnalk_conversation_data_get_is_favorite (data))
    return 1;
  return 2;
}

gint
gnalk_window_compare_conversations_by_activity (gconstpointer a,
                                                  gconstpointer b,
                                                  gpointer      user_data G_GNUC_UNUSED)
{
  /* Guard against NULL values in the list */
  if (!a && !b) return 0;
  if (!a) return 1;   /* NULL goes to end */
  if (!b) return -1;

  if (!GNALK_IS_CONVERSATION_DATA ((gpointer) a) ||
      !GNALK_IS_CONVERSATION_DATA ((gpointer) b))
    {
      g_warning ("compare_conversations_by_activity: invalid conversation data");
      return 0;
    }

  GnalkConversationData *data_a = GNALK_CONVERSATION_DATA ((gpointer) a);
  GnalkConversationData *data_b = GNALK_CONVERSATION_DATA ((gpointer) b);

  gboolean header_a = gnalk_conversation_data_is_section_header (data_a);
  gboolean header_b = gnalk_conversation_data_is_section_header (data_b);

  /* Section priority: invitations > favorites > regular */
  gint prio_a = get_section_priority (data_a);
  gint prio_b = get_section_priority (data_b);

  /* Different sections */
  if (prio_a != prio_b)
    return prio_a - prio_b;

  /* Same section - headers come first */
  if (header_a && !header_b)
    return -1;
  if (!header_a && header_b)
    return 1;

  /* Both regular items in same section - sort by last activity descending */
  gint64 activity_a = gnalk_conversation_data_get_last_activity (data_a);
  gint64 activity_b = gnalk_conversation_data_get_last_activity (data_b);

  if (activity_b > activity_a)
    return 1;
  else if (activity_b < activity_a)
    return -1;
  return 0;
}

/* ============ Async Conversation Loading ============ */

/* Async conversation loading data */
typedef struct {
  GnalkDataRepository *repo;
  gint64               account_id;
} LoadConversationsData;

static void
load_conversations_data_free (LoadConversationsData *data)
{
  g_clear_object (&data->repo);
  g_slice_free (LoadConversationsData, data);
}

static void
load_conversations_thread (GTask        *task,
                           gpointer      source_object G_GNUC_UNUSED,
                           gpointer      task_data,
                           GCancellable *cancellable G_GNUC_UNUSED)
{
  LoadConversationsData *data = task_data;

  /* Run DB query in background thread */
  GListStore *cached = gnalk_data_repository_load_conversations (data->repo, data->account_id);
  g_task_return_pointer (task, cached, g_object_unref);
}

static void
load_conversations_from_db_finish (GObject      *source_object,
                                   GAsyncResult *result,
                                   gpointer      user_data G_GNUC_UNUSED)
{
  GnalkWindow *self = GNALK_WINDOW (source_object);
  gboolean is_initial_load = !self->initial_conversations_loaded;

  self->conv_load_in_progress = FALSE;

  GListStore *cached = g_task_propagate_pointer (G_TASK (result), NULL);
  if (!cached)
    {
      /* Check for pending reload even on failure */
      if (self->conv_load_pending)
        {
          self->conv_load_pending = FALSE;
          gnalk_window_load_conversations_from_db (self);
        }
      return;
    }

  guint n_cached = g_list_model_get_n_items (G_LIST_MODEL (cached));
  if (n_cached == 0)
    {
      g_object_unref (cached);
      self->initial_conversations_loaded = TRUE;

      /* Empty DB + session online: force a sync so conversations appear */
      if (self->active_session)
        {
          GnalkSessionState state = gnalk_session_get_state (self->active_session);
          if (state == GNALK_SESSION_STATE_ONLINE)
            {
              GnalkSyncCoordinator *coord = gnalk_session_get_sync_coordinator (self->active_session);
              if (coord)
                {
                  g_message ("Empty DB with online session, forcing sync");
                  gnalk_sync_coordinator_trigger (coord, GNALK_SYNC_TRIGGER_RECONNECT);
                }
            }
        }

      /* Check for pending reload */
      if (self->conv_load_pending)
        {
          self->conv_load_pending = FALSE;
          gnalk_window_load_conversations_from_db (self);
        }
      return;
    }

  /* Track if any changes occurred */
  gboolean has_changes = FALSE;

  /* Build hash table of new conversations by token */
  g_autoptr(GHashTable) new_convs = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);
  gboolean has_favorites = FALSE;
  gboolean has_non_favorites = FALSE;
  gboolean has_invitations = FALSE;

  for (guint i = 0; i < n_cached; i++)
    {
      GnalkConversationData *data = g_list_model_get_item (G_LIST_MODEL (cached), i);
      if (data)
        {
          gnalk_conversation_data_set_session (data, self->active_session);
          const char *token = gnalk_conversation_data_get_token (data);
          if (token && *token)
            {
              g_hash_table_insert (new_convs, (gpointer) token, data);  /* Takes ownership of ref */
              if (gnalk_conversation_data_is_invitation (data))
                has_invitations = TRUE;
              else if (gnalk_conversation_data_get_is_favorite (data))
                has_favorites = TRUE;
              else
                has_non_favorites = TRUE;
            }
          else
            g_object_unref (data);
        }
    }

  /* Track which tokens we've seen in existing store */
  g_autoptr(GHashTable) seen_tokens = g_hash_table_new (g_str_hash, g_str_equal);

  /* Check if we need section headers.
   * Count how many distinct section types have entries. */
  gint section_count = (has_invitations ? 1 : 0) + (has_favorites ? 1 : 0) + (has_non_favorites ? 1 : 0);
  gboolean need_headers = (section_count > 1);

  /* First pass: check existing headers and update existing items */
  guint n_store = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
  GArray *indices_to_remove = g_array_new (FALSE, FALSE, sizeof (guint));
  gboolean has_fav_header = FALSE;
  gboolean has_conv_header = FALSE;
  gboolean has_inv_header = FALSE;

  for (guint i = 0; i < n_store; i++)
    {
      GnalkConversationData *existing = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
      if (!existing)
        continue;

      if (gnalk_conversation_data_is_section_header (existing))
        {
          /* Track existing headers; remove if no longer needed */
          if (gnalk_conversation_data_is_invitation (existing))
            {
              if (need_headers && has_invitations)
                has_inv_header = TRUE;
              else
                g_array_append_val (indices_to_remove, i);
            }
          else if (gnalk_conversation_data_get_is_favorite (existing))
            {
              if (need_headers && has_favorites)
                has_fav_header = TRUE;
              else
                g_array_append_val (indices_to_remove, i);
            }
          else
            {
              if (need_headers)
                has_conv_header = TRUE;
              else
                g_array_append_val (indices_to_remove, i);
            }
        }
      else
        {
          const char *token = gnalk_conversation_data_get_token (existing);
          if (token && *token)
            {
              GnalkConversationData *new_data = g_hash_table_lookup (new_convs, token);
              if (new_data)
                {
                  /* Update existing item in-place - track if it actually changed */
                  if (gnalk_conversation_data_update_from_data (existing, new_data))
                    has_changes = TRUE;
                  g_hash_table_add (seen_tokens, (gpointer) token);
                }
              else
                {
                  /* Item no longer exists - mark for removal */
                  g_array_append_val (indices_to_remove, i);
                }
            }
        }
      g_object_unref (existing);
    }

  /* Remove items in reverse order (to preserve indices) */
  if (indices_to_remove->len > 0)
    has_changes = TRUE;
  for (gint i = (gint) indices_to_remove->len - 1; i >= 0; i--)
    {
      guint idx = g_array_index (indices_to_remove, guint, i);
      g_list_store_remove (self->conversations_store, idx);
    }
  g_array_unref (indices_to_remove);

  /* Add new items that weren't in the existing store */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, new_convs);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *token = key;
      if (!g_hash_table_contains (seen_tokens, token))
        {
          GnalkConversationData *new_data = value;
          g_list_store_append (self->conversations_store, new_data);
          has_changes = TRUE;
        }
    }

  /* Add section headers only if needed and not already present */
  if (need_headers)
    {
      if (has_invitations && !has_inv_header)
        {
          GnalkConversationData *inv_header = gnalk_conversation_data_new_invitation_header ();
          g_list_store_append (self->conversations_store, inv_header);
          g_object_unref (inv_header);
          has_changes = TRUE;
        }
      if (has_favorites && !has_fav_header)
        {
          GnalkConversationData *fav_header = gnalk_conversation_data_new_section_header ("Favorites", TRUE);
          g_list_store_append (self->conversations_store, fav_header);
          g_object_unref (fav_header);
          has_changes = TRUE;
        }
      if (!has_conv_header)
        {
          GnalkConversationData *conv_header = gnalk_conversation_data_new_section_header ("Conversations", FALSE);
          g_list_store_append (self->conversations_store, conv_header);
          g_object_unref (conv_header);
          has_changes = TRUE;
        }
    }

  g_object_unref (cached);

  /* Notify the sort model that items may have changed order.
   * GtkSortListModel does incremental re-sorting (no full invalidation). */
  if (has_changes || is_initial_load)
    gtk_sorter_changed (GTK_SORTER (self->conversations_sorter), GTK_SORTER_CHANGE_DIFFERENT);

  g_message ("Loaded %u conversations from database (changes: %s)", n_cached, has_changes ? "yes" : "no");

  /* Push user status from 1:1 conversations to the participant list.
   * The conversations API (20s) has fresher status than the participants API (60s),
   * so we overlay the 1:1 status onto matching participants in any visible group room. */
  if (has_changes && self->participants_list && self->current_room)
    {
      guint n_convs_for_status = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
      for (guint i = 0; i < n_convs_for_status; i++)
        {
          GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
          if (conv && !gnalk_conversation_data_is_section_header (conv) &&
              gnalk_conversation_data_get_room_type (conv) == 1)
            {
              const char *status = gnalk_conversation_data_get_user_status (conv);
              const char *other_user = gnalk_conversation_data_get_name (conv);
              if (other_user && *other_user && status && *status)
                gnalk_participants_list_update_participant_status (self->participants_list, other_user, status);
            }
          if (conv)
            g_object_unref (conv);
        }
    }

  /* Scroll to top only on initial load - preserves scroll position on refresh */
  if (is_initial_load)
    {
      gtk_list_view_scroll_to (gnalk_conversation_sidebar_get_conversation_list (self->conversation_sidebar), 0,
                               GTK_LIST_SCROLL_FOCUS, NULL);
      self->initial_conversations_loaded = TRUE;

      /* Call into window.c to handle post-load room restoration */
      gnalk_window_maybe_restore_last_room (self);
    }

  /* If a reload was requested while this load was running, start another */
  if (self->conv_load_pending)
    {
      self->conv_load_pending = FALSE;
      gnalk_window_load_conversations_from_db (self);
    }
}

/* Load conversations from database asynchronously */
void
gnalk_window_load_conversations_from_db (GnalkWindow *self)
{
  const char *user_id = NULL;
  if (self->active_session)
    {
      GnalkAccount *account = gnalk_session_get_account (self->active_session);
      if (account)
        user_id = gnalk_account_get_user_id (account);
    }
  g_message ("[%s] load_conversations_from_db called", user_id ? user_id : "?");

  /* Concurrency protection: if a load is already in progress, queue a pending reload */
  if (self->conv_load_in_progress)
    {
      self->conv_load_pending = TRUE;
      return;
    }

  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  if (!app)
    return;

  GnalkDataRepository *repo = gnalk_application_get_data_repository (app);
  gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;

  /* If no active session (e.g., starting offline), get account_id from autologin accounts */
  if (account_id <= 0)
    {
      GnalkAccountManager *account_manager = gnalk_application_get_account_manager (app);
      GList *autologin_accounts = gnalk_account_manager_get_autologin_accounts (account_manager);
      if (autologin_accounts)
        {
          GnalkAccount *account = GNALK_ACCOUNT (autologin_accounts->data);
          account_id = gnalk_account_get_id (account);
          g_list_free_full (autologin_accounts, g_object_unref);
        }
    }

  if (!repo || account_id <= 0)
    return;

  self->conv_load_in_progress = TRUE;

  /* Prepare async task data */
  LoadConversationsData *data = g_slice_new (LoadConversationsData);
  data->repo = g_object_ref (repo);
  data->account_id = account_id;

  GTask *task = g_task_new (self, NULL, load_conversations_from_db_finish, NULL);
  g_task_set_task_data (task, data, (GDestroyNotify) load_conversations_data_free);
  g_task_run_in_thread (task, load_conversations_thread);
  g_object_unref (task);
}

/* ============ Expand Toggle Handler ============ */

void
gnalk_window_on_conv_expand_toggled (GnalkConversationRow *row,
                                     GnalkWindow          *self)
{
  (void) self;
  GtkTreeListRow *tree_row = g_object_get_data (G_OBJECT (row), "tree-row");
  if (!tree_row)
    return;

  gboolean expanded = gtk_tree_list_row_get_expanded (tree_row);
  gtk_tree_list_row_set_expanded (tree_row, !expanded);
  gnalk_conversation_row_set_expanded (row, !expanded);
}

/* ============ Search Results ============ */

/* Clear search results store - safe with data model pattern */
void
gnalk_window_clear_search_results (GnalkWindow *self)
{
  if (!self->search_results_store)
    return;

  /* With data model pattern, we can simply clear the store.
   * The factory will call unbind on each item which resets the widget. */
  g_list_store_remove_all (self->search_results_store);
}

/* Helper to find existing conversation token for a user */
static const char *
find_existing_conversation_for_user (GnalkWindow *self, const char *user_id)
{
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
  for (guint i = 0; i < n_items; i++)
    {
      GnalkConversationData *data = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
      /* For 1:1 conversations (type 1), the name field contains the user ID */
      if (gnalk_conversation_data_get_room_type (data) == 1)
        {
          const char *name = gnalk_conversation_data_get_name (data);
          if (name && g_strcmp0 (name, user_id) == 0)
            {
              const char *token = gnalk_conversation_data_get_token (data);
              g_object_unref (data);
              return token;
            }
        }
      g_object_unref (data);
    }
  return NULL;
}

void
gnalk_window_on_sidebar_search_changed (GnalkConversationSidebar *sidebar,
                                         const char                *text,
                                         GnalkWindow              *self)
{
  (void) sidebar;

  /* Clear previous search results */
  gnalk_window_clear_search_results (self);
  gnalk_conversation_sidebar_show_search_results (self->conversation_sidebar, FALSE);

  /* Only search if there's actually text (debouncing already handled by sidebar) */
  if (text && *text && strlen (text) >= 2 && self->conversation_controller)
    {
      gnalk_conversation_controller_search_contacts (self->conversation_controller,
                                                     text,
                                                     gnalk_window_on_search_contacts_response,
                                                     self);
    }
}

void
gnalk_window_on_search_contacts_response (GnalkConversationController *controller G_GNUC_UNUSED,
                                           JsonNode                     *response,
                                           GError                       *error,
                                           gpointer                      user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (error)
    {
      g_warning ("Search failed: %s", error->message);
      return;
    }

  if (!response)
    {
      g_warning ("Search: no response");
      return;
    }

  if (!JSON_NODE_HOLDS_OBJECT (response))
    {
      g_warning ("Search: response is not an object");
      return;
    }

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    {
      g_warning ("Search: no ocs in response");
      return;
    }

  JsonArray *data = json_object_get_array_member (ocs, "data");
  if (!data)
    {
      g_warning ("Search: no data array in response");
      return;
    }

  guint len = json_array_get_length (data);
  g_print ("Search: found %u results\n", len);

  /* First pass: collect all "users" labels to filter duplicate "rooms" entries */
  g_autoptr(GHashTable) user_labels = g_hash_table_new (g_str_hash, g_str_equal);
  for (guint i = 0; i < len; i++)
    {
      JsonObject *item = json_array_get_object_element (data, i);
      if (!item)
        continue;
      const char *source = json_object_get_string_member_with_default (item, "source", "");
      const char *label = json_object_get_string_member_with_default (item, "label", NULL);
      if (g_strcmp0 (source, "users") == 0 && label)
        g_hash_table_add (user_labels, (gpointer) label);
    }

  /* Second pass: add results, filtering duplicate rooms */
  for (guint i = 0; i < len; i++)
    {
      JsonObject *item = json_array_get_object_element (data, i);
      if (!item)
        continue;

      const char *item_id = json_object_get_string_member_with_default (item, "id", NULL);
      const char *label = json_object_get_string_member_with_default (item, "label", NULL);
      const char *source = json_object_get_string_member_with_default (item, "source", "users");

      if (!item_id || !label)
        continue;

      /* Skip "rooms" entries if there's a "users" entry with the same label.
       * This filters out existing 1:1 conversations that duplicate user entries,
       * but keeps group conversations that only appear in "rooms". */
      if (g_strcmp0 (source, "rooms") == 0 && g_hash_table_contains (user_labels, label))
        continue;

      g_autofree char *token_str = NULL;
      const char *subtitle;
      gint room_type;

      /* Handle based on source type */
      if (g_strcmp0 (source, "calls") == 0 || g_strcmp0 (source, "rooms") == 0)
        {
          /* This is a Talk conversation - id is the room token */
          token_str = g_strdup_printf ("room:%s", item_id);
          subtitle = _("Open conversation");
          room_type = 2;  /* Group conversation */
        }
      else
        {
          /* This is a user - check if we already have a conversation */
          const char *existing_token = find_existing_conversation_for_user (self, item_id);
          if (existing_token)
            {
              token_str = g_strdup (existing_token);
              subtitle = _("Open conversation");
            }
          else
            {
              token_str = g_strdup_printf ("user:%s:%s", source, item_id);
              subtitle = _("Start new conversation");
            }
          room_type = 1;  /* One-to-one */
        }

      /* Create a minimal JsonObject for the data */
      g_autoptr(JsonBuilder) builder = json_builder_new ();
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "token");
      json_builder_add_string_value (builder, token_str);
      json_builder_set_member_name (builder, "displayName");
      json_builder_add_string_value (builder, label);
      json_builder_set_member_name (builder, "type");
      json_builder_add_int_value (builder, room_type);

      /* For users, add actorId and status for avatar/status loading */
      if (room_type == 1)
        {
          json_builder_set_member_name (builder, "actorId");
          json_builder_add_string_value (builder, item_id);

          /* Get user status if available - it's an object like {"status":"online",...} */
          if (json_object_has_member (item, "status"))
            {
              JsonNode *status_node = json_object_get_member (item, "status");
              if (status_node && JSON_NODE_HOLDS_OBJECT (status_node))
                {
                  JsonObject *status_obj = json_node_get_object (status_node);
                  const char *user_status = json_object_get_string_member_with_default (status_obj, "status", "");
                  if (user_status && *user_status)
                    {
                      json_builder_set_member_name (builder, "status");
                      json_builder_add_string_value (builder, user_status);
                    }
                }
            }
        }

      /* Add lastMessage with subtitle */
      json_builder_set_member_name (builder, "lastMessage");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "message");
      json_builder_add_string_value (builder, subtitle);
      json_builder_end_object (builder);
      json_builder_end_object (builder);

      g_autoptr(JsonNode) node = json_builder_get_root (builder);
      JsonObject *room_data = json_node_get_object (node);

      /* Create data object and add to store */
      GnalkConversationData *conv_data = gnalk_conversation_data_new_from_json (room_data, self->active_session);
      g_list_store_append (self->search_results_store, conv_data);
      g_print ("Search: added %s '%s' (id: %s, %s)\n",
               g_strcmp0 (source, "calls") == 0 ? "conversation" : "user",
               label, item_id, subtitle);
      g_object_unref (conv_data);
    }

  /* Show the search results revealer if we have results */
  if (g_list_model_get_n_items (G_LIST_MODEL (self->search_results_store)) > 0)
    gnalk_conversation_sidebar_show_search_results (self->conversation_sidebar, TRUE);
}

void
gnalk_window_on_sidebar_search_result_selected (GnalkConversationSidebar *sidebar,
                                                 guint                      position,
                                                 GnalkWindow              *self)
{
  (void) sidebar;
  GnalkConversationData *data = g_list_model_get_item (G_LIST_MODEL (self->search_results_store), position);

  if (!data)
    return;

  const char *token = gnalk_conversation_data_get_token (data);

  /* Check if this is a new user search result (token starts with "user:") */
  if (token && g_str_has_prefix (token, "user:"))
    {
      /* Parse user:source:userId */
      g_auto(GStrv) parts = g_strsplit (token, ":", 3);
      if (parts && parts[0] && parts[1] && parts[2])
        {
          const char *user_id = parts[2];

          /* Create a 1:1 conversation with this user via controller */
          if (self->conversation_controller)
            gnalk_conversation_controller_create (self->conversation_controller,
                                                   1,  /* roomType 1 = one-to-one */
                                                   user_id,
                                                   NULL);  /* no room name for 1:1 */

          /* Clear search */
          gnalk_conversation_sidebar_clear_search (self->conversation_sidebar);
          gnalk_window_clear_search_results (self);
        }
    }
  /* Check if this is a group room result (token starts with "room:") */
  else if (token && g_str_has_prefix (token, "room:"))
    {
      /* Extract real token after "room:" prefix */
      const char *real_token = token + 5;

      /* Clear search and hide results */
      gnalk_conversation_sidebar_clear_search (self->conversation_sidebar);
      gnalk_window_clear_search_results (self);

      /* Open the conversation */
      gnalk_window_open_conversation_by_token (self, real_token);
    }
  /* Regular token - existing conversation */
  else if (token)
    {
      /* Clear search and hide results */
      gnalk_conversation_sidebar_clear_search (self->conversation_sidebar);
      gnalk_window_clear_search_results (self);

      /* Open the existing conversation */
      gnalk_window_open_conversation_by_token (self, token);
    }

  g_object_unref (data);
}

/* ============ List Factory Callbacks ============ */

/* Helper to update conversation row active state when room changes */
void
gnalk_window_update_conversation_active_states (GnalkWindow *self,
                                                 const char   *old_token,
                                                 const char   *new_token)
{
  /* Deactivate old row if it exists */
  if (old_token)
    {
      GnalkConversationRow *old_row = g_hash_table_lookup (self->bound_conversation_rows, old_token);
      if (old_row)
        gnalk_conversation_row_set_active (old_row, FALSE);
    }

  /* Activate new row if it exists */
  if (new_token)
    {
      GnalkConversationRow *new_row = g_hash_table_lookup (self->bound_conversation_rows, new_token);
      if (new_row)
        gnalk_conversation_row_set_active (new_row, TRUE);
    }
}

void
gnalk_window_on_conv_list_item_setup (GtkListItemFactory *factory G_GNUC_UNUSED,
                                       GtkListItem        *item,
                                       gpointer            user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  /* Create a GtkStack with all widget types - reused across bind/unbind cycles */
  GtkStack *stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_hhomogeneous (stack, TRUE);
  gtk_stack_set_vhomogeneous (stack, FALSE);

  /* Section header label */
  GtkWidget *label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_add_css_class (label, "conversation-section-header");
  gtk_stack_add_named (stack, label, "header");

  /* Conversation row with signals connected once */
  GnalkConversationRow *row = gnalk_conversation_row_new ();
  g_signal_connect (row, "leave-requested", G_CALLBACK (gnalk_window_on_conv_leave_requested), self);
  g_signal_connect (row, "delete-requested", G_CALLBACK (gnalk_window_on_conv_delete_requested), self);
  g_signal_connect (row, "favorite-toggled", G_CALLBACK (gnalk_window_on_conv_favorite_toggled), self);
  g_signal_connect (row, "expand-toggled", G_CALLBACK (gnalk_window_on_conv_expand_toggled), self);
  gtk_stack_add_named (stack, GTK_WIDGET (row), "row");

  /* Thread row */
  GnalkThreadRow *thread_row = gnalk_thread_row_new ();
  gtk_stack_add_named (stack, GTK_WIDGET (thread_row), "thread");

  gtk_list_item_set_child (item, GTK_WIDGET (stack));
}

void
gnalk_window_on_conv_list_item_bind (GtkListItemFactory *factory G_GNUC_UNUSED,
                                      GtkListItem        *item,
                                      gpointer            user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  GtkTreeListRow *tree_row = GTK_TREE_LIST_ROW (gtk_list_item_get_item (item));
  GtkStack *stack = GTK_STACK (gtk_list_item_get_child (item));
  gpointer real_item = gtk_tree_list_row_get_item (tree_row);
  guint depth = gtk_tree_list_row_get_depth (tree_row);

  if (depth == 1 && GNALK_IS_THREAD_DATA (real_item))
    {
      /* Thread row (child of a conversation) */
      GnalkThreadRow *thread_row = GNALK_THREAD_ROW (
          gtk_stack_get_child_by_name (stack, "thread"));
      gnalk_thread_row_set_from_data (thread_row, GNALK_THREAD_DATA (real_item));
      gtk_stack_set_visible_child_name (stack, "thread");
      gtk_list_item_set_activatable (item, TRUE);
      gtk_list_item_set_selectable (item, TRUE);
    }
  else if (GNALK_IS_CONVERSATION_DATA (real_item))
    {
      GnalkConversationData *data = GNALK_CONVERSATION_DATA (real_item);

      if (gnalk_conversation_data_is_section_header (data))
        {
          GtkWidget *label = gtk_stack_get_child_by_name (stack, "header");
          gtk_label_set_text (GTK_LABEL (label), gnalk_conversation_data_get_title (data));
          gtk_stack_set_visible_child_name (stack, "header");
          gtk_list_item_set_activatable (item, FALSE);
          gtk_list_item_set_selectable (item, FALSE);
        }
      else
        {
          GnalkConversationRow *row = GNALK_CONVERSATION_ROW (
              gtk_stack_get_child_by_name (stack, "row"));
          gnalk_conversation_row_set_from_data (row, data, self->avatar_cache);

          /* Set active state based on current room */
          const char *token = gnalk_conversation_data_get_token (data);
          gboolean is_active = self->current_room && g_strcmp0 (token, self->current_room->room_token) == 0;
          gnalk_conversation_row_set_active (row, is_active);

          /* Set expandable based on actual thread count (not tree model's is_expandable,
           * which is TRUE for any non-NULL child model including empty stores) */
          GListStore *threads = gnalk_conversation_data_get_threads_store (data);
          gboolean expandable = g_list_model_get_n_items (G_LIST_MODEL (threads)) > 0;
          gnalk_conversation_row_set_expandable (row, expandable);
          gnalk_conversation_row_set_expanded (row, gtk_tree_list_row_get_expanded (tree_row));
          gnalk_conversation_row_set_has_unread_threads (row,
              gnalk_conversation_data_get_has_unread_threads (data));

          /* Store tree row reference on the conversation row for expand toggle */
          g_object_set_data_full (G_OBJECT (row), "tree-row",
                                  g_object_ref (tree_row), g_object_unref);

          /* Track this row for direct active state updates */
          if (token)
            g_hash_table_insert (self->bound_conversation_rows, g_strdup (token), row);

          gtk_stack_set_visible_child_name (stack, "row");
          gtk_list_item_set_activatable (item, TRUE);
          gtk_list_item_set_selectable (item, TRUE);
        }
    }

  g_object_unref (real_item);
}

void
gnalk_window_on_conv_list_item_unbind (GtkListItemFactory *factory G_GNUC_UNUSED,
                                        GtkListItem        *item,
                                        gpointer            user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  GtkStack *stack = GTK_STACK (gtk_list_item_get_child (item));

  /* Determine which child is currently visible and reset it */
  const char *visible_name = gtk_stack_get_visible_child_name (stack);

  if (g_strcmp0 (visible_name, "row") == 0)
    {
      GtkWidget *row_widget = gtk_stack_get_child_by_name (stack, "row");
      if (GNALK_IS_CONVERSATION_ROW (row_widget))
        {
          GnalkConversationRow *row = GNALK_CONVERSATION_ROW (row_widget);
          const char *token = gnalk_conversation_row_get_token (row);

          /* Remove from tracking hash table */
          if (token)
            g_hash_table_remove (self->bound_conversation_rows, token);

          /* Clear tree row reference */
          g_object_set_data (G_OBJECT (row), "tree-row", NULL);

          gnalk_conversation_row_reset (row);
        }
    }
  else if (g_strcmp0 (visible_name, "thread") == 0)
    {
      GtkWidget *thread_widget = gtk_stack_get_child_by_name (stack, "thread");
      if (GNALK_IS_THREAD_ROW (thread_widget))
        gnalk_thread_row_reset (GNALK_THREAD_ROW (thread_widget));
    }
  /* Do NOT call gtk_list_item_set_child(NULL) — the stack persists for reuse */
}

/* Search results list factory callbacks - proper data/widget separation */
void
gnalk_window_on_search_list_item_setup (GtkListItemFactory *factory,
                                         GtkListItem        *item,
                                         gpointer            user_data)
{
  /* Create a new conversation row widget */
  GnalkConversationRow *row = gnalk_conversation_row_new ();
  gtk_list_item_set_child (item, GTK_WIDGET (row));
}

void
gnalk_window_on_search_list_item_bind (GtkListItemFactory *factory,
                                        GtkListItem        *item,
                                        gpointer            user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  GnalkConversationData *data = GNALK_CONVERSATION_DATA (gtk_list_item_get_item (item));
  GnalkConversationRow *row = GNALK_CONVERSATION_ROW (gtk_list_item_get_child (item));

  /* Populate the row from the data object */
  gnalk_conversation_row_set_from_data (row, data, self->avatar_cache);
}

void
gnalk_window_on_search_list_item_unbind (GtkListItemFactory *factory,
                                          GtkListItem        *item,
                                          gpointer            user_data)
{
  GnalkConversationRow *row = GNALK_CONVERSATION_ROW (gtk_list_item_get_child (item));
  if (row)
    gnalk_conversation_row_reset (row);
}

/* ============ Thread Sync Handler ============ */

void
gnalk_window_on_threads_synced (GnalkSession *session G_GNUC_UNUSED,
                                JsonArray    *thread_infos,
                                GnalkWindow  *self)
{
  if (!thread_infos)
    return;

  guint n_threads = json_array_get_length (thread_infos);
  g_message ("Window: threads-synced received %u thread infos", n_threads);

  /* Group threads by roomToken */
  g_autoptr(GHashTable) by_room = g_hash_table_new_full (
      g_str_hash, g_str_equal, NULL, (GDestroyNotify) g_ptr_array_unref);

  for (guint i = 0; i < n_threads; i++)
    {
      JsonObject *info = json_array_get_object_element (thread_infos, i);
      if (!info)
        continue;

      GnalkThreadData *td = gnalk_thread_data_new_from_json (info);
      if (!td)
        continue;

      const char *room_token = gnalk_thread_data_get_room_token (td);
      if (!room_token || !*room_token)
        {
          g_object_unref (td);
          continue;
        }

      GPtrArray *arr = g_hash_table_lookup (by_room, room_token);
      if (!arr)
        {
          arr = g_ptr_array_new_with_free_func (g_object_unref);
          g_hash_table_insert (by_room, (gpointer) room_token, arr);
        }
      g_ptr_array_add (arr, td);  /* takes ownership */
    }

  /* For each conversation in our store, set its threads */
  guint n_convs = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
  for (guint i = 0; i < n_convs; i++)
    {
      GnalkConversationData *data = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
      if (!data || gnalk_conversation_data_is_section_header (data))
        {
          g_clear_object (&data);
          continue;
        }

      const char *token = gnalk_conversation_data_get_token (data);
      GPtrArray *threads = token ? g_hash_table_lookup (by_room, token) : NULL;

      /* set_threads handles NULL/empty to clear threads */
      gnalk_conversation_data_set_threads (data, threads);
      g_object_unref (data);
    }
}

/* ============ Controller Signal Handlers ============ */

/* Called when a new conversation is created */
void
gnalk_window_on_conversation_controller_created (GnalkConversationController *controller G_GNUC_UNUSED,
                                                  const char                   *room_token,
                                                  GnalkWindow                 *self)
{
  g_debug ("Window: Conversation created: %s", room_token);
  /* DataRepository::data-changed handles the reload (controller saves via repository) */
  gnalk_window_open_conversation_by_token (self, room_token);
}

/* Called when user leaves a conversation */
void
gnalk_window_on_conversation_controller_left (GnalkConversationController *controller G_GNUC_UNUSED,
                                               const char                   *room_token,
                                               GnalkWindow                 *self)
{
  g_debug ("Window: Left conversation: %s", room_token);

  /* If this was the current room, clear the view */
  if (self->current_room && g_strcmp0 (room_token, self->current_room->room_token) == 0)
    {
      g_autofree char *old_token = g_strdup (self->current_room->room_token);
      self->current_room = NULL;
      self->has_active_room = FALSE;
      g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_HAS_ACTIVE_ROOM]);
      gnalk_message_view_show_empty (self->message_view);
      gnalk_conversation_header_set_visible (self->conversation_header, FALSE);

      /* Update conversation row highlight */
      gnalk_window_update_conversation_active_states (self, old_token, NULL);
    }
  /* DataRepository::data-changed handles the reload (controller deletes via repository) */
}

/* Called when a conversation is deleted */
void
gnalk_window_on_conversation_controller_deleted (GnalkConversationController *controller G_GNUC_UNUSED,
                                                  const char                   *room_token,
                                                  GnalkWindow                 *self)
{
  g_debug ("Window: Conversation deleted: %s", room_token);

  /* If this was the current room, clear the view */
  if (self->current_room && g_strcmp0 (room_token, self->current_room->room_token) == 0)
    {
      g_autofree char *old_token = g_strdup (self->current_room->room_token);
      self->current_room = NULL;
      self->has_active_room = FALSE;
      g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_HAS_ACTIVE_ROOM]);
      gnalk_message_view_show_empty (self->message_view);
      gnalk_conversation_header_set_visible (self->conversation_header, FALSE);

      /* Update conversation row highlight */
      gnalk_window_update_conversation_active_states (self, old_token, NULL);
    }
  /* DataRepository::data-changed handles the reload (controller deletes via repository) */
}

/* Debounce timer callback for conversation list reload */
static gboolean
on_conv_reload_debounce_timeout (gpointer user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  self->conv_reload_debounce_id = 0;

  gnalk_window_load_conversations_from_db (self);
  return G_SOURCE_REMOVE;
}

/* Called when DataRepository emits data-changed (e.g., from SyncCoordinator) */
void
gnalk_window_on_data_repository_changed (GnalkDataRepository *repository G_GNUC_UNUSED,
                                          const char          *scope,
                                          GnalkWindow         *self)
{
  g_debug ("Window: DataRepository data-changed (scope=%s)", scope ? scope : "(null)");

  /* Debounced reload when conversations data changes */
  if (g_strcmp0 (scope, "conversations") == 0)
    {
      if (self->conv_reload_debounce_id > 0)
        g_source_remove (self->conv_reload_debounce_id);
      self->conv_reload_debounce_id = g_timeout_add (100, on_conv_reload_debounce_timeout, self);
    }
}
