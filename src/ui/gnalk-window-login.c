/* gnalk-window-login.c
 *
 * Login-related functionality extracted from gnalk-window.c.
 * Handles authentication, autologin, and login view management.
 */

#include "gnalk-window-private.h"
#include "gnalk-application.h"
#include "gnalk-session.h"
#include "gnalk-session-manager.h"
#include "gnalk-account-manager.h"
#include "gnalk-keyring.h"
#include "gnalk-database.h"
#include "gnalk-data-repository.h"
#include "gnalk-login-view.h"
#include "gnalk-login-flow-v2.h"
#include "gnalk-account-switcher.h"
#include "gnalk-avatar-cache.h"
#include "gnalk-user-status-menu.h"
#include "gnalk-network-status-bar.h"
#include "gnalk-goa.h"
#include <glib/gi18n.h>

/* GOA integration state (module-level for now) */
static GnalkGoa *g_goa_client = NULL;

/* ============ Login View Helper Wrappers ============ */

void
gnalk_window_login_show_error (GnalkWindow *self, const char *message)
{
  gnalk_login_view_show_error (self->login_view, message);
}

void
gnalk_window_login_set_connecting (GnalkWindow *self, gboolean connecting)
{
  gnalk_login_view_set_connecting (self->login_view, connecting);
}

/* ============ Common Login Finalization ============ */

static void
finalize_authenticated_login (GnalkWindow *self,
                              const char  *server_url,
                              const char  *user_id,
                              const char  *password,
                              const char  *goa_id)
{
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  GnalkAccountManager *account_manager = gnalk_application_get_account_manager (app);

  GnalkAccount *account = gnalk_account_manager_add_account (account_manager, server_url, user_id);

  if (goa_id)
    gnalk_account_set_goa_account_id (account, goa_id);

  GError *keyring_error = NULL;
  if (!gnalk_keyring_store_password_sync (server_url, user_id, password, &keyring_error))
    {
      g_warning ("Failed to store password: %s",
                 keyring_error ? keyring_error->message : "unknown error");
      g_clear_error (&keyring_error);
    }

  gnalk_account_set_autologin (account, TRUE);
  gnalk_account_set_enabled (account, TRUE);
  gnalk_account_manager_save_account (account_manager, account);

  GSettings *settings = gnalk_application_get_settings (app);
  g_settings_set_boolean (settings, "autologin-enabled", TRUE);
  g_settings_set_string (settings, "last-server-url", server_url);
  g_settings_set_string (settings, "last-user-id", user_id);

  GnalkSession *session = gnalk_session_manager_start_session (self->session_manager, account, password);
  if (session)
    gnalk_session_manager_set_active_session (self->session_manager, session);
}

/* ============ GOA Helper Functions ============ */

#ifdef HAVE_GOA

static GList *
get_active_goa_ids (GnalkAccountManager *account_manager)
{
  GList *result = NULL;
  GListStore *accounts = gnalk_account_manager_get_accounts (account_manager);

  if (!accounts)
    return NULL;

  guint n = g_list_model_get_n_items (G_LIST_MODEL (accounts));
  for (guint i = 0; i < n; i++)
    {
      GnalkAccount *account = g_list_model_get_item (G_LIST_MODEL (accounts), i);
      const char *goa_id = gnalk_account_get_goa_account_id (account);
      if (goa_id && *goa_id)
        result = g_list_prepend (result, g_strdup (goa_id));
      g_object_unref (account);
    }

  return result;
}

static void
refresh_goa_accounts_list (GnalkWindow *self)
{
  if (!g_goa_client || !gnalk_goa_is_available (g_goa_client))
    {
      gnalk_login_view_clear_goa_accounts (self->login_view);
      return;
    }

  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  GnalkAccountManager *account_manager = gnalk_application_get_account_manager (app);

  GList *goa_accounts = gnalk_goa_get_nextcloud_accounts (g_goa_client);
  GList *active_goa_ids = get_active_goa_ids (account_manager);

  gnalk_login_view_set_goa_accounts (self->login_view, goa_accounts, active_goa_ids);

  g_list_free_full (goa_accounts, (GDestroyNotify) gnalk_goa_account_info_free);
  g_list_free_full (active_goa_ids, g_free);
}

