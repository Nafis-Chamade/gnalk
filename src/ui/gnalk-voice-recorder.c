/* gnalk-voice-recorder.c
 *
 * GStreamer-based voice recorder for voice messages.
 */

#include "gnalk-voice-recorder.h"

#include <gio/gio.h>
#include <glib/gstdio.h>

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#endif

struct _GnalkVoiceRecorder
{
  GObject parent_instance;

#ifdef HAVE_GSTREAMER
  GstElement *pipeline;
  GstElement *level;     /* For audio level monitoring */
  gulong      bus_watch_id;
#endif

  char    *temp_path;     /* Temporary file for recording */
  gint64   start_time;    /* Recording start time (monotonic microseconds) */
  gboolean recording;
};

G_DEFINE_FINAL_TYPE (GnalkVoiceRecorder, gnalk_voice_recorder, G_TYPE_OBJECT)

enum {
  SIGNAL_LEVEL_UPDATED,
  SIGNAL_RECORDING_STOPPED,
  SIGNAL_ERROR,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gnalk_voice_recorder_dispose (GObject *object)
{
  GnalkVoiceRecorder *self = GNALK_VOICE_RECORDER (object);

  gnalk_voice_recorder_cancel (self);

  G_OBJECT_CLASS (gnalk_voice_recorder_parent_class)->dispose (object);
}

static void
gnalk_voice_recorder_finalize (GObject *object)
{
  GnalkVoiceRecorder *self = GNALK_VOICE_RECORDER (object);

  g_clear_pointer (&self->temp_path, g_free);

  G_OBJECT_CLASS (gnalk_voice_recorder_parent_class)->finalize (object);
}

static void
gnalk_voice_recorder_class_init (GnalkVoiceRecorderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gnalk_voice_recorder_dispose;
  object_class->finalize = gnalk_voice_recorder_finalize;

  /**
   * GnalkVoiceRecorder::level-updated:
   * @self: the voice recorder
   * @level: audio level from 0.0 to 1.0
   *
   * Emitted periodically during recording with the current audio level.
   */
  signals[SIGNAL_LEVEL_UPDATED] = g_signal_new (
      "level-updated",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_DOUBLE);

  /**
   * GnalkVoiceRecorder::recording-stopped:
   * @self: the voice recorder
   * @audio_data: the recorded audio as GBytes (Opus/OGG format)
   * @duration: recording duration in seconds
   *
   * Emitted when recording is stopped successfully.
   */
  signals[SIGNAL_RECORDING_STOPPED] = g_signal_new (
      "recording-stopped",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL, NULL,
      G_TYPE_NONE, 2, G_TYPE_BYTES, G_TYPE_DOUBLE);

  /**
   * GnalkVoiceRecorder::error:
   * @self: the voice recorder
   * @message: error message
   *
   * Emitted when a recording error occurs.
   */
  signals[SIGNAL_ERROR] = g_signal_new (
      "error",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gnalk_voice_recorder_init (GnalkVoiceRecorder *self)
{
  self->recording = FALSE;
}

GnalkVoiceRecorder *
gnalk_voice_recorder_new (void)
{
  return g_object_new (GNALK_TYPE_VOICE_RECORDER, NULL);
}

#ifdef HAVE_GSTREAMER

static gboolean
on_bus_message (GstBus     *bus G_GNUC_UNUSED,
                GstMessage *message,
                gpointer    user_data)
{
  GnalkVoiceRecorder *self = GNALK_VOICE_RECORDER (user_data);

  switch (GST_MESSAGE_TYPE (message))
    {
    case GST_MESSAGE_ERROR:
      {
        GError *error = NULL;
        gchar *debug = NULL;
        gst_message_parse_error (message, &error, &debug);
        g_warning ("VoiceRecorder: GStreamer error: %s (debug: %s)",
                   error->message, debug ? debug : "none");
        g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
        g_error_free (error);
        g_free (debug);
        break;
      }

    case GST_MESSAGE_EOS:
      {
        g_message ("VoiceRecorder: EOS received, recording complete");
        /* EOS is handled in stop() */
        break;
      }

    case GST_MESSAGE_ELEMENT:
      {
        const GstStructure *s = gst_message_get_structure (message);
        const gchar *name = s ? gst_structure_get_name (s) : NULL;

        if (s && g_strcmp0 (name, "level") == 0)
          {
            const GValue *peak_value = gst_structure_get_value (s, "peak");
            if (peak_value)
              {
                gdouble peak_db = -60.0;  /* default to silence */

                /* Handle GValueArray (GLib), GstValueList, GstValueArray, or direct double */
                if (G_VALUE_HOLDS (peak_value, G_TYPE_VALUE_ARRAY))
                  {
                    /* GValueArray - older GLib array type used by level element */
                    GValueArray *arr = g_value_get_boxed (peak_value);
                    if (arr && arr->n_values > 0)
                      {
                        const GValue *v = g_value_array_get_nth (arr, 0);
                        if (v && G_VALUE_HOLDS_DOUBLE (v))
                          peak_db = g_value_get_double (v);
                      }
                  }
                else if (GST_VALUE_HOLDS_LIST (peak_value))
                  {
                    const GValue *v = gst_value_list_get_value (peak_value, 0);
                    if (v && G_VALUE_HOLDS_DOUBLE (v))
                      peak_db = g_value_get_double (v);
                  }
                else if (GST_VALUE_HOLDS_ARRAY (peak_value))
                  {
                    const GValue *v = gst_value_array_get_value (peak_value, 0);
                    if (v && G_VALUE_HOLDS_DOUBLE (v))
                      peak_db = g_value_get_double (v);
                  }
                else if (G_VALUE_HOLDS_DOUBLE (peak_value))
                  {
                    /* Single channel - direct double value */
                    peak_db = g_value_get_double (peak_value);
                  }
                else
                  {
                    g_warning ("VoiceRecorder: Unknown peak value type: %s",
                               G_VALUE_TYPE_NAME (peak_value));
                  }

                /* Convert dB to linear (0.0-1.0 range)
                 * -60dB = 0.0, 0dB = 1.0 */
                gdouble level = CLAMP ((peak_db + 60.0) / 60.0, 0.0, 1.0);
                g_signal_emit (self, signals[SIGNAL_LEVEL_UPDATED], 0, level);
              }
            else
              {
                g_warning ("VoiceRecorder: level message has no 'peak' field");
              }
          }
        break;
      }

    default:
      break;
    }

  return G_SOURCE_CONTINUE;
}

static gboolean
create_pipeline (GnalkVoiceRecorder *self)
{
  /* Create temporary file for recording */
  GError *error = NULL;
  int fd = g_file_open_tmp ("gnalk-voice-XXXXXX.ogg", &self->temp_path, &error);
  if (fd < 0)
    {
      g_warning ("VoiceRecorder: Failed to create temp file: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
      g_error_free (error);
      return FALSE;
    }
  close (fd);
  g_message ("VoiceRecorder: Recording to %s", self->temp_path);

  /* Use gst_parse_launch for reliable pipeline construction.
   * This handles caps negotiation automatically like gst-launch does.
   * Pipeline: pulsesrc -> queue -> audioconvert -> level -> audioresample -> capsfilter -> opusenc -> oggmux -> filesink
   *
   * Note: The level element is named "level" so we can retrieve it for the bus watch.
   */
  g_autofree char *pipeline_desc = g_strdup_printf (
      "pulsesrc buffer-time=200000 latency-time=20000 "
      "! queue max-size-time=500000000 leaky=downstream "
      "! audioconvert "
      "! level name=level post-messages=true interval=100000000 "
      "! audioresample "
      "! audio/x-raw,format=S16LE,rate=48000,channels=1 "
      "! opusenc bitrate=32000 audio-type=voice "
      "! oggmux "
      "! filesink location=\"%s\"",
      self->temp_path);

  g_message ("VoiceRecorder: Creating pipeline: %s", pipeline_desc);

  GstElement *pipeline = gst_parse_launch (pipeline_desc, &error);
  if (!pipeline)
    {
      g_warning ("VoiceRecorder: Failed to create pipeline: %s",
                 error ? error->message : "unknown error");
      g_signal_emit (self, signals[SIGNAL_ERROR], 0,
                     error ? error->message : "Failed to create pipeline");
      g_clear_error (&error);
      return FALSE;
    }

  /* Get the level element for audio visualization */
  GstElement *level = gst_bin_get_by_name (GST_BIN (pipeline), "level");
  if (!level)
    g_warning ("VoiceRecorder: Could not find level element for visualization");

  /* Set up bus watch for messages */
  GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  self->bus_watch_id = gst_bus_add_watch (bus, on_bus_message, self);
  gst_object_unref (bus);

  self->pipeline = pipeline;
  self->level = level;

  return TRUE;
}

static void
destroy_pipeline (GnalkVoiceRecorder *self)
{
  if (self->bus_watch_id)
    {
      g_source_remove (self->bus_watch_id);
      self->bus_watch_id = 0;
    }

  /* Unref level first (we got a ref from gst_bin_get_by_name) */
  if (self->level)
    {
      gst_object_unref (self->level);
      self->level = NULL;
    }

  if (self->pipeline)
    {
      gst_element_set_state (self->pipeline, GST_STATE_NULL);
      gst_object_unref (self->pipeline);
      self->pipeline = NULL;
    }
}

#endif /* HAVE_GSTREAMER */

gboolean
gnalk_voice_recorder_start (GnalkVoiceRecorder *self)
{
  g_return_val_if_fail (GNALK_IS_VOICE_RECORDER (self), FALSE);

  if (self->recording)
    {
      g_warning ("VoiceRecorder: Already recording");
      return FALSE;
    }

#ifdef HAVE_GSTREAMER
  if (!create_pipeline (self))
    return FALSE;

  GstStateChangeReturn ret = gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    {
      g_warning ("VoiceRecorder: Failed to start pipeline");
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, "Failed to start recording");
      destroy_pipeline (self);
      return FALSE;
    }

  self->start_time = g_get_monotonic_time ();
  self->recording = TRUE;
  g_message ("VoiceRecorder: Recording started");
  return TRUE;
#else
  g_signal_emit (self, signals[SIGNAL_ERROR], 0, "GStreamer not available");
  return FALSE;
#endif
}

void
gnalk_voice_recorder_stop (GnalkVoiceRecorder *self)
{
  g_return_if_fail (GNALK_IS_VOICE_RECORDER (self));

  if (!self->recording)
    return;

#ifdef HAVE_GSTREAMER
  gdouble duration = gnalk_voice_recorder_get_duration (self);

  /* Send EOS to flush the pipeline and finalize the file */
  gst_element_send_event (self->pipeline, gst_event_new_eos ());

  /* Wait for EOS to be processed */
  GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (self->pipeline));
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, 2 * GST_SECOND,
                                                 GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  if (msg)
    gst_message_unref (msg);
  gst_object_unref (bus);

  destroy_pipeline (self);
  self->recording = FALSE;

  /* Read the recorded file */
  if (self->temp_path)
    {
      GError *error = NULL;
      g_autofree char *contents = NULL;
      gsize length = 0;

      if (g_file_get_contents (self->temp_path, &contents, &length, &error))
        {
          GBytes *audio_data = g_bytes_new_take (g_steal_pointer (&contents), length);
          g_message ("VoiceRecorder: Recording complete, %zu bytes, %.1f seconds",
                     length, duration);
          g_signal_emit (self, signals[SIGNAL_RECORDING_STOPPED], 0, audio_data, duration);
          g_bytes_unref (audio_data);
        }
      else
        {
          g_warning ("VoiceRecorder: Failed to read recording: %s", error->message);
          g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
          g_error_free (error);
        }

      /* Clean up temp file */
      g_unlink (self->temp_path);
      g_clear_pointer (&self->temp_path, g_free);
    }
#endif
}

void
gnalk_voice_recorder_cancel (GnalkVoiceRecorder *self)
{
  g_return_if_fail (GNALK_IS_VOICE_RECORDER (self));

  if (!self->recording)
    return;

#ifdef HAVE_GSTREAMER
  destroy_pipeline (self);
  self->recording = FALSE;

  /* Clean up temp file without reading it */
  if (self->temp_path)
    {
      g_unlink (self->temp_path);
      g_clear_pointer (&self->temp_path, g_free);
    }

  g_message ("VoiceRecorder: Recording cancelled");
#endif
}

gboolean
gnalk_voice_recorder_is_recording (GnalkVoiceRecorder *self)
{
  g_return_val_if_fail (GNALK_IS_VOICE_RECORDER (self), FALSE);
  return self->recording;
}

gdouble
gnalk_voice_recorder_get_duration (GnalkVoiceRecorder *self)
{
  g_return_val_if_fail (GNALK_IS_VOICE_RECORDER (self), 0.0);

  if (!self->recording)
    return 0.0;

  gint64 now = g_get_monotonic_time ();
  return (gdouble) (now - self->start_time) / 1000000.0;
}
