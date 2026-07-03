/* gnalk-camera-preview.c
 *
 * Reusable camera preview widget with video recording support.
 * Uses GStreamer pipeline with tee element for simultaneous preview and recording.
 */

#include "gnalk-camera-preview.h"
#include "gnalk-camera-utils.h"

#include <gio/gio.h>
#include <glib/gstdio.h>

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video-info.h>
#endif

struct _GnalkCameraPreview
{
  GtkWidget parent_instance;

  GtkWidget *picture;
  GnalkCameraPreviewPosition position;

  GSettings *settings;
  gulong     settings_handler_id;

#ifdef HAVE_GSTREAMER
  GstElement *pipeline;
  GstElement *appsink;
  gulong      sample_handler_id;

  /* Photo capture - stores last frame for capture */
  GMutex      frame_mutex;
  GBytes     *last_frame;
  int         last_frame_width;
  int         last_frame_height;

  /* Video recording state */
  gboolean    recording_video;
  char       *video_output_path;
  GstElement *tee;               /* Tee element for branching */
  GstPad     *tee_preview_pad;   /* Preview branch src pad */
  GstPad     *tee_record_pad;    /* Recording branch src pad */

  /* Recording branch elements (dynamically added) */
  GstElement *rec_queue;
  GstElement *rec_convert;
  GstElement *rec_scale;
  GstElement *rec_caps;
  GstElement *rec_encoder;
  GstElement *audio_src;
  GstElement *audio_queue;
  GstElement *audio_convert;
  GstElement *audio_resample;
  GstElement *audio_enc;
  GstElement *muxer;
  GstElement *filesink;

  /* For waiting on EOS from recording */
  gboolean    eos_received;
  GMutex      eos_mutex;
  GCond       eos_cond;
  guint       bus_watch_id;
#endif
};

G_DEFINE_FINAL_TYPE (GnalkCameraPreview, gnalk_camera_preview, GTK_TYPE_WIDGET)

static void on_camera_quality_changed (GSettings *settings,
                                        const gchar *key,
                                        gpointer user_data);

