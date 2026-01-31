#pragma once

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include "gnalk-webrtc.h"

G_BEGIN_DECLS

#define GNALK_TYPE_PARTICIPANT_TILE (gnalk_participant_tile_get_type ())
G_DECLARE_FINAL_TYPE (GnalkParticipantTile, gnalk_participant_tile, GNALK, PARTICIPANT_TILE, GtkBox)

GnalkParticipantTile *gnalk_participant_tile_new (void);

void gnalk_participant_tile_set_session_id (GnalkParticipantTile *self,
                                              const char            *session_id);

const char *gnalk_participant_tile_get_session_id (GnalkParticipantTile *self);

void gnalk_participant_tile_set_display_name (GnalkParticipantTile *self,
                                                const char            *display_name);

const char *gnalk_participant_tile_get_display_name (GnalkParticipantTile *self);

void gnalk_participant_tile_set_subscriber_sid (GnalkParticipantTile *self,
                                                  const char            *subscriber_sid);

const char *gnalk_participant_tile_get_subscriber_sid (GnalkParticipantTile *self);

void gnalk_participant_tile_set_webrtc (GnalkParticipantTile *self,
                                          GnalkWebrtc          *webrtc);

GnalkWebrtc *gnalk_participant_tile_get_webrtc (GnalkParticipantTile *self);

void gnalk_participant_tile_set_is_local (GnalkParticipantTile *self,
                                            gboolean               is_local);

gboolean gnalk_participant_tile_get_is_local (GnalkParticipantTile *self);

void gnalk_participant_tile_set_video_enabled (GnalkParticipantTile *self,
                                                 gboolean               enabled);

gboolean gnalk_participant_tile_get_video_enabled (GnalkParticipantTile *self);

void gnalk_participant_tile_set_audio_enabled (GnalkParticipantTile *self,
                                                 gboolean               enabled);

gboolean gnalk_participant_tile_get_audio_enabled (GnalkParticipantTile *self);

void gnalk_participant_tile_set_active_speaker (GnalkParticipantTile *self,
                                                  gboolean               active);

gboolean gnalk_participant_tile_get_active_speaker (GnalkParticipantTile *self);

void gnalk_participant_tile_update_video_frame (GnalkParticipantTile *self,
                                                  GdkTexture            *texture);

void gnalk_participant_tile_set_avatar (GnalkParticipantTile *self,
                                          GdkPaintable          *avatar);

void gnalk_participant_tile_set_is_screen_share (GnalkParticipantTile *self,
                                                   gboolean               is_screen_share);

gboolean gnalk_participant_tile_get_is_screen_share (GnalkParticipantTile *self);

G_END_DECLS
