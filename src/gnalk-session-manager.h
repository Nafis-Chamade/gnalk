#pragma once

#include <glib-object.h>
#include "gnalk-session.h"
#include "gnalk-account-manager.h"
#include "gnalk-database.h"
#include "gnalk-network-monitor.h"

G_BEGIN_DECLS

#define GNALK_TYPE_SESSION_MANAGER (gnalk_session_manager_get_type ())
G_DECLARE_FINAL_TYPE (GnalkSessionManager, gnalk_session_manager, GNALK, SESSION_MANAGER, GObject)

/**
 * GnalkSessionManager:
 *
 * Manages active sessions across multiple accounts. Handles:
 * - Creating/destroying sessions for accounts
 * - Tracking "active" session for UI focus
 * - Aggregating global state (total unread count)
 * - Coordinating autologin for all enabled accounts
 *
 * Signals:
 * - "session-added" (GnalkSession *session)
 *     Emitted when a new session is created
 * - "session-removed" (gint64 account_id)
 *     Emitted when a session is destroyed
 * - "active-session-changed" (GnalkSession *session)
 *     Emitted when the active session changes (can be NULL)
 * - "global-unread-changed" (int total)
 *     Emitted when total unread count across all sessions changes
 * - "any-session-state-changed" (GnalkSession *session, int old_state, int new_state)
 *     Forwarded from any session's state-changed signal
 * - "conversations-changed" (GnalkSession *session)
 *     Emitted when any session's conversation list changes
 */

GnalkSessionManager *gnalk_session_manager_new (GnalkAccountManager *account_manager,
                                                    GnalkDatabase       *database,
                                                    GnalkNetworkMonitor *network_monitor);

/**
 * gnalk_session_manager_get_session:
 * @self: The session manager
 * @account_id: The account ID
 *
 * Gets the session for the given account, or NULL if not logged in.
 *
 * Returns: (transfer none) (nullable): The session
 */
GnalkSession *gnalk_session_manager_get_session (GnalkSessionManager *self,
                                                     gint64                account_id);

/**
 * gnalk_session_manager_get_all_sessions:
 * @self: The session manager
 *
 * Returns all active sessions.
 *
 * Returns: (transfer container) (element-type GnalkSession): List of sessions
 */
GList *gnalk_session_manager_get_all_sessions (GnalkSessionManager *self);

/**
 * gnalk_session_manager_get_session_count:
 * @self: The session manager
 *
 * Returns: The number of active sessions
 */
guint gnalk_session_manager_get_session_count (GnalkSessionManager *self);

/**
 * gnalk_session_manager_start_session:
 * @self: The session manager
 * @account: The account to connect
 * @password: The password (will be stored securely)
 *
 * Creates a session for the account and starts connecting.
 *
 * Returns: (transfer none): The new session
 */
GnalkSession *gnalk_session_manager_start_session (GnalkSessionManager *self,
                                                       GnalkAccount        *account,
                                                       const char           *password);

/**
 * gnalk_session_manager_stop_session:
 * @self: The session manager
 * @account_id: The account ID to disconnect
 *
 * Disconnects and destroys the session for the given account.
 */
void gnalk_session_manager_stop_session (GnalkSessionManager *self,
                                           gint64                account_id);

/**
 * gnalk_session_manager_connect_all:
 * @self: The session manager
 *
 * Starts sessions for all accounts with autologin enabled.
 * Passwords are loaded from the keyring.
 */
void gnalk_session_manager_connect_all (GnalkSessionManager *self);

/**
 * gnalk_session_manager_disconnect_all:
 * @self: The session manager
 *
 * Disconnects all active sessions.
 */
void gnalk_session_manager_disconnect_all (GnalkSessionManager *self);

/**
 * gnalk_session_manager_get_active_session:
 * @self: The session manager
 *
 * Gets the currently active (UI-focused) session.
 *
 * Returns: (transfer none) (nullable): The active session
 */
GnalkSession *gnalk_session_manager_get_active_session (GnalkSessionManager *self);

/**
 * gnalk_session_manager_set_active_session:
 * @self: The session manager
 * @session: The session to make active (or NULL)
 *
 * Sets the active session for UI focus.
 */
void gnalk_session_manager_set_active_session (GnalkSessionManager *self,
                                                  GnalkSession        *session);

/**
 * gnalk_session_manager_set_active_by_account_id:
 * @self: The session manager
 * @account_id: The account ID
 *
 * Sets the active session by account ID.
 *
 * Returns: TRUE if session was found and made active
 */
gboolean gnalk_session_manager_set_active_by_account_id (GnalkSessionManager *self,
                                                           gint64                account_id);

/**
 * gnalk_session_manager_get_global_unread:
 * @self: The session manager
 *
 * Returns: Total unread count across all sessions
 */
int gnalk_session_manager_get_global_unread (GnalkSessionManager *self);

/**
 * gnalk_session_manager_has_any_online:
 * @self: The session manager
 *
 * Returns: TRUE if any session is online
 */
gboolean gnalk_session_manager_has_any_online (GnalkSessionManager *self);

G_END_DECLS
