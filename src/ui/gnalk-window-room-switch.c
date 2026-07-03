/* gnalk-window-room-switch.c
 *
 * Room switching UI handlers and room controller signal handlers.
 * Orchestration logic lives in GnalkRoomController; this file contains
 * the Window-side signal handlers that update GTK widgets.
 */

#include "gnalk-window-private.h"
#include "gnalk-application.h"
#include "gnalk-conversation-data.h"
#include "gnalk-thread-data.h"
#include "gnalk-log.h"
#include "gnalk-room-controller.h"
#include "gnalk-session.h"
#include "gnalk-message-view.h"
#include "gnalk-conversation-header.h"
#include "gnalk-typing-indicator.h"
#include "gnalk-participants-list.h"
#include "gnalk-avatar-cache.h"
#include "gnalk-room-tab-manager.h"
#include "gnalk-message-controller.h"
#include "gnalk-message-processor.h"
#include "gnalk-conversation-sidebar.h"
#include "gnalk-http-client.h"
#include "gnalk-call-window.h"
#include "gnalk-incoming-call-banner.h"
#include "gnalk-signaling.h"
#include "gnalk-conversation-actions.h"
#include "gnalk-message-input.h"
#include "gnalk-participant-controller.h"
#include <glib/gi18n.h>

/* ============ Scroll State Callback ============ */

/**
 * gnalk_window_get_scroll_state:
 *
 * Callback for GnalkRoomController to read the current scroll state
 * from the message view. Called before backgrounding a room tab.
 */
void
gnalk_window_get_scroll_state (gpointer  user_data,
                                 gint64   *anchor_id,
                                 gdouble  *anchor_offset,
                                 gboolean *at_bottom,
                                 int      *viewport_height,
                                 int      *viewport_width)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  *anchor_id = 0;
  *anchor_offset = 0.0;
  *at_bottom = TRUE;
  *viewport_height = 0;
  *viewport_width = 0;

  if (!self->message_view || !self->current_room)
    return;

  gnalk_message_view_get_viewport_size (self->message_view, viewport_width, viewport_height);
  *at_bottom = gnalk_message_view_is_at_bottom (self->message_view);

  GNALK_TRACE_SCROLL ("get_scroll_state: viewport %dx%d, at_bottom=%d",
                        *viewport_width, *viewport_height, *at_bottom);

  if (!*at_bottom)
    {
      guint first_visible = gnalk_message_view_get_first_visible_position (self->message_view);
      guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));

      if (first_visible < n_items)
        {
          GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (self->messages_store), first_visible);
          if (msg)
            {
              *anchor_id = gnalk_message_data_get_id (msg);
              g_object_unref (msg);
            }
        }

      *anchor_offset = gnalk_message_view_get_scroll_position (self->message_view);
      GNALK_TRACE_SCROLL ("get_scroll_state: anchor_id=%" G_GINT64_FORMAT ", offset=%.1f",
                            *anchor_id, *anchor_offset);
    }
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

/* ============ Mosaic Background ============ */

/* Context for collecting avatars asynchronously */
typedef struct {
  GWeakRef     window_ref;    /* Weak reference to GnalkWindow */
  GPtrArray   *textures;      /* Participant avatars */
  GdkTexture  *room_avatar;   /* Room avatar (nullable) */
  guint        pending;
  guint        total;
} MosaicContext;

static void
mosaic_context_finish (MosaicContext *ctx)
{
  GnalkWindow *window = g_weak_ref_get (&ctx->window_ref);

  if (window)
    {
      if ((ctx->textures->len > 0 || ctx->room_avatar) && window->message_view)
        gnalk_message_view_set_mosaic_avatars (window->message_view,
                                                ctx->textures,
                                                ctx->room_avatar);
      g_object_unref (window);
    }

  g_weak_ref_clear (&ctx->window_ref);
  g_ptr_array_unref (ctx->textures);
  g_clear_object (&ctx->room_avatar);
  g_free (ctx);
}

static void
on_mosaic_avatar_loaded (GnalkAvatarCache *cache G_GNUC_UNUSED,
                          GdkTexture       *texture,
                          gpointer          user_data)
{
  MosaicContext *ctx = user_data;

  if (texture)
    g_ptr_array_add (ctx->textures, g_object_ref (texture));

  ctx->pending--;

  if (ctx->pending == 0)
    mosaic_context_finish (ctx);
}

