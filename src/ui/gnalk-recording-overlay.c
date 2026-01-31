/* gnalk-recording-overlay.c
 *
 * Voice recording overlay that replaces message input during recording.
 * Supports recording mode and preview mode with playback and seeking.
 */

#include "gnalk-recording-overlay.h"
#include "gnalk-voice-recorder.h"
#include "gnalk-waveform-seekbar.h"

#include <glib/gstdio.h>
#include <unistd.h>

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#endif

#define POSITION_UPDATE_INTERVAL_MS 100

typedef enum {
  MODE_RECORDING,
  MODE_PREVIEW
} OverlayMode;

struct _GnalkRecordingOverlay
{
  GtkWidget parent_instance;

  /* Recording mode widgets */
  GtkWidget *cancel_button;
  GtkWidget *recording_waveform;  /* GtkDrawingArea for recording */
  GtkWidget *duration_label;
  GtkWidget *preview_button;      /* Play/preview the recording */
  GtkWidget *send_button;

  /* Preview mode widgets */
  GtkWidget *rerecord_button;     /* Shown in preview mode */
  GnalkWaveformSeekbar *preview_seekbar;  /* For playback seeking */
  GtkWidget *play_pause_button;   /* Play/pause in preview */

  GnalkVoiceRecorder *recorder;
  guint                duration_timer_id;

  /* Waveform visualization data - stores ALL audio levels for full recording */
  GArray *levels;

  /* Preview mode state */
  OverlayMode mode;
  char       *preview_file_path;  /* Temp file for preview playback */
  gdouble     preview_duration;   /* Duration in seconds */
  GBytes     *preview_audio_data; /* Audio data for sending */

#ifdef HAVE_GSTREAMER
  GstElement *playbin;
  guint       position_update_id;
  gboolean    is_playing;
  gint64      playback_duration;  /* In nanoseconds */
#endif
};

G_DEFINE_FINAL_TYPE (GnalkRecordingOverlay, gnalk_recording_overlay, GTK_TYPE_WIDGET)

enum {
  SIGNAL_CANCELLED,
  SIGNAL_SEND_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void switch_to_mode (GnalkRecordingOverlay *self, OverlayMode mode);
static void stop_playback (GnalkRecordingOverlay *self);
static void on_recording_stopped (GnalkVoiceRecorder *recorder, GBytes *audio_data,
                                  gdouble duration, GnalkRecordingOverlay *self);
static void on_recording_stopped_for_preview (GnalkVoiceRecorder *recorder, GBytes *audio_data,
                                              gdouble duration, GnalkRecordingOverlay *self);

static void
update_duration_label (GnalkRecordingOverlay *self)
{
  gdouble duration = gnalk_voice_recorder_get_duration (self->recorder);
  int minutes = (int) duration / 60;
  int seconds = (int) duration % 60;
  g_autofree char *text = g_strdup_printf ("%d:%02d", minutes, seconds);
  gtk_label_set_label (GTK_LABEL (self->duration_label), text);
}

static void
update_duration_label_from_value (GnalkRecordingOverlay *self, gdouble duration)
{
  int minutes = (int) duration / 60;
  int seconds = (int) duration % 60;
  g_autofree char *text = g_strdup_printf ("%d:%02d", minutes, seconds);
  gtk_label_set_label (GTK_LABEL (self->duration_label), text);
}

static gboolean
on_duration_timer (gpointer user_data)
{
  GnalkRecordingOverlay *self = GNALK_RECORDING_OVERLAY (user_data);

  if (!gnalk_voice_recorder_is_recording (self->recorder))
    {
      self->duration_timer_id = 0;
      return G_SOURCE_REMOVE;
    }

  update_duration_label (self);
  return G_SOURCE_CONTINUE;
}

static void
on_recording_waveform_draw (GtkDrawingArea *area G_GNUC_UNUSED,
                            cairo_t        *cr,
                            int             width,
                            int             height,
                            gpointer        user_data)
{
  GnalkRecordingOverlay *self = GNALK_RECORDING_OVERLAY (user_data);

  /* Transparent background */
  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);

  /* Draw waveform bars */
  cairo_set_source_rgb (cr, 0.3, 0.6, 1.0);  /* Blue accent */

  guint n_levels = self->levels->len;
  if (n_levels == 0)
    return;

  double center_y = height / 2.0;

