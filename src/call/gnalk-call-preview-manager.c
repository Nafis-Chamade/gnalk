/* gnalk-call-preview-manager.c
 *
 * Camera preview management for the call window.
 * Handles GStreamer camera detection, preview pipeline creation,
 * and video frame processing for the local camera preview.
 */

#include "gnalk-call-window-private.h"

#ifdef HAVE_GSTREAMER

/* Find camera by location using GstDeviceMonitor.
 * Searches by api.libcamera.location property (libcamera <0.6) or
 * by device name containing "Front"/"Back" (libcamera >=0.6).
 * Also returns the rotation angle from api.libcamera.rotation property. */
GstElement *
gnalk_call_preview_find_camera_source_by_position (GnalkCameraPosition position, gint *out_rotation)
{
  GstDeviceMonitor *monitor;
  GstDevice *found_device = NULL;
  GstElement *source = NULL;
  const char *target_location = (position == GNALK_CAMERA_FRONT) ? "front" : "back";
  const char *target_name = (position == GNALK_CAMERA_FRONT) ? "Front" : "Back";

  if (out_rotation)
    *out_rotation = 0;

  monitor = gst_device_monitor_new ();
  gst_device_monitor_add_filter (monitor, "Video/Source", NULL);

  if (!gst_device_monitor_start (monitor))
    {
      g_warning ("Preview: Failed to start device monitor");
      gst_object_unref (monitor);
      return NULL;
    }

  GList *devices = gst_device_monitor_get_devices (monitor);
  for (GList *l = devices; l != NULL; l = l->next)
    {
      GstDevice *device = GST_DEVICE (l->data);
      GstStructure *props = gst_device_get_properties (device);
      gchar *device_name = gst_device_get_display_name (device);
      gboolean match = FALSE;

      /* Try api.libcamera.location property first (libcamera <0.6) */
      if (props)
        {
          const gchar *location = gst_structure_get_string (props, "api.libcamera.location");
          if (location && g_strcmp0 (location, target_location) == 0)
            {
              match = TRUE;
              if (out_rotation)
                {
                  gint rotation = 0;
                  if (gst_structure_get_int (props, "api.libcamera.rotation", &rotation))
                    *out_rotation = rotation;
                  else
                    {
                      const gchar *rot_str = gst_structure_get_string (props, "api.libcamera.rotation");
                      if (rot_str)
                        *out_rotation = atoi (rot_str);
                    }
                }
            }
          gst_structure_free (props);
        }

      /* Fall back to device name matching (libcamera >=0.6) */
      if (!match && device_name && strstr (device_name, target_name))
        {
          match = TRUE;
          /* Default rotations for OnePlus 6: front=90, back=270 */
          if (out_rotation)
            *out_rotation = (position == GNALK_CAMERA_FRONT) ? 90 : 270;
        }

      if (match)
        {
          found_device = gst_object_ref (device);
          g_message ("Preview: Found %s camera '%s' (rotation=%d)", target_location,
                     device_name ? device_name : "unknown",
                     out_rotation ? *out_rotation : 0);
          g_free (device_name);
          break;
        }
      g_free (device_name);
    }
  g_list_free_full (devices, gst_object_unref);
  gst_device_monitor_stop (monitor);
  gst_object_unref (monitor);

  if (found_device)
    {
      /* Use libcamerasrc directly instead of pipewiresrc (via gst_device_create_element)
       * as pipewiresrc has "Resource busy" issues with libcamera 0.6 */
      source = gst_element_factory_make ("libcamerasrc", "videosrc");
      if (source)
        {
          /* OnePlus 6 camera paths - front is i2c-bus@0, back is i2c-bus@1 */
          const char *camera_path = (position == GNALK_CAMERA_FRONT)
            ? "/base/soc@0/cci@ac4a000/i2c-bus@0/camera@10"
            : "/base/soc@0/cci@ac4a000/i2c-bus@1/camera@10";
          g_object_set (source, "camera-name", camera_path, NULL);
          g_message ("Preview: Created libcamerasrc for %s camera (%s)", target_location, camera_path);
        }
      else
        g_warning ("Preview: Failed to create libcamerasrc element");
      gst_object_unref (found_device);
    }
  else
    {
      g_warning ("Preview: No camera found with location=%s", target_location);
    }

  return source;
}

/* Preview update data for main thread callback */
typedef struct {
  GtkWidget *picture;  /* Use GtkWidget* for weak pointer compatibility */
  GdkTexture *texture;
} PreviewUpdate;

