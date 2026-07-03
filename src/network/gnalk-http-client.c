#include "gnalk-http-client.h"
#include "gnalk-log.h"

struct _GnalkHttpClient
{
  GObject parent_instance;

  SoupSession *session;
  char        *server_url;
  char        *user_id;
  char        *password;
  char        *auth_header;

  /* Guest mode */
  gboolean     is_guest;
  char        *room_token;

  GCancellable *cancellable;       /* For auth/background requests */
  GCancellable *room_cancellable;  /* For room-specific requests (messages, participants) */
};

G_DEFINE_FINAL_TYPE (GnalkHttpClient, gnalk_http_client, G_TYPE_OBJECT)

enum {
  SIGNAL_SERVER_ERROR,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/**
 * gnalk_http_error_type_from_error:
 * @error: (nullable): The GError to categorize
 * @http_status: The HTTP status code (0 if not available)
 *
 * Categorizes an error for offline mode detection.
 *
 * Returns: The error type category
 */
GnalkHttpErrorType
gnalk_http_error_type_from_error (GError *error,
                                    guint   http_status)
{
  /* Check HTTP status first */
  if (http_status >= 500 && http_status < 600)
    return GNALK_HTTP_ERROR_SERVER;
  if (http_status == 401 || http_status == 403)
    return GNALK_HTTP_ERROR_AUTH;

  /* Check GError */
  if (error == NULL)
    return GNALK_HTTP_ERROR_NONE;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
    return GNALK_HTTP_ERROR_TIMEOUT;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED))
    return GNALK_HTTP_ERROR_NETWORK;

  return GNALK_HTTP_ERROR_OTHER;
}

typedef enum
{
  HTTP_CB_JSON,    /* GnalkHttpCallback */
  HTTP_CB_BYTES,   /* GnalkHttpBytesCallback */
  HTTP_CB_EXISTS,  /* GnalkHttpBoolCallback; non-2xx means "does not exist", not an error */
  HTTP_CB_UPLOAD,  /* GnalkHttpBoolCallback; non-2xx is an error */
} HttpCallbackKind;

typedef struct
{
  GWeakRef         client_ref;  /* Weak reference to client */
  SoupMessage     *message;
  char            *url;
  HttpCallbackKind kind;
  GCallback        callback;    /* Signature determined by kind */
  gpointer         user_data;
} HttpCallbackData;

static void
gnalk_http_client_finalize (GObject *object)
{
  GnalkHttpClient *self = GNALK_HTTP_CLIENT (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_cancellable_cancel (self->room_cancellable);
  g_clear_object (&self->room_cancellable);
  g_clear_object (&self->session);
  g_free (self->server_url);
  g_free (self->user_id);
  g_free (self->password);
  g_free (self->auth_header);
  g_free (self->room_token);

  G_OBJECT_CLASS (gnalk_http_client_parent_class)->finalize (object);
}

static void
gnalk_http_client_class_init (GnalkHttpClientClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_http_client_finalize;

  /**
   * GnalkHttpClient::server-error:
   * @client: The HTTP client
   * @error_type: The error type (GnalkHttpErrorType)
   * @error_message: Human-readable error description
   *
   * Emitted when a server error or timeout is detected.
   * The ConnectionManager should connect to this signal to trigger offline mode.
   */
  signals[SIGNAL_SERVER_ERROR] =
    g_signal_new ("server-error",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_INT,     /* GnalkHttpErrorType */
                  G_TYPE_STRING); /* error message */
}

/* Default timeout for all HTTP requests (seconds).
 * Must be longer than long-polling timeout (30s server-side). */
#define GNALK_HTTP_DEFAULT_TIMEOUT 35

static void
gnalk_http_client_init (GnalkHttpClient *self)
{
  /* Create session with construct-only properties.
   * max-conns-per-host (default 2) must be set at construction time.
   * We increase it to allow concurrent requests even when long-polls are active:
   * - 1 for message long-poll (30s timeout)
   * - 1 for call operations (join, leave, etc.)
   * - 1 for other API calls (participants, conversations, heartbeat) */
  self->session = g_object_new (SOUP_TYPE_SESSION,
                                "timeout", GNALK_HTTP_DEFAULT_TIMEOUT,
                                "max-conns-per-host", 6,
                                NULL);
  self->cancellable = g_cancellable_new ();
  self->room_cancellable = g_cancellable_new ();

  SoupCookieJar *jar = soup_cookie_jar_new ();
  soup_session_add_feature (self->session, SOUP_SESSION_FEATURE (jar));
  g_object_unref (jar);
}

GnalkHttpClient *
gnalk_http_client_new (void)
{
  return g_object_new (GNALK_TYPE_HTTP_CLIENT, NULL);
}

void
gnalk_http_client_set_credentials (GnalkHttpClient *self,
                                     const char       *server_url,
                                     const char       *user_id,
                                     const char       *password)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  g_free (self->server_url);
  g_free (self->user_id);
  g_free (self->password);
  g_free (self->auth_header);

  self->server_url = g_strdup (server_url);
  self->user_id = g_strdup (user_id);
  self->password = g_strdup (password);

  if (user_id && password)
    {
      g_autofree char *credentials = g_strdup_printf ("%s:%s", user_id, password);
      g_autofree char *encoded = g_base64_encode ((guchar *)credentials, strlen (credentials));
      self->auth_header = g_strdup_printf ("Basic %s", encoded);
    }

  self->is_guest = FALSE;
  g_clear_pointer (&self->room_token, g_free);
}

void
gnalk_http_client_set_guest_mode (GnalkHttpClient *self,
                                    const char       *server_url,
                                    const char       *room_token)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  g_free (self->server_url);
  g_free (self->user_id);
  g_free (self->password);
  g_free (self->auth_header);
  g_free (self->room_token);

  self->server_url = g_strdup (server_url);
  self->user_id = NULL;
  self->password = NULL;
  self->auth_header = NULL;
  self->room_token = g_strdup (room_token);
  self->is_guest = TRUE;
}

gboolean
gnalk_http_client_is_guest (GnalkHttpClient *self)
{
  g_return_val_if_fail (GNALK_IS_HTTP_CLIENT (self), FALSE);
  return self->is_guest;
}

gboolean
gnalk_http_client_is_ready (GnalkHttpClient *self)
{
  g_return_val_if_fail (GNALK_IS_HTTP_CLIENT (self), FALSE);
  return self->server_url != NULL && self->server_url[0] != '\0';
}

const char *
gnalk_http_client_get_room_token (GnalkHttpClient *self)
{
  g_return_val_if_fail (GNALK_IS_HTTP_CLIENT (self), NULL);
  return self->room_token;
}

static SoupMessage *
create_message (GnalkHttpClient *self,
                const char       *method,
                const char       *endpoint)
{
  g_autofree char *url = NULL;
  SoupMessage *msg;
  SoupMessageHeaders *headers;

  if (g_str_has_prefix (endpoint, "http"))
    url = g_strdup (endpoint);
  else
    url = g_strdup_printf ("%s%s", self->server_url, endpoint);

  gnalk_log_http_request (self->user_id, method, url, NULL);

  msg = soup_message_new (method, url);
  if (!msg)
    {
      g_warning ("Failed to create SoupMessage for URL: %s", url);
      return NULL;
    }

  headers = soup_message_get_request_headers (msg);

  soup_message_headers_append (headers, "OCS-APIREQUEST", "true");
  soup_message_headers_append (headers, "Accept", "application/json");

  if (self->auth_header)
    soup_message_headers_append (headers, "Authorization", self->auth_header);

  return msg;
}

/* Create message for binary/image requests without JSON Accept header */
static SoupMessage *
create_bytes_message (GnalkHttpClient *self,
                      const char       *method,
                      const char       *endpoint)
{
  g_autofree char *url = NULL;
  SoupMessage *msg;
  SoupMessageHeaders *headers;

  if (g_str_has_prefix (endpoint, "http"))
    url = g_strdup (endpoint);
  else
    url = g_strdup_printf ("%s%s", self->server_url, endpoint);

  gnalk_log_http_request (self->user_id, method, url, NULL);

  msg = soup_message_new (method, url);
  if (!msg)
    {
      g_warning ("Failed to create SoupMessage for URL: %s", url);
      return NULL;
    }

  headers = soup_message_get_request_headers (msg);

  if (self->auth_header)
    soup_message_headers_append (headers, "Authorization", self->auth_header);

  return msg;
}

