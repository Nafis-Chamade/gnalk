/* gnalk-window-message-display.c
 *
 * Thin UI layer for message display. Delegates data operations to
 * GnalkMessageProcessor and handles GTK widget interactions.
 *
 * Contains:
 * - clear_messages_store() (touches messages_selection widget)
 * - Processor signal handlers (batch insert, scroll, call state, loading)
 * - MC signal wrappers (messages-loaded, poll-response, older-messages-loaded)
 * - Public wrappers that other window partials call
 */

#include "gnalk-window-private.h"
#include "gnalk-application.h"
#include "gnalk-session.h"
#include "gnalk-data-repository.h"
#include "gnalk-database.h"
#include "gnalk-message-data.h"
#include "gnalk-message-processor.h"
#include "gnalk-message-controller.h"
#include "gnalk-message-view.h"
#include "gnalk-participant-controller.h"
#include "gnalk-incoming-call-banner.h"
#include "gnalk-message-row.h"
#include "gnalk-conversation-data.h"
#include "gnalk-thread-data.h"
#include "gnalk-http-client.h"
#include "gnalk-log.h"
#include <adwaita.h>
#include <json-glib/json-glib.h>
#include <glib/gi18n.h>
#include <glib/gi18n.h>

/* ============ Clear/refresh messages ============ */

/* Clear messages by disconnecting model first to avoid GTK internal state issues */
void
gnalk_window_clear_messages_store (GnalkWindow *self)
{
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  GNALK_TRACE_STORE ("clear_messages_store: clearing %u items from store %p for room %s",
                      n_items, (void*)self->messages_store,
                      self->current_room ? self->current_room->room_token : "(null)");

  /* Disconnect model from view first - prevents GTK crashes during transitions */
  gtk_no_selection_set_model (self->messages_selection, NULL);
  g_list_store_remove_all (self->messages_store);
  gtk_no_selection_set_model (self->messages_selection, G_LIST_MODEL (self->messages_store));
  GNALK_TRACE_STORE ("clear_messages_store: cleared, store now has 0 items");
}

/* ============ Public wrappers (called from other window partials) ============ */

void
gnalk_window_update_messages_is_own (GnalkWindow *self)
{
  gnalk_message_processor_update_is_own (self->message_processor);
}

void
gnalk_window_save_messages_to_db (GnalkWindow *self)
{
  gnalk_message_processor_save_to_db (self->message_processor);
}

void
gnalk_window_load_older_messages (GnalkWindow *self)
{
  gnalk_message_processor_load_older (self->message_processor);
}

void
gnalk_window_load_messages_from_db (GnalkWindow *self,
                                      const char   *room_token)
{
  GNALK_TRACE_DATABASE ("load_messages_from_db: starting for room %s, current_room=%s",
                          room_token, self->current_room ? self->current_room->room_token : "(null)");
  g_message ("load_messages_from_db: starting for room %s", room_token);

  gnalk_window_clear_messages_store (self);

  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  if (!app)
    {
      g_message ("load_messages_from_db: no app, returning");
      return;
    }

  GnalkDataRepository *repo = gnalk_application_get_data_repository (app);
  GnalkDatabase *db = repo ? gnalk_data_repository_get_database (repo) : NULL;
  gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;

  /* If account_id not set yet (offline startup), look it up from settings */
  if (account_id <= 0 && repo && gnalk_data_repository_is_open (repo))
    {
      GSettings *settings = gnalk_application_get_settings (app);
      g_autofree char *server_url = g_settings_get_string (settings, "last-server-url");
      g_autofree char *user_id = g_settings_get_string (settings, "last-user-id");

      if (server_url && *server_url && user_id && *user_id)
        {
          account_id = gnalk_data_repository_get_account_id (repo, server_url, user_id);
          g_message ("load_messages_from_db: looked up account_id=%" G_GINT64_FORMAT " from settings", account_id);
        }
    }

  g_message ("load_messages_from_db: db=%p, account_id=%" G_GINT64_FORMAT, (void*)db, account_id);

  if (!db || account_id <= 0)
    {
      g_message ("load_messages_from_db: no db or invalid account_id, returning");
      return;
    }

  /* Update processor dependencies and delegate */
  gnalk_message_processor_set_database (self->message_processor, db);
  gnalk_message_processor_set_account_id (self->message_processor, account_id);
  gnalk_message_processor_load_from_db (self->message_processor, room_token);
}

void
gnalk_window_process_messages_array (GnalkWindow *self,
                                       JsonArray    *messages,
                                       gboolean      is_initial_load)
{
  gnalk_message_processor_process_json (self->message_processor, messages, is_initial_load,
                                          gnalk_message_view_is_at_bottom (self->message_view));
}