static void
on_mosaic_room_avatar_loaded (GnalkAvatarCache *cache G_GNUC_UNUSED,
                               GdkTexture       *texture,
                               gpointer          user_data)
{
  MosaicContext *ctx = user_data;

  if (texture)
    ctx->room_avatar = g_object_ref (texture);

  ctx->pending--;

  if (ctx->pending == 0)
    mosaic_context_finish (ctx);
}

void
gnalk_window_update_mosaic_background (GnalkWindow *self, JsonArray *participants)
{
  if (!participants || !self->avatar_cache)
    return;

  /* Check if mosaic background is enabled */
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  GSettings *settings = gnalk_application_get_settings (app);
  gint bg_style = g_settings_get_int (settings, "conversation-background");

  /* 13 = Mosaic */
  if (bg_style != 13)
    return;

  guint n = json_array_get_length (participants);
  if (n == 0)
    return;

  /* First pass: count valid participants (to avoid use-after-free when
   * callbacks fire synchronously for cached avatars) */
  guint valid_count = 0;
  for (guint i = 0; i < n; i++)
    {
      JsonObject *p = json_array_get_object_element (participants, i);
      const char *actor_id = json_object_get_string_member_with_default (p, "actorId", "");
      if (actor_id && *actor_id)
        valid_count++;
    }

  if (valid_count == 0)
    return;

  /* Create context for async avatar collection */
  MosaicContext *ctx = g_new0 (MosaicContext, 1);
  g_weak_ref_init (&ctx->window_ref, self);
  ctx->textures = g_ptr_array_new_with_free_func (g_object_unref);
  ctx->room_avatar = NULL;
  ctx->pending = valid_count + 1;  /* +1 for room avatar */
  ctx->total = valid_count + 1;

  /* Fetch room avatar */
  if (self->current_room->room_token)
    gnalk_avatar_cache_get_room_avatar (self->avatar_cache, self->current_room->room_token,
                                         on_mosaic_room_avatar_loaded, ctx);
  else
    ctx->pending--;  /* No room token, skip room avatar */

  /* Second pass: request participant avatars (callbacks may fire synchronously).
   * Note: Do NOT access ctx after this loop - if all callbacks fire synchronously,
   * the last one will free ctx via mosaic_context_finish. */
  for (guint i = 0; i < n; i++)
    {
      JsonObject *p = json_array_get_object_element (participants, i);
      const char *actor_id = json_object_get_string_member_with_default (p, "actorId", "");

      if (actor_id && *actor_id)
        gnalk_avatar_cache_get_user_avatar (self->avatar_cache, actor_id, 64,
                                             on_mosaic_avatar_loaded, ctx);
    }
}

/* ============ Load Participants ============ */

/* Load and display participants for a room.
 * Pre-configures widgets, then delegates to ParticipantController which
 * loads from DB cache (immediate signal) and fetches HTTP (signal if changed).
 * The on_participant_controller_changed handler does all widget population. */
void
gnalk_window_load_participants (GnalkWindow *self, const char *token)
{
  gnalk_conversation_header_set_call_active (self->conversation_header, FALSE);

  /* Hide participants bar for 1:1 conversations */
  if (self->current_room->room_type == 1)
    gnalk_participants_list_hide (self->participants_list);

  /* Guests cannot fetch participant list (403 Forbidden) */
  if (self->active_session && gnalk_session_is_guest (self->active_session))
    {
      gnalk_participants_list_hide (self->participants_list);
      return;
    }

  gnalk_participants_list_clear (self->participants_list);

  /* Controller loads from cache (emits immediate signal) then fetches HTTP */
  if (self->participant_controller)
    gnalk_participant_controller_set_active_room (self->participant_controller, token, self->current_room->room_type);
}

/* ============ Room Controller Signal Handlers ============ */

/**
 * on_room_controller_room_closed:
 *
 * Handles cleanup when leaving a room (either switching or closing).
 * At this point, the controller's current_room still points to the old room.
 */
