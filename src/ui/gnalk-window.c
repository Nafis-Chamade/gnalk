#include "gnalk-config.h"
#include "gnalk-window-private.h"
#include "gnalk-application.h"
#include "gnalk-connection-dialog.h"
#include "gnalk-new-conversation-dialog.h"
#include "gnalk-conversation-info-dialog.h"
#include "gnalk-conversation-row.h"
#include "gnalk-conversation-data.h"
#include "gnalk-message-row.h"
#include "gnalk-message-data.h"
#include "gnalk-network-monitor.h"
#include "gnalk-keyring.h"
#include "gnalk-preferences-dialog.h"
#include "gnalk-account-manager.h"
#include "gnalk-database.h"
#include "gnalk-app-state.h"
#include "gnalk-room-controller.h"
#include "gnalk-background-sync.h"
#include "gnalk-video-player-dialog.h"
#include <glib/gi18n.h>

/* AttachmentInfo and UploadQueueItem helper functions */
void
attachment_info_free (AttachmentInfo *info)
{
  if (info)
    {
      g_free (info->file_path);
      g_clear_pointer (&info->file_bytes, g_bytes_unref);
      g_free (info->content_type);
      g_free (info->file_name);
      g_free (info);
    }
}

void
upload_queue_item_free (UploadQueueItem *item)
{
  if (item)
    {
      attachment_info_free (item->attachment);
      g_free (item->caption);
      g_free (item->reference_id);
      g_free (item);
    }
}

/* GnalkWindow struct is defined in gnalk-window-private.h */

G_DEFINE_FINAL_TYPE (GnalkWindow, gnalk_window, ADW_TYPE_APPLICATION_WINDOW)

/* Property specs - exported for use in gnalk-window-login.c */
GParamSpec *gnalk_window_properties[GNALK_WINDOW_N_PROPS];

/* Login functions are in gnalk-window-login.c - declarations in gnalk-window-private.h */

static void on_set_status_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);

/* Session management functions are in gnalk-window-account-session.c:
 * - gnalk_window_switch_to_session()
 * - gnalk_window_on_session_manager_active_changed()
 * - gnalk_window_on_session_manager_any_state_changed()
 * - gnalk_window_on_session_manager_conversations_changed()
 * - gnalk_window_disconnect_active_session()
 */

/* AppState signal handlers (new async startup architecture) */
static void on_app_state_loading_changed (GnalkAppState *app_state, gboolean is_loading, GnalkWindow *self);

/* Background sync signal handlers */
static void on_app_background_sync_messages_synced (GnalkBackgroundSync *sync, const char *room_token, gpointer user_data);

/* Session signal handlers - connected by gnalk-window-account-session.c, implemented here.
 * Declarations are in gnalk-window-private.h for use by account-session.c */

/* Signaling signal handlers - connected by gnalk-window-account-session.c, implemented here.
 * Declarations are in gnalk-window-private.h for use by account-session.c */

/* Message controller signal handlers - connected by gnalk-window-account-session.c, implemented here.
 * Declarations are in gnalk-window-private.h for use by account-session.c */
/* Messages loaded, poll response, older messages handlers are in gnalk-window-message-display.c */

/* Conversation controller signal handlers - now in gnalk-window-conversation-list.c */

/* Participant controller signal handlers - connected by gnalk-window-account-session.c, implemented here.
 * Declarations are in gnalk-window-private.h for use by account-session.c */

/* load_messages_from_db is now gnalk_window_load_messages_from_db in gnalk-window-message-display.c */
/* load_conversations_from_db is now declared in gnalk-window-private.h as gnalk_window_load_conversations_from_db */
/* maybe_restore_last_room is now gnalk_window_maybe_restore_last_room (declared in private.h, called from conversation-list.c) */
/* save_messages_to_db is now gnalk_window_save_messages_to_db in gnalk-window-message-display.c */
/* gnalk_window_compare_conversations_by_activity is now gnalk_window_gnalk_window_compare_conversations_by_activity in gnalk-window-conversation-list.c */
/* update_call_button_state is now gnalk_window_update_call_button_state - see below */
/* update_read_status is now gnalk_window_update_read_status in gnalk-window-read-status.c */
static void on_reaction_requested (GnalkMessageRow *row, gint64 message_id, const char *emoji, gpointer user_data);
static void on_file_clicked (GnalkMessageRow *row, const char *file_id, const char *file_mimetype, const char *file_link, const char *file_name, const char *file_path, gpointer user_data);
static void on_quote_requested (GnalkMessageRow *row, gint64 message_id, const char *author_name, const char *message_text, gpointer user_data);
static void on_edit_requested (GnalkMessageRow *row, gint64 message_id, const char *message_text, gpointer user_data);
static void on_delete_requested (GnalkMessageRow *row, gint64 message_id, gpointer user_data);
static void on_scroll_to_message_requested (GnalkMessageRow *row, gint64 message_id, gpointer user_data);
/* load_older_messages is now gnalk_window_load_older_messages in gnalk-window-message-display.c */
/* on_content_page_hiding moved to gnalk-window-room-switch.c */
static void on_call_window_destroy (GtkWidget *widget, GnalkWindow *self);
/* clear_attachments is now gnalk_window_clear_attachments in gnalk-window-message-composer.c */
static void on_new_conversation_clicked (GnalkConversationSidebar *sidebar, GnalkWindow *self);
static void on_new_conversation_created (GnalkNewConversationDialog *dialog, const char *room_token, GnalkWindow *self);
/* compare_messages_by_id_for_store is now gnalk_window_compare_messages_by_id in gnalk-window-message-display.c */

/* Unread message handling */
/* mark_messages_read_up_to is now gnalk_window_mark_messages_read_up_to in gnalk-window-read-status.c */
/* on_scroll_to_bottom_clicked_for_unread is now gnalk_window_on_scroll_to_bottom_clicked_for_unread in gnalk-window-read-status.c */

/* clear_messages_store is now gnalk_window_clear_messages_store in gnalk-window-message-display.c */
/* clear_search_results_store is now gnalk_window_clear_search_results in gnalk-window-conversation-list.c */
/* generate_reference_id is now gnalk_window_generate_reference_id in gnalk-window-message-composer.c */

/* ============ Read Status Tracking ============
 *
 * Read status functions have been moved to gnalk-window-read-status.c:
 * - gnalk_window_update_read_status()
 * - gnalk_window_mark_messages_read_up_to()
 * - gnalk_window_track_bound_unread_row()
 * - gnalk_window_untrack_bound_unread_row()
 * - gnalk_window_on_message_scroll_changed()
 * - gnalk_window_on_scroll_to_bottom_clicked_for_unread()
 */

static void
gnalk_window_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  GnalkWindow *self = GNALK_WINDOW (object);

  switch (prop_id)
    {
    case GNALK_WINDOW_PROP_USER_NAME:
      g_value_set_string (value, self->user_name);
      break;
    case GNALK_WINDOW_PROP_CURRENT_ROOM_NAME:
      g_value_set_string (value, self->current_room_name);
      break;
    case GNALK_WINDOW_PROP_HAS_ACTIVE_ROOM:
      g_value_set_boolean (value, self->has_active_room);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gnalk_window_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  GnalkWindow *self = GNALK_WINDOW (object);

  switch (prop_id)
    {
    case GNALK_WINDOW_PROP_USER_NAME:
      g_free (self->user_name);
      self->user_name = g_value_dup_string (value);
      break;
    case GNALK_WINDOW_PROP_CURRENT_ROOM_NAME:
      g_free (self->current_room_name);
      self->current_room_name = g_value_dup_string (value);
      break;
    case GNALK_WINDOW_PROP_HAS_ACTIVE_ROOM:
      self->has_active_room = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
on_network_changed (GnalkNetworkMonitor *monitor G_GNUC_UNUSED,
                    gboolean              is_online,
                    gpointer              user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  /* Only show/hide banner if we had a connection before or had an error */
  if (self->initial_auth_done || self->had_connection_error)
    {
      if (!is_online)
        {
          gnalk_network_status_bar_set_offline (self->network_status_bar, TRUE, NULL);
          self->had_connection_error = TRUE;
        }
      else
        {
          gnalk_network_status_bar_hide (self->network_status_bar);
        }

      /* Show "Connected" toast when coming back online after error */
      if (is_online && self->was_offline && self->had_connection_error)
        {
          AdwToast *toast = adw_toast_new ("");
          GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
          gtk_widget_add_css_class (box, "toast-success");
          GtkWidget *icon = gtk_image_new_from_icon_name ("emblem-ok-symbolic");
          GtkWidget *label = gtk_label_new (_("Connected"));
          gtk_box_append (GTK_BOX (box), icon);
          gtk_box_append (GTK_BOX (box), label);
          adw_toast_set_custom_title (toast, box);
          adw_toast_set_timeout (toast, 3);
          adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), toast);
          self->had_connection_error = FALSE;
        }
    }

  self->was_offline = !is_online;
}

/* ============ Session Signal Handlers (New Multi-Account Architecture) ============
 *
 * Session management functions moved to gnalk-window-account-session.c:
 * - connect_session_signals()
 * - disconnect_session_signals()
 * - gnalk_window_switch_to_session()
 * - gnalk_window_on_session_manager_active_changed()
 * - gnalk_window_on_session_manager_any_state_changed()
 * - gnalk_window_on_session_manager_conversations_changed()
 * - gnalk_window_disconnect_active_session()
 */

/* AppState loading changed - show/hide spinner during async startup */
static void
on_app_state_loading_changed (GnalkAppState *app_state G_GNUC_UNUSED,
                               gboolean        is_loading,
                               GnalkWindow   *self)
{
  g_debug ("Window: AppState loading changed: %s", is_loading ? "loading" : "done");

  /* Show/hide loading indicator */
  if (self->connecting_spinner)
    gtk_widget_set_visible (self->connecting_spinner, is_loading);

  /* When async loading completes, connect to background sync for message notifications.
   * We do this here because background_sync is created during async initialization,
   * which happens AFTER the window is constructed. The window's initial connection
   * attempt in constructed() will fail because background_sync doesn't exist yet. */
  if (!is_loading && !self->bg_sync_connected)
    {
      GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
      GnalkBackgroundSync *bg_sync = gnalk_application_get_background_sync (GNALK_APPLICATION (app));
      if (bg_sync)
        {
          g_signal_connect_object (bg_sync, "messages-synced",
                                   G_CALLBACK (on_app_background_sync_messages_synced), self,
                                   G_CONNECT_DEFAULT);
          self->bg_sync_connected = TRUE;
          g_debug ("Window: Connected to background sync messages-synced signal");
        }
    }
}

/* ============ Session Signal Handlers ============
 *
 * These handlers are connected by gnalk-window-account-session.c when
 * switching sessions. They handle various session events (state changes,
 * room joins, typing, etc.) and update the window UI accordingly.
 */

void
gnalk_window_on_session_state_changed (GnalkSession *session,
                          int            old_state,
                          int            new_state,
                          GnalkWindow  *self)
{
  g_message ("Window: session state changed %d -> %d", old_state, new_state);

  GnalkSessionState state = (GnalkSessionState) new_state;
  GnalkSessionState prev_state = (GnalkSessionState) old_state;
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));

  switch (state)
    {
    case GNALK_SESSION_STATE_ONLINE:
      {
        /* Hide login connecting state if we were connecting from login */
        gnalk_window_login_set_connecting (self, FALSE);

        /* Hide spinner and offline banner */
        gtk_spinner_set_spinning (GTK_SPINNER (self->connecting_spinner), FALSE);
        gtk_widget_set_visible (self->connecting_spinner, FALSE);
        gnalk_network_status_bar_hide (self->network_status_bar);

        /* Switch to main view */
        gtk_stack_set_visible_child_name (GTK_STACK (self->main_stack), "main");

        /* Mark avatar cache as online */
        if (self->avatar_cache)
          gnalk_avatar_cache_set_online (self->avatar_cache, TRUE);

        /* Get account info for display name and user status setup */
        GnalkAccount *account = gnalk_session_get_account (session);
        const char *display_name = account ? gnalk_account_get_display_name (account) : NULL;
        const char *user_id = account ? gnalk_account_get_user_id (account) : NULL;

        /* Update display name */
        if (display_name && *display_name)
          {
            g_free (self->user_name);
            self->user_name = g_strdup (display_name);
            g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_USER_NAME]);

            /* Save display name for offline startup */
            GSettings *settings = gnalk_application_get_settings (app);
            g_settings_set_string (settings, "last-display-name", display_name);
          }

        /* Set user avatar */
        if (user_id && self->avatar_cache)
          gnalk_user_status_menu_set_user (self->user_status_menu, user_id, self->avatar_cache);

        /* Connect push client (only first time going online from connecting) */
        if (prev_state == GNALK_SESSION_STATE_CONNECTING &&
            self->push_client && !gnalk_push_client_is_connected (self->push_client) &&
            !gnalk_application_is_guest (app))
          {
            const char *push_url = gnalk_application_get_push_websocket_url (app);
            const char *server_url = account ? gnalk_account_get_server_url (account) : NULL;

            /* Get password - try application first, then fall back to keyring */
            const char *app_password = gnalk_application_get_password (app);
            g_autofree char *keyring_password = NULL;
            const char *password = app_password;

            if (!password && server_url && user_id)
              {
                keyring_password = gnalk_keyring_lookup_password_sync (server_url, user_id, NULL);
                password = keyring_password;
              }

            /* Set app-level credentials for background sync service */
            if (server_url && user_id && password)
              gnalk_application_set_credentials (app, server_url, user_id, password);

            g_message ("PUSH: Checking push client connection - url='%s', user='%s', has_password=%d",
                       push_url ? push_url : "(null)", user_id ? user_id : "(null)", password != NULL);
            if (push_url && *push_url && user_id && password)
              {
                g_message ("PUSH: Connecting push client to %s", push_url);
                /* Note: Push callback is set at app level for background sync + notifications.
                 * Window refreshes via background sync's "messages-synced" signal. */
                gnalk_push_client_connect (self->push_client, push_url, user_id, password);
              }
            else
              {
                g_message ("PUSH: Cannot connect - missing url, user_id, or password");
              }
          }

        /* Update user status menu to show online (heartbeat is handled by session) */
        if (!gnalk_application_is_guest (app) && user_id)
          gnalk_user_status_menu_set_status (self->user_status_menu, "online");

        /* Show connected/reconnected toast if we had an error */
        if (self->had_connection_error || self->was_offline)
          {
            AdwToast *toast = adw_toast_new ("");
            GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_widget_add_css_class (box, "toast-success");
            GtkWidget *icon = gtk_image_new_from_icon_name ("emblem-ok-symbolic");
            GtkWidget *label = gtk_label_new (prev_state == GNALK_SESSION_STATE_CONNECTING
                                               ? _("Connected") : _("Reconnected"));
            gtk_box_append (GTK_BOX (box), icon);
            gtk_box_append (GTK_BOX (box), label);
            adw_toast_set_custom_title (toast, box);
            adw_toast_set_timeout (toast, 3);
            adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), toast);
          }

        self->had_connection_error = FALSE;
        self->was_offline = FALSE;
        self->initial_auth_done = TRUE;
        self->autologin_pending = FALSE;

        /* Start conversation controller polling - it will sync new messages to DB
         * Skip for guests - they can only access their single room */
        if (self->conversation_controller && !gnalk_session_is_guest (session))
          gnalk_conversation_controller_start_polling (self->conversation_controller);

        /* Reload messages from DB for current room (controller will sync new ones) */
        if (self->current_room_token)
          gnalk_window_load_messages_from_db (self, self->current_room_token);

        /* Refresh participants now that we're online (may have been empty from offline start) */
        if (self->current_room_token && self->participant_controller)
          gnalk_participant_controller_refresh (self->participant_controller);
      }
      break;

    case GNALK_SESSION_STATE_CONNECTING:
      gtk_spinner_set_spinning (GTK_SPINNER (self->connecting_spinner), TRUE);
      gtk_widget_set_visible (self->connecting_spinner, TRUE);
      break;

    case GNALK_SESSION_STATE_OFFLINE:
      gnalk_network_status_bar_set_offline (self->network_status_bar, TRUE,
                                              _("Connection lost"));
      self->had_connection_error = TRUE;
      if (self->avatar_cache)
        gnalk_avatar_cache_set_online (self->avatar_cache, FALSE);
      /* Reset participant status in DB (we can't know real status when offline) */
      {
        GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
        GnalkDatabase *db = gnalk_application_get_database (app);
        gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;
        if (db && account_id > 0)
          gnalk_database_reset_participants_status (db, account_id);
      }
      break;

    case GNALK_SESSION_STATE_RECONNECTING:
      gnalk_network_status_bar_set_offline (self->network_status_bar, TRUE,
                                              _("Reconnecting..."));
      self->had_connection_error = TRUE;
      break;

    case GNALK_SESSION_STATE_DISCONNECTED:
      /* Auth failed or logged out - show login view */
      gnalk_window_login_set_connecting (self, FALSE);
      gnalk_window_show_login_view (self);
      break;
    }
}

