/* gnalk-room-controller.c
 *
 * Room open/close orchestration. Manages tab switching, debouncing,
 * room join via session, and emits signals for the Window to update UI.
 */

#include "gnalk-room-controller.h"
#include "gnalk-room-tab-manager.h"
#include "gnalk-session.h"
#include "gnalk-message-controller.h"
#include "gnalk-participant-controller.h"
#include "gnalk-conversation-data.h"
#include "gnalk-signaling.h"
#include "gnalk-http-client.h"
#include "gnalk-message-data.h"
#include "gnalk-log.h"

#define ROOM_SWITCH_DEBOUNCE_MS 300

struct _GnalkRoomController
{
  GObject parent_instance;

  /* Borrowed pointers (not owned) */
  GnalkRoomTabManager        *tab_manager;
  GListStore                 *conversations_store;
  GSettings                  *settings;
  GnalkMessageController     *message_controller;
  GnalkParticipantController *participant_controller;
  GnalkSession               *active_session;

  /* Current room state */
  GnalkRoomState *current_room;   /* Points to active tab's state (not owned) */
  GListStore     *messages_store; /* Convenience ptr, ref held */
  gboolean        has_active_room;

  /* Debounce */
  guint  debounce_timer_id;
  char  *pending_room_token;

  /* Scroll state callback (reads UI scroll info) */
  GnalkScrollStateFunc scroll_state_func;
  gpointer             scroll_state_data;
};

enum {
  SIGNAL_ROOM_OPENED,
  SIGNAL_ROOM_CLOSED,
  SIGNAL_MESSAGES_STORE_CHANGED,
  SIGNAL_SCROLL_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (GnalkRoomController, gnalk_room_controller, G_TYPE_OBJECT)

/* ============ Internal helpers ============ */

static GnalkConversationData *
find_conversation_by_token (GnalkRoomController *self, const char *token)
{
  if (!self->conversations_store)
    return NULL;

  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
  for (guint i = 0; i < n; i++)
    {
      GnalkConversationData *data = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
      const char *data_token = gnalk_conversation_data_get_token (data);
      if (g_strcmp0 (data_token, token) == 0)
        return data;  /* caller takes ownership of ref */
      g_object_unref (data);
    }

  return NULL;
}

/**
 * background_current_internal:
 *
 * Backgrounds the current room tab by saving scroll state via the
 * scroll_state_func callback and calling the tab manager.
 */
static void
background_current_internal (GnalkRoomController *self)
{
  if (!self->current_room || !self->tab_manager)
    return;

  GNALK_TRACE_ROOM_SWITCH ("room_controller: backgrounding room %s",
                             self->current_room->room_token);

  gint64 anchor_id = 0;
  gdouble anchor_offset = 0.0;
  gboolean at_bottom = TRUE;
  int viewport_height = 0, viewport_width = 0;

  if (self->scroll_state_func)
    self->scroll_state_func (self->scroll_state_data,
                              &anchor_id, &anchor_offset, &at_bottom,
                              &viewport_height, &viewport_width);

  gnalk_room_tab_manager_background_tab (self->tab_manager,
                                           self->current_room->room_token,
                                           anchor_id, anchor_offset, at_bottom,
                                           viewport_height, viewport_width);

  g_debug ("room_controller: backgrounded %s: anchor=%" G_GINT64_FORMAT ", at_bottom=%d",
           self->current_room->room_token, anchor_id, at_bottom);
}

/**
 * stop_typing_internal:
 *
 * Sends not-typing signaling message if the current room has typing active.
 */
static void
stop_typing_internal (GnalkRoomController *self)
{
  if (!self->current_room || !self->current_room->is_typing)
    return;

  if (self->active_session)
    {
      GnalkSignaling *sig = gnalk_session_get_signaling (self->active_session);
      if (sig)
        gnalk_signaling_send_typing (sig, FALSE);
    }
  self->current_room->is_typing = FALSE;
}

/**
 * find_scroll_position:
 *
 * Determines which item index to scroll to when restoring a tab
 * that already has messages.
 */
static guint
find_scroll_position (GnalkRoomController *self)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->current_room->messages_store));
  if (n == 0)
    return 0;

  if (self->current_room->is_at_bottom)
    return n - 1;

  if (self->current_room->anchor_message_id > 0)
    {
      for (guint i = 0; i < n; i++)
        {
          GnalkMessageData *msg = g_list_model_get_item (
              G_LIST_MODEL (self->current_room->messages_store), i);
          if (msg)
            {
              gint64 msg_id = gnalk_message_data_get_id (msg);
              g_object_unref (msg);
              if (msg_id == self->current_room->anchor_message_id)
                return i;
            }
        }
    }

  return n - 1;  /* fallback: bottom */
}