void
gnalk_window_on_room_controller_room_closed (GnalkRoomController *ctrl,
                                               const char          *old_token,
                                               GnalkWindow         *self)
{
  (void) ctrl;
  (void) old_token;

  /* Restore from thread/context mode if active before saving */
  if (self->current_room && self->current_room->in_thread_mode)
    {
      gnalk_message_processor_restore_from_thread (self->message_processor);
      gnalk_message_view_set_thread_mode (self->message_view, FALSE, NULL, NULL);
    }
  if (self->current_room && self->current_room->in_context_mode)
    {
      gnalk_message_processor_restore_live (self->message_processor);
      gnalk_message_view_set_context_mode (self->message_view, FALSE);
    }

  /* Save messages for the old room before switching */
  gnalk_window_save_messages_to_db (self);

  /* Stop typing timer (typing signaling already sent by controller) */
  if (self->typing_send_timer_id > 0)
    {
      g_source_remove (self->typing_send_timer_id);
      self->typing_send_timer_id = 0;
    }
  gnalk_typing_indicator_clear (self->typing_indicator);

  /* Clear read status tracking */
  gnalk_window_clear_read_status_tracking (self);

  /* Clear federation signaling when leaving a room */
  if (self->active_session)
    gnalk_session_clear_federation_signaling (self->active_session);
}

/**
 * on_room_controller_messages_store_changed:
 *
 * Rebinds the GtkNoSelection model when the active message store changes.
 */
void
gnalk_window_on_room_controller_messages_store_changed (GnalkRoomController *ctrl,
                                                          GListStore          *store,
                                                          GnalkWindow         *self)
{
  (void) ctrl;

  /* Update the Window's convenience pointer */
  gtk_no_selection_set_model (self->messages_selection, NULL);
  g_set_object (&self->messages_store, store);
  gtk_no_selection_set_model (self->messages_selection, G_LIST_MODEL (self->messages_store));

  GNALK_TRACE_UI ("messages_store_changed: reconnected model with %u items",
                    g_list_model_get_n_items (G_LIST_MODEL (self->messages_store)));
}

/**
 * on_room_controller_room_opened:
 *
 * Main UI update handler when a room is opened. Handles both the
 * "conversation found" and "conversation not found" cases.
 */
