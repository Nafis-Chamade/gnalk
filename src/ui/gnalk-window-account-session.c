/* gnalk-window-account-session.c
 *
 * Account and session management functionality extracted from gnalk-window.c.
 * Handles switching between sessions, session manager signal handlers,
 * and connecting/disconnecting session signals.
 *
 * Phase 6: Session is the single facade for the Window.
 * Window connects ONLY to Session signals. Session relays Signaling,
 * SyncCoordinator, and provides MC/CC/PC via getters.
 */

#include "gnalk-window-private.h"
#include "gnalk-application.h"
#include "gnalk-session.h"
#include "gnalk-account.h"
#include "gnalk-database.h"
#include "gnalk-avatar-cache.h"
#include "gnalk-network-status-bar.h"
#include "gnalk-user-status-menu.h"
#include "gnalk-message-processor.h"
#include "gnalk-room-controller.h"
#include "gnalk-room-tab-manager.h"
#include "gnalk-participants-list.h"
#include <glib/gi18n.h>

/* ============ Session Signal Connection Helpers ============ */

static void
connect_session_signals (GnalkWindow  *self,
                         GnalkSession *session)
{
  if (!session)
    return;

  /* Note: Don't connect to "state-changed" here - we receive state changes through
   * session_manager's "any-session-state-changed" signal (connected in gnalk_window_init). */
  g_signal_connect (session, "room-joined",
                    G_CALLBACK (gnalk_window_on_session_room_joined), self);
  g_signal_connect (session, "signaling-room-joined",
                    G_CALLBACK (gnalk_window_on_session_signaling_room_joined), self);
  g_signal_connect (session, "call-capability-changed",
                    G_CALLBACK (gnalk_window_on_session_call_capability_changed), self);
  g_signal_connect (session, "auth-failed",
                    G_CALLBACK (gnalk_window_on_session_auth_failed), self);
  g_signal_connect (session, "incoming-call",
                    G_CALLBACK (gnalk_window_on_session_incoming_call), self);
  g_signal_connect (session, "typing",
                    G_CALLBACK (gnalk_window_on_session_typing), self);
  g_signal_connect (session, "signaling-ready",
                    G_CALLBACK (gnalk_window_on_session_signaling_ready), self);

  /* Relayed signaling signals (Session absorbs Signaling) */
  g_signal_connect (session, "call-ended",
                    G_CALLBACK (gnalk_window_on_session_call_ended), self);
  g_signal_connect (session, "room-disinvited",
                    G_CALLBACK (gnalk_window_on_session_room_disinvited), self);

  /* Relayed sync coordinator signal */
  g_signal_connect (session, "room-messages-synced",
                    G_CALLBACK (gnalk_window_on_session_room_messages_synced), self);
}

static void
disconnect_session_signals (GnalkWindow  *self G_GNUC_UNUSED,
                            GnalkSession *session)
{
  if (!session)
    return;

  g_signal_handlers_disconnect_by_data (session, self);
}

/* Disconnect MC/CC/PC signal handlers (cached from old session) */
static void
disconnect_controller_signals (GnalkWindow *self)
{
  if (self->message_controller)
    {
      g_signal_handlers_disconnect_by_data (self->message_controller, self);
      self->message_controller = NULL;
    }
  if (self->conversation_controller)
    {
      g_signal_handlers_disconnect_by_data (self->conversation_controller, self);
      self->conversation_controller = NULL;
    }
  if (self->participant_controller)
    {
      g_signal_handlers_disconnect_by_data (self->participant_controller, self);
      self->participant_controller = NULL;
    }
}

