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
#include "gnalk-data-repository.h"
#include "gnalk-app-state.h"
#include "gnalk-sync-coordinator.h"
#include "gnalk-session.h"
#include "gnalk-session-manager.h"
#include "gnalk-login-view.h"
#include "gnalk-conversation-sidebar.h"
#include "gnalk-message-view.h"
#include "gnalk-message-input.h"
#include "gnalk-conversation-header.h"
#include "gnalk-conversation-actions.h"
#include "gnalk-user-status-menu.h"
#include "gnalk-typing-indicator.h"
#include "gnalk-incoming-call-banner.h"
#include "gnalk-participants-list.h"
#include "gnalk-network-status-bar.h"
#include "gnalk-message-search.h"
#include "gnalk-account-switcher.h"
#include "gnalk-call-window.h"
#include "gnalk-room-tab-manager.h"
#include "gnalk-signaling.h"
#include "gnalk-avatar-cache.h"
#include "gnalk-message-controller.h"
#include "gnalk-conversation-controller.h"
#include "gnalk-participant-controller.h"
#include "gnalk-room-controller.h"
#include "gnalk-message-processor.h"
#include "gnalk-sync-manager.h"
#include "gnalk-participant-profile-dialog.h"
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

/* AppState signal handlers (new async startup architecture) */
static void on_app_state_loading_changed (GnalkAppState *app_state, gboolean is_loading, GnalkWindow *self);

static void on_call_window_destroy (GtkWidget *widget, GnalkWindow *self);
static void on_new_conversation_clicked (GnalkConversationSidebar *sidebar, GnalkWindow *self);
static void on_new_conversation_created (GnalkNewConversationDialog *dialog, const char *room_token, GnalkWindow *self);
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
      g_value_set_string (value, self->current_room->room_name);
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
      g_free (self->current_room->room_name);
      self->current_room->room_name = g_value_dup_string (value);
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

  /* Sync coordinator connection is now handled in switch_to_session when
   * attaching to a session. Each session has its own sync coordinator. */
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

        /* Update cached capabilities (now available after capabilities fetch) */
        self->cached_has_threads = gnalk_session_has_threads (session);

        /* Force rebind of visible message rows to pick up thread capability */
        if (self->cached_has_threads && self->messages_store &&
            g_list_model_get_n_items (G_LIST_MODEL (self->messages_store)) > 0)
          {
            gtk_no_selection_set_model (self->messages_selection, NULL);
            gtk_no_selection_set_model (self->messages_selection, G_LIST_MODEL (self->messages_store));
          }

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

            /* Get password from keyring */
            g_autofree char *password = NULL;
            if (server_url && user_id)
              password = gnalk_keyring_lookup_password_sync (server_url, user_id, NULL);

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

        /* Note: SyncCoordinator handles periodic conversation sync at app level.
         * No need to start polling here - it's managed centrally. */

        /* Note: Don't reload messages here - they're already loaded from cache at startup.
         * Long-polling will fetch any new messages incrementally. */

        /* Refresh participants now that we're online (may have been empty from offline start) */
        if (self->current_room && self->participant_controller)
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
        GnalkDataRepository *repo = gnalk_application_get_data_repository (app);
        gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;
        if (repo && account_id > 0)
          gnalk_data_repository_reset_participants_status (repo, account_id);
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
gnalk_window_on_session_room_joined (GnalkSession *session G_GNUC_UNUSED,
                                      const char    *room_token,
                                      const char    *session_id,
                                      GnalkWindow  *self)
{
  g_message ("Window: session room joined: %s (session_id: %s)", room_token, session_id);

  if (!self->current_room)
    return;

  g_free (self->current_room->room_token);
  g_free (self->current_room->session_id);
  self->current_room->room_token = g_strdup (room_token);
  self->current_room->session_id = g_strdup (session_id);
  self->has_active_room = TRUE;

  g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_HAS_ACTIVE_ROOM]);

  /* Notify application of active room for notification decisions */
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  if (app)
    gnalk_application_set_active_room (app, room_token);

  /* Session cookies are now established - allow HTTP polling */
  if (self->message_controller)
    gnalk_message_controller_set_session_ready (self->message_controller, TRUE);
  if (self->participant_controller)
    gnalk_participant_controller_set_session_ready (self->participant_controller, TRUE);

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

  /* Don't show incoming call if we're already in a call */
  if (self->active_call_window)
    return;

  /* Look up room name from conversation list */
  const char *room_name = NULL;
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
  for (guint i = 0; i < n; i++)
    {
      GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
      if (g_strcmp0 (gnalk_conversation_data_get_token (conv), room_token) == 0)
        {
          room_name = gnalk_conversation_data_get_title (conv);
          g_object_unref (conv);
          break;
        }
      g_object_unref (conv);
    }

  gnalk_incoming_call_banner_show_call (self->incoming_call_banner,
                                         room_token, room_name, caller_id,
                                         self->avatar_cache);
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
  if (g_strcmp0 (room_token, self->current_room->room_token) != 0)
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

  /* Join the current room if we have one (e.g., from immediate restore at startup) */
  if (self->current_room && self->has_active_room && self->active_session)
    {
      gnalk_session_join_room (self->active_session, self->current_room->room_token);
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

/* ============ Federation Invitation Button Handlers ============ */

static void
on_invitation_accept_clicked (GnalkWindow *self)
{
  if (self->pending_invitation_id == 0 || !self->conversation_controller)
    return;

  gtk_widget_set_sensitive (self->invitation_accept_button, FALSE);
  gtk_widget_set_sensitive (self->invitation_decline_button, FALSE);

  gnalk_conversation_controller_accept_invitation (self->conversation_controller,
                                                    self->pending_invitation_id);
}

static void
on_invitation_decline_clicked (GnalkWindow *self)
{
  if (self->pending_invitation_id == 0 || !self->conversation_controller)
    return;

  gtk_widget_set_sensitive (self->invitation_accept_button, FALSE);
  gtk_widget_set_sensitive (self->invitation_decline_button, FALSE);

  gnalk_conversation_controller_decline_invitation (self->conversation_controller,
                                                     self->pending_invitation_id);
}

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

      /* For federated rooms, calls require federation-v2 capability */
      if (can_call && self->current_room_conv_data &&
          gnalk_conversation_data_is_federated (self->current_room_conv_data) &&
          !gnalk_session_has_federation_v2 (self->active_session))
        {
          can_call = FALSE;
        }
    }
  else
    {
      /* No session - can't call */
      can_call = FALSE;
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

/* Callback for header avatar re-fetch (used when avatar cache was invalidated) */
static void
on_header_avatar_refetched (GnalkAvatarCache *cache G_GNUC_UNUSED,
                             GdkTexture        *texture,
                             gpointer           user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  gnalk_conversation_header_set_avatar (self->conversation_header, texture);
}

/* Handler for ConversationData "updated" signal — keeps chat header and
 * participants list in sync with the conversation list's user status. */
static void
on_current_room_conv_data_updated (GnalkConversationData *conv_data,
                                   GnalkWindow           *self)
{
  if (!self->current_room)
    return;

  const char *user_status = gnalk_conversation_data_get_user_status (conv_data);
  gint room_type = self->current_room->room_type;

  /* Update header status icon (only meaningful for 1:1 rooms) */
  gnalk_window_update_room_status_icon (self, room_type, user_status);

  /* Update header title and nav page title */
  const char *title = gnalk_conversation_data_get_title (conv_data);
  gnalk_conversation_header_set_title (self->conversation_header, title);
  adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self->content_page),
      title ? title : "Messages");

  /* Re-fetch room avatar (cheap cache-hit in common case, actual fetch after invalidation) */
  if (self->avatar_cache && room_type != 1)
    {
      const char *token = gnalk_conversation_data_get_token (conv_data);
      if (token)
        gnalk_avatar_cache_get_room_avatar (self->avatar_cache, token,
                                               on_header_avatar_refetched, self);
    }

  /* For 1:1 rooms, also update the participant's status in the list.
   * In the conversations API, "name" is the other user's ID for 1:1 rooms. */
  if (room_type == 1)
    {
      const char *other_user_id = gnalk_conversation_data_get_name (conv_data);
      if (other_user_id && *other_user_id)
        gnalk_participants_list_update_participant_status (self->participants_list, other_user_id, user_status);
    }
}

