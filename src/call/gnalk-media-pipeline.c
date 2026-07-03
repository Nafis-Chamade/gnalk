/* gnalk-media-pipeline.c
 *
 * GStreamer media pipeline construction and management.
 */

#include "gnalk-media-pipeline.h"

#include <errno.h>
#include <unistd.h>

#ifdef HAVE_GSTREAMER
#include <gst/video/video.h>
#include <gst/app/gstappsink.h>
#include <gst/rtp/rtp.h>
#endif

struct _GnalkMediaPipeline
{
  GObject parent_instance;

#ifdef HAVE_GSTREAMER
  GstElement *video_bin;           /* Video source bin */
  GstElement *video_encoder;       /* VP8 encoder for bitrate control */
  GstElement *local_preview;       /* Local preview appsink */
  GstElement *audio_volume;        /* Volume element for mute control */

  /* Camera switching elements */
  GstElement *input_selector;      /* Switches between camera and dummy source */
  GstElement *camera_bin;          /* Current camera source bin */
  GstElement *dummy_src;           /* Black frame source for switching */
  GstPad     *camera_selector_pad; /* input-selector pad for camera */
  GstPad     *dummy_selector_pad;  /* input-selector pad for dummy */
  GstElement *prev_crop;           /* Preview crop element (adjustable per camera) */

  /* Reference to parent pipeline's webrtcbin (not owned) */
  GstElement *webrtcbin;

  /* Screen share state */
  guint32     pipewire_node_id;
  int         pipewire_fd;
#endif

  GnalkCameraPosition camera_position;
  GnalkMediaPipelineType pipeline_type;
  gboolean    video_enabled;
  gboolean    audio_enabled;
};

G_DEFINE_FINAL_TYPE (GnalkMediaPipeline, gnalk_media_pipeline, G_TYPE_OBJECT)

enum {
  SIGNAL_LOCAL_VIDEO_FRAME,
  SIGNAL_PIPELINE_ERROR,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Counter for local video frames - reset on each new pipeline */
static int local_frame_count = 0;

static void
reset_local_frame_counter (void)
{
  local_frame_count = 0;
}

static void
gnalk_media_pipeline_finalize (GObject *object)
{
  GnalkMediaPipeline *self = GNALK_MEDIA_PIPELINE (object);

  gnalk_media_pipeline_destroy (self);

  G_OBJECT_CLASS (gnalk_media_pipeline_parent_class)->finalize (object);
}

static void
gnalk_media_pipeline_class_init (GnalkMediaPipelineClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_media_pipeline_finalize;

  /**
   * GnalkMediaPipeline::local-video-frame:
   * @self: the media pipeline
   * @texture: the video frame as a GdkTexture
   *
   * Emitted when a new local video frame is available from the camera preview.
   */
  signals[SIGNAL_LOCAL_VIDEO_FRAME] = g_signal_new (
      "local-video-frame",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL, NULL,
      G_TYPE_NONE, 1, GDK_TYPE_TEXTURE);

  /**
   * GnalkMediaPipeline::pipeline-error:
   * @self: the media pipeline
   * @message: error message
   *
   * Emitted when a pipeline error occurs.
   */
  signals[SIGNAL_PIPELINE_ERROR] = g_signal_new (
      "pipeline-error",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gnalk_media_pipeline_init (GnalkMediaPipeline *self)
{
  self->camera_position = GNALK_CAMERA_FRONT;
  self->video_enabled = TRUE;
  self->audio_enabled = TRUE;
#ifdef HAVE_GSTREAMER
  self->pipewire_fd = -1;
#endif
}

GnalkMediaPipeline *
gnalk_media_pipeline_new (void)
{
  return g_object_new (GNALK_TYPE_MEDIA_PIPELINE, NULL);
}

#ifdef HAVE_GSTREAMER

/* Idle callback data for local video frame signal emission.
 * The appsink "new-sample" callback runs on a GStreamer streaming thread, so
 * the reference to the (main-thread-owned) pipeline is held via a GWeakRef,
 * which is thread-safe. g_object_add_weak_pointer is NOT thread-safe and must
 * not be called from the streaming thread. */
typedef struct {
  GWeakRef    pipeline_ref;
  GdkTexture *texture;
} LocalVideoUpdate;

static gboolean
emit_local_video_in_main_thread (gpointer user_data)
{
  LocalVideoUpdate *update = user_data;
  GnalkMediaPipeline *pipeline = g_weak_ref_get (&update->pipeline_ref);

  if (pipeline)
    {
      g_signal_emit (pipeline, signals[SIGNAL_LOCAL_VIDEO_FRAME], 0,
                     update->texture);
      g_object_unref (pipeline);
    }

  g_weak_ref_clear (&update->pipeline_ref);
  g_clear_object (&update->texture);
  g_free (update);
  return G_SOURCE_REMOVE;
}

static GstFlowReturn
on_local_video_sample (GstAppSink *appsink,
                       gpointer    user_data)
{
  GnalkMediaPipeline *self = GNALK_MEDIA_PIPELINE (user_data);
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

  local_frame_count++;
  if (local_frame_count == 1 || local_frame_count % 30 == 0)
    {
      GstVideoInfo tmp_info;
      if (gst_video_info_from_caps (&tmp_info, caps))
        g_debug ("MediaPipeline: local preview frame %d, size=%dx%d",
                   local_frame_count, tmp_info.width, tmp_info.height);
      else
        g_debug ("MediaPipeline: local preview frame %d", local_frame_count);
    }

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
              GdkTexture *texture = gdk_memory_texture_new (
                  video_info.width,
                  video_info.height,
                  GDK_MEMORY_R8G8B8,
                  bytes,
                  video_info.width * 3);
              g_bytes_unref (bytes);

              if (texture)
                {
                  LocalVideoUpdate *update = g_new (LocalVideoUpdate, 1);
                  g_weak_ref_init (&update->pipeline_ref, self);
                  update->texture = texture;
                  g_idle_add (emit_local_video_in_main_thread, update);
                }
            }
        }
      gst_buffer_unmap (buffer, &map);
    }

  gst_sample_unref (sample);
  return GST_FLOW_OK;
}

/* Find camera by location using GstDeviceMonitor.
 * Searches by api.libcamera.location property (libcamera <0.6) or
 * by device name containing "Front"/"Back" (libcamera >=0.6).
 * Also returns the rotation angle from api.libcamera.rotation property. */
