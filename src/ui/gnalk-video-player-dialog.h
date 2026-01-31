/* gnalk-video-player-dialog.h
 *
 * Dialog wrapper for playing received video files.
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define GNALK_TYPE_VIDEO_PLAYER_DIALOG (gnalk_video_player_dialog_get_type ())
G_DECLARE_FINAL_TYPE (GnalkVideoPlayerDialog, gnalk_video_player_dialog, GNALK, VIDEO_PLAYER_DIALOG, AdwDialog)

GnalkVideoPlayerDialog *gnalk_video_player_dialog_new (void);

/**
 * gnalk_video_player_dialog_set_file:
 * @self: the dialog
 * @file_path: path to video file
 * @title: optional title (file name)
 *
 * Sets the video file to play. Starts playback automatically.
 */
void gnalk_video_player_dialog_set_file (GnalkVideoPlayerDialog *self,
                                          const char              *file_path,
                                          const char              *title);

G_END_DECLS