void
gnalk_window_on_room_controller_room_opened (GnalkRoomController   *ctrl,
                                               const char            *token,
                                               const char            *old_token,
                                               GnalkConversationData *conv_data,
                                               GnalkWindow           *self)
{
  /* Sync Window's state from controller */
  self->current_room = gnalk_room_controller_get_current_room (ctrl);
  self->has_active_room = TRUE;
  self->cached_room_is_federated = FALSE;

  /* Update processor's room state pointer */
  gnalk_message_processor_set_room_state (self->message_processor, self->current_room);

  /* Update conversation row highlights */
  gnalk_window_update_conversation_active_states (self, old_token, token);

  gnalk_message_view_set_loading_older (self->message_view, FALSE);

  g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_CURRENT_ROOM_NAME]);
  g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_HAS_ACTIVE_ROOM]);

  if (conv_data)
    {
      const char *title = gnalk_conversation_data_get_title (conv_data);
      const char *description = gnalk_conversation_data_get_description (conv_data);
      gint room_type = gnalk_conversation_data_get_room_type (conv_data);
      const char *user_status = gnalk_conversation_data_get_user_status (conv_data);

      /* Check if this is a pending federation invitation */
      if (gnalk_conversation_data_is_invitation (conv_data) ||
          (self->pending_invitations &&
           g_hash_table_contains (self->pending_invitations, token)))
        {
          /* Update header minimally */
          gnalk_conversation_header_set_room (self->conversation_header, token, title, description, room_type);
          gnalk_conversation_header_set_visible (self->conversation_header, TRUE);
          adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self->content_page), title ? title : _("Invitation"));

          gnalk_window_show_invitation_view (self, token);
          adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->split_view), TRUE);

          /* Track ConversationData for live updates */
          gnalk_window_track_conv_data_for_status (self, conv_data);
          return;
        }

      /* Normal conversation - show messages view */
      gnalk_window_show_messages_view (self);

      /* Update conversation header */
      gnalk_conversation_header_set_room (self->conversation_header, token, title, description, room_type);
      gnalk_conversation_header_set_visible (self->conversation_header, TRUE);
      adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self->content_page), title ? title : _("Messages"));

      gnalk_avatar_cache_get_room_avatar (self->avatar_cache, token, on_room_avatar_loaded, self);

      gnalk_window_update_call_button_state (self);
      gnalk_window_update_room_status_icon (self, room_type, user_status);

      /* Cache federation status for the current room (disables threads, etc.) */
      self->cached_room_is_federated = gnalk_conversation_data_is_federated (conv_data);

      /* For federated rooms with federation-v2, fetch federation signaling settings.
       * This prepares the federation signaling connection needed for calls. */
      if (self->cached_room_is_federated &&
          self->active_session &&
          gnalk_session_has_federation_v2 (self->active_session))
        {
          gnalk_session_fetch_federation_signaling (self->active_session, token);
        }

      /* Show participants toggle for group conversations (hidden in guest mode by setup_guest_mode) */
      if (!self->is_guest_mode)
        {
          GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
          GSettings *settings = gnalk_application_get_settings (app);
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

      /* If tab has no messages yet, load from DB */
      guint tab_msg_count = g_list_model_get_n_items (G_LIST_MODEL (self->current_room->messages_store));
      if (!self->current_room->initial_load_done || tab_msg_count == 0)
        {
          if (self->current_room->initial_load_done && tab_msg_count == 0)
            {
              GNALK_TRACE_ROOM_SWITCH ("room_opened: tab %s claims loaded but has 0 msgs, reloading", token);
              self->current_room->initial_load_done = FALSE;
            }
          gnalk_window_load_messages_from_db (self, token);
        }
      /* else: controller already emitted scroll-requested and started polling */

      adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->split_view), TRUE);

      /* Track ConversationData for live user status updates */
      gnalk_window_track_conv_data_for_status (self, conv_data);

      /* Load participants (can happen in parallel) */
      gnalk_window_load_participants (self, token);
    }
  else
    {
      /* Conversation not in our list yet — safety net for timing (data-changed
       * may not have fired yet for a freshly saved guest room). */
      gnalk_window_show_messages_view (self);

      gnalk_conversation_header_set_room (self->conversation_header, token, token, NULL, 0);
      gnalk_conversation_header_set_visible (self->conversation_header, TRUE);

      if (self->avatar_cache)
        gnalk_avatar_cache_get_room_avatar (self->avatar_cache, token, on_room_avatar_loaded, self);

      gnalk_window_update_call_button_state (self);

      /* If tab has no messages yet, load from DB */
      guint tab_msg_count = g_list_model_get_n_items (G_LIST_MODEL (self->current_room->messages_store));
      if (!self->current_room->initial_load_done || tab_msg_count == 0)
        {
          if (self->current_room->initial_load_done && tab_msg_count == 0)
            self->current_room->initial_load_done = FALSE;
          gnalk_window_load_messages_from_db (self, token);
        }

      adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->split_view), TRUE);

      /* Load participants */
      gnalk_window_load_participants (self, token);

      /* DataRepository::data-changed will reload conversation list when sync saves this room */
    }

  /* If we navigated here to accept an incoming call, start it now */
  if (self->pending_call_with_video >= 0)
    {
      gboolean with_video = (self->pending_call_with_video == 1);
      self->pending_call_with_video = -1;
      gnalk_window_start_call (self, with_video);
    }

  /* If we navigated here to open a thread, enter thread mode now */
  if (self->pending_thread_id > 0)
    {
      gint64 tid = self->pending_thread_id;
      self->pending_thread_id = 0;

      /* Set thread mode early so poll responses are skipped while HTTP is in flight */
      if (self->current_room)
        {
          self->current_room->in_thread_mode = TRUE;
          self->current_room->thread_root_id = tid;
        }

      if (self->message_controller)
        gnalk_message_controller_load_thread (self->message_controller, tid);
    }
}

/**
 * on_room_controller_scroll_requested:
 *
 * Scrolls the message view to the requested position.
 */
void
gnalk_window_on_room_controller_scroll_requested (GnalkRoomController *ctrl,
                                                    guint                position,
                                                    GnalkWindow         *self)
{
  (void) ctrl;

  GNALK_TRACE_SCROLL ("scroll_requested: scrolling to position %u", position);
  gnalk_message_view_show_messages_at_item (self->message_view, position);
}

/* ============ Sidebar Selection ============ */

