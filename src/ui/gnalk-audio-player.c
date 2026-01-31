#include "gnalk-audio-player.h"
#include "gnalk-waveform-seekbar.h"

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#endif

#define POSITION_UPDATE_INTERVAL_MS 250
#define WAVEFORM_NUM_BARS           50

/* Static pointer to currently playing audio player (single-player policy) */
static GnalkAudioPlayer *current_playing_player = NULL;

struct _GnalkAudioPlayer
{
  GtkBox parent_instance;

  GtkWidget             *play_button;
  GtkWidget             *loading_spinner;
  GnalkWaveformSeekbar *waveform_seekbar;
  GtkWidget             *download_button;
  GtkWidget             *time_label;

#ifdef HAVE_GSTREAMER
  GstElement *playbin;
#endif

  char     *file_path;
  gboolean  is_playing;
  gboolean  is_loading;
  guint     position_update_id;
  gint64    duration;

  /* Waveform data */
  float    *waveform_data;
  gsize     waveform_size;
};

G_DEFINE_FINAL_TYPE (GnalkAudioPlayer, gnalk_audio_player, GTK_TYPE_BOX)

enum {
  SIGNAL_PLAY_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void update_time_label (GnalkAudioPlayer *self, gint64 position, gint64 duration);
static void stop_position_updates (GnalkAudioPlayer *self);

static void
format_time (gint64 time_ns, char *buf, gsize buf_len)
{
  gint64 seconds = time_ns / GST_SECOND;
  gint64 minutes = seconds / 60;
  seconds = seconds % 60;

  g_snprintf (buf, buf_len, "%"G_GINT64_FORMAT":%02"G_GINT64_FORMAT, minutes, seconds);
}

static void
update_time_label (GnalkAudioPlayer *self, gint64 position, gint64 duration)
{
  char pos_str[16], dur_str[16];

  format_time (position, pos_str, sizeof (pos_str));
  format_time (duration, dur_str, sizeof (dur_str));

  g_autofree char *label = g_strdup_printf ("%s / %s", pos_str, dur_str);
  gtk_label_set_text (GTK_LABEL (self->time_label), label);
}

#ifdef HAVE_GSTREAMER
static void on_waveform_seek (GnalkWaveformSeekbar *seekbar, double progress, gpointer user_data);

static gboolean
on_position_update (gpointer user_data)
{
  GnalkAudioPlayer *self = GNALK_AUDIO_PLAYER (user_data);

  if (!self->playbin || !self->is_playing)
    return G_SOURCE_REMOVE;

  /* Try to get duration if we don't have it yet */
  if (self->duration <= 0)
    gst_element_query_duration (self->playbin, GST_FORMAT_TIME, &self->duration);

  gint64 position = 0;
  if (gst_element_query_position (self->playbin, GST_FORMAT_TIME, &position))
    {
      /* Update waveform without triggering the seek callback */
      g_signal_handlers_block_by_func (self->waveform_seekbar,
                                       G_CALLBACK (on_waveform_seek), self);

      if (self->duration > 0)
        {
          double fraction = (double) position / (double) self->duration;
          gnalk_waveform_seekbar_set_progress (self->waveform_seekbar, fraction);
        }

      g_signal_handlers_unblock_by_func (self->waveform_seekbar,
                                         G_CALLBACK (on_waveform_seek), self);

      update_time_label (self, position, self->duration);
    }

  return G_SOURCE_CONTINUE;
}

static void
on_bus_message (GstBus     *bus G_GNUC_UNUSED,
                GstMessage *message,
                gpointer    user_data)
{
  GnalkAudioPlayer *self = GNALK_AUDIO_PLAYER (user_data);

  switch (GST_MESSAGE_TYPE (message))
    {
    case GST_MESSAGE_EOS:
      /* End of stream - reset to beginning */
      gst_element_set_state (self->playbin, GST_STATE_PAUSED);
      gst_element_seek_simple (self->playbin, GST_FORMAT_TIME,
                               GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 0);
      self->is_playing = FALSE;
      gtk_button_set_icon_name (GTK_BUTTON (self->play_button), "media-playback-start-symbolic");
      stop_position_updates (self);

      gnalk_waveform_seekbar_set_progress (self->waveform_seekbar, 0.0);
      update_time_label (self, 0, self->duration);

      if (current_playing_player == self)
        current_playing_player = NULL;
      break;

    case GST_MESSAGE_ERROR:
      {
        GError *err = NULL;
        gchar *debug_info = NULL;

        gst_message_parse_error (message, &err, &debug_info);
        g_warning ("Audio playback error: %s", err->message);
        g_error_free (err);
        g_free (debug_info);

        gst_element_set_state (self->playbin, GST_STATE_NULL);
        self->is_playing = FALSE;
        gtk_button_set_icon_name (GTK_BUTTON (self->play_button), "media-playback-start-symbolic");
        stop_position_updates (self);

        if (current_playing_player == self)
          current_playing_player = NULL;
      }
      break;

    case GST_MESSAGE_DURATION_CHANGED:
      /* Duration changed, query it again */
      if (gst_element_query_duration (self->playbin, GST_FORMAT_TIME, &self->duration))
        update_time_label (self, 0, self->duration);
      break;

    default:
      break;
    }
}

static void
start_position_updates (GnalkAudioPlayer *self)
{
  if (self->position_update_id == 0)
    {
      self->position_update_id = g_timeout_add (POSITION_UPDATE_INTERVAL_MS,
                                                 on_position_update, self);
    }
}
#endif /* HAVE_GSTREAMER */

static void
stop_position_updates (GnalkAudioPlayer *self)
{
  if (self->position_update_id > 0)
    {
      g_source_remove (self->position_update_id);
      self->position_update_id = 0;
    }
}

static void
on_play_button_clicked (GtkButton *button G_GNUC_UNUSED,
                        gpointer   user_data)
{
  GnalkAudioPlayer *self = GNALK_AUDIO_PLAYER (user_data);

  /* If no file is set, emit signal to request file download */
  if (!self->file_path)
    {
      g_signal_emit (self, signals[SIGNAL_PLAY_REQUESTED], 0);
      return;
    }

#ifdef HAVE_GSTREAMER
  if (!self->playbin)
    return;

  if (self->is_playing)
    {
      /* Pause playback */
      gst_element_set_state (self->playbin, GST_STATE_PAUSED);
      self->is_playing = FALSE;
      gtk_button_set_icon_name (GTK_BUTTON (self->play_button), "media-playback-start-symbolic");
      stop_position_updates (self);

      if (current_playing_player == self)
        current_playing_player = NULL;
    }
  else
    {
      /* Stop any other playing audio first (single-player policy) */
      if (current_playing_player && current_playing_player != self)
        gnalk_audio_player_stop (current_playing_player);

      /* Start playback */
      gst_element_set_state (self->playbin, GST_STATE_PLAYING);
      self->is_playing = TRUE;
      gtk_button_set_icon_name (GTK_BUTTON (self->play_button), "media-playback-pause-symbolic");
      start_position_updates (self);

      current_playing_player = self;
    }
#endif /* HAVE_GSTREAMER */
}

static void
on_waveform_seek (GnalkWaveformSeekbar *seekbar G_GNUC_UNUSED,
                  double                 progress,
                  gpointer               user_data)
{
  GnalkAudioPlayer *self = GNALK_AUDIO_PLAYER (user_data);

#ifdef HAVE_GSTREAMER
  if (!self->playbin || self->duration <= 0)
    return;

  gint64 position = (gint64) (progress * self->duration);

  gst_element_seek_simple (self->playbin, GST_FORMAT_TIME,
                           GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                           position);

  update_time_label (self, position, self->duration);
#endif /* HAVE_GSTREAMER */
}

static void
gnalk_audio_player_dispose (GObject *object)
{
  GnalkAudioPlayer *self = GNALK_AUDIO_PLAYER (object);

  /* Stop playback and clean up GStreamer */
  gnalk_audio_player_stop (self);

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

  G_OBJECT_CLASS (gnalk_audio_player_parent_class)->dispose (object);
}

static void
gnalk_audio_player_finalize (GObject *object)
{
  GnalkAudioPlayer *self = GNALK_AUDIO_PLAYER (object);

  g_clear_pointer (&self->file_path, g_free);
  g_clear_pointer (&self->waveform_data, g_free);

  if (current_playing_player == self)
    current_playing_player = NULL;

  G_OBJECT_CLASS (gnalk_audio_player_parent_class)->finalize (object);
}

static void
gnalk_audio_player_class_init (GnalkAudioPlayerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnalk_audio_player_dispose;
  object_class->finalize = gnalk_audio_player_finalize;

  /* Signal emitted when play is clicked but no file is loaded yet */
  signals[SIGNAL_PLAY_REQUESTED] =
    g_signal_new ("play-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /* Ensure waveform seekbar type is registered before loading template */
  g_type_ensure (GNALK_TYPE_WAVEFORM_SEEKBAR);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/net/skilpa/gnalk/gnalk-audio-player.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkAudioPlayer, play_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkAudioPlayer, loading_spinner);
  gtk_widget_class_bind_template_child (widget_class, GnalkAudioPlayer, waveform_seekbar);
  gtk_widget_class_bind_template_child (widget_class, GnalkAudioPlayer, download_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkAudioPlayer, time_label);
}

static void
gnalk_audio_player_init (GnalkAudioPlayer *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->is_playing = FALSE;
  self->is_loading = FALSE;
  self->duration = 0;
  self->position_update_id = 0;
  self->waveform_data = NULL;
  self->waveform_size = 0;

  /* Hide waveform initially (no file loaded yet) */
  gtk_widget_set_visible (GTK_WIDGET (self->waveform_seekbar), FALSE);

  /* Connect signals */
  g_signal_connect (self->play_button, "clicked",
                    G_CALLBACK (on_play_button_clicked), self);
  g_signal_connect (self->waveform_seekbar, "seek",
                    G_CALLBACK (on_waveform_seek), self);
  g_signal_connect (self->download_button, "clicked",
                    G_CALLBACK (on_play_button_clicked), self);

#ifdef HAVE_GSTREAMER
  /* Create playbin element */
  self->playbin = gst_element_factory_make ("playbin", "audio-player");
  if (self->playbin)
    {
      /* Disable video (audio only) */
      g_object_set (self->playbin, "video-sink", gst_element_factory_make ("fakesink", NULL), NULL);

      /* Use autoaudiosink for best audio output */
      GstElement *audio_sink = gst_element_factory_make ("autoaudiosink", NULL);
      if (audio_sink)
        g_object_set (self->playbin, "audio-sink", audio_sink, NULL);

      /* Disable network buffering (we're playing local files) */
      g_object_set (self->playbin, "buffer-size", 0, NULL);

      /* Set up bus watch for messages */
      GstBus *bus = gst_element_get_bus (self->playbin);
      gst_bus_add_signal_watch (bus);
      g_signal_connect (bus, "message", G_CALLBACK (on_bus_message), self);
      gst_object_unref (bus);
    }
  else
    {
      g_warning ("Failed to create GStreamer playbin element");
    }
#endif /* HAVE_GSTREAMER */
}

#ifdef HAVE_GSTREAMER
static void
extract_waveform_data (GnalkAudioPlayer *self, const char *file_path)
{
  /* Clear existing waveform data */
  g_clear_pointer (&self->waveform_data, g_free);
  self->waveform_size = 0;

  /* Create a pipeline to decode the audio file and extract samples */
  g_autofree char *uri = g_filename_to_uri (file_path, NULL, NULL);
  if (!uri)
    return;

  g_autofree char *pipeline_str = g_strdup_printf (
    "uridecodebin uri=\"%s\" ! audioconvert ! audioresample ! "
    "audio/x-raw,format=S16LE,channels=1,rate=8000 ! appsink name=sink",
    uri);

  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch (pipeline_str, &error);
  if (!pipeline)
    {
      if (error)
        {
          g_warning ("Failed to create waveform pipeline: %s", error->message);
          g_error_free (error);
        }
      return;
    }

  GstElement *appsink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
  if (!appsink)
    {
      gst_object_unref (pipeline);
      return;
    }

  /* Configure appsink - emit-signals=FALSE since we pull synchronously */
  g_object_set (appsink, "sync", FALSE, "emit-signals", FALSE, NULL);

  /* Start the pipeline and wait for it to preroll */
  GstStateChangeReturn ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    {
      g_warning ("Waveform pipeline failed to start");
      gst_object_unref (appsink);
      gst_object_unref (pipeline);
      return;
    }

  /* Collect samples until EOS */
  GArray *samples = g_array_new (FALSE, FALSE, sizeof (gint16));
  gboolean done = FALSE;

  while (!done)
    {
      GstSample *sample = gst_app_sink_try_pull_sample (GST_APP_SINK (appsink), GST_SECOND);

      if (sample)
        {
          GstBuffer *buffer = gst_sample_get_buffer (sample);
          if (buffer)
            {
              GstMapInfo map;
              if (gst_buffer_map (buffer, &map, GST_MAP_READ))
                {
                  gint16 *data = (gint16 *) map.data;
                  gsize num_samples = map.size / sizeof (gint16);

                  for (gsize i = 0; i < num_samples; i++)
                    g_array_append_val (samples, data[i]);

                  gst_buffer_unmap (buffer, &map);
                }
            }
          gst_sample_unref (sample);
        }
      else
        {
          /* No sample - check if we've reached EOS */
          if (gst_app_sink_is_eos (GST_APP_SINK (appsink)))
            done = TRUE;
          else
            done = TRUE;  /* Timeout with no data, stop anyway */
        }
    }

  /* Stop pipeline */
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (appsink);
  gst_object_unref (pipeline);

  /* No samples collected */
  if (samples->len == 0)
    {
      g_array_free (samples, TRUE);
      return;
    }

  /* Calculate amplitudes for each bar */
  self->waveform_size = WAVEFORM_NUM_BARS;
  self->waveform_data = g_new0 (float, self->waveform_size);

  gsize samples_per_bar = samples->len / WAVEFORM_NUM_BARS;
  if (samples_per_bar == 0)
    samples_per_bar = 1;

  for (gsize i = 0; i < WAVEFORM_NUM_BARS; i++)
    {
      gsize start = i * samples_per_bar;
      gsize end = MIN (start + samples_per_bar, samples->len);

      /* Find peak amplitude in this segment */
      gint32 max_abs = 0;
      for (gsize j = start; j < end; j++)
        {
          gint16 s = g_array_index (samples, gint16, j);
          gint32 abs_val = s < 0 ? -s : s;
          if (abs_val > max_abs)
            max_abs = abs_val;
        }

      /* Normalize to 0.0-1.0 range */
      self->waveform_data[i] = (float) max_abs / 32768.0f;
    }

  g_array_free (samples, TRUE);

  /* Update the waveform widget */
  gnalk_waveform_seekbar_set_waveform (self->waveform_seekbar,
                                        self->waveform_data,
                                        self->waveform_size);
}
#endif /* HAVE_GSTREAMER */

GnalkAudioPlayer *
gnalk_audio_player_new (void)
{
  return g_object_new (GNALK_TYPE_AUDIO_PLAYER, NULL);
}

void
gnalk_audio_player_set_file (GnalkAudioPlayer *self, const char *file_path)
{
  g_return_if_fail (GNALK_IS_AUDIO_PLAYER (self));

  /* Remember if we were loading (user requested playback but file wasn't available) */
  gboolean was_loading = self->is_loading;

  /* Skip if same file already set (avoid re-extracting waveform) */
  if (self->file_path && file_path && g_strcmp0 (self->file_path, file_path) == 0)
    {
      /* File already set, just handle auto-play if needed */
      if (was_loading)
        {
#ifdef HAVE_GSTREAMER
          if (self->playbin)
            {
              if (current_playing_player && current_playing_player != self)
                gnalk_audio_player_stop (current_playing_player);

              gst_element_set_state (self->playbin, GST_STATE_PLAYING);
              self->is_playing = TRUE;
              gtk_button_set_icon_name (GTK_BUTTON (self->play_button), "media-playback-pause-symbolic");
              start_position_updates (self);
              current_playing_player = self;
            }
#endif
        }
      gnalk_audio_player_set_loading (self, FALSE);
      return;
    }

  /* Stop current playback */
  gnalk_audio_player_stop (self);

  g_clear_pointer (&self->file_path, g_free);

  if (!file_path)
    {
      /* Show download button and hide waveform when file is cleared */
      gtk_widget_set_visible (self->download_button, TRUE);
      gtk_widget_set_visible (GTK_WIDGET (self->waveform_seekbar), FALSE);
      return;
    }

  self->file_path = g_strdup (file_path);

  /* Hide download button and show waveform since file is now available */
  gtk_widget_set_visible (self->download_button, FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->waveform_seekbar), TRUE);

#ifdef HAVE_GSTREAMER
  /* Extract waveform data for visualization */
  extract_waveform_data (self, file_path);

