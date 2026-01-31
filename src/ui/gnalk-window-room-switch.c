/* gnalk-window-room-switch.c
 *
 * Room switching functionality extracted from gnalk-window.c.
 * Handles opening conversations, room join sequences, and debouncing.
 */

#include "gnalk-window-private.h"
#include "gnalk-application.h"
#include "gnalk-database.h"
#include "gnalk-conversation-data.h"
#include "gnalk-log.h"
#include <glib/gi18n.h>

/* Debounce timeout for room switching (300ms) - longer to reduce server load */
#define ROOM_SWITCH_DEBOUNCE_MS 300

/* ============ Tab Management Helpers ============ */

/**
 * background_current_room_tab:
 * @self: The window
 *
 * Called before switching rooms to save the current room's scroll state
 * and trim messages for memory efficiency.
 */
static void
background_current_room_tab (GnalkWindow *self)
{
  GNALK_TRACE_ROOM_SWITCH ("background_current_room_tab: called");

  if (!self->current_room_token || !self->tab_manager)
    {
      GNALK_TRACE_ROOM_SWITCH ("background_current_room_tab: no current room or tab_manager, skipping");
      return;
    }

  GNALK_TRACE_ROOM_SWITCH ("background_current_room_tab: backgrounding room %s", self->current_room_token);

  /* Get viewport dimensions for trimming */
  int viewport_width = 0, viewport_height = 0;
  gnalk_message_view_get_viewport_size (self->message_view, &viewport_width, &viewport_height);
  GNALK_TRACE_SCROLL ("background_current_room_tab: viewport size %dx%d", viewport_width, viewport_height);

  /* Capture scroll anchor */
  gboolean at_bottom = gnalk_message_view_is_at_bottom (self->message_view);
  gint64 anchor_id = 0;
  gdouble anchor_offset = 0.0;

  GNALK_TRACE_SCROLL ("background_current_room_tab: at_bottom=%d", at_bottom);

  if (!at_bottom)
    {
      /* Find anchor message - first visible message position */
      guint first_visible = gnalk_message_view_get_first_visible_position (self->message_view);
      guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));

      GNALK_TRACE_SCROLL ("background_current_room_tab: first_visible=%u, n_items=%u", first_visible, n_items);

      if (first_visible < n_items)
        {
          GnalkMessageData *anchor_msg = g_list_model_get_item (G_LIST_MODEL (self->messages_store), first_visible);
          if (anchor_msg)
            {
              anchor_id = gnalk_message_data_get_id (anchor_msg);
              GNALK_TRACE_SCROLL ("background_current_room_tab: anchor_id=%"G_GINT64_FORMAT, anchor_id);
              g_object_unref (anchor_msg);
            }
        }

      /* Get scroll offset for precise restoration */
      anchor_offset = gnalk_message_view_get_scroll_position (self->message_view);
      GNALK_TRACE_SCROLL ("background_current_room_tab: anchor_offset=%.1f", anchor_offset);
    }

  /* Background the tab */
  gnalk_room_tab_manager_background_tab (self->tab_manager,
                                           self->current_room_token,
                                           anchor_id,
                                           anchor_offset,
                                           at_bottom,
                                           viewport_height,
                                           viewport_width);

  g_debug ("Backgrounded tab %s: anchor=%"G_GINT64_FORMAT", at_bottom=%d",
           self->current_room_token, anchor_id, at_bottom);
}

/**
 * switch_to_room_tab:
 * @self: The window
 * @token: The room token
 *
 * Called when entering a room to switch to its tab's message store
 * and restore scroll position.
 */
