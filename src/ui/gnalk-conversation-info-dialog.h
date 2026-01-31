#pragma once

#include <adwaita.h>
#include "gnalk-conversation-data.h"
#include "gnalk-http-client.h"
#include "gnalk-avatar-cache.h"

G_BEGIN_DECLS

#define GNALK_TYPE_CONVERSATION_INFO_DIALOG (gnalk_conversation_info_dialog_get_type ())
G_DECLARE_FINAL_TYPE (GnalkConversationInfoDialog, gnalk_conversation_info_dialog, GNALK, CONVERSATION_INFO_DIALOG, AdwDialog)

GnalkConversationInfoDialog *gnalk_conversation_info_dialog_new (void);

void gnalk_conversation_info_dialog_set_conversation (GnalkConversationInfoDialog *self,
                                                        GnalkConversationData       *conversation);

void gnalk_conversation_info_dialog_set_http_client (GnalkConversationInfoDialog *self,
                                                       GnalkHttpClient             *client);

void gnalk_conversation_info_dialog_set_avatar_cache (GnalkConversationInfoDialog *self,
                                                        GnalkAvatarCache            *cache);

void gnalk_conversation_info_dialog_set_my_user_id (GnalkConversationInfoDialog *self,
                                                      const char                   *user_id);

G_END_DECLS
