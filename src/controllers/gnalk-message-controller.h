#pragma once

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include "gnalk-http-client.h"
#include "gnalk-data-repository.h"

G_BEGIN_DECLS

#define GNALK_TYPE_MESSAGE_CONTROLLER (gnalk_message_controller_get_type ())
G_DECLARE_FINAL_TYPE (GnalkMessageController, gnalk_message_controller, GNALK, MESSAGE_CONTROLLER, GObject)

/**
 * GnalkMessageController:
 *
 * Handles message operations for Nextcloud Talk:
 * - Loading messages (initial and history)
 * - Long polling for new messages
 * - Sending messages (text and file uploads)
 * - Editing and deleting messages
 * - Adding reactions
 * - Saving messages to database
 *
 * All HTTP message fetching goes through this controller, which saves
 * received messages to the database before emitting signals.
 *
 * Emits signals to notify the UI of message state changes.
 *
 * Signals:
 * - "messages-loaded" (JsonArray *messages, gboolean is_initial) - Messages received and saved to DB
 * - "poll-response" (JsonArray *messages) - New messages from long polling, saved to DB
 * - "older-messages-loaded" (JsonArray *messages, gboolean has_more) - History loaded and saved to DB
 * - "message-sent" (const char *reference_id, gint64 message_id, gint64 timestamp) - Message confirmed sent
 * - "message-send-failed" (const char *reference_id, const char *error) - Send failed
 * - "message-edited" (gint64 message_id) - Edit confirmed
 * - "message-edit-failed" (gint64 message_id, const char *error) - Edit failed
 * - "message-deleted" (gint64 message_id) - Delete confirmed
 * - "message-delete-failed" (gint64 message_id, const char *error) - Delete failed
 * - "reaction-added" (gint64 message_id, const char *emoji) - Reaction confirmed
 * - "reaction-failed" (gint64 message_id, const char *emoji, const char *error) - Reaction failed
 * - "upload-started" (const char *reference_id) - File upload started
 * - "upload-completed" (const char *reference_id) - File upload and share completed
 * - "upload-failed" (const char *reference_id, const char *error) - Upload failed
 * - "read-status-updated" (gint64 last_common_read_message) - Read status changed
 */

GnalkMessageController *gnalk_message_controller_new (GnalkHttpClient *http_client);

/* Repository integration - MessageController saves messages to DB */
void gnalk_message_controller_set_repository (GnalkMessageController *self,
                                              GnalkDataRepository    *repository,
                                              gint64                  account_id);

/* Current user ID for message ownership detection */
void gnalk_message_controller_set_current_user (GnalkMessageController *self,
                                                  const char              *user_id);

/* Room management - must be set before operations */
void gnalk_message_controller_set_room (GnalkMessageController *self,
                                          const char              *room_token);
const char *gnalk_message_controller_get_room (GnalkMessageController *self);

/* Load messages */
void gnalk_message_controller_load_messages (GnalkMessageController *self);
void gnalk_message_controller_load_new_messages (GnalkMessageController *self,
                                                   gint64                   since_id);
void gnalk_message_controller_load_older_messages (GnalkMessageController *self,
                                                     gint64                   before_id);

/* Long polling */
void gnalk_message_controller_start_polling (GnalkMessageController *self,
                                               gint64                   last_known_id);
void gnalk_message_controller_stop_polling (GnalkMessageController *self);
gboolean gnalk_message_controller_is_polling (GnalkMessageController *self);

/* Send message */
void gnalk_message_controller_send_message (GnalkMessageController *self,
                                              const char              *message,
                                              gint64                   reply_to,
                                              const char              *reference_id);

/* Edit message */
void gnalk_message_controller_edit_message (GnalkMessageController *self,
                                              gint64                   message_id,
                                              const char              *new_text);

/* Delete message */
void gnalk_message_controller_delete_message (GnalkMessageController *self,
                                                gint64                   message_id);

/* Add reaction */
void gnalk_message_controller_add_reaction (GnalkMessageController *self,
                                              gint64                   message_id,
                                              const char              *emoji);

/* File upload */
void gnalk_message_controller_upload_file (GnalkMessageController *self,
                                             const char              *filename,
                                             GBytes                  *file_bytes,
                                             const char              *content_type,
                                             const char              *caption,
                                             gint64                   reply_to,
                                             const char              *reference_id);

/* Voice message upload - like file upload but with messageType: voice-message */
void gnalk_message_controller_upload_voice_message (GnalkMessageController *self,
                                                      const char              *filename,
                                                      GBytes                  *file_bytes,
                                                      const char              *reference_id);

/* Session-ready gate: defer polling until cookies are established */
void gnalk_message_controller_set_session_ready (GnalkMessageController *self,
                                                 gboolean                ready);

/* Cancel pending operations */
void gnalk_message_controller_cancel (GnalkMessageController *self);

G_END_DECLS
