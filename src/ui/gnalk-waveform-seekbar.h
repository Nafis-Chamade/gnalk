#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNALK_TYPE_WAVEFORM_SEEKBAR (gnalk_waveform_seekbar_get_type ())
G_DECLARE_FINAL_TYPE (GnalkWaveformSeekbar, gnalk_waveform_seekbar, GNALK, WAVEFORM_SEEKBAR, GtkWidget)

GnalkWaveformSeekbar *gnalk_waveform_seekbar_new (void);

/* Set waveform data (array of amplitudes 0.0-1.0) */
void gnalk_waveform_seekbar_set_waveform (GnalkWaveformSeekbar *self,
                                           const float           *data,
                                           gsize                  size);

/* Set playback progress (0.0-1.0) */
void gnalk_waveform_seekbar_set_progress (GnalkWaveformSeekbar *self,
                                           double                 progress);

/* Get current progress */
double gnalk_waveform_seekbar_get_progress (GnalkWaveformSeekbar *self);

/* Set sensitive state for seeking */
void gnalk_waveform_seekbar_set_sensitive (GnalkWaveformSeekbar *self,
                                            gboolean               sensitive);

G_END_DECLS
