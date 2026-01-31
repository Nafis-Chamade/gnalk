/* gnalk-media-capture-dialog.h
 *
 * Full-screen dialog for photo and video capture.
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define GNALK_TYPE_MEDIA_CAPTURE_DIALOG (gnalk_media_capture_dialog_get_type ())
G_DECLARE_FINAL_TYPE (GnalkMediaCaptureDialog, gnalk_media_capture_dialog, GNALK, MEDIA_CAPTURE_DIALOG, AdwDialog)

/**
 * GnalkMediaCaptureDialog:
 *
 * Full-screen dialog for capturing photos and videos.
 * Uses GnalkCameraPreview for camera display.
 *
 * Signals:
 * - "photo-captured" (GBytes *jpeg_data) - Photo captured successfully
 * - "video-captured" (GBytes *webm_data, gdouble duration) - Video recorded
 */

GnalkMediaCaptureDialog *gnalk_media_capture_dialog_new (void);

G_END_DECLS
