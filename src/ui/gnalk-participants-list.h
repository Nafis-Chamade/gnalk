#pragma once

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include "gnalk-avatar-cache.h"

G_BEGIN_DECLS

#define GNALK_TYPE_PARTICIPANTS_LIST (gnalk_participants_list_get_type ())
G_DECLARE_FINAL_TYPE (GnalkParticipantsList, gnalk_participants_list, GNALK, PARTICIPANTS_LIST, GtkBox)

GnalkParticipantsList *gnalk_participants_list_new (void);

void gnalk_participants_list_set_participants (GnalkParticipantsList *self,
                                                 JsonArray              *participants,
                                                 GnalkAvatarCache      *cache,
                                                 const char             *my_user_id,
                                                 gint                    my_participant_type,
                                                 gint                    room_type);

void gnalk_participants_list_set_online (GnalkParticipantsList *self,
                                           gboolean                is_online);

void gnalk_participants_list_update_call_state (GnalkParticipantsList *self,
                                                  const char             *actor_id,
                                                  gint64                  in_call);

void gnalk_participants_list_clear_all_call_states (GnalkParticipantsList *self);

void gnalk_participants_list_update_participant_status (GnalkParticipantsList *self,
                                                          const char             *actor_id,
                                                          const char             *status);

void gnalk_participants_list_clear (GnalkParticipantsList *self);

void gnalk_participants_list_hide (GnalkParticipantsList *self);

void gnalk_participants_list_show (GnalkParticipantsList *self);

void gnalk_participants_list_set_visible (GnalkParticipantsList *self,
                                            gboolean                visible);

G_END_DECLS