static void
switch_to_room_tab (GnalkWindow *self, const char *token)
{
  GNALK_TRACE_ROOM_SWITCH ("switch_to_room_tab: switching to room %s", token);

  if (!self->tab_manager)
    {
      GNALK_TRACE_ROOM_SWITCH ("switch_to_room_tab: no tab_manager, skipping");
      return;
    }

  /* Get anchor info before switching */
  gint64 anchor_id = 0;
  gdouble anchor_offset = 0.0;
  gboolean was_at_bottom = TRUE;

  gnalk_room_tab_manager_foreground_tab (self->tab_manager, token,
                                           &anchor_id, &anchor_offset, &was_at_bottom);

  GNALK_TRACE_SCROLL ("switch_to_room_tab: anchor_id=%"G_GINT64_FORMAT", offset=%.1f, was_at_bottom=%d",
                       anchor_id, anchor_offset, was_at_bottom);

  /* Get the tab's store */
  GnalkRoomTab *tab = gnalk_room_tab_manager_get_tab (self->tab_manager, token);
  if (!tab)
    {
      GNALK_TRACE_ROOM_SWITCH ("switch_to_room_tab: failed to get tab");
      return;
    }

  /* Point messages_store to the tab's store for easy access from other code.
   * Note: We keep the old messages_store pattern for compatibility but the
   * tab's store is the source of truth. */
  guint old_count = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  guint tab_count = g_list_model_get_n_items (G_LIST_MODEL (tab->messages_store));
  GNALK_TRACE_STORE ("switch_to_room_tab: old store %p has %u items, tab store %p has %u items",
                      (void*)self->messages_store, old_count, (void*)tab->messages_store, tab_count);

  if (self->messages_store != tab->messages_store)
    {
      GNALK_TRACE_STORE ("switch_to_room_tab: swapping message store from %p to %p",
                          (void*)self->messages_store, (void*)tab->messages_store);

      /* Disconnect old model from selection */
      gtk_no_selection_set_model (self->messages_selection, NULL);
      GNALK_TRACE_UI ("switch_to_room_tab: disconnected old model from selection");

      /* Swap to the tab's store */
      g_set_object (&self->messages_store, tab->messages_store);

      /* Reconnect model */
      gtk_no_selection_set_model (self->messages_selection, G_LIST_MODEL (self->messages_store));
      GNALK_TRACE_UI ("switch_to_room_tab: reconnected new model to selection with %u items",
                       g_list_model_get_n_items (G_LIST_MODEL (self->messages_store)));
    }
  else
    {
      GNALK_TRACE_STORE ("switch_to_room_tab: store already matches tab store, no swap needed (%u items)", tab_count);
    }

  /* Update window state from tab */
  self->oldest_loaded_message_id = tab->oldest_loaded_id;
  self->last_known_message_id = tab->newest_loaded_id;
  self->has_more_history = tab->has_more_history;
  self->initial_messages_loaded = tab->initial_load_done;
  self->last_read_message_id = tab->last_read_message_id;

  GNALK_TRACE_MESSAGES ("switch_to_room_tab: updated window state: oldest=%"G_GINT64_FORMAT", newest=%"G_GINT64_FORMAT", initial_load=%d",
                         self->oldest_loaded_message_id, self->last_known_message_id, self->initial_messages_loaded);

  /* Note: We don't scroll here anymore. The caller will use show_messages_at_* functions
   * which handle both showing and scrolling atomically to avoid visual jumps.
   * We just log the anchor info for debugging. */
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  GNALK_TRACE_SCROLL ("switch_to_room_tab: %u items in store, anchor_id=%"G_GINT64_FORMAT", was_at_bottom=%d",
                       n_items, anchor_id, was_at_bottom);

  /* Enforce tab limit after switching */
  gnalk_room_tab_manager_enforce_limit (self->tab_manager);

  g_debug ("Switched to tab %s: %u messages, anchor=%"G_GINT64_FORMAT", at_bottom=%d",
           token, n_items, anchor_id, was_at_bottom);
}

/* ============ Helper functions ============ */

static void
on_room_avatar_loaded (GnalkAvatarCache *cache,
                       GdkTexture        *texture,
                       gpointer           user_data)
{
  (void) cache;
  GnalkWindow *self = GNALK_WINDOW (user_data);

  gnalk_conversation_header_set_avatar (self->conversation_header, texture);
}

/* ============ Room Join HTTP Sequence ============ */

/* Forward declaration */
static void start_room_join_sequence (GnalkWindow *self, const char *token);