void
gnalk_window_on_session_conversations_updated (GnalkSession *session,
                                                GnalkWindow  *self)
{
  g_message ("Window: session conversations updated, updating store in-place");

  /* Sync conversations from session to our store */
  GListStore *conversations = gnalk_session_get_conversations (session);
  if (!conversations)
    return;

  /* Build hash table of new conversations by token */
  g_autoptr(GHashTable) new_convs = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);
  gboolean has_favorites = FALSE;
  gboolean has_non_favorites = FALSE;

  guint n = g_list_model_get_n_items (G_LIST_MODEL (conversations));
  for (guint i = 0; i < n; i++)
    {
      GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (conversations), i);
      if (conv)
        {
          const char *token = gnalk_conversation_data_get_token (conv);
          if (token && *token)
            {
              g_hash_table_insert (new_convs, (gpointer) token, conv);  /* Takes ownership of ref */
              if (gnalk_conversation_data_get_is_favorite (conv))
                has_favorites = TRUE;
              else
                has_non_favorites = TRUE;
            }
          else
            g_object_unref (conv);
        }
    }

  /* Track which tokens we've seen in existing store */
  g_autoptr(GHashTable) seen_tokens = g_hash_table_new (g_str_hash, g_str_equal);

  /* Check if we need section headers */
  gboolean need_headers = (has_favorites && has_non_favorites);

  /* First pass: check existing headers and update existing items */
  guint n_store = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
  GArray *indices_to_remove = g_array_new (FALSE, FALSE, sizeof (guint));
  gboolean has_fav_header = FALSE;
  gboolean has_conv_header = FALSE;

  for (guint i = 0; i < n_store; i++)
    {
      GnalkConversationData *existing = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
      if (!existing)
        continue;

      if (gnalk_conversation_data_is_section_header (existing))
        {
          /* Track existing headers; remove if no longer needed */
          if (gnalk_conversation_data_get_is_favorite (existing))
            {
              if (need_headers)
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
                  /* Update existing item in-place */
                  gnalk_conversation_data_update_from_data (existing, new_data);
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
        }
    }

  /* Add section headers only if needed and not already present */
  if (need_headers)
    {
      if (!has_fav_header)
        {
          GnalkConversationData *fav_header = gnalk_conversation_data_new_section_header ("Favorites", TRUE);
          g_list_store_append (self->conversations_store, fav_header);
          g_object_unref (fav_header);
        }
      if (!has_conv_header)
        {
          GnalkConversationData *conv_header = gnalk_conversation_data_new_section_header ("Conversations", FALSE);
          g_list_store_append (self->conversations_store, conv_header);
          g_object_unref (conv_header);
        }
    }

  /* Always sort to ensure conversations with new messages appear at top */
  g_list_store_sort (self->conversations_store, gnalk_window_compare_conversations_by_activity, NULL);
}

void
gnalk_window_on_session_room_joined (GnalkSession *session G_GNUC_UNUSED,
                                      const char    *room_token,
                                      const char    *session_id,
                                      GnalkWindow  *self)
{
  g_message ("Window: session room joined: %s (session_id: %s)", room_token, session_id);

  g_free (self->current_room_token);
  g_free (self->current_session_id);
  self->current_room_token = g_strdup (room_token);
  self->current_session_id = g_strdup (session_id);
  self->has_active_room = TRUE;

  g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_HAS_ACTIVE_ROOM]);

  /* Notify application of active room for notification decisions */
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  if (app)
    gnalk_application_set_active_room (app, room_token);

  /* Set room on message controller */
  gnalk_message_controller_set_room (self->message_controller, room_token);

  /* For guests, start polling here since they skip the normal cache load path */
  if (self->message_controller && gnalk_session_is_guest (session))
    gnalk_message_controller_start_polling (self->message_controller, 0);
}

void
gnalk_window_on_session_signaling_room_joined (GnalkSession *session G_GNUC_UNUSED,
                                                const char    *room_token G_GNUC_UNUSED,
                                                GnalkWindow  *self)
{
  g_message ("Window: session signaling room joined - call buttons enabled");
  gtk_widget_set_sensitive (self->audio_call_button, TRUE);
  gtk_widget_set_sensitive (self->video_call_button, TRUE);
}

void
gnalk_window_on_session_call_capability_changed (GnalkSession *session G_GNUC_UNUSED,
                                                  gboolean       can_call,
                                                  GnalkWindow  *self)
{
  g_message ("Window: session call capability changed: %d", can_call);
  gtk_widget_set_sensitive (self->audio_call_button, can_call);
  gtk_widget_set_sensitive (self->video_call_button, can_call);
}

void
gnalk_window_on_session_auth_failed (GnalkSession *session G_GNUC_UNUSED,
                                      const char    *error_message,
                                      GnalkWindow  *self)
{
  g_warning ("Window: session auth failed: %s", error_message);

  /* Hide connecting state and show error in login view */
  gnalk_window_login_show_error (self, error_message);
}

void
gnalk_window_on_session_incoming_call (GnalkSession *session G_GNUC_UNUSED,
                                        const char    *room_token,
                                        const char    *caller_id,
                                        GnalkWindow  *self)
{
  g_message ("Window: incoming call in room %s from %s", room_token, caller_id);

  /* Show incoming call banner using caller_id as the display name */
  gnalk_incoming_call_banner_show_call (self->incoming_call_banner, room_token, caller_id);
}

void
gnalk_window_on_session_typing (GnalkSession *session G_GNUC_UNUSED,
                                 const char    *room_token,
                                 const char    *session_id,
                                 const char    *display_name,
                                 gboolean       is_typing,
                                 GnalkWindow  *self)
{
  /* Only show typing indicator for current room */
  if (g_strcmp0 (room_token, self->current_room_token) != 0)
    return;

  gnalk_typing_indicator_set_typing (self->typing_indicator, session_id, display_name, is_typing);
}

void
gnalk_window_on_session_signaling_ready (GnalkSession *session,
                                          GnalkWindow  *self)
{
  g_message ("Window: session signaling ready");

  /* Copy ICE servers from session to application for call window to use.
   * The session parses ICE servers from the signaling hello message,
   * but the call window retrieves them from the application global state. */
  if (session)
    {
      GtkApplication *gtk_app = gtk_window_get_application (GTK_WINDOW (self));
      GnalkApplication *app = GNALK_APPLICATION (gtk_app);
      const char *stun = gnalk_session_get_stun_server (session);
      const char *turn = gnalk_session_get_turn_server (session);
      const char *turn_user = gnalk_session_get_turn_username (session);
      const char *turn_cred = gnalk_session_get_turn_credential (session);
      gnalk_application_set_ice_servers (app, stun, turn, turn_user, turn_cred);
      g_message ("Window: ICE servers copied to app: STUN=%s, TURN=%s",
                 stun ? stun : "(none)", turn ? turn : "(none)");
    }

  /* If we have a pending room to join, do it now */
  if (self->pending_room_token)
    {
      if (self->active_session)
        gnalk_session_join_room (self->active_session, self->pending_room_token);
      g_clear_pointer (&self->pending_room_token, g_free);
    }

  /* Update call button now that signaling is ready */
  gnalk_window_update_call_button_state (self);
}

/* ============ Message Composer Functions ============
 *
 * File upload and message send functions have been moved to gnalk-window-message-composer.c:
 * - gnalk_window_generate_reference_id()
 * - gnalk_window_process_upload_queue()
 * - gnalk_window_queue_file_upload()
 * - gnalk_window_clear_attachments()
 * - gnalk_window_on_message_input_send_requested()
 */

static void
on_preferences_action (GSimpleAction *action,
                       GVariant      *parameter,
                       gpointer       user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  GnalkPreferencesDialog *dialog = gnalk_preferences_dialog_new ();
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

void
gnalk_window_on_add_account_requested (GnalkAccountSwitcher *switcher G_GNUC_UNUSED,
                                        GnalkWindow          *self)
{
  /* Show login view (this initializes/refreshes GOA accounts) */
  gnalk_window_show_login_view (self);

  /* Clear login fields AFTER show_login_view to override any defaults */
  gnalk_login_view_set_server (self->login_view, "");
  gnalk_login_view_set_user (self->login_view, "");
  gnalk_login_view_set_password (self->login_view, "");
  gnalk_login_view_clear_error (self->login_view);
}

void
gnalk_window_on_sign_out_requested (GnalkAccountSwitcher *switcher G_GNUC_UNUSED,
                                     gint64                 account_id,
                                     GnalkWindow          *self)
{
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  GnalkAccountManager *account_manager = gnalk_application_get_account_manager (app);

  /* Get account info before removing */
  GnalkAccount *account = gnalk_account_manager_get_account_by_id (account_manager, account_id);
  if (!account)
    return;

  const char *server_url = gnalk_account_get_server_url (account);
  const char *user_id = gnalk_account_get_user_id (account);

  /* Delete password from keyring */
  if (server_url && user_id)
    gnalk_keyring_delete_password (server_url, user_id, NULL, NULL);

  /* Disable autologin for this account */
  gnalk_account_set_autologin (account, FALSE);
  gnalk_account_manager_save_account (account_manager, account);

  /* Stop the session */
  gnalk_session_manager_stop_session (self->session_manager, account_id);

  /* If no sessions left, show login view */
  guint session_count = gnalk_session_manager_get_session_count (self->session_manager);
  if (session_count == 0)
    {
      gnalk_login_view_set_server (self->login_view, "");
      gnalk_login_view_set_user (self->login_view, "");
      gnalk_login_view_set_password (self->login_view, "");
      gnalk_login_view_clear_error (self->login_view);
      gtk_stack_set_visible_child_name (GTK_STACK (self->main_stack), "login");
    }
}

static void
on_about_action (GSimpleAction *action,
                 GVariant      *parameter,
                 gpointer       user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  const char *developers[] = { "Skilpa", NULL };

  adw_show_about_dialog (GTK_WIDGET (self),
                          "application-name", "Gnalk",
                          "application-icon", "net.skilpa.gnalk",
                          "developer-name", "Skilpa",
                          "version", PACKAGE_VERSION,
                          "developers", developers,
                          "copyright", "Copyright 2025 Skilpa",
                          "license-type", GTK_LICENSE_GPL_3_0,
                          "comments", "AI-generated Nextcloud Talk client.\n\n"
                                      "Generated by: Claude Opus 4.5\n"
                                      "Model: claude-opus-4-5-20251101\n"
                                      "Date: January 2025",
                          NULL);
}

/* ============ UI Helper Functions (exported for room-switch.c) ============ */

void
gnalk_window_update_call_button_state (GnalkWindow *self)
{
  gboolean can_call;

  /* Use Session for call capability check */
  if (self->active_session)
    {
      can_call = gnalk_session_can_call (self->active_session);
    }
  else
    {
      /* Fallback to direct check */
      gboolean has_room = self->has_active_room && self->current_room_token != NULL;
      gboolean signaling_connected = self->signaling && gnalk_signaling_is_connected (self->signaling);
      can_call = has_room && signaling_connected;
    }

  gtk_widget_set_sensitive (self->audio_call_button, can_call);
  gtk_widget_set_sensitive (self->video_call_button, can_call);
}

void
gnalk_window_update_room_status_icon (GnalkWindow *self,
                                        gint          room_type,
                                        const char   *user_status)
{
  /* Only show status for 1:1 conversations (type 1) */
  if (room_type != 1)
    {
      gnalk_conversation_header_set_user_status (self->conversation_header, NULL);
      return;
    }

  gnalk_conversation_header_set_user_status (self->conversation_header, user_status);
}

void
gnalk_window_update_room_call_icon (GnalkWindow *self,
                                      gint          room_type,
                                      gint64        in_call)
{
  g_message ("update_room_call_icon: room_type=%d in_call=%ld", room_type, (long)in_call);

  /* Only show call indicator for 1:1 conversations (type 1) */
  if (room_type != 1)
    {
      g_message ("update_room_call_icon: hiding, room_type != 1");
      gnalk_conversation_header_set_call_active (self->conversation_header, FALSE);
      return;
    }

  if (in_call > 0)
    {
      gnalk_conversation_header_set_call_active (self->conversation_header, TRUE);
      g_message ("update_room_call_icon: showing icon");
    }
  else
    {
      gnalk_conversation_header_set_call_active (self->conversation_header, FALSE);
      g_message ("update_room_call_icon: hiding, in_call <= 0");
    }
}

static void
start_call (GnalkWindow *self,
            gboolean      with_video)
{
  if (!self->current_room_token)
    return;

  if (!gnalk_signaling_is_connected (self->signaling))
    {
      g_warning ("Cannot start call: signaling not connected");
      return;
    }

  if (!self->current_session_id)
    {
      g_warning ("Cannot start call: no session ID");
      return;
    }

  g_message ("start_call: starting %s call for room %s with session %.30s...",
             with_video ? "video" : "audio",
             self->current_room_token, self->current_session_id);

  /* Start the call with the current session.
   * The session should be valid since we joined the room when viewing it.
   * Note: We don't refresh the session here because that would invalidate
   * the signaling connection (server sends disinvite for old session). */
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
  GnalkCallWindow *call_window = gnalk_call_window_new (app);

  gnalk_call_window_start_call (call_window,
                                 self->current_room_token,
                                 self->current_room_name,
                                 self->current_session_id,
                                 self->http_client,
                                 self->webrtc,
                                 self->signaling,
                                 self->avatar_cache,
                                 with_video);

  /* Track active call window */
  self->active_call_window = call_window;
  g_signal_connect (call_window, "destroy", G_CALLBACK (on_call_window_destroy), self);

  gtk_window_present (GTK_WINDOW (call_window));
}

static void on_info_dialog_media_clicked (GnalkConversationInfoDialog *dialog,
                                           const char                   *file_id,
                                           const char                   *file_mimetype,
                                           const char                   *file_path,
                                           const char                   *file_name,
                                           GnalkWindow                 *self);

/* Forward declarations for participant action handlers */
static void on_participant_chat_requested (GnalkParticipantsList *list,
                                            const char             *actor_id,
                                            GnalkWindow           *self);
static void on_participant_promote_requested (GnalkParticipantsList *list,
                                               gint64                  attendee_id,
                                               const char             *actor_id,
                                               GnalkWindow           *self);
static void on_participant_demote_requested (GnalkParticipantsList *list,
                                              gint64                  attendee_id,
                                              const char             *actor_id,
                                              GnalkWindow           *self);
static void on_participant_remove_requested (GnalkParticipantsList *list,
                                              gint64                  attendee_id,
                                              const char             *actor_id,
                                              GnalkWindow           *self);

static void
on_conversation_info_requested (GnalkConversationHeader *header G_GNUC_UNUSED,
                                 GnalkWindow             *self)
{
  if (!self->current_room_token)
    return;

  /* Find the conversation data for the current room */
  GnalkConversationData *conversation = NULL;
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
  for (guint i = 0; i < n_items; i++)
    {
      GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
      const char *token = gnalk_conversation_data_get_token (conv);
      if (g_strcmp0 (token, self->current_room_token) == 0)
        {
          conversation = conv;
          break;
        }
      g_object_unref (conv);
    }

  if (!conversation)
    return;

  GnalkConversationInfoDialog *dialog = gnalk_conversation_info_dialog_new ();
  gnalk_conversation_info_dialog_set_http_client (dialog, self->http_client);
  gnalk_conversation_info_dialog_set_avatar_cache (dialog, self->avatar_cache);
  gnalk_conversation_info_dialog_set_conversation (dialog, conversation);

  /* Set my_user_id for context menu permission checks */
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  const char *my_user_id = gnalk_application_get_user_id (app);
  gnalk_conversation_info_dialog_set_my_user_id (dialog, my_user_id);

  g_signal_connect (dialog, "media-clicked",
                    G_CALLBACK (on_info_dialog_media_clicked), self);

  /* Connect participant action signals - use same handlers as participants list
     since they don't use the first parameter */
  g_signal_connect (dialog, "chat-requested",
                    G_CALLBACK (on_participant_chat_requested), self);
  g_signal_connect (dialog, "promote-requested",
                    G_CALLBACK (on_participant_promote_requested), self);
  g_signal_connect (dialog, "demote-requested",
                    G_CALLBACK (on_participant_demote_requested), self);
  g_signal_connect (dialog, "remove-requested",
                    G_CALLBACK (on_participant_remove_requested), self);

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));

  g_object_unref (conversation);
}