  /* Preferred bar width - used until we run out of space */
  double preferred_bar_width = 6.0;  /* 5px bar + 1px gap */
  double min_bar_width = 3.0;        /* Minimum when compressing */

  guint max_bars_preferred = (guint) (width / preferred_bar_width);
  guint max_bars_min = (guint) (width / min_bar_width);

  if (n_levels <= max_bars_preferred)
    {
      /* Not full yet: use fixed bar width, grow from left */
      double gap = 1.0;
      double bar_w = preferred_bar_width - gap;

      for (guint i = 0; i < n_levels; i++)
        {
          double level = g_array_index (self->levels, gdouble, i);
          double bar_height = MAX (2.0, level * (height - 4));
          double x = i * preferred_bar_width + gap / 2;
          double y = center_y - bar_height / 2;

          cairo_rectangle (cr, x, y, bar_w, bar_height);
        }
    }
  else if (n_levels <= max_bars_min)
    {
      /* Compressing: shrink bars to fit all samples */
      double bar_width = (double) width / n_levels;
      double gap = MAX (1.0, bar_width * 0.15);

      for (guint i = 0; i < n_levels; i++)
        {
          double level = g_array_index (self->levels, gdouble, i);
          double bar_height = MAX (2.0, level * (height - 4));
          double x = i * bar_width + gap / 2;
          double y = center_y - bar_height / 2;

          cairo_rectangle (cr, x, y, bar_width - gap, bar_height);
        }
    }
  else
    {
      /* Too many samples: group and average to fit */
      double bar_width = (double) width / max_bars_min;
      double gap = 1.0;
      double samples_per_bar = (double) n_levels / max_bars_min;

      for (guint i = 0; i < max_bars_min; i++)
        {
          /* Average samples in this bar's range */
          guint start = (guint) (i * samples_per_bar);
          guint end = (guint) ((i + 1) * samples_per_bar);
          if (end > n_levels) end = n_levels;

          double sum = 0.0;
          for (guint j = start; j < end; j++)
            sum += g_array_index (self->levels, gdouble, j);

          double level = (end > start) ? sum / (end - start) : 0.0;
          double bar_height = MAX (2.0, level * (height - 4));
          double x = i * bar_width + gap / 2;
          double y = center_y - bar_height / 2;

          cairo_rectangle (cr, x, y, bar_width - gap, bar_height);
        }
    }
  cairo_fill (cr);
}

static void
on_level_updated (GnalkVoiceRecorder    *recorder G_GNUC_UNUSED,
                  gdouble                 level,
                  GnalkRecordingOverlay *self)
{
  /* Append level to array (shows full recording history) */
  g_array_append_val (self->levels, level);

  /* Request redraw of waveform */
  gtk_widget_queue_draw (self->recording_waveform);
}

static void
on_recording_stopped (GnalkVoiceRecorder    *recorder G_GNUC_UNUSED,
                      GBytes                 *audio_data,
                      gdouble                 duration,
                      GnalkRecordingOverlay *self)
{
  g_message ("RecordingOverlay: Recording stopped, duration=%.1f", duration);
  g_signal_emit (self, signals[SIGNAL_SEND_REQUESTED], 0, audio_data, duration);
}

static void
on_recorder_error (GnalkVoiceRecorder    *recorder G_GNUC_UNUSED,
                   const char             *message,
                   GnalkRecordingOverlay *self)
{
  g_warning ("RecordingOverlay: Recorder error: %s", message);
  /* Cancel on error */
  g_signal_emit (self, signals[SIGNAL_CANCELLED], 0);
}

static void
on_cancel_clicked (GtkButton              *button G_GNUC_UNUSED,
                   GnalkRecordingOverlay *self)
{
  stop_playback (self);
  gnalk_voice_recorder_cancel (self->recorder);
  g_signal_emit (self, signals[SIGNAL_CANCELLED], 0);
}

static void
on_send_clicked (GtkButton              *button G_GNUC_UNUSED,
                 GnalkRecordingOverlay *self)
{
  if (self->mode == MODE_PREVIEW && self->preview_audio_data)
    {
      /* In preview mode, send the stored audio data */
      stop_playback (self);
      g_signal_emit (self, signals[SIGNAL_SEND_REQUESTED], 0,
                     self->preview_audio_data, self->preview_duration);
    }
  else
    {
      /* Stop recording - this will emit recording-stopped which we handle */
      gnalk_voice_recorder_stop (self->recorder);
    }
}

