#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNALK_TYPE_AUDIO_PLAYER (gnalk_audio_player_get_type ())
G_DECLARE_FINAL_TYPE (GnalkAudioPlayer, gnalk_audio_player, GNALK, AUDIO_PLAYER, GtkBox)

GnalkAudioPlayer *gnalk_audio_player_new (void);

/* Set file path (local, already downloaded) */
void gnalk_audio_player_set_file (GnalkAudioPlayer *self, const char *file_path);

/* Show loading state during download */
void gnalk_audio_player_set_loading (GnalkAudioPlayer *self, gboolean loading);

/* Stop playback (called when widget is hidden or destroyed) */
void gnalk_audio_player_stop (GnalkAudioPlayer *self);

G_END_DECLS
