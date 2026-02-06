#include "gnalk-application.h"
#include "gnalk-window.h"
#include "gnalk-config.h"
#include "gnalk-account-manager.h"
#include "gnalk-session-manager.h"
#include "gnalk-push-client.h"
#include "gnalk-notification-service.h"
#include "gnalk-http-client.h"
#include "gnalk-sync-coordinator.h"
#include "gnalk-keyring.h"
#include "gnalk-data-repository.h"
#include "gnalk-app-state.h"
#include <glib/gi18n.h>

struct _GnalkApplication
{
  AdwApplication parent_instance;

  GSettings *settings;
  char      *signaling_server;
  char      *signaling_ticket;
  char      *stun_server;
  char      *turn_server;
  char      *turn_username;
  char      *turn_credential;
  char      *push_websocket_url;

  /* Guest mode */
  gboolean   is_guest;
  char      *guest_room_token;

  /* Capabilities */
  gint64     chat_edit_timeout;    /* Max age in seconds for editing messages (0 = disabled) */
  gint64     chat_delete_timeout;  /* Max age in seconds for deleting messages (6h default) */

  /* Persistence - async wrapper for database */
  GnalkDataRepository *data_repository;
  GnalkDatabase       *database;  /* Direct access for backward compat during migration */
  GnalkNetworkMonitor *network_monitor;
  gint64                account_id;

  /* Centralized UI state */
  GnalkAppState       *app_state;

  /* Multi-account support */
  GnalkAccountManager *account_manager;
  GnalkSessionManager *session_manager;

  /* Service mode support */
  gboolean                   is_service_mode;
  GnalkPushClient          *push_client;
  GnalkNotificationService *notification_service;
  GnalkHttpClient          *http_client;  /* App-level HTTP client for service mode */
  char                      *pending_open_room;  /* Room to open when window is created */

  /* Async startup tracking */
  gboolean startup_complete;
};

G_DEFINE_FINAL_TYPE (GnalkApplication, gnalk_application, ADW_TYPE_APPLICATION)

/* Forward declarations */
static void gnalk_application_ensure_window (GnalkApplication *self);
static void apply_color_scheme (GnalkApplication *self);
static gboolean start_async_loading_idle (gpointer user_data);
static void on_database_opened (GnalkDataRepository *repository,
                                 gpointer              result,
                                 GError               *error,
                                 gpointer              user_data);
static void on_session_conversations_changed (GnalkSessionManager *manager,
                                                GnalkSession        *session,
                                                GnalkApplication    *self);
static void on_session_online_changed (GnalkSessionManager *manager,
                                         GnalkSession        *session,
                                         int                   old_state,
                                         int                   new_state,
                                         GnalkApplication    *self);

static void
apply_color_scheme (GnalkApplication *self)
{
  AdwStyleManager *style_manager = adw_style_manager_get_default ();
  gint scheme = g_settings_get_int (self->settings, "color-scheme");

  switch (scheme)
    {
    case 1:  /* Light */
      adw_style_manager_set_color_scheme (style_manager, ADW_COLOR_SCHEME_FORCE_LIGHT);
      break;
    case 2:  /* Dark */
      adw_style_manager_set_color_scheme (style_manager, ADW_COLOR_SCHEME_FORCE_DARK);
      break;
    default:  /* Auto (0) */
      adw_style_manager_set_color_scheme (style_manager, ADW_COLOR_SCHEME_DEFAULT);
      break;
    }
}

static void
on_color_scheme_changed (GSettings   *settings G_GNUC_UNUSED,
                         const gchar *key G_GNUC_UNUSED,
                         gpointer     user_data)
{
  apply_color_scheme (GNALK_APPLICATION (user_data));
}

static void
on_open_room_action (GSimpleAction *action,
                     GVariant      *parameter,
                     gpointer       user_data)
{
  GnalkApplication *self = GNALK_APPLICATION (user_data);
  const char *room_token = g_variant_get_string (parameter, NULL);

  g_message ("Action: open-room '%s'", room_token);

  /* Store the room to open */
  g_free (self->pending_open_room);
  self->pending_open_room = g_strdup (room_token);

  /* Ensure window exists and is presented */
  gnalk_application_ensure_window (self);
}

