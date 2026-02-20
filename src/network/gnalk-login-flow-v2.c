/* gnalk-login-flow-v2.c
 *
 * Nextcloud Login Flow v2 protocol implementation.
 *
 * Protocol:
 * 1. POST $server/index.php/login/v2 → { poll: { token, endpoint }, login: URL }
 * 2. Open login URL in browser
 * 3. Poll endpoint with POST token=... every 2s
 * 4. HTTP 404 = not ready (retry), HTTP 200 = { server, loginName, appPassword }
 * 5. 5min global timeout
 */

#include "gnalk-login-flow-v2.h"
#include "gnalk-async-op.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include <glib/gi18n.h>

#define POLL_INTERVAL_MS   2000
#define GLOBAL_TIMEOUT_S   300  /* 5 minutes */

typedef enum {
  FLOW_STATE_IDLE,
  FLOW_STATE_INIT,
  FLOW_STATE_POLLING
} FlowState;

struct _GnalkLoginFlowV2
{
  GObject parent_instance;

  SoupSession  *session;
  GCancellable *cancellable;
  FlowState     state;
  guint         generation;

  char *server_url;
  char *poll_endpoint;
  char *poll_token;
  char *login_url;

  guint poll_timer_id;
  guint timeout_timer_id;
};

G_DEFINE_FINAL_TYPE (GnalkLoginFlowV2, gnalk_login_flow_v2, G_TYPE_OBJECT)