/**
 * update_room_state_from_conv:
 *
 * Updates the current room state fields from conversation data.
 */
static void
update_room_state_from_conv (GnalkRoomController   *self,
                               GnalkConversationData *conv_data)
{
  g_free (self->current_room->room_name);
  g_free (self->current_room->room_description);
  self->current_room->room_name = g_strdup (gnalk_conversation_data_get_title (conv_data));
  self->current_room->room_description = g_strdup (gnalk_conversation_data_get_description (conv_data));
  self->current_room->room_type = gnalk_conversation_data_get_room_type (conv_data);

  /* Read status */
  self->current_room->last_common_read_message =
      gnalk_conversation_data_get_last_common_read_message (conv_data);
  self->current_room->last_read_message_id =
      gnalk_conversation_data_get_last_read_message (conv_data);
  self->current_room->first_unread_message_id = 0;

  /* Reset per-entry state */
  self->current_room->message_load_generation++;
  self->current_room->scroll_to_bottom = TRUE;
  self->current_room->skip_historical_call_messages = TRUE;
  g_clear_pointer (&self->current_room->session_id, g_free);
}

/**
 * join_room_via_session:
 *
 * Joins the room using the appropriate session (conversation's session
 * or the active session as fallback).
 */
static void
join_room_via_session (GnalkRoomController   *self,
                         GnalkConversationData *conv_data,
                         const char            *token)
{
  GnalkSession *session = NULL;

  if (conv_data)
    session = gnalk_conversation_data_get_session (conv_data);
  if (!session)
    session = self->active_session;

  if (session)
    {
      g_message ("room_controller: joining room %s via session", token);
      gnalk_session_join_room (session, token);
    }
}

/* ============ GObject boilerplate ============ */

static void
gnalk_room_controller_finalize (GObject *object)
{
  GnalkRoomController *self = GNALK_ROOM_CONTROLLER (object);

  if (self->debounce_timer_id > 0)
    g_source_remove (self->debounce_timer_id);

  g_free (self->pending_room_token);
  g_clear_object (&self->messages_store);

  G_OBJECT_CLASS (gnalk_room_controller_parent_class)->finalize (object);
}

static void
gnalk_room_controller_init (GnalkRoomController *self)
{
  (void) self;
}