static void
on_mark_read_response (GnalkHttpClient *client G_GNUC_UNUSED,
                        JsonNode         *response G_GNUC_UNUSED,
                        GError           *error,
                        gpointer          user_data G_GNUC_UNUSED)
{
  if (error)
    g_debug ("Mark-read action failed: %s", error->message);
  else
    g_debug ("Mark-read action succeeded");
}

static void
on_mark_read_action (GSimpleAction *action G_GNUC_UNUSED,
                     GVariant      *parameter,
                     gpointer       user_data)
{
  GnalkApplication *self = GNALK_APPLICATION (user_data);
  const char *room_token = g_variant_get_string (parameter, NULL);

  g_message ("Action: mark-read '%s'", room_token);

  /* Withdraw the notification */
  if (self->notification_service)
    gnalk_notification_service_withdraw (self->notification_service, room_token);

  /* Mark conversation as read via HTTP API */
  if (self->http_client && self->database && self->account_id > 0)
    {
      gint64 last_msg_id = gnalk_database_get_last_known_message_id (
          self->database, self->account_id, room_token);
      if (last_msg_id > 0)
        {
          gnalk_http_client_mark_chat_read (self->http_client,
                                              room_token,
                                              last_msg_id,
                                              on_mark_read_response,
                                              NULL);
        }
    }
}

static void
on_reply_send_response (GnalkHttpClient *client G_GNUC_UNUSED,
                        JsonNode         *response G_GNUC_UNUSED,
                        GError           *error,
                        gpointer          user_data G_GNUC_UNUSED)
{
  if (error)
    g_warning ("Reply action failed: %s", error->message);
  else
    g_debug ("Reply action succeeded");
}

static void
on_reply_action (GSimpleAction *action G_GNUC_UNUSED,
                 GVariant      *parameter,
                 gpointer       user_data)
{
  GnalkApplication *self = GNALK_APPLICATION (user_data);
  const char *room_token = NULL;
  const char *text = NULL;

  g_variant_get (parameter, "(&s&s)", &room_token, &text);
  g_message ("Action: reply to '%s' with '%s'", room_token, text);

  /* Validate inputs */
  if (!room_token || !*room_token || !text || !*text)
    {
      g_warning ("Reply action: missing room_token or text");
      return;
    }

  /* Send message via app-level HTTP client (no window needed) */
  if (self->http_client && gnalk_http_client_is_ready (self->http_client))
    {
      /* Generate a unique reference ID for tracking */
      g_autofree char *reference_id = g_uuid_string_random ();

      gnalk_http_client_send_message (self->http_client,
                                       room_token,
                                       text,
                                       0,  /* no reply_to */
                                       reference_id,
                                       on_reply_send_response,
                                       NULL);

      /* Withdraw notification for this room since user replied */
      if (self->notification_service)
        gnalk_notification_service_withdraw (self->notification_service, room_token);
    }
  else
    {
      g_warning ("Reply action: HTTP client not ready");
    }
}

static const GActionEntry app_actions[] = {
  { "open-room", on_open_room_action, "s", NULL, NULL },
  { "mark-read", on_mark_read_action, "s", NULL, NULL },
  { "reply", on_reply_action, "(ss)", NULL, NULL },
};

static void
gnalk_application_ensure_window (GnalkApplication *self)
{
  GtkWindow *window = NULL;

  /* Get any existing window - get_active_window may return NULL if minimized */
  GList *windows = gtk_application_get_windows (GTK_APPLICATION (self));
  if (windows)
    window = GTK_WINDOW (windows->data);

  if (window == NULL)
    window = g_object_new (GNALK_TYPE_WINDOW,
                           "application", self,
                           NULL);

  /* Present the window - this unminimizes and brings to front */
  gtk_window_present (window);

  /* If there's a pending room to open, navigate to it directly */
  if (self->pending_open_room && GNALK_IS_WINDOW (window))
    {
      g_autofree char *room_token = g_steal_pointer (&self->pending_open_room);
      g_message ("Opening room: %s", room_token);
      gnalk_window_open_room (GNALK_WINDOW (window), room_token);
    }
}

static void
gnalk_application_activate (GApplication *app)
{
  GnalkApplication *self = GNALK_APPLICATION (app);

  /* Create and present window. Database loading already started in startup(). */
  gnalk_application_ensure_window (self);
}