void
gnalk_window_on_participants_toggled (GtkToggleButton *button,
                                        GnalkWindow    *self)
{
  gboolean active = gtk_toggle_button_get_active (button);

  /* Update participants list visibility */
  gnalk_participants_list_set_visible (self->participants_list, active);

  /* Save preference to settings */
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  GSettings *settings = gnalk_application_get_settings (app);
  g_settings_set_boolean (settings, "show-participants", active);
}

/* Helper functions for blocking/unblocking participants toggle signal */
void
gnalk_window_block_participants_toggle (GnalkWindow *self)
{
  g_signal_handlers_block_by_func (self->participants_toggle_button,
                                   gnalk_window_on_participants_toggled, self);
}

void
gnalk_window_unblock_participants_toggle (GnalkWindow *self)
{
  g_signal_handlers_unblock_by_func (self->participants_toggle_button,
                                     gnalk_window_on_participants_toggled, self);
}

static void
on_video_call_clicked (GtkButton    *button,
                       GnalkWindow *self)
{
  start_call (self, TRUE);
}

static void
on_audio_call_clicked (GtkButton    *button,
                       GnalkWindow *self)
{
  start_call (self, FALSE);
}

/* Search functions moved to gnalk-window-conversation-list.c:
 * - on_sidebar_search_changed
 * - on_search_contacts_response
 * - on_sidebar_search_result_selected
 * - find_existing_conversation_for_user
 */

static gint
compare_messages_by_id_for_store (gconstpointer a,
                                  gconstpointer b,
                                  gpointer      user_data)
{
  gint64 id_a = gnalk_message_data_get_id ((GnalkMessageData *)a);
  gint64 id_b = gnalk_message_data_get_id ((GnalkMessageData *)b);
  return (id_a > id_b) - (id_a < id_b);
}

/* Conversation context menu signal handlers - exported for gnalk-window-conversation-list.c */
void
gnalk_window_on_conv_leave_requested (GnalkConversationRow *row G_GNUC_UNUSED,
                                       const char            *token,
                                       GnalkWindow          *self)
{
  if (!token)
    return;

  /* Leave via controller - the controller's signal will handle UI clearing */
  if (self->conversation_controller)
    gnalk_conversation_controller_leave (self->conversation_controller, token);
}

void
gnalk_window_on_conv_delete_requested (GnalkConversationRow *row G_GNUC_UNUSED,
                                        const char            *token,
                                        GnalkWindow          *self)
{
  if (!token)
    return;

  /* Delete via controller - the controller's signal will handle UI clearing */
  if (self->conversation_controller)
    gnalk_conversation_controller_delete (self->conversation_controller, token);
}

void
gnalk_window_on_conv_favorite_toggled (GnalkConversationRow *row G_GNUC_UNUSED,
                                        const char            *token,
                                        gboolean               favorite,
                                        GnalkWindow          *self)
{
  if (!token)
    return;

  /* Set favorite via controller */
  if (self->conversation_controller)
    gnalk_conversation_controller_set_favorite (self->conversation_controller, token, favorite);
}

/* Conversation list factory callbacks moved to gnalk-window-conversation-list.c:
 * - on_conv_list_item_setup/bind/unbind
 * - update_conversation_active_states
 */

/* Update all bound message rows with new display mode.
 * Iterates through the tracked bound rows hash table to update each visible row. */
static void
apply_message_display_mode (GnalkWindow *self)
{
  if (!self->bound_message_rows)
    return;

  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init (&iter, self->bound_message_rows);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      GnalkMessageRow *row = GNALK_MESSAGE_ROW (key);
      gnalk_message_row_set_display_mode (row, self->message_display_mode);
      gnalk_message_row_set_one_to_one (row, self->current_room_type == 1);
    }
}

/* Callback when breakpoint is applied (window becomes wide) */
static void
on_wide_breakpoint_applied (AdwBreakpoint *breakpoint G_GNUC_UNUSED,
                            GnalkWindow  *self)
{
  self->message_display_mode = GNALK_MESSAGE_DISPLAY_ALIGNED;
  apply_message_display_mode (self);
}

/* Callback when breakpoint is unapplied (window becomes narrow) */
static void
on_wide_breakpoint_unapplied (AdwBreakpoint *breakpoint G_GNUC_UNUSED,
                              GnalkWindow  *self)
{
  self->message_display_mode = GNALK_MESSAGE_DISPLAY_BUBBLES;
  apply_message_display_mode (self);
}

/* Message list factory callbacks - proper data/widget separation */
static void
on_msg_list_item_setup (GtkListItemFactory *factory,
                        GtkListItem        *item,
                        gpointer            user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  /* Create a new message row widget */
  GnalkMessageRow *row = gnalk_message_row_new ();

  /* Connect signals */
  g_signal_connect (row, "reaction-requested", G_CALLBACK (on_reaction_requested), self);
  g_signal_connect (row, "quote-requested", G_CALLBACK (on_quote_requested), self);
  g_signal_connect (row, "edit-requested", G_CALLBACK (on_edit_requested), self);
  g_signal_connect (row, "delete-requested", G_CALLBACK (on_delete_requested), self);
  g_signal_connect (row, "scroll-to-message-requested", G_CALLBACK (on_scroll_to_message_requested), self);
  g_signal_connect (row, "file-clicked", G_CALLBACK (on_file_clicked), self);

  gtk_list_item_set_child (item, GTK_WIDGET (row));
}

static void
on_msg_list_item_bind (GtkListItemFactory *factory,
                       GtkListItem        *item,
                       gpointer            user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  GnalkMessageData *data = GNALK_MESSAGE_DATA (gtk_list_item_get_item (item));
  GnalkMessageRow *row = GNALK_MESSAGE_ROW (gtk_list_item_get_child (item));

  /* Set edit/delete timeouts from capabilities */
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  gint64 edit_timeout = gnalk_application_get_chat_edit_timeout (app);
  gint64 delete_timeout = gnalk_application_get_chat_delete_timeout (app);
  gnalk_message_row_set_edit_timeout (row, edit_timeout);
  gnalk_message_row_set_delete_timeout (row, delete_timeout);

  /* Populate the row from the data object first (sets is_self, is_system, etc.) */
  /* Note: gtk_list_item_get_item returns transfer-none, don't unref */
  gnalk_message_row_set_from_data (row, data, self->avatar_cache);

  /* Determine if this is the first message of a group (for avatar grouping).
   * This is purely about message grouping - display mode visibility is handled by apply_layout. */
  guint pos = gtk_list_item_get_position (item);
  gboolean show_avatar = TRUE;  /* First of group by default */
  const char *curr_system = gnalk_message_data_get_system_message (data);
  gboolean is_system_message = curr_system && *curr_system;

  /* Message grouping: hide avatar for consecutive messages from same author */
  if (pos > 0 && !is_system_message)
    {
      GnalkMessageData *prev = g_list_model_get_item (G_LIST_MODEL (self->messages_store), pos - 1);
      if (prev)
        {
          const char *prev_author = gnalk_message_data_get_author_id (prev);
          const char *curr_author = gnalk_message_data_get_author_id (data);
          const char *prev_system = gnalk_message_data_get_system_message (prev);

          /* Hide avatar if same author and previous wasn't a system message */
          if (g_strcmp0 (prev_author, curr_author) == 0 &&
              (!prev_system || !*prev_system))
            {
              show_avatar = FALSE;
            }
          g_object_unref (prev);
        }
    }

  /* Use pre-computed timestamp visibility from data model */
  gboolean show_timestamp = gnalk_message_data_get_show_timestamp (data);

  /* Set all state before display mode (which calls apply_layout) */
  gnalk_message_row_set_show_avatar (row, show_avatar);
  gnalk_message_row_set_show_timestamp (row, show_timestamp);
  gnalk_message_row_set_one_to_one (row, self->current_room_type == 1);

  /* Set display mode last - this calls apply_layout with all state set */
  gnalk_message_row_set_display_mode (row, self->message_display_mode);

  /* Apply bubble color from avatar (for both self and other messages) */
  const char *author_id = gnalk_message_data_get_author_id (data);
  const char *system_msg = gnalk_message_data_get_system_message (data);
  if (author_id && *author_id && (!system_msg || !*system_msg))
    {
      GdkRGBA color;
      if (gnalk_avatar_cache_get_user_color (self->avatar_cache, author_id, &color))
        gnalk_message_row_set_bubble_color (row, &color);
    }

  /* Check if file is already cached and show icon */
  const char *file_id = gnalk_message_row_get_file_id (row);
  const char *file_name = gnalk_message_row_get_file_name (row);
  if (file_id && *file_id)
    {
      g_autofree char *cache_dir = g_build_filename (g_get_user_cache_dir (), "gnalk", NULL);
      const char *name = (file_name && *file_name) ? file_name : "download";
      g_autofree char *safe_name = g_strdup_printf ("%s_%s", file_id, name);
      g_autofree char *cache_path = g_build_filename (cache_dir, safe_name, NULL);

      gnalk_message_row_set_file_cached (row, g_file_test (cache_path, G_FILE_TEST_EXISTS));
    }

  /* Apply unread visual state */
  gboolean is_unread = gnalk_message_data_get_is_unread (data);
  gnalk_message_row_set_unread (row, is_unread);

  /* Track bound unread messages for marking as read */
  if (is_unread)
    {
      gint64 msg_id = gnalk_message_data_get_id (data);
      gnalk_window_track_bound_unread_row (self, msg_id, GTK_WIDGET (row));
    }

  /* Track this bound row for display mode updates */
  g_hash_table_add (self->bound_message_rows, row);
}

static void
on_msg_list_item_unbind (GtkListItemFactory *factory,
                         GtkListItem        *item,
                         gpointer            user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  GnalkMessageData *data = GNALK_MESSAGE_DATA (gtk_list_item_get_item (item));
  GnalkMessageRow *row = GNALK_MESSAGE_ROW (gtk_list_item_get_child (item));

  /* Remove from bound unread tracking if it was unread */
  if (data && gnalk_message_data_get_is_unread (data))
    {
      gint64 msg_id = gnalk_message_data_get_id (data);
      gnalk_window_untrack_bound_unread_row (self, msg_id);
    }

  if (row)
    {
      /* Remove from bound message rows tracking */
      g_hash_table_remove (self->bound_message_rows, row);

      gtk_widget_remove_css_class (GTK_WIDGET (row), "message-highlight");
      gnalk_message_row_reset (row);
    }
}

/* Search results list factory callbacks moved to gnalk-window-conversation-list.c */

static void
on_message_view_load_older_requested (GnalkMessageView *view,
                                       gpointer           user_data)
{
  (void) view;
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (self->has_more_history && !gnalk_message_view_get_loading_older (self->message_view) &&
      self->current_room_token && self->oldest_loaded_message_id > 0)
    {
      g_debug ("Message view requested older messages");
      gnalk_window_load_older_messages (self);
    }
}

/* on_content_page_hiding is now gnalk_window_on_content_page_hiding in gnalk-window-room-switch.c */

static gboolean
attempt_autologin_idle (gpointer user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));

  /* Check if startup is complete (managers are ready).
   * If not, reschedule to try again after a short delay.
   * This handles the async startup where database opens after window. */
  GnalkAccountManager *account_manager = gnalk_application_get_account_manager (app);
  GnalkSessionManager *session_manager = gnalk_application_get_session_manager (app);

  if (!account_manager || !session_manager)
    {
      /* Startup not complete yet, try again in 10ms */
      g_timeout_add (10, attempt_autologin_idle, self);
      return G_SOURCE_REMOVE;
    }

  gnalk_window_attempt_autologin (self);
  return G_SOURCE_REMOVE;
}

/* Room join response and avatar loading are now in gnalk-window-room-switch.c */

/* Handler for call state changes from signaling */
void
gnalk_window_on_signaling_call_state_changed (GnalkSignaling *signaling,
                       const char      *actor_id,
                       gint64           in_call,
                       GnalkWindow    *self)
{
  (void) signaling;

  /* Delegate to the participants list component */
  gnalk_participants_list_update_call_state (self->participants_list, actor_id, in_call);
}

/* Room switch functions are now in gnalk-window-room-switch.c:
 * - gnalk_window_load_participants()
 * - gnalk_window_open_conversation_by_token()
 * - gnalk_window_maybe_restore_last_room()
 * - gnalk_window_on_sidebar_conversation_selected()
 * - gnalk_window_on_content_page_hiding()
 */

/* Message saving/loading functions are now in gnalk-window-message-display.c:
 * - gnalk_window_save_messages_to_db()
 * - gnalk_window_load_messages_from_db()
 * - gnalk_window_load_older_messages()
 */

static void
on_set_status_response (GnalkHttpClient *client,
                        JsonNode         *response,
                        GError           *error,
                        gpointer          user_data)
{
  (void) client;
  (void) response;
  (void) user_data;
  if (error)
    g_warning ("Failed to set status: %s", error->message);
}

static void
on_user_status_changed (GnalkUserStatusMenu *menu,
                        const char           *status,
                        GnalkWindow         *self)
{
  (void) menu;
  /* Send to server */
  gnalk_http_client_set_user_status (self->http_client, status, on_set_status_response, self);
}

/* Incoming call handling */

static void
hide_incoming_call (GnalkWindow *self)
{
  gnalk_incoming_call_banner_hide (self->incoming_call_banner);
}

static void
on_call_window_destroy (GtkWidget    *widget,
                        GnalkWindow *self)
{
  /* Clear active call window reference when destroyed */
  if (self->active_call_window == GNALK_CALL_WINDOW (widget))
    self->active_call_window = NULL;
}

void
gnalk_window_on_signaling_call_ended (GnalkSignaling *signaling G_GNUC_UNUSED,
                         GnalkWindow    *self)
{
  /* Hide incoming call banner when call ends for all */
  hide_incoming_call (self);
}