static GstElement *
find_camera_source (GnalkCameraPosition position, gboolean *is_back_camera, gint *out_rotation)
{
  GstDeviceMonitor *monitor;
  GstDevice *found_device = NULL;
  GstElement *source = NULL;
  const char *target_location = (position == GNALK_CAMERA_FRONT) ? "front" : "back";
  const char *target_name = (position == GNALK_CAMERA_FRONT) ? "Front" : "Back";

  if (is_back_camera)
    *is_back_camera = (position == GNALK_CAMERA_BACK);
  if (out_rotation)
    *out_rotation = 0;

  monitor = gst_device_monitor_new ();
  gst_device_monitor_add_filter (monitor, "Video/Source", NULL);

  if (!gst_device_monitor_start (monitor))
    {
      g_warning ("MediaPipeline: Failed to start device monitor");
      gst_object_unref (monitor);
      return NULL;
    }

  GList *devices = gst_device_monitor_get_devices (monitor);
  g_message ("MediaPipeline: Device monitor found %d devices, looking for '%s'",
             g_list_length (devices), target_location);

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
          g_message ("MediaPipeline: Found %s camera '%s' (rotation=%d)", target_location,
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
          g_message ("MediaPipeline: Created libcamerasrc for %s camera (%s)",
                     target_location, camera_path);
        }
      gst_object_unref (found_device);
    }

  return source;
}

/* Create a bin containing the camera source with rotation correction and normalized output.
 * The bin applies rotation FIRST, then scales to 640x480@15fps to match dummy source.
 * This ensures correct aspect ratio regardless of camera sensor orientation.
 * Returns NULL on failure. Caller owns the returned element. */
static GstElement *
create_camera_source_bin (GnalkCameraPosition position)
{
  gboolean is_back_camera = FALSE;
  gint camera_rotation = 0;
  GstElement *camera_src = find_camera_source (position, &is_back_camera, &camera_rotation);

  if (!camera_src)
    {
      g_warning ("MediaPipeline: failed to find camera source");
      return NULL;
    }

  /* Create bin: camera -> queue -> convert -> flip -> rate -> caps
   * Matches pre-call preview structure. No forced dimensions - natural aspect ratio preserved.
   * Downstream encoder will scale to 640x480, preview stays natural. */
  GstElement *bin = gst_bin_new (NULL);
  GstElement *queue = gst_element_factory_make ("queue", NULL);
  GstElement *convert = gst_element_factory_make ("videoconvert", NULL);
  GstElement *flip = gst_element_factory_make ("videoflip", NULL);
  GstElement *rate = gst_element_factory_make ("videorate", NULL);
  GstElement *capsfilter = gst_element_factory_make ("capsfilter", NULL);

  if (!queue || !convert || !flip || !rate || !capsfilter)
    {
      g_warning ("MediaPipeline: failed to create camera bin elements");
      gst_object_unref (camera_src);
      if (queue) gst_object_unref (queue);
      if (convert) gst_object_unref (convert);
      if (flip) gst_object_unref (flip);
      if (rate) gst_object_unref (rate);
      if (capsfilter) gst_object_unref (capsfilter);
      gst_object_unref (bin);
      return NULL;
    }

  /* Queue settings to handle camera timing */
  g_object_set (queue,
                "max-size-buffers", 5,
                "max-size-time", (guint64) 500000000,  /* 500ms */
                "leaky", 2,  /* drop old frames */
                NULL);

  /* Apply rotation correction based on camera sensor orientation */
  gint video_direction = 0;
  if (camera_rotation == 90)
    video_direction = 3;  /* counterclockwise */
  else if (camera_rotation == 180)
    video_direction = 2;  /* rotate-180 */
  else if (camera_rotation == 270)
    video_direction = 1;  /* clockwise */
  g_object_set (flip, "video-direction", video_direction, NULL);

  /* Output: just normalize framerate, no forced dimensions (like pre-call preview) */
  GstCaps *out_caps = gst_caps_from_string ("video/x-raw,framerate=15/1");
  g_object_set (capsfilter, "caps", out_caps, NULL);
  gst_caps_unref (out_caps);

  /* Request resolution from camera based on quality setting */
  GnalkCameraQuality quality = gnalk_camera_quality_from_settings ();
  GnalkCameraConfig config = gnalk_camera_get_config (quality, position);
  GstElement *src_caps = gst_element_factory_make ("capsfilter", "srccaps");
  g_autofree gchar *req_caps_str = g_strdup_printf (
      "video/x-raw,width=%d,height=%d", config.src_width, config.src_height);
  GstCaps *req_caps = gst_caps_from_string (req_caps_str);
  g_object_set (src_caps, "caps", req_caps, NULL);
  gst_caps_unref (req_caps);
  g_message ("MediaPipeline: camera source requesting %dx%d (quality=%d)",
             config.src_width, config.src_height, quality);

  gst_bin_add_many (GST_BIN (bin), camera_src, src_caps, queue, convert, flip, rate, capsfilter, NULL);

  if (!gst_element_link_many (camera_src, src_caps, queue, convert, flip, rate, capsfilter, NULL))
    {
      g_warning ("MediaPipeline: failed to link camera bin elements");
      gst_object_unref (bin);
      return NULL;
    }

  /* Create ghost pad for output */
  GstPad *src_pad = gst_element_get_static_pad (capsfilter, "src");
  GstPad *ghost = gst_ghost_pad_new ("src", src_pad);
  gst_element_add_pad (bin, ghost);
  gst_object_unref (src_pad);

  g_message ("MediaPipeline: created camera bin for %s camera (rotation=%d, video-direction=%d)",
             position == GNALK_CAMERA_FRONT ? "front" : "back",
             camera_rotation, video_direction);

  return bin;
}

/* Create video pipeline with input-selector for camera switching.
 * Structure:
 *   dummy_src (black) --> input_selector --> scale -> caps -> tee
 *   camera_bin -------/                        |
 *                                              +--> encoder -> rtppay (to WebRTC)
 *                                              +--> preview -> appsink (local preview) */
