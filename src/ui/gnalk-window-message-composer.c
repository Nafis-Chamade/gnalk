/* gnalk-window-message-composer.c
 *
 * Message composition and file upload functionality extracted from gnalk-window.c.
 * Handles sending messages, attachments, editing, and upload queuing.
 */

#include "gnalk-window-private.h"
#include "gnalk-application.h"
#include "gnalk-message-controller.h"
#include "gnalk-sync-manager.h"
#include <json-glib/json-glib.h>

/* ============ Reference ID Generation ============ */

/* Generate a unique reference ID for message tracking */
char *
gnalk_window_generate_reference_id (const char *message, const char *room_token)
{
  gint64 timestamp = g_get_real_time ();
  g_autofree char *data = g_strdup_printf ("%s|%s|%ld|%d",
                                            message, room_token, timestamp, g_random_int ());
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  g_checksum_update (checksum, (guchar *) data, -1);
  return g_strdup (g_checksum_get_string (checksum));
}

/* ============ File Upload Functions ============ */

/* Forward declaration */
static void process_upload_queue (GnalkWindow *self);

/* Add a pending file message to the store for immediate UI feedback */
static void
add_pending_file_message (GnalkWindow         *self,
                          const AttachmentInfo *att,
                          const char           *caption,
                          gint64                reply_to,
                          const char           *reference_id)
{
  if (!att || !att->file_name)
    return;

  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  const char *user_id = gnalk_application_get_user_id (app);
  const char *user_display_name = gnalk_application_get_user_name (app);

  /* Create pending file message for optimistic UI */
  GnalkMessageData *pending = gnalk_message_data_new_pending_file (
      caption,
      user_display_name ? user_display_name : user_id,
      user_id,
      reference_id,
      reply_to,
      att->file_name,
      att->file_path,
      att->content_type);

  /* Add to message store immediately (optimistic UI) */
  if (self->messages_store)
    g_list_store_append (self->messages_store, pending);

  g_object_unref (pending);
}

/* Start the actual file upload (message already added to store) */
static void
start_file_upload (GnalkWindow         *self,
                   const AttachmentInfo *att,
                   const char           *caption,
                   gint64                reply_to,
                   const char           *reference_id)
{
  if (!att || !att->file_bytes || !att->file_name)
    return;

  /* Delegate to message controller */
  gnalk_message_controller_upload_file (self->message_controller,
                                          att->file_name,
                                          att->file_bytes,
                                          att->content_type,
                                          caption,
                                          reply_to,
                                          reference_id);
}

/* Process the next item in the upload queue */
static void
process_upload_queue (GnalkWindow *self)
{
  if (self->upload_in_progress)
    return;

  if (!self->upload_queue || g_queue_is_empty (self->upload_queue))
    return;

  UploadQueueItem *item = g_queue_pop_head (self->upload_queue);
  if (!item)
    return;

  self->upload_in_progress = TRUE;

  /* Start the actual upload (message already added to store when queued) */
  start_file_upload (self, item->attachment, item->caption,
                     item->reply_to, item->reference_id);

  /* Free the item (attachment data is copied by upload_file, so we can free it now) */
  upload_queue_item_free (item);
}

/* Called after upload completes to process next in queue */
void
gnalk_window_process_upload_queue (GnalkWindow *self)
{
  self->upload_in_progress = FALSE;
  process_upload_queue (self);
}

/* Queue a file for upload */
void
gnalk_window_queue_file_upload (GnalkWindow   *self,
                                 AttachmentInfo *att,   /* Takes ownership */
                                 const char     *caption,
                                 gint64          reply_to,
                                 const char     *reference_id)
{
  if (!self->upload_queue)
    self->upload_queue = g_queue_new ();

  /* Add pending message to store immediately for UI feedback (shows spinner) */
  add_pending_file_message (self, att, caption, reply_to, reference_id);

  UploadQueueItem *item = g_new0 (UploadQueueItem, 1);
  item->attachment = att;  /* Takes ownership */
  item->caption = g_strdup (caption);
  item->reply_to = reply_to;
  item->reference_id = g_strdup (reference_id);

  g_queue_push_tail (self->upload_queue, item);

  /* Start processing if not already */
  process_upload_queue (self);
}

/* ============ Attachment Management ============ */

void
gnalk_window_clear_attachments (GnalkWindow *self)
{
  if (self->attachments)
    g_ptr_array_set_size (self->attachments, 0);

  gnalk_conversation_actions_clear_attachments (self->conversation_actions);
}

/* ============ Message Send Handler ============ */

