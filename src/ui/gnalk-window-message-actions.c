#include "gnalk-window-private.h"
#include "gnalk-application.h"
#include "gnalk-database.h"
#include "gnalk-data-repository.h"
#include "gnalk-message-data.h"
#include "gnalk-account.h"
#include <glib/gi18n.h>

/* ============ Participant Controller Changed Handler ============ */

void
gnalk_window_on_participant_controller_changed (GnalkParticipantController *controller G_GNUC_UNUSED,
                                    const char                  *room_token,
                                    GnalkWindow                *self)
{
  g_debug ("Window: Participants changed for %s", room_token);

  /* Only update if this is the current room */
  if (!self->current_room || g_strcmp0 (room_token, self->current_room->room_token) != 0)
    return;

  /* Reload participants from database */
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  GnalkDataRepository *repo = gnalk_application_get_data_repository (app);
  gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;
  const char *my_user_id = NULL;
  if (self->active_session)
    {
      GnalkAccount *account = gnalk_session_get_account (self->active_session);
      my_user_id = gnalk_account_get_user_id (account);
    }

  g_debug ("on_participant_controller_changed: room=%s, account_id=%ld", room_token, (long)account_id);

  if (!repo || account_id <= 0)
    {
      g_warning ("on_participant_controller_changed: skipping, repo=%p, account_id=%ld", (void*)repo, (long)account_id);
      return;
    }

  JsonArray *participants = gnalk_data_repository_load_participants (repo, account_id, room_token);
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

      if (self->current_room->room_type == 1 && actor_id && *actor_id &&
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
      if (g_strcmp0 (token, self->current_room->room_token) == 0)
        {
          my_participant_type = gnalk_conversation_data_get_participant_type (conv);
          g_object_unref (conv);
          break;
        }
      g_object_unref (conv);
    }

  /* Delegate to the participants list component */
  GnalkSignaling *signaling = self->active_session
    ? gnalk_session_get_signaling (self->active_session) : NULL;
  gnalk_participants_list_set_participants (self->participants_list, participants,
                                              self->avatar_cache, signaling,
                                              my_user_id, my_participant_type,
                                              self->current_room->room_type);

  /* For group rooms with participants, respect user preference and sync toggle */
  if (self->current_room->room_type != 1 && n_participants > 0)
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

  /* Update mosaic background with participant avatars */
  gnalk_window_update_mosaic_background (self, participants);

  json_array_unref (participants);
}

/* ============ Participant Action Handlers ============ */

void
gnalk_window_on_participant_chat_requested (GnalkParticipantsList *list G_GNUC_UNUSED,
                                              const char             *actor_id,
                                              GnalkWindow           *self)
{
  if (!actor_id || !*actor_id || !self->conversation_controller)
    return;

  g_message ("Creating 1-to-1 conversation with %s", actor_id);

  /* ConversationController handles HTTP + DB save + emits conversation-created
   * which triggers gnalk_window_on_conversation_controller_created → open room */
  gnalk_conversation_controller_create (self->conversation_controller, 1, actor_id, NULL);
}

void
gnalk_window_on_participant_promote_requested (GnalkParticipantsList *list G_GNUC_UNUSED,
                                                 gint64                  attendee_id,
                                                 const char             *actor_id,
                                                 GnalkWindow           *self)
{
  if (!self->participant_controller || !self->current_room)
    return;

  g_message ("Promoting participant %s (attendee %" G_GINT64_FORMAT ") to moderator",
             actor_id ? actor_id : "(unknown)", attendee_id);

  gnalk_participant_controller_promote_moderator (self->participant_controller,
                                                  self->current_room->room_token,
                                                  attendee_id);
}

void
gnalk_window_on_participant_demote_requested (GnalkParticipantsList *list G_GNUC_UNUSED,
                                                gint64                  attendee_id,
                                                const char             *actor_id,
                                                GnalkWindow           *self)
{
  if (!self->participant_controller || !self->current_room)
    return;

  g_message ("Demoting participant %s (attendee %" G_GINT64_FORMAT ") from moderator",
             actor_id ? actor_id : "(unknown)", attendee_id);

  gnalk_participant_controller_demote_moderator (self->participant_controller,
                                                 self->current_room->room_token,
                                                 attendee_id);
}

void
gnalk_window_on_participant_remove_requested (GnalkParticipantsList *list G_GNUC_UNUSED,
                                                gint64                  attendee_id,
                                                const char             *actor_id,
                                                GnalkWindow           *self)
{
  if (!self->participant_controller || !self->current_room)
    return;

  g_message ("Removing participant %s (attendee %" G_GINT64_FORMAT ") from room",
             actor_id ? actor_id : "(unknown)", attendee_id);

  gnalk_participant_controller_remove_attendee (self->participant_controller,
                                                self->current_room->room_token,
                                                attendee_id);
}

