#include "gnalk-webrtc.h"
#include "gnalk-log.h"
#include "gnalk-sdp-utils.h"
#include "gnalk-data-channel.h"
#include "gnalk-media-pipeline.h"
#include "gnalk-quality-controller.h"
#include "gnalk-receive-pipeline.h"

#include <errno.h>
#include <unistd.h>

#ifdef HAVE_GSTREAMER
#include <gst/video/video.h>
#include <gst/app/gstappsink.h>
#include <gst/rtp/rtp.h>
#endif  /* HAVE_GSTREAMER */

/* Structure for buffered ICE candidates (defined outside #ifdef for init) */
typedef struct {
  char *candidate;
  int   sdp_mline_index;
  char *sdp_mid;
} PendingIceCandidate;

static void
pending_ice_candidate_free (gpointer data)
{
  PendingIceCandidate *candidate = data;
  g_free (candidate->candidate);
  g_free (candidate->sdp_mid);
  g_free (candidate);
}

struct _GnalkWebrtc
{
  GObject parent_instance;

#ifdef HAVE_GSTREAMER
  GstElement *pipeline;
  GstElement *webrtcbin;
  GstElement *video_sink;
  GstElement *local_video_sink;   /* For local camera preview */
  GstElement *remote_video_sink;  /* For remote video */
  GstElement *recv_depay;         /* For PLI requests */
  GstElement *video_encoder;      /* For adaptive bitrate control */
  GnalkMediaPipeline *media_pipeline;      /* Media source pipeline manager */
  GnalkDataChannel *data_channel_manager;  /* Manages WebRTC data channel for status messages */
  GnalkQualityController *quality_controller;  /* Video quality monitoring and adaptation */
  GnalkReceivePipeline *receive_pipeline;  /* Handles incoming media streams */

  GstElement *audio_volume;       /* Volume element for muting audio */

  /* Screen share diagnostic counters (kept here as screen share has separate pipeline) */
  guint       screen_encoder_frame_count;  /* Per-instance counter for screen share encoder */
  guint       screen_rtp_packet_count;     /* Per-instance counter for screen share RTP packets */
  guint       pipewiresrc_frame_count;     /* Per-instance counter for pipewiresrc output */
  guint       screen_queue_frame_count;    /* Per-instance counter for queue->videoconvert */
  guint       screen_capsfilter_frame_count; /* Per-instance counter for capsfilter->encoder */
  guint       screen_encoder_output_count;   /* Per-instance counter for encoder output */
  guint       screen_keyframe_timer_id;      /* Periodic keyframe timer for screen share */
  GstElement *screen_capsfilter;           /* Capsfilter for dynamic resolution scaling */
  gboolean    screen_caps_configured;      /* TRUE after first frame sets the caps */
#else
  gpointer    pipeline;
  gpointer    webrtcbin;
  gpointer    video_sink;
  gpointer    local_video_sink;
  gpointer    remote_video_sink;
  gpointer    recv_depay;
  gpointer    video_encoder;
  GnalkMediaPipeline *media_pipeline;
  GnalkDataChannel *data_channel_manager;
  GnalkQualityController *quality_controller;
  GnalkReceivePipeline *receive_pipeline;
  gpointer    audio_volume;
#endif
  char       *peer_id;
  char       *stun_url;
  char       *turn_url;
  char       *turn_username;
  char       *turn_password;
  gboolean    video_enabled;
  gboolean    audio_enabled;
  gboolean    receive_only;  /* If TRUE, don't send local media */
  gboolean    is_screen_share;  /* If TRUE, this is a screen share publisher */
  guint32     pipewire_node_id;  /* PipeWire node ID for screen capture */
  int         pipewire_fd;       /* PipeWire file descriptor for screen capture */
  GnalkCameraPosition camera_position;  /* Front or back camera */

  /* Call state machine */
  GnalkCallState call_state;

  /* Perfect negotiation pattern fields */
  gboolean    making_offer;   /* TRUE while creating/sending offer */
  gboolean    ignore_offer;   /* TRUE when impolite peer ignores collision */
  gboolean    polite;         /* TRUE if this peer is polite (yields to collisions) */

  /* ICE candidate buffering.
   * Remote candidates that arrive before webrtcbin can accept them are buffered:
   * in receive-only mode until the answer is created (local description set),
   * in sender mode until the remote answer is set. */
  GPtrArray  *pending_ice_candidates;  /* Buffered candidates before we can add them */
  gboolean    answer_created;          /* TRUE after answer is created and sent */
  gboolean    remote_answer_set;       /* TRUE after remote answer set (sender mode) */

  GnalkWebrtcSignalCallback signal_callback;
  gpointer                   signal_callback_data;
  GDestroyNotify             signal_callback_notify;

  GnalkWebrtcRemoteVideoCallback remote_video_callback;
  gpointer                        remote_video_callback_data;
  GDestroyNotify                  remote_video_callback_notify;

  GnalkWebrtcLocalVideoCallback local_video_callback;
  gpointer                       local_video_callback_data;

  GnalkWebrtcConnectionStateCallback connection_state_callback;
  gpointer                            connection_state_callback_data;
  GDestroyNotify                      connection_state_callback_notify;

  GnalkWebrtcSpeakingCallback speaking_callback;
  gpointer                     speaking_callback_data;
  GDestroyNotify               speaking_callback_notify;

  GnalkWebrtcCallStateCallback call_state_callback;
  gpointer                      call_state_callback_data;
};

G_DEFINE_FINAL_TYPE (GnalkWebrtc, gnalk_webrtc, G_TYPE_OBJECT)

/* Forward declarations */
static void start_screen_keyframe_timer (GnalkWebrtc *self);
static void stop_screen_keyframe_timer (GnalkWebrtc *self);

static void
gnalk_webrtc_finalize (GObject *object)
{
  GnalkWebrtc *self = GNALK_WEBRTC (object);

  gnalk_webrtc_end_call (self);

  g_free (self->peer_id);
  g_free (self->stun_url);
  g_free (self->turn_url);
  g_free (self->turn_username);
  g_free (self->turn_password);

  /* Free pending ICE candidates */
  g_clear_pointer (&self->pending_ice_candidates, g_ptr_array_unref);

  G_OBJECT_CLASS (gnalk_webrtc_parent_class)->finalize (object);
}

static void
gnalk_webrtc_class_init (GnalkWebrtcClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_webrtc_finalize;
}

static void
gnalk_webrtc_init (GnalkWebrtc *self)
{
  self->video_enabled = TRUE;
  self->audio_enabled = TRUE;
  self->camera_position = GNALK_CAMERA_FRONT;  /* Default to front camera */
  self->stun_url = g_strdup ("stun://stun.l.google.com:19302");
  self->call_state = GNALK_CALL_STATE_IDLE;
  self->is_screen_share = FALSE;
  self->pipewire_node_id = 0;
  self->pipewire_fd = -1;

  /* Initialize ICE candidate buffer */
  self->pending_ice_candidates = g_ptr_array_new_with_free_func (pending_ice_candidate_free);

  /* Default to polite peer (yields on offer collision) */
  self->polite = TRUE;

#ifdef HAVE_GSTREAMER
  /* gst_init() is process-global and only needs to run once. It ideally belongs
   * in application startup, but a GnalkWebrtc is created per participant, so guard
   * it with g_once to ensure it runs exactly once per process. */
  static gsize gst_init_once = 0;
  if (g_once_init_enter (&gst_init_once))
    {
      gst_init (NULL, NULL);
      g_once_init_leave (&gst_init_once, 1);
    }
#endif
}

GnalkWebrtc *
gnalk_webrtc_new (void)
{
  return g_object_new (GNALK_TYPE_WEBRTC, NULL);
}

void
gnalk_webrtc_set_stun_server (GnalkWebrtc *self,
                                const char   *stun_url)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));

  g_free (self->stun_url);
  self->stun_url = g_strdup (stun_url);
}

void
gnalk_webrtc_set_turn_server (GnalkWebrtc *self,
                                const char   *turn_url,
                                const char   *username,
                                const char   *password)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));

  g_free (self->turn_url);
  g_free (self->turn_username);
  g_free (self->turn_password);

  self->turn_url = g_strdup (turn_url);
  self->turn_username = g_strdup (username);
  self->turn_password = g_strdup (password);
}

#ifdef HAVE_GSTREAMER_WEBRTC

/* Forward declarations */
static void set_call_state (GnalkWebrtc *self, GnalkCallState state);
static void flush_pending_ice_candidates (GnalkWebrtc *self);

/* Set call state and notify callback */
static void
set_call_state (GnalkWebrtc *self, GnalkCallState state)
{
  if (self->call_state == state)
    return;

  const char *state_names[] = { "IDLE", "CONNECTING", "NEGOTIATING", "CONNECTED",
                                "RECONNECTING", "FAILED", "CLOSED" };
  g_message ("Call state: %s -> %s",
             state_names[self->call_state], state_names[state]);

  self->call_state = state;

  if (self->call_state_callback)
    self->call_state_callback (self, state, self->call_state_callback_data);
}

/* Flush buffered ICE candidates after answer is created */
static void
flush_pending_ice_candidates (GnalkWebrtc *self)
{
  if (!self->pending_ice_candidates || !self->webrtcbin)
    return;

  g_message ("Flushing %u buffered ICE candidates", self->pending_ice_candidates->len);

  for (guint i = 0; i < self->pending_ice_candidates->len; i++)
    {
      PendingIceCandidate *candidate = g_ptr_array_index (self->pending_ice_candidates, i);
      g_signal_emit_by_name (self->webrtcbin, "add-ice-candidate",
                             candidate->sdp_mline_index, candidate->candidate);
    }

  g_ptr_array_set_size (self->pending_ice_candidates, 0);
}

/* Signal callback marshaling to main thread.
 * GStreamer callbacks (like on-ice-candidate) run in GStreamer threads,
 * but the signal callback may use libsoup which is not thread-safe. */
typedef struct {
  GnalkWebrtc *webrtc;
  char *type;
  JsonNode *data;
} SignalCallbackData;

static gboolean
signal_callback_in_main_thread (gpointer user_data)
{
  SignalCallbackData *cb_data = user_data;

  /* Check if webrtc object is still valid (weak ref cleared on destroy) */
  if (cb_data->webrtc && cb_data->webrtc->signal_callback)
    {
      cb_data->webrtc->signal_callback (cb_data->webrtc,
                                         cb_data->type,
                                         cb_data->data,
                                         cb_data->webrtc->signal_callback_data);
      g_object_remove_weak_pointer (G_OBJECT (cb_data->webrtc), (gpointer *)&cb_data->webrtc);
    }

  g_free (cb_data->type);
  json_node_unref (cb_data->data);
  g_free (cb_data);
  return G_SOURCE_REMOVE;
}

/* Marshal signal callback to main thread */
static void
emit_signal_callback (GnalkWebrtc *self,
                      const char   *type,
                      JsonNode     *data)
{
  if (!self->signal_callback)
    return;

  SignalCallbackData *cb_data = g_new (SignalCallbackData, 1);
  cb_data->webrtc = self;
  cb_data->type = g_strdup (type);
  cb_data->data = json_node_ref (data);

  /* Add weak reference to detect if webrtc is destroyed before callback runs */
  g_object_add_weak_pointer (G_OBJECT (self), (gpointer *)&cb_data->webrtc);
  g_idle_add (signal_callback_in_main_thread, cb_data);
}