void
gnalk_window_on_signaling_room_disinvited (GnalkSignaling *signaling G_GNUC_UNUSED,
                              const char      *room_token,
                              GnalkWindow    *self)
{
  g_message ("Window: received disinvite from room %s", room_token);

  /* If we were disinvited from the current room, clear our room state
   * to prevent trying to rejoin after reconnection */
  if (self->current_room_token && g_strcmp0 (self->current_room_token, room_token) == 0)
    {
      g_message ("Window: clearing current room state due to disinvite");
      g_clear_pointer (&self->current_room_token, g_free);
      g_clear_pointer (&self->current_room_name, g_free);

      /* Cancel any pending room join */
      self->room_join_in_progress = FALSE;
    }
}

void
gnalk_window_on_signaling_incoming_call (GnalkSignaling *signaling,
                  const char      *session_id,
                  const char      *actor_id,
                  gint64           in_call_flags,
                  GnalkWindow    *self)
{
  (void) signaling;
  (void) session_id;
  (void) in_call_flags;

  /* Don't show incoming call if we're already in a call */
  if (self->active_call_window)
    return;

  /* Only show incoming call for the current room */
  if (!self->current_room_token)
    return;

  /* Show the incoming call banner - it handles dedup and state internally */
  gnalk_incoming_call_banner_show_call (self->incoming_call_banner,
                                          self->current_room_token,
                                          actor_id);
}

static void
on_incoming_call_accept_requested (GnalkIncomingCallBanner *banner,
                                    GnalkWindow             *self)
{
  (void) banner;

  if (!gnalk_incoming_call_banner_has_incoming_call (self->incoming_call_banner))
    return;

  /* Hide the banner */
  hide_incoming_call (self);

  /* Start the call - reuse the existing call button logic */
  if (!self->current_room_token)
    return;

  if (!gnalk_signaling_is_connected (self->signaling))
    {
      g_warning ("Cannot start call: signaling not connected");
      return;
    }

  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
  GnalkCallWindow *call_window = gnalk_call_window_new (app);

  gnalk_call_window_start_call (call_window,
                                 self->current_room_token,
                                 self->current_room_name,
                                 self->current_session_id,
                                 self->http_client,
                                 self->webrtc,
                                 self->signaling,
                                 self->avatar_cache,
                                 TRUE);

  /* Track active call window */
  self->active_call_window = call_window;
  g_signal_connect (call_window, "destroy", G_CALLBACK (on_call_window_destroy), self);

  gtk_window_present (GTK_WINDOW (call_window));
}

static void
on_incoming_call_refuse_requested (GnalkIncomingCallBanner *banner,
                                    GnalkWindow             *self)
{
  (void) banner;

  /* Just hide the banner - we don't need to do anything else
   * since we haven't actually joined the call yet */
  hide_incoming_call (self);
}

/* Typing indicator functions */

#define TYPING_RESEND_TIMEOUT_SEC 10

void
gnalk_window_on_signaling_typing (GnalkSignaling *signaling,
                     const char      *session_id,
                     const char      *display_name,
                     gboolean         is_typing,
                     GnalkWindow    *self)
{
  (void) signaling;
  gnalk_typing_indicator_set_typing (self->typing_indicator, session_id, display_name, is_typing);
}

static gboolean
on_typing_send_timer (gpointer user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  /* Check if still typing (text not empty) */
  const char *text = gnalk_message_input_get_text (self->message_input);
  if (text && *text)
    {
      /* Resend typing message */
      gnalk_signaling_send_typing (self->signaling, TRUE);
      return G_SOURCE_CONTINUE;
    }
  else
    {
      /* Stop typing */
      self->is_typing = FALSE;
      self->typing_send_timer_id = 0;
      gnalk_signaling_send_typing (self->signaling, FALSE);
      return G_SOURCE_REMOVE;
    }
}

static void
on_message_input_text_changed (GnalkMessageInput *input,
                                const char         *text,
                                GnalkWindow       *self)
{
  (void) input;
  gboolean has_text = text && *text;

  if (has_text && !self->is_typing)
    {
      /* Started typing */
      self->is_typing = TRUE;
      gnalk_signaling_send_typing (self->signaling, TRUE);

      /* Start timer to resend every 10 seconds */
      if (self->typing_send_timer_id == 0)
        self->typing_send_timer_id = g_timeout_add_seconds (TYPING_RESEND_TIMEOUT_SEC,
                                                             on_typing_send_timer,
                                                             self);
    }
  else if (!has_text && self->is_typing)
    {
      /* Stopped typing */
      self->is_typing = FALSE;
      gnalk_signaling_send_typing (self->signaling, FALSE);

      if (self->typing_send_timer_id > 0)
        {
          g_source_remove (self->typing_send_timer_id);
          self->typing_send_timer_id = 0;
        }
    }
}

/* ========== Controller Signal Handlers ========== */

/* Conversation controller signal handlers moved to gnalk-window-conversation-list.c:
 * - gnalk_window_on_conversation_controller_updated
 * - gnalk_window_on_conversation_controller_created
 * - gnalk_window_on_conversation_controller_left
 * - gnalk_window_on_conversation_controller_deleted
 * - gnalk_window_on_conversation_controller_new_messages
 */

/* Called when participant controller detects server has different data */
void
gnalk_window_on_participant_controller_changed (GnalkParticipantController *controller G_GNUC_UNUSED,
                                    const char                  *room_token,
                                    GnalkWindow                *self)
{
  g_debug ("Window: Participants changed for %s", room_token);

  /* Only update if this is the current room */
  if (!self->current_room_token || g_strcmp0 (room_token, self->current_room_token) != 0)
    return;

  /* Reload participants from database */
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  GnalkDatabase *db = gnalk_application_get_database (app);
  gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;
  const char *my_user_id = gnalk_application_get_user_id (app);

  g_debug ("on_participant_controller_changed: room=%s, account_id=%ld", room_token, (long)account_id);

  if (!db || account_id <= 0)
    {
      g_warning ("on_participant_controller_changed: skipping, db=%p, account_id=%ld", (void*)db, (long)account_id);
      return;
    }

  JsonArray *participants = gnalk_database_load_participants (db, account_id, room_token);
  if (!participants)
    {
      g_warning ("on_participant_controller_changed: no participants loaded from DB");
      return;
    }

  guint n_participants = json_array_get_length (participants);
  g_debug ("on_participant_controller_changed: loaded %u participants", n_participants);

  /* For 1:1 conversations, update room call icon for the other participant */
  for (guint i = 0; i < n_participants; i++)
    {
      JsonObject *participant = json_array_get_object_element (participants, i);
      const char *actor_id = json_object_get_string_member_with_default (participant, "actorId", "");
      gint64 in_call = json_object_get_int_member_with_default (participant, "inCall", 0);

      if (self->current_room_type == 1 && actor_id && *actor_id &&
          g_strcmp0 (actor_id, my_user_id) != 0)
        gnalk_window_update_room_call_icon (self, 1, in_call);
    }

  /* Get current user's participant type from conversation data */
  gint my_participant_type = 3;  /* Default to regular user */
  guint n_convs = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
  for (guint i = 0; i < n_convs; i++)
    {
      GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
      const char *token = gnalk_conversation_data_get_token (conv);
      if (g_strcmp0 (token, self->current_room_token) == 0)
        {
          my_participant_type = gnalk_conversation_data_get_participant_type (conv);
          g_object_unref (conv);
          break;
        }
      g_object_unref (conv);
    }

  /* Delegate to the participants list component */
  gnalk_participants_list_set_participants (self->participants_list, participants,
                                              self->avatar_cache, self->signaling,
                                              my_user_id, my_participant_type,
                                              self->current_room_type);

  /* For group rooms with participants, respect user preference and sync toggle */
  if (self->current_room_type != 1 && n_participants > 0)
    {
      GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
      GSettings *settings = gnalk_application_get_settings (app);
      gboolean show_participants = g_settings_get_boolean (settings, "show-participants");
      gnalk_participants_list_set_visible (self->participants_list, show_participants);
      gnalk_window_block_participants_toggle (self);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->participants_toggle_button), show_participants);
      gnalk_window_unblock_participants_toggle (self);
    }
  else
    gnalk_participants_list_hide (self->participants_list);

  json_array_unref (participants);
}

/* ============ Participant List Signal Handlers ============ */

typedef struct {
  GnalkWindow *window;
  char *actor_id;
} Create1to1Data;

static void
create_1to1_data_free (Create1to1Data *data)
{
  g_free (data->actor_id);
  g_free (data);
}

static void
on_create_1to1_response (GnalkHttpClient *client G_GNUC_UNUSED,
                          JsonNode         *response,
                          GError           *error,
                          gpointer          user_data)
{
  Create1to1Data *data = user_data;
  GnalkWindow *self = data->window;

  if (error)
    {
      g_warning ("Failed to create 1-to-1 conversation: %s", error->message);
      AdwToast *toast = adw_toast_new ("Failed to start chat");
      adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), toast);
      create_1to1_data_free (data);
      return;
    }

  if (!response)
    {
      create_1to1_data_free (data);
      return;
    }

  /* Parse response to get the room token */
  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    {
      create_1to1_data_free (data);
      return;
    }

  JsonObject *room_data = json_object_get_object_member (ocs, "data");
  if (!room_data)
    {
      create_1to1_data_free (data);
      return;
    }

  const char *token = json_object_get_string_member_with_default (room_data, "token", NULL);
  if (token && *token)
    {
      /* Add the new conversation to the store so it can be opened properly */
      GnalkConversationData *conv = gnalk_conversation_data_new_from_json (room_data, self->active_session);
      if (conv)
        {
          /* Insert at position 0 (will be sorted properly on next refresh) */
          g_list_store_insert (self->conversations_store, 0, conv);
          g_object_unref (conv);
        }

      /* Navigate to the new conversation */
      gnalk_window_open_room (self, token);
    }

  create_1to1_data_free (data);
}

static void
on_participant_chat_requested (GnalkParticipantsList *list G_GNUC_UNUSED,
                                const char             *actor_id,
                                GnalkWindow           *self)
{
  if (!actor_id || !*actor_id || !self->http_client)
    return;

  g_message ("Creating 1-to-1 conversation with %s", actor_id);

  Create1to1Data *data = g_new0 (Create1to1Data, 1);
  data->window = self;
  data->actor_id = g_strdup (actor_id);

  /* Create 1-to-1 conversation: roomType=1, invite=actorId */
  gnalk_http_client_create_conversation (self->http_client, 1, actor_id, NULL,
                                           on_create_1to1_response, data);
}

typedef struct {
  GnalkWindow *window;
  gint64 attendee_id;
  char *actor_id;
  char *action;  /* "promote", "demote", or "remove" */
} ModeratorActionData;

static void
moderator_action_data_free (ModeratorActionData *data)
{
  g_free (data->actor_id);
  g_free (data->action);
  g_free (data);
}

static void
on_moderator_action_response (GnalkHttpClient *client G_GNUC_UNUSED,
                               JsonNode         *response G_GNUC_UNUSED,
                               GError           *error,
                               gpointer          user_data)
{
  ModeratorActionData *data = user_data;
  GnalkWindow *self = data->window;

  if (error)
    {
      g_warning ("Moderator action '%s' failed: %s", data->action, error->message);
      g_autofree char *msg = g_strdup_printf ("Failed to %s", data->action);
      AdwToast *toast = adw_toast_new (msg);
      adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), toast);
    }
  else
    {
      /* Show success toast */
      const char *msg = NULL;
      if (g_strcmp0 (data->action, "promote") == 0)
        msg = "Promoted to moderator";
      else if (g_strcmp0 (data->action, "demote") == 0)
        msg = "Demoted from moderator";
      else if (g_strcmp0 (data->action, "remove") == 0)
        msg = "Removed from conversation";
      else
        msg = "Action completed";

      AdwToast *toast = adw_toast_new (msg);
      adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), toast);

      /* Refresh participants list */
      if (self->participant_controller)
        gnalk_participant_controller_refresh (self->participant_controller);
    }

  moderator_action_data_free (data);
}

static void
on_participant_promote_requested (GnalkParticipantsList *list G_GNUC_UNUSED,
                                   gint64                  attendee_id,
                                   const char             *actor_id,
                                   GnalkWindow           *self)
{
  if (!self->http_client || !self->current_room_token)
    return;

  g_message ("Promoting participant %s (attendee %" G_GINT64_FORMAT ") to moderator",
             actor_id ? actor_id : "(unknown)", attendee_id);

  ModeratorActionData *data = g_new0 (ModeratorActionData, 1);
  data->window = self;
  data->attendee_id = attendee_id;
  data->actor_id = g_strdup (actor_id);
  data->action = g_strdup ("promote");

  gnalk_http_client_promote_moderator (self->http_client, self->current_room_token,
                                         attendee_id, on_moderator_action_response, data);
}

static void
on_participant_demote_requested (GnalkParticipantsList *list G_GNUC_UNUSED,
                                  gint64                  attendee_id,
                                  const char             *actor_id,
                                  GnalkWindow           *self)
{
  if (!self->http_client || !self->current_room_token)
    return;

  g_message ("Demoting participant %s (attendee %" G_GINT64_FORMAT ") from moderator",
             actor_id ? actor_id : "(unknown)", attendee_id);

  ModeratorActionData *data = g_new0 (ModeratorActionData, 1);
  data->window = self;
  data->attendee_id = attendee_id;
  data->actor_id = g_strdup (actor_id);
  data->action = g_strdup ("demote");

  gnalk_http_client_demote_moderator (self->http_client, self->current_room_token,
                                        attendee_id, on_moderator_action_response, data);
}

static void
on_participant_remove_requested (GnalkParticipantsList *list G_GNUC_UNUSED,
                                  gint64                  attendee_id,
                                  const char             *actor_id,
                                  GnalkWindow           *self)
{
  if (!self->http_client || !self->current_room_token)
    return;

  g_message ("Removing participant %s (attendee %" G_GINT64_FORMAT ") from room",
             actor_id ? actor_id : "(unknown)", attendee_id);

  ModeratorActionData *data = g_new0 (ModeratorActionData, 1);
  data->window = self;
  data->attendee_id = attendee_id;
  data->actor_id = g_strdup (actor_id);
  data->action = g_strdup ("remove");

  gnalk_http_client_remove_attendee (self->http_client, self->current_room_token,
                                       attendee_id, on_moderator_action_response, data);
}

/* ============ Login handlers ============ */

/* Login view helper wrappers are now in gnalk-window-login.c:
 * - gnalk_window_login_show_error()
 * - gnalk_window_login_set_connecting()
 */

/* NOTE: LoginController signal handlers have been removed.
 * Login is now handled by Session/SessionManager.
 * - on_session_state_changed handles state transitions
 * - on_session_auth_failed handles auth failures
 * - on_session_conversations_updated handles conversation loading
 */

/* ============ Message Controller Signal Handlers ============ */

void
gnalk_window_on_message_controller_sent (GnalkMessageController *controller G_GNUC_UNUSED,
                            const char              *reference_id,
                            gint64                   message_id,
                            gint64                   timestamp,
                            GnalkWindow            *self)
{
  /* Find and update the pending message by reference ID */
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *msg_data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
      const char *existing_ref = gnalk_message_data_get_reference_id (msg_data);

      if (existing_ref && g_strcmp0 (existing_ref, reference_id) == 0)
        {
          /* Update the message with server-confirmed data */
          gnalk_message_data_confirm_sent (msg_data, message_id, timestamp);

          /* Force UI update by remove/insert at same position (preserve order while pending) */
          g_object_ref (msg_data);
          g_list_store_remove (self->messages_store, i);
          g_list_store_insert (self->messages_store, i, msg_data);
          g_object_unref (msg_data);

          g_debug ("Message confirmed sent via controller: ref=%s id=%ld", reference_id, message_id);
          g_object_unref (msg_data);

          /* Check if any pending messages remain - if not, re-sort the list */
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
            {
              /* All messages confirmed - re-sort by ID to fix any ordering issues */
              g_list_store_sort (self->messages_store, compare_messages_by_id_for_store, NULL);
            }

          break;
        }
      g_object_unref (msg_data);
    }
}