enum
{
  SIGNAL_COMPLETED,
  SIGNAL_FAILED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void start_polling       (GnalkLoginFlowV2 *self);
static void reset_state         (GnalkLoginFlowV2 *self);

/* Callback data for poll requests that need HTTP status */
typedef struct {
  GnalkAsyncOp *op;
  SoupMessage  *message;  /* borrowed from soup_session_send_and_read_async */
} PollCallbackData;

static void
poll_callback_data_free (PollCallbackData *data)
{
  if (!data)
    return;
  gnalk_async_op_free (data->op);
  g_object_unref (data->message);
  g_free (data);
}

static char *
normalize_server_url (const char *url)
{
  if (!url || !*url)
    return NULL;

  g_autofree char *trimmed = g_strstrip (g_strdup (url));
  if (!*trimmed)
    return NULL;

  /* Add https:// if no scheme */
  char *normalized;
  if (!g_str_has_prefix (trimmed, "http://") &&
      !g_str_has_prefix (trimmed, "https://"))
    normalized = g_strdup_printf ("https://%s", trimmed);
  else
    normalized = g_strdup (trimmed);

  /* Remove trailing slashes */
  gsize len = strlen (normalized);
  while (len > 0 && normalized[len - 1] == '/')
    normalized[--len] = '\0';

  return normalized;
}

static void
emit_failed (GnalkLoginFlowV2 *self,
             const char        *message)
{
  reset_state (self);
  g_signal_emit (self, signals[SIGNAL_FAILED], 0, message);
}

static void
emit_completed (GnalkLoginFlowV2 *self,
                const char        *server_url,
                const char        *login_name,
                const char        *app_password)
{
  reset_state (self);
  g_signal_emit (self, signals[SIGNAL_COMPLETED], 0,
                 server_url, login_name, app_password);
}

static void
reset_state (GnalkLoginFlowV2 *self)
{
  self->state = FLOW_STATE_IDLE;
  self->generation++;

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  if (self->poll_timer_id > 0)
    {
      g_source_remove (self->poll_timer_id);
      self->poll_timer_id = 0;
    }

  if (self->timeout_timer_id > 0)
    {
      g_source_remove (self->timeout_timer_id);
      self->timeout_timer_id = 0;
    }

  g_clear_pointer (&self->server_url, g_free);
  g_clear_pointer (&self->poll_endpoint, g_free);
  g_clear_pointer (&self->poll_token, g_free);
  g_clear_pointer (&self->login_url, g_free);
}

/* ============ Global timeout ============ */

static gboolean
on_global_timeout (gpointer user_data)
{
  GnalkLoginFlowV2 *self = GNALK_LOGIN_FLOW_V2 (user_data);

  self->timeout_timer_id = 0;
  emit_failed (self, _("Authentication timed out. Please try again."));

  return G_SOURCE_REMOVE;
}

/* ============ Poll response handling ============ */

static void
on_poll_response (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  PollCallbackData *data = user_data;
  GnalkAsyncOp *op = data->op;
  SoupMessage *msg = data->message;

  if (!gnalk_async_op_is_valid (op))
    {
      poll_callback_data_free (data);
      return;
    }

  GnalkLoginFlowV2 *self = GNALK_LOGIN_FLOW_V2 (gnalk_async_op_get_owner (op));
  poll_callback_data_free (data);

  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source),
                                                     result, &error);

  guint status = soup_message_get_status (msg);

  if (error)
    {
      g_error_free (error);
      /* Network error during polling — don't fail immediately, user may still be
       * in the browser. Just schedule the next poll. */
      if (self->state == FLOW_STATE_POLLING)
        start_polling (self);
      g_object_unref (self);
      return;
    }

  if (status == 404)
    {
      /* Not ready yet — schedule next poll */
      g_bytes_unref (bytes);
      if (self->state == FLOW_STATE_POLLING)
        start_polling (self);
      g_object_unref (self);
      return;
    }

  if (status != 200)
    {
      g_bytes_unref (bytes);
      g_autofree char *err = g_strdup_printf (_("Server returned HTTP %u"), status);
      emit_failed (self, err);
      g_object_unref (self);
      return;
    }

  /* HTTP 200 — parse credentials */
  g_autoptr(JsonParser) parser = json_parser_new ();
  gsize size;
  const char *body = g_bytes_get_data (bytes, &size);

  if (!json_parser_load_from_data (parser, body, (gssize) size, &error))
    {
      g_bytes_unref (bytes);
      g_autofree char *err = g_strdup_printf (_("Invalid response: %s"),
                                               error->message);
      g_error_free (error);
      emit_failed (self, err);
      g_object_unref (self);
      return;
    }

  g_bytes_unref (bytes);

  JsonNode *root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT (root))
    {
      emit_failed (self, _("Invalid response from server"));
      g_object_unref (self);
      return;
    }

  JsonObject *obj = json_node_get_object (root);
  const char *server_url = json_object_get_string_member_with_default (obj, "server", NULL);
  const char *login_name = json_object_get_string_member_with_default (obj, "loginName", NULL);
  const char *app_password = json_object_get_string_member_with_default (obj, "appPassword", NULL);

  if (!server_url || !login_name || !app_password)
    {
      emit_failed (self, _("Incomplete credentials from server"));
      g_object_unref (self);
      return;
    }

  emit_completed (self, server_url, login_name, app_password);
  g_object_unref (self);
}

/* ============ Poll request ============ */

static void
send_poll_request (GnalkLoginFlowV2 *self)
{
  if (self->state != FLOW_STATE_POLLING)
    return;

  SoupMessage *msg = soup_message_new ("POST", self->poll_endpoint);
  if (!msg)
    {
      emit_failed (self, _("Invalid poll endpoint URL"));
      return;
    }

  /* Set form body: token=... */
  g_autofree char *form_body = soup_form_encode ("token", self->poll_token, NULL);
  GBytes *body_bytes = g_bytes_new (form_body, strlen (form_body));
  soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded",
                                            body_bytes);
  g_bytes_unref (body_bytes);

  PollCallbackData *data = g_new0 (PollCallbackData, 1);
  data->op = gnalk_async_op_new (G_OBJECT (self), &self->generation, NULL, 0);
  data->message = g_object_ref (msg);

  soup_session_send_and_read_async (self->session, msg,
                                    G_PRIORITY_DEFAULT,
                                    self->cancellable,
                                    on_poll_response, data);
  g_object_unref (msg);
}