static void
handle_goa_account_removed (GnalkWindow *self,
                             GnalkAccount *account)
{
  const char *goa_id = gnalk_account_get_goa_account_id (account);
  if (!goa_id || !*goa_id)
    return;

  /* Check if the GOA account still exists */
  if (g_goa_client && gnalk_goa_account_exists (g_goa_client, goa_id))
    return;

  gint64 account_id = gnalk_account_get_id (account);
  g_message ("GOA account removed: %s (gnalk account %ld)",
             goa_id, (long) account_id);

  /* Disconnect the session but do NOT delete data.
   * Per GNOME GOA documentation: "Applications must not destroy data
   * if an account object is removed..." */
  if (self->session_manager)
    {
      GnalkSession *session = gnalk_session_manager_get_session (
          self->session_manager, account_id);
      if (session)
        {
          g_message ("Disconnecting session for removed GOA account");
          gnalk_session_manager_stop_session (self->session_manager, account_id);
        }
    }

  /* Clear the GOA account ID but keep the account data */
  gnalk_account_set_goa_account_id (account, NULL);

  /* Disable autologin since credentials are no longer available */
  gnalk_account_set_autologin (account, FALSE);

  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  GnalkAccountManager *account_manager = gnalk_application_get_account_manager (app);
  gnalk_account_manager_save_account (account_manager, account);
}

static void
check_for_removed_goa_accounts (GnalkWindow *self)
{
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  GnalkAccountManager *account_manager = gnalk_application_get_account_manager (app);
  GListStore *accounts = gnalk_account_manager_get_accounts (account_manager);

  if (!accounts)
    return;

  guint n = g_list_model_get_n_items (G_LIST_MODEL (accounts));
  for (guint i = 0; i < n; i++)
    {
      GnalkAccount *account = g_list_model_get_item (G_LIST_MODEL (accounts), i);
      handle_goa_account_removed (self, account);
      g_object_unref (account);
    }
}

static void
on_goa_accounts_changed (GnalkGoa    *goa G_GNUC_UNUSED,
                          GnalkWindow *self)
{
  /* Check if any linked GOA accounts were removed */
  check_for_removed_goa_accounts (self);

  /* Refresh the GOA accounts list in the login view */
  refresh_goa_accounts_list (self);
}

static void
on_login_view_goa_account_selected (GnalkLoginView *view G_GNUC_UNUSED,
                                     const char      *goa_id,
                                     GnalkWindow    *self)
{
  g_message ("GOA account selected: %s", goa_id);

  if (!g_goa_client)
    {
      gnalk_window_login_show_error (self, _("GNOME Online Accounts not available"));
      return;
    }

  /* Retrieve credentials from GOA */
  g_autofree char *server_url = NULL;
  g_autofree char *user_id = NULL;
  g_autofree char *password = NULL;
  GError *error = NULL;

  if (!gnalk_goa_get_credentials (g_goa_client, goa_id, &server_url, &user_id, &password, &error))
    {
      g_autofree char *msg = g_strdup_printf (_("Failed to get credentials: %s"),
                                               error ? error->message : "Unknown error");
      gnalk_window_login_show_error (self, msg);
      g_clear_error (&error);
      return;
    }

  /* Show connecting state */
  gnalk_window_login_set_connecting (self, TRUE);

  finalize_authenticated_login (self, server_url, user_id, password, goa_id);
}

#endif /* HAVE_GOA */

/* ============ Login Flow v2 Handlers ============ */

static void
on_login_flow_v2_completed (GnalkLoginFlowV2 *flow G_GNUC_UNUSED,
                            const char        *server_url,
                            const char        *login_name,
                            const char        *app_password,
                            GnalkWindow       *self)
{
  gnalk_login_view_set_browser_waiting (self->login_view, FALSE);
  gnalk_window_login_set_connecting (self, TRUE);
  finalize_authenticated_login (self, server_url, login_name, app_password, NULL);
  g_clear_object (&self->login_flow_v2);
}

static void
on_login_flow_v2_failed (GnalkLoginFlowV2 *flow G_GNUC_UNUSED,
                         const char        *error_message,
                         GnalkWindow       *self)
{
  gnalk_login_view_set_browser_waiting (self->login_view, FALSE);
  gnalk_window_login_show_error (self, error_message);
  g_clear_object (&self->login_flow_v2);
}

static void
start_browser_login (GnalkWindow *self)
{
  const char *server = gnalk_login_view_get_server (self->login_view);
  if (!server || !*server)
    {
      gnalk_window_login_show_error (self, _("Please enter a server URL"));
      return;
    }

  /* Cancel any previous flow */
  if (self->login_flow_v2)
    {
      gnalk_login_flow_v2_cancel (self->login_flow_v2);
      g_clear_object (&self->login_flow_v2);
    }

  self->login_flow_v2 = gnalk_login_flow_v2_new ();
  g_signal_connect (self->login_flow_v2, "completed",
                    G_CALLBACK (on_login_flow_v2_completed), self);
  g_signal_connect (self->login_flow_v2, "failed",
                    G_CALLBACK (on_login_flow_v2_failed), self);

  gnalk_login_view_set_browser_waiting (self->login_view, TRUE);
  gnalk_login_flow_v2_start (self->login_flow_v2, server);
}

