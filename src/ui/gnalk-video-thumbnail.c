/* gnalk-video-thumbnail.c
 *
 * Widget that displays first frame of a video with play overlay icon.
 */

#include "gnalk-video-thumbnail.h"

#include <glib/gstdio.h>

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video-info.h>
#endif

struct _GnalkVideoThumbnail
{
  GtkWidget parent_instance;

  GtkWidget *overlay;
  GtkWidget *picture;
  GtkWidget *play_icon;
  GtkWidget *loading_spinner;

  char *file_path;
  char *file_id;

  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (GnalkVideoThumbnail, gnalk_video_thumbnail, GTK_TYPE_WIDGET)

enum {
  SIGNAL_CLICKED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static char *
get_cache_dir (void)
{
  return g_build_filename (g_get_user_cache_dir (), "gnalk", "thumbs", NULL);
}

static char *
get_cached_thumbnail_path (const char *file_id)
{
  if (!file_id || !*file_id)
    return NULL;

  g_autofree char *cache_dir = get_cache_dir ();
  return g_build_filename (cache_dir, g_strdup_printf ("%s.jpg", file_id), NULL);
}

static gboolean
load_cached_thumbnail (GnalkVideoThumbnail *self)
{
  if (!self->file_id || !*self->file_id)
    return FALSE;

  g_autofree char *cache_path = get_cached_thumbnail_path (self->file_id);
  if (!cache_path || !g_file_test (cache_path, G_FILE_TEST_EXISTS))
    return FALSE;

  GError *error = NULL;
  GdkTexture *texture = gdk_texture_new_from_filename (cache_path, &error);
  if (!texture)
    {
      g_debug ("VideoThumbnail: Failed to load cached thumbnail: %s", error->message);
      g_error_free (error);
      return FALSE;
    }

  gtk_picture_set_paintable (GTK_PICTURE (self->picture), GDK_PAINTABLE (texture));
  g_object_unref (texture);
  return TRUE;
}

static void
save_thumbnail_to_cache (GnalkVideoThumbnail *self,
                         GdkTexture           *texture)
{
  if (!self->file_id || !*self->file_id || !texture)
    return;

  /* Ensure cache directory exists */
  g_autofree char *cache_dir = get_cache_dir ();
  g_mkdir_with_parents (cache_dir, 0755);

  g_autofree char *cache_path = get_cached_thumbnail_path (self->file_id);
  if (!cache_path)
    return;

  /* Save texture as JPEG. Download with an explicit non-premultiplied RGBA
   * layout — the default gdk_texture_download() format is premultiplied BGRA,
   * which fed to GdkPixbuf as RGBA swapped red/blue and mangled alpha. */
  int width = gdk_texture_get_width (texture);
  int height = gdk_texture_get_height (texture);

  GdkTextureDownloader *downloader = gdk_texture_downloader_new (texture);
  gdk_texture_downloader_set_format (downloader, GDK_MEMORY_R8G8B8A8);
  gsize stride = 0;
  GBytes *bytes = gdk_texture_downloader_download_bytes (downloader, &stride);
  gdk_texture_downloader_free (downloader);

  /* Create pixbuf from RGBA data (keeps a ref on @bytes for its lifetime) */
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_bytes (
      bytes, GDK_COLORSPACE_RGB, TRUE, 8,
      width, height, stride);

  if (pixbuf)
    {
      GError *error = NULL;
      if (!gdk_pixbuf_save (pixbuf, cache_path, "jpeg", &error, "quality", "85", NULL))
        {
          g_debug ("VideoThumbnail: Failed to save thumbnail: %s", error->message);
          g_error_free (error);
        }
      g_object_unref (pixbuf);
    }

  if (bytes)
    g_bytes_unref (bytes);
}

#ifdef HAVE_GSTREAMER

GdkTexture *
gnalk_video_thumbnail_extract_frame (const char *file_path)
{
  if (!file_path)
    return NULL;

  g_autofree char *uri = g_filename_to_uri (file_path, NULL, NULL);
  if (!uri)
    return NULL;

  /* Create pipeline: uridecodebin ! videoconvert ! videoscale ! capsfilter ! appsink */
  g_autofree char *pipeline_str = g_strdup_printf (
      "uridecodebin uri=\"%s\" ! videoconvert ! videoscale ! "
      "capsfilter caps=\"video/x-raw,format=RGB,width=[1,400],height=[1,300]\" ! "
      "appsink name=sink max-buffers=1 drop=true",
      uri);

  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch (pipeline_str, &error);
  if (!pipeline)
    {
      g_warning ("VideoThumbnail: Failed to create pipeline: %s", error->message);
      g_error_free (error);
      return NULL;
    }

  GstElement *appsink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
  if (!appsink)
    {
      g_warning ("VideoThumbnail: Failed to get appsink");
      gst_object_unref (pipeline);
      return NULL;
    }

  /* Configure appsink */
  g_object_set (appsink, "sync", FALSE, NULL);

  /* Start pipeline */
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  /* Wait for preroll with timeout */
  GstStateChangeReturn ret = gst_element_get_state (pipeline, NULL, NULL, 5 * GST_SECOND);
  if (ret == GST_STATE_CHANGE_FAILURE)
    {
      g_warning ("VideoThumbnail: Pipeline failed to preroll");
      gst_element_set_state (pipeline, GST_STATE_NULL);
      gst_object_unref (appsink);
      gst_object_unref (pipeline);
      return NULL;
    }

  /* Pull a sample */
  GstSample *sample = gst_app_sink_try_pull_sample (GST_APP_SINK (appsink), 2 * GST_SECOND);

  GdkTexture *texture = NULL;
  if (sample)
    {
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
                      texture = gdk_memory_texture_new (
                          video_info.width,
                          video_info.height,
                          GDK_MEMORY_R8G8B8,
                          bytes,
                          video_info.width * 3);
                      g_bytes_unref (bytes);
                    }
                  gst_buffer_unmap (buffer, &map);
                }
            }
        }
      gst_sample_unref (sample);
    }

  /* Clean up */
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (appsink);
  gst_object_unref (pipeline);

  return texture;
}

