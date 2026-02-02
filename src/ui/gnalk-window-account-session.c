/* gnalk-window-account-session.c
 *
 * Account and session management functionality extracted from gnalk-window.c.
 * Handles switching between sessions, session manager signal handlers,
 * and connecting/disconnecting session signals.
 */

#include "gnalk-window-private.h"
#include "gnalk-application.h"
#include "gnalk-database.h"
#include "gnalk-conversation-data.h"
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
  g_signal_connect (session, "conversations-updated",
                    G_CALLBACK (gnalk_window_on_session_conversations_updated), self);
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
}

static void
disconnect_session_signals (GnalkWindow  *self G_GNUC_UNUSED,
                            GnalkSession *session)
{
  if (!session)
    return;

  g_signal_handlers_disconnect_by_data (session, self);
}

/* ============ Main Session Switch Function ============ */

void
gnalk_window_switch_to_session (GnalkWindow  *self,
                                 GnalkSession *session)
{
  /* Disconnect from old session */
  if (self->active_session)
    disconnect_session_signals (self, self->active_session);

  self->active_session = session;

  if (!session)
    {
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

  /* Disconnect from old signaling before switching */
  if (self->signaling)
    g_signal_handlers_disconnect_by_data (self->signaling, self);

  self->signaling = gnalk_session_get_signaling (session);
  self->sync_manager = gnalk_session_get_sync_manager (session);

  /* Connect window handlers to session's signaling */
  if (self->signaling)
    {
      g_signal_connect (self->signaling, "typing",
                        G_CALLBACK (gnalk_window_on_signaling_typing), self);
      g_signal_connect (self->signaling, "incoming-call",
                        G_CALLBACK (gnalk_window_on_signaling_incoming_call), self);
      g_signal_connect (self->signaling, "call-state-changed",
                        G_CALLBACK (gnalk_window_on_signaling_call_state_changed), self);
      g_signal_connect (self->signaling, "call-ended",
                        G_CALLBACK (gnalk_window_on_signaling_call_ended), self);
      g_signal_connect (self->signaling, "room-disinvited",
                        G_CALLBACK (gnalk_window_on_signaling_room_disinvited), self);
    }

  /* Recreate components that depend on http_client */
  g_clear_object (&self->message_controller);
  self->message_controller = gnalk_message_controller_new (self->http_client);

  /* Set up database integration for MessageController */
  {
    GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
    if (app)
      {
        GnalkDatabase *db = gnalk_application_get_database (app);
        gint64 account_id = gnalk_session_get_account_id (session);
        const char *user_id = gnalk_application_get_user_id (app);
        gnalk_message_controller_set_database (self->message_controller, db, account_id);
        gnalk_message_controller_set_current_user (self->message_controller, user_id);
      }
  }

  /* Reconnect message controller signals */
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

  /* Restore room_token if we had an active room */
  if (self->current_room_token)
    gnalk_message_controller_set_room (self->message_controller, self->current_room_token);

  g_clear_object (&self->avatar_cache);
  gint64 account_id = gnalk_session_get_account_id (session);
  self->avatar_cache = gnalk_avatar_cache_new (self->http_client, account_id);

  /* Recreate conversation controller with new http_client and account */
  if (self->conversation_controller)
    g_signal_handlers_disconnect_by_data (self->conversation_controller, self);
  g_clear_object (&self->conversation_controller);
  {
    GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
    GnalkDatabase *db = gnalk_application_get_database (app);
    gint64 account_id = gnalk_session_get_account_id (session);

    g_debug ("switch_to_session: recreating conversation_controller with account_id=%ld", (long)account_id);
    self->conversation_controller = gnalk_conversation_controller_new (db, self->http_client);
    gnalk_conversation_controller_set_account_id (self->conversation_controller, account_id);
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

  /* Recreate participant controller with new http_client and account */
  if (self->participant_controller)
    g_signal_handlers_disconnect_by_data (self->participant_controller, self);
  g_clear_object (&self->participant_controller);
  {
    GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
    GnalkDatabase *db = gnalk_application_get_database (app);
    gint64 account_id = gnalk_session_get_account_id (session);

    g_debug ("switch_to_session: recreating participant_controller with account_id=%ld", (long)account_id);
    self->participant_controller = gnalk_participant_controller_new (db, self->http_client);
    gnalk_participant_controller_set_account_id (self->participant_controller, account_id);
    g_signal_connect (self->participant_controller, "participants-changed",
                      G_CALLBACK (gnalk_window_on_participant_controller_changed), self);
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

        /* Refresh all message rows to load file previews now that avatar cache is ready */
        gnalk_window_refresh_all_messages (self);

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
        gboolean is_guest = gnalk_application_is_guest (app);

        if (!is_guest && user_id)
          {
            g_message ("switch_to_session: setting user status to online for %s", user_id);
            gnalk_user_status_menu_set_status (self->user_status_menu, "online");
          }

        /* Configure UI for guest mode */
        if (is_guest)
          {
            /* Hide user avatar and status button for guests */
            gtk_widget_set_visible (GTK_WIDGET (self->user_status_menu), FALSE);

            /* Collapse sidebar for guests - they only have access to one room */
            adw_navigation_split_view_set_collapsed (ADW_NAVIGATION_SPLIT_VIEW (self->split_view), TRUE);
            adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->split_view), TRUE);

            /* Disable back button for guests - they can't return to conversations list */
            adw_navigation_page_set_can_pop (ADW_NAVIGATION_PAGE (self->content_page), FALSE);

            /* Auto-select the guest room */
            const char *room_token = gnalk_application_get_guest_room_token (app);
            if (room_token)
              gnalk_window_open_conversation_by_token (self, room_token);
          }
        else
          {
            /* Ensure status menu is visible for non-guests */
            gtk_widget_set_visible (GTK_WIDGET (self->user_status_menu), TRUE);
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

  /* Sync conversations from session (with section headers) */
  GListStore *conversations = gnalk_session_get_conversations (session);
  if (conversations)
    {
      /* Build complete list with headers before updating store */
      gboolean has_favorites = FALSE;
      gboolean has_non_favorites = FALSE;
      GList *rooms = NULL;

      guint n = g_list_model_get_n_items (G_LIST_MODEL (conversations));
      for (guint i = 0; i < n; i++)
        {
          GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (conversations), i);
          if (conv)
            {
              if (gnalk_conversation_data_get_is_favorite (conv))
                has_favorites = TRUE;
              else
                has_non_favorites = TRUE;
              rooms = g_list_prepend (rooms, g_object_ref (conv));
              g_object_unref (conv);
            }
        }

      /* Add section headers if we have both favorites and non-favorites */
      if (has_favorites && has_non_favorites)
        {
          GnalkConversationData *fav_header = gnalk_conversation_data_new_section_header ("Favorites", TRUE);
          rooms = g_list_prepend (rooms, fav_header);

          GnalkConversationData *conv_header = gnalk_conversation_data_new_section_header ("Conversations", FALSE);
          rooms = g_list_prepend (rooms, conv_header);
        }

      /* Sort before adding to store */
      rooms = g_list_sort_with_data (rooms, gnalk_window_compare_conversations_by_activity, NULL);

      /* Update store in one batch */
      g_list_store_remove_all (self->conversations_store);
      for (GList *l = rooms; l; l = l->next)
        g_list_store_append (self->conversations_store, l->data);

      g_list_free_full (rooms, g_object_unref);

      /* If a room was opened from immediate restore, now load its full info and participants */
      gnalk_window_maybe_restore_last_room (self);
    }
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
  /* Don't save guest sessions - they are not persisted */
  if (session && !gnalk_session_is_guest (session))
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

/* Rebuild conversation list from active session only.
 * Uses in-place updates to preserve row widgets and their state (e.g., open popovers).
 * Note: We only show conversations from the active session to ensure correct display
 * names for 1-to-1 rooms (e.g., a conversation between A and B shows as "B" for A
 * and "A" for B). */
void
gnalk_window_on_session_manager_conversations_changed (GnalkSessionManager *manager,
                                                        GnalkSession        *session,
                                                        GnalkWindow         *self)
{
  /* Only update if this is the active session */
  GnalkSession *active = gnalk_session_manager_get_active_session (manager);
  if (session != active)
    return;

  g_message ("Window: session_manager conversations_changed (active session), updating store in-place");

  /* Build hash table of new conversations by token */
  g_autoptr(GHashTable) new_convs = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);
  gboolean has_favorites = FALSE;
  gboolean has_non_favorites = FALSE;

  /* Collect conversations from active session only */
  GListStore *convs = gnalk_session_get_conversations (active);
  guint n = g_list_model_get_n_items (G_LIST_MODEL (convs));

  for (guint i = 0; i < n; i++)
    {
      GnalkConversationData *data = g_list_model_get_item (G_LIST_MODEL (convs), i);
      if (data && !gnalk_conversation_data_is_section_header (data))
        {
          const char *token = gnalk_conversation_data_get_token (data);
          if (token && *token)
            {
              g_hash_table_insert (new_convs, (gpointer) token, data);  /* Takes ownership of ref */
              if (gnalk_conversation_data_get_is_favorite (data))
                has_favorites = TRUE;
              else
                has_non_favorites = TRUE;
            }
          else
            g_object_unref (data);
        }
      else
        g_clear_object (&data);
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

  /* If a room was opened from immediate restore (before conversations loaded),
   * now populate it with full info and load participants */
  gnalk_window_maybe_restore_last_room (self);
}

/* ============ Session Disconnect Helper (for dispose) ============ */

void
gnalk_window_disconnect_active_session (GnalkWindow *self)
{
  if (self->active_session)
    {
      disconnect_session_signals (self, self->active_session);
      self->active_session = NULL;
    }
}