static void
send_ice_candidate (GnalkWebrtc *self,
                    guint         mline_index,
                    const char   *candidate)
{
  if (!self->signal_callback)
    return;

  g_autoptr(JsonBuilder) builder = json_builder_new ();

  /* Get the actual mid from the local SDP description.
   * For offers we create, this will be "video0", "audio1", etc.
   * For answers to remote offers, this will match the remote's mids. */
  g_autofree char *sdp_mid = NULL;
  GstWebRTCSessionDescription *local_desc = NULL;
  g_object_get (self->webrtcbin, "local-description", &local_desc, NULL);

  if (local_desc && local_desc->sdp && mline_index < local_desc->sdp->medias->len)
    {
      GstSDPMedia *media = &g_array_index (local_desc->sdp->medias, GstSDPMedia, mline_index);
      const char *mid = gst_sdp_media_get_attribute_val (media, "mid");
      if (mid)
        sdp_mid = g_strdup (mid);
    }

  if (local_desc)
    gst_webrtc_session_description_free (local_desc);

  /* Fallback to default naming if we couldn't get the mid */
  if (!sdp_mid)
    {
      if (mline_index == 0)
        sdp_mid = g_strdup ("video0");
      else if (mline_index == 1)
        sdp_mid = g_strdup ("audio1");
      else
        sdp_mid = g_strdup_printf ("data%u", mline_index);
    }

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "candidate");
  json_builder_add_string_value (builder, candidate);
  json_builder_set_member_name (builder, "sdpMLineIndex");
  json_builder_add_int_value (builder, mline_index);
  json_builder_set_member_name (builder, "sdpMid");
  json_builder_add_string_value (builder, sdp_mid);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  emit_signal_callback (self, "candidate", root);
}

static void
on_ice_candidate (GstElement *webrtc,
                  guint       mline_index,
                  gchar      *candidate,
                  gpointer    user_data)
{
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);

  /* Send all candidates to allow ICE to negotiate the best path.
   * This is important for same-network connections where host candidates
   * (local IPs) provide better quality than relay candidates (TURN). */
  send_ice_candidate (self, mline_index, candidate);
}

static void
send_sdp (GnalkWebrtc       *self,
          const char         *type,
          GstWebRTCSessionDescription *desc)
{
  if (!self->signal_callback)
    return;

  g_autofree char *sdp_str = gst_sdp_message_as_text (desc->sdp);

  g_autoptr(JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, type);
  json_builder_set_member_name (builder, "sdp");
  json_builder_add_string_value (builder, sdp_str);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  emit_signal_callback (self, type, root);
}

static void
on_offer_created (GstPromise *promise,
                  gpointer    user_data)
{
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);
  GstWebRTCSessionDescription *offer = NULL;
  const GstStructure *reply;

  g_message ("on_offer_created: called");

  reply = gst_promise_get_reply (promise);
  gst_structure_get (reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);

  if (!offer)
    {
      g_warning ("Failed to get offer from promise");
      self->making_offer = FALSE;  /* Clear flag on failure */
      gst_promise_unref (promise);
      return;
    }

  g_message ("on_offer_created: setting local description and sending offer");
  g_signal_emit_by_name (self->webrtcbin, "set-local-description", offer, NULL);

  send_sdp (self, "offer", offer);

  /* Update state machine */
  set_call_state (self, GNALK_CALL_STATE_NEGOTIATING);
  self->making_offer = FALSE;

  gst_webrtc_session_description_free (offer);
  gst_promise_unref (promise);
}

static void
on_answer_created (GstPromise *promise,
                   gpointer    user_data)
{
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);
  GstWebRTCSessionDescription *answer = NULL;
  const GstStructure *reply;

  reply = gst_promise_get_reply (promise);
  gst_structure_get (reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);

  if (!answer)
    {
      g_warning ("Failed to get answer from promise");
      gst_promise_unref (promise);
      return;
    }

  /* Ensure BUNDLE group is present - GStreamer sometimes omits it in receive-only answers */
  gnalk_sdp_ensure_bundle_group (answer->sdp);

  g_autofree char *sdp_str = gst_sdp_message_as_text (answer->sdp);
  g_message ("Created SDP answer:\n%s", sdp_str);

  g_signal_emit_by_name (self->webrtcbin, "set-local-description", answer, NULL);
  g_message ("on_answer_created: local description set");

  /* Now that both remote and local descriptions are set with bundle info,
   * set pipeline to PLAYING so webrtcbin creates transports correctly */
  if (self->receive_only && self->pipeline)
    {
      GstState current_state;
      gst_element_get_state (self->pipeline, &current_state, NULL, 0);
      if (current_state != GST_STATE_PLAYING)
        {
          g_message ("on_answer_created: setting pipeline to PLAYING");
          gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
        }
    }

  send_sdp (self, "answer", answer);

  /* Mark answer as created and flush any buffered ICE candidates */
  self->answer_created = TRUE;
  flush_pending_ice_candidates (self);

  /* Update state machine */
  set_call_state (self, GNALK_CALL_STATE_NEGOTIATING);

  gst_webrtc_session_description_free (answer);
  gst_promise_unref (promise);
}

static void
on_ice_connection_state_notify (GstElement *webrtc,
                                 GParamSpec *pspec,
                                 gpointer    user_data)
{
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);
  GstWebRTCICEConnectionState state;
  g_object_get (webrtc, "ice-connection-state", &state, NULL);

  (void)pspec;

  const char *state_name = "unknown";
  switch (state)
    {
    case GST_WEBRTC_ICE_CONNECTION_STATE_NEW: state_name = "new"; break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_CHECKING: state_name = "checking"; break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED: state_name = "connected"; break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED: state_name = "completed"; break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_FAILED: state_name = "FAILED"; break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED: state_name = "disconnected"; break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_CLOSED: state_name = "closed"; break;
    }
  g_message ("ICE connection state: %s (webrtc=%p, is_screen=%d)",
             state_name, (void *)self, self->is_screen_share);

  /* For screen share: when ICE connects, toggle pipewiresrc to force a new frame.
   * keepalive-time doesn't work reliably on all hardware, so we force it. */
  if ((state == GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED ||
       state == GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED) &&
      self->is_screen_share && self->pipeline)
    {
      GstElement *pipewiresrc = gst_bin_get_by_name (GST_BIN (self->pipeline), "screensrc");
      if (pipewiresrc)
        {
          g_message ("Screen share ICE connected - toggling pipewiresrc to force new frame");
          /* Toggle PAUSED -> PLAYING to trigger pipewiresrc to produce a new frame */
          gst_element_set_state (pipewiresrc, GST_STATE_PAUSED);
          gst_element_get_state (pipewiresrc, NULL, NULL, GST_MSECOND * 100);
          gst_element_set_state (pipewiresrc, GST_STATE_PLAYING);
          gst_object_unref (pipewiresrc);
        }

      /* Force keyframe from encoder so remote gets a fresh IDR after ICE connects */
      GstElement *encoder = gst_bin_get_by_name (GST_BIN (self->pipeline), "screenenc");
      if (encoder)
        {
          GstEvent *force_key = gst_video_event_new_upstream_force_key_unit (
              GST_CLOCK_TIME_NONE, TRUE, 0);
          gboolean sent = gst_element_send_event (encoder, force_key);
          g_message ("Screen share ICE connected - forced keyframe, result=%d", sent);
          gst_object_unref (encoder);
        }
    }

  /* Log ICE gathering state too */
  GstWebRTCICEGatheringState gather_state;
  g_object_get (webrtc, "ice-gathering-state", &gather_state, NULL);
  const char *gather_name = "unknown";
  switch (gather_state)
    {
    case GST_WEBRTC_ICE_GATHERING_STATE_NEW: gather_name = "new"; break;
    case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING: gather_name = "gathering"; break;
    case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE: gather_name = "complete"; break;
    }
  g_message ("ICE gathering state: %s", gather_name);
}

/* Runs on the main thread (via connection_state_in_main_thread). */
static void
run_connection_state_handler (GnalkWebrtc                  *self,
                              GstWebRTCPeerConnectionState  state)
{
  gboolean connected = FALSE;

  const char *state_name = "unknown";
  gboolean notify_callback = FALSE;  /* Only notify for meaningful transitions */

  switch (state)
    {
    case GST_WEBRTC_PEER_CONNECTION_STATE_NEW:
      state_name = "new";
      /* Don't notify - initial state, not a real disconnection */
      break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTING:
      state_name = "connecting";
      set_call_state (self, GNALK_CALL_STATE_CONNECTING);
      /* Don't notify - still setting up, not a real disconnection */
      break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED:
      state_name = "connected";
      connected = TRUE;
      notify_callback = TRUE;  /* Notify: connection established */
      set_call_state (self, GNALK_CALL_STATE_CONNECTED);
      if (self->data_channel_manager)
        gnalk_data_channel_start_periodic_status (self->data_channel_manager,
                                                   self->video_enabled,
                                                   self->audio_enabled);
      if (self->quality_controller)
        gnalk_quality_controller_start_bitrate_timer (self->quality_controller);
      /* For screen share: force keyframe now that DTLS is ready.
       * ICE-connected keyframe might arrive before DTLS completes. */
      if (self->is_screen_share && self->pipeline)
        {
          GstElement *encoder = gst_bin_get_by_name (GST_BIN (self->pipeline), "screenenc");
          if (encoder)
            {
              GstEvent *force_key = gst_video_event_new_upstream_force_key_unit (
                  GST_CLOCK_TIME_NONE, TRUE, 0);
              gboolean sent = gst_element_send_event (encoder, force_key);
              g_message ("Screen share peer connected (DTLS ready) - forced keyframe, result=%d", sent);
              gst_object_unref (encoder);
            }
          /* Start periodic keyframe timer */
          start_screen_keyframe_timer (self);
        }
      break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED:
      state_name = "disconnected";
      notify_callback = TRUE;  /* Notify: connection lost */
      set_call_state (self, GNALK_CALL_STATE_RECONNECTING);
      if (self->data_channel_manager)
        gnalk_data_channel_stop_periodic_status (self->data_channel_manager);
      if (self->quality_controller)
        gnalk_quality_controller_stop_bitrate_timer (self->quality_controller);
      break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_FAILED:
      state_name = "FAILED";
      notify_callback = TRUE;  /* Notify: connection failed */
      set_call_state (self, GNALK_CALL_STATE_FAILED);
      if (self->quality_controller)
        gnalk_quality_controller_stop_bitrate_timer (self->quality_controller);
      if (self->data_channel_manager)
        gnalk_data_channel_stop_periodic_status (self->data_channel_manager);
      break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED:
      state_name = "closed";
      notify_callback = TRUE;  /* Notify: connection closed */
      set_call_state (self, GNALK_CALL_STATE_CLOSED);
      if (self->data_channel_manager)
        gnalk_data_channel_stop_periodic_status (self->data_channel_manager);
      if (self->quality_controller)
        gnalk_quality_controller_stop_bitrate_timer (self->quality_controller);
      break;
    }
  g_message ("WebRTC peer connection state: %s (webrtc=%p, is_screen=%d)",
             state_name, (void *)self, self->is_screen_share);

  if (self->connection_state_callback && notify_callback)
    self->connection_state_callback (self, connected, self->connection_state_callback_data);
}

