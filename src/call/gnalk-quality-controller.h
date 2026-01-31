/* gnalk-quality-controller.h
 *
 * Video quality monitoring and adaptation for WebRTC calls.
 * Handles PLI (Picture Loss Indication) requests, adaptive bitrate control,
 * and RTP diagnostics for congestion detection.
 */

#pragma once

#include <glib-object.h>

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#endif

G_BEGIN_DECLS

#define GNALK_TYPE_QUALITY_CONTROLLER (gnalk_quality_controller_get_type ())
G_DECLARE_FINAL_TYPE (GnalkQualityController, gnalk_quality_controller, GNALK, QUALITY_CONTROLLER, GObject)

/**
 * gnalk_quality_controller_new:
 *
 * Creates a new quality controller for managing video quality.
 *
 * Returns: (transfer full): a new #GnalkQualityController
 */
GnalkQualityController *gnalk_quality_controller_new (void);

#ifdef HAVE_GSTREAMER

/**
 * gnalk_quality_controller_set_webrtcbin:
 * @self: the quality controller
 * @webrtcbin: the webrtcbin element for stats collection
 *
 * Sets the webrtcbin element used for collecting RTP statistics.
 * Call this after creating the WebRTC pipeline.
 */
void gnalk_quality_controller_set_webrtcbin (GnalkQualityController *self,
                                               GstElement              *webrtcbin);

/**
 * gnalk_quality_controller_set_video_encoder:
 * @self: the quality controller
 * @encoder: the video encoder element
 *
 * Sets the video encoder element for adaptive bitrate control.
 * The encoder must have a "target-bitrate" property.
 */
void gnalk_quality_controller_set_video_encoder (GnalkQualityController *self,
                                                   GstElement              *encoder);

/**
 * gnalk_quality_controller_set_pli_pad:
 * @self: the quality controller
 * @pad: the pad for sending PLI events upstream
 *
 * Sets the pad used for sending PLI (keyframe request) events.
 * Typically the sink pad of an appsink receiving decoded video.
 */
void gnalk_quality_controller_set_pli_pad (GnalkQualityController *self,
                                             GstPad                  *pad);

/**
 * gnalk_quality_controller_create_rtp_probe:
 * @self: the quality controller
 *
 * Creates a pad probe callback for RTP packet analysis.
 * Attach this probe to the sink pad of the RTP depayloader.
 *
 * Returns: the probe callback function
 */
GstPadProbeCallback gnalk_quality_controller_get_rtp_probe (GnalkQualityController *self);

/**
 * gnalk_quality_controller_create_depay_probe:
 * @self: the quality controller
 *
 * Creates a pad probe callback for depayloader output analysis.
 * Attach this probe to the src pad of the RTP depayloader.
 *
 * Returns: the probe callback function
 */
GstPadProbeCallback gnalk_quality_controller_get_depay_probe (GnalkQualityController *self);

#endif /* HAVE_GSTREAMER */

/**
 * gnalk_quality_controller_start_bitrate_timer:
 * @self: the quality controller
 *
 * Starts the adaptive bitrate timer. This periodically checks
 * packet loss and adjusts the encoder bitrate accordingly.
 */
void gnalk_quality_controller_start_bitrate_timer (GnalkQualityController *self);

/**
 * gnalk_quality_controller_stop_bitrate_timer:
 * @self: the quality controller
 *
 * Stops the adaptive bitrate timer.
 */
void gnalk_quality_controller_stop_bitrate_timer (GnalkQualityController *self);

/**
 * gnalk_quality_controller_start_pli_timer:
 * @self: the quality controller
 *
 * Starts the initial PLI timer for requesting keyframes until
 * video is successfully decoded. Stops automatically once video
 * is received.
 */
void gnalk_quality_controller_start_pli_timer (GnalkQualityController *self);

/**
 * gnalk_quality_controller_stop_pli_timer:
 * @self: the quality controller
 *
 * Stops the PLI timer.
 */
void gnalk_quality_controller_stop_pli_timer (GnalkQualityController *self);

/**
 * gnalk_quality_controller_enable_continuous_pli:
 * @self: the quality controller
 * @enable: whether to enable continuous PLI
 * @interval_ms: interval between PLI requests in milliseconds
 *
 * Enables or disables continuous PLI requests. Useful for screen
 * sharing where keyframes improve quality but can cause congestion
 * collapse if overused. The controller monitors for congestion and
 * automatically backs off when detected.
 */
void gnalk_quality_controller_enable_continuous_pli (GnalkQualityController *self,
                                                       gboolean                 enable,
                                                       guint                    interval_ms);

/**
 * gnalk_quality_controller_send_pli:
 * @self: the quality controller
 *
 * Sends an immediate PLI (keyframe request) upstream.
 *
 * Returns: TRUE if the PLI was sent successfully
 */
gboolean gnalk_quality_controller_send_pli (GnalkQualityController *self);

/**
 * gnalk_quality_controller_set_video_received:
 * @self: the quality controller
 * @received: whether video has been successfully decoded
 *
 * Notifies the controller that video has been successfully decoded.
 * This stops the initial PLI timer.
 */
void gnalk_quality_controller_set_video_received (GnalkQualityController *self,
                                                    gboolean                 received);

/**
 * gnalk_quality_controller_reset_stats:
 * @self: the quality controller
 *
 * Resets all diagnostic counters. Call this when starting a new call.
 */
void gnalk_quality_controller_reset_stats (GnalkQualityController *self);

/**
 * gnalk_quality_controller_get_keyframe_count:
 * @self: the quality controller
 *
 * Gets the number of keyframes received.
 *
 * Returns: keyframe count
 */
guint gnalk_quality_controller_get_keyframe_count (GnalkQualityController *self);

/**
 * gnalk_quality_controller_get_packet_loss_count:
 * @self: the quality controller
 *
 * Gets the number of RTP sequence gaps (packet loss indicator).
 *
 * Returns: packet loss count
 */
guint gnalk_quality_controller_get_packet_loss_count (GnalkQualityController *self);

/**
 * gnalk_quality_controller_get_current_bitrate:
 * @self: the quality controller
 *
 * Gets the current encoder bitrate.
 *
 * Returns: current bitrate in bits per second
 */
guint gnalk_quality_controller_get_current_bitrate (GnalkQualityController *self);

/*
 * Signals:
 *
 * GnalkQualityController::quality-degraded
 * @self: the quality controller
 * @reason: description of the quality issue
 *
 * Emitted when video quality degrades significantly.
 *
 * GnalkQualityController::stats-updated
 * @self: the quality controller
 * @bitrate: current bitrate in kbps
 * @packet_loss_pct: packet loss percentage
 * @keyframe_pct: keyframe percentage
 *
 * Emitted when statistics are updated.
 */

G_END_DECLS