  if (self->playbin)
    {
      /* Set URI */
      g_autofree char *uri = g_filename_to_uri (file_path, NULL, NULL);
      if (uri)
        {
          g_object_set (self->playbin, "uri", uri, NULL);

          /* Move to PAUSED to preroll and get duration */
          gst_element_set_state (self->playbin, GST_STATE_PAUSED);

          /* Query duration (may not be available until preroll completes) */
          self->duration = 0;
          gst_element_query_duration (self->playbin, GST_FORMAT_TIME, &self->duration);

          update_time_label (self, 0, self->duration);
        }
    }
#endif /* HAVE_GSTREAMER */

  /* Clear loading state when file is set */
  gnalk_audio_player_set_loading (self, FALSE);

  /* Auto-play if user requested playback (was in loading state) */
  if (was_loading)
    {
#ifdef HAVE_GSTREAMER
      if (self->playbin)
        {
          /* Stop any other playing audio first (single-player policy) */
          if (current_playing_player && current_playing_player != self)
            gnalk_audio_player_stop (current_playing_player);

          gst_element_set_state (self->playbin, GST_STATE_PLAYING);
          self->is_playing = TRUE;
          gtk_button_set_icon_name (GTK_BUTTON (self->play_button), "media-playback-pause-symbolic");
          start_position_updates (self);
          current_playing_player = self;
        }
#endif /* HAVE_GSTREAMER */
    }
}

