#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNALK_TYPE_CONVERSATION_HEADER (gnalk_conversation_header_get_type ())

G_DECLARE_FINAL_TYPE (GnalkConversationHeader, gnalk_conversation_header, GNALK, CONVERSATION_HEADER, GtkBox)

GnalkConversationHeader *gnalk_conversation_header_new (void);

void gnalk_conversation_header_set_room        (GnalkConversationHeader *self,
                                                  const char               *token,
                                                  const char               *name,
                                                  const char               *description,
                                                  gint                      room_type);
void gnalk_conversation_header_set_avatar      (GnalkConversationHeader *self,
                                                  GdkTexture               *texture);
void gnalk_conversation_header_set_user_status (GnalkConversationHeader *self,
                                                  const char               *status);
void gnalk_conversation_header_set_call_active (GnalkConversationHeader *self,
                                                  gboolean                  active);
void gnalk_conversation_header_set_visible     (GnalkConversationHeader *self,
                                                  gboolean                  visible);

G_END_DECLS