void
gnalk_window_on_message_controller_send_failed (GnalkMessageController *controller G_GNUC_UNUSED,
                                   const char              *reference_id,
                                   const char              *error,
                                   GnalkWindow            *self)
{
  g_warning ("Message send failed (ref=%s): %s", reference_id, error);
  /* TODO: Mark the pending message as failed in the UI */
  (void)self;
}

void
gnalk_window_on_message_controller_edited (GnalkMessageController *controller G_GNUC_UNUSED,
                              gint64                   message_id,
                              GnalkWindow            *self)
{
  g_debug ("Message edited via controller: id=%ld", message_id);
  /* The actual UI update comes from the message_edited system message in polling */
  (void)self;
}

void
gnalk_window_on_message_controller_deleted (GnalkMessageController *controller G_GNUC_UNUSED,
                               gint64                   message_id,
                               GnalkWindow            *self)
{
  /* Update the message in the store to show as deleted */
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  gboolean near_bottom = FALSE;
  guint deleted_pos = 0;

  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
      if (gnalk_message_data_get_id (msg) == message_id)
        {
          deleted_pos = i;
          near_bottom = (i >= n_items - 3);

          /* Just change message text to deleted */
          gnalk_message_data_set_message (msg, _("Message deleted by you"));

          /* Force list view to rebind by remove/insert */
          g_object_ref (msg);
          g_list_store_remove (self->messages_store, i);
          g_list_store_insert (self->messages_store, i, msg);
          g_object_unref (msg);
          g_object_unref (msg);
          break;
        }
      g_object_unref (msg);
    }

  /* Scroll to maintain position */
  if (near_bottom)
    gnalk_message_view_scroll_to_bottom (self->message_view);
  else if (deleted_pos > 0)
    gtk_list_view_scroll_to (gnalk_message_view_get_list_view (self->message_view), deleted_pos,
                             GTK_LIST_SCROLL_NONE, NULL);
}

typedef struct {
  GtkListView *list_view;
  guint        position;
} ScrollToPositionData;

static gboolean
scroll_to_position_idle (gpointer user_data)
{
  ScrollToPositionData *data = user_data;
  gtk_list_view_scroll_to (data->list_view, data->position,
                           GTK_LIST_SCROLL_NONE, NULL);
  g_free (data);
  return G_SOURCE_REMOVE;
}

void
gnalk_window_on_message_controller_reaction_added (GnalkMessageController *controller G_GNUC_UNUSED,
                                      gint64                   message_id,
                                      const char              *emoji,
                                      GnalkWindow            *self)
{
  /* Find the message and add the reaction to its data */
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
      if (gnalk_message_data_get_id (msg) == message_id)
        {
          gnalk_message_data_add_reaction (msg, emoji);

          /* Force list view to rebind by remove/insert */
          g_object_ref (msg);
          g_list_store_remove (self->messages_store, i);
          g_list_store_insert (self->messages_store, i, msg);
          g_object_unref (msg);

          /* Schedule scroll restoration after layout update */
          ScrollToPositionData *scroll_data = g_new (ScrollToPositionData, 1);
          scroll_data->list_view = gnalk_message_view_get_list_view (self->message_view);
          scroll_data->position = i;
          g_idle_add (scroll_to_position_idle, scroll_data);

          g_object_unref (msg);
          break;
        }
      g_object_unref (msg);
    }
}

void
gnalk_window_on_message_controller_upload_completed (GnalkMessageController *controller G_GNUC_UNUSED,
                                        const char              *reference_id,
                                        GnalkWindow            *self)
{
  /* Find the pending message by reference_id and mark it as sent */
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
      const char *ref = gnalk_message_data_get_reference_id (msg);

      if (ref && g_strcmp0 (ref, reference_id) == 0)
        {
          gnalk_message_data_set_is_uploading (msg, FALSE);
          gnalk_message_data_set_status (msg, GNALK_MESSAGE_DATA_STATUS_SENT);

          /* Force UI update by remove/insert */
          g_object_ref (msg);
          g_list_store_remove (self->messages_store, i);
          g_list_store_insert (self->messages_store, i, msg);
          g_object_unref (msg);

          g_object_unref (msg);
          break;
        }
      g_object_unref (msg);
    }

  /* Process next queued upload */
  self->upload_in_progress = FALSE;
  gnalk_window_process_upload_queue (self);

  /* Clear attachment UI */
  gnalk_window_clear_attachments (self);
}

void
gnalk_window_on_message_controller_upload_failed (GnalkMessageController *controller G_GNUC_UNUSED,
                                     const char              *reference_id,
                                     const char              *error G_GNUC_UNUSED,
                                     GnalkWindow            *self)
{
  /* Find the pending message by reference_id and mark it as failed */
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
      const char *ref = gnalk_message_data_get_reference_id (msg);

      if (ref && g_strcmp0 (ref, reference_id) == 0)
        {
          gnalk_message_data_set_is_uploading (msg, FALSE);
          gnalk_message_data_set_status (msg, GNALK_MESSAGE_DATA_STATUS_FAILED);

          /* Force UI update by remove/insert */
          g_object_ref (msg);
          g_list_store_remove (self->messages_store, i);
          g_list_store_insert (self->messages_store, i, msg);
          g_object_unref (msg);

          g_object_unref (msg);
          break;
        }
      g_object_unref (msg);
    }

  /* Process next queued upload even on failure */
  self->upload_in_progress = FALSE;
  gnalk_window_process_upload_queue (self);

  /* Clear attachment UI */
  gnalk_window_clear_attachments (self);
}

/* process_messages_array, on_message_controller_messages_loaded, on_message_controller_poll_response,
 * on_message_controller_older_messages_loaded are now in gnalk-window-message-display.c */

/* on_login_view_connect_clicked is now in gnalk-window-login.c */

static void
on_new_conversation_clicked (GnalkConversationSidebar *sidebar G_GNUC_UNUSED,
                             GnalkWindow              *self)
{
  GnalkNewConversationDialog *dialog = gnalk_new_conversation_dialog_new ();

  gnalk_new_conversation_dialog_set_http_client (dialog, self->http_client);
  gnalk_new_conversation_dialog_set_avatar_cache (dialog, self->avatar_cache);

  g_signal_connect (dialog, "conversation-created",
                    G_CALLBACK (on_new_conversation_created), self);

  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

static void
on_new_conversation_created (GnalkNewConversationDialog *dialog,
                             const char                  *room_token,
                             GnalkWindow                *self)
{
  /* Reload from DB - the dialog already created the conversation on server,
   * controller's polling will sync it to DB shortly */
  gnalk_window_load_conversations_from_db (self);
}

/* Login functions (start_login, start_guest_login, show_login_view, attempt_autologin,
 * show_offline_main_view, on_login_view_connect_clicked, login_set_connecting, login_show_error)
 * have been moved to gnalk-window-login.c */

static void
on_reaction_requested (GnalkMessageRow *row G_GNUC_UNUSED,
                       gint64            message_id,
                       const char       *emoji,
                       gpointer          user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (!self->message_controller || !self->current_room_token)
    return;

  /* Add reaction via controller - signal handler will update UI on success */
  gnalk_message_controller_set_room (self->message_controller, self->current_room_token);
  gnalk_message_controller_add_reaction (self->message_controller, message_id, emoji);
}

static gboolean
add_highlight_class_idle (gpointer user_data)
{
  GtkWidget *widget = GTK_WIDGET (user_data);
  if (GTK_IS_WIDGET (widget))
    gtk_widget_add_css_class (widget, "message-highlight");
  return G_SOURCE_REMOVE;
}

static void
find_and_highlight_row (GtkWidget *widget, gpointer user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (self->highlight_message_id <= 0)
    return;

  if (GNALK_IS_MESSAGE_ROW (widget))
    {
      GnalkMessageRow *row = GNALK_MESSAGE_ROW (widget);
      gint64 row_id = gnalk_message_row_get_id (row);
      if (row_id == self->highlight_message_id)
        {
          /* Remove class and re-add after a frame to restart animation */
          gtk_widget_remove_css_class (widget, "message-highlight");
          g_timeout_add (50, add_highlight_class_idle, widget);
          self->highlight_message_id = 0;  /* Found it, stop searching */
        }
    }
}

static void
iterate_children (GtkWidget *widget, gpointer user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (self->highlight_message_id <= 0)
    return;

  find_and_highlight_row (widget, user_data);

  /* Recurse into children */
  GtkWidget *child = gtk_widget_get_first_child (widget);
  while (child && self->highlight_message_id > 0)
    {
      iterate_children (child, user_data);
      child = gtk_widget_get_next_sibling (child);
    }
}

static gboolean
apply_highlight_timeout (gpointer user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (self->highlight_message_id <= 0)
    return G_SOURCE_REMOVE;

  /* Recursively search for the message row in the widget tree */
  iterate_children (GTK_WIDGET (gnalk_message_view_get_list_view (self->message_view)), self);

  self->highlight_message_id = 0;
  self->highlight_clear_timer_id = 0;
  return G_SOURCE_REMOVE;
}

static void
on_scroll_to_message_requested (GnalkMessageRow *row,
                                gint64            message_id,
                                gpointer          user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (!self->messages_store || message_id <= 0)
    return;

  /* Find the message index by ID */
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *msg_data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
      if (msg_data && gnalk_message_data_get_id (msg_data) == message_id)
        {
          /* Store target for highlighting */
          self->highlight_message_id = message_id;

          /* Scroll to this position with focus */
          gtk_list_view_scroll_to (gnalk_message_view_get_list_view (self->message_view), i,
                                   GTK_LIST_SCROLL_FOCUS, NULL);

          /* Apply highlight after scroll animation completes */
          if (self->highlight_clear_timer_id)
            g_source_remove (self->highlight_clear_timer_id);
          self->highlight_clear_timer_id = g_timeout_add (100, apply_highlight_timeout, self);

          g_object_unref (msg_data);
          return;
        }
      if (msg_data)
        g_object_unref (msg_data);
    }
}

static void
on_quote_preview_scroll_requested (GnalkConversationActions *actions,
                                   gint64                     message_id,
                                   GnalkWindow              *self)
{
  (void) actions;
  on_scroll_to_message_requested (NULL, message_id, self);
}

static void
on_quote_closed (GnalkConversationActions *actions,
                 gboolean                   was_editing,
                 GnalkWindow              *self)
{
  (void) actions;

  /* If we were editing, clear the entry as well */
  if (was_editing)
    gnalk_message_input_clear (self->message_input);

  self->reply_to_message_id = 0;
  self->edit_message_id = 0;
  gnalk_conversation_actions_clear_quote (self->conversation_actions);
}

/* ============ File Attachment Functions ============ */

/* clear_attachments moved to gnalk-window-message-composer.c as gnalk_window_clear_attachments */

static void
on_attachment_closed (GnalkConversationActions *actions,
                      GnalkWindow              *self)
{
  (void) actions;
  gnalk_window_clear_attachments (self);
}

static const char *
get_icon_name_for_mime_type (const char *mime_type)
{
  if (!mime_type)
    return "text-x-generic-symbolic";

  if (g_str_has_prefix (mime_type, "image/"))
    return "image-x-generic-symbolic";
  if (g_str_has_prefix (mime_type, "video/"))
    return "video-x-generic-symbolic";
  if (g_str_has_prefix (mime_type, "audio/"))
    return "audio-x-generic-symbolic";
  if (g_str_has_prefix (mime_type, "text/"))
    return "text-x-generic-symbolic";
  if (g_strstr_len (mime_type, -1, "pdf"))
    return "application-pdf-symbolic";
  if (g_strstr_len (mime_type, -1, "zip") ||
      g_strstr_len (mime_type, -1, "tar") ||
      g_strstr_len (mime_type, -1, "gzip") ||
      g_strstr_len (mime_type, -1, "archive"))
    return "package-x-generic-symbolic";
  if (g_strstr_len (mime_type, -1, "word") ||
      g_strstr_len (mime_type, -1, "document"))
    return "x-office-document-symbolic";
  if (g_strstr_len (mime_type, -1, "spreadsheet") ||
      g_strstr_len (mime_type, -1, "excel"))
    return "x-office-spreadsheet-symbolic";
  if (g_strstr_len (mime_type, -1, "presentation") ||
      g_strstr_len (mime_type, -1, "powerpoint"))
    return "x-office-presentation-symbolic";

  return "text-x-generic-symbolic";
}

static void
on_file_dialog_response (GObject      *source,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GListModel) files = NULL;

  files = gtk_file_dialog_open_multiple_finish (GTK_FILE_DIALOG (source), result, &error);
  if (!files)
    {
      if (!g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
          !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
        g_warning ("File dialog error: %s", error->message);
      return;
    }

  /* Clear any previous attachments */
  gnalk_window_clear_attachments (self);

  /* Ensure array exists */
  if (!self->attachments)
    self->attachments = g_ptr_array_new_with_free_func ((GDestroyNotify) attachment_info_free);

  guint n_files = g_list_model_get_n_items (files);

  for (guint i = 0; i < n_files; i++)
    {
      g_autoptr(GFile) file = g_list_model_get_item (files, i);
      if (!file)
        continue;

      g_autofree char *path = g_file_get_path (file);
      g_autofree char *basename = g_file_get_basename (file);
      g_autofree char *content_type = NULL;

      g_autoptr(GFileInfo) info = g_file_query_info (file,
                                                       G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                                       G_FILE_QUERY_INFO_NONE,
                                                       NULL, NULL);
      if (info)
        content_type = g_strdup (g_file_info_get_content_type (info));

      /* Read file contents */
      gchar *contents = NULL;
      gsize length = 0;
      g_autoptr(GError) read_error = NULL;

      if (!g_file_get_contents (path, &contents, &length, &read_error))
        {
          g_warning ("Failed to read file %s: %s", path, read_error->message);
          continue;
        }

      /* Create attachment info */
      AttachmentInfo *att = g_new0 (AttachmentInfo, 1);
      att->file_path = g_strdup (path);
      att->file_bytes = g_bytes_new_take (contents, length);
      att->content_type = g_strdup (content_type);
      att->file_name = g_strdup (basename);

      g_ptr_array_add (self->attachments, att);

      /* Prepare preview texture for images */
      GdkTexture *preview_texture = NULL;
      if (content_type && g_str_has_prefix (content_type, "image/"))
        preview_texture = gdk_texture_new_from_file (file, NULL);

      const char *icon_name = get_icon_name_for_mime_type (content_type);

      /* Add to UI preview */
      gnalk_conversation_actions_add_attachment (self->conversation_actions,
                                                   basename, icon_name, preview_texture);

      if (preview_texture)
        g_object_unref (preview_texture);
    }
}

static void
on_attach_clicked (GtkButton    *button,
                   GnalkWindow *self)
{
  (void) button;

  GtkFileDialog *dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, "Attach Files");
  gtk_file_dialog_set_modal (dialog, TRUE);

  gtk_file_dialog_open_multiple (dialog, GTK_WINDOW (self), NULL,
                                  on_file_dialog_response, self);
  g_object_unref (dialog);
}

static void
on_voice_message_recorded (GnalkMessageInput *input G_GNUC_UNUSED,
                           GBytes             *audio_data,
                           gdouble             duration,
                           GnalkWindow       *self)
{
  g_message ("Window: Voice message recorded, duration=%.1f", duration);

  if (!self->message_controller || !self->current_room_token)
    {
      g_warning ("Window: Cannot send voice message - no message controller or room");
      return;
    }

  /* Generate filename: Talk recording YYYY-MM-DD HH-mm-ss.ogg */
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *filename = g_date_time_format (now, "Talk recording %Y-%m-%d %H-%M-%S.ogg");

  /* Generate reference ID for tracking */
  g_autofree char *reference_id = g_uuid_string_random ();

  /* Upload as voice message */
  gnalk_message_controller_upload_voice_message (self->message_controller, filename, audio_data, reference_id);
}