static void
http_callback_data_free (HttpCallbackData *data)
{
  g_weak_ref_clear (&data->client_ref);
  g_clear_object (&data->message);
  g_free (data->url);
  g_free (data);
}

/* Deferred cleanup to avoid freeing SoupMessage while inside libsoup's callback stack.
 * This prevents a race condition where libsoup tries to access the message after
 * we've dropped the last reference. */
static gboolean
http_callback_data_free_idle (gpointer user_data)
{
  http_callback_data_free (user_data);
  return G_SOURCE_REMOVE;
}

static void
http_callback_data_free_deferred (HttpCallbackData *data)
{
  g_idle_add (http_callback_data_free_idle, data);
}

static void on_json_response (GObject *source, GAsyncResult *result, gpointer user_data);
static void on_bytes_response (GObject *source, GAsyncResult *result, gpointer user_data);
static void on_head_response (GObject *source, GAsyncResult *result, gpointer user_data);
static void on_upload_response (GObject *source, GAsyncResult *result, gpointer user_data);

static void
http_callback_invoke_error (GnalkHttpClient  *client,
                            HttpCallbackKind  kind,
                            GCallback         callback,
                            gpointer          user_data,
                            GError           *error)
{
  if (!callback)
    return;

  switch (kind)
    {
    case HTTP_CB_JSON:
      ((GnalkHttpCallback) callback) (client, NULL, error, user_data);
      break;
    case HTTP_CB_BYTES:
      ((GnalkHttpBytesCallback) callback) (client, NULL, NULL, error, user_data);
      break;
    case HTTP_CB_EXISTS:
    case HTTP_CB_UPLOAD:
      ((GnalkHttpBoolCallback) callback) (client, FALSE, error, user_data);
      break;
    default:
      g_assert_not_reached ();
    }
}

/* Central dispatch for every request. Takes ownership of @msg (which may be
 * NULL when construction failed, e.g. malformed server URL).
 *
 * Contract: the user callback is invoked exactly once for every outcome —
 * success, HTTP error, network error, CANCELLATION, or message-creation
 * failure — as long as the client is still alive. Callers rely on this to
 * free their user_data and reset in-flight state; cancellation is reported
 * as G_IO_ERROR_CANCELLED. Only client destruction swallows the callback. */
static void
dispatch_request (GnalkHttpClient  *self,
                  SoupMessage      *msg,
                  const char       *endpoint,
                  HttpCallbackKind  kind,
                  GCancellable     *cancellable,
                  GCallback         callback,
                  gpointer          user_data)
{
  static const GAsyncReadyCallback handlers[] = {
    [HTTP_CB_JSON]   = on_json_response,
    [HTTP_CB_BYTES]  = on_bytes_response,
    [HTTP_CB_EXISTS] = on_head_response,
    [HTTP_CB_UPLOAD] = on_upload_response,
  };
  HttpCallbackData *data;

  if (!msg)
    {
      g_autoptr(GError) error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                                     "Failed to create request");
      http_callback_invoke_error (self, kind, callback, user_data, error);
      return;
    }

  data = g_new0 (HttpCallbackData, 1);
  g_weak_ref_init (&data->client_ref, self);
  data->message = msg;  /* Takes ownership */
  if (g_str_has_prefix (endpoint, "http"))
    data->url = g_strdup (endpoint);
  else
    data->url = g_strdup_printf ("%s%s", self->server_url, endpoint);
  data->kind = kind;
  data->callback = callback;
  data->user_data = user_data;

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT,
                                    cancellable, handlers[kind], data);
}

static void
on_json_response (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  HttpCallbackData *data = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(JsonParser) parser = NULL;
  JsonNode *root = NULL;
  SoupMessage *msg = data->message;
  SoupStatus status;

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);

  /* Get strong reference to client - may have been destroyed */
  GnalkHttpClient *client = g_weak_ref_get (&data->client_ref);
  if (!client)
    {
      g_debug ("HTTP client destroyed during request, discarding response");
      http_callback_data_free_deferred (data);
      return;
    }

  if (error)
    {
      /* Cancellation also reaches the callback (dispatch_request contract):
       * callers depend on it to free user_data and reset in-flight flags. */
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          gnalk_log_http_response (client->user_id, 0, error->message, NULL, data->url);

          /* Emit server-error signal for timeout and network errors */
          GnalkHttpErrorType error_type = gnalk_http_error_type_from_error (error, 0);
          if (error_type == GNALK_HTTP_ERROR_TIMEOUT ||
              error_type == GNALK_HTTP_ERROR_NETWORK)
            {
              g_signal_emit (client, signals[SIGNAL_SERVER_ERROR], 0,
                             (gint) error_type, error->message);
            }
        }

      if (data->callback)
        ((GnalkHttpCallback) data->callback) (client, NULL, error, data->user_data);
      g_object_unref (client);
      http_callback_data_free_deferred (data);
      return;
    }

  status = soup_message_get_status (msg);
  const char *reason = soup_message_get_reason_phrase (msg);

  gsize size;
  const char *body = g_bytes_get_data (bytes, &size);

  gnalk_log_http_response (client->user_id, status, reason, body, data->url);

  if (status == SOUP_STATUS_NOT_MODIFIED || size == 0)
    {
      if (data->callback)
        ((GnalkHttpCallback) data->callback) (client, NULL, NULL, data->user_data);
      g_object_unref (client);
      http_callback_data_free_deferred (data);
      return;
    }

  if (!SOUP_STATUS_IS_SUCCESSFUL (status))
    {
      /* Emit server-error signal for HTTP 5xx errors */
      GnalkHttpErrorType error_type = gnalk_http_error_type_from_error (NULL, status);
      if (error_type == GNALK_HTTP_ERROR_SERVER)
        {
          g_autofree char *error_msg = g_strdup_printf ("HTTP %u: %s", status, reason);
          g_signal_emit (client, signals[SIGNAL_SERVER_ERROR], 0,
                         (gint) error_type, error_msg);
        }

      g_autoptr(GError) status_error = g_error_new (G_IO_ERROR,
                                                     G_IO_ERROR_FAILED,
                                                     "HTTP error %u: %s",
                                                     status,
                                                     reason);
      if (data->callback)
        ((GnalkHttpCallback) data->callback) (client, NULL, status_error, data->user_data);
      g_object_unref (client);
      http_callback_data_free_deferred (data);
      return;
    }

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, body, size, &error))
    {
      if (data->callback)
        ((GnalkHttpCallback) data->callback) (client, NULL, error, data->user_data);
      g_object_unref (client);
      http_callback_data_free_deferred (data);
      return;
    }

  root = json_parser_get_root (parser);
  if (data->callback)
    ((GnalkHttpCallback) data->callback) (client, root, NULL, data->user_data);
  g_object_unref (client);
  http_callback_data_free_deferred (data);
}

static void
on_bytes_response (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  HttpCallbackData *data = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = NULL;
  SoupMessage *msg = data->message;
  SoupStatus status;
  const char *content_type = NULL;

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);

  /* Get strong reference to client - may have been destroyed */
  GnalkHttpClient *client = g_weak_ref_get (&data->client_ref);
  if (!client)
    {
      g_debug ("HTTP client destroyed during bytes request, discarding response");
      http_callback_data_free_deferred (data);
      return;
    }

  if (error)
    {
      /* Cancellation also reaches the callback (dispatch_request contract) */
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          /* Emit server-error signal for timeout and network errors */
          GnalkHttpErrorType error_type = gnalk_http_error_type_from_error (error, 0);
          if (error_type == GNALK_HTTP_ERROR_TIMEOUT ||
              error_type == GNALK_HTTP_ERROR_NETWORK)
            {
              g_signal_emit (client, signals[SIGNAL_SERVER_ERROR], 0,
                             (gint) error_type, error->message);
            }
        }

      if (data->callback)
        ((GnalkHttpBytesCallback) data->callback) (client, NULL, NULL, error, data->user_data);
      g_object_unref (client);
      http_callback_data_free_deferred (data);
      return;
    }

  status = soup_message_get_status (msg);
  const char *reason = soup_message_get_reason_phrase (msg);

  SoupMessageHeaders *headers = soup_message_get_response_headers (msg);
  content_type = soup_message_headers_get_content_type (headers, NULL);

  gsize size = bytes ? g_bytes_get_size (bytes) : 0;
  g_autofree char *log_body = g_strdup_printf ("[binary %zu bytes, %s]",
                                                size, content_type ? content_type : "unknown");
  gnalk_log_http_response (client->user_id, status, reason, log_body, data->url);

  if (!SOUP_STATUS_IS_SUCCESSFUL (status))
    {
      /* Emit server-error signal for HTTP 5xx errors */
      GnalkHttpErrorType error_type = gnalk_http_error_type_from_error (NULL, status);
      if (error_type == GNALK_HTTP_ERROR_SERVER)
        {
          g_autofree char *error_msg = g_strdup_printf ("HTTP %u: %s", status, reason);
          g_signal_emit (client, signals[SIGNAL_SERVER_ERROR], 0,
                         (gint) error_type, error_msg);
        }

      g_autoptr(GError) status_error = g_error_new (G_IO_ERROR,
                                                     G_IO_ERROR_FAILED,
                                                     "HTTP error %u",
                                                     status);
      if (data->callback)
        ((GnalkHttpBytesCallback) data->callback) (client, NULL, NULL, status_error, data->user_data);
      g_object_unref (client);
      http_callback_data_free_deferred (data);
      return;
    }

  if (data->callback)
    ((GnalkHttpBytesCallback) data->callback) (client, bytes, content_type, NULL, data->user_data);
  g_object_unref (client);
  http_callback_data_free_deferred (data);
}