void
gnalk_window_on_sidebar_conversation_selected (GnalkConversationSidebar *sidebar,
                                                 guint                      position,
                                                 GnalkWindow              *self)
{
  (void) sidebar;
  GtkTreeListRow *tree_row = g_list_model_get_item (
      G_LIST_MODEL (self->conversations_tree_model), position);

  if (!tree_row)
    return;

  gpointer real_item = gtk_tree_list_row_get_item (tree_row);
  guint depth = gtk_tree_list_row_get_depth (tree_row);

  if (depth == 1 && GNALK_IS_THREAD_DATA (real_item))
    {
      /* Thread row clicked - open the parent conversation and enter thread mode */
      GnalkThreadData *thread = GNALK_THREAD_DATA (real_item);
      const char *token = gnalk_thread_data_get_room_token (thread);
      gint64 thread_id = gnalk_thread_data_get_thread_id (thread);

      /* Check if we're already in this room */
      if (self->current_room && g_strcmp0 (token, self->current_room->room_token) == 0)
        {
          /* Set thread mode early so poll responses are skipped while HTTP is in flight */
          self->current_room->in_thread_mode = TRUE;
          self->current_room->thread_root_id = thread_id;

          /* Already in the room - load thread messages */
          if (self->message_controller)
            gnalk_message_controller_load_thread (self->message_controller, thread_id);
        }
      else
        {
          /* Store pending thread and open the room */
          self->pending_thread_id = thread_id;
          gnalk_room_controller_open_room_debounced (self->room_controller, token);
        }
    }
  else if (GNALK_IS_CONVERSATION_DATA (real_item))
    {
      GnalkConversationData *data = GNALK_CONVERSATION_DATA (real_item);
      const char *token = gnalk_conversation_data_get_token (data);

      /* If already in this room and in thread mode, exit thread mode */
      if (self->current_room &&
          g_strcmp0 (token, self->current_room->room_token) == 0 &&
          self->current_room->in_thread_mode)
        {
          gnalk_message_processor_restore_from_thread (self->message_processor);
          gnalk_message_view_set_thread_mode (self->message_view, FALSE, NULL, NULL);
        }
      else
        {
          /* Delegate debounced open to the room controller */
          gnalk_room_controller_open_room_debounced (self->room_controller, token);
        }
    }

  g_object_unref (real_item);
  g_object_unref (tree_row);
}

/* ============ Content Page Hiding (Leave Room) ============ */

void
gnalk_window_on_content_page_hiding (AdwNavigationPage *page,
                                       gpointer           user_data)
{
  (void) page;
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (!self->current_room)
    return;

  g_autofree char *old_token = g_strdup (self->current_room->room_token);

  /* Delegate close to the room controller.
   * This will: background tab, stop typing, emit room-closed
   * (handler saves messages + clears typing timer),
   * leave signaling/HTTP, clear current_room. */
  gnalk_room_controller_close_current (self->room_controller);

  /* Disconnect the model from the view */
  gtk_no_selection_set_model (self->messages_selection, NULL);

  /* Disconnect ConversationData status tracking */
  gnalk_window_track_conv_data_for_status (self, NULL);

  /* Cancel any pending highlight timer before current_room is nulled;
   * its handler dereferences current_room. */
  g_clear_handle_id (&self->highlight_clear_timer_id, g_source_remove);

  /* Sync Window state from controller */
  self->current_room = NULL;
  self->has_active_room = FALSE;
  gnalk_message_processor_set_room_state (self->message_processor, NULL);
  g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_HAS_ACTIVE_ROOM]);

  /* Clear active room in application so notifications work */
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  if (app)
    gnalk_application_set_active_room (app, NULL);

  /* Update conversation row highlight */
  gnalk_window_update_conversation_active_states (self, old_token, NULL);
}

/* ============ Open Conversation (thin wrapper) ============ */

void
gnalk_window_open_conversation_by_token (GnalkWindow *self, const char *token)
{
  gnalk_room_controller_open_room (self->room_controller, token);
}

/* ============ Restore Last Room ============ */

/**
 * update_open_room_info:
 *
 * When the room is already open (from immediate restore at startup),
 * updates the header with full info from conversation data.
 * This is pure UI work - the controller doesn't need to be involved.
 */
