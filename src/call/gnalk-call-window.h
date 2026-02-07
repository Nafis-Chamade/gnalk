#pragma once

#include <adwaita.h>
#include "gnalk-http-client.h"
#include "gnalk-webrtc.h"
#include "gnalk-signaling.h"
#include "gnalk-avatar-cache.h"

G_BEGIN_DECLS

#define GNALK_TYPE_CALL_WINDOW (gnalk_call_window_get_type ())
G_DECLARE_FINAL_TYPE (GnalkCallWindow, gnalk_call_window, GNALK, CALL_WINDOW, AdwWindow)

GnalkCallWindow *gnalk_call_window_new (GtkApplication *app);

void gnalk_call_window_start_call (GnalkCallWindow  *self,
                                    const char        *room_token,
                                    const char        *room_name,
                                    const char        *session_id,
                                    GnalkHttpClient  *http_client,
                                    GnalkWebrtc      *webrtc,
                                    GnalkSignaling   *signaling,
                                    GnalkAvatarCache *avatar_cache,
                                    gboolean           start_with_video,
                                    gboolean           is_federated);

void gnalk_call_window_end_call (GnalkCallWindow *self);

G_END_DECLS
