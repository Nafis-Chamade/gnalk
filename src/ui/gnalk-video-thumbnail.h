/* gnalk-video-thumbnail.h
 *
 * Widget that displays first frame of a video with play overlay icon.
 * Used for video messages in the message list.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNALK_TYPE_VIDEO_THUMBNAIL (gnalk_video_thumbnail_get_type ())
G_DECLARE_FINAL_TYPE (GnalkVideoThumbnail, gnalk_video_thumbnail, GNALK, VIDEO_THUMBNAIL, GtkWidget)

GnalkVideoThumbnail *gnalk_video_thumbnail_new (void);

/**
 * gnalk_video_thumbnail_set_file:
 * @self: the thumbnail widget
 * @file_path: path to video file
 *
 * Sets the video file to extract thumbnail from.
 * The thumbnail is extracted asynchronously.
 */
void gnalk_video_thumbnail_set_file (GnalkVideoThumbnail *self,
                                      const char           *file_path);

/**
 * gnalk_video_thumbnail_set_file_id:
 * @self: the thumbnail widget
 * @file_id: unique file ID for caching
 *
 * Sets the file ID used for thumbnail caching.
 * Should be called before set_file for proper cache lookup.
 */
void gnalk_video_thumbnail_set_file_id (GnalkVideoThumbnail *self,
                                         const char           *file_id);

/**
 * gnalk_video_thumbnail_set_loading:
 * @self: the thumbnail widget
 * @loading: whether to show loading state
 *
 * Shows/hides loading spinner.
 */
void gnalk_video_thumbnail_set_loading (GnalkVideoThumbnail *self,
                                         gboolean              loading);

/**
 * gnalk_video_thumbnail_set_texture:
 * @self: the thumbnail widget
 * @texture: (nullable): thumbnail texture from server preview
 *
 * Sets the thumbnail texture directly (e.g., from server-generated preview).
 * Shows play icon overlay when texture is set.
 */
void gnalk_video_thumbnail_set_texture (GnalkVideoThumbnail *self,
                                         GdkTexture           *texture);

/**
 * gnalk_video_thumbnail_extract_frame:
 * @file_path: path to video file
 *
 * Synchronously extracts first frame from video file.
 * Returns a new GdkTexture or NULL on error.
 * Caller owns the returned texture.
 */
GdkTexture *gnalk_video_thumbnail_extract_frame (const char *file_path);

G_END_DECLS
