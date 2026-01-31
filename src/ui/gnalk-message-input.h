#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNALK_TYPE_MESSAGE_INPUT (gnalk_message_input_get_type ())

G_DECLARE_FINAL_TYPE (GnalkMessageInput, gnalk_message_input, GNALK, MESSAGE_INPUT, GtkBox)

/**
 * GnalkMessageInput:
 *
 * Message input widget with text entry, emoji picker, and a combined media
 * button (attach file, voice recording, camera capture). The media button
 * remembers the last-used action as the default.
 *
 * Signals:
 * - "send-requested" (const char *text) - User pressed send with text
 * - "text-changed" (const char *text) - Text entry changed
 * - "attach-clicked" - User selected attach action
 * - "voice-message-recorded" (GBytes *audio_data, gdouble duration) - Voice recording completed
 * - "camera-clicked" - User selected camera action
 */

GnalkMessageInput *gnalk_message_input_new (void);

void        gnalk_message_input_set_text        (GnalkMessageInput *self,
                                                   const char         *text);
const char *gnalk_message_input_get_text        (GnalkMessageInput *self);
void        gnalk_message_input_clear           (GnalkMessageInput *self);
void        gnalk_message_input_grab_focus      (GnalkMessageInput *self);
void        gnalk_message_input_set_sensitive   (GnalkMessageInput *self,
                                                   gboolean            sensitive);
void        gnalk_message_input_set_attach_visible (GnalkMessageInput *self,
                                                      gboolean            visible);

/**
 * gnalk_message_input_set_media_buttons_visible:
 * @self: the message input
 * @visible: whether to show the media button
 *
 * Controls visibility of the combined media button.
 * Automatically hidden when text is entered.
 */
void        gnalk_message_input_set_media_buttons_visible (GnalkMessageInput *self,
                                                             gboolean            visible);

/**
 * gnalk_message_input_is_recording:
 * @self: the message input
 *
 * Returns: TRUE if currently recording a voice message
 */
gboolean    gnalk_message_input_is_recording (GnalkMessageInput *self);

G_END_DECLS
