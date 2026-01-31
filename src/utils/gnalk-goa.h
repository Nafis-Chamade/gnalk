#pragma once

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GNALK_TYPE_GOA (gnalk_goa_get_type ())
G_DECLARE_FINAL_TYPE (GnalkGoa, gnalk_goa, GNALK, GOA, GObject)

/**
 * GnalkGoaAccountInfo:
 * @goa_id: The GOA account ID (unique identifier)
 * @presentation_identity: Display identity (e.g., "user@nextcloud.example.com")
 * @provider_name: Provider display name (e.g., "Nextcloud")
 *
 * Information about a Nextcloud account from GNOME Online Accounts.
 */
typedef struct {
  char *goa_id;
  char *presentation_identity;
  char *provider_name;
} GnalkGoaAccountInfo;

void gnalk_goa_account_info_free (GnalkGoaAccountInfo *info);

/**
 * gnalk_goa_new:
 *
 * Creates a new GOA client wrapper. The wrapper connects to the GOA daemon
 * and monitors for account changes.
 *
 * Returns: (transfer full) (nullable): A new #GnalkGoa instance, or %NULL
 *   if GOA is not available.
 */
GnalkGoa *gnalk_goa_new (void);

/**
 * gnalk_goa_is_available:
 *
 * Checks if GNOME Online Accounts integration is available.
 * This returns %FALSE if GOA support was not compiled in or if
 * the GOA daemon is not running.
 *
 * Returns: %TRUE if GOA is available
 */
gboolean gnalk_goa_is_available (GnalkGoa *self);

/**
 * gnalk_goa_get_nextcloud_accounts:
 * @self: The GOA wrapper
 *
 * Gets a list of Nextcloud/ownCloud accounts configured in GNOME Online Accounts.
 *
 * Returns: (element-type GnalkGoaAccountInfo) (transfer full): A list of
 *   account info structures. Free with g_list_free_full() using
 *   gnalk_goa_account_info_free().
 */
GList *gnalk_goa_get_nextcloud_accounts (GnalkGoa *self);

/**
 * gnalk_goa_get_credentials:
 * @self: The GOA wrapper
 * @goa_id: The GOA account ID
 * @server_url: (out) (transfer full): The server URL
 * @user_id: (out) (transfer full): The user ID
 * @password: (out) (transfer full): The password/app token
 * @error: Return location for error
 *
 * Retrieves credentials for a GOA account.
 *
 * Returns: %TRUE on success
 */
gboolean gnalk_goa_get_credentials (GnalkGoa   *self,
                                      const char  *goa_id,
                                      char       **server_url,
                                      char       **user_id,
                                      char       **password,
                                      GError     **error);

/**
 * gnalk_goa_account_exists:
 * @self: The GOA wrapper
 * @goa_id: The GOA account ID
 *
 * Checks if a GOA account still exists.
 *
 * Returns: %TRUE if the account exists
 */
gboolean gnalk_goa_account_exists (GnalkGoa  *self,
                                     const char *goa_id);

/* Signals:
 * - "accounts-changed": Emitted when GOA accounts are added, removed, or modified
 */

G_END_DECLS