void
gnalk_http_client_get_server_status (GnalkHttpClient   *self,
                                       GnalkHttpCallback  callback,
                                       gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  const char *endpoint = "/status.php";
  SoupMessage *msg = create_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

typedef struct
{
  GCancellable *cancellable;
  guint         timeout_id;
  GCancellable *parent;          /* Client cancellable at request time */
  gulong        parent_handler;  /* Cancel forwarder from parent */
} TimeoutData;

static gboolean
on_timeout_expired (gpointer user_data)
{
  TimeoutData *timeout_data = user_data;
  timeout_data->timeout_id = 0;  /* Mark as removed before cancelling */
  g_cancellable_cancel (timeout_data->cancellable);
  return G_SOURCE_REMOVE;
}

/* Forward cancellation from the client cancellable to a request-private one,
 * so gnalk_http_client_cancel_pending() also aborts timeout-guarded probes. */
static void
on_parent_cancelled (GCancellable *parent G_GNUC_UNUSED,
                     gpointer      user_data)
{
  g_cancellable_cancel (G_CANCELLABLE (user_data));
}

static void
on_json_response_with_timeout (GObject      *source,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  HttpCallbackData *data = user_data;
  TimeoutData *timeout_data = g_object_get_data (G_OBJECT (data->message), "timeout-data");
  gboolean was_timeout = FALSE;

  /* Check if this was a timeout (timeout_id == 0 means timeout fired) */
  if (timeout_data && timeout_data->timeout_id == 0)
    was_timeout = TRUE;

  /* Remove timeout source if still pending */
  if (timeout_data && timeout_data->timeout_id > 0)
    {
      g_source_remove (timeout_data->timeout_id);
      timeout_data->timeout_id = 0;
    }

  /* Clean up timeout data */
  if (timeout_data)
    {
      if (timeout_data->parent)
        {
          g_cancellable_disconnect (timeout_data->parent, timeout_data->parent_handler);
          g_object_unref (timeout_data->parent);
        }
      g_object_unref (timeout_data->cancellable);
      g_free (timeout_data);
      g_object_set_data (G_OBJECT (data->message), "timeout-data", NULL);
    }

  /* If this was a timeout, call callback with timeout error instead of letting
   * on_json_response report a generic cancellation */
  if (was_timeout)
    {
      /* Get strong reference to client - may have been destroyed */
      GnalkHttpClient *client = g_weak_ref_get (&data->client_ref);
      if (!client)
        {
          g_debug ("HTTP client destroyed during timeout request");
          http_callback_data_free_deferred (data);
          return;
        }

      g_autoptr(GError) timeout_error = g_error_new (G_IO_ERROR,
                                                      G_IO_ERROR_TIMED_OUT,
                                                      "Request timed out");

      /* Emit server-error signal for timeout */
      g_signal_emit (client, signals[SIGNAL_SERVER_ERROR], 0,
                     (gint) GNALK_HTTP_ERROR_TIMEOUT, "Request timed out");

      if (data->callback)
        ((GnalkHttpCallback) data->callback) (client, NULL, timeout_error, data->user_data);
      g_object_unref (client);
      http_callback_data_free_deferred (data);
      return;
    }

  /* Now call the regular json response handler */
  on_json_response (source, result, user_data);
}

void
gnalk_http_client_get_server_status_with_timeout (GnalkHttpClient   *self,
                                                    guint               timeout_seconds,
                                                    GnalkHttpCallback  callback,
                                                    gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  const char *endpoint = "/status.php";
  SoupMessage *msg = create_message (self, "GET", endpoint);

  if (!msg)
    {
      g_autoptr(GError) error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                                     "Failed to create request");
      if (callback)
        callback (self, NULL, error, user_data);
      return;
    }

  HttpCallbackData *data = g_new0 (HttpCallbackData, 1);
  g_weak_ref_init (&data->client_ref, self);
  data->message = msg;  /* Takes ownership */
  data->url = g_strdup_printf ("%s%s", self->server_url, endpoint);
  data->kind = HTTP_CB_JSON;
  data->callback = (GCallback) callback;
  data->user_data = user_data;

  /* Create a separate cancellable for this request with timeout, chained to
   * the client cancellable so cancel_pending() also aborts this probe. */
  TimeoutData *timeout_data = g_new0 (TimeoutData, 1);
  timeout_data->cancellable = g_cancellable_new ();
  timeout_data->timeout_id = g_timeout_add_seconds (timeout_seconds, on_timeout_expired, timeout_data);
  timeout_data->parent = g_object_ref (self->cancellable);
  timeout_data->parent_handler = g_cancellable_connect (self->cancellable,
                                                        G_CALLBACK (on_parent_cancelled),
                                                        g_object_ref (timeout_data->cancellable),
                                                        g_object_unref);

  /* Store timeout data on message for cleanup */
  g_object_set_data (G_OBJECT (msg), "timeout-data", timeout_data);

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT,
                                     timeout_data->cancellable, on_json_response_with_timeout, data);
}

