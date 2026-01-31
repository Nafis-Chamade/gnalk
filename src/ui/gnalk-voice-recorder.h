/* gnalk-voice-recorder.h
 *
 * GStreamer-based voice recorder for voice messages.
 * Records audio to Opus/OGG format.
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define GNALK_TYPE_VOICE_RECORDER (gnalk_voice_recorder_get_type ())
G_DECLARE_FINAL_TYPE (GnalkVoiceRecorder, gnalk_voice_recorder, GNALK, VOICE_RECORDER, GObject)

/**
 * GnalkVoiceRecorder:
 *
 * Records voice messages using GStreamer.
 * Uses pulsesrc for audio capture and Opus encoder for compression.
 *
 * Signals:
 * - "level-updated" (gdouble level) - Audio level (0.0-1.0) for waveform visualization
 * - "recording-stopped" (GBytes *audio_data, gdouble duration) - Recording finished
 * - "error" (const char *message) - Recording error occurred
 */

GnalkVoiceRecorder *gnalk_voice_recorder_new (void);

/**
 * gnalk_voice_recorder_start:
 * @self: the voice recorder
 *
 * Starts recording audio. Emits "level-updated" signals during recording.
 *
 * Returns: TRUE if recording started successfully, FALSE on error
 */
gboolean gnalk_voice_recorder_start (GnalkVoiceRecorder *self);

/**
 * gnalk_voice_recorder_stop:
 * @self: the voice recorder
 *
 * Stops recording and emits "recording-stopped" with the recorded audio data.
 */
void gnalk_voice_recorder_stop (GnalkVoiceRecorder *self);

/**
 * gnalk_voice_recorder_stop_for_preview:
 * @self: the voice recorder
 *
 * Stops recording but keeps the file for preview playback.
 * Call gnalk_voice_recorder_play() to play it back,
 * or gnalk_voice_recorder_get_preview_data() to get the audio data.
 *
 * Returns: TRUE if stopped successfully
 */
gboolean gnalk_voice_recorder_stop_for_preview (GnalkVoiceRecorder *self);

/**
 * gnalk_voice_recorder_play:
 * @self: the voice recorder
 *
 * Plays back the recorded audio. Emits "playback-finished" when done.
 *
 * Returns: TRUE if playback started
 */
gboolean gnalk_voice_recorder_play (GnalkVoiceRecorder *self);

/**
 * gnalk_voice_recorder_pause:
 * @self: the voice recorder
 *
 * Pauses or resumes playback.
 */
void gnalk_voice_recorder_pause (GnalkVoiceRecorder *self);

/**
 * gnalk_voice_recorder_get_preview_data:
 * @self: the voice recorder
 * @out_duration: (out) (optional): location to store duration in seconds
 *
 * Gets the recorded audio data after stop_for_preview().
 *
 * Returns: (transfer full) (nullable): The audio data as GBytes, or NULL on error
 */
GBytes *gnalk_voice_recorder_get_preview_data (GnalkVoiceRecorder *self,
                                                 gdouble             *out_duration);

/**
 * gnalk_voice_recorder_cancel:
 * @self: the voice recorder
 *
 * Cancels recording without producing output.
 */
void gnalk_voice_recorder_cancel (GnalkVoiceRecorder *self);

/**
 * gnalk_voice_recorder_is_recording:
 * @self: the voice recorder
 *
 * Returns: TRUE if currently recording
 */
gboolean gnalk_voice_recorder_is_recording (GnalkVoiceRecorder *self);

/**
 * gnalk_voice_recorder_is_playing:
 * @self: the voice recorder
 *
 * Returns: TRUE if currently playing back
 */
gboolean gnalk_voice_recorder_is_playing (GnalkVoiceRecorder *self);

/**
 * gnalk_voice_recorder_has_preview:
 * @self: the voice recorder
 *
 * Returns: TRUE if a preview recording is available for playback
 */
gboolean gnalk_voice_recorder_has_preview (GnalkVoiceRecorder *self);

/**
 * gnalk_voice_recorder_get_duration:
 * @self: the voice recorder
 *
 * Returns: Current recording duration in seconds
 */
gdouble gnalk_voice_recorder_get_duration (GnalkVoiceRecorder *self);

G_END_DECLS
