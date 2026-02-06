#pragma once

#include <gtk/gtk.h>
#include "gnalk-avatar-cache.h"

G_BEGIN_DECLS

#define GNALK_TYPE_INCOMING_CALL_BANNER (gnalk_incoming_call_banner_get_type ())
G_DECLARE_FINAL_TYPE (GnalkIncomingCallBanner, gnalk_incoming_call_banner, GNALK, INCOMING_CALL_BANNER, GtkBox)

GnalkIncomingCallBanner *gnalk_incoming_call_banner_new (void);

void gnalk_incoming_call_banner_show_call (GnalkIncomingCallBanner *self,
                                             const char               *room_token,
                                             const char               *room_name,
                                             const char               *caller_name,
                                             GnalkAvatarCache         *avatar_cache);

void gnalk_incoming_call_banner_hide (GnalkIncomingCallBanner *self);

const char *gnalk_incoming_call_banner_get_room_token (GnalkIncomingCallBanner *self);

const char *gnalk_incoming_call_banner_get_room_name (GnalkIncomingCallBanner *self);

const char *gnalk_incoming_call_banner_get_caller_id (GnalkIncomingCallBanner *self);

gboolean gnalk_incoming_call_banner_has_incoming_call (GnalkIncomingCallBanner *self);

void gnalk_incoming_call_banner_clear_responded (GnalkIncomingCallBanner *self,
                                                   const char               *caller_id);

void gnalk_incoming_call_banner_clear_all_responded (GnalkIncomingCallBanner *self);

G_END_DECLS
