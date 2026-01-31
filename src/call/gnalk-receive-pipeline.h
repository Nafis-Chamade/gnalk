/* gnalk-receive-pipeline.h
 *
 * GStreamer receive pipeline for handling incoming WebRTC streams.
 * Creates video/audio decoder chains, extracts frames, and emits
 * signals for the application layer.
 */

#pragma once

#include <glib-object.h>
#include <gdk/gdk.h>

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#endif

G_BEGIN_DECLS

#define GNALK_TYPE_RECEIVE_PIPELINE (gnalk_receive_pipeline_get_type ())
G_DECLARE_FINAL_TYPE (GnalkReceivePipeline, gnalk_receive_pipeline, GNALK, RECEIVE_PIPELINE, GObject)

/* Forward declaration */
typedef struct _GnalkQualityController GnalkQualityController;

/**
 * gnalk_receive_pipeline_new:
 *
 * Creates a new receive pipeline manager.
 *
 * Returns: (transfer full): a new #GnalkReceivePipeline
 */
GnalkReceivePipeline *gnalk_receive_pipeline_new (void);

#ifdef HAVE_GSTREAMER

/**
 * gnalk_receive_pipeline_set_pipeline:
 * @self: the receive pipeline manager
 * @pipeline: the parent GStreamer pipeline
 *
 * Sets the parent pipeline where decoder elements will be added.
 * Must be called before handling any incoming streams.
 */
void gnalk_receive_pipeline_set_pipeline (GnalkReceivePipeline *self,
                                            GstElement            *pipeline);

/**
 * gnalk_receive_pipeline_set_quality_controller:
 * @self: the receive pipeline manager
 * @controller: the quality controller for PLI management
 *
 * Sets the quality controller for managing PLI requests and RTP diagnostics.
 */
void gnalk_receive_pipeline_set_quality_controller (GnalkReceivePipeline   *self,
                                                      GnalkQualityController *controller);

/**
 * gnalk_receive_pipeline_handle_pad_added:
 * @self: the receive pipeline manager
 * @pad: the new pad from webrtcbin
 *
 * Handles a new incoming stream pad from webrtcbin.
 * Creates appropriate decoder chain based on media type (VP8 video, OPUS audio).
 */
void gnalk_receive_pipeline_handle_pad_added (GnalkReceivePipeline *self,
                                                GstPad                *pad);

/**
 * gnalk_receive_pipeline_get_video_depay:
 * @self: the receive pipeline manager
 *
 * Gets the video depayloader element (needed for PLI pad setup).
 *
 * Returns: (transfer none) (nullable): the depayloader element, or NULL
 */
GstElement *gnalk_receive_pipeline_get_video_depay (GnalkReceivePipeline *self);

/**
 * gnalk_receive_pipeline_get_video_sink:
 * @self: the receive pipeline manager
 *
 * Gets the video appsink element.
 *
 * Returns: (transfer none) (nullable): the appsink element, or NULL
 */
GstElement *gnalk_receive_pipeline_get_video_sink (GnalkReceivePipeline *self);

/**
 * gnalk_receive_pipeline_destroy:
 * @self: the receive pipeline manager
 *
 * Destroys all receive pipeline elements and releases resources.
 */
void gnalk_receive_pipeline_destroy (GnalkReceivePipeline *self);

#endif /* HAVE_GSTREAMER */

/**
 * gnalk_receive_pipeline_reset_frame_counters:
 * @self: the receive pipeline manager
 *
 * Resets frame counters. Call this when starting a new call.
 */
void gnalk_receive_pipeline_reset_frame_counters (GnalkReceivePipeline *self);

/**
 * gnalk_receive_pipeline_has_video:
 * @self: the receive pipeline manager
 *
 * Returns: TRUE if video has been successfully decoded
 */
gboolean gnalk_receive_pipeline_has_video (GnalkReceivePipeline *self);

/*
 * Signals:
 *
 * GnalkReceivePipeline::remote-video-frame
 * @self: the receive pipeline
 * @texture: (transfer none): the video frame as a GdkTexture
 *
 * Emitted when a new remote video frame is available.
 */

G_END_DECLS