/* Connect signals to controllers owned by session */
static void
connect_controller_signals (GnalkWindow  *self,
                            GnalkSession *session)
{
  /* Message controller */
  GnalkMessageController *mc = gnalk_session_get_message_controller (session);
  self->message_controller = mc;
  if (mc)
    {
      g_signal_connect (mc, "message-sent",
                        G_CALLBACK (gnalk_window_on_message_controller_sent), self);
      g_signal_connect (mc, "message-send-failed",
                        G_CALLBACK (gnalk_window_on_message_controller_send_failed), self);
      g_signal_connect (mc, "message-edited",
                        G_CALLBACK (gnalk_window_on_message_controller_edited), self);
      g_signal_connect (mc, "message-deleted",
                        G_CALLBACK (gnalk_window_on_message_controller_deleted), self);
      g_signal_connect (mc, "reaction-added",
                        G_CALLBACK (gnalk_window_on_message_controller_reaction_added), self);
      g_signal_connect (mc, "upload-completed",
                        G_CALLBACK (gnalk_window_on_message_controller_upload_completed), self);
      g_signal_connect (mc, "upload-failed",
                        G_CALLBACK (gnalk_window_on_message_controller_upload_failed), self);
      g_signal_connect (mc, "messages-loaded",
                        G_CALLBACK (gnalk_window_on_message_controller_messages_loaded), self);
      g_signal_connect (mc, "poll-response",
                        G_CALLBACK (gnalk_window_on_message_controller_poll_response), self);
      g_signal_connect (mc, "older-messages-loaded",
                        G_CALLBACK (gnalk_window_on_message_controller_older_messages_loaded), self);
      g_signal_connect (mc, "context-loaded",
                        G_CALLBACK (gnalk_window_on_context_loaded), self);
      g_signal_connect (mc, "search-results",
                        G_CALLBACK (gnalk_window_on_search_results), self);
      g_signal_connect (mc, "thread-messages-loaded",
                        G_CALLBACK (gnalk_window_on_thread_messages_loaded), self);
      g_signal_connect (mc, "thread-older-loaded",
                        G_CALLBACK (gnalk_window_on_thread_older_loaded), self);
    }

  /* Conversation controller */
  GnalkConversationController *cc = gnalk_session_get_conversation_controller (session);
  self->conversation_controller = cc;
  if (cc)
    {
      g_signal_connect (cc, "conversation-created",
                        G_CALLBACK (gnalk_window_on_conversation_controller_created), self);
      g_signal_connect (cc, "conversation-left",
                        G_CALLBACK (gnalk_window_on_conversation_controller_left), self);
      g_signal_connect (cc, "conversation-deleted",
                        G_CALLBACK (gnalk_window_on_conversation_controller_deleted), self);
      g_signal_connect (cc, "invitations-updated",
                        G_CALLBACK (gnalk_window_on_invitations_updated), self);
      g_signal_connect (cc, "invitation-accepted",
                        G_CALLBACK (gnalk_window_on_invitation_accepted), self);
      g_signal_connect (cc, "invitation-declined",
                        G_CALLBACK (gnalk_window_on_invitation_declined), self);
    }

  /* Participant controller */
  GnalkParticipantController *pc = gnalk_session_get_participant_controller (session);
  self->participant_controller = pc;
  if (pc)
    {
      g_signal_connect (pc, "participants-changed",
                        G_CALLBACK (gnalk_window_on_participant_controller_changed), self);
      g_signal_connect (pc, "participant-call-changed",
                        G_CALLBACK (gnalk_window_on_participant_call_changed), self);
      g_signal_connect (pc, "all-call-states-cleared",
                        G_CALLBACK (gnalk_window_on_all_call_states_cleared), self);
    }
}

/* ============ Main Session Switch Function ============ */

