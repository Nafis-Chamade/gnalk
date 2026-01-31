/* gnalk-video-player.c
 *
 * Reusable video player widget with play/pause, seekbar, and time display.
 * Uses GStreamer playbin for video decoding and playback.
 */

#include "gnalk-video-player.h"

#include <glib/gstdio.h>
#include <unistd.h>

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/app/gstappsink.h>
#endif

#define POSITION_UPDATE_INTERVAL_MS 100

/* Static pointer to currently playing video player (single-player policy) */
static GnalkVideoPlayer *current_playing_player = NULL;

struct _GnalkVideoPlayer
{
  GtkBox parent_instance;

  GtkWidget *video_picture;
  GtkWidget *play_button;
  GtkWidget *loading_spinner;
  GtkWidget *seekbar;
  GtkWidget *time_label;
  GtkWidget *controls_box;

#ifdef HAVE_GSTREAMER
  GstElement *playbin;
  GstElement *video_sink;
  gulong      sample_handler_id;
#endif

  char     *file_path;
  char     *temp_file_path;  /* For set_data mode */
  gboolean  is_playing;
  gboolean  is_loading;
  guint     position_update_id;
  gint64    duration;
  gboolean  seeking;  /* Block position updates during seek */
};

G_DEFINE_FINAL_TYPE (GnalkVideoPlayer, gnalk_video_player, GTK_TYPE_BOX)