static gboolean
create_video_pipeline (GnalkMediaPipeline *self, GstElement *pipeline)
{
  GError *error = NULL;
  GstElement *video_bin = NULL;

  reset_local_frame_counter ();

  g_message ("MediaPipeline: Creating video pipeline with %s camera",
             self->camera_position == GNALK_CAMERA_FRONT ? "front" : "back");

  /* Create camera source bin (includes rotation correction) */
  GstElement *camera_bin = create_camera_source_bin (self->camera_position);

  if (camera_bin)
    {
      /* Create input-selector and dummy source for seamless camera switching */
      GstElement *input_selector = gst_element_factory_make ("input-selector", "input_selector");
      GstElement *dummy_src = gst_element_factory_make ("videotestsrc", "dummy_src");
      GstElement *dummy_caps = gst_element_factory_make ("capsfilter", "dummy_caps");

      /* Rest of pipeline elements (no flip - rotation is in camera_bin) */
      GstElement *scale1 = gst_element_factory_make ("videoscale", "scale1");
      GstElement *caps2 = gst_element_factory_make ("capsfilter", "caps2");
      GstElement *tee = gst_element_factory_make ("tee", "videotee");

      /* Encode branch - crop sides to 4:3, then scale to 640x480 (no black bars) */
      GstElement *enc_queue = gst_element_factory_make ("queue", "videoqueue");
      GstElement *enc_crop = gst_element_factory_make ("videocrop", "enccrop");
      GstElement *enc_conv = gst_element_factory_make ("videoconvert", "encconv");
      GstElement *enc_scale = gst_element_factory_make ("videoscale", "encscale");
      GstElement *enc_caps = gst_element_factory_make ("capsfilter", "enccaps");
      GstElement *enc_rate = gst_element_factory_make ("videorate", "encrate");
      GstElement *enc = gst_element_factory_make ("vp8enc", "videoenc");
      GstElement *pay = gst_element_factory_make ("rtpvp8pay", "rtppay");

      /* Preview branch */
      GstElement *prev_queue = gst_element_factory_make ("queue", "previewqueue");
      GstElement *prev_scale = gst_element_factory_make ("videoscale", "prevscale");
      GstElement *prev_caps = gst_element_factory_make ("capsfilter", "prevcaps");
      GstElement *prev_crop = gst_element_factory_make ("videocrop", "prevcrop");
      GstElement *prev_conv = gst_element_factory_make ("videoconvert", "prevconv");
      GstElement *prev_caps2 = gst_element_factory_make ("capsfilter", "prevcaps2");
      GstElement *prev_sink = gst_element_factory_make ("appsink", "localpreview");

      if (input_selector && dummy_src && dummy_caps && scale1 && caps2 && tee &&
          enc_queue && enc_crop && enc_conv && enc_scale && enc_caps && enc_rate && enc && pay &&
          prev_queue && prev_scale && prev_caps && prev_crop && prev_conv && prev_caps2 && prev_sink)
        {
          /* Get camera config for crop values */
          GnalkCameraQuality quality = gnalk_camera_quality_from_settings ();
          GnalkCameraConfig config = gnalk_camera_get_config (quality, self->camera_position);

          /* Configure dummy source - black frames, small size for switching */
          g_object_set (dummy_src, "pattern", 2, /* black */
                        "is-live", TRUE, NULL);
          GstCaps *dummy_video_caps = gst_caps_from_string ("video/x-raw,width=320,height=240,framerate=15/1");
          g_object_set (dummy_caps, "caps", dummy_video_caps, NULL);
          gst_caps_unref (dummy_video_caps);

          /* Input selector settings for smooth switching */
          g_object_set (input_selector, "sync-streams", TRUE, "cache-buffers", TRUE, NULL);

          /* Preview videoscales use add-borders to preserve aspect ratio */
          g_object_set (scale1, "add-borders", TRUE, NULL);
          g_object_set (prev_scale, "add-borders", TRUE, NULL);

          /* Caps settings - don't force dimensions, preserve natural aspect ratio */
          GstCaps *c2 = gst_caps_from_string ("video/x-raw");
          GstCaps *pc1 = gst_caps_from_string ("video/x-raw,width=[1,320],height=[1,240],pixel-aspect-ratio=1/1");
          GstCaps *pc2 = gst_caps_from_string ("video/x-raw,format=RGB");
          g_object_set (caps2, "caps", c2, NULL);
          g_object_set (prev_caps, "caps", pc1, NULL);
          g_object_set (prev_caps2, "caps", pc2, NULL);
          gst_caps_unref (c2);
          gst_caps_unref (pc1);
          gst_caps_unref (pc2);

          g_object_set (enc_queue, "max-size-buffers", 3, "leaky", 2, NULL);

          /* Crop sides to ~4:3 aspect ratio before encoding.
           * Camera outputs wide aspect ratio, crop sides to approach 4:3.
           * Crop value based on quality setting. */
          g_object_set (enc_crop, "left", config.encode_crop, "right", config.encode_crop, NULL);

          /* Scale cropped video to 640x480 in I420 format for VP8 encoder */
          GstCaps *enc_i420 = gst_caps_from_string ("video/x-raw,format=I420,width=640,height=480,framerate=15/1");
          g_object_set (enc_caps, "caps", enc_i420, NULL);
          gst_caps_unref (enc_i420);
          /* VP8 encoder settings: fast encoding (deadline=1), low CPU (cpu-used=4) */
          g_object_set (enc, "deadline", 1, "cpu-used", 4,
                        "target-bitrate", 500000, "keyframe-max-dist", 30,
                        "end-usage", 1 /* VBR */, NULL);
          g_object_set (pay, "pt", 96, "mtu", 1200, NULL);

          g_object_set (tee, "allow-not-linked", TRUE, NULL);
          g_object_set (prev_queue, "max-size-buffers", 2, "leaky", 2, NULL);

          /* Set crop values based on camera and quality for preview */
          g_object_set (prev_crop, "left", config.preview_crop, "right", config.preview_crop, NULL);

          g_object_set (prev_sink, "emit-signals", TRUE, "max-buffers", 1, "drop", TRUE, NULL);

          /* Create bin and add all elements */
          video_bin = gst_bin_new ("video-bin");
          gst_bin_add_many (GST_BIN (video_bin), dummy_src, dummy_caps, input_selector,
                            camera_bin, scale1, caps2, tee,
                            enc_queue, enc_crop, enc_conv, enc_scale, enc_caps, enc_rate, enc, pay,
                            prev_queue, prev_scale, prev_caps, prev_crop, prev_conv, prev_caps2, prev_sink, NULL);

          /* Link dummy source to input_selector */
          gboolean link_ok = gst_element_link (dummy_src, dummy_caps);
          GstPad *dummy_src_pad = gst_element_get_static_pad (dummy_caps, "src");
          GstPad *dummy_sel_pad = gst_element_request_pad_simple (input_selector, "sink_%u");
          if (dummy_src_pad && dummy_sel_pad)
            link_ok = link_ok && (gst_pad_link (dummy_src_pad, dummy_sel_pad) == GST_PAD_LINK_OK);
          if (dummy_src_pad) gst_object_unref (dummy_src_pad);

          /* Link camera bin to input_selector */
          GstPad *camera_src_pad = gst_element_get_static_pad (camera_bin, "src");
          GstPad *camera_sel_pad = gst_element_request_pad_simple (input_selector, "sink_%u");
          if (camera_src_pad && camera_sel_pad)
            link_ok = link_ok && (gst_pad_link (camera_src_pad, camera_sel_pad) == GST_PAD_LINK_OK);
          if (camera_src_pad) gst_object_unref (camera_src_pad);

          /* Set active pad to camera (not dummy) */
          if (camera_sel_pad)
            g_object_set (input_selector, "active-pad", camera_sel_pad, NULL);

          /* Link input_selector -> tee */
          link_ok = link_ok && gst_element_link_many (input_selector, tee, NULL);

          /* Link encoder branch: crop sides to 4:3, scale to 640x480, encode */
          link_ok = link_ok && gst_element_link_many (tee, enc_queue, enc_crop, enc_conv,
                                                       enc_scale, enc_caps, enc_rate, enc, pay, NULL);

          /* Link preview branch */
          GstPad *tee_src = gst_element_request_pad_simple (tee, "src_%u");
          GstPad *prev_sink_pad = gst_element_get_static_pad (prev_queue, "sink");
          if (tee_src && prev_sink_pad)
            {
              link_ok = link_ok && (gst_pad_link (tee_src, prev_sink_pad) == GST_PAD_LINK_OK);
              gst_object_unref (prev_sink_pad);
            }
          link_ok = link_ok && gst_element_link_many (prev_queue, prev_scale, prev_caps, prev_crop,
                                                       prev_conv, prev_caps2, prev_sink, NULL);

          if (link_ok)
            {
              /* Create ghost pad for rtppay output */
              GstPad *pay_src = gst_element_get_static_pad (pay, "src");
              GstPad *ghost = gst_ghost_pad_new ("src", pay_src);
              gst_element_add_pad (video_bin, ghost);
              gst_object_unref (pay_src);

              /* Store references */
              self->input_selector = input_selector;
              self->camera_bin = camera_bin;
              self->dummy_src = dummy_src;
              self->camera_selector_pad = camera_sel_pad;
              self->dummy_selector_pad = dummy_sel_pad;
              self->prev_crop = prev_crop;
              self->video_encoder = enc;
              self->local_preview = prev_sink;

              /* Connect local preview callback */
              g_signal_connect (prev_sink, "new-sample", G_CALLBACK (on_local_video_sample), self);

              g_message ("MediaPipeline: Using %s camera with input-selector",
                         self->camera_position == GNALK_CAMERA_FRONT ? "front" : "back");
            }
          else
            {
              g_message ("MediaPipeline: Failed to link video pipeline");
              if (dummy_sel_pad) gst_object_unref (dummy_sel_pad);
              if (camera_sel_pad) gst_object_unref (camera_sel_pad);
              gst_object_unref (video_bin);
              video_bin = NULL;
            }
        }
      else
        {
          g_message ("MediaPipeline: Failed to create video pipeline elements");
          if (camera_bin) gst_object_unref (camera_bin);
          if (input_selector) gst_object_unref (input_selector);
          if (dummy_src) gst_object_unref (dummy_src);
          if (dummy_caps) gst_object_unref (dummy_caps);
          if (scale1) gst_object_unref (scale1);
          if (caps2) gst_object_unref (caps2);
          if (tee) gst_object_unref (tee);
          if (enc_queue) gst_object_unref (enc_queue);
          if (enc_crop) gst_object_unref (enc_crop);
          if (enc_conv) gst_object_unref (enc_conv);
          if (enc_scale) gst_object_unref (enc_scale);
          if (enc_caps) gst_object_unref (enc_caps);
          if (enc_rate) gst_object_unref (enc_rate);
          if (enc) gst_object_unref (enc);
          if (pay) gst_object_unref (pay);
          if (prev_queue) gst_object_unref (prev_queue);
          if (prev_scale) gst_object_unref (prev_scale);
          if (prev_caps) gst_object_unref (prev_caps);
          if (prev_crop) gst_object_unref (prev_crop);
          if (prev_conv) gst_object_unref (prev_conv);
          if (prev_caps2) gst_object_unref (prev_caps2);
          if (prev_sink) gst_object_unref (prev_sink);
        }
    }

  /* Try default libcamera if front camera failed */
  if (!video_bin)
    {
      const char *libcamera_pipeline_str =
          "libcamerasrc name=videosrc ! "
          "video/x-raw ! "
          "videoconvert ! "
          "videoflip video-direction=counterclockwise !"
          "videoscale ! "
          "video/x-raw,format=I420 ! "
          "videorate ! "
          "video/x-raw,framerate=15/1 ! "
          "tee name=videotee ! "
          "queue name=videoqueue max-size-buffers=3 max-size-time=0 max-size-bytes=0 leaky=downstream ! "
          "vp8enc name=videoenc deadline=1 cpu-used=4 target-bitrate=800000 keyframe-max-dist=30 end-usage=vbr undershoot=95 overshoot=100 ! "
          "rtpvp8pay name=rtppay pt=96 mtu=1200 picture-id-mode=15-bit "
          "videotee. ! "
          "queue name=previewqueue max-size-buffers=2 leaky=downstream ! "
          "videoscale ! "
          "video/x-raw ! "
          "videoconvert ! "
          "video/x-raw,format=RGB ! "
          "appsink name=localpreview emit-signals=true max-buffers=1 drop=true";

      video_bin = gst_parse_bin_from_description (libcamera_pipeline_str, TRUE, &error);
      if (error)
        {
          g_message ("MediaPipeline: libcamerasrc failed, trying v4l2src: %s", error->message);
          g_clear_error (&error);
          video_bin = NULL;
        }
      else if (video_bin)
        {
          g_message ("MediaPipeline: Using libcamerasrc with default camera");
        }
    }

  /* Fall back to v4l2src (works on laptops/desktops) */
  if (!video_bin)
    {
      const char *v4l2_pipeline_str =
          "v4l2src name=videosrc ! "
          "videoconvert ! "
          "videoscale ! "
          "video/x-raw,format=I420,framerate=15/1 ! "
          "tee name=videotee ! "
          "queue name=videoqueue max-size-buffers=3 max-size-time=0 max-size-bytes=0 leaky=downstream ! "
          "vp8enc name=videoenc deadline=1 cpu-used=8 target-bitrate=500000 keyframe-max-dist=30 end-usage=cbr ! "
          "rtpvp8pay name=rtppay pt=96 mtu=1200 picture-id-mode=15-bit "
          "videotee. ! "
          "queue name=previewqueue max-size-buffers=2 leaky=downstream ! "
          "videoscale ! "
          "video/x-raw ! "
          "videoconvert ! "
          "video/x-raw,format=RGB ! "
          "appsink name=localpreview emit-signals=true max-buffers=1 drop=true";

      video_bin = gst_parse_bin_from_description (v4l2_pipeline_str, TRUE, &error);
      if (video_bin)
        g_message ("MediaPipeline: Using v4l2src for video capture");
    }

  if (error)
    {
      g_warning ("MediaPipeline: Failed to create video bin: %s", error->message);
      g_error_free (error);
      return FALSE;
    }

  if (!video_bin)
    {
      g_warning ("MediaPipeline: video_bin is NULL");
      return FALSE;
    }

  gst_bin_add (GST_BIN (pipeline), video_bin);

  /* Connect local preview appsink callback for fallback pipelines */
  if (!self->local_preview)
    {
      GstElement *localpreview = gst_bin_get_by_name (GST_BIN (video_bin), "localpreview");
      if (localpreview)
        {
          g_signal_connect (localpreview, "new-sample", G_CALLBACK (on_local_video_sample), self);
          self->local_preview = localpreview;
        }
    }

  /* Get encoder reference for fallback pipelines */
  if (!self->video_encoder)
    {
      GstElement *videoenc = gst_bin_get_by_name (GST_BIN (video_bin), "videoenc");
      if (videoenc)
        self->video_encoder = videoenc;
    }

  /* Get rtppay to add TWCC extension */
  GstElement *rtppay = gst_bin_get_by_name (GST_BIN (video_bin), "rtppay");
  if (rtppay)
    {
      GstRTPHeaderExtension *twcc_ext = gst_rtp_header_extension_create_from_uri (
          "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01");
      if (twcc_ext)
        {
          gst_rtp_header_extension_set_id (twcc_ext, 3);
          g_signal_emit_by_name (rtppay, "add-extension", twcc_ext);
          gst_object_unref (twcc_ext);
        }
      gst_object_unref (rtppay);
    }

  /* Link video bin to webrtcbin */
  GstPad *video_src_pad = gst_element_get_static_pad (video_bin, "src");
  if (!video_src_pad)
    {
      GstIterator *iter = gst_element_iterate_src_pads (video_bin);
      GValue item = G_VALUE_INIT;
      if (gst_iterator_next (iter, &item) == GST_ITERATOR_OK)
        {
          video_src_pad = g_value_dup_object (&item);
          g_value_unset (&item);
        }
      gst_iterator_free (iter);
    }

  GstPad *video_sink_pad = gst_element_request_pad_simple (self->webrtcbin, "sink_%u");

  if (video_src_pad && video_sink_pad)
    {
      GstPadLinkReturn link_ret = gst_pad_link (video_src_pad, video_sink_pad);
      if (link_ret != GST_PAD_LINK_OK)
        g_warning ("MediaPipeline: Video pad link failed: %d", link_ret);
    }
  else
    {
      g_warning ("MediaPipeline: Could not get video pads");
    }

  if (video_src_pad)
    gst_object_unref (video_src_pad);
  if (video_sink_pad)
    gst_object_unref (video_sink_pad);

  self->video_bin = video_bin;
  return TRUE;
}