/* Marshalling wrapper: GstWebRTC fires notify::connection-state on webrtcbin's
 * internal thread, but run_connection_state_handler touches GTK (via the
 * consumer's callback), main-loop timers, and libsoup — all main-thread-only. */
typedef struct {
  GWeakRef                     webrtc_ref;
  GstWebRTCPeerConnectionState state;
} ConnStateData;

static gboolean
connection_state_in_main_thread (gpointer user_data)
{
  ConnStateData *d = user_data;
  GnalkWebrtc *self = g_weak_ref_get (&d->webrtc_ref);
  if (self)
    {
      run_connection_state_handler (self, d->state);
      g_object_unref (self);
    }
  g_weak_ref_clear (&d->webrtc_ref);
  g_free (d);
  return G_SOURCE_REMOVE;
}

static void
on_connection_state_notify (GstElement *webrtc,
                             GParamSpec *pspec,
                             gpointer    user_data)
{
  (void) pspec;
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);
  GstWebRTCPeerConnectionState state;

  g_object_get (webrtc, "connection-state", &state, NULL);

  ConnStateData *d = g_new0 (ConnStateData, 1);
  g_weak_ref_init (&d->webrtc_ref, self);
  d->state = state;
  g_idle_add (connection_state_in_main_thread, d);
}

/* Callback when new transceiver is created - enable NACK for retransmission */
static void
on_new_transceiver (GstElement               *webrtc,
                    GstWebRTCRTPTransceiver   *transceiver,
                    gpointer                   user_data)
{
  (void)webrtc;
  (void)user_data;

  GstWebRTCRTPTransceiverDirection direction;
  g_object_get (transceiver, "direction", &direction, NULL);
  g_message ("on_new_transceiver: direction=%d", direction);

  /* Enable NACK for packet loss recovery */
  g_object_set (transceiver, "do-nack", TRUE, NULL);
  g_message ("on_new_transceiver: enabled do-nack");
}

/* Callback for GnalkDataChannel speaking-changed signal */
static void
on_data_channel_speaking_changed (GnalkDataChannel *data_channel,
                                  gboolean           speaking,
                                  gpointer           user_data)
{
  (void)data_channel;
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);

  if (self->speaking_callback)
    self->speaking_callback (self, speaking, self->speaking_callback_data);
}

/* Callback when remote peer creates a data channel */
static void
on_remote_data_channel (GstElement *webrtc,
                        GObject    *channel,
                        gpointer    user_data)
{
  (void)webrtc;
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);

  /* Pass remote channel to data channel manager for message handling */
  if (self->data_channel_manager)
    gnalk_data_channel_set_remote_channel (self->data_channel_manager, channel);
}

/* Jitterbuffer diagnostics - callback when a new jitterbuffer is created */
static void
on_new_jitterbuffer (GstElement *rtpbin,
                     GstElement *jitterbuffer,
                     guint       session,
                     guint       ssrc,
                     gpointer    user_data)
{
  (void)rtpbin;
  (void)user_data;

  g_message ("Jitterbuffer created: session=%u ssrc=0x%08x jitterbuffer=%p",
             session, ssrc, (void *)jitterbuffer);

  /* Log current jitterbuffer properties */
  gint latency = 0;
  gboolean do_lost = FALSE;
  gboolean do_retransmission = FALSE;
  gint max_dropout_time = 0;
  gint max_misorder_time = 0;

  g_object_get (jitterbuffer,
                "latency", &latency,
                "do-lost", &do_lost,
                "do-retransmission", &do_retransmission,
                "max-dropout-time", &max_dropout_time,
                "max-misorder-time", &max_misorder_time,
                NULL);

  g_message ("Jitterbuffer[0x%08x] config: latency=%d do-lost=%d do-retransmission=%d "
             "max-dropout=%d max-misorder=%d",
             ssrc, latency, do_lost, do_retransmission,
             max_dropout_time, max_misorder_time);

  /* Enable do-lost to get GstRTPPacketLost events downstream.
   * Disable do-retransmission since we stripped RTX from SDP -
   * otherwise jitterbuffer may hold packets waiting for retransmits that won't come. */
  g_object_set (jitterbuffer,
                "do-lost", TRUE,
                "do-retransmission", FALSE,
                NULL);
  g_message ("Jitterbuffer[0x%08x] configured: do-lost=TRUE, do-retransmission=FALSE", ssrc);
}

/* Connect to rtpbin inside webrtcbin for jitterbuffer diagnostics */
static void
connect_rtpbin_signals (GstElement *webrtcbin)
{
  /* webrtcbin contains an internal rtpbin element.
   * We need to find it by iterating children. */
  GstIterator *iter = gst_bin_iterate_elements (GST_BIN (webrtcbin));
  GValue item = G_VALUE_INIT;
  gboolean done = FALSE;

  while (!done)
    {
      switch (gst_iterator_next (iter, &item))
        {
        case GST_ITERATOR_OK:
          {
            GstElement *element = g_value_get_object (&item);
            const gchar *name = gst_element_get_name (element);
            g_message ("webrtcbin child: %s (type: %s)",
                       name, G_OBJECT_TYPE_NAME (element));

            /* rtpbin is named "rtpbin" inside webrtcbin */
            if (g_str_has_prefix (name, "rtpbin"))
              {
                g_message ("Found rtpbin, connecting to new-jitterbuffer signal");
                g_signal_connect (element, "new-jitterbuffer",
                                  G_CALLBACK (on_new_jitterbuffer), NULL);
              }
            g_free ((gchar *)name);
            g_value_reset (&item);
          }
          break;
        case GST_ITERATOR_RESYNC:
          gst_iterator_resync (iter);
          break;
        case GST_ITERATOR_ERROR:
        case GST_ITERATOR_DONE:
          done = TRUE;
          break;
        }
    }
  g_value_unset (&item);
  gst_iterator_free (iter);
}

static void
on_negotiation_needed (GstElement *webrtc,
                        gpointer    user_data)
{
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);
  GstPromise *promise;

  g_message ("on_negotiation_needed: receive_only=%d, signal_callback=%p",
             self->receive_only, (void*)self->signal_callback);

  /* In receive-only mode, we should NOT create offers - we only respond to offers */
  if (self->receive_only)
    return;

  /* Perfect negotiation: set flag before creating offer to detect collisions */
  self->making_offer = TRUE;
  set_call_state (self, GNALK_CALL_STATE_CONNECTING);

  g_message ("on_negotiation_needed: creating offer");
  promise = gst_promise_new_with_change_func (on_offer_created, self, NULL);
  g_signal_emit_by_name (webrtc, "create-offer", NULL, promise);
}

/* Handler for receive pipeline's remote-video-frame signal */
static void
on_receive_pipeline_remote_video_frame (GnalkReceivePipeline *pipeline,
                                        GdkTexture            *texture,
                                        gpointer               user_data)
{
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);
  (void)pipeline;

  if (self->remote_video_callback)
    {
      self->remote_video_callback (self, texture, self->remote_video_callback_data);
    }
}

/* Handler for media pipeline's local-video-frame signal */
static void
on_media_pipeline_local_video_frame (GnalkMediaPipeline *pipeline,
                                     GdkTexture          *texture,
                                     gpointer             user_data)
{
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);
  (void)pipeline;  /* unused */

  if (self->local_video_callback)
    {
      self->local_video_callback (self, texture, self->local_video_callback_data);
    }
}

/* Delegate pad-added to receive pipeline */
static void
on_incoming_stream (GstElement *webrtc,
                    GstPad     *pad,
                    gpointer    user_data)
{
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);
  (void)webrtc;

  if (self->receive_pipeline)
    {
      gnalk_receive_pipeline_handle_pad_added (self->receive_pipeline, pad);

      /* Update references from receive pipeline */
      self->remote_video_sink = gnalk_receive_pipeline_get_video_sink (self->receive_pipeline);
      self->recv_depay = gnalk_receive_pipeline_get_video_depay (self->receive_pipeline);
    }
  else
    {
      g_warning ("on_incoming_stream: no receive pipeline configured");
    }
}

/* Bus message handler to catch pipeline errors */
static gboolean
on_bus_message (GstBus     *bus,
                GstMessage *msg,
                gpointer    user_data)
{
  switch (GST_MESSAGE_TYPE (msg))
    {
    case GST_MESSAGE_ERROR:
      {
        GError *err = NULL;
        gchar *debug = NULL;
        gst_message_parse_error (msg, &err, &debug);

        /* Ignore errors from secondary transports (transportreceivebin1, etc.)
         * when using BUNDLE - these transports are created but not used,
         * data flows through transportreceivebin0 only */
        const char *src_name = GST_OBJECT_NAME (msg->src);
        if (g_str_has_prefix (src_name, "nicesrc") && !g_str_has_suffix (src_name, "0"))
          {
            g_message ("Ignoring error from secondary transport %s (expected with BUNDLE)", src_name);
            g_error_free (err);
            g_free (debug);
            break;
          }
        if (g_str_has_prefix (src_name, "queue") && g_strstr_len (debug, -1, "transportreceivebin1"))
          {
            g_message ("Ignoring error from secondary transport queue (expected with BUNDLE)");
            g_error_free (err);
            g_free (debug);
            break;
          }

        g_warning ("WebRTC pipeline error from %s: %s (debug: %s)",
                   src_name, err->message, debug ? debug : "none");
        g_error_free (err);
        g_free (debug);
        break;
      }
    case GST_MESSAGE_WARNING:
      {
        GError *err = NULL;
        gchar *debug = NULL;
        gst_message_parse_warning (msg, &err, &debug);
        /* Filter out benign "not negotiated" warnings from videorate during camera switch */
        const gchar *src_name = GST_OBJECT_NAME (msg->src);
        if (!(g_str_has_prefix (src_name, "videorate") && g_strstr_len (err->message, -1, "not negotiated")))
          g_warning ("WebRTC pipeline warning from %s: %s", src_name, err->message);
        g_error_free (err);
        g_free (debug);
        break;
      }
    case GST_MESSAGE_STATE_CHANGED:
      {
        /* Only log webrtcbin state changes */
        if (GST_IS_ELEMENT (msg->src) && g_str_has_prefix (GST_OBJECT_NAME (msg->src), "webrtc"))
          {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
            g_message ("WebRTC state change: %s -> %s (pending: %s)",
                       gst_element_state_get_name (old_state),
                       gst_element_state_get_name (new_state),
                       gst_element_state_get_name (pending_state));
          }
        break;
      }
    default:
      break;
    }

  return TRUE;
}