static int
gnalk_application_command_line (GApplication            *app,
                                 GApplicationCommandLine *cmdline)
{
  GnalkApplication *self = GNALK_APPLICATION (app);

  gchar **argv;
  gint argc;
  argv = g_application_command_line_get_arguments (cmdline, &argc);

  /* Check for service mode */
  for (int i = 1; i < argc; i++)
    {
      if (g_strcmp0 (argv[i], "--gapplication-service") == 0)
        {
          /* Service mode is already handled in startup() - this code path
           * is only reached if command_line() is called, which shouldn't
           * happen with --gapplication-service, but handle it just in case */
          self->is_service_mode = TRUE;
          g_application_hold (app);
          g_strfreev (argv);
          return 0;  /* Stay running */
        }
    }

  g_strfreev (argv);

  /* Normal mode: activate to show window */
  g_application_activate (app);
  return 0;
}

static gboolean
check_service_mode_from_args (void)
{
  /* Check if --gapplication-service was passed.
   * GApplication handles this flag before command_line is called,
   * so we need to detect it ourselves in startup. */

  /* Alternative: check environment */
  const gchar *env_mode = g_getenv ("GNALK_SERVICE_MODE");
  if (env_mode && g_strcmp0 (env_mode, "1") == 0)
    return TRUE;

  /* Check program arguments directly via /proc */
  GFile *proc_cmdline = g_file_new_for_path ("/proc/self/cmdline");
  gchar *contents = NULL;
  gsize length = 0;

  if (g_file_load_contents (proc_cmdline, NULL, &contents, &length, NULL, NULL))
    {
      /* cmdline is NUL-separated arguments */
      gsize pos = 0;
      while (pos < length)
        {
          const char *arg = contents + pos;
          if (g_strcmp0 (arg, "--gapplication-service") == 0)
            {
              g_free (contents);
              g_object_unref (proc_cmdline);
              return TRUE;
            }
          pos += strlen (arg) + 1;
        }
      g_free (contents);
    }
  g_object_unref (proc_cmdline);

  return FALSE;
}

/* Push notification callback - triggers sync for ALL online accounts */
static void
on_app_push_notification (GnalkPushClient *client G_GNUC_UNUSED,
                          const char      *notification_type,
                          gpointer         user_data)
{
  GnalkApplication *self = GNALK_APPLICATION (user_data);

  g_message ("App: Push notification received: %s", notification_type);

  /* Handle notification types that indicate new messages/activity */
  if (g_strcmp0 (notification_type, "notify_notification") == 0 ||
      g_strcmp0 (notification_type, "notify_activity") == 0)
    {
      /* Trigger sync for ALL online sessions.
       * Each session owns its own sync coordinator. */
      if (!self->session_manager)
        return;

      GList *sessions = gnalk_session_manager_get_all_sessions (self->session_manager);
      for (GList *l = sessions; l != NULL; l = l->next)
        {
          GnalkSession *session = GNALK_SESSION (l->data);

          /* Only sync online, non-guest sessions */
          if (gnalk_session_get_state (session) != GNALK_SESSION_STATE_ONLINE)
            continue;
          if (gnalk_session_is_guest (session))
            continue;

          GnalkSyncCoordinator *coordinator = gnalk_session_get_sync_coordinator (session);
          if (coordinator)
            {
              const char *user_id = gnalk_account_get_user_id (gnalk_session_get_account (session));
              g_message ("App: Triggering sync for account %s (push)", user_id);
              gnalk_sync_coordinator_trigger (coordinator, GNALK_SYNC_TRIGGER_PUSH);
            }
        }
      g_list_free (sessions);
    }
}