/* Convert recorded levels to waveform data for seekbar */
static void
prepare_waveform_for_preview (GnalkRecordingOverlay *self)
{
  guint n_levels = self->levels->len;
  if (n_levels == 0)
    return;

  /* Convert GArray<gdouble> to float array for waveform seekbar */
  gsize waveform_size = MIN (n_levels, 50);  /* Target ~50 bars */
  float *waveform_data = g_new0 (float, waveform_size);

  gsize samples_per_bar = n_levels / waveform_size;
  if (samples_per_bar == 0)
    samples_per_bar = 1;

  for (gsize i = 0; i < waveform_size; i++)
    {
      gsize start = i * samples_per_bar;
      gsize end = MIN (start + samples_per_bar, n_levels);

      /* Find max level in this segment */
      gdouble max_level = 0.0;
      for (gsize j = start; j < end; j++)
        {
          gdouble level = g_array_index (self->levels, gdouble, j);
          if (level > max_level)
            max_level = level;
        }

      waveform_data[i] = (float) max_level;
    }

  gnalk_waveform_seekbar_set_waveform (self->preview_seekbar, waveform_data, waveform_size);
  g_free (waveform_data);
}

#ifdef HAVE_GSTREAMER
static void on_seekbar_seek (GnalkWaveformSeekbar *seekbar, double progress, gpointer user_data);

static gboolean
on_position_update (gpointer user_data)
{
  GnalkRecordingOverlay *self = GNALK_RECORDING_OVERLAY (user_data);

  if (!self->playbin || !self->is_playing)
    return G_SOURCE_REMOVE;

  /* Get duration if we don't have it yet */
  if (self->playback_duration <= 0)
    gst_element_query_duration (self->playbin, GST_FORMAT_TIME, &self->playback_duration);

  gint64 position = 0;
  if (gst_element_query_position (self->playbin, GST_FORMAT_TIME, &position))
    {
      /* Update seekbar without triggering seek callback */
      g_signal_handlers_block_by_func (self->preview_seekbar,
                                       G_CALLBACK (on_seekbar_seek), self);

      if (self->playback_duration > 0)
        {
          double fraction = (double) position / (double) self->playback_duration;
          gnalk_waveform_seekbar_set_progress (self->preview_seekbar, fraction);

          /* Update time label */
          gdouble pos_sec = (gdouble) position / GST_SECOND;
          update_duration_label_from_value (self, pos_sec);
        }

      g_signal_handlers_unblock_by_func (self->preview_seekbar,
                                         G_CALLBACK (on_seekbar_seek), self);
    }

  return G_SOURCE_CONTINUE;
}

static void
on_bus_message (GstBus     *bus G_GNUC_UNUSED,
                GstMessage *message,
                gpointer    user_data)
{
  GnalkRecordingOverlay *self = GNALK_RECORDING_OVERLAY (user_data);

  switch (GST_MESSAGE_TYPE (message))
    {
    case GST_MESSAGE_EOS:
      /* End of stream - reset to beginning */
      gst_element_set_state (self->playbin, GST_STATE_PAUSED);
      gst_element_seek_simple (self->playbin, GST_FORMAT_TIME,
                               GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 0);
      self->is_playing = FALSE;
      gtk_button_set_icon_name (GTK_BUTTON (self->play_pause_button),
                                "media-playback-start-symbolic");
      if (self->position_update_id > 0)
        {
          g_source_remove (self->position_update_id);
          self->position_update_id = 0;
        }
      gnalk_waveform_seekbar_set_progress (self->preview_seekbar, 0.0);
      update_duration_label_from_value (self, 0.0);
      break;

    case GST_MESSAGE_ERROR:
      {
        GError *err = NULL;
        gchar *debug_info = NULL;
        gst_message_parse_error (message, &err, &debug_info);
        g_warning ("RecordingOverlay: Playback error: %s", err->message);
        g_error_free (err);
        g_free (debug_info);

        stop_playback (self);
      }
      break;

    case GST_MESSAGE_DURATION_CHANGED:
      gst_element_query_duration (self->playbin, GST_FORMAT_TIME, &self->playback_duration);
      break;

    default:
      break;
    }
}

