#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include "gnalk-account.h"
#include "gnalk-database.h"

G_BEGIN_DECLS

#define GNALK_TYPE_ACCOUNT_MANAGER (gnalk_account_manager_get_type ())
G_DECLARE_FINAL_TYPE (GnalkAccountManager, gnalk_account_manager, GNALK, ACCOUNT_MANAGER, GObject)

/**
 * GnalkAccountManager:
 *
 * Registry of configured accounts. Handles database persistence.
 *
 * Signals:
 * - "account-added" (GnalkAccount *account)
 *     Emitted when a new account is registered
 * - "account-removed" (gint64 account_id)
 *     Emitted when an account is deleted
 * - "account-updated" (GnalkAccount *account)
 *     Emitted when account properties change
 */

GnalkAccountManager *gnalk_account_manager_new (GnalkDatabase *database);

/**
 * gnalk_account_manager_load_accounts:
 * @self: The account manager
 *
 * Loads all accounts from the database. Call this once at startup
 * after the database is opened.
 */
void gnalk_account_manager_load_accounts (GnalkAccountManager *self);

/**
 * gnalk_account_manager_get_accounts:
 * @self: The account manager
 *
 * Returns a list of all registered accounts.
 *
 * Returns: (transfer none): A GListStore containing GnalkAccount objects
 */
GListStore *gnalk_account_manager_get_accounts (GnalkAccountManager *self);

/**
 * gnalk_account_manager_get_account_count:
 * @self: The account manager
 *
 * Returns: The number of registered accounts
 */
guint gnalk_account_manager_get_account_count (GnalkAccountManager *self);

/**
 * gnalk_account_manager_get_account_by_id:
 * @self: The account manager
 * @account_id: The database ID
 *
 * Looks up an account by its database ID.
 *
 * Returns: (transfer none) (nullable): The account or NULL
 */
GnalkAccount *gnalk_account_manager_get_account_by_id (GnalkAccountManager *self,
                                                           gint64                account_id);

/**
 * gnalk_account_manager_find_account:
 * @self: The account manager
 * @server_url: The server URL
 * @user_id: The user ID
 *
 * Finds an account matching the given server and user.
 *
 * Returns: (transfer none) (nullable): The account or NULL
 */
GnalkAccount *gnalk_account_manager_find_account (GnalkAccountManager *self,
                                                      const char           *server_url,
                                                      const char           *user_id);

/**
 * gnalk_account_manager_add_account:
 * @self: The account manager
 * @server_url: The Nextcloud server URL
 * @user_id: The user login ID
 *
 * Creates and registers a new account. If an account with the same
 * server and user already exists, returns the existing account.
 *
 * Returns: (transfer none): The new or existing account
 */
GnalkAccount *gnalk_account_manager_add_account (GnalkAccountManager *self,
                                                     const char           *server_url,
                                                     const char           *user_id);

/**
 * gnalk_account_manager_remove_account:
 * @self: The account manager
 * @account_id: The database ID of the account to remove
 *
 * Removes an account from the database. Also deletes associated
 * conversations, messages, and pending queue.
 *
 * Returns: TRUE if account was removed
 */
gboolean gnalk_account_manager_remove_account (GnalkAccountManager *self,
                                                  gint64                account_id);

/**
 * gnalk_account_manager_save_account:
 * @self: The account manager
 * @account: The account to save
 *
 * Persists account changes to the database.
 */
void gnalk_account_manager_save_account (GnalkAccountManager *self,
                                           GnalkAccount        *account);

/**
 * gnalk_account_manager_add_guest_account:
 * @self: The account manager
 * @server_url: The Nextcloud server URL
 * @room_token: The room token
 * @display_name: Guest display name
 *
 * Creates or reuses a guest account for the given room.
 * Uses synthetic user_id "guest:TOKEN" to satisfy UNIQUE(server_url, user_id).
 *
 * Returns: (transfer none): The new or existing guest account
 */
GnalkAccount *gnalk_account_manager_add_guest_account (GnalkAccountManager *self,
                                                        const char           *server_url,
                                                        const char           *room_token,
                                                        const char           *display_name);

/**
 * gnalk_account_manager_get_autologin_accounts:
 * @self: The account manager
 *
 * Returns a list of accounts with autologin enabled.
 *
 * Returns: (transfer container) (element-type GnalkAccount): List of accounts
 */
GList *gnalk_account_manager_get_autologin_accounts (GnalkAccountManager *self);

/**
 * gnalk_account_manager_get_enabled_accounts:
 * @self: The account manager
 *
 * Returns a list of enabled accounts.
 *
 * Returns: (transfer container) (element-type GnalkAccount): List of accounts
 */
GList *gnalk_account_manager_get_enabled_accounts (GnalkAccountManager *self);

G_END_DECLS