static void
gnalk_application_startup (GApplication *app)
{
  GnalkApplication *self = GNALK_APPLICATION (app);
  GtkCssProvider *css_provider;

  G_APPLICATION_CLASS (gnalk_application_parent_class)->startup (app);

  /* Detect service mode early - GApplication handles --gapplication-service
   * before our command_line handler is called */
  if (check_service_mode_from_args ())
    {
      self->is_service_mode = TRUE;
      g_message ("Starting in service mode (no window)");
      g_application_hold (app);
    }

  /* Register application actions */
  g_action_map_add_action_entries (G_ACTION_MAP (app),
                                   app_actions,
                                   G_N_ELEMENTS (app_actions),
                                   self);

  css_provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (css_provider, "/net/skilpa/gnalk/style.css");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                               GTK_STYLE_PROVIDER (css_provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  self->settings = g_settings_new ("net.skilpa.gnalk");

  /* Apply color scheme from settings */
  apply_color_scheme (self);
  g_signal_connect (self->settings, "changed::color-scheme",
                    G_CALLBACK (on_color_scheme_changed), self);

  /* Initialize network monitor - this is quick, synchronous is fine */
  self->network_monitor = gnalk_network_monitor_new ();

  /* Initialize centralized app state */
  self->app_state = gnalk_app_state_new ();
  gnalk_app_state_set_loading (self->app_state, TRUE);
  gnalk_app_state_set_sync_status (self->app_state,
                                     GNALK_SYNC_STATUS_LOADING_CACHE,
                                     "Starting...");

  /* Create async data repository - database open is deferred */
  self->data_repository = gnalk_data_repository_new ();

  /* Initialize push client (needed in both service and normal mode) */
  self->push_client = gnalk_push_client_new ();

  /* Initialize notification service */
  self->notification_service = gnalk_notification_service_new ();

  /* Initialize HTTP client for service mode background sync */
  self->http_client = gnalk_http_client_new ();

  /* Start async database open immediately. This overlaps I/O with window
   * initialization, so managers are ready sooner after window appears.
   * Window shows main view by default (from UI template) while loading. */
  g_idle_add (start_async_loading_idle, self);
}

/* Async startup chain */

static gboolean
start_async_loading_idle (gpointer user_data)
{
  GnalkApplication *self = GNALK_APPLICATION (user_data);

  g_message ("Starting async database open...");
  gnalk_app_state_set_sync_status (self->app_state,
                                     GNALK_SYNC_STATUS_LOADING_CACHE,
                                     "Opening database...");

  gnalk_data_repository_open_async (self->data_repository,
                                       on_database_opened,
                                       self);

  return G_SOURCE_REMOVE;
}

static void
on_database_opened (GnalkDataRepository *repository G_GNUC_UNUSED,
                     gpointer              result,
                     GError               *error,
                     gpointer              user_data)
{
  GnalkApplication *self = GNALK_APPLICATION (user_data);

  if (error)
    {
      g_warning ("Failed to open database: %s", error->message);
      gnalk_app_state_set_sync_status (self->app_state,
                                         GNALK_SYNC_STATUS_ERROR,
                                         "Database error");
      gnalk_app_state_set_loading (self->app_state, FALSE);
      return;
    }

  gboolean success = GPOINTER_TO_INT (result);
  if (!success)
    {
      g_warning ("Database open returned FALSE");
      gnalk_app_state_set_sync_status (self->app_state,
                                         GNALK_SYNC_STATUS_ERROR,
                                         "Database error");
      gnalk_app_state_set_loading (self->app_state, FALSE);
      return;
    }

  g_message ("Database opened, initializing managers...");

  /* Get the underlying database for components that need it directly */
  self->database = gnalk_data_repository_get_database (self->data_repository);

  /* Initialize account manager with opened database */
  self->account_manager = gnalk_account_manager_new (self->database);

  /* Load accounts synchronously - this is fast (just reading from SQLite) */
  gnalk_account_manager_load_accounts (self->account_manager);

  /* Initialize session manager */
  self->session_manager = gnalk_session_manager_new (self->account_manager,
                                                     self->database,
                                                     self->network_monitor,
                                                     self->data_repository,
                                                     self->notification_service);

  /* Connect session manager signals to update app state */
  g_signal_connect (self->session_manager, "conversations-changed",
                    G_CALLBACK (on_session_conversations_changed), self);
  g_signal_connect (self->session_manager, "any-session-state-changed",
                    G_CALLBACK (on_session_online_changed), self);

  /* Set up push notification callback at application level */
  gnalk_push_client_set_notification_callback (self->push_client,
                                                  on_app_push_notification,
                                                  self);

  /* Mark startup complete */
  self->startup_complete = TRUE;

  gnalk_app_state_set_sync_status (self->app_state,
                                     GNALK_SYNC_STATUS_SYNCING,
                                     "Connecting...");

  /* Connect autologin accounts */
  g_message ("Connecting autologin accounts...");
  gnalk_session_manager_connect_all (self->session_manager);

  /* Update loading state - sessions will update when they connect */
  gnalk_app_state_set_loading (self->app_state, FALSE);
  gnalk_app_state_set_sync_status (self->app_state,
                                     GNALK_SYNC_STATUS_IDLE,
                                     NULL);
}

/* Session manager signal handlers for updating app state */

static void
on_session_conversations_changed (GnalkSessionManager *manager G_GNUC_UNUSED,
                                    GnalkSession        *session,
                                    GnalkApplication    *self)
{
  /* Get conversations from session and update app state */
  GListStore *conversations = gnalk_session_get_conversations (session);
  gnalk_app_state_set_conversations (self->app_state, conversations, session);
}

static void
on_session_online_changed (GnalkSessionManager *manager,
                           GnalkSession        *session G_GNUC_UNUSED,
                           int                  old_state G_GNUC_UNUSED,
                           int                  new_state G_GNUC_UNUSED,
                           GnalkApplication    *self)
{
  /* Update online status based on any session being online.
   * Sync is now handled by each session's own SyncCoordinator. */
  gboolean any_online = gnalk_session_manager_has_any_online (manager);
  gnalk_app_state_set_online (self->app_state, any_online);
}

static void
gnalk_application_shutdown (GApplication *app)
{
  G_APPLICATION_CLASS (gnalk_application_parent_class)->shutdown (app);
}

static void
gnalk_application_finalize (GObject *object)
{
  GnalkApplication *self = GNALK_APPLICATION (object);

  g_clear_object (&self->notification_service);
  g_clear_object (&self->http_client);
  g_clear_object (&self->push_client);
  g_clear_object (&self->session_manager);
  g_clear_object (&self->account_manager);
  /* Note: database is owned by data_repository, don't double-free */
  self->database = NULL;
  g_clear_object (&self->data_repository);
  g_clear_object (&self->app_state);
  g_clear_object (&self->network_monitor);
  g_clear_object (&self->settings);
  g_free (self->signaling_server);
  g_free (self->signaling_ticket);
  g_free (self->stun_server);
  g_free (self->turn_server);
  g_free (self->turn_username);
  g_free (self->turn_credential);
  g_free (self->push_websocket_url);
  g_free (self->guest_room_token);
  g_free (self->pending_open_room);

  G_OBJECT_CLASS (gnalk_application_parent_class)->finalize (object);
}

static void
gnalk_application_class_init (GnalkApplicationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

  object_class->finalize = gnalk_application_finalize;

  app_class->activate = gnalk_application_activate;
  app_class->startup = gnalk_application_startup;
  app_class->shutdown = gnalk_application_shutdown;
  app_class->command_line = gnalk_application_command_line;
}

static void
gnalk_application_init (GnalkApplication *self G_GNUC_UNUSED)
{
}

GnalkApplication *
gnalk_application_new (void)
{
  return g_object_new (GNALK_TYPE_APPLICATION,
                       "application-id", APPLICATION_ID,
                       "flags", G_APPLICATION_HANDLES_COMMAND_LINE,
                       NULL);
}

/* Legacy single-account credential functions removed.
 * Use gnalk_account_get_* via gnalk_session_get_account() instead. */

void
gnalk_application_set_signaling_settings (GnalkApplication *self,
                                            const char        *server,
                                            const char        *ticket)
{
  g_return_if_fail (GNALK_IS_APPLICATION (self));

  g_free (self->signaling_server);
  g_free (self->signaling_ticket);

  self->signaling_server = g_strdup (server);
  self->signaling_ticket = g_strdup (ticket);
}


void
gnalk_application_set_ice_servers (GnalkApplication *self,
                                     const char        *stun_server,
                                     const char        *turn_server,
                                     const char        *turn_username,
                                     const char        *turn_credential)
{
  g_return_if_fail (GNALK_IS_APPLICATION (self));

  g_free (self->stun_server);
  g_free (self->turn_server);
  g_free (self->turn_username);
  g_free (self->turn_credential);

  self->stun_server = g_strdup (stun_server);
  self->turn_server = g_strdup (turn_server);
  self->turn_username = g_strdup (turn_username);
  self->turn_credential = g_strdup (turn_credential);
}

const char *
gnalk_application_get_stun_server (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), NULL);
  return self->stun_server;
}