static void
on_room_join_response (GnalkHttpClient *client,
                        JsonNode         *response,
                        GError           *error,
                        gpointer          user_data)
{
  (void) client;
  GnalkWindow *self = GNALK_WINDOW (user_data);

  /* Mark join sequence complete */
  self->room_join_in_progress = FALSE;

  if (error)
    {
      g_warning ("Failed to join room: %s", error->message);
      /* If we switched rooms during the failed request, start sequence for new room */
      if (self->current_room_token && self->pending_room_token &&
          g_strcmp0 (self->current_room_token, self->pending_room_token) != 0)
        {
          g_message ("on_room_join_response: error, but room changed - starting new sequence");
          start_room_join_sequence (self, self->current_room_token);
        }
      return;
    }

  if (response && JSON_NODE_HOLDS_OBJECT (response))
    {
      JsonObject *root = json_node_get_object (response);
      JsonObject *ocs = json_object_get_object_member (root, "ocs");
      if (ocs)
        {
          JsonObject *data = json_object_get_object_member (ocs, "data");
          if (data)
            {
              const char *response_token = json_object_get_string_member_with_default (data, "token", NULL);
              const char *session_id = json_object_get_string_member_with_default (data, "sessionId", NULL);
              g_message ("on_room_join_response: got token=%s sessionId=%s",
                         response_token ? response_token : "(null)",
                         session_id ? session_id : "(null)");

              /* Check if user has switched rooms since the request was made */
              if (response_token && g_strcmp0 (response_token, self->current_room_token) != 0)
                {
                  g_message ("on_room_join_response: room changed from %s to %s - starting new sequence",
                             response_token, self->current_room_token ? self->current_room_token : "(null)");
                  /* Start sequence for the room we actually want to be in */
                  start_room_join_sequence (self, self->current_room_token);
                  return;
                }

              if (session_id)
                {
                  g_free (self->current_session_id);
                  self->current_session_id = g_strdup (session_id);

                  /* Join the signaling room with our session ID.
                   * This establishes the mapping between our Nextcloud room session ID
                   * and the signaling server's session ID, which is required for the
                   * signaling server to correctly process inCall updates. */
                  if (self->signaling && gnalk_signaling_is_connected (self->signaling) &&
                      self->current_room_token)
                    {
                      g_message ("on_room_join_response: joining signaling room %s", self->current_room_token);
                      gnalk_signaling_join_room (self->signaling, self->current_room_token, session_id);
                    }
                  else
                    {
                      g_message ("on_room_join_response: signaling not ready, will join later (connected=%d, room=%s)",
                                 self->signaling ? gnalk_signaling_is_connected (self->signaling) : -1,
                                 self->current_room_token ? self->current_room_token : "(null)");
                    }
                }
            }
        }
    }
}

/**
 * start_room_join_sequence:
 * @self: The window
 * @token: The room token to join
 *
 * Starts the HTTP sequence for joining a room. This should only be called
 * when no other join sequence is in progress, or when we need to restart
 * after a room switch.
 */
static void
start_room_join_sequence (GnalkWindow *self, const char *token)
{
  if (!token || !self->http_client)
    return;

  /* Check if we're online - if not, skip HTTP sequence */
  if (self->active_session && !gnalk_session_is_online (self->active_session))
    {
      g_message ("start_room_join_sequence: offline, skipping HTTP for %s", token);
      return;
    }

  g_message ("start_room_join_sequence: starting HTTP sequence for %s", token);
  self->room_join_in_progress = TRUE;
  self->room_join_generation++;

  /* Start HTTP join - this will get us the session ID for signaling.
   * Messages are loaded from DB, controller handles syncing new ones. */
  gnalk_http_client_join_room (self->http_client, token, on_room_join_response, self);
}

/* ============ Load Participants ============ */