/* ============ Login View Signal Handler ============ */

void
gnalk_window_on_login_view_connect_clicked (GnalkLoginView *view,
                                             GnalkWindow    *self)
{
  (void) view;

  switch (gnalk_login_view_get_login_mode (self->login_view))
    {
    case GNALK_LOGIN_MODE_BROWSER:
      start_browser_login (self);
      break;
    case GNALK_LOGIN_MODE_MANUAL:
      gnalk_window_start_login (self);
      break;
    case GNALK_LOGIN_MODE_GUEST:
      gnalk_window_start_guest_login (self);
      break;
    case GNALK_LOGIN_MODE_GOA:
      /* GOA login is handled by row activation, not connect button */
      break;
    }
}

void
gnalk_window_on_login_view_cancel_clicked (GnalkLoginView *view,
                                            GnalkWindow    *self)
{
  (void) view;

  /* Cancel Login Flow v2 if active */
  if (self->login_flow_v2)
    {
      gnalk_login_flow_v2_cancel (self->login_flow_v2);
      g_clear_object (&self->login_flow_v2);
      gnalk_login_view_set_browser_waiting (self->login_view, FALSE);
      return;
    }

  /* Check if we have any active sessions */
  guint session_count = 0;
  if (self->session_manager)
    session_count = gnalk_session_manager_get_session_count (self->session_manager);

  if (session_count > 0)
    {
      /* Return to main view with existing sessions */
      gnalk_window_show_offline_main_view (self);
    }
  else
    {
      /* No sessions - close the application */
      gtk_window_close (GTK_WINDOW (self));
    }
}

/* ============ Login Functions ============ */

void
gnalk_window_start_login (GnalkWindow *self)
{
  const char *server = gnalk_login_view_get_server (self->login_view);
  const char *user = gnalk_login_view_get_user (self->login_view);
  const char *password = gnalk_login_view_get_password (self->login_view);

  if (!server || !*server || !user || !*user || !password || !*password)
    {
      gnalk_window_login_show_error (self, _("Please fill in all fields"));
      return;
    }

  /* Show connecting state */
  gnalk_window_login_set_connecting (self, TRUE);

  finalize_authenticated_login (self, server, user, password, NULL);
}

/* Parse room URL like https://nc.example.com/call/TOKEN */
static gboolean
parse_room_url (const char  *url,
                char       **out_server,
                char       **out_token)
{
  if (!url || !*url)
    return FALSE;

  /* Parse the URL */
  g_autoptr(GUri) uri = g_uri_parse (url, G_URI_FLAGS_NONE, NULL);
  if (!uri)
    return FALSE;

  const char *scheme = g_uri_get_scheme (uri);
  const char *host = g_uri_get_host (uri);
  int port = g_uri_get_port (uri);
  const char *path = g_uri_get_path (uri);

  if (!scheme || !host || !path)
    return FALSE;

  /* Find /call/ in the path */
  const char *call_pos = strstr (path, "/call/");
  if (!call_pos)
    return FALSE;

  /* Extract token (everything after /call/) */
  const char *token = call_pos + strlen ("/call/");
  if (!token || !*token)
    return FALSE;

  /* Build server URL (without the path) */
  if (port > 0 && port != 443 && port != 80)
    *out_server = g_strdup_printf ("%s://%s:%d", scheme, host, port);
  else
    *out_server = g_strdup_printf ("%s://%s", scheme, host);

  *out_token = g_strdup (token);

  return TRUE;
}