void
gnalk_window_switch_to_session (GnalkWindow  *self,
                                GnalkSession *session)
{
  /* Disconnect from old session and controllers */
  if (self->active_session)
    {
      disconnect_session_signals (self, self->active_session);
    }
  disconnect_controller_signals (self);

  self->active_session = session;

  /* Reset initial load flags so next load works properly */
  self->initial_conversations_loaded = FALSE;
  if (self->current_room)
    self->current_room->initial_load_done = FALSE;

  if (!session)
    {
      /* Close the current room before showing login — ensures we leave
       * signaling/HTTP and clear all room state cleanly. */
      if (self->room_controller)
        gnalk_room_controller_close_current (self->room_controller);

      if (self->current_room)
        {
          self->current_room = NULL;
          gnalk_message_processor_set_room_state (self->message_processor, NULL);
          gnalk_window_clear_messages_store (self);
          self->has_active_room = FALSE;
          g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_HAS_ACTIVE_ROOM]);
          adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->split_view), FALSE);
        }

      if (self->tab_manager)
        gnalk_room_tab_manager_clear_all (self->tab_manager);

      /* No active session - show login view, but only after initial startup.
       * During startup, the UI template defaults to main view while autologin
       * is attempted. Only show login if auth has completed and there's no session. */
      if (self->initial_auth_done)
        gnalk_window_show_login_view (self);
      return;
    }

  /* Connect to new session signals */
  connect_session_signals (self, session);

  /* Update internal references to session's components */
  self->http_client = gnalk_session_get_http_client (session);
  self->sync_manager = gnalk_session_get_sync_manager (session);

  /* Connect to session's controllers */
  connect_controller_signals (self, session);

  /* Connect to session's threads-synced relay signal */
  g_signal_connect (session, "threads-synced",
                    G_CALLBACK (gnalk_window_on_threads_synced), self);

  /* Reset session_ready - new session's POST /participants/active hasn't happened yet */
  if (self->message_controller)
    gnalk_message_controller_set_session_ready (self->message_controller, FALSE);
  if (self->participant_controller)
    gnalk_participant_controller_set_session_ready (self->participant_controller, FALSE);

  /* Update processor dependencies for new session */
  {
    GnalkAccount *account = gnalk_session_get_account (session);
    const char *user_id = gnalk_account_get_user_id (account);
    gnalk_message_processor_set_current_user (self->message_processor, user_id);
    gnalk_message_processor_set_message_controller (self->message_processor, self->message_controller);

    GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
    if (app)
      {
        GnalkDatabase *db = gnalk_application_get_database (app);
        gint64 aid = gnalk_session_get_account_id (session);
        gnalk_message_processor_set_database (self->message_processor, db);
        gnalk_message_processor_set_account_id (self->message_processor, aid);

        /* Cache edit/delete timeouts to avoid per-bind application queries */
        self->cached_edit_timeout = gnalk_application_get_chat_edit_timeout (app);
        self->cached_delete_timeout = gnalk_application_get_chat_delete_timeout (app);
      }

    /* Cache thread capability (coordinator gets it from session in set_state) */
    self->cached_has_threads = gnalk_session_has_threads (session);
  }

  /* Close the current room via the controller before clearing tabs.
   * This backgrounds the tab, saves messages, leaves signaling/HTTP,
   * and clears the controller's current_room pointer so it won't
   * dangle after tab_manager_clear_all frees all tabs. */
  if (self->room_controller)
    gnalk_room_controller_close_current (self->room_controller);

  /* Clean up Window-side state */
  if (self->current_room)
    {
      self->current_room = NULL;
      gnalk_message_processor_set_room_state (self->message_processor, NULL);
      gnalk_window_clear_messages_store (self);
      self->has_active_room = FALSE;
      g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_HAS_ACTIVE_ROOM]);

      /* Hide the content pane in split view mode */
      adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->split_view), FALSE);
    }

  /* Clear participant banner to avoid showing the previous room's participants */
  if (self->participants_list)
    gnalk_participants_list_clear (self->participants_list);

  /* Clear all room tabs to avoid showing messages from the wrong account */
  if (self->tab_manager)
    gnalk_room_tab_manager_clear_all (self->tab_manager);

  /* Clear conversation list immediately to avoid showing the previous account's rooms
   * while the async DB load is in-flight (or won't run at all for guest sessions). */
  if (self->conversations_store)
    g_list_store_remove_all (self->conversations_store);

  g_clear_object (&self->avatar_cache);
  gint64 account_id = gnalk_session_get_account_id (session);
  self->avatar_cache = gnalk_avatar_cache_new (self->http_client, account_id);

  /* Update room controller with new session and controllers from session */
  if (self->room_controller)
    {
      gnalk_room_controller_set_session (self->room_controller, session);
      gnalk_room_controller_set_message_controller (self->room_controller, self->message_controller);
      gnalk_room_controller_set_participant_controller (self->room_controller, self->participant_controller);
    }

  /* Configure guest/non-guest UI regardless of session state */
  {
    gboolean is_guest = gnalk_session_is_guest (session);

    /* Teardown previous guest mode if switching away */
    if (self->is_guest_mode && !is_guest)
      gnalk_window_teardown_guest_mode (self);

    gtk_widget_set_visible (GTK_WIDGET (self->user_status_menu), TRUE);
    gnalk_user_status_menu_set_interactive (self->user_status_menu, !is_guest);

    /* Hide message search for guests (Unified Search requires a Nextcloud user) */
    gtk_widget_set_visible (self->message_search_button, !is_guest);

    if (is_guest)
      {
        /* Show generic avatar for guests (no user_id → fallback icon) */
        gnalk_user_status_menu_set_user (self->user_status_menu, NULL, NULL);
        gnalk_user_status_menu_set_status (self->user_status_menu, NULL);
        gnalk_window_setup_guest_mode (self);
      }
    else
      {
        adw_navigation_page_set_can_pop (ADW_NAVIGATION_PAGE (self->content_page), TRUE);
      }
  }

  /* Update UI based on session state */
  GnalkSessionState state = gnalk_session_get_state (session);

  switch (state)
    {
    case GNALK_SESSION_STATE_ONLINE:
      {
        gtk_stack_set_visible_child_name (GTK_STACK (self->main_stack), "main");
        gnalk_network_status_bar_hide (self->network_status_bar);

        /* Sync state that would normally come from on_session_state_changed.
         * When attaching to an already-online session, those signals already fired. */

        /* Mark avatar cache as online */
        if (self->avatar_cache)
          gnalk_avatar_cache_set_online (self->avatar_cache, TRUE);

        /* Set user avatar and status */
        GnalkAccount *account = gnalk_session_get_account (session);
        const char *user_id = account ? gnalk_account_get_user_id (account) : NULL;
        const char *display_name = account ? gnalk_account_get_display_name (account) : NULL;

        if (display_name && *display_name)
          {
            g_free (self->user_name);
            self->user_name = g_strdup (display_name);
            g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_USER_NAME]);
          }

        if (user_id && self->avatar_cache)
          gnalk_user_status_menu_set_user (self->user_status_menu, user_id, self->avatar_cache);

        /* Set user status to online (not for guests) */
        GnalkApplication *app = GNALK_APPLICATION (g_application_get_default ());
        gboolean is_guest = gnalk_session_is_guest (session);

        if (!is_guest && user_id)
          {
            g_message ("switch_to_session: setting user status to online for %s", user_id);
            gnalk_user_status_menu_set_status (self->user_status_menu, "online");
          }

        /* Auto-select the guest room */
        if (is_guest)
          {
            GnalkAccount *guest_account = gnalk_session_get_account (session);
            const char *room_token = gnalk_account_get_room_token (guest_account);
            if (room_token)
              gnalk_window_open_conversation_by_token (self, room_token);
          }

        /* Sync call capability from session */
        gboolean can_call = gnalk_session_can_call (session);
        g_message ("switch_to_session: syncing call capability: %d", can_call);
        gtk_widget_set_sensitive (self->audio_call_button, can_call);
        gtk_widget_set_sensitive (self->video_call_button, can_call);

        /* Sync ICE servers from session to application (for call window to use).
         * Normally done in on_session_signaling_room_joined, but that signal
         * already fired if session was connected before window attached. */
        const char *stun = gnalk_session_get_stun_server (session);
        const char *turn = gnalk_session_get_turn_server (session);
        const char *turn_user = gnalk_session_get_turn_username (session);
        const char *turn_cred = gnalk_session_get_turn_credential (session);
        if (stun || turn)
          {
            gnalk_application_set_ice_servers (app, stun, turn, turn_user, turn_cred);
            g_message ("switch_to_session: synced ICE servers: STUN=%s, TURN=%s",
                       stun ? stun : "(none)", turn ? turn : "(none)");
          }

        self->initial_auth_done = TRUE;
      }
      break;

    case GNALK_SESSION_STATE_CONNECTING:
      gtk_stack_set_visible_child_name (GTK_STACK (self->main_stack), "main");
      gtk_spinner_set_spinning (GTK_SPINNER (self->connecting_spinner), TRUE);
      gtk_widget_set_visible (self->connecting_spinner, TRUE);
      break;

    case GNALK_SESSION_STATE_OFFLINE:
    case GNALK_SESSION_STATE_RECONNECTING:
      gtk_stack_set_visible_child_name (GTK_STACK (self->main_stack), "main");
      gnalk_network_status_bar_set_offline (self->network_status_bar, TRUE,
                                              state == GNALK_SESSION_STATE_RECONNECTING
                                                ? _("Reconnecting...")
                                                : _("Offline"));
      break;

    case GNALK_SESSION_STATE_DISCONNECTED:
      gnalk_window_show_login_view (self);
      break;
    }

  /* Load conversations from DB when attaching to an already-online session.
   * The store was just cleared above, so we always need to populate it.
   * (For CONNECTING sessions, the load will happen via data-changed when
   * the Session's on_conversations_response saves to DB.) */
  if (state == GNALK_SESSION_STATE_ONLINE)
    gnalk_window_load_conversations_from_db (self);
}