/* Load and display participants for a room (cache-first pattern) */
void
gnalk_window_load_participants (GnalkWindow *self, const char *token)
{
  /* Clear call states from previous room */
  gnalk_signaling_clear_call_states (self->signaling);

  /* Hide room call icon initially */
  gnalk_conversation_header_set_call_active (self->conversation_header, FALSE);

  /* Hide participants bar for 1:1 conversations, but still fetch for call state */
  if (self->current_room_type == 1)
    gnalk_participants_list_hide (self->participants_list);

  /* Guests cannot fetch participant list (403 Forbidden) */
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  if (gnalk_application_is_guest (app))
    {
      gnalk_participants_list_hide (self->participants_list);
      return;
    }

  /* Clear existing participants */
  gnalk_participants_list_clear (self->participants_list);

  /* Load cached participants for immediate display */
  GnalkDatabase *db = gnalk_application_get_database (app);
  gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;
  const char *my_user_id = gnalk_application_get_user_id (app);

  g_debug ("load_participants: token=%s, account_id=%ld, room_type=%d",
           token, (long)account_id, self->current_room_type);

  /* Get current user's participant type from conversation data */
  gint my_participant_type = 3;  /* Default to regular user */
  guint n_convs = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
  for (guint i = 0; i < n_convs; i++)
    {
      GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
      const char *conv_token = gnalk_conversation_data_get_token (conv);
      if (g_strcmp0 (conv_token, token) == 0)
        {
          my_participant_type = gnalk_conversation_data_get_participant_type (conv);
          g_object_unref (conv);
          break;
        }
      g_object_unref (conv);
    }

  if (db && account_id > 0)
    {
      JsonArray *cached = gnalk_database_load_participants (db, account_id, token);
      if (cached)
        {
          guint n = json_array_get_length (cached);

          /* For 1:1, update call icon for the other participant */
          for (guint i = 0; i < n; i++)
            {
              JsonObject *p = json_array_get_object_element (cached, i);
              const char *actor_id = json_object_get_string_member_with_default (p, "actorId", "");
              gint64 in_call = json_object_get_int_member_with_default (p, "inCall", 0);
              if (self->current_room_type == 1 && actor_id && *actor_id &&
                  g_strcmp0 (actor_id, my_user_id) != 0)
                gnalk_window_update_room_call_icon (self, 1, in_call);
            }

          gnalk_participants_list_set_participants (self->participants_list, cached,
                                                      self->avatar_cache, self->signaling,
                                                      my_user_id, my_participant_type,
                                                      self->current_room_type);

          /* For group rooms with participants, respect user preference */
          if (self->current_room_type != 1 && n > 0)
            {
              GSettings *settings = gnalk_application_get_settings (app);
              gboolean show_participants = g_settings_get_boolean (settings, "show-participants");
              gnalk_participants_list_set_visible (self->participants_list, show_participants);
              /* Sync toggle button with the setting */
              gnalk_window_block_participants_toggle (self);
              gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->participants_toggle_button), show_participants);
              gnalk_window_unblock_participants_toggle (self);
            }
          else
            gnalk_participants_list_hide (self->participants_list);

          json_array_unref (cached);
        }
    }

  /* Notify participant controller to start fetching/polling (will emit signal if data changed) */
  if (self->participant_controller)
    gnalk_participant_controller_set_active_room (self->participant_controller, token, self->current_room_type);
}

/* ============ Room Switch Debouncing ============ */

static gboolean
on_room_switch_debounce_timeout (gpointer user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  self->room_switch_timer_id = 0;

  if (self->pending_room_token)
    {
      g_autofree char *token = g_steal_pointer (&self->pending_room_token);
      gnalk_window_open_conversation_by_token (self, token);
    }

  return G_SOURCE_REMOVE;
}

void
gnalk_window_on_sidebar_conversation_selected (GnalkConversationSidebar *sidebar,
                                                 guint                      position,
                                                 GnalkWindow              *self)
{
  (void) sidebar;
  GnalkConversationData *data = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), position);

  if (!data)
    return;

  const char *token = gnalk_conversation_data_get_token (data);
  g_object_unref (data);

  /* Debounce rapid room switches to avoid overwhelming the server */
  g_free (self->pending_room_token);
  self->pending_room_token = g_strdup (token);

  if (self->room_switch_timer_id > 0)
    g_source_remove (self->room_switch_timer_id);

  self->room_switch_timer_id = g_timeout_add (ROOM_SWITCH_DEBOUNCE_MS,
                                               on_room_switch_debounce_timeout, self);
}

/* ============ Content Page Hiding (Leave Room) ============ */

