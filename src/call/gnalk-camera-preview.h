/* gnalk-camera-preview.h
 *
 * Reusable camera preview widget.
 * Used by preferences dialog, call window pre-call preview, and in-call local preview.
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define GNALK_TYPE_CAMERA_PREVIEW (gnalk_camera_preview_get_type ())

G_DECLARE_FINAL_TYPE (GnalkCameraPreview, gnalk_camera_preview, GNALK, CAMERA_PREVIEW, GtkWidget)

typedef enum {
  GNALK_CAMERA_PREVIEW_FRONT,
  GNALK_CAMERA_PREVIEW_BACK,
  GNALK_CAMERA_PREVIEW_GENERIC
} GnalkCameraPreviewPosition;

GnalkCameraPreview *gnalk_camera_preview_new (void);

void gnalk_camera_preview_set_position (GnalkCameraPreview         *self,
                                          GnalkCameraPreviewPosition  position);

GnalkCameraPreviewPosition gnalk_camera_preview_get_position (GnalkCameraPreview *self);

void gnalk_camera_preview_start (GnalkCameraPreview *self);
void gnalk_camera_preview_stop  (GnalkCameraPreview *self);

gboolean gnalk_camera_preview_is_running (GnalkCameraPreview *self);

/**
 * gnalk_camera_preview_capture_photo:
 * @self: the camera preview
 *
 * Captures the current frame as JPEG data.
 * Must be called while the preview is running.
 *
 * Returns: (transfer full) (nullable): JPEG-encoded photo data, or NULL on failure
 */
GBytes *gnalk_camera_preview_capture_photo (GnalkCameraPreview *self);

/**
 * gnalk_camera_preview_start_video_recording:
 * @self: the camera preview
 * @output_path: path to save the WebM video
 *
 * Starts recording video while continuing preview.
 * Video is encoded as VP8+Opus in WebM container.
 *
 * Returns: TRUE if recording started successfully
 */
gboolean gnalk_camera_preview_start_video_recording (GnalkCameraPreview *self,
                                                       const char          *output_path);

/**
 * gnalk_camera_preview_stop_video_recording:
 * @self: the camera preview
 *
 * Stops video recording. Returns the recorded video data.
 *
 * Returns: (transfer full) (nullable): WebM video data, or NULL if not recording
 */
GBytes *gnalk_camera_preview_stop_video_recording (GnalkCameraPreview *self);

/**
 * gnalk_camera_preview_is_recording:
 * @self: the camera preview
 *
 * Returns: TRUE if currently recording video
 */
gboolean gnalk_camera_preview_is_recording_video (GnalkCameraPreview *self);

G_END_DECLS