#else /* !HAVE_GSTREAMER */

GdkTexture *
gnalk_video_thumbnail_extract_frame (const char *file_path G_GNUC_UNUSED)
{
  g_warning ("VideoThumbnail: GStreamer not available");
  return NULL;
}

#endif /* HAVE_GSTREAMER */

typedef struct {
  GnalkVideoThumbnail *self;
  char *file_path;
} ExtractThumbnailData;

static void
extract_thumbnail_data_free (ExtractThumbnailData *data)
{
  g_free (data->file_path);
  g_free (data);
}

static void
extract_thumbnail_thread (GTask        *task,
                          gpointer      source_object G_GNUC_UNUSED,
                          gpointer      task_data,
                          GCancellable *cancellable)
{
  ExtractThumbnailData *data = task_data;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      return;
    }

  GdkTexture *texture = gnalk_video_thumbnail_extract_frame (data->file_path);
  if (texture)
    g_task_return_pointer (task, texture, g_object_unref);
  else
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to extract thumbnail");
}

static void
on_thumbnail_extracted (GObject      *source_object G_GNUC_UNUSED,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  GnalkVideoThumbnail *self = GNALK_VIDEO_THUMBNAIL (user_data);

  GError *error = NULL;
  GdkTexture *texture = g_task_propagate_pointer (G_TASK (result), &error);

  /* Stop spinner */
  gtk_spinner_stop (GTK_SPINNER (self->loading_spinner));
  gtk_widget_set_visible (self->loading_spinner, FALSE);

  if (texture)
    {
      gtk_picture_set_paintable (GTK_PICTURE (self->picture), GDK_PAINTABLE (texture));
      gtk_widget_set_visible (self->picture, TRUE);
      gtk_widget_set_visible (self->play_icon, TRUE);

      /* Cache the thumbnail */
      save_thumbnail_to_cache (self, texture);

      g_object_unref (texture);
    }
  else
    {
      g_debug ("VideoThumbnail: Failed to extract: %s", error ? error->message : "unknown");
      g_clear_error (&error);
      /* Show play icon even without thumbnail */
      gtk_widget_set_visible (self->play_icon, TRUE);
    }

  g_object_unref (self);
}

static void
on_click_pressed (GtkGestureClick      *gesture G_GNUC_UNUSED,
                  int                   n_press G_GNUC_UNUSED,
                  double                x G_GNUC_UNUSED,
                  double                y G_GNUC_UNUSED,
                  GnalkVideoThumbnail *self)
{
  g_signal_emit (self, signals[SIGNAL_CLICKED], 0);
}

