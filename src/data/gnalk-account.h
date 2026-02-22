#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define GNALK_TYPE_ACCOUNT (gnalk_account_get_type ())
G_DECLARE_FINAL_TYPE (GnalkAccount, gnalk_account, GNALK, ACCOUNT, GObject)

/**
 * GnalkAccount:
 *
 * Data object representing a stored Nextcloud Talk account.
 * Contains credentials and configuration, not connection state.
 *
 * Properties:
 * - id: Database row ID (stable identifier)
 * - server-url: Nextcloud server URL
 * - user-id: User login ID
 * - display-name: User's display name (from server)
 * - avatar-url: URL to user's avatar
 * - enabled: Whether account is active
 * - autologin: Whether to auto-connect on startup
 * - goa-account-id: GNOME Online Accounts ID (nullable, for GOA-linked accounts)
 */

GnalkAccount *gnalk_account_new (gint64      id,
                                    const char *server_url,
                                    const char *user_id);

GnalkAccount *gnalk_account_new_full (gint64      id,
                                          const char *server_url,
                                          const char *user_id,
                                          const char *display_name,
                                          const char *avatar_url,
                                          gboolean    enabled,
                                          gboolean    autologin,
                                          const char *goa_account_id,
                                          gboolean    is_guest,
                                          const char *room_token,
                                          const char *last_room_token);

/* Getters */
gint64      gnalk_account_get_id             (GnalkAccount *self);
const char *gnalk_account_get_server_url     (GnalkAccount *self);
const char *gnalk_account_get_user_id        (GnalkAccount *self);
const char *gnalk_account_get_display_name   (GnalkAccount *self);
const char *gnalk_account_get_avatar_url     (GnalkAccount *self);
gboolean    gnalk_account_get_enabled        (GnalkAccount *self);
gboolean    gnalk_account_get_autologin      (GnalkAccount *self);
const char *gnalk_account_get_goa_account_id (GnalkAccount *self);

/* Setters (will emit notify signals) */
void gnalk_account_set_display_name   (GnalkAccount *self,
                                         const char    *display_name);
void gnalk_account_set_avatar_url     (GnalkAccount *self,
                                         const char    *avatar_url);
void gnalk_account_set_enabled        (GnalkAccount *self,
                                         gboolean       enabled);
void gnalk_account_set_autologin      (GnalkAccount *self,
                                         gboolean       autologin);
void gnalk_account_set_goa_account_id (GnalkAccount *self,
                                         const char    *goa_account_id);

/* Guest account constructor (creates unpersisted account with id=-1) */
GnalkAccount *gnalk_account_new_guest (const char *server_url,
                                         const char *room_token,
                                         const char *display_name);

/* Guest-specific getters */
gboolean    gnalk_account_get_is_guest   (GnalkAccount *self);
const char *gnalk_account_get_room_token (GnalkAccount *self);

/* Per-account last room token */
const char *gnalk_account_get_last_room_token (GnalkAccount *self);
void        gnalk_account_set_last_room_token (GnalkAccount *self,
                                               const char   *token);

/* Set account ID (used when persisting a guest account) */
void gnalk_account_set_id (GnalkAccount *self,
                           gint64        id);

/**
 * gnalk_account_get_unique_key:
 * @self: The account
 *
 * Returns a unique key for this account (server_url + user_id).
 * Used for hashtable lookups and keyring storage.
 *
 * Returns: (transfer full): A newly allocated unique key string
 */
char *gnalk_account_get_unique_key (GnalkAccount *self);

/**
 * gnalk_account_matches:
 * @self: The account
 * @server_url: Server URL to match
 * @user_id: User ID to match
 *
 * Checks if account matches the given server and user.
 *
 * Returns: TRUE if account matches
 */
gboolean gnalk_account_matches (GnalkAccount *self,
                                  const char    *server_url,
                                  const char    *user_id);

G_END_DECLS