/* Create audio pipeline: mic -> queue -> convert -> resample -> volume -> encode -> rtp */
static gboolean
create_audio_pipeline (GnalkMediaPipeline *self, GstElement *pipeline)
{
  GstElement *audiosrc, *audioqueue, *audioconvert, *audioresample, *volume, *opusenc, *rtpopuspay;

  audiosrc = gst_element_factory_make ("pulsesrc", "audiosrc");
  if (!audiosrc)
    {
      audiosrc = gst_element_factory_make ("audiotestsrc", "audiosrc");
      if (audiosrc)
        g_object_set (audiosrc, "is-live", TRUE, "wave", 0, NULL);
    }
  else
    {
      g_object_set (audiosrc,
                    "buffer-time", (gint64) 200000,
                    "latency-time", (gint64) 20000,
                    NULL);
    }

  audioqueue = gst_element_factory_make ("queue", "audioqueue");
  audioconvert = gst_element_factory_make ("audioconvert", "audioconvert");
  audioresample = gst_element_factory_make ("audioresample", "audioresample");
  volume = gst_element_factory_make ("volume", "audiovolume");
  opusenc = gst_element_factory_make ("opusenc", "opusenc");
  rtpopuspay = gst_element_factory_make ("rtpopuspay", "rtpopuspay");

  if (audiosrc && audioqueue && audioconvert && audioresample && volume && opusenc && rtpopuspay)
    {
      g_object_set (rtpopuspay, "pt", 111, NULL);
      g_object_set (audioqueue,
                    "max-size-buffers", 0,
                    "max-size-bytes", 0,
                    "max-size-time", (guint64) 200000000,
                    "leaky", 2,
                    NULL);

      self->audio_volume = volume;

      gst_bin_add_many (GST_BIN (pipeline), audiosrc, audioqueue, audioconvert,
                        audioresample, volume, opusenc, rtpopuspay, NULL);

      gst_element_link_many (audiosrc, audioqueue, audioconvert, audioresample,
                             volume, opusenc, rtpopuspay, NULL);

      /* Link to webrtcbin */
      GstPad *audio_src_pad = gst_element_get_static_pad (rtpopuspay, "src");
      GstPad *audio_sink_pad = gst_element_request_pad_simple (self->webrtcbin, "sink_%u");
      gst_pad_link (audio_src_pad, audio_sink_pad);
      gst_object_unref (audio_src_pad);
      gst_object_unref (audio_sink_pad);

      return TRUE;
    }
  else
    {
      g_warning ("MediaPipeline: Could not create audio pipeline elements");
      if (audiosrc) gst_object_unref (audiosrc);
      if (audioqueue) gst_object_unref (audioqueue);
      if (audioconvert) gst_object_unref (audioconvert);
      if (audioresample) gst_object_unref (audioresample);
      if (volume) gst_object_unref (volume);
      if (opusenc) gst_object_unref (opusenc);
      if (rtpopuspay) gst_object_unref (rtpopuspay);
      return FALSE;
    }
}