/* ============ Session Manager Signal Handlers ============ */

void
gnalk_window_on_session_manager_active_changed (GnalkSessionManager *manager G_GNUC_UNUSED,
                                                 GnalkSession        *session,
                                                 GnalkWindow         *self)
{
  g_message ("Window: active session changed");
  gnalk_window_switch_to_session (self, session);

  /* Save last active account to settings for restore on next launch */
  if (session)
    {
      gint64 account_id = gnalk_session_get_account_id (session);
      GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
      GSettings *settings = gnalk_application_get_settings (app);
      g_settings_set_int64 (settings, "last-account-id", account_id);
    }
}

void
gnalk_window_on_session_manager_any_state_changed (GnalkSessionManager *manager G_GNUC_UNUSED,
                                                    GnalkSession        *session,
                                                    int                   old_state,
                                                    int                   new_state,
                                                    GnalkWindow         *self)
{
  /* Only update UI if this is the active session */
  if (session != self->active_session)
    return;

  gnalk_window_on_session_state_changed (session, old_state, new_state, self);
}

/* ============ Session Disconnect Helper (for dispose) ============ */

void
gnalk_window_disconnect_active_session (GnalkWindow *self)
{
  if (self->active_session)
    {
      disconnect_session_signals (self, self->active_session);
      disconnect_controller_signals (self);
      self->active_session = NULL;
    }
}
