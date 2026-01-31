#include "gnalk-keyring.h"

#ifdef HAVE_LIBSECRET
#include <libsecret/secret.h>

/* Schema for Gnalk credentials */
static const SecretSchema GNALK_SCHEMA = {
  "net.skilpa.gnalk.credentials",
  SECRET_SCHEMA_NONE,
  {
    { "server_url", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "user_id", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};

/* Store password */

void
gnalk_keyring_store_password (const char          *server_url,
                                const char          *user_id,
                                const char          *password,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  g_return_if_fail (server_url != NULL);
  g_return_if_fail (user_id != NULL);
  g_return_if_fail (password != NULL);

  char *label = g_strdup_printf ("Gnalk password for %s on %s", user_id, server_url);

  secret_password_store (&GNALK_SCHEMA,
                         SECRET_COLLECTION_DEFAULT,
                         label,
                         password,
                         NULL,  /* GCancellable */
                         callback,
                         user_data,
                         "server_url", server_url,
                         "user_id", user_id,
                         NULL);

  g_free (label);
}

gboolean
gnalk_keyring_store_password_finish (GAsyncResult  *result,
                                       GError       **error)
{
  return secret_password_store_finish (result, error);
}

gboolean
gnalk_keyring_store_password_sync (const char  *server_url,
                                     const char  *user_id,
                                     const char  *password,
                                     GError     **error)
{
  g_return_val_if_fail (server_url != NULL, FALSE);
  g_return_val_if_fail (user_id != NULL, FALSE);
  g_return_val_if_fail (password != NULL, FALSE);

  char *label = g_strdup_printf ("Gnalk password for %s on %s", user_id, server_url);

  gboolean success = secret_password_store_sync (&GNALK_SCHEMA,
                                                  SECRET_COLLECTION_DEFAULT,
                                                  label,
                                                  password,
                                                  NULL,  /* GCancellable */
                                                  error,
                                                  "server_url", server_url,
                                                  "user_id", user_id,
                                                  NULL);

  g_free (label);
  return success;
}

/* Lookup password */

void
gnalk_keyring_lookup_password (const char          *server_url,
                                 const char          *user_id,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  g_return_if_fail (server_url != NULL);
  g_return_if_fail (user_id != NULL);

  secret_password_lookup (&GNALK_SCHEMA,
                          NULL,  /* GCancellable */
                          callback,
                          user_data,
                          "server_url", server_url,
                          "user_id", user_id,
                          NULL);
}

char *
gnalk_keyring_lookup_password_finish (GAsyncResult  *result,
                                        GError       **error)
{
  return secret_password_lookup_finish (result, error);
}

char *
gnalk_keyring_lookup_password_sync (const char  *server_url,
                                      const char  *user_id,
                                      GError     **error)
{
  g_return_val_if_fail (server_url != NULL, NULL);
  g_return_val_if_fail (user_id != NULL, NULL);

  return secret_password_lookup_sync (&GNALK_SCHEMA,
                                       NULL,  /* GCancellable */
                                       error,
                                       "server_url", server_url,
                                       "user_id", user_id,
                                       NULL);
}

/* Delete password */

void
gnalk_keyring_delete_password (const char          *server_url,
                                 const char          *user_id,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  g_return_if_fail (server_url != NULL);
  g_return_if_fail (user_id != NULL);

  secret_password_clear (&GNALK_SCHEMA,
                         NULL,  /* GCancellable */
                         callback,
                         user_data,
                         "server_url", server_url,
                         "user_id", user_id,
                         NULL);
}

gboolean
gnalk_keyring_delete_password_finish (GAsyncResult  *result,
                                        GError       **error)
{
  return secret_password_clear_finish (result, error);
}

gboolean
gnalk_keyring_delete_password_sync (const char  *server_url,
                                      const char  *user_id,
                                      GError     **error)
{
  g_return_val_if_fail (server_url != NULL, FALSE);
  g_return_val_if_fail (user_id != NULL, FALSE);

  return secret_password_clear_sync (&GNALK_SCHEMA,
                                      NULL,  /* GCancellable */
                                      error,
                                      "server_url", server_url,
                                      "user_id", user_id,
                                      NULL);
}

#else /* !HAVE_LIBSECRET */

/* Stub implementations when libsecret is not available */

void
gnalk_keyring_store_password (const char          *server_url G_GNUC_UNUSED,
                                const char          *user_id G_GNUC_UNUSED,
                                const char          *password G_GNUC_UNUSED,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  /* Call callback with NULL result to indicate not supported */
  if (callback)
    {
      GTask *task = g_task_new (NULL, NULL, callback, user_data);
      g_task_return_boolean (task, FALSE);
      g_object_unref (task);
    }
}

gboolean
gnalk_keyring_store_password_finish (GAsyncResult  *result,
                                       GError       **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "Keyring not available (libsecret not compiled in)");
  return g_task_propagate_boolean (G_TASK (result), NULL);
}

gboolean
gnalk_keyring_store_password_sync (const char  *server_url G_GNUC_UNUSED,
                                     const char  *user_id G_GNUC_UNUSED,
                                     const char  *password G_GNUC_UNUSED,
                                     GError     **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "Keyring not available (libsecret not compiled in)");
  return FALSE;
}

void
gnalk_keyring_lookup_password (const char          *server_url G_GNUC_UNUSED,
                                 const char          *user_id G_GNUC_UNUSED,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  if (callback)
    {
      GTask *task = g_task_new (NULL, NULL, callback, user_data);
      g_task_return_pointer (task, NULL, NULL);
      g_object_unref (task);
    }
}

char *
gnalk_keyring_lookup_password_finish (GAsyncResult  *result,
                                        GError       **error G_GNUC_UNUSED)
{
  return g_task_propagate_pointer (G_TASK (result), NULL);
}

char *
gnalk_keyring_lookup_password_sync (const char  *server_url G_GNUC_UNUSED,
                                      const char  *user_id G_GNUC_UNUSED,
                                      GError     **error G_GNUC_UNUSED)
{
  return NULL;
}

void
gnalk_keyring_delete_password (const char          *server_url G_GNUC_UNUSED,
                                 const char          *user_id G_GNUC_UNUSED,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  if (callback)
    {
      GTask *task = g_task_new (NULL, NULL, callback, user_data);
      g_task_return_boolean (task, FALSE);
      g_object_unref (task);
    }
}

gboolean
gnalk_keyring_delete_password_finish (GAsyncResult  *result,
                                        GError       **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "Keyring not available (libsecret not compiled in)");
  return g_task_propagate_boolean (G_TASK (result), NULL);
}

gboolean
gnalk_keyring_delete_password_sync (const char  *server_url G_GNUC_UNUSED,
                                      const char  *user_id G_GNUC_UNUSED,
                                      GError     **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "Keyring not available (libsecret not compiled in)");
  return FALSE;
}

#endif /* HAVE_LIBSECRET */