static void
on_photo_captured (GnalkMediaCaptureDialog *dialog G_GNUC_UNUSED,
                   GBytes                   *jpeg_data,
                   GnalkWindow             *self)
{
  g_message ("Window: Photo captured");

  if (!self->message_controller || !self->current_room_token)
    {
      g_warning ("Window: Cannot send photo - no message controller or room");
      return;
    }

  /* Generate filename: Talk photo YYYY-MM-DD HH-mm-ss.jpg */
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *filename = g_date_time_format (now, "Talk photo %Y-%m-%d %H-%M-%S.jpg");

  /* Generate reference ID for tracking */
  g_autofree char *reference_id = g_uuid_string_random ();

  /* Upload photo */
  gnalk_message_controller_upload_file (self->message_controller,
                                          filename,
                                          jpeg_data,
                                          "image/jpeg",
                                          NULL,  /* caption */
                                          0,     /* reply_to */
                                          reference_id);
}

static void
on_video_captured (GnalkMediaCaptureDialog *dialog G_GNUC_UNUSED,
                   GBytes                   *video_data,
                   GnalkWindow             *self)
{
  g_message ("Window: Video captured");

  if (!self->message_controller || !self->current_room_token)
    {
      g_warning ("Window: Cannot send video - no message controller or room");
      return;
    }

  /* Generate filename: Talk video YYYY-MM-DD HH-mm-ss.webm */
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *filename = g_date_time_format (now, "Talk video %Y-%m-%d %H-%M-%S.webm");

  /* Generate reference ID for tracking */
  g_autofree char *reference_id = g_uuid_string_random ();

  /* Upload video */
  gnalk_message_controller_upload_file (self->message_controller,
                                          filename,
                                          video_data,
                                          "video/webm",
                                          NULL,  /* caption */
                                          0,     /* reply_to */
                                          reference_id);
}

static void
on_media_capture_dialog_closed (GnalkMediaCaptureDialog *dialog G_GNUC_UNUSED,
                                GnalkWindow             *self)
{
  self->media_capture_dialog = NULL;
}

static void
on_camera_clicked (GnalkMessageInput *input G_GNUC_UNUSED,
                   GnalkWindow       *self)
{
  g_message ("Window: Camera button clicked");

  /* Prevent opening multiple dialogs (double-click protection) */
  if (self->media_capture_dialog != NULL)
    {
      g_message ("Window: Media capture dialog already open, ignoring");
      return;
    }

  /* Create and present the media capture dialog */
  self->media_capture_dialog = gnalk_media_capture_dialog_new ();

  g_signal_connect (self->media_capture_dialog, "photo-captured", G_CALLBACK (on_photo_captured), self);
  g_signal_connect (self->media_capture_dialog, "video-captured", G_CALLBACK (on_video_captured), self);
  g_signal_connect (self->media_capture_dialog, "closed", G_CALLBACK (on_media_capture_dialog_closed), self);

  adw_dialog_present (ADW_DIALOG (self->media_capture_dialog), GTK_WIDGET (self));
}

static void
on_quote_preview_image_loaded (GnalkAvatarCache *cache,
                               GdkTexture        *texture,
                               gpointer           user_data)
{
  (void) cache;
  GnalkWindow *self = GNALK_WINDOW (user_data);

  gnalk_conversation_actions_set_quote_image (self->conversation_actions, texture);
}

static void
on_quote_requested (GnalkMessageRow *row,
                    gint64            message_id,
                    const char       *author_name,
                    const char       *message_text,
                    gpointer          user_data)
{
  (void) row;
  GnalkWindow *self = GNALK_WINDOW (user_data);

  self->reply_to_message_id = message_id;

  gnalk_conversation_actions_set_reply (self->conversation_actions, message_id, author_name, message_text);

  /* Look up the message in the store to check for file preview */
  if (self->messages_store && self->avatar_cache)
    {
      guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
      for (guint i = 0; i < n_items; i++)
        {
          GnalkMessageData *msg_data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
          if (msg_data && gnalk_message_data_get_id (msg_data) == message_id)
            {
              JsonObject *params = gnalk_message_data_get_message_parameters (msg_data);
              if (params && json_object_has_member (params, "file"))
                {
                  JsonNode *file_node = json_object_get_member (params, "file");
                  if (file_node && JSON_NODE_HOLDS_OBJECT (file_node))
                    {
                      JsonObject *file = json_node_get_object (file_node);
                      const char *file_id = json_object_get_string_member_with_default (file, "id", NULL);
                      const char *preview_avail = json_object_get_string_member_with_default (file, "preview-available", "no");

                      if (file_id && g_strcmp0 (preview_avail, "yes") == 0)
                        {
                          gnalk_avatar_cache_get_file_preview (self->avatar_cache, file_id, 96, 96,
                                                                on_quote_preview_image_loaded, self);
                        }
                    }
                }
              g_object_unref (msg_data);
              break;
            }
          if (msg_data)
            g_object_unref (msg_data);
        }
    }

  gnalk_message_input_grab_focus (self->message_input);
}

static void
on_edit_requested (GnalkMessageRow *row,
                   gint64            message_id,
                   const char       *message_text,
                   gpointer          user_data)
{
  (void) row;
  GnalkWindow *self = GNALK_WINDOW (user_data);

  /* Set edit mode - clear any reply in progress */
  self->edit_message_id = message_id;
  self->reply_to_message_id = 0;

  /* Show edit indicator using conversation_actions */
  gnalk_conversation_actions_set_edit (self->conversation_actions, message_id, message_text);

  /* Put the message text in the entry for editing */
  gnalk_message_input_set_text (self->message_input, message_text);
  gnalk_message_input_grab_focus (self->message_input);
}

static void
on_delete_requested (GnalkMessageRow *row G_GNUC_UNUSED,
                     gint64            message_id,
                     gpointer          user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (!self->current_room_token || message_id <= 0)
    return;

  /* Delete message via controller */
  gnalk_message_controller_set_room (self->message_controller, self->current_room_token);
  gnalk_message_controller_delete_message (self->message_controller, message_id);
}

typedef struct {
  char     *file_path;
  GWeakRef  row_ref;
  GWeakRef  window_ref;  /* Window reference for info dialog downloads */
  gboolean  is_video;    /* TRUE when downloaded from info dialog for video player */
} FileDownloadData;

static void
file_download_data_free (FileDownloadData *data)
{
  g_free (data->file_path);
  g_weak_ref_clear (&data->row_ref);
  g_weak_ref_clear (&data->window_ref);
  g_free (data);
}

static void
on_file_downloaded (GnalkHttpClient *client,
                    GBytes           *bytes,
                    const char       *content_type,
                    GError           *error,
                    gpointer          user_data)
{
  FileDownloadData *data = user_data;
  GnalkMessageRow *row = g_weak_ref_get (&data->row_ref);

  /* Hide spinner */
  if (row)
    gnalk_message_row_set_downloading (row, FALSE);

  if (error)
    {
      g_warning ("Failed to download file: %s", error->message);
      if (row)
        g_object_unref (row);
      file_download_data_free (data);
      return;
    }

  if (!bytes)
    {
      g_warning ("No data received for file download");
      if (row)
        g_object_unref (row);
      file_download_data_free (data);
      return;
    }

  /* Write bytes to cache file */
  g_autoptr(GError) write_error = NULL;
  gsize size;
  gconstpointer file_data = g_bytes_get_data (bytes, &size);

  if (!g_file_set_contents (data->file_path, file_data, size, &write_error))
    {
      g_warning ("Failed to write cache file: %s", write_error->message);
      if (row)
        g_object_unref (row);
      file_download_data_free (data);
      return;
    }

  /* Show cached icon */
  if (row)
    {
      gnalk_message_row_set_file_cached (row, TRUE);

      /* Check if this is an audio file - set on audio player instead of launching */
      if (gnalk_message_row_has_audio_file (row))
        {
          gnalk_message_row_set_audio_file (row, data->file_path);
          g_object_unref (row);
          file_download_data_free (data);
          return;
        }

      /* Check if this is a video file - open video player dialog */
      if (gnalk_message_row_has_video_file (row))
        {
          /* Get the window from the row's toplevel */
          GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (row));
          if (GTK_IS_WINDOW (root))
            {
              /* Set thumbnail file path so it displays */
              gnalk_message_row_set_video_file (row, data->file_path);

              /* Open video player dialog */
              g_autofree char *file_name = g_path_get_basename (data->file_path);
              GnalkVideoPlayerDialog *dialog = gnalk_video_player_dialog_new ();
              gnalk_video_player_dialog_set_file (dialog, data->file_path, file_name);
              adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (root));
            }
          g_object_unref (row);
          file_download_data_free (data);
          return;
        }

      g_object_unref (row);
    }
  else if (data->is_video)
    {
      /* Video download from info dialog (no row) - open in video player */
      GnalkWindow *window = g_weak_ref_get (&data->window_ref);
      if (window)
        {
          g_autofree char *file_name = g_path_get_basename (data->file_path);
          GnalkVideoPlayerDialog *dialog = gnalk_video_player_dialog_new ();
          gnalk_video_player_dialog_set_file (dialog, data->file_path, file_name);
          adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (window));
          g_object_unref (window);
          file_download_data_free (data);
          return;
        }
    }

  /* Open with default application */
  g_autofree char *uri = g_filename_to_uri (data->file_path, NULL, NULL);
  if (uri)
    {
      g_autoptr(GError) launch_error = NULL;
      if (!g_app_info_launch_default_for_uri (uri, NULL, &launch_error))
        {
          g_warning ("Failed to open file: %s", launch_error->message);
        }
    }

  file_download_data_free (data);
}

/* Check if a mimetype represents an image */
static gboolean
is_image_mimetype (const char *mimetype)
{
  if (!mimetype)
    return FALSE;

  return g_str_has_prefix (mimetype, "image/");
}

/* Collect all images from the messages store and find the index of target file */
static GPtrArray *
collect_conversation_images (GnalkWindow *self,
                             const char   *target_file_id,
                             guint        *out_index)
{
  GPtrArray *images = g_ptr_array_new_with_free_func ((GDestroyNotify) gnalk_image_info_free);
  guint target_idx = 0;

  guint n_messages = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  for (guint i = 0; i < n_messages; i++)
    {
      g_autoptr(GnalkMessageData) data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
      if (!data)
        continue;

      JsonObject *params = gnalk_message_data_get_message_parameters (data);
      if (!params)
        continue;

      /* Look for file parameter in message parameters */
      JsonObjectIter iter;
      const char *key;
      JsonNode *value;

      json_object_iter_init (&iter, params);
      while (json_object_iter_next (&iter, &key, &value))
        {
          if (!JSON_NODE_HOLDS_OBJECT (value))
            continue;

          JsonObject *param_obj = json_node_get_object (value);
          const char *type = json_object_get_string_member_with_default (param_obj, "type", "");

          if (g_strcmp0 (type, "file") != 0)
            continue;

          const char *mimetype = json_object_get_string_member_with_default (param_obj, "mimetype", "");
          if (!is_image_mimetype (mimetype))
            continue;

          /* Found an image - extract info */
          const char *fid = json_object_get_string_member_with_default (param_obj, "id", "");
          const char *fname = json_object_get_string_member_with_default (param_obj, "name", "");
          const char *fpath = json_object_get_string_member_with_default (param_obj, "path", "");
          gint64 msg_id = gnalk_message_data_get_id (data);

          GnalkImageInfo *info = gnalk_image_info_new (fid, fname, fpath, mimetype, msg_id);
          g_ptr_array_add (images, info);

          /* Check if this is the target */
          if (target_file_id && g_strcmp0 (fid, target_file_id) == 0)
            target_idx = images->len - 1;
        }
    }

  if (out_index)
    *out_index = target_idx;

  return images;
}

/* Handler for when gallery downloads and caches an image */
static void
on_image_gallery_image_cached (GnalkImageGallery *gallery G_GNUC_UNUSED,
                               const char         *file_id,
                               const char         *file_name,
                               GnalkWindow       *self)
{
  /* Find all message rows with this file_id and update their cache state */
  if (!self->messages_store || !file_id || !*file_id)
    return;

  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
      if (!msg)
        continue;

      JsonObject *params = gnalk_message_data_get_message_parameters (msg);
      if (params && json_object_has_member (params, "file"))
        {
          JsonNode *file_node = json_object_get_member (params, "file");
          if (file_node && JSON_NODE_HOLDS_OBJECT (file_node))
            {
              JsonObject *file = json_node_get_object (file_node);
              const char *msg_file_id = json_object_get_string_member_with_default (file, "id", NULL);

              if (msg_file_id && g_strcmp0 (msg_file_id, file_id) == 0)
                {
                  /* Force re-bind by remove/insert so cache state is updated */
                  g_object_ref (msg);
                  g_list_store_remove (self->messages_store, i);
                  g_list_store_insert (self->messages_store, i, msg);
                  g_object_unref (msg);
                  g_object_unref (msg);
                  break;
                }
            }
        }
      g_object_unref (msg);
    }
}

/* Called when the image gallery dialog is closed */
static void
on_image_gallery_closed (AdwDialog *dialog G_GNUC_UNUSED,
                         gpointer   user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  /* Clear the pointer since AdwDialog destroys itself when closed */
  self->image_gallery = NULL;
}

/* Ensure the image gallery is created */
static void
ensure_image_gallery (GnalkWindow *self)
{
  if (self->image_gallery)
    return;

  self->image_gallery = gnalk_image_gallery_new ();
  gnalk_image_gallery_set_http_client (self->image_gallery, self->http_client);
  gnalk_image_gallery_set_avatar_cache (self->image_gallery, self->avatar_cache);

  /* Connect to image-cached signal to update row cache state */
  g_signal_connect (self->image_gallery, "image-cached",
                    G_CALLBACK (on_image_gallery_image_cached), self);

  /* Clear pointer when dialog closes (AdwDialog destroys itself on close) */
  g_signal_connect (self->image_gallery, "closed",
                    G_CALLBACK (on_image_gallery_closed), self);
}