static gboolean
create_pipeline (GnalkWebrtc *self)
{
  GstElement *pipeline, *webrtc;

  g_message ("WebRTC: Creating pipeline with %s camera",
             self->camera_position == GNALK_CAMERA_FRONT ? "front" : "back");

  pipeline = gst_pipeline_new ("call-pipeline");
  webrtc = gst_element_factory_make ("webrtcbin", "webrtc");

  if (!webrtc)
    {
      g_warning ("Failed to create webrtcbin element");
      gst_object_unref (pipeline);
      return FALSE;
    }

  g_object_set (webrtc, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, NULL);

  /* Set latency for proper RTP timestamp handling */
  g_object_set (webrtc, "latency", 200, NULL);  /* 200ms jitter buffer for packet reordering */

  if (self->stun_url)
    {
      g_autofree char *stun_uri = NULL;
      if (g_str_has_prefix (self->stun_url, "stun:"))
        stun_uri = g_strdup_printf ("stun://%s", self->stun_url + 5);
      else
        stun_uri = g_strdup (self->stun_url);
      g_object_set (webrtc, "stun-server", stun_uri, NULL);
    }

  if (self->turn_url && self->turn_username && self->turn_password)
    {
      g_autofree char *turn_uri = NULL;
      const char *server_part = self->turn_url;
      if (g_str_has_prefix (self->turn_url, "turn:"))
        server_part = self->turn_url + 5;
      else if (g_str_has_prefix (self->turn_url, "turns:"))
        server_part = self->turn_url + 6;

      g_autofree char *escaped_user = g_uri_escape_string (self->turn_username, NULL, TRUE);
      g_autofree char *escaped_pass = g_uri_escape_string (self->turn_password, NULL, TRUE);

      turn_uri = g_strdup_printf ("turn://%s:%s@%s",
                                   escaped_user, escaped_pass, server_part);
      gboolean ret = FALSE;
      g_signal_emit_by_name (webrtc, "add-turn-server", turn_uri, &ret);
      if (!ret)
        g_warning ("Failed to add TURN server");
    }

  gst_bin_add (GST_BIN (pipeline), webrtc);

  /* Set pipeline to READY state before creating data channel
   * (required by GStreamer webrtcbin) */
  GstStateChangeReturn ret = gst_element_set_state (pipeline, GST_STATE_READY);
  if (ret == GST_STATE_CHANGE_FAILURE)
    {
      g_warning ("Failed to set pipeline to READY state");
      gst_object_unref (pipeline);
      return FALSE;
    }

  /* Create data channel for status messages (videoOn/audioOn) - required for Android
   * IMPORTANT: Must be created after pipeline is in READY state */
  {
    GObject *data_channel = NULL;
    GstStructure *options = gst_structure_new ("options",
                                                "ordered", G_TYPE_BOOLEAN, TRUE,
                                                NULL);
    g_signal_emit_by_name (webrtc, "create-data-channel", "status", options, &data_channel);
    gst_structure_free (options);

    if (data_channel)
      {
        /* Create data channel manager and pass the GStreamer channel to it */
        self->data_channel_manager = gnalk_data_channel_new ();
        gnalk_data_channel_set_channel (self->data_channel_manager, data_channel);
        gnalk_data_channel_update_status (self->data_channel_manager,
                                           self->video_enabled,
                                           self->audio_enabled);

        /* Connect to speaking-changed signal for remote speaking notifications */
        g_signal_connect (self->data_channel_manager, "speaking-changed",
                          G_CALLBACK (on_data_channel_speaking_changed), self);

        g_object_unref (data_channel); /* Manager now owns it */
      }
    else
      {
        g_warning ("Failed to create data channel - Android may not show video");
      }
  }

  /* Create media pipeline for video and audio sources */
  {
    self->media_pipeline = gnalk_media_pipeline_new ();
    if (!gnalk_media_pipeline_create (self->media_pipeline,
                                       GNALK_MEDIA_PIPELINE_SEND,
                                       webrtc,
                                       self->camera_position))
      {
        g_warning ("WebRTC: Failed to create media pipeline");
        g_clear_object (&self->media_pipeline);
        gst_object_unref (pipeline);
        return FALSE;
      }

    /* Get references from media pipeline */
    self->video_encoder = gnalk_media_pipeline_get_video_encoder (self->media_pipeline);
    self->audio_volume = gnalk_media_pipeline_get_audio_volume (self->media_pipeline);
    self->local_video_sink = gnalk_media_pipeline_get_local_preview (self->media_pipeline);

    /* Connect to local video frame signal */
    g_signal_connect (self->media_pipeline, "local-video-frame",
                      G_CALLBACK (on_media_pipeline_local_video_frame), self);

    g_message ("WebRTC: Media pipeline created successfully");
  }

  /* Create quality controller for adaptive bitrate */
  {
    self->quality_controller = gnalk_quality_controller_new ();
    gnalk_quality_controller_set_webrtcbin (self->quality_controller, webrtc);
    gnalk_quality_controller_set_video_encoder (self->quality_controller, self->video_encoder);
    g_message ("WebRTC: Quality controller created");
  }

  /* Create receive pipeline for handling incoming streams */
  {
    self->receive_pipeline = gnalk_receive_pipeline_new ();
    gnalk_receive_pipeline_set_pipeline (self->receive_pipeline, pipeline);
    gnalk_receive_pipeline_set_quality_controller (self->receive_pipeline, self->quality_controller);
    g_signal_connect (self->receive_pipeline, "remote-video-frame",
                      G_CALLBACK (on_receive_pipeline_remote_video_frame), self);
    g_message ("WebRTC: Receive pipeline created");
  }

  /* NOTE: Pipeline element creation now handled by GnalkMediaPipeline */

  /* Connect to internal rtpbin for jitterbuffer config (disable RTX) */
  connect_rtpbin_signals (webrtc);

  g_signal_connect (webrtc, "on-ice-candidate", G_CALLBACK (on_ice_candidate), self);
  g_signal_connect (webrtc, "on-negotiation-needed", G_CALLBACK (on_negotiation_needed), self);
  g_signal_connect (webrtc, "pad-added", G_CALLBACK (on_incoming_stream), self);
  g_signal_connect (webrtc, "notify::ice-connection-state", G_CALLBACK (on_ice_connection_state_notify), self);
  g_signal_connect (webrtc, "notify::connection-state", G_CALLBACK (on_connection_state_notify), self);
  g_signal_connect (webrtc, "on-new-transceiver", G_CALLBACK (on_new_transceiver), self);

  self->pipeline = pipeline;
  self->webrtcbin = webrtc;

  /* Add bus watch to catch errors */
  GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  gst_bus_add_watch (bus, on_bus_message, self);
  gst_object_unref (bus);

  return TRUE;
}

static gboolean
create_receive_only_pipeline (GnalkWebrtc *self)
{
  GstElement *pipeline, *webrtc;

  pipeline = gst_pipeline_new ("receive-pipeline");
  webrtc = gst_element_factory_make ("webrtcbin", "webrtc");

  if (!webrtc)
    {
      g_warning ("Failed to create webrtcbin element");
      gst_object_unref (pipeline);
      return FALSE;
    }

  /* Set MAX_BUNDLE to ensure bundle group is included in answers.
   * Without this, webrtcbin creates separate transports causing "not-linked" errors. */
  g_object_set (webrtc, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, NULL);

  /* Set latency for proper RTP timestamp handling */
  g_object_set (webrtc, "latency", 200, NULL);  /* 200ms jitter buffer for packet reordering */

  /* Connect to internal rtpbin for jitterbuffer diagnostics */
  connect_rtpbin_signals (webrtc);

  if (self->stun_url)
    {
      g_autofree char *stun_uri = NULL;
      if (g_str_has_prefix (self->stun_url, "stun:"))
        stun_uri = g_strdup_printf ("stun://%s", self->stun_url + 5);
      else
        stun_uri = g_strdup (self->stun_url);
      g_object_set (webrtc, "stun-server", stun_uri, NULL);
    }

  if (self->turn_url && self->turn_username && self->turn_password)
    {
      g_autofree char *turn_uri = NULL;
      const char *server_part = self->turn_url;
      if (g_str_has_prefix (self->turn_url, "turn:"))
        server_part = self->turn_url + 5;
      else if (g_str_has_prefix (self->turn_url, "turns:"))
        server_part = self->turn_url + 6;

      g_autofree char *escaped_user = g_uri_escape_string (self->turn_username, NULL, TRUE);
      g_autofree char *escaped_pass = g_uri_escape_string (self->turn_password, NULL, TRUE);

      turn_uri = g_strdup_printf ("turn://%s:%s@%s",
                                   escaped_user, escaped_pass, server_part);
      gboolean ret = FALSE;
      g_signal_emit_by_name (webrtc, "add-turn-server", turn_uri, &ret);
      if (!ret)
        g_warning ("Failed to add TURN server");
    }

  gst_bin_add (GST_BIN (pipeline), webrtc);

  /* No local media sources - this is receive-only */

  /* Create quality controller for PLI and diagnostics */
  {
    self->quality_controller = gnalk_quality_controller_new ();
    gnalk_quality_controller_set_webrtcbin (self->quality_controller, webrtc);
    g_message ("Receive pipeline: Quality controller created");
  }

  /* Create receive pipeline for handling incoming streams */
  {
    self->receive_pipeline = gnalk_receive_pipeline_new ();
    gnalk_receive_pipeline_set_pipeline (self->receive_pipeline, pipeline);
    gnalk_receive_pipeline_set_quality_controller (self->receive_pipeline, self->quality_controller);
    g_signal_connect (self->receive_pipeline, "remote-video-frame",
                      G_CALLBACK (on_receive_pipeline_remote_video_frame), self);
    g_message ("Receive pipeline: Receive pipeline created");
  }

  g_signal_connect (webrtc, "on-ice-candidate", G_CALLBACK (on_ice_candidate), self);
  g_signal_connect (webrtc, "on-negotiation-needed", G_CALLBACK (on_negotiation_needed), self);
  g_signal_connect (webrtc, "pad-added", G_CALLBACK (on_incoming_stream), self);
  g_signal_connect (webrtc, "notify::ice-connection-state", G_CALLBACK (on_ice_connection_state_notify), self);
  g_signal_connect (webrtc, "notify::connection-state", G_CALLBACK (on_connection_state_notify), self);
  g_signal_connect (webrtc, "on-new-transceiver", G_CALLBACK (on_new_transceiver), self);
  g_signal_connect (webrtc, "on-data-channel", G_CALLBACK (on_remote_data_channel), self);

  self->pipeline = pipeline;
  self->webrtcbin = webrtc;
  self->receive_only = TRUE;

  /* Add bus watch to catch errors */
  GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  gst_bus_add_watch (bus, on_bus_message, self);
  gst_object_unref (bus);

  return TRUE;
}