void
gnalk_window_on_content_page_hiding (AdwNavigationPage *page,
                                       gpointer           user_data)
{
  (void) page;
  GnalkWindow *self = GNALK_WINDOW (user_data);

  /* Background the current tab (saves scroll anchor) before clearing */
  background_current_room_tab (self);

  /* Disconnect the model from the view but don't clear the tab's store.
   * The tab manager keeps the messages for when the user returns. */
  gtk_no_selection_set_model (self->messages_selection, NULL);

  /* Leave the room when navigating back in collapsed mode.
   * This ensures push notifications work instead of signaling messages,
   * and allows re-entering the room later. */
  if (self->current_room_token)
    {
      g_message ("Content page hiding: leaving room %s", self->current_room_token);

      /* Stop the participants refresh via controller */
      if (self->participant_controller)
        gnalk_participant_controller_set_active_room (self->participant_controller, NULL, 0);

      /* Resume conversation polling now that we're leaving the room */
      if (self->conversation_controller)
        gnalk_conversation_controller_set_room_active (self->conversation_controller, FALSE);

      if (self->signaling)
        gnalk_signaling_leave_room (self->signaling, self->current_room_token);

      if (self->http_client)
        gnalk_http_client_leave_room (self->http_client, self->current_room_token, NULL, NULL);

      /* Clear active room in application so notifications work */
      GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
      if (app)
        gnalk_application_set_active_room (app, NULL);

      /* Clear room state so user can re-enter */
      g_autofree char *old_token = g_steal_pointer (&self->current_room_token);
      g_clear_pointer (&self->current_room_name, g_free);
      g_clear_pointer (&self->current_room_description, g_free);
      self->has_active_room = FALSE;
      g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_HAS_ACTIVE_ROOM]);

      /* Update conversation row highlight */
      gnalk_window_update_conversation_active_states (self, old_token, NULL);
    }
}

/* ============ Open Conversation ============ */

/**
 * open_conversation_by_token:
 * @self: The window
 * @token: The room token to open
 *
 * Opens a conversation. The UI is updated immediately from cached data,
 * and HTTP requests are serialized to avoid overlapping sequences.
 */
