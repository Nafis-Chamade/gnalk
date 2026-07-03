#include "gnalk-push-client.h"
#include "gnalk-log.h"

#include <gio/gio.h>

typedef enum {
  PUSH_STATE_DISCONNECTED,
  PUSH_STATE_CONNECTING,
  PUSH_STATE_CONNECTED,
  PUSH_STATE_SENT_USER,
  PUSH_STATE_SENT_PASSWORD,
  PUSH_STATE_AUTHENTICATED
} PushState;

struct _GnalkPushClient
{
  GObject parent_instance;

  SoupSession             *session;
  SoupWebsocketConnection *ws_connection;
  SoupMessage             *pending_ws_msg;  /* Keep message alive during async connect */
  GCancellable            *connect_cancellable;  /* Cancels pending WebSocket connect on disconnect/finalize */
  char                    *websocket_url;
  char                    *user_id;
  char                    *password;
  PushState                state;
  gboolean                 pending_reconnect;  /* Reconnect after current disconnect completes */
  guint                    reconnect_timeout_id;

  GnalkPushNotificationCallback notification_callback;
  gpointer                       notification_callback_data;
};

G_DEFINE_FINAL_TYPE (GnalkPushClient, gnalk_push_client, G_TYPE_OBJECT)

static void gnalk_push_client_do_connect (GnalkPushClient *self);

static void
gnalk_push_client_finalize (GObject *object)
{
  GnalkPushClient *self = GNALK_PUSH_CLIENT (object);

  if (self->reconnect_timeout_id > 0)
    g_source_remove (self->reconnect_timeout_id);

  gnalk_push_client_disconnect (self);

  g_clear_object (&self->pending_ws_msg);
  g_clear_object (&self->session);
  g_free (self->websocket_url);
  g_free (self->user_id);
  g_free (self->password);

  G_OBJECT_CLASS (gnalk_push_client_parent_class)->finalize (object);
}

static void
gnalk_push_client_class_init (GnalkPushClientClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_push_client_finalize;
}

static void
gnalk_push_client_init (GnalkPushClient *self)
{
  self->session = soup_session_new ();
  self->state = PUSH_STATE_DISCONNECTED;
}

GnalkPushClient *
gnalk_push_client_new (void)
{
  return g_object_new (GNALK_TYPE_PUSH_CLIENT, NULL);
}

static void
send_text_message (GnalkPushClient *self, const char *message, gboolean sensitive)
{
  if (!self->ws_connection ||
      soup_websocket_connection_get_state (self->ws_connection) != SOUP_WEBSOCKET_STATE_OPEN)
    return;

  /* notify_push authenticates by sending the raw user id then the raw
   * app-password as plain-text frames (no JSON envelope to redact) — never
   * echo those credentials to the log. */
  gnalk_log_ws_send (self->user_id, sensitive ? "[redacted credential]" : message);
  soup_websocket_connection_send_text (self->ws_connection, message);
}

static void
on_ws_message (SoupWebsocketConnection *ws,
               SoupWebsocketDataType    type,
               GBytes                  *message,
               gpointer                 user_data)
{
  GnalkPushClient *self = GNALK_PUSH_CLIENT (user_data);

  if (type != SOUP_WEBSOCKET_DATA_TEXT)
    return;

  gsize size;
  const char *data = g_bytes_get_data (message, &size);
  g_autofree char *msg = g_strndup (data, size);

  gnalk_log_ws_recv (self->user_id, msg);

  /* Handle authentication flow */
  if (self->state == PUSH_STATE_SENT_PASSWORD)
    {
      if (g_strcmp0 (msg, "authenticated") == 0)
        {
          self->state = PUSH_STATE_AUTHENTICATED;
          g_message ("Push client authenticated successfully");
        }
      else if (g_str_has_prefix (msg, "err:"))
        {
          g_warning ("Push authentication failed: %s", msg);
          self->state = PUSH_STATE_CONNECTED;
        }
      return;
    }

  /* Handle notifications after authentication */
  if (self->state == PUSH_STATE_AUTHENTICATED)
    {
      /* Notification format is just the notification type as a string */
      if (self->notification_callback)
        self->notification_callback (self, msg, self->notification_callback_data);
    }
}