/* Probe on encoder sink pad to track frames and force initial keyframes */
static GstPadProbeReturn
on_screen_share_encoder_probe (GstPad          *pad,
                               GstPadProbeInfo *info,
                               gpointer         user_data)
{
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);
  self->screen_encoder_frame_count++;

  /* Log detailed info for first 10 frames and periodically */
  if (self->screen_encoder_frame_count <= 10 || self->screen_encoder_frame_count % 100 == 0)
    {
      GstElement *encoder = gst_pad_get_parent_element (pad);
      if (encoder)
        {
          GstState state, pending;
          gst_element_get_state (encoder, &state, &pending, 0);

          /* Check if encoder src pad has caps negotiated */
          GstPad *src_pad = gst_element_get_static_pad (encoder, "src");
          gboolean has_src_caps = FALSE;
          if (src_pad)
            {
              GstCaps *src_caps = gst_pad_get_current_caps (src_pad);
              has_src_caps = (src_caps != NULL);
              if (src_caps) gst_caps_unref (src_caps);
              gst_object_unref (src_pad);
            }

          g_debug ("Screen encoder[%p] INPUT: frame %u, state=%s pending=%s, src_caps=%s",
                     (void *)self, self->screen_encoder_frame_count,
                     gst_element_state_get_name (state),
                     gst_element_state_get_name (pending),
                     has_src_caps ? "YES" : "NO");
          gst_object_unref (encoder);
        }
      else
        {
          g_debug ("Screen encoder[%p] INPUT: frame %u (no encoder ref)",
                     (void *)self, self->screen_encoder_frame_count);
        }
    }

  /* Force keyframes on first 3 frames to help remote peer decode faster.
   * Push upstream force-key-unit event from encoder sink pad. */
  if (self->screen_encoder_frame_count <= 3)
    {
      GstElement *encoder = gst_pad_get_parent_element (pad);
      if (encoder)
        {
          GstEvent *force_key = gst_video_event_new_upstream_force_key_unit (
              GST_CLOCK_TIME_NONE, TRUE, self->screen_encoder_frame_count);
          gboolean sent = gst_element_send_event (encoder, force_key);
          g_message ("Screen share[%p]: forced initial keyframe #%u, result=%d",
                     (void *)self, self->screen_encoder_frame_count, sent);
          gst_object_unref (encoder);
        }
    }

  (void)info;
  return GST_PAD_PROBE_OK;
}

/* Probe on encoder SOURCE pad to track encoded output */
static GstPadProbeReturn
on_screen_encoder_output_probe (GstPad          *pad,
                                GstPadProbeInfo *info,
                                gpointer         user_data)
{
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);
  self->screen_encoder_output_count++;
  if (self->screen_encoder_output_count <= 10 || self->screen_encoder_output_count % 50 == 0)
    {
      GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
      gsize size = buf ? gst_buffer_get_size (buf) : 0;
      g_debug ("Screen share encoder[%p] OUTPUT: frame %u (size=%zu bytes)",
                 (void *)self, self->screen_encoder_output_count, size);
    }
  (void)pad;
  return GST_PAD_PROBE_OK;
}

/* Probe to count RTP packets going to webrtcbin */
static GstPadProbeReturn
on_screen_rtppay_probe (GstPad          *pad,
                        GstPadProbeInfo *info,
                        gpointer         user_data)
{
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);
  self->screen_rtp_packet_count++;

  /* Log first 10 and every 50th to track RTP packet flow */
  if (self->screen_rtp_packet_count <= 10 || self->screen_rtp_packet_count % 50 == 0)
    {
      GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
      gsize size = buf ? gst_buffer_get_size (buf) : 0;
      g_debug ("Screen share RTP[%p]: packet %u to webrtcbin (size=%zu bytes)",
                 (void *)self, self->screen_rtp_packet_count, size);
    }
  (void)pad;
  return GST_PAD_PROBE_OK;
}

/* Per-instance probe for pipewiresrc output.
 * On first frame, detects input dimensions and configures capsfilter
 * to scale to max 720 on the longer edge while preserving aspect ratio. */
static GstPadProbeReturn
on_pipewiresrc_log_probe (GstPad          *pad,
                          GstPadProbeInfo *info,
                          gpointer         user_data)
{
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);
  self->pipewiresrc_frame_count++;

  /* On first frame, configure capsfilter based on input dimensions */
  if (!self->screen_caps_configured && self->screen_capsfilter)
    {
      GstCaps *pad_caps = gst_pad_get_current_caps (pad);
      if (pad_caps)
        {
          GstStructure *s = gst_caps_get_structure (pad_caps, 0);
          gint width = 0, height = 0;
          if (gst_structure_get_int (s, "width", &width) &&
              gst_structure_get_int (s, "height", &height) &&
              width > 0 && height > 0)
            {
              /* Scale to max 720 on the longer edge */
              gint target_width, target_height;
              gint max_dim = 720;

              if (width >= height)
                {
                  /* Landscape or square */
                  target_width = MIN (width, max_dim);
                  target_height = height * target_width / width;
                }
              else
                {
                  /* Portrait */
                  target_height = MIN (height, max_dim);
                  target_width = width * target_height / height;
                }

              /* Ensure even dimensions for I420 */
              target_width &= ~1;
              target_height &= ~1;

              g_autofree gchar *caps_str = g_strdup_printf (
                  "video/x-raw,format=I420,width=%d,height=%d",
                  target_width, target_height);
              GstCaps *new_caps = gst_caps_from_string (caps_str);
              g_object_set (self->screen_capsfilter, "caps", new_caps, NULL);
              gst_caps_unref (new_caps);

              g_message ("Screen share: input %dx%d -> scaled to %dx%d",
                         width, height, target_width, target_height);
              self->screen_caps_configured = TRUE;
            }
          gst_caps_unref (pad_caps);
        }
    }

  if (self->pipewiresrc_frame_count <= 10 || self->pipewiresrc_frame_count % 50 == 0)
    {
      GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
      gsize size = buf ? gst_buffer_get_size (buf) : 0;
      g_debug ("pipewiresrc[%p] output: frame %u (size=%zu)",
                 (void *)self, self->pipewiresrc_frame_count, size);
    }
  return GST_PAD_PROBE_OK;
}

/* Probe after queue to see if frames reach videoconvert */
static GstPadProbeReturn
on_screen_queue_src_probe (GstPad          *pad,
                           GstPadProbeInfo *info,
                           gpointer         user_data)
{
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);
  self->screen_queue_frame_count++;
  if (self->screen_queue_frame_count <= 10 || self->screen_queue_frame_count % 50 == 0)
    {
      GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
      gsize size = buf ? gst_buffer_get_size (buf) : 0;
      g_debug ("screen[%p] queue->videoconvert: frame %u (size=%zu)",
                 (void *)self, self->screen_queue_frame_count, size);
    }
  (void)pad;
  return GST_PAD_PROBE_OK;
}

/* Probe after capsfilter to see if frames reach encoder */
static GstPadProbeReturn
on_screen_capsfilter_src_probe (GstPad          *pad,
                                GstPadProbeInfo *info,
                                gpointer         user_data)
{
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);
  self->screen_capsfilter_frame_count++;
  if (self->screen_capsfilter_frame_count <= 10 || self->screen_capsfilter_frame_count % 50 == 0)
    {
      GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
      gsize size = buf ? gst_buffer_get_size (buf) : 0;
      g_debug ("screen[%p] capsfilter->encoder: frame %u (size=%zu)",
                 (void *)self, self->screen_capsfilter_frame_count, size);
    }
  (void)pad;
  return GST_PAD_PROBE_OK;
}

/* Periodic timer to force keyframes during screen share.
 * Ensures remote receives decodable frames even if RTCP feedback is lost. */
static gboolean
on_screen_keyframe_timer (gpointer user_data)
{
  GnalkWebrtc *self = GNALK_WEBRTC (user_data);

  if (!self->is_screen_share || !self->pipeline)
    {
      self->screen_keyframe_timer_id = 0;
      return G_SOURCE_REMOVE;
    }

  GstElement *encoder = gst_bin_get_by_name (GST_BIN (self->pipeline), "screenenc");
  if (encoder)
    {
      GstEvent *force_key = gst_video_event_new_upstream_force_key_unit (
          GST_CLOCK_TIME_NONE, TRUE, 0);
      gst_element_send_event (encoder, force_key);
      gst_object_unref (encoder);
    }

  return G_SOURCE_CONTINUE;
}

static void
start_screen_keyframe_timer (GnalkWebrtc *self)
{
  if (self->screen_keyframe_timer_id > 0)
    return;  /* Already running */

  /* Force keyframe every 2 seconds */
  self->screen_keyframe_timer_id = g_timeout_add_seconds (2, on_screen_keyframe_timer, self);
  g_message ("Screen share keyframe timer started");
}

static void
stop_screen_keyframe_timer (GnalkWebrtc *self)
{
  if (self->screen_keyframe_timer_id > 0)
    {
      g_source_remove (self->screen_keyframe_timer_id);
      self->screen_keyframe_timer_id = 0;
      g_message ("Screen share keyframe timer stopped");
    }
}