const char *
gnalk_application_get_turn_server (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), NULL);
  return self->turn_server;
}

const char *
gnalk_application_get_turn_username (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), NULL);
  return self->turn_username;
}

const char *
gnalk_application_get_turn_credential (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), NULL);
  return self->turn_credential;
}

void
gnalk_application_set_push_websocket_url (GnalkApplication *self,
                                            const char        *url)
{
  g_return_if_fail (GNALK_IS_APPLICATION (self));

  g_free (self->push_websocket_url);
  self->push_websocket_url = g_strdup (url);
}

const char *
gnalk_application_get_push_websocket_url (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), NULL);
  return self->push_websocket_url;
}

void
gnalk_application_set_guest_mode (GnalkApplication *self,
                                    gboolean           is_guest,
                                    const char        *room_token)
{
  g_return_if_fail (GNALK_IS_APPLICATION (self));

  self->is_guest = is_guest;
  g_free (self->guest_room_token);
  self->guest_room_token = g_strdup (room_token);
}

gboolean
gnalk_application_is_guest (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), FALSE);
  return self->is_guest;
}

const char *
gnalk_application_get_guest_room_token (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), NULL);
  return self->guest_room_token;
}

void
gnalk_application_set_chat_edit_timeout (GnalkApplication *self,
                                           gint64             timeout)
{
  g_return_if_fail (GNALK_IS_APPLICATION (self));
  self->chat_edit_timeout = timeout;
}