/* ============ Processor signal handlers ============ */

void
gnalk_window_on_processor_batch_insert_begin (GnalkMessageProcessor *proc G_GNUC_UNUSED,
                                                GnalkWindow            *self)
{
  gtk_no_selection_set_model (self->messages_selection, NULL);
}

void
gnalk_window_on_processor_batch_insert_end (GnalkMessageProcessor *proc G_GNUC_UNUSED,
                                              GnalkWindow            *self)
{
  gtk_no_selection_set_model (self->messages_selection, G_LIST_MODEL (self->messages_store));
}

void
gnalk_window_on_processor_scroll_to_bottom (GnalkMessageProcessor *proc G_GNUC_UNUSED,
                                              GnalkWindow            *self)
{
  gnalk_message_view_scroll_to_bottom (self->message_view);
}

void
gnalk_window_on_processor_scroll_to_item (GnalkMessageProcessor *proc G_GNUC_UNUSED,
                                            guint                   position,
                                            GnalkWindow            *self)
{
  gnalk_message_view_show_messages_at_item (self->message_view, position);
}

void
gnalk_window_on_processor_call_state_left (GnalkMessageProcessor *proc G_GNUC_UNUSED,
                                             const char             *actor_id,
                                             GnalkWindow            *self)
{
  /* Route through PC (DB→signal→UI automatic) */
  if (self->participant_controller)
    gnalk_participant_controller_update_call_state (self->participant_controller, actor_id, 0);
  gnalk_incoming_call_banner_clear_responded (self->incoming_call_banner, actor_id);
}

void
gnalk_window_on_processor_call_ended (GnalkMessageProcessor *proc G_GNUC_UNUSED,
                                        GnalkWindow            *self)
{
  /* Route through PC (DB→signal→UI, icons hidden without destroying list) */
  if (self->participant_controller)
    gnalk_participant_controller_clear_all_call_states (self->participant_controller);
  gnalk_incoming_call_banner_clear_all_responded (self->incoming_call_banner);
}

void
gnalk_window_on_processor_loading_older_changed (GnalkMessageProcessor *proc G_GNUC_UNUSED,
                                                   gboolean                loading,
                                                   GnalkWindow            *self)
{
  gnalk_message_view_set_loading_older (self->message_view, loading);
  if (!loading)
    {
      GtkScrolledWindow *scroll = gnalk_message_view_get_scrolled_window (self->message_view);
      g_idle_add_once ((GSourceOnceFunc) (void (*)(void)) gtk_widget_queue_allocate, GTK_WIDGET (scroll));
    }
}

void
gnalk_window_on_processor_state_synced (GnalkMessageProcessor *proc G_GNUC_UNUSED,
                                          guint                   reason,
                                          GnalkWindow            *self)
{
  if (reason == GNALK_PROCESSOR_SYNC_DB_LOADED)
    {
      guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
      if (n_items > 0)
        gnalk_message_view_show_messages_at_bottom (self->message_view);
      else
        gnalk_message_view_show_messages (self->message_view);
    }
  else if (reason == GNALK_PROCESSOR_SYNC_CONTEXT_LOADED)
    {
      gnalk_message_view_show_messages (self->message_view);
    }
}

/* ============ Message Controller Signal Handlers ============ */

