/* gnalk-receive-pipeline.c
 *
 * GStreamer receive pipeline for handling incoming WebRTC streams.
 */

#include "gnalk-receive-pipeline.h"
#include "gnalk-quality-controller.h"

#ifdef HAVE_GSTREAMER
#include <gst/video/video.h>
#include <gst/app/gstappsink.h>
#include <gst/rtp/rtp.h>
#endif

struct _GnalkReceivePipeline
{
  GObject parent_instance;

#ifdef HAVE_GSTREAMER
  GstElement *pipeline;        /* Parent pipeline (not owned) */
  GstElement *video_depay;     /* Video depayloader */
  GstElement *video_decoder;   /* Video decoder */
  GstElement *video_sink;      /* Video appsink */
  GstElement *audio_sink;      /* Audio sink */
  GnalkQualityController *quality_controller;  /* Not owned */
#endif

  /* Frame state */
  gboolean got_decoded_video;
  guint    decoded_frame_count;
};

enum {
  SIGNAL_REMOTE_VIDEO_FRAME,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE (GnalkReceivePipeline, gnalk_receive_pipeline, G_TYPE_OBJECT)

static void
gnalk_receive_pipeline_dispose (GObject *object)
{
  GnalkReceivePipeline *self = GNALK_RECEIVE_PIPELINE (object);

  gnalk_receive_pipeline_destroy (self);

  G_OBJECT_CLASS (gnalk_receive_pipeline_parent_class)->dispose (object);
}

static void
gnalk_receive_pipeline_class_init (GnalkReceivePipelineClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gnalk_receive_pipeline_dispose;

  signals[SIGNAL_REMOTE_VIDEO_FRAME] =
    g_signal_new ("remote-video-frame",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1,
                  GDK_TYPE_TEXTURE);
}

static void
gnalk_receive_pipeline_init (GnalkReceivePipeline *self)
{
  self->got_decoded_video = FALSE;
  self->decoded_frame_count = 0;
}

GnalkReceivePipeline *
gnalk_receive_pipeline_new (void)
{
  return g_object_new (GNALK_TYPE_RECEIVE_PIPELINE, NULL);
}

#ifdef HAVE_GSTREAMER

void
gnalk_receive_pipeline_set_pipeline (GnalkReceivePipeline *self,
                                       GstElement            *pipeline)
{
  g_return_if_fail (GNALK_IS_RECEIVE_PIPELINE (self));
  self->pipeline = pipeline;  /* Not ref'd - owned by caller */
}

void
gnalk_receive_pipeline_set_quality_controller (GnalkReceivePipeline   *self,
                                                 GnalkQualityController *controller)
{
  g_return_if_fail (GNALK_IS_RECEIVE_PIPELINE (self));
  self->quality_controller = controller;  /* Not ref'd - owned by caller */
}

/* Structure for dispatching video frames to main thread */
typedef struct {
  GnalkReceivePipeline *recv_pipeline;
  GdkTexture *texture;
} RemoteVideoUpdate;

static gboolean
update_remote_video_in_main_thread (gpointer user_data)
{
  RemoteVideoUpdate *update = user_data;

  if (update->recv_pipeline)
    {
      g_signal_emit (update->recv_pipeline, signals[SIGNAL_REMOTE_VIDEO_FRAME], 0, update->texture);
      g_object_remove_weak_pointer (G_OBJECT (update->recv_pipeline), (gpointer *)&update->recv_pipeline);
    }

  g_object_unref (update->texture);
  g_free (update);
  return G_SOURCE_REMOVE;
}

/* Diagnostic pad probes */
static GstPadProbeReturn
on_webrtcbin_video_probe (GstPad          *pad,
                          GstPadProbeInfo *info,
                          gpointer         user_data)
{
  (void)pad;
  (void)info;
  (void)user_data;

  static int webrtc_packet_count = 0;
  webrtc_packet_count++;
  if (webrtc_packet_count <= 5 || webrtc_packet_count % 100 == 0)
    g_message ("WebRTC video probe: packet %d from webrtcbin", webrtc_packet_count);
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
on_decoded_video_probe (GstPad          *pad,
                        GstPadProbeInfo *info,
                        gpointer         user_data)
{
  (void)pad;
  (void)info;
  (void)user_data;

  static int decoded_frame_count = 0;
  decoded_frame_count++;
  if (decoded_frame_count <= 5 || decoded_frame_count % 30 == 0)
    g_message ("Decoded video probe: frame %d from vp8dec", decoded_frame_count);
  return GST_PAD_PROBE_OK;
}

static GstFlowReturn
on_remote_video_sample (GstAppSink *appsink,
                        gpointer    user_data)
{
  GnalkReceivePipeline *self = GNALK_RECEIVE_PIPELINE (user_data);
  GstSample *sample;
  GstBuffer *buffer;
  GstMapInfo map;
  GstCaps *caps;
  GstVideoInfo video_info;

  sample = gst_app_sink_pull_sample (appsink);
  if (!sample)
    {
      g_message ("on_remote_video_sample: no sample available");
      return GST_FLOW_OK;
    }

  self->decoded_frame_count++;

  /* After receiving 10 frames from decoder, assume we have good video */
  if (!self->got_decoded_video && self->decoded_frame_count >= 10)
    {
      self->got_decoded_video = TRUE;
      if (self->quality_controller)
        gnalk_quality_controller_set_video_received (self->quality_controller, TRUE);
      g_message ("Remote video: got %u frames, stopping PLI timer for recv_pipeline=%p",
                 self->decoded_frame_count, (void *)self);
    }

  if (self->decoded_frame_count % 30 == 0)
    g_message ("Remote video: received frame %u for recv_pipeline=%p",
               self->decoded_frame_count, (void *)self);

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
          gint stride = GST_VIDEO_INFO_PLANE_STRIDE (&video_info, 0);
          if (stride <= 0)
            stride = video_info.width * 3;

          /* Log resolution changes */
          static int last_width = 0, last_height = 0;
          if (video_info.width != last_width || video_info.height != last_height)
            {
              g_message ("Remote video resolution: %dx%d, stride=%d (expected=%d)",
                         video_info.width, video_info.height, stride, video_info.width * 3);
              last_width = video_info.width;
              last_height = video_info.height;
            }

          GBytes *bytes = g_bytes_new (map.data, map.size);
          if (bytes)
            {
              GdkTexture *texture = gdk_memory_texture_new (
                video_info.width,
                video_info.height,
                GDK_MEMORY_R8G8B8,
                bytes,
                stride);
              g_bytes_unref (bytes);

              if (texture)
                {
                  RemoteVideoUpdate *update = g_new (RemoteVideoUpdate, 1);
                  update->recv_pipeline = self;
                  update->texture = texture;
                  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *)&update->recv_pipeline);
                  g_idle_add (update_remote_video_in_main_thread, update);
                }
            }
        }
      gst_buffer_unmap (buffer, &map);
    }

  gst_sample_unref (sample);
  return GST_FLOW_OK;
}