static void
update_open_room_info (GnalkWindow *self)
{
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
  g_message ("update_open_room_info: room %s, searching %u conversations",
             self->current_room->room_token, n_items);

  for (guint i = 0; i < n_items; i++)
    {
      GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
      const char *token = gnalk_conversation_data_get_token (conv);
      if (g_strcmp0 (token, self->current_room->room_token) == 0)
        {
          const char *title = gnalk_conversation_data_get_title (conv);
          const char *description = gnalk_conversation_data_get_description (conv);
          gint room_type = gnalk_conversation_data_get_room_type (conv);

          /* Update room state with full info */
          g_free (self->current_room->room_name);
          g_free (self->current_room->room_description);
          self->current_room->room_name = g_strdup (title);
          self->current_room->room_description = g_strdup (description);
          self->current_room->room_type = room_type;

          g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_CURRENT_ROOM_NAME]);

          gnalk_conversation_header_set_room (self->conversation_header, token, title, description, room_type);
          gnalk_conversation_header_set_visible (self->conversation_header, TRUE);
          adw_navigation_page_set_title (ADW_NAVIGATION_PAGE (self->content_page), title ? title : _("Messages"));

          /* Load room avatar (was skipped during immediate restore) */
          if (self->avatar_cache)
            gnalk_avatar_cache_get_room_avatar (self->avatar_cache, token, on_room_avatar_loaded, self);

          /* Show participants toggle for group conversations (hidden in guest mode by setup_guest_mode) */
          if (!self->is_guest_mode)
            gtk_widget_set_visible (self->participants_toggle_button, room_type != 1);

          /* Load participants only if not already loaded for this room */
          const char *active_room = self->participant_controller
            ? gnalk_participant_controller_get_active_room (self->participant_controller)
            : NULL;
          if (g_strcmp0 (active_room, self->current_room->room_token) != 0)
            gnalk_window_load_participants (self, self->current_room->room_token);

          /* Ensure session has joined room (enables call capability) */
          if (self->active_session && !gnalk_session_can_call (self->active_session))
            gnalk_session_join_room (self->active_session, self->current_room->room_token);

          g_object_unref (conv);
          return;
        }
      g_object_unref (conv);
    }
  g_message ("update_open_room_info: room not found in conversations");
}

void
gnalk_window_maybe_restore_last_room (GnalkWindow *self)
{
  /* If room already open, just update header with full info from conversation data */
  if (self->current_room)
    {
      update_open_room_info (self);
      return;
    }

  gboolean is_collapsed = adw_navigation_split_view_get_collapsed (ADW_NAVIGATION_SPLIT_VIEW (self->split_view));
  gnalk_room_controller_restore_last_room (self->room_controller, is_collapsed);
}

/* ============ Federation Invitation View ============ */

/**
 * gnalk_window_on_invitations_updated:
 *
 * Stores pending invitations indexed by local token for quick lookup
 * when opening rooms. Saves invitations to DB so they persist across
 * conversation list reloads (no more flickering).
 */