void
gnalk_http_client_get_user_info (GnalkHttpClient   *self,
                                   GnalkHttpCallback  callback,
                                   gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  const char *endpoint = "/ocs/v2.php/cloud/user?format=json";
  SoupMessage *msg = create_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_get_conversations (GnalkHttpClient   *self,
                                       GnalkHttpCallback  callback,
                                       gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  /* includeStatus=true returns user status for 1:1 conversations */
  const char *endpoint = "/ocs/v2.php/apps/spreed/api/v4/room?format=json&includeStatus=true";
  SoupMessage *msg = create_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_get_capabilities (GnalkHttpClient   *self,
                                      GnalkHttpCallback  callback,
                                      gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  const char *endpoint = "/ocs/v2.php/cloud/capabilities?format=json";
  SoupMessage *msg = create_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_get_signaling_settings (GnalkHttpClient   *self,
                                            GnalkHttpCallback  callback,
                                            gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  const char *endpoint = "/ocs/v2.php/apps/spreed/api/v3/signaling/settings?format=json";
  SoupMessage *msg = create_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_get_messages (GnalkHttpClient   *self,
                                  const char         *room_token,
                                  gint64              last_known_message_id,
                                  gboolean            look_into_future,
                                  gint                timeout,
                                  GnalkHttpCallback  callback,
                                  gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_message ("HTTP: get_messages room=%s lastKnownId=%" G_GINT64_FORMAT " lookIntoFuture=%d timeout=%d",
             room_token, last_known_message_id, look_into_future, timeout);

  g_autofree char *endpoint = NULL;

  if (look_into_future)
    {
      /* lookIntoFuture=1 gets messages AFTER lastKnownMessageId.
       * timeout=0 returns immediately, timeout>0 long-polls for new messages. */
      endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v1/chat/%s"
                                   "?format=json&lookIntoFuture=1&lastKnownMessageId=%ld&timeout=%d&setReadMarker=0",
                                   room_token, last_known_message_id, timeout);
    }
  else
    {
      /* lookIntoFuture=0 gets messages BEFORE lastKnownMessageId (history).
       * When lastKnownMessageId=0, gets the most recent messages. */
      endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v1/chat/%s"
                                   "?format=json&lookIntoFuture=0&lastKnownMessageId=%ld&limit=50",
                                   room_token, last_known_message_id);
    }

  SoupMessage *msg = create_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->room_cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_get_message_context (GnalkHttpClient  *self,
                                       const char       *room_token,
                                       gint64            message_id,
                                       gint              limit,
                                       GnalkHttpCallback callback,
                                       gpointer          user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (message_id > 0);

  g_message ("HTTP: get_message_context room=%s messageId=%" G_GINT64_FORMAT " limit=%d",
             room_token, message_id, limit);

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v1/chat/%s/%" G_GINT64_FORMAT "/context?format=json&limit=%d",
    room_token, message_id, limit);

  SoupMessage *msg = create_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->room_cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_get_thread_messages (GnalkHttpClient  *self,
                                       const char       *room_token,
                                       gint64            thread_id,
                                       gint64            last_known_id,
                                       gint              limit,
                                       GnalkHttpCallback callback,
                                       gpointer          user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (thread_id > 0);

  g_message ("HTTP: get_thread_messages room=%s threadId=%" G_GINT64_FORMAT
             " lastKnownId=%" G_GINT64_FORMAT " limit=%d",
             room_token, thread_id, last_known_id, limit);

  g_autofree char *endpoint = NULL;
  if (last_known_id > 0)
    endpoint = g_strdup_printf (
      "/ocs/v2.php/apps/spreed/api/v1/chat/%s"
      "?format=json&lookIntoFuture=0&lastKnownMessageId=%" G_GINT64_FORMAT
      "&threadId=%" G_GINT64_FORMAT "&limit=%d",
      room_token, last_known_id, thread_id, limit);
  else
    endpoint = g_strdup_printf (
      "/ocs/v2.php/apps/spreed/api/v1/chat/%s"
      "?format=json&lookIntoFuture=0&threadId=%" G_GINT64_FORMAT "&limit=%d",
      room_token, thread_id, limit);

  SoupMessage *msg = create_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->room_cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_send_thread_message (GnalkHttpClient  *self,
                                       const char       *room_token,
                                       const char       *message,
                                       gint64            thread_id,
                                       gint64            reply_to,
                                       const char       *reference_id,
                                       GnalkHttpCallback callback,
                                       gpointer          user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (message != NULL);
  g_return_if_fail (thread_id > 0);

  g_autofree char *endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v1/chat/%s?format=json",
                                                room_token);
  SoupMessage *msg = create_message (self, "POST", endpoint);

  g_autofree char *encoded_message = g_uri_escape_string (message, NULL, TRUE);
  g_autofree char *body = NULL;

  if (reply_to > 0 && reference_id && *reference_id)
    body = g_strdup_printf ("message=%s&threadId=%" G_GINT64_FORMAT "&replyTo=%" G_GINT64_FORMAT "&referenceId=%s",
                            encoded_message, thread_id, reply_to, reference_id);
  else if (reference_id && *reference_id)
    body = g_strdup_printf ("message=%s&threadId=%" G_GINT64_FORMAT "&referenceId=%s",
                            encoded_message, thread_id, reference_id);
  else
    body = g_strdup_printf ("message=%s&threadId=%" G_GINT64_FORMAT,
                            encoded_message, thread_id);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_create_thread (GnalkHttpClient  *self,
                                 const char       *room_token,
                                 const char       *message,
                                 const char       *thread_title,
                                 const char       *reference_id,
                                 GnalkHttpCallback callback,
                                 gpointer          user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v1/chat/%s?format=json",
                                                room_token);
  SoupMessage *msg = create_message (self, "POST", endpoint);

  g_autofree char *encoded_message = g_uri_escape_string (message ? message : "", NULL, TRUE);
  g_autofree char *encoded_title = thread_title ? g_uri_escape_string (thread_title, NULL, TRUE) : NULL;

  /* NOTE: The Nextcloud API requires replyTo=0 for thread creation:
   * $createThread = $replyTo === 0 && $threadTitle !== ''
   * So we NEVER send replyTo here — it would prevent thread creation. */
  GString *body_str = g_string_new (NULL);
  g_string_append_printf (body_str, "message=%s", encoded_message);
  if (encoded_title && *encoded_title)
    g_string_append_printf (body_str, "&threadTitle=%s", encoded_title);
  if (reference_id && *reference_id)
    g_string_append_printf (body_str, "&referenceId=%s", reference_id);

  g_autofree char *body = g_string_free (body_str, FALSE);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_poll_thread_messages (GnalkHttpClient  *self,
                                        const char       *room_token,
                                        gint64            thread_id,
                                        gint64            last_known_id,
                                        gint              timeout,
                                        GnalkHttpCallback callback,
                                        gpointer          user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (thread_id > 0);

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v1/chat/%s"
    "?format=json&lookIntoFuture=1&lastKnownMessageId=%" G_GINT64_FORMAT
    "&threadId=%" G_GINT64_FORMAT "&timeout=%d&setReadMarker=0",
    room_token, last_known_id, thread_id, timeout);

  SoupMessage *msg = create_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->room_cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_search_messages (GnalkHttpClient  *self,
                                   const char       *room_token,
                                   const char       *search_term,
                                   gint              limit,
                                   gint              cursor,
                                   GnalkHttpCallback callback,
                                   gpointer          user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (search_term != NULL);

  g_message ("HTTP: search_messages room=%s term='%s' limit=%d cursor=%d",
             room_token, search_term, limit, cursor);

  g_autofree char *encoded_term = g_uri_escape_string (search_term, NULL, TRUE);
  g_autofree char *endpoint = NULL;

  if (cursor > 0)
    endpoint = g_strdup_printf (
      "/ocs/v2.php/search/providers/talk-message-current/search?term=%s&from=/call/%s&limit=%d&cursor=%d",
      encoded_term, room_token, limit, cursor);
  else
    endpoint = g_strdup_printf (
      "/ocs/v2.php/search/providers/talk-message-current/search?term=%s&from=/call/%s&limit=%d",
      encoded_term, room_token, limit);

  SoupMessage *msg = create_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_send_message (GnalkHttpClient   *self,
                                  const char         *room_token,
                                  const char         *message,
                                  gint64              reply_to,
                                  const char         *reference_id,
                                  GnalkHttpCallback  callback,
                                  gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (message != NULL);

  g_autofree char *endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v1/chat/%s?format=json",
                                                room_token);

  SoupMessage *msg = create_message (self, "POST", endpoint);

  /* URL-encode the message */
  g_autofree char *encoded_message = g_uri_escape_string (message, NULL, TRUE);

  g_autofree char *body = NULL;
  if (reply_to > 0 && reference_id && *reference_id)
    body = g_strdup_printf ("message=%s&replyTo=%ld&referenceId=%s", encoded_message, (long) reply_to, reference_id);
  else if (reply_to > 0)
    body = g_strdup_printf ("message=%s&replyTo=%ld", encoded_message, (long) reply_to);
  else if (reference_id && *reference_id)
    body = g_strdup_printf ("message=%s&referenceId=%s", encoded_message, reference_id);
  else
    body = g_strdup_printf ("message=%s", encoded_message);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_edit_message (GnalkHttpClient   *self,
                                  const char         *room_token,
                                  gint64              message_id,
                                  const char         *message,
                                  GnalkHttpCallback  callback,
                                  gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (message != NULL);
  g_return_if_fail (message_id > 0);

  g_autofree char *endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v1/chat/%s/%ld?format=json",
                                                room_token, (long) message_id);

  SoupMessage *msg = create_message (self, "PUT", endpoint);

  /* URL-encode the message */
  g_autofree char *encoded_message = g_uri_escape_string (message, NULL, TRUE);
  g_autofree char *body = g_strdup_printf ("message=%s", encoded_message);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_delete_message (GnalkHttpClient   *self,
                                    const char         *room_token,
                                    gint64              message_id,
                                    GnalkHttpCallback  callback,
                                    gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (message_id > 0);

  g_autofree char *endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v1/chat/%s/%ld?format=json",
                                                room_token, (long) message_id);

  SoupMessage *msg = create_message (self, "DELETE", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_mark_chat_read (GnalkHttpClient   *self,
                                    const char         *room_token,
                                    gint64              last_read_message_id,
                                    GnalkHttpCallback  callback,
                                    gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (last_read_message_id > 0);

  g_autofree char *endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v1/chat/%s/read?format=json",
                                                room_token);

  SoupMessage *msg = create_message (self, "POST", endpoint);

  g_autofree char *body = g_strdup_printf ("lastReadMessage=%ld", (long) last_read_message_id);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_join_room (GnalkHttpClient   *self,
                               const char         *room_token,
                               GnalkHttpCallback  callback,
                               gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v4/room/%s/participants/active?format=json",
                                                room_token);

  SoupMessage *msg = create_message (self, "POST", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_leave_room (GnalkHttpClient   *self,
                                const char         *room_token,
                                GnalkHttpCallback  callback,
                                gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v4/room/%s/participants/active?format=json",
                                                room_token);

  SoupMessage *msg = create_message (self, "DELETE", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_get_participants (GnalkHttpClient   *self,
                                      const char         *room_token,
                                      GnalkHttpCallback  callback,
                                      gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_message ("HTTP: get_participants room=%s", room_token);

  g_autofree char *endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v4/room/%s/participants?format=json&includeStatus=true",
                                                room_token);

  SoupMessage *msg = create_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->room_cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_start_call (GnalkHttpClient   *self,
                                const char         *room_token,
                                gint                flags,
                                GnalkHttpCallback  callback,
                                gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v4/call/%s?format=json",
                                                room_token);

  SoupMessage *msg = create_message (self, "POST", endpoint);

  g_autofree char *body = g_strdup_printf ("flags=%d", flags);
  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_leave_call (GnalkHttpClient   *self,
                                const char         *room_token,
                                GnalkHttpCallback  callback,
                                gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v4/call/%s?format=json",
                                                room_token);

  SoupMessage *msg = create_message (self, "DELETE", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_get_room_avatar (GnalkHttpClient        *self,
                                     const char              *room_token,
                                     GnalkHttpBytesCallback  callback,
                                     gpointer                 user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v1/room/%s/avatar",
                                                room_token);

  SoupMessage *msg = create_bytes_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_BYTES, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_get_user_avatar (GnalkHttpClient        *self,
                                     const char              *user_id,
                                     int                      size,
                                     GnalkHttpBytesCallback  callback,
                                     gpointer                 user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (user_id != NULL);

  g_autofree char *endpoint = g_strdup_printf ("/avatar/%s/%d", user_id, size);

  SoupMessage *msg = create_bytes_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_BYTES, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_get_federated_user_avatar (GnalkHttpClient        *self,
                                                const char              *room_token,
                                                const char              *cloud_id,
                                                int                      size,
                                                GnalkHttpBytesCallback  callback,
                                                gpointer                 user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (cloud_id != NULL);

  g_autofree char *encoded_cloud_id = g_uri_escape_string (cloud_id, NULL, FALSE);
  g_autofree char *endpoint = g_strdup_printf (
      "/ocs/v2.php/apps/spreed/api/v1/proxy/%s/user-avatar/%d?cloudId=%s&darkTheme=0",
      room_token, size, encoded_cloud_id);

  SoupMessage *msg = create_bytes_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_BYTES, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_get_file_preview (GnalkHttpClient        *self,
                                      const char              *file_id,
                                      int                      width,
                                      int                      height,
                                      GnalkHttpBytesCallback  callback,
                                      gpointer                 user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (file_id != NULL);

  g_autofree char *endpoint = g_strdup_printf ("/core/preview?fileId=%s&x=%d&y=%d&a=1",
                                                file_id, width, height);

  SoupMessage *msg = create_bytes_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_BYTES, self->cancellable,
                    (GCallback) callback, user_data);
}

/* Build a /remote.php/dav/files/<user>/<path> endpoint with both the user id
 * and each path segment percent-escaped (path separators preserved). Without
 * this, names like "My File (1).png" produce malformed WebDAV URLs. */
static char *
build_dav_files_endpoint (const char *user_id, const char *path)
{
  g_autofree char *euser = g_uri_escape_string (user_id ? user_id : "", NULL, FALSE);
  g_autofree char *epath = g_uri_escape_string (path ? path : "", "/", FALSE);
  return g_strdup_printf ("/remote.php/dav/files/%s%s%s",
                          euser, (epath[0] == '/') ? "" : "/", epath);
}

void
gnalk_http_client_download_file (GnalkHttpClient        *self,
                                   const char              *file_path,
                                   GnalkHttpBytesCallback  callback,
                                   gpointer                 user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (file_path != NULL);

  /* WebDAV endpoint for file download (user id + path percent-escaped) */
  g_autofree char *endpoint = build_dav_files_endpoint (self->user_id, file_path);

  SoupMessage *msg = create_bytes_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_BYTES, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_cancel_pending (GnalkHttpClient *self)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  /* Cancel GCancellables so pending requests are cancelled and callbacks
   * know to bail out early. We DON'T destroy/recreate the SoupSession
   * because that can cause crashes if libsoup still has internal cleanup
   * pending from the cancelled requests. */
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  g_cancellable_cancel (self->room_cancellable);
  g_clear_object (&self->room_cancellable);
  self->room_cancellable = g_cancellable_new ();
}

void
gnalk_http_client_cancel_room_requests (GnalkHttpClient *self)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  g_cancellable_cancel (self->room_cancellable);
  g_clear_object (&self->room_cancellable);
  self->room_cancellable = g_cancellable_new ();
}

void
gnalk_http_client_add_reaction (GnalkHttpClient   *self,
                                  const char         *room_token,
                                  gint64              message_id,
                                  const char         *reaction,
                                  GnalkHttpCallback  callback,
                                  gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (reaction != NULL);

  g_autofree char *endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v1/reaction/%s/%ld",
                                                room_token, (long) message_id);

  SoupMessage *msg = create_message (self, "POST", endpoint);

  /* Build JSON payload: {"reaction":"👍"} — via JsonBuilder so quotes/backslashes
   * in the reaction are escaped rather than breaking (or injecting) the body. */
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "reaction");
  json_builder_add_string_value (builder, reaction);
  json_builder_end_object (builder);
  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  g_autoptr(JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, root);
  g_autofree char *body = json_generator_to_data (gen, NULL);
  gnalk_log_http_request (self->user_id, "POST", endpoint, body);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/json", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_set_user_status (GnalkHttpClient   *self,
                                     const char         *status_type,
                                     GnalkHttpCallback  callback,
                                     gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (status_type != NULL);

  const char *endpoint = "/ocs/v2.php/apps/user_status/api/v1/user_status/status?format=json";

  SoupMessage *msg = create_message (self, "PUT", endpoint);

  g_autoptr(JsonBuilder) builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "statusType");
  json_builder_add_string_value (builder, status_type);
  json_builder_end_object (builder);
  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  g_autoptr(JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, root);
  g_autofree char *body = json_generator_to_data (gen, NULL);
  gnalk_log_http_request (self->user_id, "PUT", endpoint, body);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/json", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_send_heartbeat (GnalkHttpClient   *self,
                                    GnalkHttpCallback  callback,
                                    gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  const char *endpoint = "/ocs/v2.php/apps/user_status/api/v1/heartbeat?format=json";

  SoupMessage *msg = create_message (self, "PUT", endpoint);

  /* Heartbeat requires status parameter */
  const char *body = "{\"status\":\"online\"}";
  gnalk_log_http_request (self->user_id, "PUT", endpoint, body);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/json", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_search_contacts (GnalkHttpClient   *self,
                                     const char         *search_term,
                                     gboolean            include_federated,
                                     GnalkHttpCallback  callback,
                                     gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  /* Search for users AND conversations using core autocomplete API
   * shareTypes[]=0 means regular users, shareTypes[]=10 means Talk conversations
   * shareTypes[]=6 means remote/federated users
   * itemId=new for new conversation context */
  g_autofree char *escaped_term = g_uri_escape_string (search_term ? search_term : "", NULL, FALSE);
  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/core/autocomplete/get?format=json&search=%s&itemType=call&itemId=new&shareTypes[]=0&shareTypes[]=10%s&limit=20",
    escaped_term,
    include_federated ? "&shareTypes[]=6" : "");

  SoupMessage *msg = create_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_create_conversation (GnalkHttpClient   *self,
                                         int                 room_type,
                                         const char         *invite,
                                         const char         *room_name,
                                         GnalkHttpCallback  callback,
                                         gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (invite != NULL);

  const char *endpoint = "/ocs/v2.php/apps/spreed/api/v4/room?format=json";

  SoupMessage *msg = create_message (self, "POST", endpoint);

  /* Build JSON payload */
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "roomType");
  json_builder_add_int_value (builder, room_type);
  json_builder_set_member_name (builder, "invite");
  json_builder_add_string_value (builder, invite);
  if (room_name && *room_name)
    {
      json_builder_set_member_name (builder, "roomName");
      json_builder_add_string_value (builder, room_name);
    }
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  g_autoptr(JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, root);
  g_autofree char *body = json_generator_to_data (gen, NULL);

  gnalk_log_http_request (self->user_id, "POST", endpoint, body);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/json", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_create_conversation_with_participants (GnalkHttpClient   *self,
                                                          int                 room_type,
                                                          const char         *room_name,
                                                          const char         *participants_source,
                                                          const char         *participant_id,
                                                          GnalkHttpCallback  callback,
                                                          gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (participants_source != NULL);
  g_return_if_fail (participant_id != NULL);

  const char *endpoint = "/ocs/v2.php/apps/spreed/api/v4/room?format=json";

  SoupMessage *msg = create_message (self, "POST", endpoint);

  /* Build JSON payload: { roomType, roomName, participants: { source: [id] } } */
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "roomType");
  json_builder_add_int_value (builder, room_type);
  if (room_name && *room_name)
    {
      json_builder_set_member_name (builder, "roomName");
      json_builder_add_string_value (builder, room_name);
    }
  json_builder_set_member_name (builder, "participants");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, participants_source);
  json_builder_begin_array (builder);
  json_builder_add_string_value (builder, participant_id);
  json_builder_end_array (builder);
  json_builder_end_object (builder);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  g_autoptr(JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, root);
  g_autofree char *body = json_generator_to_data (gen, NULL);

  gnalk_log_http_request (self->user_id, "POST", endpoint, body);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/json", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_leave_conversation (GnalkHttpClient   *self,
                                        const char         *room_token,
                                        GnalkHttpCallback  callback,
                                        gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v4/room/%s/participants/self?format=json", room_token);

  SoupMessage *msg = create_message (self, "DELETE", endpoint);

  gnalk_log_http_request (self->user_id, "DELETE", endpoint, NULL);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_delete_conversation (GnalkHttpClient   *self,
                                         const char         *room_token,
                                         GnalkHttpCallback  callback,
                                         gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v4/room/%s?format=json", room_token);

  SoupMessage *msg = create_message (self, "DELETE", endpoint);

  gnalk_log_http_request (self->user_id, "DELETE", endpoint, NULL);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_set_conversation_favorite (GnalkHttpClient   *self,
                                               const char         *room_token,
                                               gboolean            favorite,
                                               GnalkHttpCallback  callback,
                                               gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v4/room/%s/favorite?format=json", room_token);

  const char *method = favorite ? "POST" : "DELETE";
  SoupMessage *msg = create_message (self, method, endpoint);

  gnalk_log_http_request (self->user_id, method, endpoint, NULL);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_search_users (GnalkHttpClient   *self,
                                  const char         *search_term,
                                  gboolean            include_federated,
                                  GnalkHttpCallback  callback,
                                  gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  /* Users only: shareTypes[]=0 (no rooms/conversations)
   * shareTypes[]=6 means remote/federated users */
  g_autofree char *escaped_term = g_uri_escape_string (search_term ? search_term : "", NULL, FALSE);
  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/core/autocomplete/get?format=json&search=%s&itemType=call&itemId=new&shareTypes[]=0%s&limit=20",
    escaped_term,
    include_federated ? "&shareTypes[]=6" : "");

  SoupMessage *msg = create_message (self, "GET", endpoint);

  gnalk_log_http_request (self->user_id, "GET", endpoint, NULL);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_set_room_description (GnalkHttpClient   *self,
                                          const char         *room_token,
                                          const char         *description,
                                          GnalkHttpCallback  callback,
                                          gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v4/room/%s/description?format=json", room_token);

  SoupMessage *msg = create_message (self, "PUT", endpoint);

  /* Build JSON payload */
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "description");
  json_builder_add_string_value (builder, description ? description : "");
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  g_autoptr(JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, root);
  g_autofree char *body = json_generator_to_data (gen, NULL);

  gnalk_log_http_request (self->user_id, "PUT", endpoint, body);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/json", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_add_participant (GnalkHttpClient   *self,
                                     const char         *room_token,
                                     const char         *user_id,
                                     const char         *source,
                                     GnalkHttpCallback  callback,
                                     gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (user_id != NULL);

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v4/room/%s/participants?format=json", room_token);

  SoupMessage *msg = create_message (self, "POST", endpoint);

  /* Build JSON payload */
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "newParticipant");
  json_builder_add_string_value (builder, user_id);
  json_builder_set_member_name (builder, "source");
  json_builder_add_string_value (builder, source ? source : "users");
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  g_autoptr(JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, root);
  g_autofree char *body = json_generator_to_data (gen, NULL);

  gnalk_log_http_request (self->user_id, "POST", endpoint, body);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/json", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

/* ============ Guest Mode API ============ */

void
gnalk_http_client_get_room_info (GnalkHttpClient   *self,
                                   const char         *room_token,
                                   GnalkHttpCallback  callback,
                                   gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v4/room/%s?format=json",
                                                room_token);

  SoupMessage *msg = create_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_join_room_as_guest (GnalkHttpClient   *self,
                                        const char         *room_token,
                                        const char         *password,
                                        GnalkHttpCallback  callback,
                                        gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v4/room/%s/participants/active?format=json",
                                                room_token);

  SoupMessage *msg = create_message (self, "POST", endpoint);

  /* Build JSON payload with optional password */
  g_autoptr(JsonBuilder) builder2 = json_builder_new ();
  json_builder_begin_object (builder2);
  json_builder_set_member_name (builder2, "force");
  json_builder_add_boolean_value (builder2, TRUE);
  if (password && *password)
    {
      json_builder_set_member_name (builder2, "password");
      json_builder_add_string_value (builder2, password);
    }
  json_builder_end_object (builder2);

  g_autoptr(JsonNode) root2 = json_builder_get_root (builder2);
  g_autoptr(JsonGenerator) gen2 = json_generator_new ();
  json_generator_set_root (gen2, root2);
  g_autofree char *body2 = json_generator_to_data (gen2, NULL);

  gnalk_log_http_request (self->user_id, "POST", endpoint, body2);

  GBytes *body_bytes2 = g_bytes_new (body2, strlen (body2));
  soup_message_set_request_body_from_bytes (msg, "application/json", body_bytes2);
  g_bytes_unref (body_bytes2);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_set_guest_name (GnalkHttpClient   *self,
                                    const char         *room_token,
                                    const char         *display_name,
                                    GnalkHttpCallback  callback,
                                    gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (display_name != NULL);

  /* Note: This uses API v1 for guest name endpoint */
  g_autofree char *endpoint = g_strdup_printf ("/ocs/v2.php/apps/spreed/api/v1/guest/%s/name?format=json",
                                                room_token);

  SoupMessage *msg = create_message (self, "POST", endpoint);

  /* Build JSON payload */
  g_autoptr(JsonBuilder) builder3 = json_builder_new ();
  json_builder_begin_object (builder3);
  json_builder_set_member_name (builder3, "displayName");
  json_builder_add_string_value (builder3, display_name);
  json_builder_end_object (builder3);

  g_autoptr(JsonNode) root3 = json_builder_get_root (builder3);
  g_autoptr(JsonGenerator) gen3 = json_generator_new ();
  json_generator_set_root (gen3, root3);
  g_autofree char *body3 = json_generator_to_data (gen3, NULL);

  gnalk_log_http_request (self->user_id, "POST", endpoint, body3);

  GBytes *body_bytes3 = g_bytes_new (body3, strlen (body3));
  soup_message_set_request_body_from_bytes (msg, "application/json", body_bytes3);
  g_bytes_unref (body_bytes3);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

/* ============ File Upload API ============ */

const char *
gnalk_http_client_get_user_id (GnalkHttpClient *self)
{
  g_return_val_if_fail (GNALK_IS_HTTP_CLIENT (self), NULL);
  return self->user_id;
}

static void
on_head_response (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  HttpCallbackData *data = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = NULL;
  SoupMessage *msg = data->message;
  SoupStatus status;

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);

  /* Get strong reference to client - may have been destroyed */
  GnalkHttpClient *client = g_weak_ref_get (&data->client_ref);
  if (!client)
    {
      g_debug ("HTTP client destroyed during HEAD request, discarding response");
      http_callback_data_free_deferred (data);
      return;
    }

  if (error)
    {
      /* Cancellation also reaches the callback (dispatch_request contract) */
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        gnalk_log_http_response (client->user_id, 0, error->message, NULL, data->url);
      if (data->callback)
        ((GnalkHttpBoolCallback) data->callback) (client, FALSE, error, data->user_data);
      g_object_unref (client);
      http_callback_data_free_deferred (data);
      return;
    }

  status = soup_message_get_status (msg);
  const char *reason = soup_message_get_reason_phrase (msg);

  gnalk_log_http_response (client->user_id, status, reason, NULL, data->url);

  /* File exists if we get 200, doesn't exist if 404 */
  gboolean exists = SOUP_STATUS_IS_SUCCESSFUL (status);

  if (data->callback)
    ((GnalkHttpBoolCallback) data->callback) (client, exists, NULL, data->user_data);
  g_object_unref (client);
  http_callback_data_free_deferred (data);
}

void
gnalk_http_client_check_file_exists (GnalkHttpClient      *self,
                                       const char            *remote_path,
                                       GnalkHttpBoolCallback callback,
                                       gpointer               user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (remote_path != NULL);

  /* WebDAV HEAD request to check file existence (user id + path escaped) */
  g_autofree char *endpoint = build_dav_files_endpoint (self->user_id, remote_path);

  SoupMessage *msg = create_bytes_message (self, "HEAD", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_EXISTS, self->cancellable,
                    (GCallback) callback, user_data);
}

static void
on_upload_response (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  HttpCallbackData *data = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = NULL;
  SoupMessage *msg = data->message;
  SoupStatus status;

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);

  /* Get strong reference to client - may have been destroyed */
  GnalkHttpClient *client = g_weak_ref_get (&data->client_ref);
  if (!client)
    {
      g_debug ("HTTP client destroyed during upload request, discarding response");
      http_callback_data_free_deferred (data);
      return;
    }

  if (error)
    {
      /* Cancellation also reaches the callback (dispatch_request contract) */
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        gnalk_log_http_response (client->user_id, 0, error->message, NULL, data->url);
      if (data->callback)
        ((GnalkHttpBoolCallback) data->callback) (client, FALSE, error, data->user_data);
      g_object_unref (client);
      http_callback_data_free_deferred (data);
      return;
    }

  status = soup_message_get_status (msg);
  const char *reason = soup_message_get_reason_phrase (msg);

  /* Log response body on failure to aid debugging (e.g., avatar upload 400) */
  const char *body_str = NULL;
  g_autofree char *body_text = NULL;
  if (bytes && !SOUP_STATUS_IS_SUCCESSFUL (status))
    {
      gsize body_size;
      gconstpointer body_data = g_bytes_get_data (bytes, &body_size);
      if (body_data && body_size > 0 && body_size < 4096)
        {
          body_text = g_strndup (body_data, body_size);
          body_str = body_text;
        }
    }

  gnalk_log_http_response (client->user_id, status, reason, body_str, data->url);

  /* 201 Created or 204 No Content means success */
  gboolean success = (status == SOUP_STATUS_CREATED || status == SOUP_STATUS_NO_CONTENT ||
                      SOUP_STATUS_IS_SUCCESSFUL (status));

  if (!success)
    {
      g_autoptr(GError) status_error = g_error_new (G_IO_ERROR,
                                                     G_IO_ERROR_FAILED,
                                                     "Upload failed: HTTP %u %s",
                                                     status, reason);
      if (data->callback)
        ((GnalkHttpBoolCallback) data->callback) (client, FALSE, status_error, data->user_data);
      g_object_unref (client);
      http_callback_data_free_deferred (data);
      return;
    }

  if (data->callback)
    ((GnalkHttpBoolCallback) data->callback) (client, TRUE, NULL, data->user_data);
  g_object_unref (client);
  http_callback_data_free_deferred (data);
}

void
gnalk_http_client_upload_file (GnalkHttpClient      *self,
                                 const char            *remote_path,
                                 GBytes                *file_bytes,
                                 const char            *content_type,
                                 GnalkHttpBoolCallback callback,
                                 gpointer               user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (remote_path != NULL);
  g_return_if_fail (file_bytes != NULL);

  /* WebDAV PUT request to upload file (user id + path escaped) */
  g_autofree char *endpoint = build_dav_files_endpoint (self->user_id, remote_path);

  SoupMessage *msg = create_bytes_message (self, "PUT", endpoint);
  if (!msg)
    {
      g_autoptr(GError) error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create request");
      if (callback)
        callback (self, FALSE, error, user_data);
      return;
    }

  /* Set content type */
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);
  if (content_type)
    soup_message_headers_set_content_type (headers, content_type, NULL);

  gsize size = g_bytes_get_size (file_bytes);
  g_autofree char *log_body = g_strdup_printf ("[binary %zu bytes]", size);
  gnalk_log_http_request (self->user_id, "PUT", endpoint, log_body);

  /* Set request body */
  soup_message_set_request_body_from_bytes (msg, content_type, file_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_UPLOAD, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_share_to_conversation (GnalkHttpClient   *self,
                                           const char         *remote_path,
                                           const char         *room_token,
                                           const char         *caption,
                                           gint64              reply_to,
                                           const char         *reference_id,
                                           const char         *message_type,
                                           GnalkHttpCallback  callback,
                                           gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (remote_path != NULL);
  g_return_if_fail (room_token != NULL);

  const char *endpoint = "/ocs/v2.php/apps/files_sharing/api/v1/shares?format=json";

  SoupMessage *msg = create_message (self, "POST", endpoint);

  /* Build talkMetaData JSON */
  g_autoptr(JsonBuilder) meta_builder = json_builder_new ();
  json_builder_begin_object (meta_builder);
  if (caption && *caption)
    {
      json_builder_set_member_name (meta_builder, "caption");
      json_builder_add_string_value (meta_builder, caption);
    }
  if (reply_to > 0)
    {
      json_builder_set_member_name (meta_builder, "replyTo");
      g_autofree char *reply_str = g_strdup_printf ("%ld", reply_to);
      json_builder_add_string_value (meta_builder, reply_str);
    }
  if (message_type && *message_type)
    {
      json_builder_set_member_name (meta_builder, "messageType");
      json_builder_add_string_value (meta_builder, message_type);
    }
  json_builder_end_object (meta_builder);

  g_autoptr(JsonNode) meta_root = json_builder_get_root (meta_builder);
  g_autoptr(JsonGenerator) meta_gen = json_generator_new ();
  json_generator_set_root (meta_gen, meta_root);
  g_autofree char *talk_meta_data = json_generator_to_data (meta_gen, NULL);

  /* Build form-encoded body */
  g_autofree char *escaped_path = g_uri_escape_string (remote_path, NULL, FALSE);
  g_autofree char *escaped_meta = g_uri_escape_string (talk_meta_data, NULL, FALSE);
  g_autofree char *body = NULL;

  if (reference_id && *reference_id)
    {
      g_autofree char *escaped_ref = g_uri_escape_string (reference_id, NULL, FALSE);
      body = g_strdup_printf ("shareType=10&path=%s&shareWith=%s&talkMetaData=%s&referenceId=%s",
                               escaped_path, room_token, escaped_meta, escaped_ref);
    }
  else
    {
      body = g_strdup_printf ("shareType=10&path=%s&shareWith=%s&talkMetaData=%s",
                               escaped_path, room_token, escaped_meta);
    }

  gnalk_log_http_request (self->user_id, "POST", endpoint, body);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

/* ============ Room Settings API ============ */

void
gnalk_http_client_set_room_name (GnalkHttpClient   *self,
                                   const char         *room_token,
                                   const char         *name,
                                   GnalkHttpCallback  callback,
                                   gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v4/room/%s?format=json", room_token);

  SoupMessage *msg = create_message (self, "PUT", endpoint);

  /* Build JSON payload */
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "roomName");
  json_builder_add_string_value (builder, name ? name : "");
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  g_autoptr(JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, root);
  g_autofree char *body = json_generator_to_data (gen, NULL);

  gnalk_log_http_request (self->user_id, "PUT", endpoint, body);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/json", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_upload_room_avatar (GnalkHttpClient      *self,
                                        const char            *room_token,
                                        GBytes                *avatar_bytes,
                                        const char            *content_type,
                                        GnalkHttpBoolCallback callback,
                                        gpointer               user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (avatar_bytes != NULL);

  g_autofree char *url = g_strdup_printf (
    "%s/ocs/v2.php/apps/spreed/api/v1/room/%s/avatar", self->server_url, room_token);

  /* Create multipart form data */
  SoupMultipart *multipart = soup_multipart_new ("multipart/form-data");

  /* Add the file field */
  soup_multipart_append_form_file (multipart, "file", "avatar",
                                    content_type ? content_type : "image/png",
                                    avatar_bytes);

  /* Create message from multipart (libsoup3 API) */
  SoupMessage *msg = soup_message_new_from_multipart (url, multipart);
  soup_multipart_free (multipart);

  if (!msg)
    {
      g_autoptr(GError) error = g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                               "Failed to create multipart message");
      if (callback)
        callback (self, FALSE, error, user_data);
      return;
    }

  /* Add auth header */
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);
  soup_message_headers_append (headers, "OCS-APIREQUEST", "true");
  if (self->auth_header)
    soup_message_headers_append (headers, "Authorization", self->auth_header);

  gsize size = g_bytes_get_size (avatar_bytes);
  g_autofree char *log_body = g_strdup_printf ("[multipart avatar %zu bytes]", size);
  gnalk_log_http_request (self->user_id, "POST", url, log_body);

  dispatch_request (self, msg, url, HTTP_CB_UPLOAD, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_set_notification_level (GnalkHttpClient   *self,
                                            const char         *room_token,
                                            gint                level,
                                            GnalkHttpCallback  callback,
                                            gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v4/room/%s/notify?format=json", room_token);

  SoupMessage *msg = create_message (self, "POST", endpoint);

  /* Build JSON payload - level: 0=default, 1=all, 2=mentions, 3=never */
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "level");
  json_builder_add_int_value (builder, level);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  g_autoptr(JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, root);
  g_autofree char *body = json_generator_to_data (gen, NULL);

  gnalk_log_http_request (self->user_id, "POST", endpoint, body);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/json", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_set_call_notifications (GnalkHttpClient   *self,
                                            const char         *room_token,
                                            gint                level,
                                            GnalkHttpCallback  callback,
                                            gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v4/room/%s/notify-calls?format=json", room_token);

  SoupMessage *msg = create_message (self, "POST", endpoint);

  /* Build JSON payload - level: 0=off, 1=on */
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "level");
  json_builder_add_int_value (builder, level);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  g_autoptr(JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, root);
  g_autofree char *body = json_generator_to_data (gen, NULL);

  gnalk_log_http_request (self->user_id, "POST", endpoint, body);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/json", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_get_shared_items (GnalkHttpClient   *self,
                                      const char         *room_token,
                                      const char         *object_type G_GNUC_UNUSED,
                                      gint                limit,
                                      GnalkHttpCallback  callback,
                                      gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  /* Use the overview endpoint which returns all shared item types */
  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v1/chat/%s/share/overview?format=json&limit=%d",
    room_token, limit > 0 ? limit : 7);

  SoupMessage *msg = create_message (self, "GET", endpoint);

  gnalk_log_http_request (self->user_id, "GET", endpoint, NULL);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_remove_attendee (GnalkHttpClient   *self,
                                     const char         *room_token,
                                     gint64              attendee_id,
                                     GnalkHttpCallback  callback,
                                     gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v4/room/%s/attendees?format=json&attendeeId=%" G_GINT64_FORMAT,
    room_token, attendee_id);

  SoupMessage *msg = create_message (self, "DELETE", endpoint);

  gnalk_log_http_request (self->user_id, "DELETE", endpoint, NULL);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_promote_moderator (GnalkHttpClient   *self,
                                       const char         *room_token,
                                       gint64              attendee_id,
                                       GnalkHttpCallback  callback,
                                       gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v4/room/%s/moderators?format=json", room_token);

  SoupMessage *msg = create_message (self, "POST", endpoint);

  /* Build JSON payload */
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "attendeeId");
  json_builder_add_int_value (builder, attendee_id);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  g_autoptr(JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, root);
  g_autofree char *body = json_generator_to_data (gen, NULL);

  gnalk_log_http_request (self->user_id, "POST", endpoint, body);

  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/json", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_demote_moderator (GnalkHttpClient   *self,
                                      const char         *room_token,
                                      gint64              attendee_id,
                                      GnalkHttpCallback  callback,
                                      gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v4/room/%s/moderators?format=json&attendeeId=%" G_GINT64_FORMAT,
    room_token, attendee_id);

  SoupMessage *msg = create_message (self, "DELETE", endpoint);

  gnalk_log_http_request (self->user_id, "DELETE", endpoint, NULL);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

/* Room-specific signaling settings (for federated calls) */

void
gnalk_http_client_get_signaling_settings_for_room (GnalkHttpClient   *self,
                                                   const char         *room_token,
                                                   GnalkHttpCallback  callback,
                                                   gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v3/signaling/settings?token=%s&format=json",
    room_token);
  SoupMessage *msg = create_message (self, "GET", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

/* Federated call API */

void
gnalk_http_client_start_call_federation (GnalkHttpClient   *self,
                                         const char         *room_token,
                                         gint                flags,
                                         GnalkHttpCallback  callback,
                                         gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v4/call/%s/federation?format=json",
    room_token);

  SoupMessage *msg = create_message (self, "POST", endpoint);

  g_autofree char *body = g_strdup_printf ("flags=%d", flags);
  GBytes *body_bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded", body_bytes);
  g_bytes_unref (body_bytes);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_leave_call_federation (GnalkHttpClient   *self,
                                         const char         *room_token,
                                         GnalkHttpCallback  callback,
                                         gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v4/call/%s/federation?format=json",
    room_token);

  SoupMessage *msg = create_message (self, "DELETE", endpoint);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

/* Subscribed threads API */

void
gnalk_http_client_get_subscribed_threads (GnalkHttpClient  *self,
                                          gint              limit,
                                          gint              offset,
                                          GnalkHttpCallback callback,
                                          gpointer          user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  g_autofree char *endpoint = g_strdup_printf (
      "/ocs/v2.php/apps/spreed/api/v1/chat/subscribed-threads?format=json&limit=%d&offset=%d",
      limit, offset);

  SoupMessage *msg = create_message (self, "GET", endpoint);

  gnalk_log_http_request (self->user_id, "GET", endpoint, NULL);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

/* Federation invitation API */

void
gnalk_http_client_get_federation_invitations (GnalkHttpClient   *self,
                                              GnalkHttpCallback  callback,
                                              gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  const char *endpoint = "/ocs/v2.php/apps/spreed/api/v1/federation/invitation?format=json";

  SoupMessage *msg = create_message (self, "GET", endpoint);

  gnalk_log_http_request (self->user_id, "GET", endpoint, NULL);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_accept_federation_invitation (GnalkHttpClient   *self,
                                                gint64              invitation_id,
                                                GnalkHttpCallback  callback,
                                                gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v1/federation/invitation/%" G_GINT64_FORMAT "?format=json",
    invitation_id);

  SoupMessage *msg = create_message (self, "POST", endpoint);

  gnalk_log_http_request (self->user_id, "POST", endpoint, NULL);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}

void
gnalk_http_client_decline_federation_invitation (GnalkHttpClient   *self,
                                                 gint64              invitation_id,
                                                 GnalkHttpCallback  callback,
                                                 gpointer            user_data)
{
  g_return_if_fail (GNALK_IS_HTTP_CLIENT (self));

  g_autofree char *endpoint = g_strdup_printf (
    "/ocs/v2.php/apps/spreed/api/v1/federation/invitation/%" G_GINT64_FORMAT "?format=json",
    invitation_id);

  SoupMessage *msg = create_message (self, "DELETE", endpoint);

  gnalk_log_http_request (self->user_id, "DELETE", endpoint, NULL);

  dispatch_request (self, msg, endpoint, HTTP_CB_JSON, self->cancellable,
                    (GCallback) callback, user_data);
}
