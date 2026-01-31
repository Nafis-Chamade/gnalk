/* gnalk-video-player.h
 *
 * Reusable video player widget with play/pause, seekbar, and fullscreen support.
 * Follows same patterns as GnalkAudioPlayer.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNALK_TYPE_VIDEO_PLAYER (gnalk_video_player_get_type ())
G_DECLARE_FINAL_TYPE (GnalkVideoPlayer, gnalk_video_player, GNALK, VIDEO_PLAYER, GtkBox)

GnalkVideoPlayer *gnalk_video_player_new (void);

/**
 * gnalk_video_player_set_file:
 * @self: the video player
 * @file_path: path to local video file
 *
 * Sets a video file to play. The file must be a local path.
 */
void gnalk_video_player_set_file (GnalkVideoPlayer *self,
                                   const char        *file_path);

/**
 * gnalk_video_player_set_data:
 * @self: the video player
 * @data: video data as GBytes
 *
 * Sets video data directly (for preview mode after recording).
 * Writes to a temp file internally for GStreamer playback.
 */
void gnalk_video_player_set_data (GnalkVideoPlayer *self,
                                   GBytes            *data);

/**
 * gnalk_video_player_play:
 * @self: the video player
 *
 * Starts or resumes playback.
 */
void gnalk_video_player_play (GnalkVideoPlayer *self);

/**
 * gnalk_video_player_pause:
 * @self: the video player
 *
 * Pauses playback.
 */
void gnalk_video_player_pause (GnalkVideoPlayer *self);

/**
 * gnalk_video_player_stop:
 * @self: the video player
 *
 * Stops playback and resets to beginning.
 */
void gnalk_video_player_stop (GnalkVideoPlayer *self);

/**
 * gnalk_video_player_set_loading:
 * @self: the video player
 * @loading: whether loading state is active
 *
 * Shows/hides loading spinner during video download.
 */
void gnalk_video_player_set_loading (GnalkVideoPlayer *self,
                                      gboolean           loading);

/**
 * gnalk_video_player_is_playing:
 * @self: the video player
 *
 * Returns: TRUE if video is currently playing
 */
gboolean gnalk_video_player_is_playing (GnalkVideoPlayer *self);

/**
 * gnalk_video_player_get_duration:
 * @self: the video player
 *
 * Returns: duration in seconds, or 0 if not available
 */
double gnalk_video_player_get_duration (GnalkVideoPlayer *self);

G_END_DECLS