void
gnalk_window_on_invitations_updated (GnalkConversationController *controller,
                                      JsonArray                   *invitations,
                                      GnalkWindow                 *self)
{
  (void) controller;

  if (!self->pending_invitations)
    self->pending_invitations = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                        g_free,
                                                        (GDestroyNotify) json_object_unref);

  /* Build set of new pending invitation tokens */
  g_autoptr(GHashTable) new_tokens = g_hash_table_new (g_str_hash, g_str_equal);

  if (invitations)
    {
      guint n = json_array_get_length (invitations);
      for (guint i = 0; i < n; i++)
        {
          JsonObject *inv = json_array_get_object_element (invitations, i);
          gint64 state = json_object_get_int_member_with_default (inv, "state", -1);

          /* Only store pending invitations (state=0) */
          if (state != 0)
            continue;

          const char *local_token = json_object_get_string_member_with_default (inv, "localToken", "");
          if (local_token && *local_token)
            g_hash_table_add (new_tokens, (gpointer) local_token);
        }
    }

  /* Remove invitations from DB that are no longer pending (accepted/declined elsewhere) */
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  GnalkDataRepository *repo = app ? gnalk_application_get_data_repository (app) : NULL;
  gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;

  if (repo && account_id > 0)
    {
      GHashTableIter old_iter;
      gpointer old_key, old_value;
      g_hash_table_iter_init (&old_iter, self->pending_invitations);
      while (g_hash_table_iter_next (&old_iter, &old_key, &old_value))
        {
          const char *old_token = old_key;
          if (!g_hash_table_contains (new_tokens, old_token))
            {
              gnalk_data_repository_delete_conversation (repo, account_id, old_token);
              g_debug ("Window: Removed stale invitation from DB: %s", old_token);
            }
        }
    }

  /* Update pending_invitations hash table */
  g_hash_table_remove_all (self->pending_invitations);

  if (!invitations)
    return;

  gboolean saved_any = FALSE;
  guint n = json_array_get_length (invitations);
  for (guint i = 0; i < n; i++)
    {
      JsonObject *inv = json_array_get_object_element (invitations, i);
      gint64 state = json_object_get_int_member_with_default (inv, "state", -1);

      if (state != 0)
        continue;

      const char *local_token = json_object_get_string_member_with_default (inv, "localToken", "");
      if (!local_token || !*local_token)
        continue;

      g_hash_table_insert (self->pending_invitations,
                           g_strdup (local_token),
                           json_object_ref (inv));

      /* Save invitation as a conversation in DB */
      if (repo && account_id > 0)
        {
          const char *room_name = json_object_get_string_member_with_default (inv, "roomName", _("Federation invitation"));
          const char *remote_server = json_object_get_string_member_with_default (inv, "remoteServerUrl", "");
          const char *remote_token = json_object_get_string_member_with_default (inv, "remoteToken", "");
          const char *inviter = json_object_get_string_member_with_default (inv, "inviterDisplayName", "");
          gint64 inv_id = json_object_get_int_member_with_default (inv, "id", 1);

          g_autofree char *subtitle = g_strdup_printf (_("Invitation from %s"), inviter);

          GnalkConversationDataInit init = {
            .token                   = local_token,
            .title                   = room_name,
            .name                    = room_name,
            .room_type               = 2,  /* group */
            .last_activity           = g_get_real_time () / G_USEC_PER_SEC,
            .description             = subtitle,
            .last_message_text       = subtitle,
            .last_message_actor_name = inviter,
            .remote_server           = remote_server,
            .remote_token            = remote_token,
            .invitation_id           = inv_id,
            .session                 = self->active_session,
          };
          GnalkConversationData *conv = gnalk_conversation_data_new_from_db (&init);

          gnalk_data_repository_save_conversation (repo, account_id, conv);
          g_object_unref (conv);
          saved_any = TRUE;

          g_debug ("Window: Saved invitation to DB: token=%s name=%s from=%s",
                   local_token, room_name, inviter);
        }
    }

  g_message ("Window: Stored %u pending federation invitations",
             g_hash_table_size (self->pending_invitations));

  /* data-changed from save_conversation will trigger a debounced reload,
   * but if we saved multiple invitations we only need one reload. */
  if (saved_any)
    gnalk_window_load_conversations_from_db (self);
}

/**
 * gnalk_window_show_invitation_view:
 *
 * Switches the content stack to the invitation page and populates
 * it with the invitation details for the given room token.
 */
void
gnalk_window_show_invitation_view (GnalkWindow *self,
                                    const char  *room_token)
{
  if (!self->pending_invitations || !room_token)
    return;

  JsonObject *inv = g_hash_table_lookup (self->pending_invitations, room_token);
  if (!inv)
    return;

  gint64 invitation_id = json_object_get_int_member_with_default (inv, "id", 0);
  const char *room_name = json_object_get_string_member_with_default (inv, "roomName", "");
  const char *inviter_display_name = json_object_get_string_member_with_default (inv, "inviterDisplayName", "");
  const char *inviter_cloud_id = json_object_get_string_member_with_default (inv, "inviterCloudId", "");
  const char *remote_server_url = json_object_get_string_member_with_default (inv, "remoteServerUrl", "");

  self->pending_invitation_id = invitation_id;

  /* Update the status page */
  adw_status_page_set_title (ADW_STATUS_PAGE (self->invitation_status_page),
                             (room_name && *room_name) ? room_name : _("Federation Invitation"));

  /* Build details text */
  g_autofree char *details = NULL;
  if (inviter_display_name && *inviter_display_name)
    details = g_strdup_printf (_("Invited by %s (%s)\nServer: %s"),
                               inviter_display_name, inviter_cloud_id, remote_server_url);
  else
    details = g_strdup_printf (_("Invited from %s"), remote_server_url);

  gtk_label_set_text (GTK_LABEL (self->invitation_details_label), details);

  /* Ensure buttons are sensitive */
  gtk_widget_set_sensitive (self->invitation_accept_button, TRUE);
  gtk_widget_set_sensitive (self->invitation_decline_button, TRUE);

  /* Switch to invitation page */
  gtk_stack_set_visible_child_name (GTK_STACK (self->content_stack), "invitation");
}

