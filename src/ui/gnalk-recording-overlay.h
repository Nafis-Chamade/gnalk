/* gnalk-recording-overlay.h
 *
 * Voice recording overlay that replaces message input during recording.
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define GNALK_TYPE_RECORDING_OVERLAY (gnalk_recording_overlay_get_type ())
G_DECLARE_FINAL_TYPE (GnalkRecordingOverlay, gnalk_recording_overlay, GNALK, RECORDING_OVERLAY, GtkWidget)

/**
 * GnalkRecordingOverlay:
 *
 * UI overlay for voice recording. Shows:
 * - Cancel button (X)
 * - Waveform visualization
 * - Recording duration
 * - Send button (checkmark)
 *
 * Signals:
 * - "cancelled" - User pressed cancel
 * - "send-requested" (GBytes *audio_data, gdouble duration) - User pressed send
 */

GnalkRecordingOverlay *gnalk_recording_overlay_new (void);

/**
 * gnalk_recording_overlay_start:
 * @self: the overlay
 *
 * Starts recording and shows the overlay.
 *
 * Returns: TRUE if recording started successfully
 */
gboolean gnalk_recording_overlay_start (GnalkRecordingOverlay *self);

/**
 * gnalk_recording_overlay_is_recording:
 * @self: the overlay
 *
 * Returns: TRUE if currently recording
 */
gboolean gnalk_recording_overlay_is_recording (GnalkRecordingOverlay *self);

G_END_DECLS
