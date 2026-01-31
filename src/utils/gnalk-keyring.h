#pragma once

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* Store credentials in keyring (async) */
void gnalk_keyring_store_password (const char          *server_url,
                                     const char          *user_id,
                                     const char          *password,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data);
gboolean gnalk_keyring_store_password_finish (GAsyncResult  *result,
                                                GError       **error);

/* Lookup credentials from keyring (async) */
void gnalk_keyring_lookup_password (const char          *server_url,
                                      const char          *user_id,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data);
char *gnalk_keyring_lookup_password_finish (GAsyncResult  *result,
                                              GError       **error);

/* Delete credentials from keyring (async) */
void gnalk_keyring_delete_password (const char          *server_url,
                                      const char          *user_id,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data);
gboolean gnalk_keyring_delete_password_finish (GAsyncResult  *result,
                                                 GError       **error);

/* Synchronous versions */
char *gnalk_keyring_lookup_password_sync (const char  *server_url,
                                            const char  *user_id,
                                            GError     **error);
gboolean gnalk_keyring_store_password_sync (const char  *server_url,
                                              const char  *user_id,
                                              const char  *password,
                                              GError     **error);
gboolean gnalk_keyring_delete_password_sync (const char  *server_url,
                                               const char  *user_id,
                                               GError     **error);

G_END_DECLS
