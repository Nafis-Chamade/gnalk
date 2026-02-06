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
#include "gnalk-database.h"
#include "gnalk-message-data.h"
#include "gnalk-message-processor.h"
#include "gnalk-message-controller.h"
#include "gnalk-log.h"
#include <json-glib/json-glib.h>
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

  GnalkDatabase *db = gnalk_application_get_database (app);
  gint64 account_id = self->active_session ? gnalk_session_get_account_id (self->active_session) : 0;

  /* If account_id not set yet (offline startup), look it up from settings */
  if (account_id <= 0 && db)
    {
      GSettings *settings = gnalk_application_get_settings (app);
      g_autofree char *server_url = g_settings_get_string (settings, "last-server-url");
      g_autofree char *user_id = g_settings_get_string (settings, "last-user-id");

      if (server_url && *server_url && user_id && *user_id)
        {
          account_id = gnalk_database_get_or_create_account (db, server_url, user_id);
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
  GnalkSignaling *signaling = self->active_session
    ? gnalk_session_get_signaling (self->active_session) : NULL;
  if (signaling)
    gnalk_signaling_set_call_state (signaling, actor_id, 0);
  gnalk_participants_list_update_call_state (self->participants_list, actor_id, 0);
  gnalk_incoming_call_banner_clear_responded (self->incoming_call_banner, actor_id);
}

void
gnalk_window_on_processor_call_ended (GnalkMessageProcessor *proc G_GNUC_UNUSED,
                                        GnalkWindow            *self)
{
  GnalkSignaling *signaling = self->active_session
    ? gnalk_session_get_signaling (self->active_session) : NULL;
  if (signaling)
    gnalk_signaling_clear_call_states (signaling);
  gnalk_participants_list_clear (self->participants_list);
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
}

/* ============ Message Controller Signal Handlers ============ */

void
gnalk_window_on_message_controller_messages_loaded (GnalkMessageController *controller G_GNUC_UNUSED,
                                                      JsonArray               *messages,
                                                      gboolean                 is_initial G_GNUC_UNUSED,
                                                      GnalkWindow            *self)
{
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
  gnalk_message_processor_process_older_from_server (self->message_processor, messages, has_more);
}