static void
on_seekbar_seek (GnalkWaveformSeekbar *seekbar G_GNUC_UNUSED,
                 double                 progress,
                 gpointer               user_data)
{
  GnalkRecordingOverlay *self = GNALK_RECORDING_OVERLAY (user_data);

  if (!self->playbin || self->playback_duration <= 0)
    return;

  gint64 position = (gint64) (progress * self->playback_duration);
  gst_element_seek_simple (self->playbin, GST_FORMAT_TIME,
                           GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                           position);

  gdouble pos_sec = (gdouble) position / GST_SECOND;
  update_duration_label_from_value (self, pos_sec);
}

static void
setup_playback_pipeline (GnalkRecordingOverlay *self)
{
  if (self->playbin)
    return;  /* Already set up */

  self->playbin = gst_element_factory_make ("playbin", "preview-player");
  if (!self->playbin)
    {
      g_warning ("RecordingOverlay: Failed to create playbin");
      return;
    }

  /* Disable video */
  g_object_set (self->playbin, "video-sink",
                gst_element_factory_make ("fakesink", NULL), NULL);

  /* Set up bus watch */
  GstBus *bus = gst_element_get_bus (self->playbin);
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message", G_CALLBACK (on_bus_message), self);
  gst_object_unref (bus);
}

static void
start_preview_playback (GnalkRecordingOverlay *self)
{
  if (!self->preview_file_path || !self->playbin)
    return;

  g_autofree char *uri = g_filename_to_uri (self->preview_file_path, NULL, NULL);
  if (!uri)
    return;

  g_object_set (self->playbin, "uri", uri, NULL);

  /* Start playing */
  gst_element_set_state (self->playbin, GST_STATE_PLAYING);
  self->is_playing = TRUE;
  gtk_button_set_icon_name (GTK_BUTTON (self->play_pause_button),
                            "media-playback-pause-symbolic");

  /* Start position updates */
  if (self->position_update_id == 0)
    {
      self->position_update_id = g_timeout_add (POSITION_UPDATE_INTERVAL_MS,
                                                 on_position_update, self);
    }
}
#endif /* HAVE_GSTREAMER */

static void
stop_playback (GnalkRecordingOverlay *self)
{
#ifdef HAVE_GSTREAMER
  if (self->position_update_id > 0)
    {
      g_source_remove (self->position_update_id);
      self->position_update_id = 0;
    }

  if (self->playbin)
    {
      gst_element_set_state (self->playbin, GST_STATE_NULL);
    }

  self->is_playing = FALSE;
#endif
}

static void
on_preview_clicked (GtkButton              *button G_GNUC_UNUSED,
                    GnalkRecordingOverlay *self)
{
  if (self->mode != MODE_RECORDING)
    return;

  /* Stop the duration timer */
  if (self->duration_timer_id)
    {
      g_source_remove (self->duration_timer_id);
      self->duration_timer_id = 0;
    }

  /* Switch to preview handler for recording-stopped */
  g_signal_handlers_disconnect_by_func (self->recorder, on_recording_stopped, self);
  g_signal_handlers_disconnect_by_func (self->recorder, on_recording_stopped_for_preview, self);
  g_signal_connect (self->recorder, "recording-stopped",
                    G_CALLBACK (on_recording_stopped_for_preview), self);

  /* Stop recording - will trigger preview mode via handler */
  gnalk_voice_recorder_stop (self->recorder);
}

/* Called after recording stops to switch to preview mode */
static void
on_recording_stopped_for_preview (GnalkVoiceRecorder    *recorder G_GNUC_UNUSED,
                                  GBytes                 *audio_data,
                                  gdouble                 duration,
                                  GnalkRecordingOverlay *self)
{
  /* Store the audio data for later sending */
  g_clear_pointer (&self->preview_audio_data, g_bytes_unref);
  self->preview_audio_data = g_bytes_ref (audio_data);
  self->preview_duration = duration;

  /* Write to a temp file for playback */
  GError *error = NULL;
  int fd = g_file_open_tmp ("gnalk-preview-XXXXXX.ogg", &self->preview_file_path, &error);
  if (fd < 0)
    {
      g_warning ("RecordingOverlay: Failed to create preview temp file: %s", error->message);
      g_error_free (error);
      return;
    }

  gsize size;
  const void *data = g_bytes_get_data (audio_data, &size);
  if (write (fd, data, size) != (ssize_t) size)
    {
      g_warning ("RecordingOverlay: Failed to write preview file");
      close (fd);
      return;
    }
  close (fd);

  /* Switch to preview mode */
  switch_to_mode (self, MODE_PREVIEW);

#ifdef HAVE_GSTREAMER
  /* Set up and start playback */
  setup_playback_pipeline (self);
  start_preview_playback (self);
#endif
}

