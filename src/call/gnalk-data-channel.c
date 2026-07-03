/* gnalk-data-channel.c
 *
 * WebRTC data channel manager for status message exchange.
 */

#include "gnalk-data-channel.h"

#include <json-glib/json-glib.h>

#ifdef HAVE_GSTREAMER_WEBRTC
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#endif

struct _GnalkDataChannel
{
  GObject parent_instance;

  GObject *channel;         /* Our local data channel */
  GObject *remote_channel;  /* Remote-created data channel (for receiving) */

  /* Periodic status timer */
  guint    status_timer_id;
  gboolean cached_video_enabled;
  gboolean cached_audio_enabled;
};

G_DEFINE_FINAL_TYPE (GnalkDataChannel, gnalk_data_channel, G_TYPE_OBJECT)

enum
{
  SIGNAL_SPEAKING_CHANGED,
  SIGNAL_CHANNEL_OPENED,
  SIGNAL_CHANNEL_CLOSED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void send_status_message (GnalkDataChannel *self,
                                 const char        *status_type);
static void disconnect_channel  (GnalkDataChannel *self);

/* Check if channel is open */
static gboolean
channel_is_open (GObject *channel)
{
#ifdef HAVE_GSTREAMER_WEBRTC
  if (!channel)
    return FALSE;

  GstWebRTCDataChannelState ready_state;
  g_object_get (channel, "ready-state", &ready_state, NULL);
  return ready_state == GST_WEBRTC_DATA_CHANNEL_STATE_OPEN;
#else
  return FALSE;
#endif
}

/* Send a JSON message on the data channel */
static void
send_status_message (GnalkDataChannel *self,
                     const char        *status_type)
{
  if (!self->channel || !channel_is_open (self->channel))
    return;

  g_autofree char *message = g_strdup_printf ("{\"type\":\"%s\"}", status_type);
  g_signal_emit_by_name (self->channel, "send-string", message);
}

/* Data channel signals ("on-open"/"on-close"/"on-message-string") are emitted
 * on the webrtcbin/data-channel streaming thread. All work that touches
 * main-thread-owned state (signal emission into speaking/active-speaker
 * handling, sending status messages) must be marshalled to the main thread.
 * We use a GWeakRef (thread-safe, unlike g_object_add_weak_pointer) to the
 * owner so a pending idle survives disposal safely. */
typedef struct {
  GWeakRef  self_ref;
  char     *message;   /* dup'd; NULL for open/close events */
} DataChannelEvent;

static DataChannelEvent *
data_channel_event_new (GnalkDataChannel *self,
                        const char        *message)
{
  DataChannelEvent *ev = g_new0 (DataChannelEvent, 1);
  g_weak_ref_init (&ev->self_ref, self);
  ev->message = g_strdup (message);
  return ev;
}

static void
data_channel_event_free (DataChannelEvent *ev)
{
  g_weak_ref_clear (&ev->self_ref);
  g_free (ev->message);
  g_free (ev);
}

/* Main-thread half of the "on-open" handler */
static gboolean
on_channel_open_main (gpointer user_data)
{
  DataChannelEvent *ev = user_data;
  GnalkDataChannel *self = g_weak_ref_get (&ev->self_ref);

  if (self)
    {
      /* Send initial status */
      send_status_message (self, self->cached_video_enabled ? "videoOn" : "videoOff");
      send_status_message (self, self->cached_audio_enabled ? "audioOn" : "audioOff");

      g_signal_emit (self, signals[SIGNAL_CHANNEL_OPENED], 0);
      g_object_unref (self);
    }

  data_channel_event_free (ev);
  return G_SOURCE_REMOVE;
}

/* Callback when data channel is opened (streaming thread) */
static void
on_channel_open (GObject  *channel,
                 gpointer  user_data)
{
  (void)channel;
  GnalkDataChannel *self = GNALK_DATA_CHANNEL (user_data);

  g_message ("Data channel opened");

  g_idle_add (on_channel_open_main, data_channel_event_new (self, NULL));
}

/* Callback for data channel errors */
static void
on_channel_error (GObject  *channel,
                  GError   *error,
                  gpointer  user_data)
{
  (void)channel;
  (void)user_data;
  g_warning ("Data channel error: %s", error ? error->message : "unknown");
}

/* Main-thread half of the "on-close" handler */
static gboolean
on_channel_close_main (gpointer user_data)
{
  DataChannelEvent *ev = user_data;
  GnalkDataChannel *self = g_weak_ref_get (&ev->self_ref);

  if (self)
    {
      g_signal_emit (self, signals[SIGNAL_CHANNEL_CLOSED], 0);
      g_object_unref (self);
    }

  data_channel_event_free (ev);
  return G_SOURCE_REMOVE;
}

/* Callback for data channel close (streaming thread) */
static void
on_channel_close (GObject  *channel,
                  gpointer  user_data)
{
  (void)channel;
  GnalkDataChannel *self = GNALK_DATA_CHANNEL (user_data);

  g_message ("Data channel closed");

  g_idle_add (on_channel_close_main, data_channel_event_new (self, NULL));
}

/* Main-thread half of the "on-message-string" handler. Parsing logic is
 * identical to the original; only its execution (and the signal emission it
 * drives) has moved onto the main thread. */
static gboolean
on_channel_message_main (gpointer user_data)
{
  DataChannelEvent *ev = user_data;
  GnalkDataChannel *self = g_weak_ref_get (&ev->self_ref);

  if (self && ev->message)
    {
      /* Parse JSON message */
      g_autoptr (JsonParser) parser = json_parser_new ();
      if (json_parser_load_from_data (parser, ev->message, -1, NULL))
        {
          JsonNode *root = json_parser_get_root (parser);
          if (JSON_NODE_HOLDS_OBJECT (root))
            {
              JsonObject *obj = json_node_get_object (root);
              const char *type = json_object_get_string_member_with_default (obj, "type", NULL);

              /* Handle speaking status messages */
              if (g_strcmp0 (type, "speaking") == 0)
                g_signal_emit (self, signals[SIGNAL_SPEAKING_CHANGED], 0, TRUE);
              else if (g_strcmp0 (type, "stoppedSpeaking") == 0)
                g_signal_emit (self, signals[SIGNAL_SPEAKING_CHANGED], 0, FALSE);
            }
        }
    }

  if (self)
    g_object_unref (self);

  data_channel_event_free (ev);
  return G_SOURCE_REMOVE;
}

/* Callback for incoming data channel messages (streaming thread) */
static void
on_channel_message_string (GObject  *channel,
                           gchar    *message,
                           gpointer  user_data)
{
  (void)channel;
  GnalkDataChannel *self = GNALK_DATA_CHANNEL (user_data);

  g_debug ("Data channel message received: %s", message ? message : "(null)");

  if (!message)
    return;

  g_idle_add (on_channel_message_main, data_channel_event_new (self, message));
}

/* Timer callback to periodically send status messages */
static gboolean
status_timer_callback (gpointer user_data)
{
  GnalkDataChannel *self = GNALK_DATA_CHANNEL (user_data);

  if (self->channel && channel_is_open (self->channel))
    {
      send_status_message (self, self->cached_video_enabled ? "videoOn" : "videoOff");
      send_status_message (self, self->cached_audio_enabled ? "audioOn" : "audioOff");
    }

  return G_SOURCE_CONTINUE;
}

/* Disconnect signal handlers from a channel */
static void
disconnect_channel (GnalkDataChannel *self)
{
  if (self->channel)
    {
      g_signal_handlers_disconnect_by_data (self->channel, self);
      g_clear_object (&self->channel);
    }

  if (self->remote_channel)
    {
      g_signal_handlers_disconnect_by_data (self->remote_channel, self);
      g_clear_object (&self->remote_channel);
    }
}

static void
gnalk_data_channel_dispose (GObject *object)
{
  GnalkDataChannel *self = GNALK_DATA_CHANNEL (object);

  gnalk_data_channel_stop_periodic_status (self);
  disconnect_channel (self);

  G_OBJECT_CLASS (gnalk_data_channel_parent_class)->dispose (object);
}

static void
gnalk_data_channel_class_init (GnalkDataChannelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gnalk_data_channel_dispose;

  /**
   * GnalkDataChannel::speaking-changed:
   * @self: the data channel manager
   * @speaking: TRUE if remote peer started speaking
   *
   * Emitted when a speaking/stoppedSpeaking message is received.
   */
  signals[SIGNAL_SPEAKING_CHANGED] =
    g_signal_new ("speaking-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  /**
   * GnalkDataChannel::channel-opened:
   * @self: the data channel manager
   *
   * Emitted when the data channel becomes open.
   */
  signals[SIGNAL_CHANNEL_OPENED] =
    g_signal_new ("channel-opened",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);

  /**
   * GnalkDataChannel::channel-closed:
   * @self: the data channel manager
   *
   * Emitted when the data channel is closed.
   */
  signals[SIGNAL_CHANNEL_CLOSED] =
    g_signal_new ("channel-closed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);
}

static void
gnalk_data_channel_init (GnalkDataChannel *self)
{
  self->cached_video_enabled = TRUE;
  self->cached_audio_enabled = TRUE;
}

GnalkDataChannel *
gnalk_data_channel_new (void)
{
  return g_object_new (GNALK_TYPE_DATA_CHANNEL, NULL);
}

void
gnalk_data_channel_set_channel (GnalkDataChannel *self,
                                 GObject           *channel)
{
  g_return_if_fail (GNALK_IS_DATA_CHANNEL (self));

  /* Disconnect from old channel */
  if (self->channel)
    {
      g_signal_handlers_disconnect_by_data (self->channel, self);
      g_clear_object (&self->channel);
    }

  if (!channel)
    return;

  self->channel = g_object_ref (channel);

  /* Connect to channel signals */
  g_signal_connect (channel, "on-open",
                    G_CALLBACK (on_channel_open), self);
  g_signal_connect (channel, "on-error",
                    G_CALLBACK (on_channel_error), self);
  g_signal_connect (channel, "on-close",
                    G_CALLBACK (on_channel_close), self);
  g_signal_connect (channel, "on-message-string",
                    G_CALLBACK (on_channel_message_string), self);

  gchar *label = NULL;
  g_object_get (channel, "label", &label, NULL);
  g_message ("Data channel attached: label=%s", label ? label : "(null)");
  g_free (label);
}

void
gnalk_data_channel_set_remote_channel (GnalkDataChannel *self,
                                        GObject           *channel)
{
  g_return_if_fail (GNALK_IS_DATA_CHANNEL (self));
  g_return_if_fail (channel != NULL);

  /* Store reference to remote channel */
  if (self->remote_channel)
    {
      g_signal_handlers_disconnect_by_data (self->remote_channel, self);
      g_clear_object (&self->remote_channel);
    }

  self->remote_channel = g_object_ref (channel);

  /* Connect to receive messages from remote channel */
  g_signal_connect (channel, "on-message-string",
                    G_CALLBACK (on_channel_message_string), self);

  gchar *label = NULL;
  g_object_get (channel, "label", &label, NULL);
  g_message ("Remote data channel attached: label=%s", label ? label : "(null)");
  g_free (label);
}

void
gnalk_data_channel_send_video_status (GnalkDataChannel *self,
                                       gboolean           enabled)
{
  g_return_if_fail (GNALK_IS_DATA_CHANNEL (self));

  self->cached_video_enabled = enabled;
  send_status_message (self, enabled ? "videoOn" : "videoOff");
}

void
gnalk_data_channel_send_audio_status (GnalkDataChannel *self,
                                       gboolean           enabled)
{
  g_return_if_fail (GNALK_IS_DATA_CHANNEL (self));

  self->cached_audio_enabled = enabled;
  send_status_message (self, enabled ? "audioOn" : "audioOff");
}

void
gnalk_data_channel_start_periodic_status (GnalkDataChannel *self,
                                           gboolean           video_enabled,
                                           gboolean           audio_enabled)
{
  g_return_if_fail (GNALK_IS_DATA_CHANNEL (self));

  if (self->status_timer_id > 0)
    return; /* Already running */

  self->cached_video_enabled = video_enabled;
  self->cached_audio_enabled = audio_enabled;

  /* Send status every 2 seconds to ensure new subscribers receive it */
  self->status_timer_id = g_timeout_add_seconds (2, status_timer_callback, self);
}

void
gnalk_data_channel_stop_periodic_status (GnalkDataChannel *self)
{
  g_return_if_fail (GNALK_IS_DATA_CHANNEL (self));

  if (self->status_timer_id > 0)
    {
      g_source_remove (self->status_timer_id);
      self->status_timer_id = 0;
    }
}

void
gnalk_data_channel_update_status (GnalkDataChannel *self,
                                   gboolean           video_enabled,
                                   gboolean           audio_enabled)
{
  g_return_if_fail (GNALK_IS_DATA_CHANNEL (self));

  gboolean video_changed = (self->cached_video_enabled != video_enabled);
  gboolean audio_changed = (self->cached_audio_enabled != audio_enabled);

  self->cached_video_enabled = video_enabled;
  self->cached_audio_enabled = audio_enabled;

  /* Send updates immediately for changed states */
  if (video_changed)
    send_status_message (self, video_enabled ? "videoOn" : "videoOff");
  if (audio_changed)
    send_status_message (self, audio_enabled ? "audioOn" : "audioOff");
}

gboolean
gnalk_data_channel_is_open (GnalkDataChannel *self)
{
  g_return_val_if_fail (GNALK_IS_DATA_CHANNEL (self), FALSE);

  return channel_is_open (self->channel);
}