void
gnalk_window_track_conv_data_for_status (GnalkWindow           *self,
                                          GnalkConversationData *conv_data)
{
  /* Disconnect previous handler */
  if (self->current_room_conv_data && self->conv_data_updated_handler_id > 0)
    {
      g_signal_handler_disconnect (self->current_room_conv_data,
                                   self->conv_data_updated_handler_id);
    }
  self->current_room_conv_data = NULL;
  self->conv_data_updated_handler_id = 0;

  if (!conv_data)
    return;

  /* Connect to the new ConversationData's "updated" signal */
  self->current_room_conv_data = conv_data;
  self->conv_data_updated_handler_id =
    g_signal_connect (conv_data, "updated",
                      G_CALLBACK (on_current_room_conv_data_updated), self);
}

void
gnalk_window_start_call (GnalkWindow *self,
                         gboolean      with_video)
{
  if (!self->current_room || !self->active_session)
    return;

  /* Check if this is a federated conversation */
  gboolean is_federated = self->current_room_conv_data &&
    gnalk_conversation_data_is_federated (self->current_room_conv_data);

  /* For federated calls, use the federation signaling connection */
  GnalkSignaling *signaling;
  if (is_federated)
    {
      signaling = gnalk_session_get_fed_signaling (self->active_session);
      if (!signaling || !gnalk_signaling_is_connected (signaling))
        {
          g_warning ("Cannot start federated call: federation signaling not connected");
          return;
        }
    }
  else
    {
      signaling = gnalk_session_get_signaling (self->active_session);
      if (!signaling || !gnalk_signaling_is_connected (signaling))
        {
          g_warning ("Cannot start call: signaling not connected");
          return;
        }
    }

  if (!self->current_room->session_id)
    {
      g_warning ("Cannot start call: no session ID");
      return;
    }

  g_message ("start_call: starting %s%s call for room %s with session %.30s...",
             is_federated ? "federated " : "",
             with_video ? "video" : "audio",
             self->current_room->room_token, self->current_room->session_id);

  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
  GnalkCallWindow *call_window = gnalk_call_window_new (app);

  gnalk_call_window_start_call (call_window,
                                 self->current_room->room_token,
                                 self->current_room->room_name,
                                 self->current_room->session_id,
                                 self->http_client,
                                 self->webrtc,
                                 signaling,
                                 self->avatar_cache,
                                 with_video,
                                 is_federated);

  /* Track active call window */
  self->active_call_window = call_window;
  g_signal_connect (call_window, "destroy", G_CALLBACK (on_call_window_destroy), self);

  gtk_window_present (GTK_WINDOW (call_window));
}