void
gnalk_window_open_conversation_by_token (GnalkWindow *self, const char *token)
{
  if (!token)
    return;

  /* If we're already in this room, do nothing */
  if (self->current_room_token && g_strcmp0 (self->current_room_token, token) == 0)
    {
      g_message ("open_conversation_by_token: already in room %s", token);
      return;
    }

  /* Find the conversation in our store */
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
  GnalkConversationData *found_data = NULL;

  for (guint i = 0; i < n_items; i++)
    {
      GnalkConversationData *data = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
      const char *data_token = gnalk_conversation_data_get_token (data);
      if (g_strcmp0 (data_token, token) == 0)
        {
          found_data = data;
          break;
        }
      g_object_unref (data);
    }

  if (found_data)
    {
      const char *title = gnalk_conversation_data_get_title (found_data);
      const char *description = gnalk_conversation_data_get_description (found_data);
      gint64 last_common_read = gnalk_conversation_data_get_last_common_read_message (found_data);
      gint room_type = gnalk_conversation_data_get_room_type (found_data);
      const char *user_status = gnalk_conversation_data_get_user_status (found_data);

      g_message ("open_conversation_by_token: entering room %s (join_in_progress=%d)",
                 token, self->room_join_in_progress);

      /* Notify controllers that a room is being opened */
      if (self->conversation_controller)
        gnalk_conversation_controller_set_room_active (self->conversation_controller, TRUE);

      /* Background the old room's tab (saves scroll anchor, trims messages) */
      background_current_room_tab (self);

      /* Save messages for the old room before switching */
      gnalk_window_save_messages_to_db (self);

      /* Stop typing in the old room */
      if (self->is_typing)
        {
          gnalk_signaling_send_typing (self->signaling, FALSE);
          self->is_typing = FALSE;
        }
      if (self->typing_send_timer_id > 0)
        {
          g_source_remove (self->typing_send_timer_id);
          self->typing_send_timer_id = 0;
        }
      gnalk_typing_indicator_clear (self->typing_indicator);

      /* === INSTANT UI UPDATE === */
      /* Update room state immediately - no HTTP needed for this */
      g_autofree char *old_room_token = g_steal_pointer (&self->current_room_token);
      g_free (self->current_room_name);
      g_free (self->current_room_description);
      self->current_room_token = g_strdup (token);
      self->current_room_name = g_strdup (title);
      self->current_room_description = g_strdup (description);
      self->current_room_type = room_type;
      gnalk_message_controller_set_room (self->message_controller, token);

      /* Update conversation row highlights */
      gnalk_window_update_conversation_active_states (self, old_room_token, token);

      /* Save last opened room for autologin */
      GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
      GSettings *settings = gnalk_application_get_settings (app);
      g_settings_set_string (settings, "last-room-token", token);
      self->last_known_message_id = 0;
      self->message_load_generation++;
      self->last_common_read_message = last_common_read;
      self->last_read_message_id = gnalk_conversation_data_get_last_read_message (found_data);
      self->first_unread_message_id = 0;

      /* Remove all weak refs before clearing hash table */
      /* Clear read status tracking before switching rooms */
      gnalk_window_clear_read_status_tracking (self);
      self->has_active_room = TRUE;
      self->scroll_to_bottom = TRUE;
      self->initial_messages_loaded = FALSE;
      self->skip_historical_call_messages = TRUE;
      self->oldest_loaded_message_id = 0;
      self->has_more_history = TRUE;
      gnalk_message_view_set_loading_older (self->message_view, FALSE);
      g_clear_pointer (&self->current_session_id, g_free);

      g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_CURRENT_ROOM_NAME]);
      g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_HAS_ACTIVE_ROOM]);

      /* Update conversation header immediately */
      gnalk_conversation_header_set_room (self->conversation_header, token, title, description, room_type);
      gnalk_conversation_header_set_visible (self->conversation_header, TRUE);
      adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self->content_page), title ? title : _("Messages"));

      gnalk_avatar_cache_get_room_avatar (self->avatar_cache, token, on_room_avatar_loaded, self);

      gnalk_window_update_call_button_state (self);
      gnalk_window_update_room_status_icon (self, room_type, user_status);

      /* Show participants toggle for group conversations */
      {
        gboolean is_group = (room_type != 1);
        gtk_widget_set_visible (self->participants_toggle_button, is_group);
        if (is_group)
          {
            gboolean show_participants = g_settings_get_boolean (settings, "show-participants");
            gnalk_window_block_participants_toggle (self);
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->participants_toggle_button), show_participants);
            gnalk_window_unblock_participants_toggle (self);
          }
      }

      /* Switch to the room's tab (may have cached messages) */
      switch_to_room_tab (self, token);

      /* If tab is empty (new tab), load messages from DB */
      GnalkRoomTab *tab = gnalk_room_tab_manager_get_tab (self->tab_manager, token);
      guint tab_message_count = g_list_model_get_n_items (G_LIST_MODEL (tab->messages_store));
      GNALK_TRACE_ROOM_SWITCH ("open_conversation: tab for %s has %u messages, initial_load_done=%d",
                                 token, tab_message_count, tab->initial_load_done);

      if (!tab->initial_load_done || tab_message_count == 0)
        {
          if (tab->initial_load_done && tab_message_count == 0)
            {
              GNALK_TRACE_ROOM_SWITCH ("open_conversation: tab %s claims loaded but has 0 messages, reloading",
                                         token);
              /* Reset the flag so we properly reload */
              tab->initial_load_done = FALSE;
            }
          GNALK_TRACE_ROOM_SWITCH ("open_conversation: loading messages from DB for %s", token);
          /* Store last_read_message_id in tab for unread marking */
          tab->last_read_message_id = self->last_read_message_id;
          /* Don't show messages view yet - will be shown after load + scroll in load_messages_from_db_finish */
          gnalk_window_load_messages_from_db (self, token);
        }
      else
        {
          GNALK_TRACE_ROOM_SWITCH ("open_conversation: tab %s has %u messages, starting polling from %"G_GINT64_FORMAT,
                                     token, tab_message_count, self->last_known_message_id);

          /* Tab already has messages - show view at the restored position (no visual jump).
           * Use the anchor info from the tab to determine where to show. */
          if (tab->is_at_bottom)
            {
              GNALK_TRACE_SCROLL ("open_conversation: showing at bottom");
              gnalk_message_view_show_messages_at_item (self->message_view, tab_message_count - 1);
            }
          else if (tab->anchor_message_id > 0)
            {
              /* Find anchor position */
              guint anchor_pos = tab_message_count - 1;  /* Default to end */
              for (guint i = 0; i < tab_message_count; i++)
                {
                  GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (tab->messages_store), i);
                  if (msg)
                    {
                      gint64 msg_id = gnalk_message_data_get_id (msg);
                      g_object_unref (msg);
                      if (msg_id == tab->anchor_message_id)
                        {
                          anchor_pos = i;
                          break;
                        }
                    }
                }
              GNALK_TRACE_SCROLL ("open_conversation: showing at anchor position %u", anchor_pos);
              gnalk_message_view_show_messages_at_item (self->message_view, anchor_pos);
            }
          else
            {
              /* No anchor, show at bottom */
              GNALK_TRACE_SCROLL ("open_conversation: no anchor, showing at bottom");
              gnalk_message_view_show_messages_at_item (self->message_view, tab_message_count - 1);
            }

          /* Start polling for new messages */
          if (self->message_controller)
            gnalk_message_controller_start_polling (self->message_controller,
                                                      self->last_known_message_id);
        }

      adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->split_view), TRUE);

      /* Get the session that owns this conversation BEFORE unreferencing */
      GnalkSession *conv_session = gnalk_conversation_data_get_session (found_data);
      if (!conv_session)
        conv_session = self->active_session;  /* Fallback to active session */

      g_object_unref (found_data);

      /* Use the conversation's session to handle room join */
      if (conv_session)
        {
          g_message ("open_conversation_by_token: using session to join %s", token);
          gnalk_session_join_room (conv_session, token);
        }

      /* Load participants (can happen in parallel) */
      gnalk_window_load_participants (self, token);
    }
  else
    {
      /* Conversation not in our list - join and refresh */
      g_message ("open_conversation_by_token: room %s not in store, joining", token);

      g_autofree char *old_room_token = g_steal_pointer (&self->current_room_token);
      self->current_room_token = g_strdup (token);
      self->has_active_room = TRUE;
      gnalk_message_controller_set_room (self->message_controller, token);

      /* Update conversation row highlights */
      gnalk_window_update_conversation_active_states (self, old_room_token, token);

      g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_HAS_ACTIVE_ROOM]);

      /* Set up header with minimal info - will be updated when conversations refresh */
      gnalk_conversation_header_set_room (self->conversation_header, token, token, NULL, 0);
      gnalk_conversation_header_set_visible (self->conversation_header, TRUE);

      /* Try to load avatar even though we don't have conversation data yet */
      if (self->avatar_cache)
        gnalk_avatar_cache_get_room_avatar (self->avatar_cache, token, on_room_avatar_loaded, self);

      /* Room join via session */
      if (self->active_session)
        {
          g_message ("open_conversation_by_token: using session to join new room %s", token);
          gnalk_session_join_room (self->active_session, token);
        }

      /* Reload conversations from cache (controller keeps it fresh) */
      g_message ("open_conversation_by_token: reloading conversations");
      gnalk_window_load_conversations_from_db (self);
    }
}