static void
on_play_pause_clicked (GtkButton              *button G_GNUC_UNUSED,
                       GnalkRecordingOverlay *self)
{
#ifdef HAVE_GSTREAMER
  if (!self->playbin)
    return;

  if (self->is_playing)
    {
      gst_element_set_state (self->playbin, GST_STATE_PAUSED);
      self->is_playing = FALSE;
      gtk_button_set_icon_name (GTK_BUTTON (self->play_pause_button),
                                "media-playback-start-symbolic");
      if (self->position_update_id > 0)
        {
          g_source_remove (self->position_update_id);
          self->position_update_id = 0;
        }
    }
  else
    {
      gst_element_set_state (self->playbin, GST_STATE_PLAYING);
      self->is_playing = TRUE;
      gtk_button_set_icon_name (GTK_BUTTON (self->play_pause_button),
                                "media-playback-pause-symbolic");
      if (self->position_update_id == 0)
        {
          self->position_update_id = g_timeout_add (POSITION_UPDATE_INTERVAL_MS,
                                                     on_position_update, self);
        }
    }
#endif
}

static void
on_rerecord_clicked (GtkButton              *button G_GNUC_UNUSED,
                     GnalkRecordingOverlay *self)
{
  /* Stop playback and clean up preview */
  stop_playback (self);

  if (self->preview_file_path)
    {
      g_unlink (self->preview_file_path);
      g_clear_pointer (&self->preview_file_path, g_free);
    }
  g_clear_pointer (&self->preview_audio_data, g_bytes_unref);

  /* Switch back to recording mode and start recording */
  switch_to_mode (self, MODE_RECORDING);
  gnalk_recording_overlay_start (self);
}

static void
switch_to_mode (GnalkRecordingOverlay *self, OverlayMode mode)
{
  self->mode = mode;

  if (mode == MODE_RECORDING)
    {
      /* Show recording widgets */
      gtk_widget_set_visible (self->recording_waveform, TRUE);
      gtk_widget_set_visible (self->preview_button, TRUE);
      gtk_widget_set_visible (self->cancel_button, TRUE);

      /* Hide preview widgets */
      gtk_widget_set_visible (GTK_WIDGET (self->preview_seekbar), FALSE);
      gtk_widget_set_visible (self->play_pause_button, FALSE);
      gtk_widget_set_visible (self->rerecord_button, FALSE);
    }
  else /* MODE_PREVIEW */
    {
      /* Prepare waveform for preview seekbar */
      prepare_waveform_for_preview (self);

      /* Hide recording widgets */
      gtk_widget_set_visible (self->recording_waveform, FALSE);
      gtk_widget_set_visible (self->preview_button, FALSE);

      /* Show preview widgets - cancel becomes rerecord */
      gtk_widget_set_visible (GTK_WIDGET (self->preview_seekbar), TRUE);
      gtk_widget_set_visible (self->play_pause_button, TRUE);
      gtk_widget_set_visible (self->rerecord_button, TRUE);
      gtk_widget_set_visible (self->cancel_button, FALSE);

      /* Update duration label */
      update_duration_label_from_value (self, 0.0);
    }
}

static void
gnalk_recording_overlay_dispose (GObject *object)
{
  GnalkRecordingOverlay *self = GNALK_RECORDING_OVERLAY (object);

  stop_playback (self);

#ifdef HAVE_GSTREAMER
  if (self->playbin)
    {
      GstBus *bus = gst_element_get_bus (self->playbin);
      if (bus)
        {
          gst_bus_remove_signal_watch (bus);
          gst_object_unref (bus);
        }
      gst_element_set_state (self->playbin, GST_STATE_NULL);
      gst_object_unref (self->playbin);
      self->playbin = NULL;
    }
#endif

  if (self->duration_timer_id)
    {
      g_source_remove (self->duration_timer_id);
      self->duration_timer_id = 0;
    }

  if (self->recorder && gnalk_voice_recorder_is_recording (self->recorder))
    gnalk_voice_recorder_cancel (self->recorder);

  g_clear_object (&self->recorder);
  g_clear_pointer (&self->levels, g_array_unref);
  g_clear_pointer (&self->preview_audio_data, g_bytes_unref);

  if (self->preview_file_path)
    {
      g_unlink (self->preview_file_path);
      g_clear_pointer (&self->preview_file_path, g_free);
    }

  /* Unparent children before chain up */
  g_clear_pointer (&self->cancel_button, gtk_widget_unparent);
  g_clear_pointer (&self->rerecord_button, gtk_widget_unparent);
  g_clear_pointer (&self->recording_waveform, gtk_widget_unparent);
  g_clear_pointer ((GtkWidget **) &self->preview_seekbar, gtk_widget_unparent);
  g_clear_pointer (&self->duration_label, gtk_widget_unparent);
  g_clear_pointer (&self->preview_button, gtk_widget_unparent);
  g_clear_pointer (&self->play_pause_button, gtk_widget_unparent);
  g_clear_pointer (&self->send_button, gtk_widget_unparent);

  G_OBJECT_CLASS (gnalk_recording_overlay_parent_class)->dispose (object);
}