static void
on_conversation_info_requested (GnalkConversationHeader *header G_GNUC_UNUSED,
                                 GnalkWindow             *self)
{
  if (!self->current_room)
    return;

  /* Find the conversation data for the current room */
  GnalkConversationData *conversation = NULL;
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
  for (guint i = 0; i < n_items; i++)
    {
      GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
      const char *token = gnalk_conversation_data_get_token (conv);
      if (g_strcmp0 (token, self->current_room->room_token) == 0)
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
  const char *my_user_id = NULL;
  if (self->active_session)
    {
      GnalkAccount *account = gnalk_session_get_account (self->active_session);
      my_user_id = gnalk_account_get_user_id (account);
    }
  gnalk_conversation_info_dialog_set_my_user_id (dialog, my_user_id);

  g_signal_connect (dialog, "media-clicked",
                    G_CALLBACK (gnalk_window_on_info_dialog_media_clicked), self);

  /* Connect participant action signals - use same handlers as participants list
     since they don't use the first parameter */
  g_signal_connect (dialog, "chat-requested",
                    G_CALLBACK (gnalk_window_on_participant_chat_requested), self);
  g_signal_connect (dialog, "promote-requested",
                    G_CALLBACK (gnalk_window_on_participant_promote_requested), self);
  g_signal_connect (dialog, "demote-requested",
                    G_CALLBACK (gnalk_window_on_participant_demote_requested), self);
  g_signal_connect (dialog, "remove-requested",
                    G_CALLBACK (gnalk_window_on_participant_remove_requested), self);

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
  gnalk_window_start_call (self, TRUE);
}

static void
on_audio_call_clicked (GtkButton    *button,
                       GnalkWindow *self)
{
  gnalk_window_start_call (self, FALSE);
}

static void
on_participant_clicked (GnalkParticipantsList *list G_GNUC_UNUSED,
                         const char            *actor_id,
                         JsonObject            *participant_json,
                         GnalkWindow           *self)
{
  const char *actor_type = json_object_get_string_member_with_default (participant_json, "actorType", "users");
  const char *display_name = json_object_get_string_member_with_default (participant_json, "displayName", "");
  gint participant_type = (gint) json_object_get_int_member_with_default (participant_json, "participantType", 3);
  const char *status = json_object_get_string_member_with_default (participant_json, "status", "");
  const char *status_icon = json_object_get_string_member_with_default (participant_json, "statusIcon", "");
  const char *status_message = json_object_get_string_member_with_default (participant_json, "statusMessage", "");

  GnalkParticipantProfileDialog *dialog = gnalk_participant_profile_dialog_new ();
  gnalk_participant_profile_dialog_set_participant (dialog, actor_id, actor_type, display_name,
                                                     participant_type, status, status_icon,
                                                     status_message, self->avatar_cache,
                                                     self->current_room ? self->current_room->room_token : NULL);
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

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
      gnalk_message_row_set_one_to_one (row, self->current_room->room_type == 1);
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

  /* Connect signals — g_signal_connect_object auto-disconnects when self is destroyed */
  g_signal_connect_object (row, "reaction-requested", G_CALLBACK (gnalk_window_on_reaction_requested), self, 0);
  g_signal_connect_object (row, "quote-requested", G_CALLBACK (gnalk_window_on_quote_requested), self, 0);
  g_signal_connect_object (row, "edit-requested", G_CALLBACK (gnalk_window_on_edit_requested), self, 0);
  g_signal_connect_object (row, "delete-requested", G_CALLBACK (gnalk_window_on_delete_requested), self, 0);
  g_signal_connect_object (row, "scroll-to-message-requested", G_CALLBACK (gnalk_window_on_scroll_to_message_requested), self, 0);
  g_signal_connect_object (row, "file-clicked", G_CALLBACK (gnalk_window_on_file_clicked), self, 0);
  g_signal_connect_object (row, "thread-open-requested", G_CALLBACK (gnalk_window_on_thread_open_requested), self, 0);
  g_signal_connect_object (row, "thread-create-requested", G_CALLBACK (gnalk_window_on_thread_create_requested), self, 0);

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

  /* Set edit/delete timeouts and thread support from cached capabilities */
  gnalk_message_row_set_edit_timeout (row, self->cached_edit_timeout);
  gnalk_message_row_set_delete_timeout (row, self->cached_delete_timeout);
  gnalk_message_row_set_threads_supported (row, self->cached_has_threads && !self->cached_room_is_federated);

  /* Populate the row from the data object first (sets is_self, is_system, etc.) */
  /* Note: gtk_list_item_get_item returns transfer-none, don't unref */
  gnalk_message_row_set_from_data (row, data, self->avatar_cache,
                                     self->current_room ? self->current_room->room_token : NULL);

  /* Use pre-computed display flags from MessageProcessor */
  gboolean show_avatar = gnalk_message_data_get_show_avatar (data);
  gboolean show_timestamp = gnalk_message_data_get_show_timestamp (data);

  /* Set all state before display mode (which calls apply_layout) */
  gnalk_message_row_set_show_avatar (row, show_avatar);
  gnalk_message_row_set_show_timestamp (row, show_timestamp);
  gnalk_message_row_set_one_to_one (row, self->current_room->room_type == 1);

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

  /* Check if file is already cached — read from data first, syscall only if needed */
  const char *file_id = gnalk_message_row_get_file_id (row);
  if (file_id && *file_id)
    {
      gboolean cached = gnalk_message_data_get_file_cached (data);
      if (!cached)
        {
          const char *file_name = gnalk_message_row_get_file_name (row);
          g_autofree char *cache_dir = g_build_filename (g_get_user_cache_dir (), "gnalk", NULL);
          const char *name = (file_name && *file_name) ? file_name : "download";
          g_autofree char *safe_name = g_strdup_printf ("%s_%s", file_id, name);
          g_autofree char *cache_path = g_build_filename (cache_dir, safe_name, NULL);

          cached = g_file_test (cache_path, G_FILE_TEST_EXISTS);
          if (cached)
            gnalk_message_data_set_file_cached (data, TRUE);
        }
      gnalk_message_row_set_file_cached (row, cached);
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

static void
on_message_view_load_older_requested (GnalkMessageView *view,
                                       gpointer           user_data)
{
  (void) view;
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (self->current_room &&
      self->current_room->has_more_history && !gnalk_message_view_get_loading_older (self->message_view) &&
      self->current_room->oldest_loaded_id > 0)
    {
      if (self->current_room->in_thread_mode && self->message_controller)
        {
          g_debug ("Message view requested older thread messages");
          gnalk_message_controller_load_older_thread (self->message_controller,
              self->current_room->thread_root_id,
              self->current_room->oldest_loaded_id);
        }
      else
        {
          g_debug ("Message view requested older messages");
          gnalk_window_load_older_messages (self);
        }
    }
}

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

/* Handler for individual participant call state change (from ParticipantController) */
void
gnalk_window_on_participant_call_changed (GnalkParticipantController *controller G_GNUC_UNUSED,
                                            const char                  *room_token,
                                            const char                  *actor_id,
                                            gint64                       in_call,
                                            GnalkWindow                *self)
{
  /* Only update if this is the current room */
  if (!self->current_room || g_strcmp0 (room_token, self->current_room->room_token) != 0)
    return;

  gnalk_participants_list_update_call_state (self->participants_list, actor_id, in_call);

  /* For 1:1 rooms, update header call icon in real time for the other participant */
  if (self->current_room->room_type == 1)
    {
      GnalkAccount *account = self->active_session
        ? gnalk_session_get_account (self->active_session) : NULL;
      const char *my_user_id = account ? gnalk_account_get_user_id (account) : NULL;
      if (my_user_id && g_strcmp0 (actor_id, my_user_id) != 0)
        gnalk_window_update_room_call_icon (self, 1, in_call);
    }
}

/* Handler for all call states cleared (from ParticipantController) */
void
gnalk_window_on_all_call_states_cleared (GnalkParticipantController *controller G_GNUC_UNUSED,
                                            const char                  *room_token,
                                            GnalkWindow                *self)
{
  /* Only update if this is the current room */
  if (!self->current_room || g_strcmp0 (room_token, self->current_room->room_token) != 0)
    return;

  gnalk_participants_list_clear_all_call_states (self->participants_list);

  /* For 1:1 rooms, clear header call icon */
  if (self->current_room->room_type == 1)
    gnalk_window_update_room_call_icon (self, 1, 0);
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
on_user_status_changed (GnalkUserStatusMenu *menu,
                        const char           *status,
                        GnalkWindow         *self)
{
  (void) menu;
  if (self->active_session)
    gnalk_session_set_user_status (self->active_session, status);
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
gnalk_window_on_session_call_ended (GnalkSession *session G_GNUC_UNUSED,
                                    GnalkWindow  *self)
{
  /* Hide incoming call banner when call ends for all */
  hide_incoming_call (self);
}

void
gnalk_window_on_session_room_disinvited (GnalkSession *session G_GNUC_UNUSED,
                                         const char    *room_token,
                                         GnalkWindow  *self)
{
  g_message ("Window: received disinvite from room %s", room_token);

  /* If we were disinvited from the current room, clear our room state
   * to prevent trying to rejoin after reconnection */
  if (self->current_room && g_strcmp0 (self->current_room->room_token, room_token) == 0)
    {
      g_message ("Window: clearing current room state due to disinvite");
      self->current_room = NULL;
    }
}

static void
accept_incoming_call (GnalkWindow *self,
                      gboolean      with_video)
{
  if (!gnalk_incoming_call_banner_has_incoming_call (self->incoming_call_banner))
    return;

  const char *call_room_token = gnalk_incoming_call_banner_get_room_token (self->incoming_call_banner);

  /* Withdraw call desktop notification */
  {
    GnalkApplication *gnalk_app = GNALK_APPLICATION (g_application_get_default ());
    gnalk_notification_service_withdraw_call (
        gnalk_application_get_notification_service (gnalk_app), call_room_token);
    if (self->active_session)
      {
        GnalkSyncCoordinator *coord = gnalk_session_get_sync_coordinator (self->active_session);
        if (coord)
          gnalk_sync_coordinator_clear_active_call (coord, call_room_token);
      }
  }

  /* Hide the banner */
  g_autofree char *saved_token = g_strdup (call_room_token);
  hide_incoming_call (self);

  /* If already in the call's room, start immediately */
  if (self->current_room &&
      g_strcmp0 (self->current_room->room_token, saved_token) == 0)
    {
      gnalk_window_start_call (self, with_video);
      return;
    }

  /* Otherwise navigate to that room; pending_call_with_video will trigger start_call
   * once room_opened fires */
  self->pending_call_with_video = with_video ? 1 : 0;
  gnalk_window_open_room (self, saved_token);
}

static void
on_incoming_call_accept_audio (GnalkIncomingCallBanner *banner,
                               GnalkWindow             *self)
{
  (void) banner;
  accept_incoming_call (self, FALSE);
}

static void
on_incoming_call_accept_video (GnalkIncomingCallBanner *banner,
                               GnalkWindow             *self)
{
  (void) banner;
  accept_incoming_call (self, TRUE);
}

static void
on_incoming_call_refuse_requested (GnalkIncomingCallBanner *banner,
                                    GnalkWindow             *self)
{
  (void) banner;

  /* Withdraw call desktop notification */
  {
    const char *call_room = gnalk_incoming_call_banner_get_room_token (self->incoming_call_banner);
    GnalkApplication *gnalk_app = GNALK_APPLICATION (g_application_get_default ());
    gnalk_notification_service_withdraw_call (
        gnalk_application_get_notification_service (gnalk_app), call_room);
    if (self->active_session)
      {
        GnalkSyncCoordinator *coord = gnalk_session_get_sync_coordinator (self->active_session);
        if (coord)
          gnalk_sync_coordinator_clear_active_call (coord, call_room);
      }
  }

  /* Just hide the banner - we don't need to do anything else
   * since we haven't actually joined the call yet */
  hide_incoming_call (self);
}

/* Typing indicator functions */

#define TYPING_RESEND_TIMEOUT_SEC 10

static gboolean
on_typing_send_timer (gpointer user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  GnalkSignaling *signaling = self->active_session
    ? gnalk_session_get_signaling (self->active_session) : NULL;

  /* Check if still typing (text not empty) */
  const char *text = gnalk_message_input_get_text (self->message_input);
  if (text && *text)
    {
      /* Resend typing message */
      if (signaling)
        gnalk_signaling_send_typing (signaling, TRUE);
      return G_SOURCE_CONTINUE;
    }
  else
    {
      /* Stop typing */
      self->current_room->is_typing = FALSE;
      self->typing_send_timer_id = 0;
      if (signaling)
        gnalk_signaling_send_typing (signaling, FALSE);
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

  GnalkSignaling *signaling = self->active_session
    ? gnalk_session_get_signaling (self->active_session) : NULL;

  if (has_text && !self->current_room->is_typing)
    {
      /* Started typing */
      self->current_room->is_typing = TRUE;
      if (signaling)
        gnalk_signaling_send_typing (signaling, TRUE);

      /* Start timer to resend every 10 seconds */
      if (self->typing_send_timer_id == 0)
        self->typing_send_timer_id = g_timeout_add_seconds (TYPING_RESEND_TIMEOUT_SEC,
                                                             on_typing_send_timer,
                                                             self);
    }
  else if (!has_text && self->current_room->is_typing)
    {
      /* Stopped typing */
      self->current_room->is_typing = FALSE;
      if (signaling)
        gnalk_signaling_send_typing (signaling, FALSE);

      if (self->typing_send_timer_id > 0)
        {
          g_source_remove (self->typing_send_timer_id);
          self->typing_send_timer_id = 0;
        }
    }
}

/* Participant controller changed handler and participant action handlers
 * have been moved to gnalk-window-message-actions.c */

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

          /* Save confirmed message to database */
          GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
          if (app)
            {
              GnalkDataRepository *repo = gnalk_application_get_data_repository (app);
              gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;
              if (repo && account_id > 0 && self->current_room)
                gnalk_data_repository_save_message (repo, account_id, self->current_room->room_token, msg_data);
            }

          /* UI update happens via notify from confirm_sent (status change) */
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

          /* Change message text — UI update happens via notify::message */
          gnalk_message_data_set_message (msg, _("Message deleted by you"));
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
          /* UI update happens via notify::reactions */
          gnalk_message_data_add_reaction (msg, emoji);

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
          /* UI update happens via notify (is-uploading + status) */
          gnalk_message_data_set_is_uploading (msg, FALSE);
          gnalk_message_data_set_status (msg, GNALK_MESSAGE_DATA_STATUS_SENT);

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
          /* UI update happens via notify (is-uploading + status) */
          gnalk_message_data_set_is_uploading (msg, FALSE);
          gnalk_message_data_set_status (msg, GNALK_MESSAGE_DATA_STATUS_FAILED);

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
on_new_conversation_created (GnalkNewConversationDialog *dialog G_GNUC_UNUSED,
                             const char                  *room_token,
                             GnalkWindow                *self)
{
  /* DataRepository::data-changed handles the reload when sync saves to DB.
   * Open the new conversation immediately. */
  gnalk_window_open_conversation_by_token (self, room_token);
}

/* Login functions (start_login, start_guest_login, show_login_view, attempt_autologin,
 * show_offline_main_view, on_login_view_connect_clicked, login_set_connecting, login_show_error)
 * have been moved to gnalk-window-login.c */

/* Message row action handlers, scroll-to-message highlight helpers,
 * and participant action handlers have been moved to
 * gnalk-window-message-actions.c */

static void
gnalk_window_dispose (GObject *object)
{
  GnalkWindow *self = GNALK_WINDOW (object);

  /* Leave the current room when window is closed.
   * This ensures push notifications work instead of signaling messages. */
  if (self->current_room)
    {
      g_message ("Window dispose: leaving room %s", self->current_room->room_token);

      /* Tell controllers we're leaving the room */
      if (self->message_controller)
        gnalk_message_controller_set_room (self->message_controller, NULL);
      if (self->participant_controller)
        gnalk_participant_controller_set_active_room (self->participant_controller, NULL, 0);

      if (self->active_session)
        {
          GnalkSignaling *signaling = gnalk_session_get_signaling (self->active_session);
          if (signaling)
            gnalk_signaling_leave_room (signaling, self->current_room->room_token);
        }

      if (self->http_client)
        gnalk_http_client_leave_room (self->http_client, self->current_room->room_token, NULL, NULL);

      /* Clear active room in sync coordinator so notifications work */
      GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
      if (app)
        gnalk_application_set_active_room (app, NULL);
    }

  /* Disconnect from ConversationData status tracking */
  gnalk_window_track_conv_data_for_status (self, NULL);

  /* Disconnect from session and controller signals BEFORE widgets are destroyed.
   * This prevents signal callbacks from accessing destroyed widgets. */
  gnalk_window_disconnect_active_session (self);

  /* Disconnect from session_manager signals */
  if (self->session_manager)
    {
      g_signal_handlers_disconnect_by_data (self->session_manager, self);
      self->session_manager = NULL;
    }

  /* Disconnect from DataRepository signals (it survives window close) */
  {
    GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
    if (app)
      {
        GnalkDataRepository *repo = gnalk_application_get_data_repository (app);
        if (repo)
          g_signal_handlers_disconnect_by_data (repo, self);
      }
  }

  G_OBJECT_CLASS (gnalk_window_parent_class)->dispose (object);
}

static void
gnalk_window_finalize (GObject *object)
{
  GnalkWindow *self = GNALK_WINDOW (object);

  /* MC/CC/PC are cached from session - not owned, just clear pointers */
  self->message_controller = NULL;
  self->conversation_controller = NULL;
  self->participant_controller = NULL;

  if (self->typing_send_timer_id > 0)
    {
      g_source_remove (self->typing_send_timer_id);
      self->typing_send_timer_id = 0;
    }

  g_clear_object (&self->room_controller);
  g_clear_object (&self->message_processor);

  if (self->conv_reload_debounce_id > 0)
    {
      g_source_remove (self->conv_reload_debounce_id);
      self->conv_reload_debounce_id = 0;
    }

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

  /* sync_manager is borrowed from session.
   * http_client and push_client are borrowed from application. Just clear references. */
  self->http_client = NULL;
  self->push_client = NULL;
  self->sync_manager = NULL;

  /* These are always owned by the window */
  g_clear_object (&self->avatar_cache);
  g_clear_object (&self->webrtc);
  g_clear_object (&self->conversations_sort_model);
  g_clear_object (&self->conversations_sorter);
  g_clear_object (&self->conversations_store);
  g_clear_object (&self->messages_store);
  g_clear_object (&self->search_results_store);
  g_clear_object (&self->tab_manager);

  self->current_room = NULL;
  g_free (self->user_name);
  g_free (self->login_room_token);

  /* Federation invitation cleanup */
  g_clear_pointer (&self->pending_invitations, g_hash_table_unref);

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

/* Room messages synced notification - relayed from SyncCoordinator via Session. */
void
gnalk_window_on_session_room_messages_synced (GnalkSession *session G_GNUC_UNUSED,
                                              const char    *room_token,
                                              guint          count,
                                              GnalkWindow  *self)
{
  g_message ("Session: room %s synced (%u messages)", room_token, count);

  /* Conversation list reload is handled by DataRepository::data-changed (debounced) */

  /* For the active room, long-polling handles message updates - no need to reload.
   * Only background rooms need their tabs refreshed when switched to. */
  if (self->current_room && g_strcmp0 (room_token, self->current_room->room_token) == 0)
    {
      g_debug ("Session: active room %s synced, polling handles updates", room_token);
      /* Don't reload - polling already handles incremental updates */
    }
  else if (self->tab_manager && gnalk_room_tab_manager_has_tab (self->tab_manager, room_token))
    {
      /* Room has a tab but is not the active room - the tab's store will be
       * refreshed from DB when the user switches to it. */
      g_debug ("Session: room %s has background tab, will refresh on switch", room_token);
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
  g_type_ensure (GNALK_TYPE_MESSAGE_SEARCH);

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
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, message_search);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, message_search_revealer);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, message_search_button);

  /* Federation invitation view */
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, content_stack);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, invitation_status_page);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, invitation_details_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, invitation_accept_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkWindow, invitation_decline_button);
}

/* ============ Message Search Handlers ============ */

static void
on_message_search_requested (GnalkMessageSearch *search G_GNUC_UNUSED,
                             const char         *search_term,
                             gint                cursor,
                             GnalkWindow        *self)
{
  if (self->message_controller)
    gnalk_message_controller_search_messages (self->message_controller, search_term, cursor);
}

static void
on_message_search_result_activated (GnalkMessageSearch *search G_GNUC_UNUSED,
                                    gint64              message_id,
                                    GnalkWindow        *self)
{
  gnalk_window_on_scroll_to_message_requested (NULL, message_id, self);
}

static void
on_message_search_load_more (GnalkMessageSearch *search G_GNUC_UNUSED,
                             const char         *search_term,
                             gint                cursor,
                             GnalkWindow        *self)
{
  if (self->message_controller)
    gnalk_message_controller_search_messages (self->message_controller, search_term, cursor);
}

void
gnalk_window_on_search_results (GnalkMessageController *mc G_GNUC_UNUSED,
                                JsonArray              *entries,
                                gboolean                has_more,
                                gint                    cursor,
                                GnalkWindow            *self)
{
  gnalk_message_search_set_results (self->message_search, entries, has_more, cursor);
}

static void
on_message_search_close (GnalkMessageSearch *search G_GNUC_UNUSED,
                         GnalkWindow        *self)
{
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->message_search_button), FALSE);
}