/* ============ Restore Last Room ============ */

/* Try to restore last opened room if no room is currently open.
 * If room is already open (from immediate restore), update the header with full info.
 * Only in non-collapsed/desktop mode - in mobile mode, stay on room list. */
void
gnalk_window_maybe_restore_last_room (GnalkWindow *self)
{
  /* If room already open, just update header with full conversation info */
  if (self->current_room_token)
    {
      guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
      g_message ("maybe_restore_last_room: room %s already open, searching %u conversations",
                 self->current_room_token, n_items);
      for (guint i = 0; i < n_items; i++)
        {
          GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
          const char *token = gnalk_conversation_data_get_token (conv);
          if (g_strcmp0 (token, self->current_room_token) == 0)
            {
              const char *title = gnalk_conversation_data_get_title (conv);
              const char *description = gnalk_conversation_data_get_description (conv);
              gint room_type = gnalk_conversation_data_get_room_type (conv);

              /* Update room name and header with full info */
              g_free (self->current_room_name);
              g_free (self->current_room_description);
              self->current_room_name = g_strdup (title);
              self->current_room_description = g_strdup (description);
              self->current_room_type = room_type;

              g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_CURRENT_ROOM_NAME]);

              gnalk_conversation_header_set_room (self->conversation_header, token, title, description, room_type);
              gnalk_conversation_header_set_visible (self->conversation_header, TRUE);
              adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self->content_page), title ? title : _("Messages"));

              /* Load room avatar (was skipped during immediate restore) */
              if (self->avatar_cache)
                gnalk_avatar_cache_get_room_avatar (self->avatar_cache, token, on_room_avatar_loaded, self);

              /* Show participants toggle for group conversations */
              gboolean is_group = (room_type != 1);
              gtk_widget_set_visible (self->participants_toggle_button, is_group);

              /* Load participants only if not already loaded for this room.
               * This prevents HTTP fetch on every conversation update. */
              const char *active_room = self->participant_controller
                ? gnalk_participant_controller_get_active_room (self->participant_controller)
                : NULL;
              if (g_strcmp0 (active_room, self->current_room_token) != 0)
                gnalk_window_load_participants (self, self->current_room_token);

              /* Ensure session has joined room (enables call capability).
               * This handles the case where window opened with room already set
               * but session hasn't joined yet (e.g., service mode). */
              if (self->active_session && !gnalk_session_can_call (self->active_session))
                gnalk_session_join_room (self->active_session, self->current_room_token);

              g_object_unref (conv);
              return;
            }
          g_object_unref (conv);
        }
      g_message ("maybe_restore_last_room: room not found in conversations");
      return;
    }

  gboolean is_collapsed = adw_navigation_split_view_get_collapsed (ADW_NAVIGATION_SPLIT_VIEW (self->split_view));
  g_message ("Restore: is_collapsed=%d", is_collapsed);
  if (is_collapsed)
    return;

  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  GSettings *settings = gnalk_application_get_settings (app);
  g_autofree char *last_room = g_settings_get_string (settings, "last-room-token");
  g_message ("Restore: last_room=%s", last_room ? last_room : "(null)");

  if (!last_room || !*last_room)
    return;

  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
  for (guint i = 0; i < n_items; i++)
    {
      GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
      const char *token = gnalk_conversation_data_get_token (conv);
      if (g_strcmp0 (token, last_room) == 0)
        {
          const char *title = gnalk_conversation_data_get_title (conv);
          const char *description = gnalk_conversation_data_get_description (conv);
          gint room_type = gnalk_conversation_data_get_room_type (conv);

          g_message ("Restore: opening room %s", token);

          /* Update room state - UI only, no network */
          g_free (self->current_room_token);
          g_free (self->current_room_name);
          g_free (self->current_room_description);
          self->current_room_token = g_strdup (token);
          self->current_room_name = g_strdup (title);
          self->current_room_description = g_strdup (description);
          self->current_room_type = room_type;
          self->has_active_room = TRUE;
          gnalk_message_controller_set_room (self->message_controller, token);

          g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_CURRENT_ROOM_NAME]);
          g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_HAS_ACTIVE_ROOM]);

          /* Update conversation header */
          gnalk_conversation_header_set_room (self->conversation_header, token, title, description, room_type);
          gnalk_conversation_header_set_visible (self->conversation_header, TRUE);
          adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self->content_page), title ? title : _("Messages"));

          /* Show participants toggle for group conversations */
          {
            gboolean is_group = (room_type != 1);
            gtk_widget_set_visible (self->participants_toggle_button, is_group);
            if (is_group)
              {
                gboolean show_participants = g_settings_get_boolean (settings, "show-participants");
                gnalk_window_block_participants_toggle (self);
                gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->participants_toggle_button), show_participants);
                gnalk_window_unblock_participants_toggle (self);
              }
          }

          /* Load room avatar */
          if (self->avatar_cache)
            gnalk_avatar_cache_get_room_avatar (self->avatar_cache, token, on_room_avatar_loaded, self);

          /* Load messages from cache and show */
          gnalk_window_load_messages_from_db (self, token);
          gnalk_message_view_show_messages (self->message_view);

          /* Load participants from cache first, then fetch live */
          GnalkDatabase *db = gnalk_application_get_database (app);
          gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;
          if (db && account_id > 0 && room_type != 1)
            {
              JsonArray *cached_participants = gnalk_database_load_participants (db, account_id, token);
              if (cached_participants && json_array_get_length (cached_participants) > 0)
                {
                  const char *my_user_id = gnalk_application_get_user_id (app);
                  gint my_participant_type = gnalk_conversation_data_get_participant_type (conv);
                  gnalk_participants_list_set_participants (self->participants_list, cached_participants,
                                                              self->avatar_cache, NULL, my_user_id,
                                                              my_participant_type, room_type);
                  /* Show based on user preference */
                  gboolean show_participants = g_settings_get_boolean (settings, "show-participants");
                  gnalk_participants_list_set_visible (self->participants_list, show_participants);
                }
              if (cached_participants)
                json_array_unref (cached_participants);

              /* Fetch live participants to update the list */
              gnalk_window_load_participants (self, token);
            }

          /* Join room via session (enables call capability) */
          if (self->active_session)
            gnalk_session_join_room (self->active_session, token);

          g_object_unref (conv);
          return;
        }
      g_object_unref (conv);
    }
}
