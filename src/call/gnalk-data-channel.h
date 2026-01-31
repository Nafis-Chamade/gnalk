/* gnalk-data-channel.h
 *
 * WebRTC data channel manager for status message exchange.
 * Handles the "status" data channel used to communicate media state
 * (video/audio enabled) between peers.
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define GNALK_TYPE_DATA_CHANNEL (gnalk_data_channel_get_type ())
G_DECLARE_FINAL_TYPE (GnalkDataChannel, gnalk_data_channel, GNALK, DATA_CHANNEL, GObject)

/**
 * gnalk_data_channel_new:
 *
 * Creates a new data channel manager.
 * Call gnalk_data_channel_set_channel() to attach the actual GStreamer data channel.
 *
 * Returns: (transfer full): a new #GnalkDataChannel
 */
GnalkDataChannel *gnalk_data_channel_new (void);

/**
 * gnalk_data_channel_set_channel:
 * @self: the data channel manager
 * @channel: (nullable): the GObject data channel from webrtcbin's
 *           "create-data-channel" or "on-data-channel" signal
 *
 * Sets the underlying WebRTC data channel. The manager will connect to
 * the channel's signals (on-open, on-close, on-error, on-message-string).
 *
 * When the channel opens, any queued messages will be sent.
 * Pass %NULL to disconnect from the current channel.
 */
void gnalk_data_channel_set_channel (GnalkDataChannel *self,
                                      GObject           *channel);

/**
 * gnalk_data_channel_set_remote_channel:
 * @self: the data channel manager
 * @channel: the GObject data channel from webrtcbin's "on-data-channel" signal
 *
 * Adds a remote-created data channel for receiving messages.
 * This is separate from the local channel we create.
 */
void gnalk_data_channel_set_remote_channel (GnalkDataChannel *self,
                                             GObject           *channel);

/**
 * gnalk_data_channel_send_video_status:
 * @self: the data channel manager
 * @enabled: TRUE if video is enabled
 *
 * Sends a videoOn or videoOff message on the data channel.
 * If the channel is not yet open, the message will be queued.
 */
void gnalk_data_channel_send_video_status (GnalkDataChannel *self,
                                            gboolean           enabled);

/**
 * gnalk_data_channel_send_audio_status:
 * @self: the data channel manager
 * @enabled: TRUE if audio is enabled
 *
 * Sends an audioOn or audioOff message on the data channel.
 * If the channel is not yet open, the message will be queued.
 */
void gnalk_data_channel_send_audio_status (GnalkDataChannel *self,
                                            gboolean           enabled);

/**
 * gnalk_data_channel_start_periodic_status:
 * @self: the data channel manager
 * @video_enabled: current video state
 * @audio_enabled: current audio state
 *
 * Starts a periodic timer to resend status messages every 2 seconds.
 * This is needed for Android compatibility because Janus doesn't replay
 * data channel messages to late-joining subscribers.
 */
void gnalk_data_channel_start_periodic_status (GnalkDataChannel *self,
                                                gboolean           video_enabled,
                                                gboolean           audio_enabled);

/**
 * gnalk_data_channel_stop_periodic_status:
 * @self: the data channel manager
 *
 * Stops the periodic status timer.
 */
void gnalk_data_channel_stop_periodic_status (GnalkDataChannel *self);

/**
 * gnalk_data_channel_update_status:
 * @self: the data channel manager
 * @video_enabled: current video state
 * @audio_enabled: current audio state
 *
 * Updates the cached status values used by the periodic timer.
 * Also sends the updated status immediately if the channel is open.
 */
void gnalk_data_channel_update_status (GnalkDataChannel *self,
                                        gboolean           video_enabled,
                                        gboolean           audio_enabled);

/**
 * gnalk_data_channel_is_open:
 * @self: the data channel manager
 *
 * Returns: TRUE if the data channel is open and ready to send messages
 */
gboolean gnalk_data_channel_is_open (GnalkDataChannel *self);

/*
 * Signals:
 *
 * GnalkDataChannel::speaking-changed
 * @self: the data channel manager
 * @speaking: TRUE if remote peer started speaking, FALSE if stopped
 *
 * Emitted when a speaking/stoppedSpeaking message is received.
 *
 * GnalkDataChannel::channel-opened
 * @self: the data channel manager
 *
 * Emitted when the data channel becomes open.
 *
 * GnalkDataChannel::channel-closed
 * @self: the data channel manager
 *
 * Emitted when the data channel is closed.
 */

G_END_DECLS