static gboolean
update_preview_in_main_thread (gpointer user_data)
{
  PreviewUpdate *update = user_data;

  /* Check if picture widget is still valid (weak ref cleared on destroy) */
  if (update->picture)
    {
      gtk_picture_set_paintable (GTK_PICTURE (update->picture), GDK_PAINTABLE (update->texture));
      g_object_remove_weak_pointer (G_OBJECT (update->picture), (gpointer *)&update->picture);
    }

  g_object_unref (update->texture);
  g_free (update);
  return G_SOURCE_REMOVE;
}

/* Update for fullscreen preview (before call starts) */
static GstFlowReturn
on_preview_sample (GstAppSink *appsink,
                   gpointer    user_data)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (user_data);
  GstSample *sample;
  GstBuffer *buffer;
  GstMapInfo map;
  GstCaps *caps;
  GstVideoInfo video_info;

  sample = gst_app_sink_pull_sample (appsink);
  if (!sample)
    return GST_FLOW_OK;

  buffer = gst_sample_get_buffer (sample);
  caps = gst_sample_get_caps (sample);

  if (!gst_video_info_from_caps (&video_info, caps))
    {
      gst_sample_unref (sample);
      return GST_FLOW_OK;
    }

  if (gst_buffer_map (buffer, &map, GST_MAP_READ))
    {
      if (map.data && map.size > 0)
        {
          GBytes *bytes = g_bytes_new (map.data, map.size);
          if (bytes)
            {
              /* Use RGB format (3 bytes per pixel) */
              GdkTexture *texture = gdk_memory_texture_new (
                video_info.width,
                video_info.height,
                GDK_MEMORY_R8G8B8,
                bytes,
                video_info.width * 3);
              g_bytes_unref (bytes);

              if (texture)
                {
                  PreviewUpdate *update = g_new (PreviewUpdate, 1);
                  update->picture = self->local_video_fullscreen;
                  update->texture = texture;
                  /* Add weak reference to detect if widget is destroyed before callback runs */
                  g_object_add_weak_pointer (G_OBJECT (update->picture), (gpointer *)&update->picture);
                  g_idle_add (update_preview_in_main_thread, update);
                }
            }
        }
      gst_buffer_unmap (buffer, &map);
    }

  gst_sample_unref (sample);
  return GST_FLOW_OK;
}