/* ============ Message Row Action Handlers ============ */

void
gnalk_window_on_reaction_requested (GnalkMessageRow *row G_GNUC_UNUSED,
                                      gint64            message_id,
                                      const char       *emoji,
                                      gpointer          user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (!self->message_controller || !self->current_room)
    return;

  /* Add reaction via controller - signal handler will update UI on success */
  gnalk_message_controller_set_room (self->message_controller, self->current_room->room_token);
  gnalk_message_controller_add_reaction (self->message_controller, message_id, emoji);
}

/* ============ Scroll-to-message highlight helpers ============ */

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

  if (self->current_room->highlight_message_id <= 0)
    return;

  if (GNALK_IS_MESSAGE_ROW (widget))
    {
      GnalkMessageRow *row = GNALK_MESSAGE_ROW (widget);
      gint64 row_id = gnalk_message_row_get_id (row);
      if (row_id == self->current_room->highlight_message_id)
        {
          /* Remove class and re-add after a frame to restart animation */
          gtk_widget_remove_css_class (widget, "message-highlight");
          g_timeout_add (50, add_highlight_class_idle, widget);
          self->current_room->highlight_message_id = 0;  /* Found it, stop searching */
        }
    }
}

static void
iterate_children (GtkWidget *widget, gpointer user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (self->current_room->highlight_message_id <= 0)
    return;

  find_and_highlight_row (widget, user_data);

  /* Recurse into children */
  GtkWidget *child = gtk_widget_get_first_child (widget);
  while (child && self->current_room->highlight_message_id > 0)
    {
      iterate_children (child, user_data);
      child = gtk_widget_get_next_sibling (child);
    }
}

static gboolean
apply_highlight_timeout (gpointer user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (self->current_room->highlight_message_id <= 0)
    return G_SOURCE_REMOVE;

  /* Recursively search for the message row in the widget tree */
  iterate_children (GTK_WIDGET (gnalk_message_view_get_list_view (self->message_view)), self);

  self->current_room->highlight_message_id = 0;
  self->highlight_clear_timer_id = 0;
  return G_SOURCE_REMOVE;
}

void
gnalk_window_on_scroll_to_message_requested (GnalkMessageRow *row,
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
          self->current_room->highlight_message_id = message_id;

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

void
gnalk_window_on_quote_preview_scroll_requested (GnalkConversationActions *actions,
                                                  gint64                     message_id,
                                                  GnalkWindow              *self)
{
  (void) actions;
  gnalk_window_on_scroll_to_message_requested (NULL, message_id, self);
}

void
gnalk_window_on_quote_closed (GnalkConversationActions *actions,
                                gboolean                   was_editing,
                                GnalkWindow              *self)
{
  (void) actions;

  /* If we were editing, clear the entry as well */
  if (was_editing)
    gnalk_message_input_clear (self->message_input);

  self->current_room->reply_to_message_id = 0;
  self->current_room->edit_message_id = 0;
  gnalk_conversation_actions_clear_quote (self->conversation_actions);
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

void
gnalk_window_on_quote_requested (GnalkMessageRow *row,
                                   gint64            message_id,
                                   const char       *author_name,
                                   const char       *message_text,
                                   gpointer          user_data)
{
  (void) row;
  GnalkWindow *self = GNALK_WINDOW (user_data);

  self->current_room->reply_to_message_id = message_id;

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

void
gnalk_window_on_edit_requested (GnalkMessageRow *row,
                                  gint64            message_id,
                                  const char       *message_text,
                                  gpointer          user_data)
{
  (void) row;
  GnalkWindow *self = GNALK_WINDOW (user_data);

  /* Set edit mode - clear any reply in progress */
  self->current_room->edit_message_id = message_id;
  self->current_room->reply_to_message_id = 0;

  /* Show edit indicator using conversation_actions */
  gnalk_conversation_actions_set_edit (self->conversation_actions, message_id, message_text);

  /* Put the message text in the entry for editing */
  gnalk_message_input_set_text (self->message_input, message_text);
  gnalk_message_input_grab_focus (self->message_input);
}

void
gnalk_window_on_delete_requested (GnalkMessageRow *row G_GNUC_UNUSED,
                                    gint64            message_id,
                                    gpointer          user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (!self->current_room || message_id <= 0)
    return;

  /* Delete message via controller */
  gnalk_message_controller_set_room (self->message_controller, self->current_room->room_token);
  gnalk_message_controller_delete_message (self->message_controller, message_id);
}