static void
on_file_clicked (GnalkMessageRow *row,
                 const char       *file_id,
                 const char       *file_mimetype,
                 const char       *file_link G_GNUC_UNUSED,
                 const char       *file_name,
                 const char       *file_path,
                 gpointer          user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (!file_path || !*file_path)
    {
      g_warning ("No file path available for download");
      return;
    }

  /* Check if this is an image - open in gallery */
  if (is_image_mimetype (file_mimetype))
    {
      ensure_image_gallery (self);

      guint current_index = 0;
      GPtrArray *images = collect_conversation_images (self, file_id, &current_index);

      if (images->len > 0)
        {
          gnalk_image_gallery_set_images (self->image_gallery, images, current_index);
          gnalk_image_gallery_present (self->image_gallery, GTK_WIDGET (self));
          return;
        }

      /* No images found (shouldn't happen), fall through to download */
      g_ptr_array_unref (images);
    }

  /* Build cache path: ~/.cache/gnalk/{file_id}_{filename} */
  g_autofree char *cache_dir = g_build_filename (g_get_user_cache_dir (), "gnalk", NULL);
  g_mkdir_with_parents (cache_dir, 0700);

  const char *name = (file_name && *file_name) ? file_name : "download";
  g_autofree char *safe_name = g_strdup_printf ("%s_%s", file_id ? file_id : "unknown", name);
  g_autofree char *cache_path = g_build_filename (cache_dir, safe_name, NULL);

  /* Check if this is an audio file - handle via audio player */
  gboolean is_audio = gnalk_message_row_has_audio_file (row);
  if (is_audio)
    {
      /* Check if file is already cached */
      if (g_file_test (cache_path, G_FILE_TEST_EXISTS))
        {
          /* Set loading briefly so auto-play triggers */
          gnalk_message_row_set_audio_loading (row, TRUE);
          gnalk_message_row_set_audio_file (row, cache_path);
          return;
        }

      /* Show loading state on audio player */
      gnalk_message_row_set_audio_loading (row, TRUE);

      /* Prepare callback data with weak reference to row */
      FileDownloadData *data = g_new0 (FileDownloadData, 1);
      data->file_path = g_strdup (cache_path);
      g_weak_ref_init (&data->row_ref, row);
      g_weak_ref_init (&data->window_ref, NULL);

      /* Download file via WebDAV */
      gnalk_http_client_download_file (self->http_client, file_path,
                                         on_file_downloaded, data);
      return;
    }

  /* Check if this is a video file - handle via video player dialog */
  gboolean is_video = gnalk_message_row_has_video_file (row);
  if (is_video)
    {
      /* Check if file is already cached */
      if (g_file_test (cache_path, G_FILE_TEST_EXISTS))
        {
          /* Show cached icon and open video player dialog */
          gnalk_message_row_set_file_cached (row, TRUE);

          /* Open video player dialog */
          GnalkVideoPlayerDialog *dialog = gnalk_video_player_dialog_new ();
          gnalk_video_player_dialog_set_file (dialog, cache_path, file_name);
          adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
          return;
        }

      /* Show download spinner */
      gnalk_message_row_set_downloading (row, TRUE);

      /* Prepare callback data with weak reference to row */
      FileDownloadData *data = g_new0 (FileDownloadData, 1);
      data->file_path = g_strdup (cache_path);
      g_weak_ref_init (&data->row_ref, row);
      g_weak_ref_init (&data->window_ref, NULL);

      /* Download file via WebDAV */
      gnalk_http_client_download_file (self->http_client, file_path,
                                         on_file_downloaded, data);
      return;
    }

  /* Non-image, non-audio, non-video file: download and open with default application */

  /* Check if file is already cached */
  if (g_file_test (cache_path, G_FILE_TEST_EXISTS))
    {
      /* Show cached icon and open directly */
      gnalk_message_row_set_file_cached (row, TRUE);

      g_autofree char *uri = g_filename_to_uri (cache_path, NULL, NULL);
      if (uri)
        {
          g_autoptr(GError) error = NULL;
          if (!g_app_info_launch_default_for_uri (uri, NULL, &error))
            g_warning ("Failed to open cached file: %s", error->message);
        }
      return;
    }

  /* Show download spinner */
  gnalk_message_row_set_downloading (row, TRUE);

  /* Prepare callback data with weak reference to row */
  FileDownloadData *data = g_new0 (FileDownloadData, 1);
  data->file_path = g_strdup (cache_path);
  g_weak_ref_init (&data->row_ref, row);
  g_weak_ref_init (&data->window_ref, NULL);

  /* Download file via WebDAV */
  gnalk_http_client_download_file (self->http_client, file_path,
                                     on_file_downloaded, data);
}

/* Handler for media clicks from the conversation info dialog */
static void
on_info_dialog_media_clicked (GnalkConversationInfoDialog *dialog,
                               const char                   *file_id,
                               const char                   *file_mimetype,
                               const char                   *file_path,
                               const char                   *file_name,
                               GnalkWindow                 *self)
{
  if (!file_path || !*file_path)
    {
      g_warning ("No file path available for media");
      return;
    }

  /* Check if this is an image - open in gallery (stacks on top of info dialog) */
  if (file_mimetype && g_str_has_prefix (file_mimetype, "image/"))
    {
      ensure_image_gallery (self);

      guint current_index = 0;
      GPtrArray *images = collect_conversation_images (self, file_id, &current_index);

      if (images->len > 0)
        {
          gnalk_image_gallery_set_images (self->image_gallery, images, current_index);
          gnalk_image_gallery_present (self->image_gallery, GTK_WIDGET (self));
          return;
        }

      /* No images found in conversation, just show this single image */
      g_ptr_array_unref (images);

      /* Create a single-image array */
      images = g_ptr_array_new_with_free_func ((GDestroyNotify) gnalk_image_info_free);
      GnalkImageInfo *info = gnalk_image_info_new (file_id, file_name, file_path, file_mimetype, 0);
      g_ptr_array_add (images, info);

      gnalk_image_gallery_set_images (self->image_gallery, images, 0);
      gnalk_image_gallery_present (self->image_gallery, GTK_WIDGET (self));
      return;
    }

  /* Build cache path */
  g_autofree char *cache_dir = g_build_filename (g_get_user_cache_dir (), "gnalk", NULL);
  g_mkdir_with_parents (cache_dir, 0700);

  const char *name = (file_name && *file_name) ? file_name : "download";
  g_autofree char *safe_name = g_strdup_printf ("%s_%s", file_id ? file_id : "unknown", name);
  g_autofree char *cache_path = g_build_filename (cache_dir, safe_name, NULL);

  /* Check if this is a video - open in in-app player */
  gboolean is_video = file_mimetype && g_str_has_prefix (file_mimetype, "video/");
  if (is_video)
    {
      /* Check if file is already cached */
      if (g_file_test (cache_path, G_FILE_TEST_EXISTS))
        {
          /* Open video player dialog */
          GnalkVideoPlayerDialog *video_dialog = gnalk_video_player_dialog_new ();
          gnalk_video_player_dialog_set_file (video_dialog, cache_path, file_name);
          adw_dialog_present (ADW_DIALOG (video_dialog), GTK_WIDGET (self));
          return;
        }

      /* Download video first, then open in player */
      FileDownloadData *data = g_new0 (FileDownloadData, 1);
      data->file_path = g_strdup (cache_path);
      data->is_video = TRUE;
      g_weak_ref_init (&data->row_ref, NULL);     /* No row to update */
      g_weak_ref_init (&data->window_ref, self);  /* Keep window ref for video player */

      gnalk_http_client_download_file (self->http_client, file_path,
                                         on_file_downloaded, data);
      return;
    }

  /* Non-video file: download and open with default application */

  /* Check if file is already cached */
  if (g_file_test (cache_path, G_FILE_TEST_EXISTS))
    {
      g_autofree char *uri = g_filename_to_uri (cache_path, NULL, NULL);
      if (uri)
        {
          g_autoptr(GError) error = NULL;
          if (!g_app_info_launch_default_for_uri (uri, NULL, &error))
            g_warning ("Failed to open cached file: %s", error->message);
        }
      return;
    }

  /* Download file via WebDAV */
  FileDownloadData *data = g_new0 (FileDownloadData, 1);
  data->file_path = g_strdup (cache_path);
  g_weak_ref_init (&data->row_ref, NULL);     /* No row to update */
  g_weak_ref_init (&data->window_ref, NULL);  /* Not a video, no window needed */

  gnalk_http_client_download_file (self->http_client, file_path,
                                     on_file_downloaded, data);
}

static void
gnalk_window_dispose (GObject *object)
{
  GnalkWindow *self = GNALK_WINDOW (object);

  /* Leave the current room when window is closed.
   * This ensures push notifications work instead of signaling messages. */
  if (self->current_room_token)
    {
      g_message ("Window dispose: leaving room %s", self->current_room_token);

      /* Tell controllers we're leaving the room */
      if (self->message_controller)
        gnalk_message_controller_set_room (self->message_controller, NULL);
      if (self->participant_controller)
        gnalk_participant_controller_set_active_room (self->participant_controller, NULL, 0);
      if (self->conversation_controller)
        gnalk_conversation_controller_stop_polling (self->conversation_controller);

      if (self->signaling)
        gnalk_signaling_leave_room (self->signaling, self->current_room_token);

      if (self->http_client)
        gnalk_http_client_leave_room (self->http_client, self->current_room_token, NULL, NULL);

      /* Clear active room in background sync so notifications work */
      GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
      if (app)
        gnalk_application_set_active_room (app, NULL);
    }

  /* Disconnect from signaling BEFORE widgets are destroyed.
   * Signaling is owned by session and survives window close. */
  if (self->signaling)
    {
      g_signal_handlers_disconnect_by_data (self->signaling, self);
      self->signaling = NULL;
    }

  /* Disconnect from session signals BEFORE widgets are destroyed.
   * This prevents signal callbacks from accessing destroyed widgets. */
  /* Disconnect session signals - now handled by account-session.c */
  gnalk_window_disconnect_active_session (self);

  /* Disconnect from session_manager signals */
  if (self->session_manager)
    {
      g_signal_handlers_disconnect_by_data (self->session_manager, self);
      self->session_manager = NULL;
    }

  G_OBJECT_CLASS (gnalk_window_parent_class)->dispose (object);
}

static void
gnalk_window_finalize (GObject *object)
{
  GnalkWindow *self = GNALK_WINDOW (object);

  /* Clean up controllers (timers are managed by controllers) */
  g_clear_object (&self->conversation_controller);
  g_clear_object (&self->participant_controller);

  if (self->typing_send_timer_id > 0)
    {
      g_source_remove (self->typing_send_timer_id);
      self->typing_send_timer_id = 0;
    }

  if (self->room_switch_timer_id > 0)
    {
      g_source_remove (self->room_switch_timer_id);
      self->room_switch_timer_id = 0;
    }
  g_clear_pointer (&self->pending_room_token, g_free);

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

  /* Clean up read status tracking (removes weak refs and cancels timers) */
  gnalk_window_clear_read_status_tracking (self);
  g_clear_pointer (&self->bound_unread_rows, g_hash_table_unref);
  g_clear_pointer (&self->bound_conversation_rows, g_hash_table_unref);
  g_clear_pointer (&self->bound_message_rows, g_hash_table_unref);
  g_clear_pointer (&self->visible_unread_ids, g_array_unref);

  /* signaling/sync_manager are borrowed from session.
   * http_client and push_client are borrowed from application. Just clear references. */
  self->http_client = NULL;
  self->push_client = NULL;

  /* These are always owned by the window */
  g_clear_object (&self->avatar_cache);
  g_clear_object (&self->webrtc);
  g_clear_object (&self->message_controller);
  g_clear_object (&self->conversations_store);
  g_clear_object (&self->messages_store);
  g_clear_object (&self->search_results_store);
  g_clear_object (&self->tab_manager);

  g_free (self->current_room_token);
  g_free (self->current_room_name);
  g_free (self->current_room_description);
  g_free (self->current_session_id);
  g_free (self->user_name);
  g_free (self->login_room_token);

  /* Attachment cleanup */
  g_clear_pointer (&self->attachments, g_ptr_array_unref);

  /* Upload queue cleanup */
  if (self->upload_queue)
    {
      while (!g_queue_is_empty (self->upload_queue))
        {
          UploadQueueItem *item = g_queue_pop_head (self->upload_queue);
          upload_queue_item_free (item);
        }
      g_queue_free (self->upload_queue);
      self->upload_queue = NULL;
    }

  /* Autologin cleanup */
  g_free (self->autologin_server);
  g_free (self->autologin_user);
  g_free (self->autologin_password);

  G_OBJECT_CLASS (gnalk_window_parent_class)->finalize (object);
}

/* Focus tracking for notification decisions.
 * Note: We do NOT reload conversations here. The ConversationController polls
 * and emits signals when data changes - the window reacts to those signals.
 * Reloading on activation was causing bugs (popovers close because GTK4
 * popovers briefly change window active state) and is architecturally redundant. */
static void
on_window_is_active_changed (GObject    *object,
                              GParamSpec *pspec G_GNUC_UNUSED,
                              gpointer    user_data G_GNUC_UNUSED)
{
  GnalkWindow *self = GNALK_WINDOW (object);
  GtkWindow *window = GTK_WINDOW (object);
  gboolean is_active = gtk_window_is_active (window);

  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (window));
  if (app)
    {
      gnalk_application_set_window_focused (app, is_active);

      /* Handle pending room from notification click */
      if (is_active && !self->was_active)
        {
          g_autofree char *pending_room = gnalk_application_consume_pending_room (app);
          if (pending_room)
            {
              g_message ("Window activated: opening pending room %s", pending_room);
              gnalk_window_open_conversation_by_token (self, pending_room);
            }
        }
    }

  self->was_active = is_active;
}

/* Background sync service notification - refresh messages and conversations */
static void
on_app_background_sync_messages_synced (GnalkBackgroundSync *sync G_GNUC_UNUSED,
                                         const char           *room_token,
                                         gpointer              user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  g_message ("Background sync service: room %s synced, refreshing UI", room_token);

  /* Reload conversation list from cache (background sync already updated DB) */
  gnalk_window_load_conversations_from_db (self);

  /* If we're viewing this room, reload messages from DB to show the new ones */
  if (self->current_room_token && g_strcmp0 (room_token, self->current_room_token) == 0)
    {
      g_debug ("Background sync service: refreshing messages for current room %s", room_token);
      gnalk_window_load_messages_from_db (self, room_token);
    }
  else if (self->tab_manager && gnalk_room_tab_manager_has_tab (self->tab_manager, room_token))
    {
      /* Room has a tab but is not the active room - the tab's store will be
       * refreshed from DB when the user switches to it. We don't need to
       * do anything here since the tab system handles this on foreground. */
      g_debug ("Background sync service: room %s has background tab, will refresh on switch", room_token);
    }
}

static void
gnalk_window_class_init (GnalkWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = gnalk_window_get_property;
  object_class->set_property = gnalk_window_set_property;
  object_class->dispose = gnalk_window_dispose;
  object_class->finalize = gnalk_window_finalize;

  gnalk_window_properties[GNALK_WINDOW_PROP_USER_NAME] =
    g_param_spec_string ("user-name", NULL, NULL, "", G_PARAM_READWRITE);

  gnalk_window_properties[GNALK_WINDOW_PROP_CURRENT_ROOM_NAME] =
    g_param_spec_string ("current-room-name", NULL, NULL, "", G_PARAM_READWRITE);

  gnalk_window_properties[GNALK_WINDOW_PROP_HAS_ACTIVE_ROOM] =
    g_param_spec_boolean ("has-active-room", NULL, NULL, FALSE, G_PARAM_READWRITE);

  g_object_class_install_properties (object_class, GNALK_WINDOW_N_PROPS, gnalk_window_properties);

  /* Ensure all custom widget types are registered before loading the template */
  g_type_ensure (GNALK_TYPE_LOGIN_VIEW);
  g_type_ensure (GNALK_TYPE_NETWORK_STATUS_BAR);
  g_type_ensure (GNALK_TYPE_USER_STATUS_MENU);
  g_type_ensure (GNALK_TYPE_ACCOUNT_SWITCHER);
  g_type_ensure (GNALK_TYPE_CONVERSATION_SIDEBAR);
  g_type_ensure (GNALK_TYPE_CONVERSATION_HEADER);
  g_type_ensure (GNALK_TYPE_INCOMING_CALL_BANNER);
  g_type_ensure (GNALK_TYPE_PARTICIPANTS_LIST);
  g_type_ensure (GNALK_TYPE_MESSAGE_VIEW);
  g_type_ensure (GNALK_TYPE_TYPING_INDICATOR);
  g_type_ensure (GNALK_TYPE_CONVERSATION_ACTIONS);
  g_type_ensure (GNALK_TYPE_MESSAGE_INPUT);

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-window.ui");
  /* Login view */
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, main_stack);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, login_view);

  /* Main app view */
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, toast_overlay);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, connecting_spinner);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, split_view);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, conversation_sidebar);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, message_view);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, account_switcher);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, content_page);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, message_input);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, conversation_header);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, participants_toggle_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, audio_call_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, video_call_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, conversation_actions);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, user_status_menu);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, typing_indicator);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, network_status_bar);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, incoming_call_banner);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, participants_list);
}