static gboolean
reconnect_timeout (gpointer user_data)
{
  GnalkPushClient *self = GNALK_PUSH_CLIENT (user_data);

  self->reconnect_timeout_id = 0;
  gnalk_push_client_do_connect (self);

  return G_SOURCE_REMOVE;
}

static void
on_ws_closed (SoupWebsocketConnection *ws,
              gpointer                 user_data)
{
  GnalkPushClient *self = GNALK_PUSH_CLIENT (user_data);

  /* Check if this is the current connection or an old one being closed.
   * This handles the race condition where a new connection is started
   * before the old one finishes closing. */
  if (self->ws_connection != ws)
    {
      g_message ("Push: old WebSocket closed, ignoring (already reconnected)");
      return;
    }

  g_message ("Push WebSocket closed");

  self->state = PUSH_STATE_DISCONNECTED;
  g_clear_object (&self->ws_connection);

  /* Check if we need to reconnect immediately (requested by gnalk_push_client_connect) */
  if (self->pending_reconnect)
    {
      self->pending_reconnect = FALSE;
      g_message ("Push: disconnect complete, now reconnecting");
      gnalk_push_client_do_connect (self);
      return;
    }

  /* Schedule reconnect if we have credentials */
  if (self->websocket_url && self->user_id && self->password)
    {
      if (self->reconnect_timeout_id == 0)
        self->reconnect_timeout_id = g_timeout_add_seconds (5, reconnect_timeout, self);
    }
}

static void
on_ws_error (SoupWebsocketConnection *ws,
             GError                  *error,
             gpointer                 user_data)
{
  g_warning ("Push WebSocket error: %s", error->message);
}

static void
on_ws_connected (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  GnalkPushClient *self = GNALK_PUSH_CLIENT (user_data);
  g_autoptr(GError) error = NULL;

  SoupWebsocketConnection *conn =
    soup_session_websocket_connect_finish (SOUP_SESSION (source), result, &error);

  /* If the connect was cancelled, the client is being disconnected/finalized;
   * self may be mid-teardown, so drop the result and return without touching it. */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_clear_object (&conn);
      return;
    }

  self->ws_connection = conn;

  /* Now safe to release the pending message - handshake is complete */
  g_clear_object (&self->pending_ws_msg);

  if (error)
    {
      g_warning ("Push WebSocket connection failed: %s", error->message);
      self->state = PUSH_STATE_DISCONNECTED;

      /* Schedule reconnect */
      if (self->reconnect_timeout_id == 0)
        self->reconnect_timeout_id = g_timeout_add_seconds (5, reconnect_timeout, self);
      return;
    }

  g_message ("Push WebSocket connected, authenticating...");
  self->state = PUSH_STATE_CONNECTED;

  g_signal_connect (self->ws_connection, "message", G_CALLBACK (on_ws_message), self);
  g_signal_connect (self->ws_connection, "closed", G_CALLBACK (on_ws_closed), self);
  g_signal_connect (self->ws_connection, "error", G_CALLBACK (on_ws_error), self);

  /* Send user_id first */
  send_text_message (self, self->user_id, TRUE);
  self->state = PUSH_STATE_SENT_USER;

  /* Send password immediately after */
  send_text_message (self, self->password, TRUE);
  self->state = PUSH_STATE_SENT_PASSWORD;
}

