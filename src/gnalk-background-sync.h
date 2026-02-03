#pragma once

#include <glib-object.h>
#include "gnalk-data-repository.h"
#include "gnalk-http-client.h"
#include "gnalk-notification-service.h"

G_BEGIN_DECLS

#define GNALK_TYPE_BACKGROUND_SYNC (gnalk_background_sync_get_type ())
G_DECLARE_FINAL_TYPE (GnalkBackgroundSync, gnalk_background_sync, GNALK, BACKGROUND_SYNC, GObject)

/**
 * GnalkBackgroundSync:
 *
 * Service that syncs messages in the background and triggers notifications.
 * Works without a window for service mode.
 *
 * Emits "messages-synced" signal when new messages are saved to database,
 * allowing the window (if present) to update its UI.
 */

GnalkBackgroundSync *gnalk_background_sync_new (GnalkDataRepository      *repository,
                                                GnalkHttpClient          *http_client,
                                                GnalkNotificationService *notification_service);

/**
 * gnalk_background_sync_set_account:
 * @self: The background sync service
 * @account_id: The account ID for database operations
 * @user_id: The current user's ID (to filter out own messages from notifications)
 *
 * Sets the active account for sync operations. Must be called after login.
 */
void gnalk_background_sync_set_account (GnalkBackgroundSync *self,
                                          gint64                account_id,
                                          const char           *user_id);

/**
 * gnalk_background_sync_set_http_client:
 * @self: The background sync service
 * @http_client: The HTTP client to use for requests
 *
 * Sets the HTTP client to use for sync requests. Call this before triggering
 * if you want to use a different client than the one passed to _new().
 */
void gnalk_background_sync_set_http_client (GnalkBackgroundSync *self,
                                              GnalkHttpClient      *http_client);

/**
 * gnalk_background_sync_trigger:
 * @self: The background sync service
 *
 * Triggers a background sync. Fetches conversations and syncs new messages
 * for rooms that have updates. Call this when a push notification is received.
 */
void gnalk_background_sync_trigger (GnalkBackgroundSync *self);

/**
 * gnalk_background_sync_set_active_room:
 * @self: The background sync service
 * @room_token: (nullable): The currently open room token, or NULL if none
 *
 * Sets the currently active room. Messages in this room won't trigger
 * notifications when the window is focused.
 */
void gnalk_background_sync_set_active_room (GnalkBackgroundSync *self,
                                              const char           *room_token);

/**
 * gnalk_background_sync_set_window_focused:
 * @self: The background sync service
 * @focused: Whether the window is currently focused
 *
 * Sets whether the application window is focused. Used to determine
 * whether to show notifications.
 */
void gnalk_background_sync_set_window_focused (GnalkBackgroundSync *self,
                                                 gboolean              focused);

G_END_DECLS