/* Create screen share pipeline - video only, higher bitrate, uses PipeWire source */
static gboolean
create_screen_share_pipeline (GnalkWebrtc *self)
{
  GstElement *pipeline, *webrtc;
  GstElement *pipewiresrc, *queue, *capsfilter;
  GstElement *encoder, *rtppay;

  g_message ("WebRTC: Creating screen share pipeline (node_id=%u, fd=%d)",
             self->pipewire_node_id, self->pipewire_fd);

  /* Reset per-instance counters and state for new screen share session */
  self->screen_encoder_frame_count = 0;
  self->screen_rtp_packet_count = 0;
  self->pipewiresrc_frame_count = 0;
  self->screen_queue_frame_count = 0;
  self->screen_capsfilter_frame_count = 0;
  self->screen_encoder_output_count = 0;
  self->screen_capsfilter = NULL;
  self->screen_caps_configured = FALSE;

  pipeline = gst_pipeline_new ("screen-share-pipeline");
  webrtc = gst_element_factory_make ("webrtcbin", "webrtc");

  if (!webrtc)
    {
      g_warning ("Failed to create webrtcbin element for screen share");
      gst_object_unref (pipeline);
      return FALSE;
    }

  g_object_set (webrtc, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, NULL);
  g_object_set (webrtc, "latency", 200, NULL);

  if (self->stun_url)
    {
      g_autofree char *stun_uri = NULL;
      if (g_str_has_prefix (self->stun_url, "stun:"))
        stun_uri = g_strdup_printf ("stun://%s", self->stun_url + 5);
      else
        stun_uri = g_strdup (self->stun_url);
      g_object_set (webrtc, "stun-server", stun_uri, NULL);
    }

  if (self->turn_url && self->turn_username && self->turn_password)
    {
      g_autofree char *turn_uri = NULL;
      const char *server_part = self->turn_url;
      if (g_str_has_prefix (self->turn_url, "turn:"))
        server_part = self->turn_url + 5;
      else if (g_str_has_prefix (self->turn_url, "turns:"))
        server_part = self->turn_url + 6;

      g_autofree char *escaped_user = g_uri_escape_string (self->turn_username, NULL, TRUE);
      g_autofree char *escaped_pass = g_uri_escape_string (self->turn_password, NULL, TRUE);

      turn_uri = g_strdup_printf ("turn://%s:%s@%s",
                                   escaped_user, escaped_pass, server_part);
      gboolean ret = FALSE;
      g_signal_emit_by_name (webrtc, "add-turn-server", turn_uri, &ret);
      if (!ret)
        g_warning ("Failed to add TURN server for screen share");
    }

  gst_bin_add (GST_BIN (pipeline), webrtc);

  /* Create video pipeline elements.
   * Use always-copy=true to force pipewiresrc to copy DMA-BUF to system memory.
   * videoconvert may produce "code not implemented" warnings but still processes frames. */
  pipewiresrc = gst_element_factory_make ("pipewiresrc", "screensrc");
  queue = gst_element_factory_make ("queue", "srcqueue");
  GstElement *videoconvert = gst_element_factory_make ("videoconvert", "videoconvert");
  capsfilter = gst_element_factory_make ("capsfilter", "capsfilter");
  encoder = gst_element_factory_make ("vp8enc", "screenenc");
  rtppay = gst_element_factory_make ("rtpvp8pay", "rtppay");

  if (!pipewiresrc || !queue || !videoconvert || !capsfilter || !encoder || !rtppay)
    {
      g_warning ("Failed to create screen share pipeline elements");
      if (pipewiresrc) gst_object_unref (pipewiresrc);
      if (queue) gst_object_unref (queue);
      if (videoconvert) gst_object_unref (videoconvert);
      if (capsfilter) gst_object_unref (capsfilter);
      if (encoder) gst_object_unref (encoder);
      if (rtppay) gst_object_unref (rtppay);
      gst_object_unref (pipeline);
      return FALSE;
    }

  /* Configure pipewiresrc with node ID and file descriptor from portal.
   * pipewiresrc is implicitly a live source (no is-live property needed).
   * always-copy=true forces pipewiresrc to copy DMA-BUF to system memory.
   * keepalive-time=100 resends last buffer every 100ms on static screens. */
  g_autofree char *path_str = g_strdup_printf ("%u", self->pipewire_node_id);
  g_object_set (pipewiresrc,
                "path", path_str,
                "fd", self->pipewire_fd,
                "do-timestamp", TRUE,
                "always-copy", TRUE,
                "keepalive-time", 100,
                NULL);

  /* Add logging probe on pipewiresrc to see if frames are produced */
  GstPad *pw_src_pad = gst_element_get_static_pad (pipewiresrc, "src");
  if (pw_src_pad)
    {
      gst_pad_add_probe (pw_src_pad, GST_PAD_PROBE_TYPE_BUFFER, on_pipewiresrc_log_probe, self, NULL);
      gst_object_unref (pw_src_pad);
    }

  /* Add videoscale to reduce resolution. The actual target dimensions are set
   * dynamically by on_pipewiresrc_log_probe based on input aspect ratio.
   * This preserves aspect ratio while limiting the longer edge to 720 pixels. */
  GstElement *videoscale = gst_element_factory_make ("videoscale", "videoscale");
  if (!videoscale)
    {
      g_warning ("Failed to create videoscale element");
      gst_object_unref (pipewiresrc);
      gst_object_unref (queue);
      gst_object_unref (videoconvert);
      gst_object_unref (capsfilter);
      gst_object_unref (encoder);
      gst_object_unref (rtppay);
      gst_object_unref (pipeline);
      return FALSE;
    }

  /* Initial caps - will be updated dynamically by pipewiresrc probe.
   * Start with just format, dimensions set on first frame. */
  GstCaps *caps = gst_caps_from_string ("video/x-raw,format=I420");
  g_object_set (capsfilter, "caps", caps, NULL);
  gst_caps_unref (caps);

  /* Store capsfilter reference for dynamic configuration */
  self->screen_capsfilter = capsfilter;
  self->screen_caps_configured = FALSE;

  /* Configure encoder for screen content:
   * - 1.5 Mbps bitrate for ~300K pixel screen content
   * - deadline=1 for realtime encoding
   * - cpu-used=16 for maximum speed on ARM (trades quality for speed)
   * - keyframe-max-dist=30 for keyframe every second
   * - end-usage=1 (CBR) for more predictable performance */
  g_object_set (encoder,
                "deadline", 1,
                "cpu-used", 16,
                "target-bitrate", 1500000,
                "keyframe-max-dist", 30,
                "end-usage", 1,  /* CBR mode */
                NULL);

  g_object_set (rtppay, "pt", 96, "mtu", 1200, NULL);

  /* Configure queue for decoupling - leaky to drop old frames if encoder is slow */
  g_object_set (queue, "max-size-buffers", 10, "leaky", 2, NULL);

  GstElement *queue2 = gst_element_factory_make ("queue", "encqueue");
  g_object_set (queue2, "max-size-buffers", 2, "leaky", 2, NULL);

  /* Add capsfilter after pipewiresrc to force raw video format.
   * This prevents DMA-BUF negotiation issues and ensures we get system memory frames. */
  GstElement *rawcaps = gst_element_factory_make ("capsfilter", "rawcaps");
  if (!rawcaps)
    {
      g_warning ("Failed to create rawcaps element");
      gst_object_unref (pipewiresrc);
      gst_object_unref (queue);
      gst_object_unref (videoconvert);
      gst_object_unref (videoscale);
      gst_object_unref (capsfilter);
      gst_object_unref (encoder);
      gst_object_unref (rtppay);
      gst_object_unref (queue2);
      gst_object_unref (pipeline);
      return FALSE;
    }
  GstCaps *raw_video_caps = gst_caps_from_string ("video/x-raw");
  g_object_set (rawcaps, "caps", raw_video_caps, NULL);
  gst_caps_unref (raw_video_caps);

  /* Add elements to pipeline */
  gst_bin_add_many (GST_BIN (pipeline), pipewiresrc, rawcaps, queue,
                    videoconvert, videoscale, capsfilter, queue2, encoder, rtppay, NULL);

  /* Pipeline: pipewiresrc -> rawcaps(video/x-raw) -> queue -> videoconvert -> videoscale ->
   *           capsfilter(I420,dynamic) -> queue2 -> encoder -> rtppay -> webrtcbin
   * The capsfilter dimensions are set dynamically based on input aspect ratio. */
  if (!gst_element_link_many (pipewiresrc, rawcaps, queue,
                              videoconvert, videoscale, capsfilter, queue2, encoder, rtppay, NULL))
    {
      g_warning ("Failed to link screen share pipeline");
      gst_object_unref (pipeline);
      return FALSE;
    }

  /* Add probes to trace frame flow through pipeline */
  GstPad *queue_src = gst_element_get_static_pad (queue, "src");
  if (queue_src)
    {
      gst_pad_add_probe (queue_src, GST_PAD_PROBE_TYPE_BUFFER, on_screen_queue_src_probe, self, NULL);
      gst_object_unref (queue_src);
    }

  GstPad *capsfilter_src = gst_element_get_static_pad (capsfilter, "src");
  if (capsfilter_src)
    {
      gst_pad_add_probe (capsfilter_src, GST_PAD_PROBE_TYPE_BUFFER, on_screen_capsfilter_src_probe, self, NULL);
      gst_object_unref (capsfilter_src);
    }

  /* Link RTP payloader to webrtcbin */
  GstPad *rtppay_src = gst_element_get_static_pad (rtppay, "src");
  GstPad *webrtc_sink = gst_element_request_pad_simple (webrtc, "sink_%u");

  if (!rtppay_src || !webrtc_sink)
    {
      g_warning ("Failed to get pads for screen share webrtcbin link");
      if (rtppay_src) gst_object_unref (rtppay_src);
      if (webrtc_sink) gst_object_unref (webrtc_sink);
      gst_object_unref (pipeline);
      return FALSE;
    }

  /* Add probe on rtppay src to count RTP packets going to webrtcbin */
  gst_pad_add_probe (rtppay_src, GST_PAD_PROBE_TYPE_BUFFER, on_screen_rtppay_probe, self, NULL);

  if (gst_pad_link (rtppay_src, webrtc_sink) != GST_PAD_LINK_OK)
    {
      g_warning ("Failed to link rtppay to webrtcbin for screen share");
      gst_object_unref (rtppay_src);
      gst_object_unref (webrtc_sink);
      gst_object_unref (pipeline);
      return FALSE;
    }

  gst_object_unref (rtppay_src);
  gst_object_unref (webrtc_sink);

  /* Connect WebRTC signals */
  g_signal_connect (webrtc, "on-ice-candidate", G_CALLBACK (on_ice_candidate), self);
  g_signal_connect (webrtc, "on-negotiation-needed", G_CALLBACK (on_negotiation_needed), self);
  g_signal_connect (webrtc, "notify::ice-connection-state", G_CALLBACK (on_ice_connection_state_notify), self);
  g_signal_connect (webrtc, "notify::connection-state", G_CALLBACK (on_connection_state_notify), self);

  self->pipeline = pipeline;
  self->webrtcbin = webrtc;
  self->video_encoder = encoder;
  self->is_screen_share = TRUE;
  self->receive_only = FALSE;

  /* Add bus watch to catch errors */
  GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  gst_bus_add_watch (bus, on_bus_message, self);
  gst_object_unref (bus);

  /* Add probe on encoder sink to track frames and force initial keyframes.
   * Must be on sink pad so upstream force-key-unit events flow correctly. */
  GstPad *enc_sink = gst_element_get_static_pad (encoder, "sink");
  if (enc_sink)
    {
      gst_pad_add_probe (enc_sink, GST_PAD_PROBE_TYPE_BUFFER,
                         (GstPadProbeCallback) on_screen_share_encoder_probe, self, NULL);
      gst_object_unref (enc_sink);
    }

  /* Add probe on encoder source to track encoded output */
  GstPad *enc_src = gst_element_get_static_pad (encoder, "src");
  if (enc_src)
    {
      gst_pad_add_probe (enc_src, GST_PAD_PROBE_TYPE_BUFFER,
                         (GstPadProbeCallback) on_screen_encoder_output_probe, self, NULL);
      gst_object_unref (enc_src);
    }

  g_message ("WebRTC: Screen share pipeline created successfully");

  return TRUE;
}

#endif /* HAVE_GSTREAMER_WEBRTC */

void
gnalk_webrtc_start_call (GnalkWebrtc *self,
                           const char   *peer_id)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));

#ifdef HAVE_GSTREAMER_WEBRTC
  if (self->pipeline)
    gnalk_webrtc_end_call (self);

  g_free (self->peer_id);
  self->peer_id = g_strdup (peer_id);
  self->receive_only = FALSE;  /* Publisher sends media */

  if (!create_pipeline (self))
    {
      g_warning ("start_call: create_pipeline failed");
      return;
    }

  gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
#else
  g_warning ("start_call: WebRTC support not available");
#endif
}

void
gnalk_webrtc_start_screen_share (GnalkWebrtc *self,
                                   const char   *peer_id,
                                   guint32       pipewire_node_id,
                                   int           pipewire_fd)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));
  g_return_if_fail (pipewire_node_id > 0);
  g_return_if_fail (pipewire_fd >= 0);