static void
gnalk_video_thumbnail_dispose (GObject *object)
{
  GnalkVideoThumbnail *self = GNALK_VIDEO_THUMBNAIL (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  g_clear_pointer (&self->overlay, gtk_widget_unparent);

  G_OBJECT_CLASS (gnalk_video_thumbnail_parent_class)->dispose (object);
}

static void
gnalk_video_thumbnail_finalize (GObject *object)
{
  GnalkVideoThumbnail *self = GNALK_VIDEO_THUMBNAIL (object);

  g_free (self->file_path);
  g_free (self->file_id);

  G_OBJECT_CLASS (gnalk_video_thumbnail_parent_class)->finalize (object);
}

static void
gnalk_video_thumbnail_class_init (GnalkVideoThumbnailClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnalk_video_thumbnail_dispose;
  object_class->finalize = gnalk_video_thumbnail_finalize;

  /**
   * GnalkVideoThumbnail::clicked:
   * @self: the thumbnail widget
   *
   * Emitted when the thumbnail is clicked.
   */
  signals[SIGNAL_CLICKED] =
    g_signal_new ("clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnalk_video_thumbnail_init (GnalkVideoThumbnail *self)
{
  self->cancellable = g_cancellable_new ();

  /* Main overlay container */
  self->overlay = gtk_overlay_new ();
  gtk_widget_set_parent (self->overlay, GTK_WIDGET (self));

  /* Picture for thumbnail - preserve aspect ratio */
  self->picture = gtk_picture_new ();
  gtk_picture_set_content_fit (GTK_PICTURE (self->picture), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_add_css_class (self->picture, "video-thumbnail");
  gtk_overlay_set_child (GTK_OVERLAY (self->overlay), self->picture);

  /* Play icon overlay */
  self->play_icon = gtk_image_new_from_icon_name ("media-playback-start-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (self->play_icon), 24);
  gtk_widget_add_css_class (self->play_icon, "video-play-overlay");
  gtk_widget_set_halign (self->play_icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->play_icon, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->play_icon, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay), self->play_icon);

  /* Loading spinner */
  self->loading_spinner = gtk_spinner_new ();
  gtk_widget_set_halign (self->loading_spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->loading_spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->loading_spinner, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay), self->loading_spinner);

  /* Click gesture */
  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_click_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

GnalkVideoThumbnail *
gnalk_video_thumbnail_new (void)
{
  return g_object_new (GNALK_TYPE_VIDEO_THUMBNAIL, NULL);
}

void
gnalk_video_thumbnail_set_file (GnalkVideoThumbnail *self,
                                 const char           *file_path)
{
  g_return_if_fail (GNALK_IS_VIDEO_THUMBNAIL (self));

  /* Cancel any pending extraction */
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  g_free (self->file_path);
  self->file_path = g_strdup (file_path);

  if (!file_path)
    {
      gtk_picture_set_paintable (GTK_PICTURE (self->picture), NULL);
      gtk_widget_set_visible (self->play_icon, FALSE);
      return;
    }

  /* Try to load from cache first */
  if (load_cached_thumbnail (self))
    {
      gtk_widget_set_visible (self->picture, TRUE);
      gtk_widget_set_visible (self->play_icon, TRUE);
      return;
    }

  /* Extract thumbnail asynchronously */
  gtk_widget_set_visible (self->loading_spinner, TRUE);
  gtk_spinner_start (GTK_SPINNER (self->loading_spinner));

  ExtractThumbnailData *data = g_new0 (ExtractThumbnailData, 1);
  data->self = self;
  data->file_path = g_strdup (file_path);

  GTask *task = g_task_new (NULL, self->cancellable, on_thumbnail_extracted, g_object_ref (self));
  g_task_set_task_data (task, data, (GDestroyNotify) extract_thumbnail_data_free);
  g_task_run_in_thread (task, extract_thumbnail_thread);
  g_object_unref (task);
}

void
gnalk_video_thumbnail_set_file_id (GnalkVideoThumbnail *self,
                                    const char           *file_id)
{
  g_return_if_fail (GNALK_IS_VIDEO_THUMBNAIL (self));

  g_free (self->file_id);
  self->file_id = g_strdup (file_id);

  /* Try to load cached thumbnail if it exists */
  if (load_cached_thumbnail (self))
    {
      gtk_widget_set_visible (self->picture, TRUE);
      gtk_widget_set_visible (self->play_icon, TRUE);
    }
  else
    {
      /* No cached thumbnail - show picture with background and play icon as placeholder */
      gtk_widget_set_visible (self->picture, TRUE);
      gtk_widget_set_visible (self->play_icon, TRUE);
    }
}

void
gnalk_video_thumbnail_set_loading (GnalkVideoThumbnail *self,
                                    gboolean              loading)
{
  g_return_if_fail (GNALK_IS_VIDEO_THUMBNAIL (self));

  gtk_widget_set_visible (self->loading_spinner, loading);

  if (loading)
    gtk_spinner_start (GTK_SPINNER (self->loading_spinner));
  else
    gtk_spinner_stop (GTK_SPINNER (self->loading_spinner));
}

void
gnalk_video_thumbnail_set_texture (GnalkVideoThumbnail *self,
                                    GdkTexture           *texture)
{
  g_return_if_fail (GNALK_IS_VIDEO_THUMBNAIL (self));

  if (texture)
    {
      gtk_picture_set_paintable (GTK_PICTURE (self->picture), GDK_PAINTABLE (texture));
      gtk_widget_set_visible (self->picture, TRUE);
      gtk_widget_set_visible (self->play_icon, TRUE);
      gtk_widget_set_visible (self->loading_spinner, FALSE);
      gtk_spinner_stop (GTK_SPINNER (self->loading_spinner));
    }
  else
    {
      gtk_picture_set_paintable (GTK_PICTURE (self->picture), NULL);
      gtk_widget_set_visible (self->play_icon, FALSE);
    }
}