gint64
gnalk_application_get_chat_edit_timeout (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), 0);
  return self->chat_edit_timeout;
}

void
gnalk_application_set_chat_delete_timeout (GnalkApplication *self,
                                             gint64             timeout)
{
  g_return_if_fail (GNALK_IS_APPLICATION (self));
  self->chat_delete_timeout = timeout;
}

gint64
gnalk_application_get_chat_delete_timeout (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), 0);
  return self->chat_delete_timeout;
}

/* Multi-account support */
GnalkAccountManager *
gnalk_application_get_account_manager (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), NULL);
  return self->account_manager;
}

GnalkSessionManager *
gnalk_application_get_session_manager (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), NULL);
  return self->session_manager;
}

GnalkDatabase *
gnalk_application_get_database (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), NULL);
  return self->database;
}

GnalkDataRepository *
gnalk_application_get_data_repository (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), NULL);
  return self->data_repository;
}

GnalkAppState *
gnalk_application_get_app_state (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), NULL);
  return self->app_state;
}

GnalkNetworkMonitor *
gnalk_application_get_network_monitor (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), NULL);
  return self->network_monitor;
}

GSettings *
gnalk_application_get_settings (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), NULL);
  return self->settings;
}

GnalkPushClient *
gnalk_application_get_push_client (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), NULL);
  return self->push_client;
}

GnalkHttpClient *
gnalk_application_get_http_client (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), NULL);
  return self->http_client;
}

void
gnalk_application_set_active_room (GnalkApplication *self,
                                   const char       *room_token)
{
  g_return_if_fail (GNALK_IS_APPLICATION (self));

  /* Update active room on all session sync coordinators */
  if (self->session_manager)
    {
      GList *sessions = gnalk_session_manager_get_all_sessions (self->session_manager);
      for (GList *l = sessions; l != NULL; l = l->next)
        {
          GnalkSession *session = GNALK_SESSION (l->data);
          GnalkSyncCoordinator *coordinator = gnalk_session_get_sync_coordinator (session);
          if (coordinator)
            gnalk_sync_coordinator_set_active_room (coordinator, room_token);
        }
      g_list_free (sessions);
    }
}

void
gnalk_application_set_window_focused (GnalkApplication *self,
                                      gboolean          focused)
{
  g_return_if_fail (GNALK_IS_APPLICATION (self));

  /* Update window focus on all session sync coordinators */
  if (self->session_manager)
    {
      GList *sessions = gnalk_session_manager_get_all_sessions (self->session_manager);
      for (GList *l = sessions; l != NULL; l = l->next)
        {
          GnalkSession *session = GNALK_SESSION (l->data);
          GnalkSyncCoordinator *coordinator = gnalk_session_get_sync_coordinator (session);
          if (coordinator)
            gnalk_sync_coordinator_set_window_focused (coordinator, focused);
        }
      g_list_free (sessions);
    }
}

char *
gnalk_application_consume_pending_room (GnalkApplication *self)
{
  g_return_val_if_fail (GNALK_IS_APPLICATION (self), NULL);

  char *room = self->pending_open_room;
  self->pending_open_room = NULL;
  return room;  /* Caller owns the string */
}