void
gnalk_receive_pipeline_handle_pad_added (GnalkReceivePipeline *self,
                                           GstPad                *pad)
{
  g_return_if_fail (GNALK_IS_RECEIVE_PIPELINE (self));
  g_return_if_fail (GST_IS_PAD (pad));
  g_return_if_fail (self->pipeline != NULL);

  g_message ("recv_pipeline: pad-added signal, pad name: %s, direction: %s",
             GST_PAD_NAME (pad),
             GST_PAD_DIRECTION (pad) == GST_PAD_SRC ? "SRC" : "SINK");

  if (GST_PAD_DIRECTION (pad) != GST_PAD_SRC)
    {
      g_message ("recv_pipeline: ignoring non-src pad");
      return;
    }

  /* Get caps to determine media type and encoding */
  GstCaps *pad_caps = gst_pad_get_current_caps (pad);
  if (!pad_caps)
    {
      g_message ("recv_pipeline: pad has no caps yet, waiting for caps");
      return;
    }

  g_autofree char *caps_str = gst_caps_to_string (pad_caps);
  g_message ("recv_pipeline: pad caps: %s", caps_str);

  /* Parse caps to get media type and encoding */
  GstStructure *s = gst_caps_get_structure (pad_caps, 0);
  const gchar *media = gst_structure_get_string (s, "media");
  const gchar *encoding = gst_structure_get_string (s, "encoding-name");
  gst_caps_unref (pad_caps);

  if (!media || !encoding)
    {
      g_warning ("recv_pipeline: missing media or encoding in caps");
      return;
    }

  g_message ("recv_pipeline: media=%s, encoding=%s", media, encoding);

  if (g_strcmp0 (media, "video") == 0 && g_strcmp0 (encoding, "VP8") == 0)
    {
      g_message ("recv_pipeline: creating VP8 video decoder chain");

      GstElement *depay = gst_element_factory_make ("rtpvp8depay", NULL);
      GstElement *decoder = gst_element_factory_make ("vp8dec", NULL);
      GstElement *queue = gst_element_factory_make ("queue", NULL);

      if (depay)
        {
          g_object_set (depay,
                        "request-keyframe", TRUE,
                        "wait-for-keyframe", FALSE,
                        NULL);
          g_message ("recv_pipeline: enabled request-keyframe on depay");
        }

      GstElement *convert = gst_element_factory_make ("videoconvert", NULL);
      GstElement *scale = gst_element_factory_make ("videoscale", NULL);
      GstElement *appsink = gst_element_factory_make ("appsink", "remote-video-sink");

      if (depay && decoder && queue && convert && scale && appsink)
        {
          GstCaps *sink_caps = gst_caps_from_string ("video/x-raw,format=RGB");
          g_object_set (appsink,
                        "emit-signals", TRUE,
                        "caps", sink_caps,
                        "max-buffers", 1,
                        "drop", TRUE,
                        "async", FALSE,
                        NULL);
          gst_caps_unref (sink_caps);

          gulong signal_id = g_signal_connect (appsink, "new-sample", G_CALLBACK (on_remote_video_sample), self);
          g_message ("recv_pipeline: connected new-sample signal, id=%lu", signal_id);

          g_object_set (queue,
                        "max-size-buffers", 1,
                        "max-size-bytes", 0,
                        "max-size-time", (guint64) 0,
                        NULL);

          gst_bin_add_many (GST_BIN (self->pipeline), depay, decoder, queue, convert, scale, appsink, NULL);

          gst_element_set_state (depay, GST_STATE_PLAYING);
          gst_element_set_state (decoder, GST_STATE_PLAYING);
          gst_element_set_state (queue, GST_STATE_PLAYING);
          gst_element_set_state (convert, GST_STATE_PLAYING);
          gst_element_set_state (scale, GST_STATE_PLAYING);
          gst_element_set_state (appsink, GST_STATE_PLAYING);

          gboolean link_ok = gst_element_link_many (depay, decoder, queue, convert, scale, appsink, NULL);
          g_message ("recv_pipeline: video element link result = %d", link_ok);

          GstPad *sink_pad = gst_element_get_static_pad (depay, "sink");
          GstPadLinkReturn link_ret = gst_pad_link (pad, sink_pad);
          g_message ("recv_pipeline: video pad link result=%d", link_ret);

          /* Add diagnostic probes */
          gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER,
                             on_webrtcbin_video_probe, NULL, NULL);
          g_message ("recv_pipeline: added probe on webrtcbin src pad");

          /* Use quality controller probes for RTP diagnostics */
          if (self->quality_controller)
            {
              GstPadProbeCallback rtp_probe = gnalk_quality_controller_get_rtp_probe (self->quality_controller);
              gst_pad_add_probe (sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
                                 rtp_probe, self->quality_controller, NULL);
              g_message ("recv_pipeline: added QC RTP probe on depay sink pad");
            }
          gst_object_unref (sink_pad);

          /* Add probe on depay src */
          GstPad *depay_src = gst_element_get_static_pad (depay, "src");
          if (depay_src)
            {
              GstPad *peer = gst_pad_get_peer (depay_src);
              g_message ("recv_pipeline: depay src linked=%d peer=%s",
                         peer != NULL,
                         peer ? GST_PAD_NAME (peer) : "none");
              if (peer)
                gst_object_unref (peer);

              if (self->quality_controller)
                {
                  GstPadProbeCallback depay_probe = gnalk_quality_controller_get_depay_probe (self->quality_controller);
                  gst_pad_add_probe (depay_src, GST_PAD_PROBE_TYPE_BUFFER,
                                     depay_probe, self->quality_controller, NULL);
                  g_message ("recv_pipeline: added QC probe on depay src pad");
                }
              gst_object_unref (depay_src);
            }

          GstPad *decoder_src = gst_element_get_static_pad (decoder, "src");
          if (decoder_src)
            {
              gst_pad_add_probe (decoder_src, GST_PAD_PROBE_TYPE_BUFFER,
                                 on_decoded_video_probe, NULL, NULL);
              g_message ("recv_pipeline: added decoded frame probe on decoder src pad");
              gst_object_unref (decoder_src);
            }

          /* Log actual states */
          GstState pipeline_state, depay_state, decoder_state;
          gst_element_get_state (self->pipeline, &pipeline_state, NULL, 0);
          gst_element_get_state (depay, &depay_state, NULL, 0);
          gst_element_get_state (decoder, &decoder_state, NULL, 0);
          g_message ("recv_pipeline: states - pipeline=%s depay=%s decoder=%s",
                     gst_element_state_get_name (pipeline_state),
                     gst_element_state_get_name (depay_state),
                     gst_element_state_get_name (decoder_state));

          self->video_sink = appsink;
          self->video_depay = depay;
          self->video_decoder = decoder;
          g_message ("recv_pipeline: VP8 video pipeline created successfully");

          /* Set up quality controller for PLI management */
          GstPad *appsink_pad = gst_element_get_static_pad (appsink, "sink");
          if (appsink_pad)
            {
              if (self->quality_controller)
                {
                  gnalk_quality_controller_set_pli_pad (self->quality_controller, appsink_pad);
                  gnalk_quality_controller_reset_stats (self->quality_controller);
                  gnalk_quality_controller_send_pli (self->quality_controller);
                  gnalk_quality_controller_start_pli_timer (self->quality_controller);
                  g_message ("recv_pipeline: set up quality controller PLI");
                }
              gst_object_unref (appsink_pad);
            }
        }
      else
        {
          g_warning ("Failed to create VP8 video decoder elements");
          if (depay) gst_object_unref (depay);
          if (decoder) gst_object_unref (decoder);
          if (queue) gst_object_unref (queue);
          if (convert) gst_object_unref (convert);
          if (scale) gst_object_unref (scale);
          if (appsink) gst_object_unref (appsink);
        }
    }
  else if (g_strcmp0 (media, "audio") == 0 && g_strcmp0 (encoding, "OPUS") == 0)
    {
      g_message ("recv_pipeline: creating OPUS audio decoder chain");

      GstElement *depay = gst_element_factory_make ("rtpopusdepay", NULL);
      GstElement *decoder = gst_element_factory_make ("opusdec", NULL);
      GstElement *convert = gst_element_factory_make ("audioconvert", NULL);
      GstElement *resample = gst_element_factory_make ("audioresample", NULL);
      GstElement *sink = gst_element_factory_make ("autoaudiosink", NULL);

      if (depay && decoder && convert && resample && sink)
        {
          gst_bin_add_many (GST_BIN (self->pipeline), depay, decoder, convert, resample, sink, NULL);

          gboolean link_ok = gst_element_link_many (depay, decoder, convert, resample, sink, NULL);
          g_message ("recv_pipeline: audio element link result = %d", link_ok);

          GstPad *sink_pad = gst_element_get_static_pad (depay, "sink");
          GstPadLinkReturn link_ret = gst_pad_link (pad, sink_pad);
          g_message ("recv_pipeline: audio pad link result=%d", link_ret);
          gst_object_unref (sink_pad);

          gst_element_set_state (depay, GST_STATE_PLAYING);
          gst_element_set_state (decoder, GST_STATE_PLAYING);
          gst_element_set_state (convert, GST_STATE_PLAYING);
          gst_element_set_state (resample, GST_STATE_PLAYING);
          gst_element_set_state (sink, GST_STATE_PLAYING);

          self->audio_sink = sink;
          g_message ("recv_pipeline: OPUS audio pipeline created successfully");
        }
      else
        {
          g_warning ("Failed to create OPUS audio decoder elements");
          if (depay) gst_object_unref (depay);
          if (decoder) gst_object_unref (decoder);
          if (convert) gst_object_unref (convert);
          if (resample) gst_object_unref (resample);
          if (sink) gst_object_unref (sink);
        }
    }
  else
    {
      g_message ("recv_pipeline: unsupported media=%s encoding=%s", media, encoding);
    }
}