void
gnalk_audio_player_set_loading (GnalkAudioPlayer *self, gboolean loading)
{
  g_return_if_fail (GNALK_IS_AUDIO_PLAYER (self));

  self->is_loading = loading;

  gtk_widget_set_visible (self->play_button, !loading);
  gtk_widget_set_visible (self->loading_spinner, loading);

  if (loading)
    gtk_spinner_start (GTK_SPINNER (self->loading_spinner));
  else
    gtk_spinner_stop (GTK_SPINNER (self->loading_spinner));

  /* Disable waveform seekbar while loading */
  gnalk_waveform_seekbar_set_sensitive (self->waveform_seekbar, !loading);
}

void
gnalk_audio_player_stop (GnalkAudioPlayer *self)
{
  g_return_if_fail (GNALK_IS_AUDIO_PLAYER (self));

  stop_position_updates (self);

#ifdef HAVE_GSTREAMER
  if (self->playbin && self->is_playing)
    {
      gst_element_set_state (self->playbin, GST_STATE_PAUSED);
      gst_element_seek_simple (self->playbin, GST_FORMAT_TIME,
                               GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 0);
    }
#endif /* HAVE_GSTREAMER */

  self->is_playing = FALSE;
  gtk_button_set_icon_name (GTK_BUTTON (self->play_button), "media-playback-start-symbolic");
  gnalk_waveform_seekbar_set_progress (self->waveform_seekbar, 0.0);

  if (current_playing_player == self)
    current_playing_player = NULL;
}