/* Probes for screen share debugging */
static GstPadProbeReturn
on_screen_pipewiresrc_log_probe (GstPad          *pad,
                                 GstPadProbeInfo *info,
                                 gpointer         user_data)
{
  static int frame_count = 0;
  frame_count++;
  if (frame_count <= 5 || frame_count % 30 == 0)
    g_debug ("MediaPipeline: Screen pipewiresrc output frame %d", frame_count);
  (void)pad; (void)info; (void)user_data;
  return GST_PAD_PROBE_OK;
}

/* Create screen share pipeline - video only, higher bitrate, uses PipeWire source */
static gboolean
create_screen_share_video_pipeline (GnalkMediaPipeline *self, GstElement *pipeline)
{
  GstElement *pipewiresrc, *queue, *capsfilter;
  GstElement *encoder, *rtppay;

  g_message ("MediaPipeline: Creating screen share pipeline (node_id=%u, fd=%d)",
             self->pipewire_node_id, self->pipewire_fd);

  pipewiresrc = gst_element_factory_make ("pipewiresrc", "screensrc");
  queue = gst_element_factory_make ("queue", "srcqueue");
  GstElement *videoconvert = gst_element_factory_make ("videoconvert", "videoconvert");
  capsfilter = gst_element_factory_make ("capsfilter", "capsfilter");
  encoder = gst_element_factory_make ("vp8enc", "screenenc");
  rtppay = gst_element_factory_make ("rtpvp8pay", "rtppay");

  if (!pipewiresrc || !queue || !videoconvert || !capsfilter || !encoder || !rtppay)
    {
      g_warning ("MediaPipeline: Failed to create screen share elements");
      if (pipewiresrc) gst_object_unref (pipewiresrc);
      if (queue) gst_object_unref (queue);
      if (videoconvert) gst_object_unref (videoconvert);
      if (capsfilter) gst_object_unref (capsfilter);
      if (encoder) gst_object_unref (encoder);
      if (rtppay) gst_object_unref (rtppay);
      return FALSE;
    }

  /* Configure pipewiresrc - implicitly a live source.
   * keepalive-time=100 resends last buffer every 100ms on static screens. */
  g_autofree char *path_str = g_strdup_printf ("%u", self->pipewire_node_id);
  g_object_set (pipewiresrc,
                "path", path_str,
                "fd", self->pipewire_fd,
                "do-timestamp", TRUE,
                "always-copy", TRUE,
                "keepalive-time", 100,
                NULL);

  /* Add logging probe */
  GstPad *pw_src_pad = gst_element_get_static_pad (pipewiresrc, "src");
  if (pw_src_pad)
    {
      gst_pad_add_probe (pw_src_pad, GST_PAD_PROBE_TYPE_BUFFER,
                         on_screen_pipewiresrc_log_probe, NULL, NULL);
      gst_object_unref (pw_src_pad);
    }

  /* Configure output caps - I420 format for VP8 encoder */
  GstCaps *caps = gst_caps_from_string ("video/x-raw,format=I420");
  g_object_set (capsfilter, "caps", caps, NULL);
  gst_caps_unref (caps);

  /* Configure encoder for screen content */
  g_object_set (encoder,
                "deadline", 1,
                "cpu-used", 16,
                "target-bitrate", 2000000,
                "keyframe-max-dist", 30,
                "end-usage", 1,
                NULL);

  g_object_set (rtppay, "pt", 96, "mtu", 1200, NULL);
  g_object_set (queue, "max-size-buffers", 10, "leaky", 2, NULL);

  GstElement *queue2 = gst_element_factory_make ("queue", "encqueue");
  g_object_set (queue2, "max-size-buffers", 2, "leaky", 2, NULL);

  gst_bin_add_many (GST_BIN (pipeline), pipewiresrc, queue,
                    videoconvert, capsfilter, queue2, encoder, rtppay, NULL);

  if (!gst_element_link_many (pipewiresrc, queue,
                              videoconvert, capsfilter, queue2, encoder, rtppay, NULL))
    {
      g_warning ("MediaPipeline: Failed to link screen share pipeline");
      return FALSE;
    }

  /* Link to webrtcbin */
  GstPad *rtppay_src = gst_element_get_static_pad (rtppay, "src");
  GstPad *webrtc_sink = gst_element_request_pad_simple (self->webrtcbin, "sink_%u");

  if (!rtppay_src || !webrtc_sink)
    {
      g_warning ("MediaPipeline: Failed to get pads for screen share");
      if (rtppay_src) gst_object_unref (rtppay_src);
      if (webrtc_sink) gst_object_unref (webrtc_sink);
      return FALSE;
    }

  if (gst_pad_link (rtppay_src, webrtc_sink) != GST_PAD_LINK_OK)
    {
      g_warning ("MediaPipeline: Failed to link rtppay to webrtcbin");
      gst_object_unref (rtppay_src);
      gst_object_unref (webrtc_sink);
      return FALSE;
    }

  gst_object_unref (rtppay_src);
  gst_object_unref (webrtc_sink);

  self->video_encoder = encoder;

  g_message ("MediaPipeline: Screen share pipeline created successfully");
  return TRUE;
}

