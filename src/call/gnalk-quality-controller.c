/* gnalk-quality-controller.c
 *
 * Video quality monitoring and adaptation for WebRTC calls.
 */

#include "gnalk-quality-controller.h"

#ifdef HAVE_GSTREAMER
#include <gst/rtp/rtp.h>
#include <gst/video/video.h>
#ifdef HAVE_GSTREAMER_WEBRTC
#include <gst/webrtc/webrtc.h>
#endif
#endif

/* Adaptive bitrate constants */
#define BITRATE_MIN      100000   /* 100 kbps minimum */
#define BITRATE_MAX     1500000   /* 1.5 Mbps maximum */
#define BITRATE_START    500000   /* 500 kbps starting bitrate */
#define BITRATE_INTERVAL   2000   /* Check every 2 seconds */

/* PLI timer intervals */
#define PLI_INITIAL_INTERVAL  300  /* 300ms for initial keyframe requests */

struct _GnalkQualityController
{
  GObject parent_instance;

#ifdef HAVE_GSTREAMER
  GstElement *webrtcbin;
  GstElement *video_encoder;
  GstPad     *pli_pad;
#endif

  /* Bitrate adaptation state */
  guint       bitrate_timer_id;
  guint       current_bitrate;
  guint64     last_packets_sent;
  guint64     last_packets_lost;

  /* PLI state */
  guint       pli_timer_id;
  gboolean    got_decoded_video;
  guint       decoded_frame_count;

  /* Continuous PLI for screen share */
  gboolean    continuous_pli;
  guint       continuous_pli_timer_id;
  guint       continuous_pli_interval;

  /* RTP diagnostics */
  guint       rtp_packet_count;
  guint16     last_rtp_seq;
  guint32     last_rtp_ssrc;
  guint       rtp_seq_gaps;
  guint       keyframe_count;
  guint       depay_frame_count;

  /* Recovery PLI state */
  guint       last_keyframe_count;
  guint       keyframe_stall_cycles;

  /* PLI request tracking */
  guint       pli_request_count;
};