void
gnalk_window_start_guest_login (GnalkWindow *self)
{
  const char *room_url = gnalk_login_view_get_room_url (self->login_view);
  const char *display_name = gnalk_login_view_get_display_name (self->login_view);
  const char *room_password = gnalk_login_view_get_room_password (self->login_view);

  if (!room_url || !*room_url)
    {
      gnalk_window_login_show_error (self, _("Please enter a room URL"));
      return;
    }

  if (!display_name || !*display_name)
    {
      gnalk_window_login_show_error (self, _("Please enter a display name"));
      return;
    }

  /* Parse room URL → server_url, room_token */
  g_autofree char *server_url = NULL;
  g_autofree char *room_token = NULL;

  if (!parse_room_url (room_url, &server_url, &room_token))
    {
      gnalk_window_login_show_error (self, _("Invalid room URL. Expected format: https://server.com/call/TOKEN"));
      return;
    }

  /* Show connecting state */
  gnalk_window_login_set_connecting (self, TRUE);

  /* Set guest mode in application */
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  gnalk_application_set_guest_mode (app, TRUE, room_token);

  /* Start guest session via SessionManager */
  GnalkSession *session = gnalk_session_manager_start_guest_session (
      self->session_manager, server_url, room_token, display_name,
      (room_password && *room_password) ? room_password : NULL);

  if (session)
    gnalk_session_manager_set_active_session (self->session_manager, session);
}

void
gnalk_window_show_login_view (GnalkWindow *self)
{
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));

#ifdef HAVE_GOA
  /* Initialize GOA client if not already done */
  if (!g_goa_client)
    {
      g_goa_client = gnalk_goa_new ();
      if (g_goa_client && gnalk_goa_is_available (g_goa_client))
        {
          g_signal_connect (g_goa_client, "accounts-changed",
                            G_CALLBACK (on_goa_accounts_changed), self);

          /* Connect the GOA account selection signal */
          g_signal_connect (self->login_view, "goa-account-selected",
                            G_CALLBACK (on_login_view_goa_account_selected), self);
        }
    }

  /* Refresh GOA accounts list */
  refresh_goa_accounts_list (self);
#endif /* HAVE_GOA */

  /* Pre-fill defaults from settings (last used values) */
  GSettings *settings = gnalk_application_get_settings (app);
  g_autofree char *server_url = g_settings_get_string (settings, "last-server-url");
  g_autofree char *user_id = g_settings_get_string (settings, "last-user-id");

  if (server_url && *server_url)
    gnalk_login_view_set_server (self->login_view, server_url);
  if (user_id && *user_id)
    gnalk_login_view_set_user (self->login_view, user_id);

  gtk_stack_set_visible_child_name (GTK_STACK (self->main_stack), "login");
}

void
gnalk_window_show_offline_main_view (GnalkWindow *self)
{
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  GSettings *settings = gnalk_application_get_settings (app);

  /* Reset participant status since we're starting offline and can't know real status */
  {
    GnalkDataRepository *repo = gnalk_application_get_data_repository (app);
    GnalkAccountManager *account_manager = gnalk_application_get_account_manager (app);
    GList *autologin_accounts = gnalk_account_manager_get_autologin_accounts (account_manager);
    if (repo && autologin_accounts)
      {
        GnalkAccount *account = GNALK_ACCOUNT (autologin_accounts->data);
        gint64 account_id = gnalk_account_get_id (account);
        if (account_id > 0)
          gnalk_data_repository_reset_participants_status (repo, account_id);
        g_list_free_full (autologin_accounts, g_object_unref);
      }
  }

  /* Load cached conversations from database */
  gnalk_window_load_conversations_from_db (self);

  /* Mark avatar cache as offline to prevent network requests */
  if (self->avatar_cache)
    gnalk_avatar_cache_set_online (self->avatar_cache, FALSE);

  /* Load user display name and avatar from cache */
  g_autofree char *display_name = g_settings_get_string (settings, "last-display-name");
  g_autofree char *user_id = g_settings_get_string (settings, "last-user-id");

  if (display_name && *display_name)
    {
      g_free (self->user_name);
      self->user_name = g_strdup (display_name);
      g_object_notify_by_pspec (G_OBJECT (self), gnalk_window_properties[GNALK_WINDOW_PROP_USER_NAME]);
    }

  /* Load cached user avatar */
  if (user_id && *user_id && self->avatar_cache)
    gnalk_user_status_menu_set_user (self->user_status_menu, user_id, self->avatar_cache);

  /* Ensure main view is shown (UI template defaults to main, so this is usually a no-op) */
  const char *current = gtk_stack_get_visible_child_name (GTK_STACK (self->main_stack));
  if (g_strcmp0 (current, "main") != 0)
    gtk_stack_set_visible_child_name (GTK_STACK (self->main_stack), "main");

  /* Only show offline banner if we had a previous connection error, not on initial startup */
  if (self->had_connection_error)
    {
      gnalk_network_status_bar_set_offline (self->network_status_bar, TRUE, NULL);
      self->was_offline = TRUE;
    }

  /* Room restore is handled by gnalk_room_controller_restore_last_room,
   * triggered from maybe_restore_last_room when the first conversation
   * DB load completes. This ensures a single canonical load path with
   * no race against switch_to_session clearing tabs. */
}