enum {
  SIGNAL_PLAYBACK_FINISHED,
  SIGNAL_PLAY_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void update_time_label (GnalkVideoPlayer *self, gint64 position, gint64 duration);
static void stop_position_updates (GnalkVideoPlayer *self);
static void on_seekbar_value_changed (GtkRange *range, gpointer user_data);

static void
format_time (gint64 time_ns, char *buf, gsize buf_len)
{
  gint64 seconds = time_ns / GST_SECOND;
  gint64 minutes = seconds / 60;
  seconds = seconds % 60;

  g_snprintf (buf, buf_len, "%"G_GINT64_FORMAT":%02"G_GINT64_FORMAT, minutes, seconds);
}

static void
update_time_label (GnalkVideoPlayer *self, gint64 position, gint64 duration)
{
  char pos_str[16], dur_str[16];

  format_time (position, pos_str, sizeof (pos_str));
  format_time (duration, dur_str, sizeof (dur_str));

  g_autofree char *label = g_strdup_printf ("%s / %s", pos_str, dur_str);
  gtk_label_set_text (GTK_LABEL (self->time_label), label);
}

#ifdef HAVE_GSTREAMER

typedef struct {
  GtkWidget  *picture;
  GdkTexture *texture;
} VideoFrameUpdate;

static gboolean
update_video_frame_idle (gpointer user_data)
{
  VideoFrameUpdate *update = user_data;

  if (update->picture && GTK_IS_PICTURE (update->picture))
    gtk_picture_set_paintable (GTK_PICTURE (update->picture),
                               GDK_PAINTABLE (update->texture));

  g_object_unref (update->texture);
  g_free (update);
  return G_SOURCE_REMOVE;
}

static GstFlowReturn
on_video_sample (GstAppSink *appsink, gpointer user_data)
{
  GnalkVideoPlayer *self = GNALK_VIDEO_PLAYER (user_data);
  GstSample *sample = gst_app_sink_pull_sample (appsink);

  if (!sample)
    return GST_FLOW_OK;

  GstBuffer *buffer = gst_sample_get_buffer (sample);
  GstCaps *caps = gst_sample_get_caps (sample);

  if (buffer && caps)
    {
      GstVideoInfo video_info;
      if (gst_video_info_from_caps (&video_info, caps))
        {
          GstMapInfo map;
          if (gst_buffer_map (buffer, &map, GST_MAP_READ))
            {
              gsize expected_size = video_info.width * video_info.height * 3;
              if (map.size >= expected_size)
                {
                  GBytes *bytes = g_bytes_new (map.data, expected_size);
                  if (bytes)
                    {
                      GdkTexture *texture = gdk_memory_texture_new (
                          video_info.width,
                          video_info.height,
                          GDK_MEMORY_R8G8B8,
                          bytes,
                          video_info.width * 3);
                      g_bytes_unref (bytes);

                      if (texture)
                        {
                          VideoFrameUpdate *update = g_new0 (VideoFrameUpdate, 1);
                          update->picture = self->video_picture;
                          update->texture = texture;
                          g_idle_add (update_video_frame_idle, update);
                        }
                    }
                }
              gst_buffer_unmap (buffer, &map);
            }
        }
    }

  gst_sample_unref (sample);
  return GST_FLOW_OK;
}

static gboolean
on_position_update (gpointer user_data)
{
  GnalkVideoPlayer *self = GNALK_VIDEO_PLAYER (user_data);

  if (!self->playbin || !self->is_playing || self->seeking)
    return G_SOURCE_CONTINUE;

  /* Try to get duration if we don't have it yet */
  if (self->duration <= 0)
    gst_element_query_duration (self->playbin, GST_FORMAT_TIME, &self->duration);

  gint64 position = 0;
  if (gst_element_query_position (self->playbin, GST_FORMAT_TIME, &position))
    {
      if (self->duration > 0)
        {
          double fraction = (double) position / (double) self->duration;
          /* Block signal to prevent feedback loop */
          g_signal_handlers_block_by_func (self->seekbar,
                                           G_CALLBACK (on_seekbar_value_changed), self);
          gtk_range_set_value (GTK_RANGE (self->seekbar), fraction);
          g_signal_handlers_unblock_by_func (self->seekbar,
                                             G_CALLBACK (on_seekbar_value_changed), self);
        }

      update_time_label (self, position, self->duration);
    }

  return G_SOURCE_CONTINUE;
}

static void
on_bus_message (GstBus     *bus G_GNUC_UNUSED,
                GstMessage *message,
                gpointer    user_data)
{
  GnalkVideoPlayer *self = GNALK_VIDEO_PLAYER (user_data);

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

      gtk_range_set_value (GTK_RANGE (self->seekbar), 0.0);
      update_time_label (self, 0, self->duration);

      if (current_playing_player == self)
        current_playing_player = NULL;

      g_signal_emit (self, signals[SIGNAL_PLAYBACK_FINISHED], 0);
      break;

    case GST_MESSAGE_ERROR:
      {
        GError *err = NULL;
        gchar *debug_info = NULL;

        gst_message_parse_error (message, &err, &debug_info);
        g_warning ("Video playback error: %s", err->message);
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
start_position_updates (GnalkVideoPlayer *self)
{
  if (self->position_update_id == 0)
    {
      self->position_update_id = g_timeout_add (POSITION_UPDATE_INTERVAL_MS,
                                                 on_position_update, self);
    }
}

#endif /* HAVE_GSTREAMER */

static void
stop_position_updates (GnalkVideoPlayer *self)
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
  GnalkVideoPlayer *self = GNALK_VIDEO_PLAYER (user_data);

  /* If no file is set, emit signal to request file download */
  if (!self->file_path && !self->temp_file_path)
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
      /* Stop any other playing video first (single-player policy) */
      if (current_playing_player && current_playing_player != self)
        gnalk_video_player_stop (current_playing_player);

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
on_seekbar_value_changed (GtkRange *range,
                          gpointer  user_data)
{
  GnalkVideoPlayer *self = GNALK_VIDEO_PLAYER (user_data);

#ifdef HAVE_GSTREAMER
  if (!self->playbin || self->duration <= 0)
    return;

  self->seeking = TRUE;

  double value = gtk_range_get_value (range);
  gint64 position = (gint64) (value * self->duration);

  gst_element_seek_simple (self->playbin, GST_FORMAT_TIME,
                           GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                           position);

  update_time_label (self, position, self->duration);

  self->seeking = FALSE;
#endif /* HAVE_GSTREAMER */
}

static void
gnalk_video_player_dispose (GObject *object)
{
  GnalkVideoPlayer *self = GNALK_VIDEO_PLAYER (object);

  /* Stop playback and clean up GStreamer */
  gnalk_video_player_stop (self);

#ifdef HAVE_GSTREAMER
  if (self->playbin)
    {
      if (self->sample_handler_id && self->video_sink)
        {
          g_signal_handler_disconnect (self->video_sink, self->sample_handler_id);
          self->sample_handler_id = 0;
        }

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

  G_OBJECT_CLASS (gnalk_video_player_parent_class)->dispose (object);
}

static void
gnalk_video_player_finalize (GObject *object)
{
  GnalkVideoPlayer *self = GNALK_VIDEO_PLAYER (object);

  g_clear_pointer (&self->file_path, g_free);

  /* Clean up temp file if we created one */
  if (self->temp_file_path)
    {
      g_unlink (self->temp_file_path);
      g_clear_pointer (&self->temp_file_path, g_free);
    }

  if (current_playing_player == self)
    current_playing_player = NULL;

  G_OBJECT_CLASS (gnalk_video_player_parent_class)->finalize (object);
}

static void
gnalk_video_player_class_init (GnalkVideoPlayerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnalk_video_player_dispose;
  object_class->finalize = gnalk_video_player_finalize;

  /**
   * GnalkVideoPlayer::playback-finished:
   * @self: the video player
   *
   * Emitted when video playback reaches the end.
   */
  signals[SIGNAL_PLAYBACK_FINISHED] =
    g_signal_new ("playback-finished",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * GnalkVideoPlayer::play-requested:
   * @self: the video player
   *
   * Emitted when play is clicked but no file is loaded yet.
   */
  signals[SIGNAL_PLAY_REQUESTED] =
    g_signal_new ("play-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/net/skilpa/gnalk/gnalk-video-player.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkVideoPlayer, video_picture);
  gtk_widget_class_bind_template_child (widget_class, GnalkVideoPlayer, play_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkVideoPlayer, loading_spinner);
  gtk_widget_class_bind_template_child (widget_class, GnalkVideoPlayer, seekbar);
  gtk_widget_class_bind_template_child (widget_class, GnalkVideoPlayer, time_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkVideoPlayer, controls_box);
}

static void
gnalk_video_player_init (GnalkVideoPlayer *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->is_playing = FALSE;
  self->is_loading = FALSE;
  self->duration = 0;
  self->position_update_id = 0;
  self->seeking = FALSE;

  /* Connect signals */
  g_signal_connect (self->play_button, "clicked",
                    G_CALLBACK (on_play_button_clicked), self);
  g_signal_connect (self->seekbar, "value-changed",
                    G_CALLBACK (on_seekbar_value_changed), self);

#ifdef HAVE_GSTREAMER
  /* Create playbin element */
  self->playbin = gst_element_factory_make ("playbin", "video-player");
  if (self->playbin)
    {
      /* Create custom video sink using appsink for GTK display */
      GstElement *video_bin = gst_bin_new ("video-sink-bin");
      GstElement *convert = gst_element_factory_make ("videoconvert", NULL);
      GstElement *scale = gst_element_factory_make ("videoscale", NULL);
      GstElement *capsfilter = gst_element_factory_make ("capsfilter", NULL);
      GstElement *appsink = gst_element_factory_make ("appsink", "videosink");

      if (convert && scale && capsfilter && appsink)
        {
          /* Set caps for RGB output */
          GstCaps *caps = gst_caps_from_string ("video/x-raw,format=RGB");
          g_object_set (capsfilter, "caps", caps, NULL);
          gst_caps_unref (caps);

          /* Configure appsink */
          g_object_set (appsink,
                        "emit-signals", TRUE,
                        "max-buffers", 1,
                        "drop", TRUE,
                        "sync", TRUE,
                        NULL);

          gst_bin_add_many (GST_BIN (video_bin), convert, scale, capsfilter, appsink, NULL);
          gst_element_link_many (convert, scale, capsfilter, appsink, NULL);

          /* Create ghost pad */
          GstPad *pad = gst_element_get_static_pad (convert, "sink");
          GstPad *ghost = gst_ghost_pad_new ("sink", pad);
          gst_element_add_pad (video_bin, ghost);
          gst_object_unref (pad);

          g_object_set (self->playbin, "video-sink", video_bin, NULL);

          self->video_sink = appsink;
          self->sample_handler_id = g_signal_connect (appsink, "new-sample",
                                                       G_CALLBACK (on_video_sample), self);
        }
      else
        {
          g_warning ("VideoPlayer: Failed to create video sink elements");
          if (convert) gst_object_unref (convert);
          if (scale) gst_object_unref (scale);
          if (capsfilter) gst_object_unref (capsfilter);
          if (appsink) gst_object_unref (appsink);
          gst_object_unref (video_bin);
        }

      /* Use autoaudiosink for audio */
      GstElement *audio_sink = gst_element_factory_make ("autoaudiosink", NULL);
      if (audio_sink)
        g_object_set (self->playbin, "audio-sink", audio_sink, NULL);

      /* Set up bus watch for messages */
      GstBus *bus = gst_element_get_bus (self->playbin);
      gst_bus_add_signal_watch (bus);
      g_signal_connect (bus, "message", G_CALLBACK (on_bus_message), self);
      gst_object_unref (bus);
    }
  else
    {
      g_warning ("VideoPlayer: Failed to create GStreamer playbin element");
    }
#endif /* HAVE_GSTREAMER */
}

GnalkVideoPlayer *
gnalk_video_player_new (void)
{
  return g_object_new (GNALK_TYPE_VIDEO_PLAYER, NULL);
}

void
gnalk_video_player_set_file (GnalkVideoPlayer *self,
                              const char        *file_path)
{
  g_return_if_fail (GNALK_IS_VIDEO_PLAYER (self));

  /* Stop current playback */
  gnalk_video_player_stop (self);

  g_clear_pointer (&self->file_path, g_free);
  if (self->temp_file_path)
    {
      g_unlink (self->temp_file_path);
      g_clear_pointer (&self->temp_file_path, g_free);
    }

  if (!file_path)
    return;

  self->file_path = g_strdup (file_path);

#ifdef HAVE_GSTREAMER
  if (self->playbin)
    {
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
  gnalk_video_player_set_loading (self, FALSE);
}

void
gnalk_video_player_set_data (GnalkVideoPlayer *self,
                              GBytes            *data)
{
  g_return_if_fail (GNALK_IS_VIDEO_PLAYER (self));
  g_return_if_fail (data != NULL);

  /* Stop current playback */
  gnalk_video_player_stop (self);

  g_clear_pointer (&self->file_path, g_free);
  if (self->temp_file_path)
    {
      g_unlink (self->temp_file_path);
      g_clear_pointer (&self->temp_file_path, g_free);
    }

  /* Write data to temp file for GStreamer playback */
  GError *error = NULL;
  int fd = g_file_open_tmp ("gnalk-video-XXXXXX.webm", &self->temp_file_path, &error);
  if (fd < 0)
    {
      g_warning ("VideoPlayer: Failed to create temp file: %s", error->message);
      g_error_free (error);
      return;
    }

  gsize data_size;
  const guchar *data_ptr = g_bytes_get_data (data, &data_size);
  ssize_t written = write (fd, data_ptr, data_size);
  close (fd);

  if (written != (ssize_t) data_size)
    {
      g_warning ("VideoPlayer: Failed to write temp file");
      g_unlink (self->temp_file_path);
      g_clear_pointer (&self->temp_file_path, g_free);
      return;
    }

#ifdef HAVE_GSTREAMER
  if (self->playbin)
    {
      g_autofree char *uri = g_filename_to_uri (self->temp_file_path, NULL, NULL);
      if (uri)
        {
          g_object_set (self->playbin, "uri", uri, NULL);

          /* Move to PAUSED to preroll and get duration */
          gst_element_set_state (self->playbin, GST_STATE_PAUSED);

          self->duration = 0;
          gst_element_query_duration (self->playbin, GST_FORMAT_TIME, &self->duration);

          update_time_label (self, 0, self->duration);
        }
    }
#endif /* HAVE_GSTREAMER */

  gnalk_video_player_set_loading (self, FALSE);
}

void
gnalk_video_player_play (GnalkVideoPlayer *self)
{
  g_return_if_fail (GNALK_IS_VIDEO_PLAYER (self));

  if (self->is_playing)
    return;

#ifdef HAVE_GSTREAMER
  if (!self->playbin)
    return;

  /* Stop any other playing video first (single-player policy) */
  if (current_playing_player && current_playing_player != self)
    gnalk_video_player_stop (current_playing_player);

  gst_element_set_state (self->playbin, GST_STATE_PLAYING);
  self->is_playing = TRUE;
  gtk_button_set_icon_name (GTK_BUTTON (self->play_button), "media-playback-pause-symbolic");
  start_position_updates (self);

  current_playing_player = self;
#endif
}

void
gnalk_video_player_pause (GnalkVideoPlayer *self)
{
  g_return_if_fail (GNALK_IS_VIDEO_PLAYER (self));

  if (!self->is_playing)
    return;

#ifdef HAVE_GSTREAMER
  if (!self->playbin)
    return;

  gst_element_set_state (self->playbin, GST_STATE_PAUSED);
  self->is_playing = FALSE;
  gtk_button_set_icon_name (GTK_BUTTON (self->play_button), "media-playback-start-symbolic");
  stop_position_updates (self);

  if (current_playing_player == self)
    current_playing_player = NULL;
#endif
}

void
gnalk_video_player_stop (GnalkVideoPlayer *self)
{
  g_return_if_fail (GNALK_IS_VIDEO_PLAYER (self));

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
  gtk_range_set_value (GTK_RANGE (self->seekbar), 0.0);

  if (current_playing_player == self)
    current_playing_player = NULL;
}

void
gnalk_video_player_set_loading (GnalkVideoPlayer *self,
                                 gboolean           loading)
{
  g_return_if_fail (GNALK_IS_VIDEO_PLAYER (self));

  self->is_loading = loading;

  gtk_widget_set_visible (self->play_button, !loading);
  gtk_widget_set_visible (self->loading_spinner, loading);

  if (loading)
    gtk_spinner_start (GTK_SPINNER (self->loading_spinner));
  else
    gtk_spinner_stop (GTK_SPINNER (self->loading_spinner));

  /* Disable seekbar while loading */
  gtk_widget_set_sensitive (self->seekbar, !loading);
}

gboolean
gnalk_video_player_is_playing (GnalkVideoPlayer *self)
{
  g_return_val_if_fail (GNALK_IS_VIDEO_PLAYER (self), FALSE);
  return self->is_playing;
}

double
gnalk_video_player_get_duration (GnalkVideoPlayer *self)
{
  g_return_val_if_fail (GNALK_IS_VIDEO_PLAYER (self), 0.0);
  return (double) self->duration / GST_SECOND;
}