void
gnalk_window_on_message_input_send_requested (GnalkMessageInput *input,
                                                const char         *text,
                                                GnalkWindow       *self)
{
  (void) input;

  if (!self->current_room_token)
    return;

  gboolean has_text = text && *text;
  gboolean has_attachments = self->attachments && self->attachments->len > 0;

  /* Must have either text or attachment */
  if (!has_text && !has_attachments)
    return;

  /* If we have attachments, queue them for sequential upload (text only on the last one) */
  if (has_attachments)
    {
      guint n_attachments = self->attachments->len;

      for (guint i = 0; i < n_attachments; i++)
        {
          AttachmentInfo *att = g_ptr_array_index (self->attachments, i);
          gboolean is_last = (i == n_attachments - 1);

          /* Caption only on the last message */
          const char *caption = is_last ? text : NULL;
          /* Reply-to only on the first message */
          gint64 reply_to = (i == 0) ? self->reply_to_message_id : 0;

          g_autofree char *reference_id = gnalk_window_generate_reference_id (
              caption ? caption : att->file_name,
              self->current_room_token);

          /* Create a copy of the attachment for the queue (since we'll clear the array) */
          AttachmentInfo *att_copy = g_new0 (AttachmentInfo, 1);
          att_copy->file_path = g_strdup (att->file_path);
          att_copy->file_bytes = g_bytes_ref (att->file_bytes);
          att_copy->content_type = g_strdup (att->content_type);
          att_copy->file_name = g_strdup (att->file_name);

          gnalk_window_queue_file_upload (self, att_copy, caption, reply_to, reference_id);
        }

      /* Scroll to bottom after all pending messages are added */
      gnalk_message_view_scroll_to_bottom (self->message_view);

      /* Clear attachments preview immediately */
      gnalk_window_clear_attachments (self);

      /* Clear entry and quote state */
      gnalk_message_input_clear (self->message_input);
      self->reply_to_message_id = 0;
      gnalk_conversation_actions_clear_quote (self->conversation_actions);
      return;
    }

  /* If we're in edit mode, send edit request instead of new message */
  if (self->edit_message_id > 0)
    {
      /* Check if we're editing a message near the bottom */
      guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
      gboolean editing_near_bottom = FALSE;

      /* Update local message immediately (optimistic update) */
      for (guint j = 0; j < n_items; j++)
        {
          GnalkMessageData *existing = g_list_model_get_item (G_LIST_MODEL (self->messages_store), j);
          if (gnalk_message_data_get_id (existing) == self->edit_message_id)
            {
              /* Update message text and set edit timestamp to current time */
              gnalk_message_data_set_message (existing, text);
              gint64 now = g_get_real_time () / G_USEC_PER_SEC;
              gnalk_message_data_set_last_edit_timestamp (existing, now);

              /* Force rebind by removing and re-inserting */
              g_object_ref (existing);
              g_list_store_remove (self->messages_store, j);
              g_list_store_insert (self->messages_store, j, existing);
              g_object_unref (existing);
              g_object_unref (existing);

              /* Consider "near bottom" if within last 3 messages */
              editing_near_bottom = (j >= n_items - 3);
              break;
            }
          g_object_unref (existing);
        }

      /* If editing near bottom, scroll to bottom to keep message visible */
      if (editing_near_bottom)
        gnalk_message_view_scroll_to_bottom (self->message_view);

      /* Edit message via controller */
      gnalk_message_controller_set_room (self->message_controller, self->current_room_token);
      gnalk_message_controller_edit_message (self->message_controller,
                                               self->edit_message_id,
                                               text);

      gnalk_message_input_clear (self->message_input);

      /* Clear edit state */
      self->edit_message_id = 0;
      gnalk_conversation_actions_clear_quote (self->conversation_actions);
      return;
    }

  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  const char *user_id = gnalk_application_get_user_id (app);
  const char *user_display_name = gnalk_application_get_user_name (app);

  /* Generate unique reference ID for tracking */
  g_autofree char *reference_id = gnalk_window_generate_reference_id (text, self->current_room_token);

  /* Create pending message data immediately (optimistic UI) */
  GnalkMessageData *pending_data = gnalk_message_data_new_pending (
      text,
      user_display_name ? user_display_name : user_id,
      user_id,
      reference_id,
      self->reply_to_message_id);

  /* If replying to a message, copy the parent data for quote display */
  if (self->reply_to_message_id > 0 && self->messages_store)
    {
      guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
      for (guint i = 0; i < n_items; i++)
        {
          GnalkMessageData *msg_data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
          if (msg_data && gnalk_message_data_get_id (msg_data) == self->reply_to_message_id)
            {
              /* Build parent object with necessary fields for quote display */
              JsonObject *parent = json_object_new ();
              json_object_set_int_member (parent, "id", gnalk_message_data_get_id (msg_data));
              json_object_set_string_member (parent, "actorId",
                  gnalk_message_data_get_author_id (msg_data) ? gnalk_message_data_get_author_id (msg_data) : "");
              json_object_set_string_member (parent, "actorDisplayName",
                  gnalk_message_data_get_author_name (msg_data) ? gnalk_message_data_get_author_name (msg_data) : "");
              json_object_set_string_member (parent, "message",
                  gnalk_message_data_get_message (msg_data) ? gnalk_message_data_get_message (msg_data) : "");

              /* Copy messageParameters if present (for file preview) */
              JsonObject *params = gnalk_message_data_get_message_parameters (msg_data);
              if (params)
                json_object_set_object_member (parent, "messageParameters", json_object_ref (params));

              gnalk_message_data_set_parent (pending_data, parent);
              json_object_unref (parent);

              g_object_unref (msg_data);
              break;
            }
          if (msg_data)
            g_object_unref (msg_data);
        }
    }

  /* Add to message store at the end */
  g_list_store_append (self->messages_store, pending_data);
  g_object_unref (pending_data);

  /* Scroll to bottom to show the new message */
  gnalk_message_view_scroll_to_bottom (self->message_view);

  /* Check if online - if offline, queue the message instead of sending */
  gboolean is_online = !self->sync_manager || gnalk_sync_manager_is_online (self->sync_manager);

  if (is_online)
    {
      /* Send message via controller */
      gnalk_message_controller_set_room (self->message_controller, self->current_room_token);
      gnalk_message_controller_send_message (self->message_controller,
                                               text,
                                               self->reply_to_message_id,
                                               reference_id);
    }
  else
    {
      /* Queue message for later sending */
      gnalk_sync_manager_queue_message (self->sync_manager,
                                          self->current_room_token,
                                          text,
                                          self->reply_to_message_id,
                                          reference_id);
      g_debug ("Message queued for offline send: %s", reference_id);
    }

  gnalk_message_input_clear (self->message_input);

  /* Clear quote state */
  self->reply_to_message_id = 0;
  gnalk_conversation_actions_clear_quote (self->conversation_actions);
}