void
gnalk_window_on_message_controller_messages_loaded (GnalkMessageController *controller G_GNUC_UNUSED,
                                                      JsonArray               *messages,
                                                      gboolean                 is_initial,
                                                      GnalkWindow            *self)
{
  /* In context mode, non-initial loads (HPB refresh) are already saved
   * to DB by the MC. Skip display — they'll appear when restoring. */
  if (!is_initial && self->current_room && self->current_room->in_context_mode)
    return;

  /* In thread mode, non-initial loads: pass to processor with is_initial=FALSE
   * so per-message threadId filtering keeps only current-thread messages. */
  if (!is_initial && self->current_room && self->current_room->in_thread_mode)
    {
      gnalk_message_processor_process_json (self->message_processor, messages, FALSE,
                                              gnalk_message_view_is_at_bottom (self->message_view));
      return;
    }

  /* Non-initial loads in normal mode (e.g. chat relay refresh via HPB):
   * process incrementally like a poll response — no trim, no scroll reset. */
  if (!is_initial)
    {
      gnalk_message_processor_process_json (self->message_processor, messages, FALSE,
                                              gnalk_message_view_is_at_bottom (self->message_view));
      guint len = json_array_get_length (messages);
      if (len > 0)
        gnalk_message_processor_save_to_db (self->message_processor);
      return;
    }

  gnalk_message_processor_process_json (self->message_processor, messages, TRUE,
                                          gnalk_message_view_is_at_bottom (self->message_view));

  /* Save ALL messages to database for offline access (before trimming display) */
  gnalk_message_processor_save_to_db (self->message_processor);

  /* Trim display to last 15 messages (keep full cache in DB) */
  guint total = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  if (total > 15)
    {
      guint to_remove = total - 15;
      gtk_no_selection_set_model (self->messages_selection, NULL);
      g_list_store_splice (self->messages_store, 0, to_remove, NULL, 0);
      gtk_no_selection_set_model (self->messages_selection, G_LIST_MODEL (self->messages_store));
      total = 15;

      GnalkMessageData *oldest = g_list_model_get_item (G_LIST_MODEL (self->messages_store), 0);
      if (oldest)
        {
          self->current_room->oldest_loaded_id = gnalk_message_data_get_id (oldest);
          g_object_unref (oldest);
        }
    }
  g_message ("Messages displayed: %u (after server fetch)", total);

  gnalk_message_view_scroll_to_bottom (self->message_view);

  /* Start polling after initial messages are loaded */
  if (self->current_room && !self->current_room->initial_load_done)
    {
      self->current_room->initial_load_done = TRUE;
      gnalk_message_controller_start_polling (self->message_controller,
                                                self->current_room->last_known_message_id);
    }

  /* Update read status for displayed messages */
  if (self->current_room && self->current_room->last_common_read_message > 0)
    gnalk_window_update_read_status (self, self->current_room->last_common_read_message);
}

void
gnalk_window_on_message_controller_poll_response (GnalkMessageController *controller G_GNUC_UNUSED,
                                                    JsonArray               *messages,
                                                    GnalkWindow            *self)
{
  guint len = json_array_get_length (messages);
  GNALK_TRACE_POLLING ("poll_response: received %u messages", len);

  gnalk_message_processor_process_json (self->message_processor, messages, FALSE,
                                          gnalk_message_view_is_at_bottom (self->message_view));

  if (len > 0)
    {
      guint total = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
      g_message ("Messages displayed: %u (+%u from poll)", total, len);
      GNALK_TRACE_DATABASE ("poll_response: saving %u messages to database", len);
      gnalk_message_processor_save_to_db (self->message_processor);
    }

  /* Update read status */
  if (self->current_room && self->current_room->last_common_read_message > 0)
    gnalk_window_update_read_status (self, self->current_room->last_common_read_message);
}

void
gnalk_window_on_message_controller_older_messages_loaded (GnalkMessageController *controller G_GNUC_UNUSED,
                                                            JsonArray               *messages,
                                                            gboolean                 has_more,
                                                            GnalkWindow            *self)
{
  /* Skip stale main-timeline older-messages responses that arrive after entering thread/context mode */
  if (self->current_room &&
      (self->current_room->in_thread_mode || self->current_room->in_context_mode))
    return;

  gnalk_message_processor_process_older_from_server (self->message_processor, messages, has_more);
}

/* ============ Context mode handlers ============ */

void
gnalk_window_on_context_loaded (GnalkMessageController *mc G_GNUC_UNUSED,
                                JsonArray              *messages,
                                gint64                  target_id,
                                GnalkWindow            *self)
{
  gnalk_message_processor_load_context (self->message_processor, messages, target_id);
  gnalk_message_view_set_context_mode (self->message_view, TRUE);

  /* Schedule highlight on the target message after scroll completes */
  if (self->current_room && self->current_room->highlight_message_id > 0)
    {
      if (self->highlight_clear_timer_id)
        g_source_remove (self->highlight_clear_timer_id);
      self->highlight_clear_timer_id = g_timeout_add (200, gnalk_window_apply_highlight_timeout, self);
    }
}

void
gnalk_window_on_jump_to_present (GnalkMessageView *view G_GNUC_UNUSED,
                                 GnalkWindow      *self)
{
  gnalk_message_processor_restore_live (self->message_processor);
  gnalk_message_view_set_context_mode (self->message_view, FALSE);
}

/* ============ Thread mode handlers ============ */

void
gnalk_window_on_thread_open_requested (GnalkMessageRow *row G_GNUC_UNUSED,
                                       gint64           message_id,
                                       gpointer         user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (!self->current_room || !self->message_controller)
    return;

  /* If already in context mode, restore first */
  if (self->current_room->in_context_mode)
    {
      gnalk_message_processor_restore_live (self->message_processor);
      gnalk_message_view_set_context_mode (self->message_view, FALSE);
    }

  /* If already in thread mode, restore first */
  if (self->current_room->in_thread_mode)
    {
      gnalk_message_processor_restore_from_thread (self->message_processor);
      gnalk_message_view_set_thread_mode (self->message_view, FALSE, NULL, NULL);
    }

  /* Load thread messages from server */
  gnalk_message_controller_load_thread (self->message_controller, message_id);
}