static gboolean
on_poll_timer (gpointer user_data)
{
  GnalkLoginFlowV2 *self = GNALK_LOGIN_FLOW_V2 (user_data);

  self->poll_timer_id = 0;
  send_poll_request (self);

  return G_SOURCE_REMOVE;
}

static void
start_polling (GnalkLoginFlowV2 *self)
{
  if (self->state != FLOW_STATE_POLLING)
    return;

  /* Schedule next poll after interval */
  if (self->poll_timer_id > 0)
    g_source_remove (self->poll_timer_id);

  self->poll_timer_id = g_timeout_add (POLL_INTERVAL_MS, on_poll_timer, self);
}

/* ============ Init request (step 1) ============ */

static void
on_init_response (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  PollCallbackData *data = user_data;
  GnalkAsyncOp *op = data->op;
  SoupMessage *msg = data->message;

  if (!gnalk_async_op_is_valid (op))
    {
      poll_callback_data_free (data);
      return;
    }

  GnalkLoginFlowV2 *self = GNALK_LOGIN_FLOW_V2 (gnalk_async_op_get_owner (op));
  poll_callback_data_free (data);

  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source),
                                                     result, &error);

  if (error)
    {
      g_autofree char *err = g_strdup_printf (_("Connection failed: %s"),
                                               error->message);
      g_error_free (error);
      emit_failed (self, err);
      g_object_unref (self);
      return;
    }

  guint status = soup_message_get_status (msg);
  if (status != 200)
    {
      g_bytes_unref (bytes);
      g_autofree char *err = g_strdup_printf (
          _("Server does not support Login Flow v2 (HTTP %u)"), status);
      emit_failed (self, err);
      g_object_unref (self);
      return;
    }

  /* Parse JSON: { poll: { token, endpoint }, login: URL } */
  g_autoptr(JsonParser) parser = json_parser_new ();
  gsize size;
  const char *body = g_bytes_get_data (bytes, &size);

  if (!json_parser_load_from_data (parser, body, (gssize) size, &error))
    {
      g_bytes_unref (bytes);
      g_autofree char *err = g_strdup_printf (_("Invalid response: %s"),
                                               error->message);
      g_error_free (error);
      emit_failed (self, err);
      g_object_unref (self);
      return;
    }

  g_bytes_unref (bytes);

  JsonNode *root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT (root))
    {
      emit_failed (self, _("Invalid response from server"));
      g_object_unref (self);
      return;
    }

  JsonObject *obj = json_node_get_object (root);

  /* Extract login URL */
  const char *login_url = json_object_get_string_member_with_default (obj, "login", NULL);
  if (!login_url)
    {
      emit_failed (self, _("Missing login URL in response"));
      g_object_unref (self);
      return;
    }

  /* Extract poll object */
  if (!json_object_has_member (obj, "poll"))
    {
      emit_failed (self, _("Missing poll data in response"));
      g_object_unref (self);
      return;
    }

  JsonObject *poll_obj = json_object_get_object_member (obj, "poll");
  if (!poll_obj)
    {
      emit_failed (self, _("Invalid poll data in response"));
      g_object_unref (self);
      return;
    }

  const char *token = json_object_get_string_member_with_default (poll_obj, "token", NULL);
  const char *endpoint = json_object_get_string_member_with_default (poll_obj, "endpoint", NULL);

  if (!token || !endpoint)
    {
      emit_failed (self, _("Incomplete poll data in response"));
      g_object_unref (self);
      return;
    }

  /* Store poll data */
  self->poll_endpoint = g_strdup (endpoint);
  self->poll_token = g_strdup (token);
  self->login_url = g_strdup (login_url);

  /* Open browser */
  GError *launch_error = NULL;
  if (!g_app_info_launch_default_for_uri (login_url, NULL, &launch_error))
    {
      g_autofree char *err = g_strdup_printf (_("Failed to open browser: %s"),
                                               launch_error->message);
      g_error_free (launch_error);
      emit_failed (self, err);
      g_object_unref (self);
      return;
    }

  /* Start polling */
  self->state = FLOW_STATE_POLLING;

  /* Start global timeout */
  self->timeout_timer_id = g_timeout_add_seconds (GLOBAL_TIMEOUT_S,
                                                   on_global_timeout, self);

  /* Send first poll immediately (server may already have creds if user was fast) */
  send_poll_request (self);

  g_object_unref (self);
}