static void
on_search_messages_action (GSimpleAction *action G_GNUC_UNUSED,
                           GVariant      *parameter G_GNUC_UNUSED,
                           gpointer       user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  gboolean active = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->message_search_button));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->message_search_button), !active);
  if (!active)
    gnalk_message_search_grab_focus (self->message_search);
}

static void
gnalk_window_init (GnalkWindow *self)
{
  static GActionEntry win_actions[] = {
    { "preferences", on_preferences_action },
    { "search-messages", on_search_messages_action },
  };

  static GActionEntry app_actions[] = {
    { "about", on_about_action },
  };

  gtk_widget_init_template (GTK_WIDGET (self));

  g_action_map_add_action_entries (G_ACTION_MAP (self), win_actions, G_N_ELEMENTS (win_actions), self);

  GApplication *app = g_application_get_default ();
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.search-messages",
                                          (const char *[]) { "<Control>f", NULL });
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
      /* conversations-changed reload now handled by DataRepository::data-changed */

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

  /* Sync coordinator connection is now handled in switch_to_session when
   * attaching to a session. Each session has its own sync coordinator. */

  /* Track window focus for notification decisions */
  g_signal_connect (self, "notify::is-active",
                    G_CALLBACK (on_window_is_active_changed), NULL);

  /* Set initial focus state */
  gnalk_application_set_window_focused (GNALK_APPLICATION (app),
                                          gtk_window_is_active (GTK_WINDOW (self)));

  /* MC/CC/PC are now owned by Session - created + connected in switch_to_session
   * via connect_controller_signals() in gnalk-window-account-session.c.
   * Window caches non-owned pointers after session switch. */

  /* Connect to DataRepository for background sync updates */
  {
    GnalkDataRepository *repo = gnalk_application_get_data_repository (GNALK_APPLICATION (app));
    if (repo)
      g_signal_connect (repo, "data-changed",
                        G_CALLBACK (gnalk_window_on_data_repository_changed), self);
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

    /* Create room controller for open/close orchestration.
     * MC/PC will be set when session switches (in switch_to_session). */
    self->room_controller = gnalk_room_controller_new (self->tab_manager,
                                                         self->conversations_store,
                                                         settings);
    gnalk_room_controller_set_scroll_state_func (self->room_controller,
                                                   gnalk_window_get_scroll_state, self);

    g_signal_connect (self->room_controller, "room-opened",
                      G_CALLBACK (gnalk_window_on_room_controller_room_opened), self);
    g_signal_connect (self->room_controller, "room-closed",
                      G_CALLBACK (gnalk_window_on_room_controller_room_closed), self);
    g_signal_connect (self->room_controller, "messages-store-changed",
                      G_CALLBACK (gnalk_window_on_room_controller_messages_store_changed), self);
    g_signal_connect (self->room_controller, "scroll-requested",
                      G_CALLBACK (gnalk_window_on_room_controller_scroll_requested), self);
  }

  /* Create message processor for data operations.
   * MC will be set when session switches (in switch_to_session). */
  self->message_processor = gnalk_message_processor_new ();
  gnalk_message_processor_set_conversations_store (self->message_processor, self->conversations_store);
  g_signal_connect (self->message_processor, "batch-insert-begin",
                    G_CALLBACK (gnalk_window_on_processor_batch_insert_begin), self);
  g_signal_connect (self->message_processor, "batch-insert-end",
                    G_CALLBACK (gnalk_window_on_processor_batch_insert_end), self);
  g_signal_connect (self->message_processor, "scroll-to-bottom",
                    G_CALLBACK (gnalk_window_on_processor_scroll_to_bottom), self);
  g_signal_connect (self->message_processor, "scroll-to-item",
                    G_CALLBACK (gnalk_window_on_processor_scroll_to_item), self);
  g_signal_connect (self->message_processor, "call-state-left",
                    G_CALLBACK (gnalk_window_on_processor_call_state_left), self);
  g_signal_connect (self->message_processor, "call-ended",
                    G_CALLBACK (gnalk_window_on_processor_call_ended), self);
  g_signal_connect (self->message_processor, "loading-older-changed",
                    G_CALLBACK (gnalk_window_on_processor_loading_older_changed), self);
  g_signal_connect (self->message_processor, "state-synced",
                    G_CALLBACK (gnalk_window_on_processor_state_synced), self);

  self->pending_call_with_video = -1;
  self->bound_unread_rows = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->bound_conversation_rows = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->bound_message_rows = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->visible_unread_ids = g_array_new (FALSE, FALSE, sizeof (gint64));

  /* Federation invitation buttons */
  g_signal_connect_swapped (self->invitation_accept_button, "clicked",
                             G_CALLBACK (on_invitation_accept_clicked), self);
  g_signal_connect_swapped (self->invitation_decline_button, "clicked",
                             G_CALLBACK (on_invitation_decline_clicked), self);

  /* Conversation list - uses GtkSortListModel for incremental sorting */
  self->conversations_sorter = gtk_custom_sorter_new (
      (GCompareDataFunc) gnalk_window_compare_conversations_by_activity, NULL, NULL);
  self->conversations_sort_model = gtk_sort_list_model_new (
      G_LIST_MODEL (g_object_ref (self->conversations_store)),
      GTK_SORTER (g_object_ref (self->conversations_sorter)));
  GtkNoSelection *conv_selection = gtk_no_selection_new (
      G_LIST_MODEL (g_object_ref (self->conversations_sort_model)));
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
  g_signal_connect (self->message_input, "attach-clicked", G_CALLBACK (gnalk_window_on_attach_clicked), self);
  g_signal_connect (self->message_input, "voice-message-recorded", G_CALLBACK (gnalk_window_on_voice_message_recorded), self);
  g_signal_connect (self->message_input, "camera-clicked", G_CALLBACK (gnalk_window_on_camera_clicked), self);
  g_signal_connect (self->conversation_header, "info-requested", G_CALLBACK (on_conversation_info_requested), self);
  g_signal_connect (self->video_call_button, "clicked", G_CALLBACK (on_video_call_clicked), self);
  g_signal_connect (self->audio_call_button, "clicked", G_CALLBACK (on_audio_call_clicked), self);
  g_signal_connect (self->participants_toggle_button, "toggled", G_CALLBACK (gnalk_window_on_participants_toggled), self);
  g_signal_connect (self->participants_list, "chat-requested", G_CALLBACK (gnalk_window_on_participant_chat_requested), self);
  g_signal_connect (self->participants_list, "promote-requested", G_CALLBACK (gnalk_window_on_participant_promote_requested), self);
  g_signal_connect (self->participants_list, "demote-requested", G_CALLBACK (gnalk_window_on_participant_demote_requested), self);
  g_signal_connect (self->participants_list, "remove-requested", G_CALLBACK (gnalk_window_on_participant_remove_requested), self);
  g_signal_connect (self->participants_list, "participant-clicked", G_CALLBACK (on_participant_clicked), self);
  g_signal_connect (self->conversation_actions, "quote-closed", G_CALLBACK (gnalk_window_on_quote_closed), self);
  g_signal_connect (self->conversation_actions, "attachment-closed", G_CALLBACK (gnalk_window_on_attachment_closed), self);
  g_signal_connect (self->conversation_actions, "scroll-to-message-requested", G_CALLBACK (gnalk_window_on_quote_preview_scroll_requested), self);
  g_signal_connect (self->user_status_menu, "status-changed", G_CALLBACK (on_user_status_changed), self);
  /* Signaling handlers are connected in switch_to_session after getting signaling from session */
  g_signal_connect (self->incoming_call_banner, "accept-audio-call-requested", G_CALLBACK (on_incoming_call_accept_audio), self);
  g_signal_connect (self->incoming_call_banner, "accept-video-call-requested", G_CALLBACK (on_incoming_call_accept_video), self);
  g_signal_connect (self->incoming_call_banner, "refuse-call-requested", G_CALLBACK (on_incoming_call_refuse_requested), self);

  /* Clear messages when navigating back in compact mode (before view is hidden) */
  g_signal_connect (self->content_page, "hiding", G_CALLBACK (gnalk_window_on_content_page_hiding), self);

  /* Connect message view signals for lazy loading */
  g_signal_connect (self->message_view, "load-older-requested", G_CALLBACK (on_message_view_load_older_requested), self);

  /* Connect message view signal for scroll-to-bottom button */
  g_signal_connect (self->message_view, "scroll-to-bottom-clicked", G_CALLBACK (gnalk_window_on_scroll_to_bottom_clicked_for_unread), self);

  /* Connect scroll-changed to re-check visibility of unread messages on scroll */
  g_signal_connect (self->message_view, "scroll-changed", G_CALLBACK (gnalk_window_on_message_scroll_changed), self);
  g_signal_connect (self->message_view, "jump-to-present-clicked", G_CALLBACK (gnalk_window_on_jump_to_present), self);
  g_signal_connect (self->message_view, "exit-thread-clicked", G_CALLBACK (gnalk_window_on_exit_thread), self);

  /* Message search panel: bind toggle button to revealer */
  g_object_bind_property (self->message_search_button, "active",
                          self->message_search_revealer, "reveal-child",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
  g_signal_connect (self->message_search, "search-requested",
                    G_CALLBACK (on_message_search_requested), self);
  g_signal_connect (self->message_search, "result-activated",
                    G_CALLBACK (on_message_search_result_activated), self);
  g_signal_connect (self->message_search, "load-more-requested",
                    G_CALLBACK (on_message_search_load_more), self);
  g_signal_connect (self->message_search, "close-requested",
                    G_CALLBACK (on_message_search_close), self);

  /* Set up breakpoint for responsive message display (wide vs narrow layout) */
  AdwBreakpoint *wide_breakpoint = adw_breakpoint_new (
    adw_breakpoint_condition_parse ("min-width: 1100px"));
  g_signal_connect (wide_breakpoint, "apply", G_CALLBACK (on_wide_breakpoint_applied), self);
  g_signal_connect (wide_breakpoint, "unapply", G_CALLBACK (on_wide_breakpoint_unapplied), self);
  adw_application_window_add_breakpoint (ADW_APPLICATION_WINDOW (self), wide_breakpoint);

  /* Connect login view signals */
  g_signal_connect (self->login_view, "connect-clicked", G_CALLBACK (gnalk_window_on_login_view_connect_clicked), self);
  g_signal_connect (self->login_view, "cancel-clicked", G_CALLBACK (gnalk_window_on_login_view_cancel_clicked), self);

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