static void
gnalk_recording_overlay_class_init (GnalkRecordingOverlayClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnalk_recording_overlay_dispose;

  /**
   * GnalkRecordingOverlay::cancelled:
   * @self: the overlay
   *
   * Emitted when the user cancels recording.
   */
  signals[SIGNAL_CANCELLED] = g_signal_new (
      "cancelled",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL, NULL,
      G_TYPE_NONE, 0);

  /**
   * GnalkRecordingOverlay::send-requested:
   * @self: the overlay
   * @audio_data: the recorded audio as GBytes
   * @duration: recording duration in seconds
   *
   * Emitted when the user presses send.
   */
  signals[SIGNAL_SEND_REQUESTED] = g_signal_new (
      "send-requested",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL, NULL,
      G_TYPE_NONE, 2, G_TYPE_BYTES, G_TYPE_DOUBLE);

  /* Ensure waveform seekbar type is registered */
  g_type_ensure (GNALK_TYPE_WAVEFORM_SEEKBAR);

  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "recording-overlay");
}

static void
gnalk_recording_overlay_init (GnalkRecordingOverlay *self)
{
  GtkLayoutManager *layout = gtk_widget_get_layout_manager (GTK_WIDGET (self));
  gtk_orientable_set_orientation (GTK_ORIENTABLE (layout), GTK_ORIENTATION_HORIZONTAL);
  gtk_box_layout_set_spacing (GTK_BOX_LAYOUT (layout), 8);

  /* Fill the available width */
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);

  self->mode = MODE_RECORDING;

  /* Cancel button (recording mode) */
  self->cancel_button = gtk_button_new_from_icon_name ("window-close-symbolic");
  gtk_widget_add_css_class (self->cancel_button, "circular");
  gtk_widget_add_css_class (self->cancel_button, "destructive-action");
  gtk_widget_set_tooltip_text (self->cancel_button, "Cancel recording");
  gtk_widget_set_parent (self->cancel_button, GTK_WIDGET (self));
  g_signal_connect (self->cancel_button, "clicked", G_CALLBACK (on_cancel_clicked), self);

  /* Re-record button (preview mode) - hidden initially */
  self->rerecord_button = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  gtk_widget_add_css_class (self->rerecord_button, "circular");
  gtk_widget_set_tooltip_text (self->rerecord_button, "Record again");
  gtk_widget_set_parent (self->rerecord_button, GTK_WIDGET (self));
  gtk_widget_set_visible (self->rerecord_button, FALSE);
  g_signal_connect (self->rerecord_button, "clicked", G_CALLBACK (on_rerecord_clicked), self);

  /* Recording waveform visualization area */
  self->recording_waveform = gtk_drawing_area_new ();
  gtk_widget_set_hexpand (self->recording_waveform, TRUE);
  gtk_widget_set_size_request (self->recording_waveform, 100, 32);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self->recording_waveform),
                                   on_recording_waveform_draw, self, NULL);
  gtk_widget_set_parent (self->recording_waveform, GTK_WIDGET (self));

  /* Preview seekbar (hidden initially) */
  self->preview_seekbar = gnalk_waveform_seekbar_new ();
  gtk_widget_set_hexpand (GTK_WIDGET (self->preview_seekbar), TRUE);
  gtk_widget_set_parent (GTK_WIDGET (self->preview_seekbar), GTK_WIDGET (self));
  gtk_widget_set_visible (GTK_WIDGET (self->preview_seekbar), FALSE);
#ifdef HAVE_GSTREAMER
  g_signal_connect (self->preview_seekbar, "seek", G_CALLBACK (on_seekbar_seek), self);