#ifdef HAVE_GSTREAMER_WEBRTC
  if (self->pipeline)
    gnalk_webrtc_end_call (self);

  g_free (self->peer_id);
  self->peer_id = g_strdup (peer_id);
  self->pipewire_node_id = pipewire_node_id;

  /* Duplicate the fd so pipewiresrc can take ownership of its own copy.
   * The caller (screen_capture) will close the original fd when stopped. */
  self->pipewire_fd = dup (pipewire_fd);
  if (self->pipewire_fd < 0)
    {
      g_warning ("start_screen_share: failed to dup pipewire fd: %s", g_strerror (errno));
      return;
    }

  if (!create_screen_share_pipeline (self))
    {
      g_warning ("start_screen_share: create_screen_share_pipeline failed");
      close (self->pipewire_fd);
      self->pipewire_fd = -1;
      return;
    }

  /* First go to PAUSED to negotiate caps with PipeWire.
   * This avoids "not-negotiated" errors when the portal stream isn't ready yet. */
  GstStateChangeReturn ret = gst_element_set_state (self->pipeline, GST_STATE_PAUSED);
  if (ret == GST_STATE_CHANGE_ASYNC)
    {
      /* Wait up to 3 seconds for caps negotiation */
      ret = gst_element_get_state (self->pipeline, NULL, NULL, 3 * GST_SECOND);
    }

  if (ret == GST_STATE_CHANGE_FAILURE)
    {
      g_warning ("start_screen_share: Failed to negotiate caps, retrying...");
      /* Give PipeWire more time to set up the stream */
      g_usleep (200000);  /* 200ms */
      ret = gst_element_set_state (self->pipeline, GST_STATE_PAUSED);
      if (ret == GST_STATE_CHANGE_ASYNC)
        ret = gst_element_get_state (self->pipeline, NULL, NULL, 3 * GST_SECOND);
    }

  if (ret == GST_STATE_CHANGE_FAILURE)
    {
      g_warning ("start_screen_share: Caps negotiation failed after retry");
      gst_element_set_state (self->pipeline, GST_STATE_NULL);
      close (self->pipewire_fd);
      self->pipewire_fd = -1;
      gst_object_unref (self->pipeline);
      self->pipeline = NULL;
      self->webrtcbin = NULL;
      return;
    }

  /* Configure capsfilter based on pipewiresrc negotiated dimensions.
   * This must be done while PAUSED, before going to PLAYING. */
  if (self->screen_capsfilter && !self->screen_caps_configured)
    {
      GstElement *screensrc = gst_bin_get_by_name (GST_BIN (self->pipeline), "screensrc");
      if (screensrc)
        {
          GstPad *src_pad = gst_element_get_static_pad (screensrc, "src");
          if (src_pad)
            {
              GstCaps *pad_caps = gst_pad_get_current_caps (src_pad);
              if (pad_caps)
                {
                  GstStructure *s = gst_caps_get_structure (pad_caps, 0);
                  gint width = 0, height = 0;
                  if (gst_structure_get_int (s, "width", &width) &&
                      gst_structure_get_int (s, "height", &height) &&
                      width > 0 && height > 0)
                    {
                      /* Scale to max 720 on the longer edge */
                      gint target_width, target_height;
                      gint max_dim = 720;

                      if (width >= height)
                        {
                          target_width = MIN (width, max_dim);
                          target_height = height * target_width / width;
                        }
                      else
                        {
                          target_height = MIN (height, max_dim);
                          target_width = width * target_height / height;
                        }

                      /* Ensure even dimensions for I420 */
                      target_width &= ~1;
                      target_height &= ~1;

                      g_autofree gchar *caps_str = g_strdup_printf (
                          "video/x-raw,format=I420,width=%d,height=%d",
                          target_width, target_height);
                      GstCaps *new_caps = gst_caps_from_string (caps_str);
                      g_object_set (self->screen_capsfilter, "caps", new_caps, NULL);
                      gst_caps_unref (new_caps);

                      g_message ("Screen share: input %dx%d -> scaled to %dx%d",
                                 width, height, target_width, target_height);
                      self->screen_caps_configured = TRUE;
                    }
                  gst_caps_unref (pad_caps);
                }
              gst_object_unref (src_pad);
            }
          gst_object_unref (screensrc);
        }
    }

  gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
#else
  (void)peer_id;
  (void)pipewire_node_id;
  (void)pipewire_fd;
  g_warning ("start_screen_share: WebRTC support not available");
#endif
}

gboolean
gnalk_webrtc_is_screen_share (GnalkWebrtc *self)
{
  g_return_val_if_fail (GNALK_IS_WEBRTC (self), FALSE);
  return self->is_screen_share;
}

void
gnalk_webrtc_end_call (GnalkWebrtc *self)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));

#ifdef HAVE_GSTREAMER
  /* Stop periodic timers first to prevent timer callbacks during shutdown */
  stop_screen_keyframe_timer (self);
  if (self->data_channel_manager)
    gnalk_data_channel_stop_periodic_status (self->data_channel_manager);
  if (self->quality_controller)
    {
      gnalk_quality_controller_stop_bitrate_timer (self->quality_controller);
      gnalk_quality_controller_stop_pli_timer (self->quality_controller);
      gnalk_quality_controller_enable_continuous_pli (self->quality_controller, FALSE, 0);
    }

  /* CRITICAL: Stop the GStreamer pipeline BEFORE cleaning up receive_pipeline/media_pipeline.
   * The appsink's "new-sample" signal uses receive_pipeline as user_data, so if we free
   * receive_pipeline while the pipeline is still running, a final sample could trigger
   * a callback with freed memory, causing a crash. */
  if (self->pipeline)
    {
      /* Stop pipeline - transition through states to release camera properly.
       * Don't use EOS as it can hang the main thread.
       *
       * IMPORTANT: Stop camera_bin separately first to allow libcamerasrc to
       * properly drain its queued frame requests. Without this, libcamera
       * crashes with assertion "data->queuedRequests_.empty()" in stop(). */

      /* Stop camera first to drain libcamera's queued requests.
       * libcamera on some devices (e.g., OnePlus 6) crashes if stopped
       * while requests are pending. */
      if (self->media_pipeline)
        gnalk_media_pipeline_stop_camera (self->media_pipeline);

      /* Remove bus watch BEFORE stopping pipeline to prevent use-after-free.
       * The bus watch callback holds a raw pointer to self, so we must remove
       * it before the pipeline is destroyed to avoid callbacks firing on freed memory. */
      GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (self->pipeline));
      if (bus)
        {
          gst_bus_remove_watch (bus);
          gst_object_unref (bus);
        }

      /* Now stop the rest of the pipeline */
      gst_element_set_state (self->pipeline, GST_STATE_PAUSED);
      gst_element_get_state (self->pipeline, NULL, NULL, 500 * GST_MSECOND);

      gst_element_set_state (self->pipeline, GST_STATE_READY);
      gst_element_get_state (self->pipeline, NULL, NULL, 500 * GST_MSECOND);

      gst_element_set_state (self->pipeline, GST_STATE_NULL);
      gst_element_get_state (self->pipeline, NULL, NULL, GST_SECOND);

      g_message ("WebRTC: Pipeline stopped and camera released");

      /* CRITICAL: Destroy media/receive pipelines BEFORE unreffing the main pipeline.
       * The helper objects hold raw pointers to GStreamer elements (e.g., local_preview).
       * When the main pipeline is unreffed, these elements are freed. Calling
       * gnalk_media_pipeline_destroy after that would access freed memory. */
      if (self->media_pipeline)
        {
          gnalk_media_pipeline_destroy (self->media_pipeline);
          g_clear_object (&self->media_pipeline);
        }

      if (self->receive_pipeline)
        {
          gnalk_receive_pipeline_destroy (self->receive_pipeline);
          g_clear_object (&self->receive_pipeline);
        }

      gst_object_unref (self->pipeline);
      self->pipeline = NULL;
      self->webrtcbin = NULL;
      self->video_sink = NULL;
      self->local_video_sink = NULL;
      self->video_encoder = NULL;

      /* Reset screen share state - pipewiresrc owns the fd and will close it */
      self->is_screen_share = FALSE;
      self->pipewire_fd = -1;
    }

  /* Now safe to clean up helper objects - pipeline is stopped so no more callbacks */
  g_clear_object (&self->data_channel_manager);
  g_clear_object (&self->quality_controller);
#endif

  self->receive_only = FALSE;
  g_clear_pointer (&self->peer_id, g_free);

  /* Reset state machine and negotiation state */
  set_call_state (self, GNALK_CALL_STATE_IDLE);
  self->making_offer = FALSE;
  self->ignore_offer = FALSE;
  self->answer_created = FALSE;
  self->remote_answer_set = FALSE;

  /* Clear pending ICE candidates */
  if (self->pending_ice_candidates)
    g_ptr_array_set_size (self->pending_ice_candidates, 0);

  /* Free callback user_data owned via GDestroyNotify to avoid leaks.
   * Only touch callbacks that own their data (notify set) - these are the
   * per-subscriber TileVideoCallbackData structs. Callbacks registered with a
   * NULL notify (e.g. the publisher's callbacks using the window as user_data)
   * are left untouched so the object stays usable across a pipeline restart.
   * NULL-out afterwards so a later end_call/finalize does not double-free. */
  if (self->signal_callback_notify && self->signal_callback_data)
    {
      self->signal_callback_notify (self->signal_callback_data);
      self->signal_callback = NULL;
      self->signal_callback_data = NULL;
      self->signal_callback_notify = NULL;
    }

  if (self->remote_video_callback_notify && self->remote_video_callback_data)
    {
      self->remote_video_callback_notify (self->remote_video_callback_data);
      self->remote_video_callback = NULL;
      self->remote_video_callback_data = NULL;
      self->remote_video_callback_notify = NULL;
    }

  if (self->connection_state_callback_notify && self->connection_state_callback_data)
    {
      self->connection_state_callback_notify (self->connection_state_callback_data);
      self->connection_state_callback = NULL;
      self->connection_state_callback_data = NULL;
      self->connection_state_callback_notify = NULL;
    }

  if (self->speaking_callback_notify && self->speaking_callback_data)
    {
      self->speaking_callback_notify (self->speaking_callback_data);
      self->speaking_callback = NULL;
      self->speaking_callback_data = NULL;
      self->speaking_callback_notify = NULL;
    }
}

void
gnalk_webrtc_handle_offer (GnalkWebrtc *self,
                              const char   *sdp)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));
  g_return_if_fail (sdp != NULL);

#ifdef HAVE_GSTREAMER_WEBRTC
  g_message ("handle_offer: received SDP offer:\n%s", sdp);

  /* Validate SDP before processing */
  if (!gnalk_sdp_validate (sdp, TRUE))
    {
      g_warning ("handle_offer: SDP validation failed, ignoring offer");
      return;
    }

  /* Perfect negotiation: check for offer collision.
   * If we're making an offer AND we receive one, there's a collision.
   * Polite peer yields (accepts incoming offer), impolite peer ignores. */
  if (self->making_offer)
    {
      if (!self->polite)
        {
          g_message ("handle_offer: ignoring incoming offer (impolite peer, offer collision)");
          self->ignore_offer = TRUE;
          return;
        }
      g_message ("handle_offer: accepting incoming offer (polite peer, offer collision)");
      /* Polite peer: accept the incoming offer, abandon our own */
      self->making_offer = FALSE;
    }
  self->ignore_offer = FALSE;

  /* Reset answer_created flag for new negotiation */
  self->answer_created = FALSE;

  if (!self->pipeline)
    {
      g_message ("handle_offer: creating receive-only pipeline");
      if (!create_receive_only_pipeline (self))
        {
          g_warning ("handle_offer: failed to create receive-only pipeline");
          return;
        }
      /* Keep pipeline in READY state - don't go to PLAYING yet.
       * We need to set the remote description BEFORE going to PLAYING
       * so webrtcbin knows about bundle and creates only one transport. */
      gst_element_set_state (self->pipeline, GST_STATE_READY);
      g_message ("handle_offer: pipeline set to READY");
    }

  /* Strip RTX from SDP to test if RTX handling causes packet loss */
  g_autofree char *cleaned_sdp = gnalk_sdp_strip_rtx (sdp);

  GstSDPMessage *sdp_msg;
  gst_sdp_message_new (&sdp_msg);
  if (gst_sdp_message_parse_buffer ((guint8 *)cleaned_sdp, strlen (cleaned_sdp), sdp_msg) != GST_SDP_OK)
    {
      g_warning ("handle_offer: failed to parse SDP buffer, ignoring offer");
      gst_sdp_message_free (sdp_msg);
      return;
    }

  GstWebRTCSessionDescription *offer =
    gst_webrtc_session_description_new (GST_WEBRTC_SDP_TYPE_OFFER, sdp_msg);

  g_message ("handle_offer: setting remote description");
  g_signal_emit_by_name (self->webrtcbin, "set-remote-description", offer, NULL);

  /* Update state machine */
  set_call_state (self, GNALK_CALL_STATE_NEGOTIATING);

  g_message ("handle_offer: creating answer");
  GstPromise *promise = gst_promise_new_with_change_func (on_answer_created, self, NULL);
  g_signal_emit_by_name (self->webrtcbin, "create-answer", NULL, promise);

  gst_webrtc_session_description_free (offer);
