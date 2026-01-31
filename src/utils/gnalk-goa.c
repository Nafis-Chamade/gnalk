#include "gnalk-goa.h"

#ifdef HAVE_GOA
#define GOA_API_IS_SUBJECT_TO_CHANGE
#include <goa/goa.h>
#endif

struct _GnalkGoa
{
  GObject parent_instance;

#ifdef HAVE_GOA
  GoaClient *client;
#endif
  gboolean available;
};

G_DEFINE_FINAL_TYPE (GnalkGoa, gnalk_goa, G_TYPE_OBJECT)

enum
{
  SIGNAL_ACCOUNTS_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

void
gnalk_goa_account_info_free (GnalkGoaAccountInfo *info)
{
  if (!info)
    return;
  g_free (info->goa_id);
  g_free (info->presentation_identity);
  g_free (info->provider_name);
  g_free (info);
}

#ifdef HAVE_GOA

static void
on_account_added (GoaClient  *client G_GNUC_UNUSED,
                  GoaObject  *object,
                  GnalkGoa  *self)
{
  GoaAccount *account = goa_object_peek_account (object);
  const char *provider_type = goa_account_get_provider_type (account);

  /* Only emit for Nextcloud/ownCloud accounts */
  if (g_strcmp0 (provider_type, "owncloud") == 0)
    {
      g_message ("GOA: Nextcloud account added: %s",
                 goa_account_get_presentation_identity (account));
      g_signal_emit (self, signals[SIGNAL_ACCOUNTS_CHANGED], 0);
    }
}

static void
on_account_removed (GoaClient  *client G_GNUC_UNUSED,
                    GoaObject  *object,
                    GnalkGoa  *self)
{
  GoaAccount *account = goa_object_peek_account (object);
  const char *provider_type = goa_account_get_provider_type (account);

  if (g_strcmp0 (provider_type, "owncloud") == 0)
    {
      g_message ("GOA: Nextcloud account removed: %s",
                 goa_account_get_presentation_identity (account));
      g_signal_emit (self, signals[SIGNAL_ACCOUNTS_CHANGED], 0);
    }
}

static void
on_account_changed (GoaClient  *client G_GNUC_UNUSED,
                    GoaObject  *object,
                    GnalkGoa  *self)
{
  GoaAccount *account = goa_object_peek_account (object);
  const char *provider_type = goa_account_get_provider_type (account);

  if (g_strcmp0 (provider_type, "owncloud") == 0)
    {
      g_message ("GOA: Nextcloud account changed: %s",
                 goa_account_get_presentation_identity (account));
      g_signal_emit (self, signals[SIGNAL_ACCOUNTS_CHANGED], 0);
    }
}

#endif /* HAVE_GOA */

static void
gnalk_goa_finalize (GObject *object)
{
  GnalkGoa *self = GNALK_GOA (object);

#ifdef HAVE_GOA
  g_clear_object (&self->client);
#endif

  G_OBJECT_CLASS (gnalk_goa_parent_class)->finalize (object);
}

static void
gnalk_goa_class_init (GnalkGoaClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_goa_finalize;

  signals[SIGNAL_ACCOUNTS_CHANGED] =
    g_signal_new ("accounts-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);
}

static void
gnalk_goa_init (GnalkGoa *self)
{
  self->available = FALSE;

#ifdef HAVE_GOA
  GError *error = NULL;
  self->client = goa_client_new_sync (NULL, &error);

  if (error)
    {
      g_warning ("Failed to create GOA client: %s", error->message);
      g_error_free (error);
      return;
    }

  self->available = TRUE;

  g_signal_connect (self->client, "account-added",
                    G_CALLBACK (on_account_added), self);
  g_signal_connect (self->client, "account-removed",
                    G_CALLBACK (on_account_removed), self);
  g_signal_connect (self->client, "account-changed",
                    G_CALLBACK (on_account_changed), self);

  g_message ("GOA: Client initialized successfully");
#else
  g_message ("GOA: Not compiled in");
#endif
}

GnalkGoa *
gnalk_goa_new (void)
{
  return g_object_new (GNALK_TYPE_GOA, NULL);
}

gboolean
gnalk_goa_is_available (GnalkGoa *self)
{
  g_return_val_if_fail (GNALK_IS_GOA (self), FALSE);
  return self->available;
}

GList *
gnalk_goa_get_nextcloud_accounts (GnalkGoa *self)
{
  g_return_val_if_fail (GNALK_IS_GOA (self), NULL);

#ifdef HAVE_GOA
  if (!self->client)
    return NULL;

  GList *result = NULL;
  GList *accounts = goa_client_get_accounts (self->client);

  for (GList *l = accounts; l != NULL; l = l->next)
    {
      GoaObject *object = GOA_OBJECT (l->data);
      GoaAccount *account = goa_object_peek_account (object);

      if (!account)
        continue;

      const char *provider_type = goa_account_get_provider_type (account);

      /* Filter for Nextcloud/ownCloud accounts */
      if (g_strcmp0 (provider_type, "owncloud") != 0)
        continue;

      /* Skip disabled accounts */
      if (goa_account_get_files_disabled (account))
        continue;

      GnalkGoaAccountInfo *info = g_new0 (GnalkGoaAccountInfo, 1);
      info->goa_id = g_strdup (goa_account_get_id (account));
      info->presentation_identity = g_strdup (goa_account_get_presentation_identity (account));
      info->provider_name = g_strdup (goa_account_get_provider_name (account));

      result = g_list_prepend (result, info);
    }

  g_list_free_full (accounts, g_object_unref);
  return g_list_reverse (result);
#else
  return NULL;
#endif
}

gboolean
gnalk_goa_get_credentials (GnalkGoa   *self,
                             const char  *goa_id,
                             char       **server_url,
                             char       **user_id,
                             char       **password,
                             GError     **error)
{
  g_return_val_if_fail (GNALK_IS_GOA (self), FALSE);
  g_return_val_if_fail (goa_id != NULL, FALSE);
  g_return_val_if_fail (server_url != NULL, FALSE);
  g_return_val_if_fail (user_id != NULL, FALSE);
  g_return_val_if_fail (password != NULL, FALSE);

  *server_url = NULL;
  *user_id = NULL;
  *password = NULL;

#ifdef HAVE_GOA
  if (!self->client)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                           "GOA client not available");
      return FALSE;
    }