void
gnalk_window_attempt_autologin (GnalkWindow *self)
{
  GnalkApplication *app = GNALK_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
  GnalkAccountManager *account_manager = gnalk_application_get_account_manager (app);
  GnalkSessionManager *session_manager = gnalk_application_get_session_manager (app);
  GSettings *settings = gnalk_application_get_settings (app);

  /* Update our cached session_manager reference (may have been NULL during init
   * if window was created before async startup completed) */
  if (!self->session_manager && session_manager)
    {
      self->session_manager = session_manager;
      /* Connect signals we missed during init */
      g_signal_connect_object (self->session_manager, "active-session-changed",
                               G_CALLBACK (gnalk_window_on_session_manager_active_changed), self, G_CONNECT_DEFAULT);
      g_signal_connect_object (self->session_manager, "any-session-state-changed",
                               G_CALLBACK (gnalk_window_on_session_manager_any_state_changed), self, G_CONNECT_DEFAULT);
      /* conversations-changed → Application/AppState only; Window uses DataRepository::data-changed */

      if (self->account_switcher)
        {
          gnalk_account_switcher_set_session_manager (self->account_switcher, self->session_manager);
          g_signal_connect (self->account_switcher, "add-account-requested",
                            G_CALLBACK (gnalk_window_on_add_account_requested), self);
          g_signal_connect (self->account_switcher, "sign-out-requested",
                            G_CALLBACK (gnalk_window_on_sign_out_requested), self);
        }
    }

  /* Check if we have any accounts with autologin enabled */
  GList *autologin_accounts = gnalk_account_manager_get_autologin_accounts (account_manager);

  if (!autologin_accounts)
    {
      /* Fallback: check legacy GSettings autologin (for migration) */
      if (!g_settings_get_boolean (settings, "autologin-enabled"))
        {
          gnalk_window_show_login_view (self);
          return;
        }

      /* Try to migrate legacy settings to account */
      g_autofree char *server_url = g_settings_get_string (settings, "last-server-url");
      g_autofree char *user_id = g_settings_get_string (settings, "last-user-id");

      if (!server_url || !*server_url || !user_id || !*user_id)
        {
          gnalk_window_show_login_view (self);
          return;
        }

      /* Check if password exists in keyring */
      g_autofree char *password = gnalk_keyring_lookup_password_sync (server_url, user_id, NULL);
      if (!password || !*password)
        {
          gnalk_window_show_login_view (self);
          return;
        }

      /* Migrate: create account with autologin enabled */
      GnalkAccount *account = gnalk_account_manager_add_account (account_manager, server_url, user_id);
      if (account)
        {
          gnalk_account_set_autologin (account, TRUE);
          gnalk_account_set_enabled (account, TRUE);
          gnalk_account_manager_save_account (account_manager, account);
        }
    }
  else
    {
      /* Free the list - SessionManager will refetch accounts */
      g_list_free_full (autologin_accounts, g_object_unref);
    }

  /* Show main view with cached data first */
  gnalk_window_show_offline_main_view (self);

  /* Check if session manager already has an active session (e.g., from service mode).
   * If not, connect_all will trigger session creation. */
  GnalkSession *active = gnalk_session_manager_get_active_session (self->session_manager);
  if (!active)
    {
      /* Connect all autologin accounts via SessionManager.
       * SessionManager handles:
       * - Loading passwords from keyring
       * - Starting sessions for each account
       * - Reconnection on network changes
       * - Background auth with timeouts
       * Session state changes will update the UI via on_session_state_changed() */
      g_message ("Autologin: no active session, connecting all accounts via SessionManager");
      gnalk_session_manager_connect_all (self->session_manager);
    }

  /* Try to restore the last active account from settings.
   * This works both for fresh connect_all and for service mode with existing sessions. */
  gint64 last_account_id = g_settings_get_int64 (settings, "last-account-id");
  if (last_account_id > 0)
    {
      GnalkSession *last_session = gnalk_session_manager_get_session (self->session_manager, last_account_id);
      if (last_session)
        {
          g_message ("Autologin: restoring last active account %ld", (long) last_account_id);
          gnalk_session_manager_set_active_session (self->session_manager, last_session);
          gnalk_window_switch_to_session (self, last_session);
          return;
        }
    }

  /* Fallback: switch to whatever session is currently active */
  active = gnalk_session_manager_get_active_session (self->session_manager);
  if (active)
    {
      g_message ("Autologin: switching to current active session");
      gnalk_window_switch_to_session (self, active);
    }
}