GstElement *
gnalk_receive_pipeline_get_video_depay (GnalkReceivePipeline *self)
{
  g_return_val_if_fail (GNALK_IS_RECEIVE_PIPELINE (self), NULL);
  return self->video_depay;
}

GstElement *
gnalk_receive_pipeline_get_video_sink (GnalkReceivePipeline *self)
{
  g_return_val_if_fail (GNALK_IS_RECEIVE_PIPELINE (self), NULL);
  return self->video_sink;
}

void
gnalk_receive_pipeline_destroy (GnalkReceivePipeline *self)
{
  g_return_if_fail (GNALK_IS_RECEIVE_PIPELINE (self));

  /* Elements are owned by the parent pipeline, just clear our refs */
  self->video_depay = NULL;
  self->video_decoder = NULL;
  self->video_sink = NULL;
  self->audio_sink = NULL;
  self->pipeline = NULL;
  self->quality_controller = NULL;
}

#endif /* HAVE_GSTREAMER */

void
gnalk_receive_pipeline_reset_frame_counters (GnalkReceivePipeline *self)
{
  g_return_if_fail (GNALK_IS_RECEIVE_PIPELINE (self));

  self->got_decoded_video = FALSE;
  self->decoded_frame_count = 0;
}

gboolean
gnalk_receive_pipeline_has_video (GnalkReceivePipeline *self)
{
  g_return_val_if_fail (GNALK_IS_RECEIVE_PIPELINE (self), FALSE);
  return self->got_decoded_video;
}