  /* Find the account by ID */
  GoaObject *object = goa_client_lookup_by_id (self->client, goa_id);
  if (!object)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "GOA account not found: %s", goa_id);
      return FALSE;
    }

  GoaAccount *account = goa_object_peek_account (object);
  if (!account)
    {
      g_object_unref (object);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Failed to get account interface");
      return FALSE;
    }

  /* Check if account needs attention */
  if (goa_account_get_attention_needed (account))
    {
      g_object_unref (object);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "Account needs attention in GNOME Settings");
      return FALSE;
    }

  /* Get password via GoaPasswordBased interface */
  GoaPasswordBased *password_based = goa_object_peek_password_based (object);
  if (!password_based)
    {
      g_object_unref (object);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Account does not support password-based authentication");
      return FALSE;
    }

  /* Call EnsureCredentials first to ensure credentials are fresh */
  gboolean expires = FALSE;
  if (!goa_account_call_ensure_credentials_sync (account, NULL, NULL, error))
    {
      g_prefix_error (error, "Failed to ensure credentials: ");
      g_object_unref (object);
      return FALSE;
    }

  /* Get the password */
  char *retrieved_password = NULL;
  if (!goa_password_based_call_get_password_sync (password_based,
                                                   "password",
                                                   &retrieved_password,
                                                   NULL,
                                                   error))
    {
      g_prefix_error (error, "Failed to get password: ");
      g_object_unref (object);
      return FALSE;
    }

  /* Extract server URL from presentation identity or use Files URI
   * The presentation identity format is typically "user@server.example.com"
   * For ownCloud/Nextcloud, we need the actual server URL */
  const char *identity = goa_account_get_presentation_identity (account);
  const char *at_sign = identity ? strchr (identity, '@') : NULL;

  if (at_sign && at_sign[1])
    {
      /* Construct server URL from host part */
      *server_url = g_strdup_printf ("https://%s", at_sign + 1);
      *user_id = g_strndup (identity, at_sign - identity);
    }
  else
    {
      /* Fallback: try to get from Files interface */
      GoaFiles *files = goa_object_peek_files (object);
      if (files)
        {
          const char *uri = goa_files_get_uri (files);
          if (uri)
            {
              /* Parse the WebDAV URI to extract server base URL
               * Format: https://server/remote.php/dav/files/username/ */
              GUri *guri = g_uri_parse (uri, G_URI_FLAGS_NONE, NULL);
              if (guri)
                {
                  *server_url = g_strdup_printf ("https://%s", g_uri_get_host (guri));
                  /* Extract username from path */
                  const char *path = g_uri_get_path (guri);
                  if (path && g_str_has_prefix (path, "/remote.php/dav/files/"))
                    {
                      const char *username_start = path + strlen ("/remote.php/dav/files/");
                      const char *username_end = strchr (username_start, '/');
                      if (username_end)
                        *user_id = g_strndup (username_start, username_end - username_start);
                      else
                        *user_id = g_strdup (username_start);
                    }
                  g_uri_unref (guri);
                }
            }
        }

      if (!*server_url || !*user_id)
        {
          g_free (*server_url);
          g_free (*user_id);
          g_free (retrieved_password);
          g_object_unref (object);
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Could not extract server URL and user ID from GOA account");
          return FALSE;
        }
    }

  *password = retrieved_password;
  g_object_unref (object);

  g_message ("GOA: Retrieved credentials for %s@%s", *user_id, *server_url);
  return TRUE;
#else
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "GOA support not compiled in");
  return FALSE;
#endif
}

gboolean
gnalk_goa_account_exists (GnalkGoa  *self,
                            const char *goa_id)
{
  g_return_val_if_fail (GNALK_IS_GOA (self), FALSE);
  g_return_val_if_fail (goa_id != NULL, FALSE);

#ifdef HAVE_GOA
  if (!self->client)
    return FALSE;

  GoaObject *object = goa_client_lookup_by_id (self->client, goa_id);
  if (object)
    {
      g_object_unref (object);
      return TRUE;
    }
  return FALSE;
#else
  return FALSE;
#endif
}