#endif

  /* Duration label */
  self->duration_label = gtk_label_new ("0:00");
  gtk_widget_add_css_class (self->duration_label, "dim-label");
  gtk_widget_add_css_class (self->duration_label, "monospace");
  gtk_widget_set_parent (self->duration_label, GTK_WIDGET (self));

  /* Preview/play button (recording mode) */
  self->preview_button = gtk_button_new_from_icon_name ("media-playback-start-symbolic");
  gtk_widget_add_css_class (self->preview_button, "circular");
  gtk_widget_set_tooltip_text (self->preview_button, "Preview recording");
  gtk_widget_set_parent (self->preview_button, GTK_WIDGET (self));
  g_signal_connect (self->preview_button, "clicked", G_CALLBACK (on_preview_clicked), self);

  /* Play/pause button (preview mode) - hidden initially */
  self->play_pause_button = gtk_button_new_from_icon_name ("media-playback-pause-symbolic");
  gtk_widget_add_css_class (self->play_pause_button, "circular");
  gtk_widget_set_tooltip_text (self->play_pause_button, "Play/Pause");
  gtk_widget_set_parent (self->play_pause_button, GTK_WIDGET (self));
  gtk_widget_set_visible (self->play_pause_button, FALSE);
  g_signal_connect (self->play_pause_button, "clicked", G_CALLBACK (on_play_pause_clicked), self);

  /* Send button */
  self->send_button = gtk_button_new_from_icon_name ("emblem-ok-symbolic");
  gtk_widget_add_css_class (self->send_button, "circular");
  gtk_widget_add_css_class (self->send_button, "suggested-action");
  gtk_widget_set_tooltip_text (self->send_button, "Send voice message");
  gtk_widget_set_parent (self->send_button, GTK_WIDGET (self));
  g_signal_connect (self->send_button, "clicked", G_CALLBACK (on_send_clicked), self);

  /* Initialize waveform data */
  self->levels = g_array_new (FALSE, TRUE, sizeof (gdouble));

  /* Create recorder - use special handler for preview mode */
  self->recorder = gnalk_voice_recorder_new ();
  g_signal_connect (self->recorder, "level-updated", G_CALLBACK (on_level_updated), self);
  g_signal_connect (self->recorder, "error", G_CALLBACK (on_recorder_error), self);

  /* Connect recording-stopped to handle both normal send and preview mode */
  /* We'll determine which handler to use based on mode */
}

GnalkRecordingOverlay *
gnalk_recording_overlay_new (void)
{
  return g_object_new (GNALK_TYPE_RECORDING_OVERLAY, NULL);
}

gboolean
gnalk_recording_overlay_start (GnalkRecordingOverlay *self)
{
  g_return_val_if_fail (GNALK_IS_RECORDING_OVERLAY (self), FALSE);

  /* Clean up any previous preview state */
  stop_playback (self);
  if (self->preview_file_path)
    {
      g_unlink (self->preview_file_path);
      g_clear_pointer (&self->preview_file_path, g_free);
    }
  g_clear_pointer (&self->preview_audio_data, g_bytes_unref);

  /* Reset to recording mode */
  switch_to_mode (self, MODE_RECORDING);

  /* Reset waveform - clear all previous levels */
  g_array_set_size (self->levels, 0);
  gtk_widget_queue_draw (self->recording_waveform);

  /* Reset duration */
  gtk_label_set_label (GTK_LABEL (self->duration_label), "0:00");

  /* Disconnect old recording-stopped handler and connect new one based on context */
  g_signal_handlers_disconnect_by_func (self->recorder, on_recording_stopped, self);
  g_signal_handlers_disconnect_by_func (self->recorder, on_recording_stopped_for_preview, self);
  g_signal_connect (self->recorder, "recording-stopped", G_CALLBACK (on_recording_stopped), self);

  /* Start recording */
  if (!gnalk_voice_recorder_start (self->recorder))
    return FALSE;

  /* Start duration timer (update every 100ms) */
  self->duration_timer_id = g_timeout_add (100, on_duration_timer, self);

  return TRUE;
}

gboolean
gnalk_recording_overlay_is_recording (GnalkRecordingOverlay *self)
{
  g_return_val_if_fail (GNALK_IS_RECORDING_OVERLAY (self), FALSE);
  return gnalk_voice_recorder_is_recording (self->recorder);
}