/**
 * gnalk_window_show_messages_view:
 *
 * Switches the content stack back to the messages page.
 */
void
gnalk_window_show_messages_view (GnalkWindow *self)
{
  gtk_stack_set_visible_child_name (GTK_STACK (self->content_stack), "messages");
  self->pending_invitation_id = 0;
}

/**
 * gnalk_window_on_invitation_accepted:
 *
 * Handles successful invitation acceptance.
 * Reloads conversations and switches to message view.
 */
void
gnalk_window_on_invitation_accepted (GnalkConversationController *controller,
                                      gint64                       invitation_id,
                                      GnalkWindow                 *self)
{
  (void) controller;

  g_debug ("Window: Invitation %" G_GINT64_FORMAT " accepted", invitation_id);

  /* Remove the invitation from our local cache and from DB */
  if (self->pending_invitations)
    {
      GHashTableIter iter;
      gpointer key, value;
      g_hash_table_iter_init (&iter, self->pending_invitations);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          JsonObject *inv = value;
          if (json_object_get_int_member_with_default (inv, "id", 0) == invitation_id)
            {
              const char *token = key;

              /* Delete invitation entry from DB */
              GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
              GnalkDataRepository *repo = app ? gnalk_application_get_data_repository (app) : NULL;
              gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;
              if (repo && account_id > 0)
                gnalk_data_repository_delete_conversation (repo, account_id, token);

              g_hash_table_iter_remove (&iter);
              break;
            }
        }
    }

  /* Switch back to messages view */
  gnalk_window_show_messages_view (self);

  /* Re-join the room: the initial join 404'd because the invitation
   * wasn't accepted yet. Now that it is, the room exists and we can join. */
  if (self->current_room && self->active_session)
    {
      const char *room_token = self->current_room->room_token;
      gnalk_session_join_room (self->active_session, room_token);

      /* Start message polling (room had no messages loaded) */
      if (self->message_controller)
        gnalk_message_controller_start_polling (self->message_controller, 0);

      /* Enable unread tracking: invitation rooms start with last_read_message_id=0
       * (no read state), which prevents the message processor from marking anything
       * as unread. Set to 1 so incoming messages will trigger mark-as-read flow. */
      self->current_room->last_read_message_id = 1;
    }

  /* Reload conversations to pick up the now-accepted conversation */
  gnalk_window_load_conversations_from_db (self);

  /* Show a toast */
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay),
                               adw_toast_new (_("Invitation accepted")));
}

/**
 * gnalk_window_on_invitation_declined:
 *
 * Handles successful invitation decline.
 * Closes the room and reloads conversations.
 */
void
gnalk_window_on_invitation_declined (GnalkConversationController *controller,
                                      gint64                       invitation_id,
                                      GnalkWindow                 *self)
{
  (void) controller;

  g_debug ("Window: Invitation %" G_GINT64_FORMAT " declined", invitation_id);

  /* Remove from cache and from DB */
  if (self->pending_invitations)
    {
      GHashTableIter iter;
      gpointer key, value;
      g_hash_table_iter_init (&iter, self->pending_invitations);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          JsonObject *inv = value;
          if (json_object_get_int_member_with_default (inv, "id", 0) == invitation_id)
            {
              const char *token = key;

              /* Delete invitation entry from DB */
              GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
              GnalkDataRepository *repo = app ? gnalk_application_get_data_repository (app) : NULL;
              gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;
              if (repo && account_id > 0)
                gnalk_data_repository_delete_conversation (repo, account_id, token);

              g_hash_table_iter_remove (&iter);
              break;
            }
        }
    }

  /* Close the current room */
  gnalk_window_on_content_page_hiding (ADW_NAVIGATION_PAGE (self->content_page), self);

  /* Reload conversations */
  gnalk_window_load_conversations_from_db (self);

  /* Show a toast */
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay),
                               adw_toast_new (_("Invitation declined")));
}
