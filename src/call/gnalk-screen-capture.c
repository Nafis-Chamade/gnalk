#include "gnalk-screen-capture.h"
#include <gio/gunixfdlist.h>
#include <unistd.h>

#define PORTAL_BUS_NAME      "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH   "/org/freedesktop/portal/desktop"
#define PORTAL_SCREENCAST_INTERFACE "org.freedesktop.portal.ScreenCast"
#define PORTAL_REQUEST_INTERFACE    "org.freedesktop.portal.Request"

struct _GnalkScreenCapture
{
  GObject parent_instance;

  GDBusConnection *connection;
  GCancellable    *cancellable;

  char *session_handle;
  char *request_handle;

  guint32 pipewire_node_id;
  int     pipewire_fd;

  gboolean is_active;

  guint response_signal_id;
  char *sender_name;
};

G_DEFINE_FINAL_TYPE (GnalkScreenCapture, gnalk_screen_capture, G_TYPE_OBJECT)

enum
{
  SIGNAL_CAPTURE_STARTED,
  SIGNAL_CAPTURE_STOPPED,
  SIGNAL_CAPTURE_FAILED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void create_session (GnalkScreenCapture *self);
static void select_sources (GnalkScreenCapture *self);
static void start_capture (GnalkScreenCapture *self);
static void open_pipewire_remote (GnalkScreenCapture *self);

static char *
get_request_token (void)
{
  static guint counter = 0;
  return g_strdup_printf ("gnalk%u", counter++);
}

static char *
get_session_token (void)
{
  static guint counter = 0;
  return g_strdup_printf ("gnalk_session%u", counter++);
}

static void
cleanup_response_signal (GnalkScreenCapture *self)
{
  if (self->response_signal_id > 0)
    {
      g_dbus_connection_signal_unsubscribe (self->connection, self->response_signal_id);
      self->response_signal_id = 0;
    }

  g_clear_pointer (&self->request_handle, g_free);
}

static void
close_session (GnalkScreenCapture *self)
{
  if (self->session_handle && self->connection)
    {
      g_dbus_connection_call (self->connection,
                              PORTAL_BUS_NAME,
                              self->session_handle,
                              "org.freedesktop.portal.Session",
                              "Close",
                              NULL,
                              NULL,
                              G_DBUS_CALL_FLAGS_NONE,
                              -1,
                              NULL,
                              NULL,
                              NULL);
    }

  g_clear_pointer (&self->session_handle, g_free);
}

static void
emit_failed (GnalkScreenCapture *self,
             const char          *message)
{
  g_warning ("Screen capture failed: %s", message);

  cleanup_response_signal (self);
  close_session (self);

  self->is_active = FALSE;

  g_signal_emit (self, signals[SIGNAL_CAPTURE_FAILED], 0, message);
}

/* Response handlers */

static void
on_create_session_response (GDBusConnection *connection G_GNUC_UNUSED,
                            const char      *sender_name G_GNUC_UNUSED,
                            const char      *object_path G_GNUC_UNUSED,
                            const char      *interface_name G_GNUC_UNUSED,
                            const char      *signal_name G_GNUC_UNUSED,
                            GVariant        *parameters,
                            gpointer         user_data)
{
  GnalkScreenCapture *self = GNALK_SCREEN_CAPTURE (user_data);
  guint32 response;
  g_autoptr(GVariant) results = NULL;

  cleanup_response_signal (self);

  g_variant_get (parameters, "(u@a{sv})", &response, &results);

  if (response != 0)
    {
      emit_failed (self, "CreateSession was cancelled or failed");
      return;
    }

  const char *session_handle = NULL;
  if (!g_variant_lookup (results, "session_handle", "&s", &session_handle))
    {
      emit_failed (self, "No session_handle in CreateSession response");
      return;
    }

  self->session_handle = g_strdup (session_handle);
  g_debug ("Screen capture session created: %s", self->session_handle);

  select_sources (self);
}

static void
on_select_sources_response (GDBusConnection *connection G_GNUC_UNUSED,
                            const char      *sender_name G_GNUC_UNUSED,
                            const char      *object_path G_GNUC_UNUSED,
                            const char      *interface_name G_GNUC_UNUSED,
                            const char      *signal_name G_GNUC_UNUSED,
                            GVariant        *parameters,
                            gpointer         user_data)
{
  GnalkScreenCapture *self = GNALK_SCREEN_CAPTURE (user_data);
  guint32 response;
  g_autoptr(GVariant) results = NULL;

  cleanup_response_signal (self);

  g_variant_get (parameters, "(u@a{sv})", &response, &results);

  if (response != 0)
    {
      emit_failed (self, "SelectSources was cancelled or failed");
      return;
    }

  g_debug ("Screen capture sources selected");

  start_capture (self);
}

static void
on_start_response (GDBusConnection *connection G_GNUC_UNUSED,
                   const char      *sender_name G_GNUC_UNUSED,
                   const char      *object_path G_GNUC_UNUSED,
                   const char      *interface_name G_GNUC_UNUSED,
                   const char      *signal_name G_GNUC_UNUSED,
                   GVariant        *parameters,
                   gpointer         user_data)
{
  GnalkScreenCapture *self = GNALK_SCREEN_CAPTURE (user_data);
  guint32 response;
  g_autoptr(GVariant) results = NULL;

  cleanup_response_signal (self);

  g_variant_get (parameters, "(u@a{sv})", &response, &results);

  if (response != 0)
    {
      emit_failed (self, "User cancelled screen sharing");
      return;
    }

  /* Extract streams array to get node_id */
  g_autoptr(GVariant) streams = g_variant_lookup_value (results, "streams", G_VARIANT_TYPE ("a(ua{sv})"));
  if (!streams)
    {
      emit_failed (self, "No streams in Start response");
      return;
    }

  gsize n_streams = g_variant_n_children (streams);
  if (n_streams == 0)
    {
      emit_failed (self, "Empty streams array in Start response");
      return;
    }

  /* Get first stream */
  g_autoptr(GVariant) stream = g_variant_get_child_value (streams, 0);
  g_variant_get_child (stream, 0, "u", &self->pipewire_node_id);

  g_debug ("Screen capture started, PipeWire node ID: %u", self->pipewire_node_id);

  open_pipewire_remote (self);
}

/* D-Bus call helpers */

static void
subscribe_to_response (GnalkScreenCapture *self,
                       const char          *request_path,
                       GDBusSignalCallback  callback)
{
  cleanup_response_signal (self);

  self->request_handle = g_strdup (request_path);
  self->response_signal_id = g_dbus_connection_signal_subscribe (
    self->connection,
    PORTAL_BUS_NAME,
    PORTAL_REQUEST_INTERFACE,
    "Response",
    request_path,
    NULL,
    G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
    callback,
    self,
    NULL);
}

static char *
build_request_path (GnalkScreenCapture *self,
                    const char          *token)
{
  /* Request path format: /org/freedesktop/portal/desktop/request/SENDER/TOKEN */
  g_autofree char *escaped_sender = g_strdup (self->sender_name + 1);  /* Skip leading ':' */

  /* Replace dots with underscores in sender name */
  for (char *p = escaped_sender; *p; p++)
    {
      if (*p == '.')
        *p = '_';
    }

  return g_strdup_printf ("/org/freedesktop/portal/desktop/request/%s/%s",
                          escaped_sender, token);
}

static void
on_create_session_call_done (GObject      *source,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  GnalkScreenCapture *self = GNALK_SCREEN_CAPTURE (user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) ret = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  if (error)
    {
      emit_failed (self, error->message);
      return;
    }

  /* Response will come via the signal we subscribed to */
}

static void
create_session (GnalkScreenCapture *self)
{
  g_autofree char *token = get_session_token ();
  g_autofree char *request_token = get_request_token ();
  g_autofree char *request_path = build_request_path (self, request_token);

  subscribe_to_response (self, request_path, on_create_session_response);

  GVariantBuilder options;
  g_variant_builder_init (&options, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&options, "{sv}", "handle_token", g_variant_new_string (request_token));
  g_variant_builder_add (&options, "{sv}", "session_handle_token", g_variant_new_string (token));

  g_dbus_connection_call (self->connection,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          PORTAL_SCREENCAST_INTERFACE,
                          "CreateSession",
                          g_variant_new ("(a{sv})", &options),
                          G_VARIANT_TYPE ("(o)"),
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          self->cancellable,
                          on_create_session_call_done,
                          self);
}

static void
on_select_sources_call_done (GObject      *source,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  GnalkScreenCapture *self = GNALK_SCREEN_CAPTURE (user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) ret = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  if (error)
    {
      emit_failed (self, error->message);
      return;
    }
}

static void
select_sources (GnalkScreenCapture *self)
{
  g_autofree char *request_token = get_request_token ();
  g_autofree char *request_path = build_request_path (self, request_token);

  subscribe_to_response (self, request_path, on_select_sources_response);

  /* Allow both monitors and windows */
  guint32 source_types = GNALK_SCREEN_CAPTURE_SOURCE_MONITOR |
                         GNALK_SCREEN_CAPTURE_SOURCE_WINDOW;

  /* Embed cursor in the stream */
  guint32 cursor_mode = GNALK_SCREEN_CAPTURE_CURSOR_EMBEDDED;

  GVariantBuilder options;
  g_variant_builder_init (&options, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&options, "{sv}", "handle_token", g_variant_new_string (request_token));
  g_variant_builder_add (&options, "{sv}", "types", g_variant_new_uint32 (source_types));
  g_variant_builder_add (&options, "{sv}", "cursor_mode", g_variant_new_uint32 (cursor_mode));
  g_variant_builder_add (&options, "{sv}", "multiple", g_variant_new_boolean (FALSE));

  g_dbus_connection_call (self->connection,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          PORTAL_SCREENCAST_INTERFACE,
                          "SelectSources",
                          g_variant_new ("(oa{sv})", self->session_handle, &options),
                          G_VARIANT_TYPE ("(o)"),
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          self->cancellable,
                          on_select_sources_call_done,
                          self);
}

static void
on_start_call_done (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  GnalkScreenCapture *self = GNALK_SCREEN_CAPTURE (user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) ret = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  if (error)
    {
      emit_failed (self, error->message);
      return;
    }
}

static void
start_capture (GnalkScreenCapture *self)
{
  g_autofree char *request_token = get_request_token ();
  g_autofree char *request_path = build_request_path (self, request_token);

  subscribe_to_response (self, request_path, on_start_response);

  GVariantBuilder options;
  g_variant_builder_init (&options, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&options, "{sv}", "handle_token", g_variant_new_string (request_token));

  /* Empty parent window identifier (string) - we don't have X11/Wayland handle */
  g_dbus_connection_call (self->connection,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          PORTAL_SCREENCAST_INTERFACE,
                          "Start",
                          g_variant_new ("(osa{sv})", self->session_handle, "", &options),
                          G_VARIANT_TYPE ("(o)"),
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          self->cancellable,
                          on_start_call_done,
                          self);
}

static void
on_open_pipewire_remote_done (GObject      *source,
                              GAsyncResult *result,
                              gpointer      user_data)
{
  GnalkScreenCapture *self = GNALK_SCREEN_CAPTURE (user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) ret = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;

  ret = g_dbus_connection_call_with_unix_fd_list_finish (G_DBUS_CONNECTION (source),
                                                          &fd_list,
                                                          result,
                                                          &error);
  if (error)
    {
      emit_failed (self, error->message);
      return;
    }

  gint32 fd_index;
  g_variant_get (ret, "(h)", &fd_index);

  if (!fd_list || g_unix_fd_list_get_length (fd_list) <= fd_index)
    {
      emit_failed (self, "No file descriptor in OpenPipeWireRemote response");
      return;
    }

  self->pipewire_fd = g_unix_fd_list_get (fd_list, fd_index, &error);
  if (self->pipewire_fd < 0)
    {
      emit_failed (self, error ? error->message : "Failed to get PipeWire FD");
      return;
    }

  self->is_active = TRUE;

  g_debug ("Screen capture ready: node_id=%u fd=%d", self->pipewire_node_id, self->pipewire_fd);

  g_signal_emit (self, signals[SIGNAL_CAPTURE_STARTED], 0,
                 self->pipewire_node_id, self->pipewire_fd);
}

static void
open_pipewire_remote (GnalkScreenCapture *self)
{
  GVariantBuilder options;
  g_variant_builder_init (&options, G_VARIANT_TYPE ("a{sv}"));

  g_dbus_connection_call_with_unix_fd_list (self->connection,
                                            PORTAL_BUS_NAME,
                                            PORTAL_OBJECT_PATH,
                                            PORTAL_SCREENCAST_INTERFACE,
                                            "OpenPipeWireRemote",
                                            g_variant_new ("(oa{sv})", self->session_handle, &options),
                                            G_VARIANT_TYPE ("(h)"),
                                            G_DBUS_CALL_FLAGS_NONE,
                                            -1,
                                            NULL,  /* fd_list for input */
                                            self->cancellable,
                                            on_open_pipewire_remote_done,
                                            self);
}

/* Public API */

void
gnalk_screen_capture_start (GnalkScreenCapture *self)
{
  g_return_if_fail (GNALK_IS_SCREEN_CAPTURE (self));

  if (self->is_active)
    {
      g_warning ("Screen capture already active");
      return;
    }

  if (!self->connection)
    {
      g_autoptr(GError) error = NULL;
      self->connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
      if (!self->connection)
        {
          emit_failed (self, error ? error->message : "Failed to connect to session bus");
          return;
        }

      self->sender_name = g_strdup (g_dbus_connection_get_unique_name (self->connection));
    }

  g_debug ("Starting screen capture...");

  create_session (self);
}

void
gnalk_screen_capture_stop (GnalkScreenCapture *self)
{
  g_return_if_fail (GNALK_IS_SCREEN_CAPTURE (self));

  if (!self->is_active && !self->session_handle)
    return;

  g_debug ("Stopping screen capture...");

  cleanup_response_signal (self);

  if (self->pipewire_fd >= 0)
    {
      close (self->pipewire_fd);
      self->pipewire_fd = -1;
    }

  close_session (self);

  self->pipewire_node_id = 0;
  self->is_active = FALSE;

  g_signal_emit (self, signals[SIGNAL_CAPTURE_STOPPED], 0);
}

gboolean
gnalk_screen_capture_is_active (GnalkScreenCapture *self)
{
  g_return_val_if_fail (GNALK_IS_SCREEN_CAPTURE (self), FALSE);
  return self->is_active;
}

guint32
gnalk_screen_capture_get_pipewire_node_id (GnalkScreenCapture *self)
{
  g_return_val_if_fail (GNALK_IS_SCREEN_CAPTURE (self), 0);
  return self->pipewire_node_id;
}

int
gnalk_screen_capture_get_pipewire_fd (GnalkScreenCapture *self)
{
  g_return_val_if_fail (GNALK_IS_SCREEN_CAPTURE (self), -1);
  return self->pipewire_fd;
}

/* GObject implementation */

static void
gnalk_screen_capture_dispose (GObject *object)
{
  GnalkScreenCapture *self = GNALK_SCREEN_CAPTURE (object);

  /* Stop capture if active */
  gnalk_screen_capture_stop (self);

  if (self->cancellable)
    {
      g_cancellable_cancel (self->cancellable);
      g_clear_object (&self->cancellable);
    }

  g_clear_pointer (&self->sender_name, g_free);
  g_clear_object (&self->connection);

  G_OBJECT_CLASS (gnalk_screen_capture_parent_class)->dispose (object);
}

static void
gnalk_screen_capture_class_init (GnalkScreenCaptureClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gnalk_screen_capture_dispose;

  signals[SIGNAL_CAPTURE_STARTED] =
    g_signal_new ("capture-started",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE,
                  2,
                  G_TYPE_UINT,
                  G_TYPE_INT);

  signals[SIGNAL_CAPTURE_STOPPED] =
    g_signal_new ("capture-stopped",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE,
                  0);

  signals[SIGNAL_CAPTURE_FAILED] =
    g_signal_new ("capture-failed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE,
                  1,
                  G_TYPE_STRING);
}

static void
gnalk_screen_capture_init (GnalkScreenCapture *self)
{
  self->cancellable = g_cancellable_new ();
  self->pipewire_fd = -1;
  self->pipewire_node_id = 0;
  self->is_active = FALSE;
}

GnalkScreenCapture *
gnalk_screen_capture_new (void)
{
  return g_object_new (GNALK_TYPE_SCREEN_CAPTURE, NULL);
}