static void
on_create_thread_dialog_response (AdwAlertDialog *dialog,
                                  const char     *response,
                                  gpointer        user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (g_strcmp0 (response, "create") != 0)
    return;

  GtkEntry *title_entry = GTK_ENTRY (g_object_get_data (G_OBJECT (dialog), "title-entry"));
  GtkEntry *msg_entry = GTK_ENTRY (g_object_get_data (G_OBJECT (dialog), "msg-entry"));

  const char *title = gtk_editable_get_text (GTK_EDITABLE (title_entry));
  const char *message = gtk_editable_get_text (GTK_EDITABLE (msg_entry));

  if (!title || !*title)
    title = _("Thread");
  if (!message || !*message)
    return;

  g_autofree char *ref_id = gnalk_window_generate_reference_id (message,
      self->current_room ? self->current_room->room_token : "");
  gnalk_message_controller_create_thread (self->message_controller,
      message, title, ref_id);
}

void
gnalk_window_on_thread_create_requested (GnalkMessageRow *row G_GNUC_UNUSED,
                                         gint64           message_id G_GNUC_UNUSED,
                                         gpointer         user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (!self->current_room || !self->message_controller)
    return;

  AdwAlertDialog *dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (_("Start a thread"), NULL));
  adw_alert_dialog_add_responses (dialog, "cancel", _("Cancel"), "create", _("Create"), NULL);
  adw_alert_dialog_set_response_appearance (dialog, "create", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (dialog, "create");
  adw_alert_dialog_set_close_response (dialog, "cancel");

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);

  GtkWidget *title_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (title_entry), _("Thread title"));
  gtk_box_append (GTK_BOX (box), title_entry);

  GtkWidget *msg_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (msg_entry), _("First message"));
  gtk_box_append (GTK_BOX (box), msg_entry);

  adw_alert_dialog_set_extra_child (dialog, box);

  g_object_set_data (G_OBJECT (dialog), "title-entry", title_entry);
  g_object_set_data (G_OBJECT (dialog), "msg-entry", msg_entry);

  g_signal_connect (dialog, "response",
      G_CALLBACK (on_create_thread_dialog_response), self);
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

void
gnalk_window_on_thread_messages_loaded (GnalkMessageController *mc G_GNUC_UNUSED,
                                        JsonArray              *messages,
                                        gint64                  thread_id,
                                        GnalkWindow            *self)
{
  gnalk_message_processor_load_thread (self->message_processor, messages, thread_id);

  /* Extract title and first message from thread for the banner */
  const char *title = NULL;
  const char *description = NULL;
  if (json_array_get_length (messages) > 0)
    {
      JsonObject *root = json_array_get_object_element (messages, 0);
      title = json_object_get_string_member_with_default (root, "threadTitle", NULL);
      description = json_object_get_string_member_with_default (root, "message", NULL);
    }

  gnalk_message_view_set_thread_mode (self->message_view, TRUE, title, description);

  /* Clear unread state for this thread in the sidebar */
  if (self->current_room_conv_data)
    gnalk_conversation_data_mark_thread_read (self->current_room_conv_data, thread_id);

  /* Mark thread messages as read on the server.
   * Thread messages contribute to the room's unread count, so we need to
   * call mark_chat_read with the highest thread message ID.
   * Use mark_messages_read_up_to which updates the conversation data locally
   * from the server response (avoids waiting for next sync cycle). */
  if (self->current_room)
    {
      guint n = json_array_get_length (messages);
      gint64 highest_id = 0;
      for (guint i = 0; i < n; i++)
        {
          JsonObject *msg = json_array_get_object_element (messages, i);
          gint64 mid = json_object_get_int_member_with_default (msg, "id", 0);
          if (mid > highest_id)
            highest_id = mid;
        }
      if (highest_id > 0)
        gnalk_window_mark_messages_read_up_to (self, highest_id);
    }
}

void
gnalk_window_on_thread_older_loaded (GnalkMessageController *mc G_GNUC_UNUSED,
                                     JsonArray              *messages,
                                     gboolean                has_more,
                                     GnalkWindow            *self)
{
  gnalk_message_processor_process_older_from_server (self->message_processor, messages, has_more);
}

void
gnalk_window_on_exit_thread (GnalkMessageView *view G_GNUC_UNUSED,
                             GnalkWindow      *self)
{
  gnalk_message_processor_restore_from_thread (self->message_processor);
  gnalk_message_view_set_thread_mode (self->message_view, FALSE, NULL, NULL);
}
