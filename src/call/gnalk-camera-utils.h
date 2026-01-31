/* gnalk-camera-utils.h
 *
 * Shared camera detection and rotation utilities.
 * Used by both media pipeline and preferences dialog.
 */

#pragma once

#include <glib.h>

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#endif

G_BEGIN_DECLS

/**
 * GnalkCameraPosition:
 * @GNALK_CAMERA_FRONT: Front-facing camera
 * @GNALK_CAMERA_BACK: Back-facing camera
 * @GNALK_CAMERA_GENERIC: Generic/unknown camera (laptop, desktop webcam)
 */
typedef enum {
  GNALK_CAMERA_FRONT = 0,
  GNALK_CAMERA_BACK = 1,
  GNALK_CAMERA_GENERIC = 2,
} GnalkCameraPosition;

/**
 * GnalkCameraQuality:
 * @GNALK_CAMERA_QUALITY_LOW: Low quality (1080x1440, fast encoding)
 * @GNALK_CAMERA_QUALITY_MEDIUM: Medium quality (1440x1920)
 * @GNALK_CAMERA_QUALITY_HIGH: High quality (2580x3440, best FOV)
 */
typedef enum {
  GNALK_CAMERA_QUALITY_LOW = 0,
  GNALK_CAMERA_QUALITY_MEDIUM = 1,
  GNALK_CAMERA_QUALITY_HIGH = 2,
} GnalkCameraQuality;

/**
 * GnalkCameraConfig:
 * @src_width: Camera source width to request (before rotation, portrait)
 * @src_height: Camera source height to request (before rotation, portrait)
 * @preview_crop: Pixels to crop from each side for preview
 * @encode_crop: Pixels to crop from each side for encoding
 *
 * Configuration parameters for camera pipeline based on quality setting.
 */
typedef struct {
  gint src_width;
  gint src_height;
  gint preview_crop;
  gint encode_crop;
} GnalkCameraConfig;

/**
 * gnalk_camera_get_config:
 * @quality: the quality preset
 * @position: front or back camera
 *
 * Returns camera pipeline configuration for the given quality and position.
 *
 * Returns: configuration struct with resolution and crop values
 */
GnalkCameraConfig gnalk_camera_get_config (GnalkCameraQuality quality,
                                              GnalkCameraPosition position);

/**
 * gnalk_camera_quality_from_settings:
 *
 * Reads the camera quality setting from GSettings.
 *
 * Returns: the configured quality preset
 */
GnalkCameraQuality gnalk_camera_quality_from_settings (void);

/**
 * gnalk_camera_get_rotation_from_device:
 * @device: a GstDevice representing a camera
 * @out_is_front: (out) (optional): location to store whether this is a front camera
 * @out_rotation: (out) (optional): location to store the sensor rotation angle
 *
 * Queries camera rotation and position from device properties.
 * First tries api.libcamera.location and api.libcamera.rotation properties,
 * then falls back to device name matching with OnePlus 6 defaults.
 */
#ifdef HAVE_GSTREAMER
void gnalk_camera_get_rotation_from_device (GstDevice *device,
                                              gboolean  *out_is_front,
                                              gint      *out_rotation);
#endif

/**
 * gnalk_camera_rotation_to_video_direction:
 * @rotation: sensor rotation angle (0, 90, 180, or 270)
 *
 * Converts a sensor rotation angle to GStreamer videoflip video-direction value.
 *
 * Returns: video-direction value (0=identity, 1=90cw, 2=180, 3=90ccw)
 */
gint gnalk_camera_rotation_to_video_direction (gint rotation);

/**
 * gnalk_camera_utils_create_audio_source:
 *
 * Creates a GStreamer audio source element suitable for recording.
 * Uses pulsesrc if available, falls back to audiotestsrc.
 * Caller owns the returned element.
 *
 * Returns: (transfer full) (nullable): A new audio source element, or NULL on failure
 */
#ifdef HAVE_GSTREAMER
GstElement *gnalk_camera_utils_create_audio_source (void);
#endif

G_END_DECLS