enum {
  SIGNAL_QUALITY_DEGRADED,
  SIGNAL_STATS_UPDATED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE (GnalkQualityController, gnalk_quality_controller, G_TYPE_OBJECT)

static void
gnalk_quality_controller_dispose (GObject *object)
{
  GnalkQualityController *self = GNALK_QUALITY_CONTROLLER (object);

  gnalk_quality_controller_stop_bitrate_timer (self);
  gnalk_quality_controller_stop_pli_timer (self);

  if (self->continuous_pli_timer_id)
    {
      g_source_remove (self->continuous_pli_timer_id);
      self->continuous_pli_timer_id = 0;
    }

#ifdef HAVE_GSTREAMER
  g_clear_pointer (&self->pli_pad, gst_object_unref);
  self->webrtcbin = NULL;
  self->video_encoder = NULL;
#endif

  G_OBJECT_CLASS (gnalk_quality_controller_parent_class)->dispose (object);
}

static void
gnalk_quality_controller_class_init (GnalkQualityControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gnalk_quality_controller_dispose;

  signals[SIGNAL_QUALITY_DEGRADED] =
    g_signal_new ("quality-degraded",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);

  signals[SIGNAL_STATS_UPDATED] =
    g_signal_new ("stats-updated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_UINT,    /* bitrate_kbps */
                  G_TYPE_UINT,    /* packet_loss_pct */
                  G_TYPE_UINT);   /* keyframe_pct */
}

static void
gnalk_quality_controller_init (GnalkQualityController *self)
{
  self->current_bitrate = BITRATE_START;
}

GnalkQualityController *
gnalk_quality_controller_new (void)
{
  return g_object_new (GNALK_TYPE_QUALITY_CONTROLLER, NULL);
}

#ifdef HAVE_GSTREAMER

void
gnalk_quality_controller_set_webrtcbin (GnalkQualityController *self,
                                          GstElement              *webrtcbin)
{
  g_return_if_fail (GNALK_IS_QUALITY_CONTROLLER (self));
  self->webrtcbin = webrtcbin;  /* Not ref'd - owned by pipeline */
}

void
gnalk_quality_controller_set_video_encoder (GnalkQualityController *self,
                                              GstElement              *encoder)
{
  g_return_if_fail (GNALK_IS_QUALITY_CONTROLLER (self));
  self->video_encoder = encoder;  /* Not ref'd - owned by pipeline */
}

void
gnalk_quality_controller_set_pli_pad (GnalkQualityController *self,
                                        GstPad                  *pad)
{
  g_return_if_fail (GNALK_IS_QUALITY_CONTROLLER (self));

  if (self->pli_pad)
    gst_object_unref (self->pli_pad);

  self->pli_pad = pad ? gst_object_ref (pad) : NULL;
}

#ifdef HAVE_GSTREAMER_WEBRTC
/* Stats promise callback for bitrate adaptation */
static void
on_stats_promise_reply (GstPromise *promise, gpointer user_data)
{
  GnalkQualityController *self = GNALK_QUALITY_CONTROLLER (user_data);
  const GstStructure *reply, *stats;
  guint64 packets_sent = 0, packets_lost = 0;
  gboolean found_outbound = FALSE;

  if (gst_promise_wait (promise) != GST_PROMISE_RESULT_REPLIED)
    {
      gst_promise_unref (promise);
      return;
    }

  reply = gst_promise_get_reply (promise);
  if (!reply)
    {
      gst_promise_unref (promise);
      return;
    }

  /* Iterate through all stats structures to find outbound-rtp and remote-inbound-rtp */
  guint n_fields = gst_structure_n_fields (reply);
  for (guint i = 0; i < n_fields; i++)
    {
      const gchar *field_name = gst_structure_nth_field_name (reply, i);
      const GValue *value = gst_structure_get_value (reply, field_name);

      if (G_VALUE_TYPE (value) == GST_TYPE_STRUCTURE)
        {
          stats = gst_value_get_structure (value);
          GstWebRTCStatsType stats_type;

          if (gst_structure_get (stats, "type", GST_TYPE_WEBRTC_STATS_TYPE, &stats_type, NULL))
            {
              if (stats_type == GST_WEBRTC_STATS_OUTBOUND_RTP)
                {
                  gst_structure_get_uint64 (stats, "packets-sent", &packets_sent);
                  found_outbound = TRUE;
                }
              else if (stats_type == GST_WEBRTC_STATS_REMOTE_INBOUND_RTP)
                {
                  gst_structure_get_uint64 (stats, "packets-lost", &packets_lost);
                }
              else if (stats_type == GST_WEBRTC_STATS_INBOUND_RTP)
                {
                  /* Log inbound RTP stats for debugging */
                  guint64 packets_received = 0, packets_lost_in = 0, nack_count = 0;
                  guint ssrc = 0;
                  gdouble jitter = 0;

                  gst_structure_get_uint (stats, "ssrc", &ssrc);
                  gst_structure_get_uint64 (stats, "packets-received", &packets_received);
                  gst_structure_get_uint64 (stats, "packets-lost", &packets_lost_in);
                  gst_structure_get_uint64 (stats, "nack-count", &nack_count);
                  gst_structure_get_double (stats, "jitter", &jitter);

                  static guint64 last_log_packets = 0;
                  if (packets_received > last_log_packets + 200)
                    {
                      g_message ("QC INBOUND RTP[0x%08x]: recv=%lu lost=%lu nack=%lu jitter=%.1fms",
                                 ssrc, (unsigned long)packets_received, (unsigned long)packets_lost_in,
                                 (unsigned long)nack_count, jitter * 1000.0);
                      last_log_packets = packets_received;
                    }
                }
            }
        }
    }

  if (found_outbound && packets_sent > 0)
    {
      /* Calculate packet loss ratio since last check */
      guint64 new_packets = packets_sent - self->last_packets_sent;
      guint64 new_lost = packets_lost - self->last_packets_lost;
      gdouble loss_ratio = 0.0;

      if (new_packets > 10)
        {
          loss_ratio = (gdouble)new_lost / (gdouble)new_packets;

          guint new_bitrate = self->current_bitrate;

          if (loss_ratio > 0.10)
            {
              /* > 10% loss: aggressive reduction */
              new_bitrate = (guint)(self->current_bitrate * 0.7);
              g_message ("QC: HIGH loss (%.1f%%), reducing to %u kbps",
                         loss_ratio * 100, new_bitrate / 1000);
            }
          else if (loss_ratio > 0.05)
            {
              /* 5-10% loss: moderate reduction */
              new_bitrate = (guint)(self->current_bitrate * 0.85);
              g_message ("QC: moderate loss (%.1f%%), reducing to %u kbps",
                         loss_ratio * 100, new_bitrate / 1000);
            }
          else if (loss_ratio < 0.02 && self->current_bitrate < BITRATE_MAX)
            {
              /* < 2% loss: try increasing */
              new_bitrate = (guint)(self->current_bitrate * 1.1);
              if (new_bitrate > BITRATE_MAX)
                new_bitrate = BITRATE_MAX;
              g_message ("QC: low loss (%.1f%%), increasing to %u kbps",
                         loss_ratio * 100, new_bitrate / 1000);
            }

          /* Clamp to min/max */
          if (new_bitrate < BITRATE_MIN)
            new_bitrate = BITRATE_MIN;
          if (new_bitrate > BITRATE_MAX)
            new_bitrate = BITRATE_MAX;

          /* Apply new bitrate if changed significantly (> 5%) */
          if (new_bitrate != self->current_bitrate)
            {
              gint diff = (gint)new_bitrate - (gint)self->current_bitrate;
              if (diff < 0) diff = -diff;
              if (diff > (gint)(self->current_bitrate * 0.05))
                {
                  self->current_bitrate = new_bitrate;
                  if (self->video_encoder)
                    {
                      g_object_set (self->video_encoder, "target-bitrate", new_bitrate, NULL);
                      g_message ("QC: encoder set to %u kbps", new_bitrate / 1000);
                    }
                }
            }

          /* Emit stats signal */
          guint loss_pct = (guint)(loss_ratio * 100);
          guint keyframe_pct = self->depay_frame_count > 0
            ? (self->keyframe_count * 100) / self->depay_frame_count : 0;
          g_signal_emit (self, signals[SIGNAL_STATS_UPDATED], 0,
                         self->current_bitrate / 1000, loss_pct, keyframe_pct);
        }

      self->last_packets_sent = packets_sent;
      self->last_packets_lost = packets_lost;
    }

  gst_promise_unref (promise);
}

static gboolean
bitrate_timer_callback (gpointer user_data)
{
  GnalkQualityController *self = GNALK_QUALITY_CONTROLLER (user_data);

  if (!self->webrtcbin)
    return G_SOURCE_CONTINUE;

  /* Request stats from webrtcbin */
  GstPromise *promise = gst_promise_new_with_change_func (on_stats_promise_reply, self, NULL);
  g_signal_emit_by_name (self->webrtcbin, "get-stats", NULL, promise);

  return G_SOURCE_CONTINUE;
}
#endif /* HAVE_GSTREAMER_WEBRTC */

/* RTP probe for packet analysis */
static GstPadProbeReturn
rtp_video_probe_cb (GstPad          *pad,
                    GstPadProbeInfo *info,
                    gpointer         user_data)
{
  GnalkQualityController *self = GNALK_QUALITY_CONTROLLER (user_data);
  (void)pad;

  GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  if (gst_rtp_buffer_map (buf, GST_MAP_READ, &rtp))
    {
      guint16 seq = gst_rtp_buffer_get_seq (&rtp);
      guint32 ssrc = gst_rtp_buffer_get_ssrc (&rtp);
      guint payload_len = gst_rtp_buffer_get_payload_len (&rtp);
      guint8 *payload = gst_rtp_buffer_get_payload (&rtp);

      self->rtp_packet_count++;

      /* Detect SSRC changes */
      if (self->last_rtp_ssrc != 0 && ssrc != self->last_rtp_ssrc)
        {
          g_message ("QC RTP SSRC CHANGE: was 0x%08x, now 0x%08x at packet %u",
                     self->last_rtp_ssrc, ssrc, self->rtp_packet_count);
        }
      self->last_rtp_ssrc = ssrc;

      /* Check for sequence number gaps (packet loss) */
      if (self->rtp_packet_count > 1)
        {
          guint16 expected_seq = self->last_rtp_seq + 1;
          if (seq != expected_seq)
            {
              gint16 gap = (gint16)(seq - expected_seq);
              if (gap > 0)
                {
                  self->rtp_seq_gaps++;
                  g_message ("QC RTP seq gap: expected %u, got %u (gap=%d, total=%u)",
                             expected_seq, seq, gap, self->rtp_seq_gaps);
                }
            }
        }
      self->last_rtp_seq = seq;

      /* VP8 payload parsing for keyframe detection (RFC 7741) */
      guint8 desc0 = payload_len > 0 ? payload[0] : 0;
      gboolean is_start = (desc0 & 0x10) != 0;
      gboolean has_ext = (desc0 & 0x80) != 0;
      guint8 part_id = desc0 & 0x0F;

      guint8 vp8_hdr = 0;
      if (is_start && payload_len > 1)
        {
          int offset = 1;

          if (has_ext && payload_len > (guint)offset)
            {
              guint8 ext = payload[offset];
              offset++;

              if ((ext & 0x80) && payload_len > (guint)offset)
                {
                  if (payload[offset] & 0x80)
                    offset += 2;
                  else
                    offset += 1;
                }

              if ((ext & 0x40) && payload_len > (guint)offset)
                offset++;

              if ((ext & 0x20 || ext & 0x10) && payload_len > (guint)offset)
                offset++;
            }

          if (payload_len > (guint)offset)
            vp8_hdr = payload[offset];
        }

      gboolean keyframe = is_start && part_id == 0 && ((vp8_hdr & 0x01) == 0);

      if (keyframe)
        {
          self->keyframe_count++;
          g_message ("QC KEYFRAME #%u at packet %u (seq=%u)",
                     self->keyframe_count, self->rtp_packet_count, seq);
        }

      /* Log periodically */
      if (self->rtp_packet_count <= 20 || self->rtp_packet_count % 300 == 0)
        {
          guint8 pt = gst_rtp_buffer_get_payload_type (&rtp);
          gboolean marker = gst_rtp_buffer_get_marker (&rtp);
          g_message ("QC RTP pkt=%u seq=%u pt=%u marker=%d len=%u keyframes=%u gaps=%u",
                     self->rtp_packet_count, seq, pt, marker,
                     payload_len, self->keyframe_count, self->rtp_seq_gaps);
        }

      gst_rtp_buffer_unmap (&rtp);
    }

  return GST_PAD_PROBE_OK;
}

/* Depayloader output probe */
static GstPadProbeReturn
depay_src_probe_cb (GstPad          *pad,
                    GstPadProbeInfo *info,
                    gpointer         user_data)
{
  GnalkQualityController *self = GNALK_QUALITY_CONTROLLER (user_data);
  (void)pad;
  (void)info;

  self->depay_frame_count++;

  if (self->depay_frame_count <= 5 || self->depay_frame_count % 100 == 0)
    g_message ("QC Depay frame %u (rtp_pkts=%u, keyframes=%u, gaps=%u)",
               self->depay_frame_count, self->rtp_packet_count,
               self->keyframe_count, self->rtp_seq_gaps);

  return GST_PAD_PROBE_OK;
}

GstPadProbeCallback
gnalk_quality_controller_get_rtp_probe (GnalkQualityController *self)
{
  g_return_val_if_fail (GNALK_IS_QUALITY_CONTROLLER (self), NULL);
  (void)self;  /* Probe uses user_data */
  return rtp_video_probe_cb;
}

GstPadProbeCallback
gnalk_quality_controller_get_depay_probe (GnalkQualityController *self)
{
  g_return_val_if_fail (GNALK_IS_QUALITY_CONTROLLER (self), NULL);
  (void)self;  /* Probe uses user_data */
  return depay_src_probe_cb;
}

#endif /* HAVE_GSTREAMER */

void
gnalk_quality_controller_start_bitrate_timer (GnalkQualityController *self)
{
  g_return_if_fail (GNALK_IS_QUALITY_CONTROLLER (self));

  if (self->bitrate_timer_id > 0)
    return;

#ifdef HAVE_GSTREAMER_WEBRTC
  self->current_bitrate = BITRATE_START;
  self->last_packets_sent = 0;
  self->last_packets_lost = 0;
  self->bitrate_timer_id = g_timeout_add (BITRATE_INTERVAL, bitrate_timer_callback, self);
  g_message ("QC: bitrate timer started, initial %u kbps", BITRATE_START / 1000);
#endif
}

void
gnalk_quality_controller_stop_bitrate_timer (GnalkQualityController *self)
{
  g_return_if_fail (GNALK_IS_QUALITY_CONTROLLER (self));

  if (self->bitrate_timer_id > 0)
    {
      g_source_remove (self->bitrate_timer_id);
      self->bitrate_timer_id = 0;
    }
}

/* Helper to send PLI request using correct GStreamer API.
 * Tries gst_element_send_event on webrtcbin first (preferred),
 * falls back to gst_pad_push_event on pli_pad. */
static gboolean
send_pli_request (GnalkQualityController *self)
{
#ifdef HAVE_GSTREAMER
  gboolean sent = FALSE;

  self->pli_request_count++;

  /* Try sending via webrtcbin first - this is the correct way as webrtcbin
   * knows how to route the event through the RTP stack to generate PLI. */
  if (self->webrtcbin)
    {
      GstEvent *force_key = gst_video_event_new_upstream_force_key_unit (
          GST_CLOCK_TIME_NONE, TRUE, 0);
      sent = gst_element_send_event (self->webrtcbin, force_key);
      if (sent)
        {
          g_message ("QC PLI #%u: sent via webrtcbin", self->pli_request_count);
          return TRUE;
        }
    }

  /* Fallback to pad push if webrtcbin method failed */
  if (self->pli_pad)
    {
      GstEvent *force_key = gst_video_event_new_upstream_force_key_unit (
          GST_CLOCK_TIME_NONE, TRUE, 0);
      sent = gst_pad_push_event (self->pli_pad, force_key);
      g_message ("QC PLI #%u: sent via pad push, result=%d",
                 self->pli_request_count, sent);
      return sent;
    }

  g_message ("QC PLI #%u: no webrtcbin or pli_pad available",
             self->pli_request_count);
  return FALSE;
#else
  return FALSE;
#endif
}

/* PLI timer callbacks */
static gboolean
periodic_pli_request_cb (gpointer user_data)
{
  GnalkQualityController *self = GNALK_QUALITY_CONTROLLER (user_data);

  if (self->got_decoded_video)
    {
      g_message ("QC PLI timer: got video, stopping");
      self->pli_timer_id = 0;
      return G_SOURCE_REMOVE;
    }

  send_pli_request (self);

  return G_SOURCE_CONTINUE;
}

static gboolean
continuous_pli_request_cb (gpointer user_data)
{
  GnalkQualityController *self = GNALK_QUALITY_CONTROLLER (user_data);

#ifdef HAVE_GSTREAMER
  if (!self->continuous_pli || (!self->webrtcbin && !self->pli_pad))
    {
      g_message ("QC Continuous PLI: timer STOPPED (continuous=%d, webrtcbin=%p, pad=%p)",
                 self->continuous_pli, (void *)self->webrtcbin, (void *)self->pli_pad);
      self->continuous_pli_timer_id = 0;
      return G_SOURCE_REMOVE;
    }

  /* Check for congestion */
  if (self->depay_frame_count > 50)
    {
      guint keyframe_pct = (self->keyframe_count * 100) / self->depay_frame_count;
      guint loss_pct = (self->rtp_seq_gaps * 100) /
        (self->rtp_packet_count > 0 ? self->rtp_packet_count : 1);

      if (keyframe_pct > 30 || loss_pct > 5)
        {
          /* Check if keyframes stalled */
          if (self->keyframe_count == self->last_keyframe_count)
            {
              self->keyframe_stall_cycles++;
              if (self->keyframe_stall_cycles >= 2)
                {
                  g_message ("QC: RECOVERY PLI needed, stalled %u cycles",
                             self->keyframe_stall_cycles);
                  self->keyframe_stall_cycles = 0;
                  goto send_pli;
                }
            }
          else
            {
              self->keyframe_stall_cycles = 0;
            }
          self->last_keyframe_count = self->keyframe_count;

          g_message ("QC: CONGESTION - keyframes=%u%%, loss=%u%%, stall=%u - NOT sending PLI",
                     keyframe_pct, loss_pct, self->keyframe_stall_cycles);

          /* Emit quality degraded signal */
          g_signal_emit (self, signals[SIGNAL_QUALITY_DEGRADED], 0, "congestion detected");

          return G_SOURCE_CONTINUE;
        }
    }

send_pli:
  send_pli_request (self);
#endif

  return G_SOURCE_CONTINUE;
}

void
gnalk_quality_controller_start_pli_timer (GnalkQualityController *self)
{
  g_return_if_fail (GNALK_IS_QUALITY_CONTROLLER (self));

  if (self->pli_timer_id > 0)
    return;

  self->got_decoded_video = FALSE;
  self->decoded_frame_count = 0;
  self->pli_timer_id = g_timeout_add (PLI_INITIAL_INTERVAL, periodic_pli_request_cb, self);
  g_message ("QC: PLI timer started");
}

void
gnalk_quality_controller_stop_pli_timer (GnalkQualityController *self)
{
  g_return_if_fail (GNALK_IS_QUALITY_CONTROLLER (self));

  if (self->pli_timer_id > 0)
    {
      g_source_remove (self->pli_timer_id);
      self->pli_timer_id = 0;
    }
}

void
gnalk_quality_controller_enable_continuous_pli (GnalkQualityController *self,
                                                  gboolean                 enable,
                                                  guint                    interval_ms)
{
  g_return_if_fail (GNALK_IS_QUALITY_CONTROLLER (self));

  self->continuous_pli = enable;
  self->continuous_pli_interval = interval_ms;

  if (self->continuous_pli_timer_id)
    {
      g_source_remove (self->continuous_pli_timer_id);
      self->continuous_pli_timer_id = 0;
    }

  if (enable && interval_ms > 0)
    {
      self->continuous_pli_timer_id = g_timeout_add (interval_ms, continuous_pli_request_cb, self);
      g_message ("QC: Continuous PLI enabled, interval %u ms", interval_ms);
    }
}

gboolean
gnalk_quality_controller_send_pli (GnalkQualityController *self)
{
  g_return_val_if_fail (GNALK_IS_QUALITY_CONTROLLER (self), FALSE);

  return send_pli_request (self);
}

void
gnalk_quality_controller_set_video_received (GnalkQualityController *self,
                                               gboolean                 received)
{
  g_return_if_fail (GNALK_IS_QUALITY_CONTROLLER (self));
  self->got_decoded_video = received;

  if (received)
    g_message ("QC: video received flag set");
}

void
gnalk_quality_controller_reset_stats (GnalkQualityController *self)
{
  g_return_if_fail (GNALK_IS_QUALITY_CONTROLLER (self));

  self->rtp_packet_count = 0;
  self->last_rtp_seq = 0;
  self->last_rtp_ssrc = 0;
  self->rtp_seq_gaps = 0;
  self->keyframe_count = 0;
  self->depay_frame_count = 0;
  self->last_keyframe_count = 0;
  self->keyframe_stall_cycles = 0;
  self->decoded_frame_count = 0;
  self->got_decoded_video = FALSE;
  self->pli_request_count = 0;

  g_message ("QC: stats reset");
}

guint
gnalk_quality_controller_get_keyframe_count (GnalkQualityController *self)
{
  g_return_val_if_fail (GNALK_IS_QUALITY_CONTROLLER (self), 0);
  return self->keyframe_count;
}

guint
gnalk_quality_controller_get_packet_loss_count (GnalkQualityController *self)
{
  g_return_val_if_fail (GNALK_IS_QUALITY_CONTROLLER (self), 0);
  return self->rtp_seq_gaps;
}

guint
gnalk_quality_controller_get_current_bitrate (GnalkQualityController *self)
{
  g_return_val_if_fail (GNALK_IS_QUALITY_CONTROLLER (self), 0);
  return self->current_bitrate;
}