void
gnalk_call_preview_start_local_preview (GnalkCallWindow *self)
{
  if (self->preview_pipeline)
    return;

  GstElement *pipeline = gst_pipeline_new ("preview-pipeline");
  GError *error = NULL;
  GstElement *bin = NULL;
  const char *src_name = NULL;

  /* Find camera by current position using GstDeviceMonitor.
   * This creates the source element directly from the device, which is the
   * proper GStreamer way to use discovered devices. */
  gint camera_rotation = 0;
  GstElement *camera_src = gnalk_call_preview_find_camera_source_by_position (self->camera_position, &camera_rotation);

  if (camera_src)
    {
      /* Get shared camera config based on quality setting */
      GnalkCameraQuality quality = gnalk_camera_quality_from_settings ();
      GnalkCameraConfig config = gnalk_camera_get_config (quality, self->camera_position);

      g_message ("Preview: using quality=%d, resolution=%dx%d, crop=%d",
                 quality, config.src_width, config.src_height, config.preview_crop);

      /* Pipeline structure (matches preferences preview and in-call):
       * source -> srccaps -> queue -> convert -> flip -> rate -> ratecaps -> scale -> scalecaps -> crop -> convert2 -> fmtcaps -> appsink */
      GstElement *srccaps = gst_element_factory_make ("capsfilter", "srccaps");
      GstElement *queue = gst_element_factory_make ("queue", NULL);
      GstElement *convert = gst_element_factory_make ("videoconvert", NULL);
      GstElement *flip = gst_element_factory_make ("videoflip", NULL);
      GstElement *rate = gst_element_factory_make ("videorate", NULL);
      GstElement *ratecaps = gst_element_factory_make ("capsfilter", NULL);
      GstElement *scale = gst_element_factory_make ("videoscale", NULL);
      GstElement *scalecaps = gst_element_factory_make ("capsfilter", NULL);
      GstElement *crop = gst_element_factory_make ("videocrop", NULL);
      GstElement *convert2 = gst_element_factory_make ("videoconvert", NULL);
      GstElement *fmtcaps = gst_element_factory_make ("capsfilter", NULL);
      GstElement *appsink = gst_element_factory_make ("appsink", "appsink");

      if (srccaps && queue && convert && flip && rate && ratecaps && scale &&
          scalecaps && crop && convert2 && fmtcaps && appsink)
        {
          /* Request specific resolution from camera */
          g_autofree gchar *src_caps_str = g_strdup_printf (
              "video/x-raw,width=%d,height=%d", config.src_width, config.src_height);
          GstCaps *src_c = gst_caps_from_string (src_caps_str);
          g_object_set (srccaps, "caps", src_c, NULL);
          gst_caps_unref (src_c);

          /* Queue with generous buffer to handle Software ISP timing variations */
          g_object_set (queue,
                        "max-size-buffers", 5,
                        "max-size-time", (guint64) 500000000,  /* 500ms */
                        "leaky", 2,  /* downstream - drop old frames */
                        NULL);

          /* Rotation: api.libcamera.rotation indicates sensor mounting angle.
           * We apply the inverse to get correct orientation.
           * video-direction: 0=identity, 1=90r(CW), 2=180, 3=90l(CCW) */
          gint video_direction = gnalk_camera_rotation_to_video_direction (camera_rotation);
          g_object_set (flip, "video-direction", video_direction, NULL);
          g_message ("Preview: Sensor rotation=%d, compensating with video-direction=%d",
                     camera_rotation, video_direction);

          /* Normalize framerate */
          GstCaps *rate_c = gst_caps_from_string ("video/x-raw,framerate=15/1");
          g_object_set (ratecaps, "caps", rate_c, NULL);
          gst_caps_unref (rate_c);

          /* Scale with borders to preserve aspect ratio */
          g_object_set (scale, "add-borders", TRUE, NULL);

          /* Scale caps */
          GstCaps *scale_c = gst_caps_from_string ("video/x-raw,width=[1,320],height=[1,240],pixel-aspect-ratio=1/1");
          g_object_set (scalecaps, "caps", scale_c, NULL);
          gst_caps_unref (scale_c);

          /* Crop to match in-call preview framing */
          g_object_set (crop, "left", config.preview_crop, "right", config.preview_crop, NULL);

          /* Output format - RGB for GTK */
          GstCaps *fmt_c = gst_caps_from_string ("video/x-raw,format=RGB");
          g_object_set (fmtcaps, "caps", fmt_c, NULL);
          gst_caps_unref (fmt_c);

          /* Appsink: drop frames if we can't keep up, emit signals for new samples */
          g_object_set (appsink,
                        "emit-signals", TRUE,
                        "max-buffers", 1,
                        "drop", TRUE,
                        "sync", FALSE,  /* Don't sync to clock - reduces latency */
                        NULL);

          bin = gst_bin_new ("camera-bin");
          gst_bin_add_many (GST_BIN (bin), camera_src, srccaps, queue, convert, flip, rate, ratecaps,
                            scale, scalecaps, crop, convert2, fmtcaps, appsink, NULL);

          if (gst_element_link_many (camera_src, srccaps, queue, convert, flip, rate, ratecaps,
                                     scale, scalecaps, crop, convert2, fmtcaps, appsink, NULL))
            {
              src_name = self->camera_position == GNALK_CAMERA_FRONT ?
                         "libcamerasrc (front)" : "libcamerasrc (back)";
              g_message ("Preview: Using %s camera via GstDevice",
                         self->camera_position == GNALK_CAMERA_FRONT ? "front" : "back");
            }
          else
            {
              g_message ("Preview: Failed to link camera pipeline");
              gst_object_unref (bin);
              bin = NULL;
            }
        }
      else
        {
          g_message ("Preview: Failed to create pipeline elements for camera");
          if (camera_src) gst_object_unref (camera_src);
          if (srccaps) gst_object_unref (srccaps);
          if (queue) gst_object_unref (queue);
          if (convert) gst_object_unref (convert);
          if (flip) gst_object_unref (flip);
          if (rate) gst_object_unref (rate);
          if (ratecaps) gst_object_unref (ratecaps);
          if (scale) gst_object_unref (scale);
          if (scalecaps) gst_object_unref (scalecaps);
          if (crop) gst_object_unref (crop);
          if (convert2) gst_object_unref (convert2);
          if (fmtcaps) gst_object_unref (fmtcaps);
          if (appsink) gst_object_unref (appsink);
        }
    }

  if (!bin)
    {
      /* Try libcamerasrc with default camera - use caps to limit resolution */
      const char *libcamera_pipeline =
        "libcamerasrc ! "
        "video/x-raw,width=(int)[1,1280],height=(int)[1,720] ! "
        "queue max-size-buffers=3 leaky=downstream ! "
        "videoconvert ! "
        "videoflip video-direction=counterclockwise ! "
        "videoscale ! "
        "video/x-raw,format=RGB,width=320,height=240 ! "
        "appsink name=appsink emit-signals=true max-buffers=1 drop=true";

      bin = gst_parse_bin_from_description (libcamera_pipeline, TRUE, &error);
      if (bin)
        {
          src_name = "libcamerasrc";
          g_message ("Preview: Using libcamerasrc with default camera");
        }
      else if (error)
        {
          g_message ("Preview: libcamerasrc failed: %s", error->message);
          g_clear_error (&error);
        }
    }

  /* Fall back to v4l2src (works on laptops/desktops) */
  if (!bin)
    {
      const char *v4l2_pipeline =
        "v4l2src ! "
        "videoconvert ! "
        "videoscale ! "
        "video/x-raw,format=RGB,width=320,height=240 ! "
        "appsink name=appsink emit-signals=true max-buffers=1 drop=true";

      bin = gst_parse_bin_from_description (v4l2_pipeline, TRUE, &error);
      if (bin)
        {
          src_name = "v4l2src";
          g_message ("Preview: Using v4l2src");
        }
      else
        {
          if (error)
            {
              g_message ("Preview: v4l2src failed: %s", error->message);
              g_clear_error (&error);
            }
        }
    }

  /* Last resort: videotestsrc for testing */
  if (!bin)
    {
      const char *test_pipeline =
        "videotestsrc is-live=true pattern=0 ! "
        "video/x-raw,format=RGB,width=320,height=240,framerate=15/1 ! "
        "appsink name=appsink emit-signals=true max-buffers=1 drop=true";

      bin = gst_parse_bin_from_description (test_pipeline, TRUE, &error);
      if (bin)
        {
          src_name = "videotestsrc";
          g_message ("Preview: Using videotestsrc (no camera available)");
        }
      else
        {
          if (error)
            {
              g_warning ("Preview: videotestsrc failed: %s", error->message);
              g_clear_error (&error);
            }
        }
    }

  if (!bin)
    {
      g_warning ("Preview: Failed to create any preview pipeline");
      gst_object_unref (pipeline);
      return;
    }

  gst_bin_add (GST_BIN (pipeline), bin);

  GstElement *appsink = gst_bin_get_by_name (GST_BIN (bin), "appsink");
  if (appsink)
    {
      g_signal_connect (appsink, "new-sample", G_CALLBACK (on_preview_sample), self);
      self->preview_appsink = appsink;
      gst_object_unref (appsink);
    }

  self->preview_pipeline = pipeline;

  GstStateChangeReturn ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    g_warning ("Preview: Failed to start pipeline with %s", src_name ? src_name : "unknown");
  else
    g_message ("Preview: Pipeline started with %s", src_name ? src_name : "unknown");
}