gboolean
gnalk_media_pipeline_create (GnalkMediaPipeline   *self,
                              GnalkMediaPipelineType type,
                              GstElement            *webrtcbin,
                              GnalkCameraPosition   camera_position)
{
  g_return_val_if_fail (GNALK_IS_MEDIA_PIPELINE (self), FALSE);
  g_return_val_if_fail (GST_IS_ELEMENT (webrtcbin), FALSE);

  self->webrtcbin = webrtcbin;
  self->pipeline_type = type;
  self->camera_position = camera_position;

  GstElement *pipeline = GST_ELEMENT (gst_element_get_parent (webrtcbin));
  if (!pipeline)
    {
      g_warning ("MediaPipeline: webrtcbin has no parent pipeline");
      return FALSE;
    }

  gboolean success = TRUE;

  switch (type)
    {
    case GNALK_MEDIA_PIPELINE_SEND:
      success = create_video_pipeline (self, pipeline) &&
                create_audio_pipeline (self, pipeline);
      break;

    case GNALK_MEDIA_PIPELINE_RECEIVE:
      /* No source pipelines for receive-only mode */
      break;

    case GNALK_MEDIA_PIPELINE_SCREEN_SHARE:
      /* Screen share uses separate function with PipeWire params */
      g_warning ("MediaPipeline: Use create_screen_share for screen share pipelines");
      success = FALSE;
      break;
    }

  gst_object_unref (pipeline);
  return success;
}

