#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNALK_TYPE_CONVERSATION_ACTIONS (gnalk_conversation_actions_get_type ())

G_DECLARE_FINAL_TYPE (GnalkConversationActions, gnalk_conversation_actions, GNALK, CONVERSATION_ACTIONS, GtkBox)

GnalkConversationActions *gnalk_conversation_actions_new (void);

/* Quote/Reply preview */
void   gnalk_conversation_actions_set_reply       (GnalkConversationActions *self,
                                                     gint64                     message_id,
                                                     const char                *author,
                                                     const char                *text);
void   gnalk_conversation_actions_set_edit        (GnalkConversationActions *self,
                                                     gint64                     message_id,
                                                     const char                *text);
void   gnalk_conversation_actions_set_quote_image (GnalkConversationActions *self,
                                                     GdkTexture                *texture);
void   gnalk_conversation_actions_clear_quote     (GnalkConversationActions *self);
gint64 gnalk_conversation_actions_get_reply_id    (GnalkConversationActions *self);
gint64 gnalk_conversation_actions_get_edit_id     (GnalkConversationActions *self);

/* Attachment preview (multi-file support) */
void        gnalk_conversation_actions_add_attachment       (GnalkConversationActions *self,
                                                               const char                *file_name,
                                                               const char                *icon_name,
                                                               GdkTexture                *preview_texture);
void        gnalk_conversation_actions_clear_attachments    (GnalkConversationActions *self);
gboolean    gnalk_conversation_actions_has_attachments      (GnalkConversationActions *self);
guint       gnalk_conversation_actions_get_attachment_count (GnalkConversationActions *self);

G_END_DECLS