void
gnalk_call_preview_stop_local_preview (GnalkCallWindow *self)
{
  if (self->preview_pipeline)
    {
      g_message ("Preview: Stopping pipeline");

      /* Stop libcamerasrc separately first to drain queued requests.
       * Without this, libcamera crashes with assertion
       * "data->queuedRequests_.empty()" in stop(). */
      GstElement *camera_src = gst_bin_get_by_name (GST_BIN (self->preview_pipeline), "videosrc");
      if (camera_src)
        {
          g_message ("Preview: Stopping camera source first");
          gst_element_set_state (camera_src, GST_STATE_PAUSED);
          gst_element_get_state (camera_src, NULL, NULL, 500 * GST_MSECOND);
          gst_element_set_state (camera_src, GST_STATE_NULL);
          gst_element_get_state (camera_src, NULL, NULL, GST_SECOND);
          gst_object_unref (camera_src);
          g_message ("Preview: Camera source stopped");
        }

      /* Now stop the rest of the pipeline */
      gst_element_set_state (self->preview_pipeline, GST_STATE_READY);
      gst_element_get_state (self->preview_pipeline, NULL, NULL, 500 * GST_MSECOND);

      gst_element_set_state (self->preview_pipeline, GST_STATE_NULL);
      gst_element_get_state (self->preview_pipeline, NULL, NULL, GST_SECOND);

      g_message ("Preview: Pipeline stopped");

      gst_object_unref (self->preview_pipeline);
      self->preview_pipeline = NULL;
      self->preview_appsink = NULL;
    }
}

#endif /* HAVE_GSTREAMER */

/* Deferred WebRTC start callback - called after preview releases camera */
gboolean
gnalk_call_preview_on_start_webrtc_after_preview_stop (gpointer user_data)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (user_data);

  self->start_webrtc_timeout_id = 0;

  if (!self->webrtc)
    return G_SOURCE_REMOVE;

#ifdef HAVE_GSTREAMER
  /* Stop preview if still running to release camera for WebRTC */
  gnalk_call_preview_stop_local_preview (self);
#endif

  /* Set video enabled state BEFORE starting - this ensures correct status on data channel open */
  gnalk_webrtc_set_video_enabled (self->webrtc, self->video_enabled);

  g_message ("Starting WebRTC call after camera release");
  gnalk_webrtc_start_call (self->webrtc, self->room_token ? self->room_token : "");

  return G_SOURCE_REMOVE;
}