#endif
}

void
gnalk_webrtc_handle_answer (GnalkWebrtc *self,
                               const char   *sdp)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));
  g_return_if_fail (sdp != NULL);

#ifdef HAVE_GSTREAMER_WEBRTC
  if (!self->webrtcbin)
    return;

  /* Perfect negotiation: ignore answer if we ignored the offer collision */
  if (self->ignore_offer)
    {
      g_message ("handle_answer: ignoring answer (offer was ignored)");
      self->ignore_offer = FALSE;
      return;
    }

  /* Validate SDP before processing */
  if (!gnalk_sdp_validate (sdp, FALSE))
    {
      g_warning ("handle_answer: SDP validation failed, ignoring answer");
      return;
    }

  g_message ("handle_answer: received SDP answer:\n%s", sdp);

  /* Strip RTX from SDP - GStreamer's RTX handling causes packet loss */
  g_autofree char *cleaned_sdp = gnalk_sdp_strip_rtx (sdp);

  GstSDPMessage *sdp_msg;
  gst_sdp_message_new (&sdp_msg);
  if (gst_sdp_message_parse_buffer ((guint8 *)cleaned_sdp, strlen (cleaned_sdp), sdp_msg) != GST_SDP_OK)
    {
      g_warning ("handle_answer: failed to parse SDP buffer, ignoring answer");
      gst_sdp_message_free (sdp_msg);
      return;
    }

  GstWebRTCSessionDescription *answer =
    gst_webrtc_session_description_new (GST_WEBRTC_SDP_TYPE_ANSWER, sdp_msg);

  g_signal_emit_by_name (self->webrtcbin, "set-remote-description", answer, NULL);

  /* Remote description is now set - flush any ICE candidates that arrived early. */
  self->remote_answer_set = TRUE;
  flush_pending_ice_candidates (self);

  gst_webrtc_session_description_free (answer);
#endif
}

void
gnalk_webrtc_handle_ice_candidate (GnalkWebrtc *self,
                                      const char   *candidate,
                                      int           sdp_mline_index,
                                      const char   *sdp_mid)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));
  g_return_if_fail (candidate != NULL);

#ifdef HAVE_GSTREAMER_WEBRTC
  if (!self->webrtcbin)
    return;

  /* Buffer remote ICE candidates until webrtcbin can process them.
   * In receive-only mode, this is until our answer is created (local description
   * set). In sender mode, it is until the remote answer is set - candidates that
   * arrive before the answer would otherwise be dropped by webrtcbin. */
  gboolean can_add = self->receive_only ? self->answer_created : self->remote_answer_set;
  if (!can_add)
    {
      g_message ("handle_ice_candidate: buffering candidate (not ready to add yet)");
      PendingIceCandidate *pending = g_new (PendingIceCandidate, 1);
      pending->candidate = g_strdup (candidate);
      pending->sdp_mline_index = sdp_mline_index;
      pending->sdp_mid = g_strdup (sdp_mid);
      g_ptr_array_add (self->pending_ice_candidates, pending);
      return;
    }

  g_signal_emit_by_name (self->webrtcbin, "add-ice-candidate", sdp_mline_index, candidate);
#endif
}

void
gnalk_webrtc_set_signal_callback (GnalkWebrtc               *self,
                                     GnalkWebrtcSignalCallback  callback,
                                     gpointer                    user_data,
                                     GDestroyNotify              notify)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));

  /* Free the previous user_data if it owned one */
  if (self->signal_callback_notify && self->signal_callback_data &&
      self->signal_callback_data != user_data)
    self->signal_callback_notify (self->signal_callback_data);

  self->signal_callback = callback;
  self->signal_callback_data = user_data;
  self->signal_callback_notify = notify;
}

void
gnalk_webrtc_set_remote_video_callback (GnalkWebrtc                    *self,
                                           GnalkWebrtcRemoteVideoCallback  callback,
                                           gpointer                         user_data,
                                           GDestroyNotify                   notify)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));

  if (self->remote_video_callback_notify && self->remote_video_callback_data &&
      self->remote_video_callback_data != user_data)
    self->remote_video_callback_notify (self->remote_video_callback_data);

  self->remote_video_callback = callback;
  self->remote_video_callback_data = user_data;
  self->remote_video_callback_notify = notify;
}

void
gnalk_webrtc_set_local_video_callback (GnalkWebrtc                   *self,
                                          GnalkWebrtcLocalVideoCallback  callback,
                                          gpointer                        user_data)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));

  self->local_video_callback = callback;
  self->local_video_callback_data = user_data;
}

void
gnalk_webrtc_set_connection_state_callback (GnalkWebrtc                        *self,
                                               GnalkWebrtcConnectionStateCallback  callback,
                                               gpointer                              user_data,
                                               GDestroyNotify                        notify)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));

  if (self->connection_state_callback_notify && self->connection_state_callback_data &&
      self->connection_state_callback_data != user_data)
    self->connection_state_callback_notify (self->connection_state_callback_data);

  self->connection_state_callback = callback;
  self->connection_state_callback_data = user_data;
  self->connection_state_callback_notify = notify;
}

void
gnalk_webrtc_set_speaking_callback (GnalkWebrtc                  *self,
                                       GnalkWebrtcSpeakingCallback   callback,
                                       gpointer                       user_data,
                                       GDestroyNotify                 notify)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));

  if (self->speaking_callback_notify && self->speaking_callback_data &&
      self->speaking_callback_data != user_data)
    self->speaking_callback_notify (self->speaking_callback_data);

  self->speaking_callback = callback;
  self->speaking_callback_data = user_data;
  self->speaking_callback_notify = notify;
}

#ifdef HAVE_GSTREAMER
GstElement *
gnalk_webrtc_get_video_sink (GnalkWebrtc *self)
{
  g_return_val_if_fail (GNALK_IS_WEBRTC (self), NULL);
  return self->video_sink;
}

GstElement *
gnalk_webrtc_get_local_video_sink (GnalkWebrtc *self)
{
  g_return_val_if_fail (GNALK_IS_WEBRTC (self), NULL);
  return self->local_video_sink;
}
#endif

GdkPaintable *
gnalk_webrtc_get_local_video_paintable (GnalkWebrtc *self)
{
  g_return_val_if_fail (GNALK_IS_WEBRTC (self), NULL);

#ifdef HAVE_GSTREAMER
  if (!self->local_video_sink)
    return NULL;

  GdkPaintable *paintable = NULL;
  g_object_get (self->local_video_sink, "paintable", &paintable, NULL);
  return paintable;
#else
  return NULL;
#endif
}

void
gnalk_webrtc_set_video_enabled (GnalkWebrtc *self,
                                   gboolean      enabled)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));
  self->video_enabled = enabled;

  /* Delegate to media pipeline */
  if (self->media_pipeline)
    gnalk_media_pipeline_set_video_enabled (self->media_pipeline, enabled);

  /* Send status via data channel */
  if (self->data_channel_manager)
    gnalk_data_channel_send_video_status (self->data_channel_manager, enabled);
}

void
gnalk_webrtc_set_audio_enabled (GnalkWebrtc *self,
                                   gboolean      enabled)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));
  self->audio_enabled = enabled;

  /* Delegate to media pipeline */
  if (self->media_pipeline)
    gnalk_media_pipeline_set_audio_enabled (self->media_pipeline, enabled);

  /* Send status via data channel */
  if (self->data_channel_manager)
    gnalk_data_channel_send_audio_status (self->data_channel_manager, enabled);
}

void
gnalk_webrtc_set_camera_position (GnalkWebrtc        *self,
                                     GnalkCameraPosition position)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));
  self->camera_position = position;

  /* Forward to media pipeline */
  if (self->media_pipeline)
    gnalk_media_pipeline_set_camera_position (self->media_pipeline, position);
}

GnalkCameraPosition
gnalk_webrtc_get_camera_position (GnalkWebrtc *self)
{
  g_return_val_if_fail (GNALK_IS_WEBRTC (self), GNALK_CAMERA_FRONT);
  return self->camera_position;
}

void
gnalk_webrtc_switch_camera (GnalkWebrtc *self)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));

  /* Delegate to media pipeline */
  if (self->media_pipeline)
    {
      gnalk_media_pipeline_switch_camera (self->media_pipeline);
      self->camera_position = gnalk_media_pipeline_get_camera_position (self->media_pipeline);
    }
  else
    {
      /* No media pipeline - just toggle preference for next call */
      if (self->camera_position == GNALK_CAMERA_FRONT)
        self->camera_position = GNALK_CAMERA_BACK;
      else
        self->camera_position = GNALK_CAMERA_FRONT;
      g_message ("Camera preference switched to %s (will apply on next call)",
                 self->camera_position == GNALK_CAMERA_FRONT ? "front" : "back");
    }
}

void
gnalk_webrtc_request_keyframe (GnalkWebrtc *self)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));

  /* Request keyframe via PLI - this is used when a remote peer unmutes video
   * to ensure we get a fresh keyframe for proper decoding. */
  if (self->quality_controller)
    {
      gboolean sent = gnalk_quality_controller_send_pli (self->quality_controller);
      g_message ("Keyframe request (unmute) sent for webrtc=%p, result=%d", (void *)self, sent);
    }
  else
    {
      g_message ("Cannot request keyframe - no quality controller for webrtc=%p", (void *)self);
    }
}

void
gnalk_webrtc_enable_continuous_pli (GnalkWebrtc *self,
                                      gboolean      enable,
                                      guint         interval_ms)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));

  if (self->quality_controller)
    {
      gnalk_quality_controller_enable_continuous_pli (self->quality_controller, enable, interval_ms);
      g_message ("Continuous PLI: %s for webrtc=%p via quality controller",
                 enable ? "enabled" : "disabled", (void *)self);
    }
}

void
gnalk_webrtc_set_call_state_callback (GnalkWebrtc                  *self,
                                         GnalkWebrtcCallStateCallback  callback,
                                         gpointer                       user_data)
{
  g_return_if_fail (GNALK_IS_WEBRTC (self));

  self->call_state_callback = callback;
  self->call_state_callback_data = user_data;
}

GnalkCallState
gnalk_webrtc_get_call_state (GnalkWebrtc *self)
{
  g_return_val_if_fail (GNALK_IS_WEBRTC (self), GNALK_CALL_STATE_IDLE);
  return self->call_state;
}
