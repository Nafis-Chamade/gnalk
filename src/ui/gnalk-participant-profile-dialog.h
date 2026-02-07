#pragma once

#include <adwaita.h>
#include "gnalk-avatar-cache.h"

G_BEGIN_DECLS

#define GNALK_TYPE_PARTICIPANT_PROFILE_DIALOG (gnalk_participant_profile_dialog_get_type ())
G_DECLARE_FINAL_TYPE (GnalkParticipantProfileDialog, gnalk_participant_profile_dialog, GNALK, PARTICIPANT_PROFILE_DIALOG, AdwDialog)

/**
 * gnalk_participant_profile_dialog_new:
 *
 * Creates a new participant profile dialog.
 *
 * Returns: (transfer full): A new participant profile dialog
 */
GnalkParticipantProfileDialog *gnalk_participant_profile_dialog_new (void);

/**
 * gnalk_participant_profile_dialog_set_participant:
 * @self: The dialog
 * @actor_id: The participant's actor ID
 * @actor_type: The actor type ("users", "federated_users", etc.)
 * @display_name: The display name
 * @participant_type: The participant type (1=owner, 2=moderator, 3=user, 5=guest)
 * @status: (nullable): Online status ("online", "away", "dnd", "offline")
 * @status_icon: (nullable): Status emoji
 * @status_message: (nullable): Status message text
 * @cache: (nullable): Avatar cache for loading avatar
 *
 * Populates the dialog with participant information.
 */
void gnalk_participant_profile_dialog_set_participant (GnalkParticipantProfileDialog *self,
                                                        const char                     *actor_id,
                                                        const char                     *actor_type,
                                                        const char                     *display_name,
                                                        gint                            participant_type,
                                                        const char                     *status,
                                                        const char                     *status_icon,
                                                        const char                     *status_message,
                                                        GnalkAvatarCache              *cache);

G_END_DECLS
