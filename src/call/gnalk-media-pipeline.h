/* gnalk-media-pipeline.h
 *
 * GStreamer media pipeline construction and management.
 * Handles camera source setup, video/audio encoding, local preview,
 * and camera switching for WebRTC calls.
 */

#pragma once

#include <glib-object.h>
#include <gdk/gdk.h>
#include "gnalk-camera-utils.h"  /* For GnalkCameraPosition, GnalkCameraConfig */

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#endif

G_BEGIN_DECLS

#define GNALK_TYPE_MEDIA_PIPELINE (gnalk_media_pipeline_get_type ())
G_DECLARE_FINAL_TYPE (GnalkMediaPipeline, gnalk_media_pipeline, GNALK, MEDIA_PIPELINE, GObject)

/**
 * GnalkMediaPipelineType:
 * @GNALK_MEDIA_PIPELINE_SEND: Full send pipeline (camera + audio)
 * @GNALK_MEDIA_PIPELINE_RECEIVE: Receive-only pipeline
 * @GNALK_MEDIA_PIPELINE_SCREEN_SHARE: Screen share pipeline (video only)
 *
 * The type of media pipeline to create.
 */
typedef enum {
  GNALK_MEDIA_PIPELINE_SEND,
  GNALK_MEDIA_PIPELINE_RECEIVE,
  GNALK_MEDIA_PIPELINE_SCREEN_SHARE
} GnalkMediaPipelineType;

/**
 * gnalk_media_pipeline_new:
 *
 * Creates a new media pipeline manager.
 *
 * Returns: (transfer full): a new #GnalkMediaPipeline
 */
GnalkMediaPipeline *gnalk_media_pipeline_new (void);

#ifdef HAVE_GSTREAMER

/**
 * gnalk_media_pipeline_create:
 * @self: the media pipeline manager
 * @type: the type of pipeline to create
 * @webrtcbin: the webrtcbin element to link to
 * @camera_position: initial camera position (ignored for screen share)
 *
 * Creates the media pipeline elements and links them to webrtcbin.
 * For SEND type, creates camera + audio pipelines with local preview.
 * For RECEIVE type, no source elements are created.
 * For SCREEN_SHARE type, creates screen capture pipeline.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean gnalk_media_pipeline_create (GnalkMediaPipeline  *self,
                                       GnalkMediaPipelineType type,
                                       GstElement           *webrtcbin,
                                       GnalkCameraPosition  camera_position);

/**
 * gnalk_media_pipeline_create_screen_share:
 * @self: the media pipeline manager
 * @webrtcbin: the webrtcbin element to link to
 * @pipewire_node_id: PipeWire node ID from screen capture portal
 * @pipewire_fd: PipeWire file descriptor from screen capture portal
 *
 * Creates a screen share pipeline using PipeWire source.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean gnalk_media_pipeline_create_screen_share (GnalkMediaPipeline *self,
                                                    GstElement          *webrtcbin,
                                                    guint32              pipewire_node_id,
                                                    int                  pipewire_fd);

/**
 * gnalk_media_pipeline_stop_camera:
 * @self: the media pipeline manager
 *
 * Stops the camera source gracefully, allowing libcamera to drain
 * queued frame requests. Must be called before destroying the pipeline
 * to prevent libcamera crashes during shutdown.
 */
void gnalk_media_pipeline_stop_camera (GnalkMediaPipeline *self);

/**
 * gnalk_media_pipeline_destroy:
 * @self: the media pipeline manager
 *
 * Destroys all pipeline elements and releases resources.
 * The webrtcbin is NOT destroyed (owned by caller).
 */
void gnalk_media_pipeline_destroy (GnalkMediaPipeline *self);

/**
 * gnalk_media_pipeline_get_video_bin:
 * @self: the media pipeline manager
 *
 * Gets the video bin element containing all video processing elements.
 *
 * Returns: (transfer none) (nullable): the video bin, or NULL if not created
 */
GstElement *gnalk_media_pipeline_get_video_bin (GnalkMediaPipeline *self);

/**
 * gnalk_media_pipeline_get_video_encoder:
 * @self: the media pipeline manager
 *
 * Gets the video encoder element for bitrate control.
 *
 * Returns: (transfer none) (nullable): the encoder element, or NULL
 */
GstElement *gnalk_media_pipeline_get_video_encoder (GnalkMediaPipeline *self);

/**
 * gnalk_media_pipeline_get_audio_volume:
 * @self: the media pipeline manager
 *
 * Gets the audio volume element for mute control.
 *
 * Returns: (transfer none) (nullable): the volume element, or NULL
 */
GstElement *gnalk_media_pipeline_get_audio_volume (GnalkMediaPipeline *self);

/**
 * gnalk_media_pipeline_get_local_preview:
 * @self: the media pipeline manager
 *
 * Gets the local preview appsink element.
 *
 * Returns: (transfer none) (nullable): the appsink element, or NULL
 */
GstElement *gnalk_media_pipeline_get_local_preview (GnalkMediaPipeline *self);

#endif /* HAVE_GSTREAMER */

/**
 * gnalk_media_pipeline_switch_camera:
 * @self: the media pipeline manager
 *
 * Switches between front and back camera during a call.
 * Uses input-selector for seamless switching without renegotiation.
 * If no pipeline is active, just toggles the preference for next call.
 */
void gnalk_media_pipeline_switch_camera (GnalkMediaPipeline *self);

/**
 * gnalk_media_pipeline_set_camera_position:
 * @self: the media pipeline manager
 * @position: the camera position to set
 *
 * Sets the camera position preference. Does not switch camera
 * during an active call - use switch_camera() for that.
 */
void gnalk_media_pipeline_set_camera_position (GnalkMediaPipeline  *self,
                                                GnalkCameraPosition  position);

/**
 * gnalk_media_pipeline_get_camera_position:
 * @self: the media pipeline manager
 *
 * Gets the current camera position.
 *
 * Returns: the current camera position
 */
GnalkCameraPosition gnalk_media_pipeline_get_camera_position (GnalkMediaPipeline *self);

/**
 * gnalk_media_pipeline_set_video_enabled:
 * @self: the media pipeline manager
 * @enabled: whether video should be enabled
 *
 * Enables or disables video output. When disabled, sends black frames
 * instead of camera feed.
 */
void gnalk_media_pipeline_set_video_enabled (GnalkMediaPipeline *self,
                                              gboolean             enabled);

/**
 * gnalk_media_pipeline_set_audio_enabled:
 * @self: the media pipeline manager
 * @enabled: whether audio should be enabled
 *
 * Enables or disables audio output by controlling the volume element.
 */
void gnalk_media_pipeline_set_audio_enabled (GnalkMediaPipeline *self,
                                              gboolean             enabled);

/**
 * gnalk_media_pipeline_update_preview_crop:
 * @self: the media pipeline manager
 *
 * Updates the preview crop settings based on current camera position.
 * Front camera typically needs cropping, back camera does not.
 */
void gnalk_media_pipeline_update_preview_crop (GnalkMediaPipeline *self);

/*
 * Signals:
 *
 * GnalkMediaPipeline::local-video-frame
 * @self: the media pipeline
 * @texture: (transfer none): the video frame as a GdkTexture
 *
 * Emitted when a new local video frame is available from the camera preview.
 *
 * GnalkMediaPipeline::pipeline-error
 * @self: the media pipeline
 * @message: error message
 *
 * Emitted when a pipeline error occurs.
 */

G_END_DECLS