static void
gnalk_push_client_do_connect (GnalkPushClient *self)
{
  if (!self->websocket_url || !self->user_id || !self->password)
    return;

  if (self->state != PUSH_STATE_DISCONNECTED)
    return;

  self->state = PUSH_STATE_CONNECTING;

  g_message ("Connecting to push server: %s", self->websocket_url);

  /* Clear any previous pending message */
  g_clear_object (&self->pending_ws_msg);

  self->pending_ws_msg = soup_message_new (SOUP_METHOD_GET, self->websocket_url);
  if (!self->pending_ws_msg)
    {
      g_warning ("Failed to create push WebSocket message for URL: %s", self->websocket_url);
      self->state = PUSH_STATE_DISCONNECTED;
      return;
    }

  /* Fresh cancellable for this connect attempt so disconnect/finalize can
   * abort an in-flight handshake and avoid a use-after-free in the callback. */
  g_clear_object (&self->connect_cancellable);
  self->connect_cancellable = g_cancellable_new ();

  soup_session_websocket_connect_async (self->session,
                                         self->pending_ws_msg,
                                         NULL,  /* origin */
                                         NULL,  /* protocols */
                                         G_PRIORITY_DEFAULT,
                                         self->connect_cancellable,
                                         on_ws_connected,
                                         self);
  /* Don't unref msg here - keep it alive until on_ws_connected finishes */
}

void
gnalk_push_client_connect (GnalkPushClient *self,
                             const char       *websocket_url,
                             const char       *user_id,
                             const char       *password)
{
  g_return_if_fail (GNALK_IS_PUSH_CLIENT (self));
  g_return_if_fail (websocket_url != NULL);
  g_return_if_fail (user_id != NULL);
  g_return_if_fail (password != NULL);

  gboolean was_connected = (self->ws_connection != NULL);

  /* Store new credentials first */
  g_free (self->websocket_url);
  g_free (self->user_id);
  g_free (self->password);

  self->websocket_url = g_strdup (websocket_url);
  self->user_id = g_strdup (user_id);
  self->password = g_strdup (password);

  if (was_connected)
    {
      /* Wait for disconnect to complete before reconnecting */
      self->pending_reconnect = TRUE;
      gnalk_push_client_disconnect (self);
    }
  else
    {
      gnalk_push_client_do_connect (self);
    }
}

void
gnalk_push_client_disconnect (GnalkPushClient *self)
{
  g_return_if_fail (GNALK_IS_PUSH_CLIENT (self));

  if (self->reconnect_timeout_id > 0)
    {
      g_source_remove (self->reconnect_timeout_id);
      self->reconnect_timeout_id = 0;
    }

  /* Cancel any pending async WebSocket connect so its callback won't touch us */
  g_cancellable_cancel (self->connect_cancellable);
  g_clear_object (&self->connect_cancellable);

  if (self->ws_connection)
    {
      /* Disconnect the message/closed/error handlers BEFORE closing and clearing.
       * We clear the connection synchronously here, so the "closed" handler must
       * not fire against a half-torn-down client. */
      g_signal_handlers_disconnect_by_data (self->ws_connection, self);

      soup_websocket_connection_close (self->ws_connection,
                                        SOUP_WEBSOCKET_CLOSE_NORMAL,
                                        "Client disconnecting");
      g_clear_object (&self->ws_connection);
    }

  self->state = PUSH_STATE_DISCONNECTED;
}

gboolean
gnalk_push_client_is_connected (GnalkPushClient *self)
{
  g_return_val_if_fail (GNALK_IS_PUSH_CLIENT (self), FALSE);
  return self->state >= PUSH_STATE_CONNECTED;
}

gboolean
gnalk_push_client_is_authenticated (GnalkPushClient *self)
{
  g_return_val_if_fail (GNALK_IS_PUSH_CLIENT (self), FALSE);
  return self->state == PUSH_STATE_AUTHENTICATED;
}

void
gnalk_push_client_set_notification_callback (GnalkPushClient               *self,
                                               GnalkPushNotificationCallback  callback,
                                               gpointer                        user_data)
{
  g_return_if_fail (GNALK_IS_PUSH_CLIENT (self));

  self->notification_callback = callback;
  self->notification_callback_data = user_data;
}
