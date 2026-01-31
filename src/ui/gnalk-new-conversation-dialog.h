#pragma once

#include <adwaita.h>
#include "gnalk-http-client.h"
#include "gnalk-avatar-cache.h"

G_BEGIN_DECLS

#define GNALK_TYPE_NEW_CONVERSATION_DIALOG (gnalk_new_conversation_dialog_get_type ())
G_DECLARE_FINAL_TYPE (GnalkNewConversationDialog, gnalk_new_conversation_dialog,
                      GNALK, NEW_CONVERSATION_DIALOG, AdwDialog)

GnalkNewConversationDialog *gnalk_new_conversation_dialog_new (void);

void gnalk_new_conversation_dialog_set_http_client (GnalkNewConversationDialog *self,
                                                      GnalkHttpClient            *client);

void gnalk_new_conversation_dialog_set_avatar_cache (GnalkNewConversationDialog *self,
                                                       GnalkAvatarCache           *cache);

G_END_DECLS