gboolean
gnalk_media_pipeline_create_screen_share (GnalkMediaPipeline *self,
                                           GstElement          *webrtcbin,
                                           guint32              pipewire_node_id,
                                           int                  pipewire_fd)
{
  g_return_val_if_fail (GNALK_IS_MEDIA_PIPELINE (self), FALSE);
  g_return_val_if_fail (GST_IS_ELEMENT (webrtcbin), FALSE);
  g_return_val_if_fail (pipewire_node_id > 0, FALSE);
  g_return_val_if_fail (pipewire_fd >= 0, FALSE);

  self->webrtcbin = webrtcbin;
  self->pipeline_type = GNALK_MEDIA_PIPELINE_SCREEN_SHARE;
  self->pipewire_node_id = pipewire_node_id;

  /* Duplicate the fd so we can take ownership */
  self->pipewire_fd = dup (pipewire_fd);
  if (self->pipewire_fd < 0)
    {
      g_warning ("MediaPipeline: failed to dup pipewire fd: %s", g_strerror (errno));
      return FALSE;
    }

  GstElement *pipeline = GST_ELEMENT (gst_element_get_parent (webrtcbin));
  if (!pipeline)
    {
      g_warning ("MediaPipeline: webrtcbin has no parent pipeline");
      close (self->pipewire_fd);
      self->pipewire_fd = -1;
      return FALSE;
    }

  gboolean success = create_screen_share_video_pipeline (self, pipeline);

  gst_object_unref (pipeline);

  if (!success)
    {
      close (self->pipewire_fd);
      self->pipewire_fd = -1;
    }

  return success;
}

void
gnalk_media_pipeline_stop_camera (GnalkMediaPipeline *self)
{
  g_return_if_fail (GNALK_IS_MEDIA_PIPELINE (self));

#ifdef HAVE_GSTREAMER
  /* Stop camera_bin first to drain libcamera's queued requests.
   * libcamera on some devices (e.g., OnePlus 6) crashes with assertion
   * "data->queuedRequests_.empty()" or in LogMessage destructor if
   * stopped while requests are pending.
   * Use gradual state transitions with timeouts to allow proper drain. */
  if (self->camera_bin)
    {
      g_message ("MediaPipeline: Stopping camera (allowing libcamera to drain)");

      gst_element_set_state (self->camera_bin, GST_STATE_PAUSED);
      gst_element_get_state (self->camera_bin, NULL, NULL, 2 * GST_SECOND);

      gst_element_set_state (self->camera_bin, GST_STATE_READY);
      gst_element_get_state (self->camera_bin, NULL, NULL, 2 * GST_SECOND);

      gst_element_set_state (self->camera_bin, GST_STATE_NULL);
      gst_element_get_state (self->camera_bin, NULL, NULL, 3 * GST_SECOND);

      g_message ("MediaPipeline: Camera stopped");
    }
#endif
}

void
gnalk_media_pipeline_destroy (GnalkMediaPipeline *self)
{
  g_return_if_fail (GNALK_IS_MEDIA_PIPELINE (self));

  /* Disconnect appsink signal before clearing references */
  if (self->local_preview)
    g_signal_handlers_disconnect_by_data (self->local_preview, self);

  /* Clear element references - elements are owned by the pipeline */
  self->video_bin = NULL;
  self->video_encoder = NULL;
  self->local_preview = NULL;
  self->audio_volume = NULL;
  self->input_selector = NULL;
  self->camera_bin = NULL;
  self->dummy_src = NULL;
  self->prev_crop = NULL;
  self->webrtcbin = NULL;

  if (self->camera_selector_pad)
    {
      gst_object_unref (self->camera_selector_pad);
      self->camera_selector_pad = NULL;
    }

  if (self->dummy_selector_pad)
    {
      gst_object_unref (self->dummy_selector_pad);
      self->dummy_selector_pad = NULL;
    }

  if (self->pipewire_fd >= 0)
    {
      close (self->pipewire_fd);
      self->pipewire_fd = -1;
    }
}

GstElement *
gnalk_media_pipeline_get_video_bin (GnalkMediaPipeline *self)
{
  g_return_val_if_fail (GNALK_IS_MEDIA_PIPELINE (self), NULL);
  return self->video_bin;
}

GstElement *
gnalk_media_pipeline_get_video_encoder (GnalkMediaPipeline *self)
{
  g_return_val_if_fail (GNALK_IS_MEDIA_PIPELINE (self), NULL);
  return self->video_encoder;
}

GstElement *
gnalk_media_pipeline_get_audio_volume (GnalkMediaPipeline *self)
{
  g_return_val_if_fail (GNALK_IS_MEDIA_PIPELINE (self), NULL);
  return self->audio_volume;
}

GstElement *
gnalk_media_pipeline_get_local_preview (GnalkMediaPipeline *self)
{
  g_return_val_if_fail (GNALK_IS_MEDIA_PIPELINE (self), NULL);
  return self->local_preview;
}

#endif /* HAVE_GSTREAMER */