static void
gnalk_room_controller_class_init (GnalkRoomControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gnalk_room_controller_finalize;

  /**
   * GnalkRoomController::room-opened:
   * @self: The controller
   * @token: The new room token
   * @old_token: (nullable): The previous room token
   * @conv_data: (nullable): Conversation data for the new room
   *
   * Emitted after the controller has switched to a new room tab.
   * The Window should update all UI (header, highlights, buttons, etc.)
   * and check whether messages need loading from DB.
   */
  signals[SIGNAL_ROOM_OPENED] =
    g_signal_new ("room-opened",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_OBJECT);

  /**
   * GnalkRoomController::room-closed:
   * @self: The controller
   * @old_token: The room token being closed
   *
   * Emitted when leaving a room (content page hiding).
   * current_room is still valid when this signal fires.
   */
  signals[SIGNAL_ROOM_CLOSED] =
    g_signal_new ("room-closed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);

  /**
   * GnalkRoomController::messages-store-changed:
   * @self: The controller
   * @store: The new message store
   *
   * Emitted when the active GListStore changes.
   * Window should rebind its GtkNoSelection model.
   */
  signals[SIGNAL_MESSAGES_STORE_CHANGED] =
    g_signal_new ("messages-store-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_OBJECT);

  /**
   * GnalkRoomController::scroll-requested:
   * @self: The controller
   * @position: Item index to scroll to
   *
   * Emitted when the controller wants the message view to scroll
   * to a specific position (e.g., restoring a tab's scroll state).
   */
  signals[SIGNAL_SCROLL_REQUESTED] =
    g_signal_new ("scroll-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_UINT);

}

/* ============ Constructor and dependency setters ============ */

GnalkRoomController *
gnalk_room_controller_new (GnalkRoomTabManager *tab_manager,
                             GListStore          *conversations_store,
                             GSettings           *settings)
{
  GnalkRoomController *self = g_object_new (GNALK_TYPE_ROOM_CONTROLLER, NULL);
  self->tab_manager = tab_manager;
  self->conversations_store = conversations_store;
  self->settings = settings;
  return self;
}

void
gnalk_room_controller_set_session (GnalkRoomController *self,
                                     GnalkSession        *session)
{
  self->active_session = session;
}

void
gnalk_room_controller_set_message_controller (GnalkRoomController    *self,
                                                GnalkMessageController *mc)
{
  self->message_controller = mc;
}

void
gnalk_room_controller_set_participant_controller (GnalkRoomController        *self,
                                                    GnalkParticipantController *pc)
{
  self->participant_controller = pc;
}

void
gnalk_room_controller_set_conversations_store (GnalkRoomController *self,
                                                 GListStore          *store)
{
  self->conversations_store = store;
}

void
gnalk_room_controller_set_scroll_state_func (GnalkRoomController  *self,
                                               GnalkScrollStateFunc  func,
                                               gpointer              user_data)
{
  self->scroll_state_func = func;
  self->scroll_state_data = user_data;
}

/* ============ Core operations ============ */

/**
 * gnalk_room_controller_open_room:
 * @self: The controller
 * @token: Room token to open
 *
 * Opens a room: backgrounds the current room, switches to the new tab,
 * updates state, and emits signals for the Window to update UI.
 *
 * Signal emission order:
 * 1. "room-closed" (if there was an old room) - Window saves messages, stops typing timer
 * 2. "messages-store-changed" (if store changed) - Window rebinds selection model
 * 3. "room-opened" - Window updates all UI
 * 4. "scroll-requested" (if tab already had messages) - Window scrolls view
 */
void
gnalk_room_controller_open_room (GnalkRoomController *self, const char *token)
{
  if (!token)
    return;

  /* Already in this room */
  if (self->current_room &&
      g_strcmp0 (self->current_room->room_token, token) == 0)
    {
      g_message ("room_controller: already in room %s", token);
      return;
    }

  /* Find conversation data in the store */
  g_autoptr(GnalkConversationData) found_data = find_conversation_by_token (self, token);

  g_message ("room_controller: opening room %s (found=%d)", token, found_data != NULL);

  /* Save old room token before switching */
  g_autofree char *old_token = self->current_room
      ? g_strdup (self->current_room->room_token) : NULL;

  /* Background the current room's tab (saves scroll anchor, trims messages) */
  background_current_internal (self);

  /* Stop typing in the old room */
  stop_typing_internal (self);

  /* Emit room-closed for the old room so Window can save messages + do cleanup.
   * current_room still points to the old room at this point. */
  if (self->current_room)
    g_signal_emit (self, signals[SIGNAL_ROOM_CLOSED], 0, old_token);

  /* === SWITCH TO NEW TAB === */
  GnalkRoomTab *tab = gnalk_room_tab_manager_get_tab (self->tab_manager, token);
  self->current_room = &tab->state;
  tab->last_access_time = g_get_monotonic_time ();

  /* Swap messages store and notify if changed */
  GListStore *new_store = self->current_room->messages_store;
  if (self->messages_store != new_store)
    {
      GNALK_TRACE_STORE ("room_controller: swapping store from %p to %p",
                           (void *)self->messages_store, (void *)new_store);
      g_set_object (&self->messages_store, new_store);
      g_signal_emit (self, signals[SIGNAL_MESSAGES_STORE_CHANGED], 0, new_store);
    }

  gnalk_room_tab_manager_enforce_limit (self->tab_manager);

  /* Update state from conversation data */
  if (found_data)
    update_room_state_from_conv (self, found_data);

  self->has_active_room = TRUE;

  /* Set message controller room */
  if (self->message_controller)
    gnalk_message_controller_set_room (self->message_controller, token);

  /* Save last room to settings (skip for guest sessions — don't overwrite
   * the normal account's last room with a guest room token) */
  if (self->settings && found_data &&
      !(self->active_session && gnalk_session_is_guest (self->active_session)))
    g_settings_set_string (self->settings, "last-room-token", token);

  /* Emit room-opened - Window does all UI updates */
  g_signal_emit (self, signals[SIGNAL_ROOM_OPENED], 0, token, old_token, found_data);

  /* Handle scroll restoration for tabs that already have messages */
  guint tab_msg_count = g_list_model_get_n_items (G_LIST_MODEL (self->current_room->messages_store));
  GNALK_TRACE_ROOM_SWITCH ("room_controller: tab for %s has %u msgs, initial_load_done=%d",
                             token, tab_msg_count, self->current_room->initial_load_done);

  if (self->current_room->initial_load_done && tab_msg_count > 0)
    {
      /* Tab already has messages - request scroll restoration */
      guint scroll_pos = find_scroll_position (self);
      GNALK_TRACE_SCROLL ("room_controller: requesting scroll to position %u", scroll_pos);
      g_signal_emit (self, signals[SIGNAL_SCROLL_REQUESTED], 0, scroll_pos);

      /* Start polling for new messages */
      if (self->message_controller)
        gnalk_message_controller_start_polling (self->message_controller,
                                                  self->current_room->last_known_message_id);
    }
  /* else: the room-opened handler will load messages from DB */

  /* Join room via session (skip for pending invitations — not a real room yet) */
  if (!found_data || !gnalk_conversation_data_is_invitation (found_data))
    join_room_via_session (self, found_data, token);
}

/* Debounce callback */
static gboolean
on_debounce_timeout (gpointer user_data)
{
  GnalkRoomController *self = GNALK_ROOM_CONTROLLER (user_data);

  self->debounce_timer_id = 0;

  if (self->pending_room_token)
    {
      g_autofree char *token = g_steal_pointer (&self->pending_room_token);
      gnalk_room_controller_open_room (self, token);
    }

  return G_SOURCE_REMOVE;
}

void
gnalk_room_controller_open_room_debounced (GnalkRoomController *self,
                                             const char          *token)
{
  g_free (self->pending_room_token);
  self->pending_room_token = g_strdup (token);

  if (self->debounce_timer_id > 0)
    g_source_remove (self->debounce_timer_id);

  self->debounce_timer_id = g_timeout_add (ROOM_SWITCH_DEBOUNCE_MS,
                                             on_debounce_timeout, self);
}

/**
 * gnalk_room_controller_close_current:
 *
 * Closes the current room (e.g., when navigating back in mobile mode).
 * Backgrounds the tab, emits room-closed, leaves signaling/HTTP, clears state.
 */
void
gnalk_room_controller_close_current (GnalkRoomController *self)
{
  if (!self->current_room)
    return;

  g_autofree char *old_token = g_strdup (self->current_room->room_token);
  g_message ("room_controller: closing room %s", old_token);

  /* Background current tab (saves scroll anchor) */
  background_current_internal (self);

  /* Stop typing */
  stop_typing_internal (self);

  /* Emit room-closed BEFORE clearing state (handler needs current_room) */
  g_signal_emit (self, signals[SIGNAL_ROOM_CLOSED], 0, old_token);

  /* Stop participant polling */
  if (self->participant_controller)
    gnalk_participant_controller_set_active_room (self->participant_controller, NULL, 0);

  /* Leave room via session's signaling and HTTP.
   * Skip for guest sessions: leaving the room invalidates the guest's
   * server-side session entirely (signaling drops, API returns 404).
   * Guest sessions keep their connection alive in the background. */
  if (self->active_session && !gnalk_session_is_guest (self->active_session))
    {
      GnalkSignaling *sig = gnalk_session_get_signaling (self->active_session);
      GnalkHttpClient *http = gnalk_session_get_http_client (self->active_session);
      if (sig)
        gnalk_signaling_leave_room (sig, old_token);
      if (http)
        gnalk_http_client_leave_room (http, old_token, NULL, NULL);
    }

  /* Clear state */
  self->current_room = NULL;
  g_clear_object (&self->messages_store);
  self->has_active_room = FALSE;
}

void
gnalk_room_controller_restore_last_room (GnalkRoomController *self,
                                           gboolean             is_collapsed)
{
  /* If room already open, nothing to do here (Window handles header update) */
  if (self->current_room)
    return;

  /* In collapsed/mobile mode, stay on room list */
  if (is_collapsed)
    return;

  if (!self->settings)
    return;

  g_autofree char *last_room = g_settings_get_string (self->settings, "last-room-token");
  if (!last_room || !*last_room)
    return;

  g_message ("room_controller: restoring last room %s", last_room);

  /* Only restore if the room exists in our conversation list */
  g_autoptr(GnalkConversationData) conv = find_conversation_by_token (self, last_room);
  if (conv)
    {
      gnalk_room_controller_open_room (self, last_room);
      return;
    }

  /* Last room not found (e.g., account was removed and we switched to another).
   * Fall back to opening the first conversation in the list. */
  if (!self->conversations_store)
    return;

  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->conversations_store));
  for (guint i = 0; i < n; i++)
    {
      g_autoptr(GnalkConversationData) first = g_list_model_get_item (G_LIST_MODEL (self->conversations_store), i);
      if (first && !gnalk_conversation_data_is_section_header (first))
        {
          const char *token = gnalk_conversation_data_get_token (first);
          if (token)
            {
              g_message ("room_controller: last room not found, opening first conversation %s", token);
              gnalk_room_controller_open_room (self, token);
            }
          return;
        }
    }
}

/* ============ State queries ============ */

GnalkRoomState *
gnalk_room_controller_get_current_room (GnalkRoomController *self)
{
  return self->current_room;
}

const char *
gnalk_room_controller_get_current_room_token (GnalkRoomController *self)
{
  return self->current_room ? self->current_room->room_token : NULL;
}

gboolean
gnalk_room_controller_has_active_room (GnalkRoomController *self)
{
  return self->has_active_room;
}

GListStore *
gnalk_room_controller_get_messages_store (GnalkRoomController *self)
{
  return self->messages_store;
}