/* ============ Public API ============ */

static void
gnalk_login_flow_v2_finalize (GObject *object)
{
  GnalkLoginFlowV2 *self = GNALK_LOGIN_FLOW_V2 (object);

  reset_state (self);
  g_clear_object (&self->session);

  G_OBJECT_CLASS (gnalk_login_flow_v2_parent_class)->finalize (object);
}

static void
gnalk_login_flow_v2_class_init (GnalkLoginFlowV2Class *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_login_flow_v2_finalize;

  signals[SIGNAL_COMPLETED] =
    g_signal_new ("completed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_STRING,   /* server_url */
                  G_TYPE_STRING,   /* login_name */
                  G_TYPE_STRING);  /* app_password */

  signals[SIGNAL_FAILED] =
    g_signal_new ("failed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);  /* error_message */
}

static void
gnalk_login_flow_v2_init (GnalkLoginFlowV2 *self)
{
  self->session = soup_session_new ();
  self->state = FLOW_STATE_IDLE;
  self->generation = 0;
}

GnalkLoginFlowV2 *
gnalk_login_flow_v2_new (void)
{
  return g_object_new (GNALK_TYPE_LOGIN_FLOW_V2, NULL);
}

void
gnalk_login_flow_v2_start (GnalkLoginFlowV2 *self,
                            const char       *server_url)
{
  g_return_if_fail (GNALK_IS_LOGIN_FLOW_V2 (self));
  g_return_if_fail (server_url != NULL);

  /* Cancel any previous flow */
  if (self->state != FLOW_STATE_IDLE)
    reset_state (self);

  self->server_url = normalize_server_url (server_url);
  if (!self->server_url)
    {
      emit_failed (self, _("Invalid server URL"));
      return;
    }

  self->state = FLOW_STATE_INIT;
  self->cancellable = g_cancellable_new ();

  /* POST to $server/index.php/login/v2 */
  g_autofree char *init_url = g_strdup_printf ("%s/index.php/login/v2",
                                                self->server_url);

  SoupMessage *msg = soup_message_new ("POST", init_url);
  if (!msg)
    {
      emit_failed (self, _("Invalid server URL"));
      return;
    }

  /* Per spec: Clear-Site-Data header, empty form body */
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);
  soup_message_headers_append (headers, "Clear-Site-Data", "\"cookies\"");

  GBytes *empty_body = g_bytes_new ("", 0);
  soup_message_set_request_body_from_bytes (msg, "application/x-www-form-urlencoded",
                                            empty_body);
  g_bytes_unref (empty_body);

  PollCallbackData *data = g_new0 (PollCallbackData, 1);
  data->op = gnalk_async_op_new (G_OBJECT (self), &self->generation, NULL, 0);
  data->message = g_object_ref (msg);

  soup_session_send_and_read_async (self->session, msg,
                                    G_PRIORITY_DEFAULT,
                                    self->cancellable,
                                    on_init_response, data);
  g_object_unref (msg);
}

void
gnalk_login_flow_v2_cancel (GnalkLoginFlowV2 *self)
{
  g_return_if_fail (GNALK_IS_LOGIN_FLOW_V2 (self));

  reset_state (self);
}

gboolean
gnalk_login_flow_v2_is_active (GnalkLoginFlowV2 *self)
{
  g_return_val_if_fail (GNALK_IS_LOGIN_FLOW_V2 (self), FALSE);

  return self->state != FLOW_STATE_IDLE;
}