enum {
  PROP_0,
  PROP_POSITION,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

#ifdef HAVE_GSTREAMER

typedef struct {
  GtkWidget  *picture;
  GdkTexture *texture;
} FrameUpdate;

static gboolean
update_frame_idle (gpointer user_data)
{
  FrameUpdate *update = user_data;

  /* Check if picture widget is still valid (weak ref cleared on destroy) */
  if (update->picture)
    {
      if (GTK_IS_PICTURE (update->picture))
        gtk_picture_set_paintable (GTK_PICTURE (update->picture),
                                   GDK_PAINTABLE (update->texture));
      g_object_remove_weak_pointer (G_OBJECT (update->picture), (gpointer *)&update->picture);
    }

  g_object_unref (update->texture);
  g_free (update);
  return G_SOURCE_REMOVE;
}

static GstFlowReturn
on_new_sample (GstAppSink *appsink, gpointer user_data)
{
  GnalkCameraPreview *self = GNALK_CAMERA_PREVIEW (user_data);
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
                      /* Store last frame for photo capture */
                      g_mutex_lock (&self->frame_mutex);
                      g_clear_pointer (&self->last_frame, g_bytes_unref);
                      self->last_frame = g_bytes_ref (bytes);
                      self->last_frame_width = video_info.width;
                      self->last_frame_height = video_info.height;
                      g_mutex_unlock (&self->frame_mutex);

                      GdkTexture *texture = gdk_memory_texture_new (
                          video_info.width,
                          video_info.height,
                          GDK_MEMORY_R8G8B8,
                          bytes,
                          video_info.width * 3);
                      g_bytes_unref (bytes);

                      if (texture)
                        {
                          static int frame_count = 0;
                          if (frame_count++ % 30 == 0)
                            g_debug ("CameraPreview: frame %d, size=%dx%d",
                                       frame_count, video_info.width, video_info.height);

                          FrameUpdate *update = g_new0 (FrameUpdate, 1);
                          update->picture = self->picture;
                          update->texture = texture;
                          /* Add weak reference to detect if widget is destroyed before callback runs */
                          g_object_add_weak_pointer (G_OBJECT (update->picture), (gpointer *)&update->picture);
                          g_idle_add (update_frame_idle, update);
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

/* Check if we're running on OnePlus 6 by looking for its camera paths */
static gboolean
is_oneplus6_device (void)
{
  /* Check if the OP6 front camera path exists via libcamera */
  GstDeviceMonitor *monitor = gst_device_monitor_new ();
  gst_device_monitor_add_filter (monitor, "Video/Source", NULL);
  gboolean is_op6 = FALSE;

  if (gst_device_monitor_start (monitor))
    {
      GList *devices = gst_device_monitor_get_devices (monitor);
      for (GList *l = devices; l != NULL; l = l->next)
        {
          GstDevice *device = GST_DEVICE (l->data);
          g_autofree gchar *name = gst_device_get_display_name (device);
          /* OP6 cameras are named "Built-in Front Camera" and "Built-in Back Camera" */
          if (name && (strstr (name, "Built-in Front") || strstr (name, "Built-in Back")))
            {
              is_op6 = TRUE;
              break;
            }
        }
      g_list_free_full (devices, gst_object_unref);
      gst_device_monitor_stop (monitor);
    }
  gst_object_unref (monitor);
  return is_op6;
}

/* Create camera source - handles OP6 and generic devices */
static GstElement *
find_camera_device (GnalkCameraPreviewPosition  position,
                    GnalkCameraPosition        *out_cam_position,
                    gint                        *out_rotation)
{
  gboolean is_op6 = is_oneplus6_device ();

  if (is_op6 && position != GNALK_CAMERA_PREVIEW_GENERIC)
    {
      /* OnePlus 6: use libcamerasrc with specific camera paths */
      gboolean is_front = (position == GNALK_CAMERA_PREVIEW_FRONT);

      GstElement *source = gst_element_factory_make ("libcamerasrc", "videosrc");
      if (!source)
        {
          g_warning ("CameraPreview: failed to create libcamerasrc");
          return NULL;
        }

      /* OnePlus 6 camera paths - front is i2c-bus@0, back is i2c-bus@1 */
      const char *camera_path = is_front
          ? "/base/soc@0/cci@ac4a000/i2c-bus@0/camera@10"
          : "/base/soc@0/cci@ac4a000/i2c-bus@1/camera@10";
      g_object_set (source, "camera-name", camera_path, NULL);

      if (out_cam_position)
        *out_cam_position = is_front ? GNALK_CAMERA_FRONT : GNALK_CAMERA_BACK;
      if (out_rotation)
        *out_rotation = is_front ? 90 : 270;

      g_message ("CameraPreview: using libcamerasrc for %s camera (%s)",
                 is_front ? "front" : "back", camera_path);
      return source;
    }
  else
    {
      /* Generic device (laptop/desktop): use v4l2src like the call pipeline does.
       * pipewiresrc without portal fd doesn't reliably access webcams. */
      GstElement *source = gst_element_factory_make ("v4l2src", "videosrc");
      if (!source)
        {
          g_warning ("CameraPreview: failed to create v4l2src");
          return NULL;
        }
      g_message ("CameraPreview: using v4l2src for generic camera");

      if (out_cam_position)
        *out_cam_position = GNALK_CAMERA_GENERIC;
      if (out_rotation)
        *out_rotation = 0;  /* Generic cameras typically don't need rotation */

      return source;
    }
}

static void
create_pipeline (GnalkCameraPreview *self)
{
  GnalkCameraPosition cam_pos = GNALK_CAMERA_FRONT;
  gint rotation = 0;

  GstElement *src = find_camera_device (self->position, &cam_pos, &rotation);
  if (!src)
    {
      g_warning ("CameraPreview: failed to find camera device");
      return;
    }

  /* Get shared camera config based on quality setting and actual camera position */
  GnalkCameraQuality quality = gnalk_camera_quality_from_settings ();
  GnalkCameraConfig config = gnalk_camera_get_config (quality, cam_pos);

  g_message ("CameraPreview: using quality=%d, resolution=%dx%d, crop=%d",
             quality, config.src_width, config.src_height, config.preview_crop);

  /* Pipeline structure with tee for recording support:
   * src -> srccaps -> queue -> convert -> flip -> rate -> ratecaps -> tee
   *                                                                    |-> preview_queue -> scale -> scalecaps -> crop -> convert2 -> fmtcaps -> appsink
   *                                                                    \-> (recording branch added dynamically) */
  GstElement *pipeline = gst_pipeline_new ("camera-preview");
  GstElement *srccaps = gst_element_factory_make ("capsfilter", "srccaps");
  GstElement *queue = gst_element_factory_make ("queue", NULL);
  GstElement *convert = gst_element_factory_make ("videoconvert", NULL);
  GstElement *flip = gst_element_factory_make ("videoflip", NULL);
  GstElement *rate = gst_element_factory_make ("videorate", NULL);
  GstElement *ratecaps = gst_element_factory_make ("capsfilter", NULL);
  GstElement *tee = gst_element_factory_make ("tee", "video_tee");

  /* Preview branch elements */
  GstElement *prev_queue = gst_element_factory_make ("queue", "preview_queue");
  GstElement *scale = gst_element_factory_make ("videoscale", NULL);
  GstElement *scalecaps = gst_element_factory_make ("capsfilter", NULL);
  GstElement *crop = gst_element_factory_make ("videocrop", NULL);
  GstElement *convert2 = gst_element_factory_make ("videoconvert", NULL);
  GstElement *fmtcaps = gst_element_factory_make ("capsfilter", NULL);
  GstElement *appsink = gst_element_factory_make ("appsink", "appsink");

  if (!srccaps || !queue || !convert || !flip || !rate || !ratecaps || !tee ||
      !prev_queue || !scale || !scalecaps || !crop || !convert2 || !fmtcaps || !appsink)
    {
      g_warning ("CameraPreview: failed to create pipeline elements");
      gst_object_unref (src);
      gst_object_unref (pipeline);
      return;
    }

  /* Request specific resolution from camera for mobile devices.
   * For generic cameras (laptops), let v4l2src pick native resolution. */
  if (cam_pos != GNALK_CAMERA_GENERIC)
    {
      g_autofree gchar *src_caps_str = g_strdup_printf (
          "video/x-raw,width=%d,height=%d", config.src_width, config.src_height);
      GstCaps *src_c = gst_caps_from_string (src_caps_str);
      g_object_set (srccaps, "caps", src_c, NULL);
      gst_caps_unref (src_c);
    }
  /* else: no srccaps restriction for generic cameras */

  /* Queue settings */
  g_object_set (queue,
                "max-size-buffers", 5,
                "max-size-time", (guint64) 500000000,
                "leaky", 2,
                NULL);

  /* Rotation correction */
  gint video_direction = gnalk_camera_rotation_to_video_direction (rotation);
  g_object_set (flip, "video-direction", video_direction, NULL);

  /* Normalize framerate (matches stream camera_bin) */
  GstCaps *rate_c = gst_caps_from_string ("video/x-raw,framerate=15/1");
  g_object_set (ratecaps, "caps", rate_c, NULL);
  gst_caps_unref (rate_c);

  /* Tee settings - allow branches to be unlinked */
  g_object_set (tee, "allow-not-linked", TRUE, NULL);

  /* Preview queue */
  g_object_set (prev_queue, "max-size-buffers", 2, "leaky", 2, NULL);

  /* Scale with borders to preserve aspect ratio (matches stream preview) */
  g_object_set (scale, "add-borders", TRUE, NULL);

  /* Scale caps (matches stream preview) */
  GstCaps *scale_c = gst_caps_from_string ("video/x-raw,width=[1,320],height=[1,240],pixel-aspect-ratio=1/1");
  g_object_set (scalecaps, "caps", scale_c, NULL);
  gst_caps_unref (scale_c);

  /* Crop to match in-call preview framing */
  g_object_set (crop, "left", config.preview_crop, "right", config.preview_crop, NULL);

  /* Format conversion to RGB */
  GstCaps *fmt_c = gst_caps_from_string ("video/x-raw,format=RGB");
  g_object_set (fmtcaps, "caps", fmt_c, NULL);
  gst_caps_unref (fmt_c);

  /* Appsink settings */
  g_object_set (appsink,
                "emit-signals", TRUE,
                "max-buffers", 1,
                "drop", TRUE,
                "sync", FALSE,
                NULL);

  gst_bin_add_many (GST_BIN (pipeline), src, srccaps, queue, convert, flip, rate, ratecaps,
                    tee, prev_queue, scale, scalecaps, crop, convert2, fmtcaps, appsink, NULL);

  /* Link source chain up to tee */
  if (!gst_element_link_many (src, srccaps, queue, convert, flip, rate, ratecaps, tee, NULL))
    {
      g_warning ("CameraPreview: failed to link source chain");
      gst_object_unref (pipeline);
      return;
    }

  /* Link preview branch from tee */
  GstPad *tee_src = gst_element_request_pad_simple (tee, "src_%u");
  GstPad *prev_sink = gst_element_get_static_pad (prev_queue, "sink");

  if (!tee_src || !prev_sink)
    {
      g_warning ("CameraPreview: failed to get tee/preview pads");
      if (tee_src) gst_object_unref (tee_src);
      if (prev_sink) gst_object_unref (prev_sink);
      gst_object_unref (pipeline);
      return;
    }

  if (gst_pad_link (tee_src, prev_sink) != GST_PAD_LINK_OK)
    {
      g_warning ("CameraPreview: failed to link tee to preview");
      gst_object_unref (tee_src);
      gst_object_unref (prev_sink);
      gst_object_unref (pipeline);
      return;
    }
  gst_object_unref (prev_sink);

  /* Link rest of preview branch */
  if (!gst_element_link_many (prev_queue, scale, scalecaps, crop, convert2, fmtcaps, appsink, NULL))
    {
      g_warning ("CameraPreview: failed to link preview branch");
      gst_object_unref (tee_src);
      gst_object_unref (pipeline);
      return;
    }

  self->pipeline = pipeline;
  self->appsink = appsink;
  self->tee = tee;
  self->tee_preview_pad = tee_src;
  self->sample_handler_id = g_signal_connect (appsink, "new-sample",
                                               G_CALLBACK (on_new_sample), self);
}

static void
destroy_pipeline (GnalkCameraPreview *self)
{
  if (self->pipeline)
    {
      if (self->sample_handler_id && self->appsink)
        {
          g_signal_handler_disconnect (self->appsink, self->sample_handler_id);
          self->sample_handler_id = 0;
        }

      /* Release tee pads */
      if (self->tee_preview_pad && self->tee)
        {
          gst_element_release_request_pad (self->tee, self->tee_preview_pad);
          gst_object_unref (self->tee_preview_pad);
          self->tee_preview_pad = NULL;
        }
      if (self->tee_record_pad && self->tee)
        {
          gst_element_release_request_pad (self->tee, self->tee_record_pad);
          gst_object_unref (self->tee_record_pad);
          self->tee_record_pad = NULL;
        }

      gst_element_set_state (self->pipeline, GST_STATE_NULL);
      /* Wait for pipeline to fully stop so v4l2src releases the device */
      gst_element_get_state (self->pipeline, NULL, NULL, GST_SECOND);
      gst_object_unref (self->pipeline);
      self->pipeline = NULL;
      self->appsink = NULL;
      self->tee = NULL;
    }
}

#endif /* HAVE_GSTREAMER */

static void
on_camera_quality_changed (GSettings   *settings G_GNUC_UNUSED,
                           const gchar *key,
                           gpointer     user_data)
{
  GnalkCameraPreview *self = GNALK_CAMERA_PREVIEW (user_data);

  if (g_strcmp0 (key, "camera-quality") != 0)
    return;

#ifdef HAVE_GSTREAMER
  /* For generic cameras (laptops), quality doesn't affect the preview pipeline
   * since we don't apply source caps and crop is always 0. Skip restart. */
  if (self->position == GNALK_CAMERA_PREVIEW_GENERIC)
    return;

  /* Restart pipeline if running to apply new quality setting (mobile only) */
  if (self->pipeline)
    {
      g_message ("CameraPreview: quality changed, restarting pipeline");
      gnalk_camera_preview_stop (self);
      gnalk_camera_preview_start (self);
    }
#endif
}

static void
gnalk_camera_preview_dispose (GObject *object)
{
  GnalkCameraPreview *self = GNALK_CAMERA_PREVIEW (object);

#ifdef HAVE_GSTREAMER
  destroy_pipeline (self);

  g_mutex_lock (&self->frame_mutex);
  g_clear_pointer (&self->last_frame, g_bytes_unref);
  g_mutex_unlock (&self->frame_mutex);

  g_clear_pointer (&self->video_output_path, g_free);
#endif

  if (self->settings_handler_id && self->settings)
    {
      g_signal_handler_disconnect (self->settings, self->settings_handler_id);
      self->settings_handler_id = 0;
    }
  g_clear_object (&self->settings);

  g_clear_pointer (&self->picture, gtk_widget_unparent);

  G_OBJECT_CLASS (gnalk_camera_preview_parent_class)->dispose (object);
}

static void
gnalk_camera_preview_finalize (GObject *object)
{
  GnalkCameraPreview *self = GNALK_CAMERA_PREVIEW (object);

#ifdef HAVE_GSTREAMER
  g_mutex_clear (&self->frame_mutex);
  g_mutex_clear (&self->eos_mutex);
  g_cond_clear (&self->eos_cond);
#endif

  G_OBJECT_CLASS (gnalk_camera_preview_parent_class)->finalize (object);
}

static void
gnalk_camera_preview_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  GnalkCameraPreview *self = GNALK_CAMERA_PREVIEW (object);

  switch (prop_id)
    {
    case PROP_POSITION:
      g_value_set_int (value, self->position);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gnalk_camera_preview_set_property (GObject      *object,
                                     guint         prop_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
  GnalkCameraPreview *self = GNALK_CAMERA_PREVIEW (object);

  switch (prop_id)
    {
    case PROP_POSITION:
      gnalk_camera_preview_set_position (self, g_value_get_int (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gnalk_camera_preview_class_init (GnalkCameraPreviewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnalk_camera_preview_dispose;
  object_class->finalize = gnalk_camera_preview_finalize;
  object_class->get_property = gnalk_camera_preview_get_property;
  object_class->set_property = gnalk_camera_preview_set_property;

  properties[PROP_POSITION] =
    g_param_spec_int ("position", NULL, NULL,
                      GNALK_CAMERA_PREVIEW_FRONT,
                      GNALK_CAMERA_PREVIEW_GENERIC,
                      GNALK_CAMERA_PREVIEW_FRONT,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnalk_camera_preview_init (GnalkCameraPreview *self)
{
  self->position = GNALK_CAMERA_PREVIEW_FRONT;

#ifdef HAVE_GSTREAMER
  g_mutex_init (&self->frame_mutex);
  g_mutex_init (&self->eos_mutex);
  g_cond_init (&self->eos_cond);
#endif

  self->picture = gtk_picture_new ();
  gtk_picture_set_content_fit (GTK_PICTURE (self->picture), GTK_CONTENT_FIT_COVER);
  gtk_widget_set_parent (self->picture, GTK_WIDGET (self));

  /* Listen for camera quality changes for live preview updates */
  self->settings = g_settings_new ("net.skilpa.gnalk");
  self->settings_handler_id = g_signal_connect (self->settings, "changed",
                                                 G_CALLBACK (on_camera_quality_changed), self);
}

GnalkCameraPreview *
gnalk_camera_preview_new (void)
{
  return g_object_new (GNALK_TYPE_CAMERA_PREVIEW, NULL);
}

void
gnalk_camera_preview_set_position (GnalkCameraPreview         *self,
                                     GnalkCameraPreviewPosition  position)
{
  g_return_if_fail (GNALK_IS_CAMERA_PREVIEW (self));

  if (self->position == position)
    return;

  gboolean was_running = gnalk_camera_preview_is_running (self);
  if (was_running)
    gnalk_camera_preview_stop (self);

  self->position = position;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_POSITION]);

  if (was_running)
    gnalk_camera_preview_start (self);
}

GnalkCameraPreviewPosition
gnalk_camera_preview_get_position (GnalkCameraPreview *self)
{
  g_return_val_if_fail (GNALK_IS_CAMERA_PREVIEW (self), GNALK_CAMERA_PREVIEW_FRONT);
  return self->position;
}

void
gnalk_camera_preview_start (GnalkCameraPreview *self)
{
  g_return_if_fail (GNALK_IS_CAMERA_PREVIEW (self));

#ifdef HAVE_GSTREAMER
  if (self->pipeline)
    return;

  create_pipeline (self);

  if (self->pipeline)
    gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
#endif
}

void
gnalk_camera_preview_stop (GnalkCameraPreview *self)
{
  g_return_if_fail (GNALK_IS_CAMERA_PREVIEW (self));

#ifdef HAVE_GSTREAMER
  destroy_pipeline (self);
#endif

  gtk_picture_set_paintable (GTK_PICTURE (self->picture), NULL);
}

gboolean
gnalk_camera_preview_is_running (GnalkCameraPreview *self)
{
  g_return_val_if_fail (GNALK_IS_CAMERA_PREVIEW (self), FALSE);

#ifdef HAVE_GSTREAMER
  return self->pipeline != NULL;
#else
  return FALSE;
#endif
}

#ifdef HAVE_GSTREAMER

/* Helper function to encode RGB data to JPEG using GdkPixbuf */
static GBytes *
encode_rgb_to_jpeg (const guchar *data,
                    int           width,
                    int           height,
                    int           quality)
{
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data (
      data,
      GDK_COLORSPACE_RGB,
      FALSE,  /* no alpha */
      8,      /* bits per sample */
      width,
      height,
      width * 3,  /* row stride */
      NULL, NULL);

  if (!pixbuf)
    return NULL;

  gchar *buffer = NULL;
  gsize buffer_size = 0;
  GError *error = NULL;

  g_autofree gchar *quality_str = g_strdup_printf ("%d", quality);

  gboolean saved = gdk_pixbuf_save_to_buffer (
      pixbuf,
      &buffer,
      &buffer_size,
      "jpeg",
      &error,
      "quality", quality_str,
      NULL);

  g_object_unref (pixbuf);

  if (!saved)
    {
      g_warning ("CameraPreview: Failed to encode JPEG: %s",
                 error ? error->message : "unknown error");
      g_clear_error (&error);
      return NULL;
    }

  return g_bytes_new_take (buffer, buffer_size);
}

#endif /* HAVE_GSTREAMER */

GBytes *
gnalk_camera_preview_capture_photo (GnalkCameraPreview *self)
{
  g_return_val_if_fail (GNALK_IS_CAMERA_PREVIEW (self), NULL);

#ifdef HAVE_GSTREAMER
  if (!self->pipeline)
    {
      g_warning ("CameraPreview: Cannot capture photo - preview not running");
      return NULL;
    }

  /* Get the last frame data */
  g_mutex_lock (&self->frame_mutex);

  if (!self->last_frame)
    {
      g_mutex_unlock (&self->frame_mutex);
      g_warning ("CameraPreview: No frame available for capture");
      return NULL;
    }

  /* Make a copy of the frame data while holding the lock */
  gsize frame_size;
  const guchar *frame_data = g_bytes_get_data (self->last_frame, &frame_size);
  g_autofree guchar *data_copy = g_memdup2 (frame_data, frame_size);
  int width = self->last_frame_width;
  int height = self->last_frame_height;

  g_mutex_unlock (&self->frame_mutex);

  /* Encode to JPEG (outside of lock) */
  GBytes *jpeg = encode_rgb_to_jpeg (data_copy, width, height, 90);

  if (jpeg)
    {
      gsize jpeg_size;
      g_bytes_get_data (jpeg, &jpeg_size);
      g_message ("CameraPreview: Captured photo %dx%d, JPEG size=%zu bytes",
                 width, height, jpeg_size);
    }

  return jpeg;
#else
  g_warning ("CameraPreview: GStreamer not available");
  return NULL;
#endif
}


gboolean
gnalk_camera_preview_start_video_recording (GnalkCameraPreview *self,
                                              const char          *output_path)
{
  g_return_val_if_fail (GNALK_IS_CAMERA_PREVIEW (self), FALSE);
  g_return_val_if_fail (output_path != NULL, FALSE);

#ifdef HAVE_GSTREAMER
  if (!self->pipeline || !self->tee)
    {
      g_warning ("CameraPreview: Cannot start video recording - preview not running");
      return FALSE;
    }

  if (self->recording_video)
    {
      g_warning ("CameraPreview: Already recording video");
      return FALSE;
    }

  g_message ("CameraPreview: Starting video recording to %s", output_path);

  g_clear_pointer (&self->video_output_path, g_free);
  self->video_output_path = g_strdup (output_path);

  /* Create recording branch elements:
   * tee -> rec_queue -> rec_convert -> rec_scale -> rec_caps -> rec_encoder -> muxer -> filesink
   *        (audio_src -> audio_queue -> audio_convert -> audio_resample -> audio_enc) -> muxer */

  self->rec_queue = gst_element_factory_make ("queue", "rec_queue");
  self->rec_convert = gst_element_factory_make ("videoconvert", "rec_convert");
  self->rec_scale = gst_element_factory_make ("videoscale", "rec_scale");
  self->rec_caps = gst_element_factory_make ("capsfilter", "rec_caps");
  self->rec_encoder = gst_element_factory_make ("vp8enc", "rec_encoder");
  self->muxer = gst_element_factory_make ("webmmux", "rec_muxer");
  self->filesink = gst_element_factory_make ("filesink", "rec_filesink");

  /* Audio elements */
  self->audio_src = gst_element_factory_make ("pulsesrc", "rec_audiosrc");
  self->audio_queue = gst_element_factory_make ("queue", "rec_audioqueue");
  self->audio_convert = gst_element_factory_make ("audioconvert", "rec_audioconvert");
  self->audio_resample = gst_element_factory_make ("audioresample", "rec_audioresample");
  self->audio_enc = gst_element_factory_make ("opusenc", "rec_opusenc");

  if (!self->rec_queue || !self->rec_convert || !self->rec_scale ||
      !self->rec_caps || !self->rec_encoder || !self->muxer || !self->filesink ||
      !self->audio_src || !self->audio_queue || !self->audio_convert ||
      !self->audio_resample || !self->audio_enc)
    {
      g_warning ("CameraPreview: Failed to create recording elements");
      /* Clean up any created elements */
      g_clear_pointer (&self->rec_queue, gst_object_unref);
      g_clear_pointer (&self->rec_convert, gst_object_unref);
      g_clear_pointer (&self->rec_scale, gst_object_unref);
      g_clear_pointer (&self->rec_caps, gst_object_unref);
      g_clear_pointer (&self->rec_encoder, gst_object_unref);
      g_clear_pointer (&self->muxer, gst_object_unref);
      g_clear_pointer (&self->filesink, gst_object_unref);
      g_clear_pointer (&self->audio_src, gst_object_unref);
      g_clear_pointer (&self->audio_queue, gst_object_unref);
      g_clear_pointer (&self->audio_convert, gst_object_unref);
      g_clear_pointer (&self->audio_resample, gst_object_unref);
      g_clear_pointer (&self->audio_enc, gst_object_unref);
      return FALSE;
    }

  /* Configure recording queue */
  g_object_set (self->rec_queue,
                "max-size-buffers", 100,
                "max-size-time", (guint64) GST_SECOND,
                "leaky", 0,  /* Don't drop frames for recording */
                NULL);

  /* Configure video encoding: 640x480 I420 at 15fps */
  GstCaps *rec_c = gst_caps_from_string ("video/x-raw,format=I420,width=640,height=480,framerate=15/1");
  g_object_set (self->rec_caps, "caps", rec_c, NULL);
  gst_caps_unref (rec_c);

  /* VP8 encoder settings (from media-pipeline.c) */
  g_object_set (self->rec_encoder,
                "deadline", 1,           /* Ultra-fast encoding */
                "cpu-used", 4,           /* Conservative for mobile */
                "target-bitrate", 500000, /* 500 kbps */
                "keyframe-max-dist", 30,  /* Keyframe every 30 frames */
                "end-usage", 1,           /* VBR mode */
                NULL);

  /* Configure muxer for streaming (better for seeking) */
  /* streamable=FALSE allows muxer to rewrite header with correct duration at end */
  g_object_set (self->muxer, "streamable", FALSE, NULL);

  /* Configure filesink */
  g_object_set (self->filesink, "location", output_path, NULL);

  /* Configure audio source.
   * do-timestamp=TRUE uses pipeline clock for timestamps (needed for muxer).
   * provide-clock=FALSE avoids clock conflicts with video source. */
  g_object_set (self->audio_src,
                "buffer-time", (gint64) 200000,
                "latency-time", (gint64) 20000,
                "do-timestamp", TRUE,
                "provide-clock", FALSE,
                NULL);

  /* Configure audio queue - DON'T drop buffers (leaky=0 is critical!) */
  g_object_set (self->audio_queue,
                "max-size-buffers", 0,
                "max-size-bytes", 0,
                "max-size-time", (guint64) 500000000,  /* 500ms buffer */
                "leaky", 0,
                NULL);

  /* Add all recording elements to pipeline */
  gst_bin_add_many (GST_BIN (self->pipeline),
                    self->rec_queue, self->rec_convert, self->rec_scale,
                    self->rec_caps, self->rec_encoder, self->muxer, self->filesink,
                    self->audio_src, self->audio_queue, self->audio_convert,
                    self->audio_resample, self->audio_enc, NULL);

  /* Link video recording branch */
  if (!gst_element_link_many (self->rec_queue, self->rec_convert, self->rec_scale,
                              self->rec_caps, self->rec_encoder, NULL))
    {
      g_warning ("CameraPreview: Failed to link video recording elements");
      goto cleanup_error;
    }

  /* Link video encoder to muxer */
  GstPad *video_src = gst_element_get_static_pad (self->rec_encoder, "src");
  GstPad *mux_video_sink = gst_element_request_pad_simple (self->muxer, "video_%u");
  if (!video_src || !mux_video_sink ||
      gst_pad_link (video_src, mux_video_sink) != GST_PAD_LINK_OK)
    {
      g_warning ("CameraPreview: Failed to link video encoder to muxer");
      if (video_src) gst_object_unref (video_src);
      if (mux_video_sink) gst_object_unref (mux_video_sink);
      goto cleanup_error;
    }
  gst_object_unref (video_src);
  gst_object_unref (mux_video_sink);

  /* Link audio branch */
  if (!gst_element_link_many (self->audio_src, self->audio_queue, self->audio_convert,
                              self->audio_resample, self->audio_enc, NULL))
    {
      g_warning ("CameraPreview: Failed to link audio elements");
      goto cleanup_error;
    }

  /* Link audio encoder to muxer */
  GstPad *audio_src_pad = gst_element_get_static_pad (self->audio_enc, "src");
  GstPad *mux_audio_sink = gst_element_request_pad_simple (self->muxer, "audio_%u");
  if (!audio_src_pad || !mux_audio_sink ||
      gst_pad_link (audio_src_pad, mux_audio_sink) != GST_PAD_LINK_OK)
    {
      g_warning ("CameraPreview: Failed to link audio encoder to muxer");
      if (audio_src_pad) gst_object_unref (audio_src_pad);
      if (mux_audio_sink) gst_object_unref (mux_audio_sink);
      goto cleanup_error;
    }
  gst_object_unref (audio_src_pad);
  gst_object_unref (mux_audio_sink);

  /* Link muxer to filesink */
  if (!gst_element_link (self->muxer, self->filesink))
    {
      g_warning ("CameraPreview: Failed to link muxer to filesink");
      goto cleanup_error;
    }

  /* Request new tee pad for recording branch */
  self->tee_record_pad = gst_element_request_pad_simple (self->tee, "src_%u");
  if (!self->tee_record_pad)
    {
      g_warning ("CameraPreview: Failed to get tee recording pad");
      goto cleanup_error;
    }

  /* Sync states before linking to running pipeline */
  gst_element_sync_state_with_parent (self->rec_queue);
  gst_element_sync_state_with_parent (self->rec_convert);
  gst_element_sync_state_with_parent (self->rec_scale);
  gst_element_sync_state_with_parent (self->rec_caps);
  gst_element_sync_state_with_parent (self->rec_encoder);
  gst_element_sync_state_with_parent (self->audio_src);
  gst_element_sync_state_with_parent (self->audio_queue);
  gst_element_sync_state_with_parent (self->audio_convert);
  gst_element_sync_state_with_parent (self->audio_resample);
  gst_element_sync_state_with_parent (self->audio_enc);
  gst_element_sync_state_with_parent (self->muxer);
  gst_element_sync_state_with_parent (self->filesink);

  /* Link tee to recording queue */
  GstPad *rec_sink = gst_element_get_static_pad (self->rec_queue, "sink");
  if (!rec_sink || gst_pad_link (self->tee_record_pad, rec_sink) != GST_PAD_LINK_OK)
    {
      g_warning ("CameraPreview: Failed to link tee to recording queue");
      if (rec_sink) gst_object_unref (rec_sink);
      goto cleanup_error;
    }

  /* NOTE: Not using pad offset for now - debugging data flow issue.
   * TODO: Re-enable offset once basic recording works. */
  g_message ("CameraPreview: Recording branch linked (no timestamp offset)");

  gst_object_unref (rec_sink);

  self->recording_video = TRUE;

  g_message ("CameraPreview: Video recording started");
  return TRUE;

cleanup_error:
  /* Remove and unref all recording elements */
  if (self->tee_record_pad)
    {
      gst_element_release_request_pad (self->tee, self->tee_record_pad);
      gst_object_unref (self->tee_record_pad);
      self->tee_record_pad = NULL;
    }
  gst_bin_remove_many (GST_BIN (self->pipeline),
                       self->rec_queue, self->rec_convert, self->rec_scale,
                       self->rec_caps, self->rec_encoder,
                       self->muxer, self->filesink,
                       self->audio_src, self->audio_queue, self->audio_convert,
                       self->audio_resample, self->audio_enc, NULL);
  self->rec_queue = NULL;
  self->rec_convert = NULL;
  self->rec_scale = NULL;
  self->rec_caps = NULL;
  self->rec_encoder = NULL;
  self->muxer = NULL;
  self->filesink = NULL;
  self->audio_src = NULL;
  self->audio_queue = NULL;
  self->audio_convert = NULL;
  self->audio_resample = NULL;
  self->audio_enc = NULL;
  return FALSE;
#else
  (void) output_path;
  g_warning ("CameraPreview: GStreamer not available");
  return FALSE;
#endif
}

GBytes *
gnalk_camera_preview_stop_video_recording (GnalkCameraPreview *self)
{
  g_return_val_if_fail (GNALK_IS_CAMERA_PREVIEW (self), NULL);

#ifdef HAVE_GSTREAMER
  if (!self->recording_video)
    {
      g_warning ("CameraPreview: Not recording video");
      return NULL;
    }

  g_message ("CameraPreview: Stopping video recording");

  /* Step 1: Block the tee pad to stop new data flowing to recording branch */
  GstPad *queue_sink_pad = gst_element_get_static_pad (self->rec_queue, "sink");

  g_message ("CameraPreview: Stopping recording - sending EOS...");

  /* Step 2: Send EOS to video recording branch (after the tee, into the queue) */
  if (queue_sink_pad)
    {
      gst_pad_send_event (queue_sink_pad, gst_event_new_eos ());
    }

  /* Send EOS to audio branch */
  gst_element_send_event (self->audio_src, gst_event_new_eos ());

  /* Step 3: Wait for EOS to reach filesink by polling its state.
   * When muxer receives EOS on all pads, it finalizes and forwards EOS.
   * We poll until filesink has received and processed the EOS. */
  g_message ("CameraPreview: Waiting for muxer to finalize...");

  GstPad *filesink_pad = gst_element_get_static_pad (self->filesink, "sink");
  if (filesink_pad)
    {
      /* Wait up to 10 seconds for EOS to arrive */
      gint64 end_time = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
      gboolean eos_received = FALSE;

      while (g_get_monotonic_time () < end_time && !eos_received)
        {
          /* Check if pad is EOS */
          GstEvent *event = gst_pad_get_sticky_event (filesink_pad, GST_EVENT_EOS, 0);
          if (event)
            {
              eos_received = TRUE;
              gst_event_unref (event);
              g_message ("CameraPreview: EOS received at filesink");
            }
          else
            {
              g_usleep (50000);  /* 50ms */
            }
        }

      if (!eos_received)
        g_warning ("CameraPreview: Timeout waiting for EOS at filesink");

      gst_object_unref (filesink_pad);
    }

  /* Step 4: Unlink recording branch from tee */
  if (self->tee_record_pad && queue_sink_pad)
    {
      gst_pad_unlink (self->tee_record_pad, queue_sink_pad);
    }
  if (queue_sink_pad)
    gst_object_unref (queue_sink_pad);

  /* Release tee recording pad */
  if (self->tee_record_pad && self->tee)
    {
      gst_element_release_request_pad (self->tee, self->tee_record_pad);
      gst_object_unref (self->tee_record_pad);
      self->tee_record_pad = NULL;
    }

  /* Step 5: Set all recording elements to NULL state while still in pipeline.
   * This properly flushes any remaining data. */
  gst_element_set_state (self->filesink, GST_STATE_NULL);
  gst_element_set_state (self->muxer, GST_STATE_NULL);
  gst_element_set_state (self->rec_encoder, GST_STATE_NULL);
  gst_element_set_state (self->audio_enc, GST_STATE_NULL);
  gst_element_set_state (self->audio_resample, GST_STATE_NULL);
  gst_element_set_state (self->audio_convert, GST_STATE_NULL);
  gst_element_set_state (self->audio_queue, GST_STATE_NULL);
  gst_element_set_state (self->audio_src, GST_STATE_NULL);
  gst_element_set_state (self->rec_caps, GST_STATE_NULL);
  gst_element_set_state (self->rec_scale, GST_STATE_NULL);
  gst_element_set_state (self->rec_convert, GST_STATE_NULL);
  gst_element_set_state (self->rec_queue, GST_STATE_NULL);

  /* Step 6: Remove all recording elements from pipeline */
  gst_bin_remove_many (GST_BIN (self->pipeline),
                       self->rec_queue, self->rec_convert, self->rec_scale,
                       self->rec_caps, self->rec_encoder,
                       self->muxer, self->filesink,
                       self->audio_src, self->audio_queue, self->audio_convert,
                       self->audio_resample, self->audio_enc, NULL);

  g_message ("CameraPreview: Recording finalization complete");

  /* Clear element pointers (elements were already removed and set to NULL above) */
  self->rec_queue = NULL;
  self->rec_convert = NULL;
  self->rec_scale = NULL;
  self->rec_caps = NULL;
  self->rec_encoder = NULL;
  self->muxer = NULL;
  self->filesink = NULL;
  self->audio_src = NULL;
  self->audio_queue = NULL;
  self->audio_convert = NULL;
  self->audio_resample = NULL;
  self->audio_enc = NULL;

  self->recording_video = FALSE;

  /* Read the recorded file */
  GBytes *result = NULL;
  if (self->video_output_path)
    {
      g_autofree char *contents = NULL;
      gsize length = 0;
      GError *error = NULL;

      if (g_file_get_contents (self->video_output_path, &contents, &length, &error))
        {
          result = g_bytes_new_take (g_steal_pointer (&contents), length);
          g_message ("CameraPreview: Video recording complete, size=%zu bytes", length);
        }
      else
        {
          g_warning ("CameraPreview: Failed to read recorded video: %s", error->message);
          g_error_free (error);
        }

      /* Clean up temp file */
      g_unlink (self->video_output_path);
    }

  g_clear_pointer (&self->video_output_path, g_free);

  return result;
#else
  return NULL;
#endif
}

gboolean
gnalk_camera_preview_is_recording_video (GnalkCameraPreview *self)
{
  g_return_val_if_fail (GNALK_IS_CAMERA_PREVIEW (self), FALSE);

#ifdef HAVE_GSTREAMER
  return self->recording_video;
#else
  return FALSE;
#endif
}
