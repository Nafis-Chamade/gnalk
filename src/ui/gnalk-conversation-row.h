#pragma once

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include "gnalk-avatar-cache.h"
#include "gnalk-conversation-data.h"

G_BEGIN_DECLS

#define GNALK_TYPE_CONVERSATION_ROW (gnalk_conversation_row_get_type ())
G_DECLARE_FINAL_TYPE (GnalkConversationRow, gnalk_conversation_row, GNALK, CONVERSATION_ROW, GtkBox)

GnalkConversationRow *gnalk_conversation_row_new (void);

void gnalk_conversation_row_set_data (GnalkConversationRow *self,
                                        JsonObject            *room_data,
                                        GnalkAvatarCache     *avatar_cache);

/* Set row data from GnalkConversationData object (for list factory pattern) */
void gnalk_conversation_row_set_from_data (GnalkConversationRow  *self,
                                             GnalkConversationData *data,
                                             GnalkAvatarCache      *avatar_cache);

/* Reset row to initial state (for unbind in list factory) */
void gnalk_conversation_row_reset (GnalkConversationRow *self);

const char *gnalk_conversation_row_get_token (GnalkConversationRow *self);
const char *gnalk_conversation_row_get_title (GnalkConversationRow *self);
const char *gnalk_conversation_row_get_name (GnalkConversationRow *self);
const char *gnalk_conversation_row_get_user_status (GnalkConversationRow *self);
gint gnalk_conversation_row_get_room_type (GnalkConversationRow *self);
gint64 gnalk_conversation_row_get_last_activity (GnalkConversationRow *self);
gint64 gnalk_conversation_row_get_last_common_read_message (GnalkConversationRow *self);
void gnalk_conversation_row_set_active (GnalkConversationRow *self, gboolean active);

void gnalk_conversation_row_set_expandable         (GnalkConversationRow *self, gboolean expandable);
void gnalk_conversation_row_set_expanded            (GnalkConversationRow *self, gboolean expanded);
void gnalk_conversation_row_set_has_unread_threads  (GnalkConversationRow *self, gboolean has_unread);

G_END_DECLS