void
gnalk_media_pipeline_switch_camera (GnalkMediaPipeline *self)
{
  g_return_if_fail (GNALK_IS_MEDIA_PIPELINE (self));

#ifdef HAVE_GSTREAMER
  GstElement *pipeline = NULL;

  /* Get pipeline from webrtcbin if available */
  if (self->webrtcbin)
    pipeline = GST_ELEMENT (gst_element_get_parent (self->webrtcbin));

  /* Check if we have the input-selector setup for dynamic switching */
  if (!pipeline || !self->input_selector || !self->camera_bin)
    {
      /* No pipeline or no input-selector - just toggle preference for next call */
      if (self->camera_position == GNALK_CAMERA_FRONT)
        self->camera_position = GNALK_CAMERA_BACK;
      else
        self->camera_position = GNALK_CAMERA_FRONT;
      g_message ("MediaPipeline: Camera preference switched to %s (will apply on next call)",
                 self->camera_position == GNALK_CAMERA_FRONT ? "front" : "back");
      if (pipeline)
        gst_object_unref (pipeline);
      return;
    }

  /* Toggle camera position */
  GnalkCameraPosition new_position;
  if (self->camera_position == GNALK_CAMERA_FRONT)
    new_position = GNALK_CAMERA_BACK;
  else
    new_position = GNALK_CAMERA_FRONT;

  g_message ("MediaPipeline: Switching camera from %s to %s using input-selector",
             self->camera_position == GNALK_CAMERA_FRONT ? "front" : "back",
             new_position == GNALK_CAMERA_FRONT ? "front" : "back");

  /* Step 1: Switch to dummy source to keep stream alive */
  g_message ("MediaPipeline: Switch step 1: Switching to dummy source");
  g_object_set (self->input_selector, "active-pad", self->dummy_selector_pad, NULL);

  /* Step 2: Stop and remove old camera bin */
  g_message ("MediaPipeline: Switch step 2: Stopping old camera");
  gst_element_set_state (self->camera_bin, GST_STATE_NULL);

  /* Unlink camera from input-selector */
  GstPad *camera_src_pad = gst_element_get_static_pad (self->camera_bin, "src");
  if (camera_src_pad && self->camera_selector_pad)
    {
      gst_pad_unlink (camera_src_pad, self->camera_selector_pad);
      gst_object_unref (camera_src_pad);
    }

  /* Release the old selector pad */
  if (self->camera_selector_pad)
    {
      gst_element_release_request_pad (self->input_selector, self->camera_selector_pad);
      gst_object_unref (self->camera_selector_pad);
      self->camera_selector_pad = NULL;
    }

  /* Get the video-bin that contains input_selector */
  GstElement *video_bin = gst_bin_get_by_name (GST_BIN (pipeline), "video-bin");
  if (!video_bin)
    {
      g_warning ("MediaPipeline: Failed to find video-bin for camera switch");
      gst_object_unref (pipeline);
      return;
    }

  /* Remove old camera bin from video-bin */
  gst_bin_remove (GST_BIN (video_bin), self->camera_bin);
  self->camera_bin = NULL;

  /* Step 3: Create new camera bin */
  g_message ("MediaPipeline: Switch step 3: Creating new camera source");
  GstElement *new_camera_bin = create_camera_source_bin (new_position);

  if (!new_camera_bin)
    {
      g_warning ("MediaPipeline: Failed to create new camera bin");
      gst_object_unref (video_bin);
      gst_object_unref (pipeline);
      self->camera_position = new_position;
      return;
    }

  /* Step 4: Add new camera bin to video-bin and link to input-selector */
  g_message ("MediaPipeline: Switch step 4: Adding new camera to video-bin");
  gst_bin_add (GST_BIN (video_bin), new_camera_bin);

  /* Sync state with parent before linking */
  gst_element_sync_state_with_parent (new_camera_bin);

  /* Request new sink pad on input-selector */
  GstPad *new_camera_sel_pad = gst_element_request_pad_simple (self->input_selector, "sink_%u");
  GstPad *new_camera_src_pad = gst_element_get_static_pad (new_camera_bin, "src");

  if (!new_camera_sel_pad || !new_camera_src_pad)
    {
      g_warning ("MediaPipeline: Failed to get pads for new camera");
      gst_bin_remove (GST_BIN (video_bin), new_camera_bin);
      gst_object_unref (video_bin);
      gst_object_unref (pipeline);
      if (new_camera_sel_pad) gst_object_unref (new_camera_sel_pad);
      if (new_camera_src_pad) gst_object_unref (new_camera_src_pad);
      return;
    }

  GstPadLinkReturn link_ret = gst_pad_link (new_camera_src_pad, new_camera_sel_pad);
  if (link_ret != GST_PAD_LINK_OK)
    {
      g_warning ("MediaPipeline: Failed to link new camera: %s",
                 gst_pad_link_get_name (link_ret));
      gst_bin_remove (GST_BIN (video_bin), new_camera_bin);
      gst_element_release_request_pad (self->input_selector, new_camera_sel_pad);
      gst_object_unref (new_camera_sel_pad);
      gst_object_unref (new_camera_src_pad);
      gst_object_unref (video_bin);
      gst_object_unref (pipeline);
      return;
    }
  gst_object_unref (new_camera_src_pad);

  /* Step 5: Start new camera */
  g_message ("MediaPipeline: Switch step 5: Starting new camera");
  gst_element_set_state (new_camera_bin, GST_STATE_PLAYING);

  /* Brief wait for state change */
  gst_element_get_state (new_camera_bin, NULL, NULL, 100 * GST_MSECOND);

  g_message ("MediaPipeline: Switch step 6: Switching to new camera");
  g_object_set (self->input_selector, "active-pad", new_camera_sel_pad, NULL);

  /* Update state */
  self->camera_bin = new_camera_bin;
  self->camera_selector_pad = new_camera_sel_pad;
  self->camera_position = new_position;

  /* Update preview crop for the new camera */
  gnalk_media_pipeline_update_preview_crop (self);

  gst_object_unref (video_bin);
  gst_object_unref (pipeline);

  g_message ("MediaPipeline: Camera switch complete - now using %s camera",
             new_position == GNALK_CAMERA_FRONT ? "front" : "back");

#else
  /* No GStreamer - just toggle preference */
  if (self->camera_position == GNALK_CAMERA_FRONT)
    self->camera_position = GNALK_CAMERA_BACK;
  else
    self->camera_position = GNALK_CAMERA_FRONT;
  g_message ("MediaPipeline: Camera switched to %s",
             self->camera_position == GNALK_CAMERA_FRONT ? "front" : "back");
#endif
}

void
gnalk_media_pipeline_set_camera_position (GnalkMediaPipeline  *self,
                                           GnalkCameraPosition  position)
{
  g_return_if_fail (GNALK_IS_MEDIA_PIPELINE (self));
  self->camera_position = position;
}

GnalkCameraPosition
gnalk_media_pipeline_get_camera_position (GnalkMediaPipeline *self)
{
  g_return_val_if_fail (GNALK_IS_MEDIA_PIPELINE (self), GNALK_CAMERA_FRONT);
  return self->camera_position;
}

void
gnalk_media_pipeline_set_video_enabled (GnalkMediaPipeline *self,
                                         gboolean             enabled)
{
  g_return_if_fail (GNALK_IS_MEDIA_PIPELINE (self));
  self->video_enabled = enabled;

#ifdef HAVE_GSTREAMER
  /* Switch to dummy source (black frames) when video disabled */
  if (self->input_selector && self->dummy_selector_pad && self->camera_selector_pad)
    {
      if (enabled)
        g_object_set (self->input_selector, "active-pad", self->camera_selector_pad, NULL);
      else
        g_object_set (self->input_selector, "active-pad", self->dummy_selector_pad, NULL);
    }
#endif
}

void
gnalk_media_pipeline_set_audio_enabled (GnalkMediaPipeline *self,
                                         gboolean             enabled)
{
  g_return_if_fail (GNALK_IS_MEDIA_PIPELINE (self));
  self->audio_enabled = enabled;

#ifdef HAVE_GSTREAMER
  if (self->audio_volume)
    g_object_set (self->audio_volume, "volume", enabled ? 1.0 : 0.0, NULL);
#endif
}

void
gnalk_media_pipeline_update_preview_crop (GnalkMediaPipeline *self)
{
  g_return_if_fail (GNALK_IS_MEDIA_PIPELINE (self));

#ifdef HAVE_GSTREAMER
  if (self->prev_crop)
    {
      GnalkCameraQuality quality = gnalk_camera_quality_from_settings ();
      GnalkCameraConfig config = gnalk_camera_get_config (quality, self->camera_position);
      g_object_set (self->prev_crop, "left", config.preview_crop, "right", config.preview_crop, NULL);
    }
#endif
}