static void
gnalk_window_init (GnalkWindow *self)
{
  static GActionEntry win_actions[] = {
    { "preferences", on_preferences_action },
  };

  static GActionEntry app_actions[] = {
    { "about", on_about_action },
  };

  gtk_widget_init_template (GTK_WIDGET (self));

  g_action_map_add_action_entries (G_ACTION_MAP (self), win_actions, G_N_ELEMENTS (win_actions), self);

  GApplication *app = g_application_get_default ();
  g_action_map_add_action_entries (G_ACTION_MAP (app), app_actions, G_N_ELEMENTS (app_actions), self);

  /* Connect to network monitor for offline/online status */
  /* Use g_signal_connect_object for application-level objects that outlive the window */
  GnalkNetworkMonitor *net_monitor = gnalk_application_get_network_monitor (GNALK_APPLICATION (app));
  if (net_monitor)
    {
      gboolean is_online = gnalk_network_monitor_is_online (net_monitor);
      self->was_offline = !is_online;
      /* Don't show banner on init - let autologin handle it */
      g_signal_connect_object (net_monitor, "network-changed",
                               G_CALLBACK (on_network_changed), self, G_CONNECT_DEFAULT);
    }

  /* Connect to session manager for multi-account support (new architecture) */
  self->session_manager = gnalk_application_get_session_manager (GNALK_APPLICATION (app));
  if (self->session_manager)
    {
      g_signal_connect_object (self->session_manager, "active-session-changed",
                               G_CALLBACK (gnalk_window_on_session_manager_active_changed), self, G_CONNECT_DEFAULT);
      g_signal_connect_object (self->session_manager, "any-session-state-changed",
                               G_CALLBACK (gnalk_window_on_session_manager_any_state_changed), self, G_CONNECT_DEFAULT);
      g_signal_connect_object (self->session_manager, "conversations-changed",
                               G_CALLBACK (gnalk_window_on_session_manager_conversations_changed), self, G_CONNECT_DEFAULT);

      /* Set up account switcher (created from template) */
      if (self->account_switcher)
        {
          gnalk_account_switcher_set_session_manager (self->account_switcher, self->session_manager);
          g_signal_connect (self->account_switcher, "add-account-requested",
                            G_CALLBACK (gnalk_window_on_add_account_requested), self);
          g_signal_connect (self->account_switcher, "sign-out-requested",
                            G_CALLBACK (gnalk_window_on_sign_out_requested), self);
        }
    }

  /* Connect to AppState for centralized UI state (new architecture).
   * This enables the window to react to loading status, sync progress, etc. */
  GnalkAppState *app_state = gnalk_application_get_app_state (GNALK_APPLICATION (app));
  if (app_state)
    {
      /* Show/hide loading indicator based on app state */
      g_signal_connect_object (app_state, "loading-changed",
                               G_CALLBACK (on_app_state_loading_changed), self, G_CONNECT_DEFAULT);
      /* Query initial state - signal only fires on changes */
      on_app_state_loading_changed (app_state, gnalk_app_state_is_loading (app_state), self);
    }

  /* Get http_client from application (shared for service mode support).
   * This ensures credentials are preserved when window is closed/reopened. */
  self->http_client = gnalk_application_get_http_client (GNALK_APPLICATION (app));
  /* Initial avatar cache with account_id=0; will be recreated in switch_to_session */
  self->avatar_cache = gnalk_avatar_cache_new (self->http_client, 0);
  /* Signaling comes from session via switch_to_session - don't create here */
  self->webrtc = gnalk_webrtc_new ();

  /* Get push_client from application (shared for service mode support) */
  self->push_client = gnalk_application_get_push_client (GNALK_APPLICATION (app));

  /* Connect to background sync for message refresh notifications.
   * Use g_signal_connect_object so the handler is automatically disconnected
   * when the window is destroyed (background sync outlives the window).
   * NOTE: bg_sync may be NULL here if async initialization hasn't completed yet.
   * In that case, the connection will be made in on_app_state_loading_changed. */
  GnalkBackgroundSync *bg_sync = gnalk_application_get_background_sync (GNALK_APPLICATION (app));
  if (bg_sync && !self->bg_sync_connected)
    {
      g_signal_connect_object (bg_sync, "messages-synced",
                               G_CALLBACK (on_app_background_sync_messages_synced), self,
                               G_CONNECT_DEFAULT);
      self->bg_sync_connected = TRUE;
    }

  /* Track window focus for notification decisions */
  g_signal_connect (self, "notify::is-active",
                    G_CALLBACK (on_window_is_active_changed), NULL);

  /* Set initial focus state */
  gnalk_application_set_window_focused (GNALK_APPLICATION (app),
                                          gtk_window_is_active (GTK_WINDOW (self)));

  /* Create message controller (will be recreated in switch_to_session with correct account) */
  self->message_controller = gnalk_message_controller_new (self->http_client);

  /* Set up database integration for MessageController - account_id=0 until session switch */
  {
    GnalkDatabase *db = gnalk_application_get_database (GNALK_APPLICATION (app));
    const char *user_id = gnalk_application_get_user_id (GNALK_APPLICATION (app));
    gnalk_message_controller_set_database (self->message_controller, db, 0);
    gnalk_message_controller_set_current_user (self->message_controller, user_id);
  }

  g_signal_connect (self->message_controller, "message-sent",
                    G_CALLBACK (gnalk_window_on_message_controller_sent), self);
  g_signal_connect (self->message_controller, "message-send-failed",
                    G_CALLBACK (gnalk_window_on_message_controller_send_failed), self);
  g_signal_connect (self->message_controller, "message-edited",
                    G_CALLBACK (gnalk_window_on_message_controller_edited), self);
  g_signal_connect (self->message_controller, "message-deleted",
                    G_CALLBACK (gnalk_window_on_message_controller_deleted), self);
  g_signal_connect (self->message_controller, "reaction-added",
                    G_CALLBACK (gnalk_window_on_message_controller_reaction_added), self);
  g_signal_connect (self->message_controller, "upload-completed",
                    G_CALLBACK (gnalk_window_on_message_controller_upload_completed), self);
  g_signal_connect (self->message_controller, "upload-failed",
                    G_CALLBACK (gnalk_window_on_message_controller_upload_failed), self);
  g_signal_connect (self->message_controller, "messages-loaded",
                    G_CALLBACK (gnalk_window_on_message_controller_messages_loaded), self);
  g_signal_connect (self->message_controller, "poll-response",
                    G_CALLBACK (gnalk_window_on_message_controller_poll_response), self);
  g_signal_connect (self->message_controller, "older-messages-loaded",
                    G_CALLBACK (gnalk_window_on_message_controller_older_messages_loaded), self);

  /* Create conversation controller (will be recreated in switch_to_session with correct account) */
  {
    GnalkDatabase *db = gnalk_application_get_database (GNALK_APPLICATION (app));
    self->conversation_controller = gnalk_conversation_controller_new (db, self->http_client);
    gnalk_conversation_controller_set_account_id (self->conversation_controller, 0);
    g_signal_connect (self->conversation_controller, "conversations-updated",
                      G_CALLBACK (gnalk_window_on_conversation_controller_updated), self);
    g_signal_connect (self->conversation_controller, "conversation-created",
                      G_CALLBACK (gnalk_window_on_conversation_controller_created), self);
    g_signal_connect (self->conversation_controller, "conversation-left",
                      G_CALLBACK (gnalk_window_on_conversation_controller_left), self);
    g_signal_connect (self->conversation_controller, "conversation-deleted",
                      G_CALLBACK (gnalk_window_on_conversation_controller_deleted), self);
    g_signal_connect (self->conversation_controller, "new-messages-detected",
                      G_CALLBACK (gnalk_window_on_conversation_controller_new_messages), self);
  }

  /* Create participant controller (will be recreated in switch_to_session with correct account) */
  {
    GnalkDatabase *db = gnalk_application_get_database (GNALK_APPLICATION (app));
    self->participant_controller = gnalk_participant_controller_new (db, self->http_client);
    gnalk_participant_controller_set_account_id (self->participant_controller, 0);
    g_signal_connect (self->participant_controller, "participants-changed",
                      G_CALLBACK (gnalk_window_on_participant_controller_changed), self);
  }

  self->conversations_store = g_list_store_new (GNALK_TYPE_CONVERSATION_DATA);
  self->messages_store = g_list_store_new (GNALK_TYPE_MESSAGE_DATA);
  self->search_results_store = g_list_store_new (GNALK_TYPE_CONVERSATION_DATA);

  /* Create room tab manager for per-room message storage */
  {
    GSettings *settings = gnalk_application_get_settings (GNALK_APPLICATION (app));
    gint max_tabs = g_settings_get_int (settings, "max-loaded-rooms");
    gdouble height_mult = g_settings_get_double (settings, "background-tab-height-multiplier");
    self->tab_manager = gnalk_room_tab_manager_new ((guint) max_tabs, height_mult);
  }
  self->bound_unread_rows = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->bound_conversation_rows = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->bound_message_rows = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->visible_unread_ids = g_array_new (FALSE, FALSE, sizeof (gint64));

  /* Conversation list - uses data/widget separation pattern */
  GtkNoSelection *conv_selection = gtk_no_selection_new (G_LIST_MODEL (self->conversations_store));
  GtkListItemFactory *conv_factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (conv_factory, "setup", G_CALLBACK (gnalk_window_on_conv_list_item_setup), self);
  g_signal_connect (conv_factory, "bind", G_CALLBACK (gnalk_window_on_conv_list_item_bind), self);
  g_signal_connect (conv_factory, "unbind", G_CALLBACK (gnalk_window_on_conv_list_item_unbind), self);

  gnalk_conversation_sidebar_set_conversation_model (self->conversation_sidebar,
                                                       GTK_SELECTION_MODEL (conv_selection),
                                                       conv_factory);

  /* Set up search results list - uses data/widget separation like messages */
  self->search_selection = gtk_no_selection_new (G_LIST_MODEL (g_object_ref (self->search_results_store)));
  GtkListItemFactory *search_factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (search_factory, "setup", G_CALLBACK (gnalk_window_on_search_list_item_setup), self);
  g_signal_connect (search_factory, "bind", G_CALLBACK (gnalk_window_on_search_list_item_bind), self);
  g_signal_connect (search_factory, "unbind", G_CALLBACK (gnalk_window_on_search_list_item_unbind), self);

  gnalk_conversation_sidebar_set_search_model (self->conversation_sidebar,
                                                 GTK_SELECTION_MODEL (self->search_selection),
                                                 search_factory);

  /* Message list factory - uses proper data/widget separation */
  self->messages_selection = gtk_no_selection_new (G_LIST_MODEL (g_object_ref (self->messages_store)));
  GtkListItemFactory *msg_factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (msg_factory, "setup", G_CALLBACK (on_msg_list_item_setup), self);
  g_signal_connect (msg_factory, "bind", G_CALLBACK (on_msg_list_item_bind), self);
  g_signal_connect (msg_factory, "unbind", G_CALLBACK (on_msg_list_item_unbind), self);

  gnalk_message_view_set_model (self->message_view, GTK_SELECTION_MODEL (self->messages_selection), msg_factory);

  g_signal_connect (self->conversation_sidebar, "conversation-selected", G_CALLBACK (gnalk_window_on_sidebar_conversation_selected), self);
  g_signal_connect (self->conversation_sidebar, "search-result-selected", G_CALLBACK (gnalk_window_on_sidebar_search_result_selected), self);
  g_signal_connect (self->conversation_sidebar, "search-changed", G_CALLBACK (gnalk_window_on_sidebar_search_changed), self);
  g_signal_connect (self->message_input, "send-requested", G_CALLBACK (gnalk_window_on_message_input_send_requested), self);
  g_signal_connect (self->message_input, "text-changed", G_CALLBACK (on_message_input_text_changed), self);
  g_signal_connect (self->message_input, "attach-clicked", G_CALLBACK (on_attach_clicked), self);
  g_signal_connect (self->message_input, "voice-message-recorded", G_CALLBACK (on_voice_message_recorded), self);
  g_signal_connect (self->message_input, "camera-clicked", G_CALLBACK (on_camera_clicked), self);
  g_signal_connect (self->conversation_header, "info-requested", G_CALLBACK (on_conversation_info_requested), self);
  g_signal_connect (self->video_call_button, "clicked", G_CALLBACK (on_video_call_clicked), self);
  g_signal_connect (self->audio_call_button, "clicked", G_CALLBACK (on_audio_call_clicked), self);
  g_signal_connect (self->participants_toggle_button, "toggled", G_CALLBACK (gnalk_window_on_participants_toggled), self);
  g_signal_connect (self->participants_list, "chat-requested", G_CALLBACK (on_participant_chat_requested), self);
  g_signal_connect (self->participants_list, "promote-requested", G_CALLBACK (on_participant_promote_requested), self);
  g_signal_connect (self->participants_list, "demote-requested", G_CALLBACK (on_participant_demote_requested), self);
  g_signal_connect (self->participants_list, "remove-requested", G_CALLBACK (on_participant_remove_requested), self);
  g_signal_connect (self->conversation_actions, "quote-closed", G_CALLBACK (on_quote_closed), self);
  g_signal_connect (self->conversation_actions, "attachment-closed", G_CALLBACK (on_attachment_closed), self);
  g_signal_connect (self->conversation_actions, "scroll-to-message-requested", G_CALLBACK (on_quote_preview_scroll_requested), self);
  g_signal_connect (self->user_status_menu, "status-changed", G_CALLBACK (on_user_status_changed), self);
  /* Signaling handlers are connected in switch_to_session after getting signaling from session */
  g_signal_connect (self->incoming_call_banner, "accept-call-requested", G_CALLBACK (on_incoming_call_accept_requested), self);
  g_signal_connect (self->incoming_call_banner, "refuse-call-requested", G_CALLBACK (on_incoming_call_refuse_requested), self);

  /* Clear messages when navigating back in compact mode (before view is hidden) */
  g_signal_connect (self->content_page, "hiding", G_CALLBACK (gnalk_window_on_content_page_hiding), self);

  /* Connect message view signals for lazy loading */
  g_signal_connect (self->message_view, "load-older-requested", G_CALLBACK (on_message_view_load_older_requested), self);

  /* Connect message view signal for scroll-to-bottom button */
  g_signal_connect (self->message_view, "scroll-to-bottom-clicked", G_CALLBACK (gnalk_window_on_scroll_to_bottom_clicked_for_unread), self);

  /* Connect scroll-changed to re-check visibility of unread messages on scroll */
  g_signal_connect (self->message_view, "scroll-changed", G_CALLBACK (gnalk_window_on_message_scroll_changed), self);

  /* Set up breakpoint for responsive message display (wide vs narrow layout) */
  AdwBreakpoint *wide_breakpoint = adw_breakpoint_new (
    adw_breakpoint_condition_parse ("min-width: 1100px"));
  g_signal_connect (wide_breakpoint, "apply", G_CALLBACK (on_wide_breakpoint_applied), self);
  g_signal_connect (wide_breakpoint, "unapply", G_CALLBACK (on_wide_breakpoint_unapplied), self);
  adw_application_window_add_breakpoint (ADW_APPLICATION_WINDOW (self), wide_breakpoint);

  /* Connect login view signal */
  g_signal_connect (self->login_view, "connect-clicked", G_CALLBACK (gnalk_window_on_login_view_connect_clicked), self);

  /* Connect new conversation button from sidebar */
  g_signal_connect (self->conversation_sidebar, "new-conversation-requested", G_CALLBACK (on_new_conversation_clicked), self);

  /* UI template defaults to main view. Login view is shown explicitly only when
   * autologin check fails. This avoids the login view flash during startup. */
  g_idle_add (attempt_autologin_idle, self);
}

GnalkWindow *
gnalk_window_new (GtkApplication *app)
{
  return g_object_new (GNALK_TYPE_WINDOW,
                       "application", app,
                       NULL);
}

void
gnalk_window_open_room (GnalkWindow *self,
                          const char   *room_token)
{
  g_return_if_fail (GNALK_IS_WINDOW (self));
  g_return_if_fail (room_token != NULL);

  g_message ("Window: opening room %s via public API", room_token);
  gnalk_window_open_conversation_by_token (self, room_token);
}
