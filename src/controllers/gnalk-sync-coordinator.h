#pragma once

#include <glib-object.h>
#include "gnalk-data-repository.h"
#include "gnalk-http-client.h"
#include "gnalk-notification-service.h"

G_BEGIN_DECLS

#define GNALK_TYPE_SYNC_COORDINATOR (gnalk_sync_coordinator_get_type ())
G_DECLARE_FINAL_TYPE (GnalkSyncCoordinator, gnalk_sync_coordinator,
                      GNALK, SYNC_COORDINATOR, GObject)

/**
 * GnalkSyncTrigger:
 * @GNALK_SYNC_TRIGGER_TIMER: Periodic timer (20s)
 * @GNALK_SYNC_TRIGGER_PUSH: Push notification received
 * @GNALK_SYNC_TRIGGER_USER_ACTION: User pull-to-refresh
 * @GNALK_SYNC_TRIGGER_RECONNECT: Network reconnection
 *
 * Indicates what triggered a sync operation.
 */
typedef enum {
  GNALK_SYNC_TRIGGER_TIMER,
  GNALK_SYNC_TRIGGER_PUSH,
  GNALK_SYNC_TRIGGER_USER_ACTION,
  GNALK_SYNC_TRIGGER_RECONNECT,
} GnalkSyncTrigger;

/**
 * GnalkSyncCoordinator:
 *
 * Centralizes all synchronization logic for conversations and messages.
 * Replaces the polling in ConversationController and all of BackgroundSync.
 *
 * Features:
 * - Periodic 20s conversation polling (when no room active)
 * - Push notification triggered sync
 * - Message fetching for rooms with new messages
 * - Notification display for new messages
 * - Coordination with MessageController (skips active room)
 *
 * Signals:
 * - "conversations-synced": Conversations updated in DB
 * - "room-messages-synced" (const char *room_token, guint count): Messages synced for room
 * - "sync-started" (GnalkSyncTrigger trigger): Sync operation started
 * - "sync-finished" (gboolean success): Sync operation completed
 * - "error" (const char *message): Sync error occurred
 */

/**
 * gnalk_sync_coordinator_new:
 * @repository: The data repository for caching
 * @notification_service: Service for desktop notifications
 *
 * Creates a new sync coordinator.
 *
 * Returns: (transfer full): A new sync coordinator
 */
GnalkSyncCoordinator *gnalk_sync_coordinator_new (
    GnalkDataRepository      *repository,
    GnalkNotificationService *notification_service);

/**
 * gnalk_sync_coordinator_set_account:
 * @self: The sync coordinator
 * @account_id: The account ID for database operations
 * @user_id: The current user's ID (to filter out own messages from notifications)
 *
 * Sets the active account for sync operations. Must be called after login.
 */
void gnalk_sync_coordinator_set_account (GnalkSyncCoordinator *self,
                                         gint64                account_id,
                                         const char           *user_id);

/**
 * gnalk_sync_coordinator_set_http_client:
 * @self: The sync coordinator
 * @http_client: The HTTP client to use for requests
 *
 * Sets the HTTP client for sync operations. Call before triggering sync.
 */
void gnalk_sync_coordinator_set_http_client (GnalkSyncCoordinator *self,
                                             GnalkHttpClient      *http_client);

/**
 * gnalk_sync_coordinator_trigger:
 * @self: The sync coordinator
 * @trigger: What triggered this sync
 *
 * Triggers a sync operation. Fetches conversations and syncs messages
 * for rooms with updates.
 */
void gnalk_sync_coordinator_trigger (GnalkSyncCoordinator *self,
                                     GnalkSyncTrigger      trigger);

/**
 * gnalk_sync_coordinator_sync_room:
 * @self: The sync coordinator
 * @room_token: The room to sync messages for
 *
 * Syncs messages for a specific room. Used for targeted sync when
 * we know a specific room has updates.
 */
void gnalk_sync_coordinator_sync_room (GnalkSyncCoordinator *self,
                                       const char           *room_token);

/**
 * gnalk_sync_coordinator_start_periodic_sync:
 * @self: The sync coordinator
 *
 * Starts the 20-second periodic sync timer.
 */
void gnalk_sync_coordinator_start_periodic_sync (GnalkSyncCoordinator *self);

/**
 * gnalk_sync_coordinator_stop_periodic_sync:
 * @self: The sync coordinator
 *
 * Stops the periodic sync timer.
 */
void gnalk_sync_coordinator_stop_periodic_sync (GnalkSyncCoordinator *self);

/**
 * gnalk_sync_coordinator_set_active_room:
 * @self: The sync coordinator
 * @room_token: (nullable): The currently active room token, or NULL
 *
 * Sets the currently active room. This room will be skipped during
 * background sync (MessageController handles it via long-polling).
 * Messages in this room won't trigger notifications when window is focused.
 */
void gnalk_sync_coordinator_set_active_room (GnalkSyncCoordinator *self,
                                             const char           *room_token);

/**
 * gnalk_sync_coordinator_set_window_focused:
 * @self: The sync coordinator
 * @focused: Whether the application window is focused
 *
 * Sets window focus state for notification decisions.
 */
void gnalk_sync_coordinator_set_window_focused (GnalkSyncCoordinator *self,
                                                gboolean              focused);

/**
 * gnalk_sync_coordinator_is_syncing:
 * @self: The sync coordinator
 *
 * Returns: TRUE if a sync operation is currently in progress
 */
gboolean gnalk_sync_coordinator_is_syncing (GnalkSyncCoordinator *self);

/**
 * gnalk_sync_coordinator_get_account_id:
 * @self: The sync coordinator
 *
 * Returns: The account ID currently configured for sync operations
 */
gint64 gnalk_sync_coordinator_get_account_id (GnalkSyncCoordinator *self);

/**
 * gnalk_sync_coordinator_clear_active_call:
 * @self: The sync coordinator
 * @room_token: The room whose call state to clear
 *
 * Clears the tracked active call for a room, preventing re-notification
 * after user interaction (accept/refuse).
 */
void gnalk_sync_coordinator_clear_active_call (GnalkSyncCoordinator *self,
                                                const char            *room_token);

/**
 * gnalk_sync_coordinator_set_has_threads:
 * @self: The sync coordinator
 * @has_threads: Whether the server supports threads
 *
 * Enables thread syncing when TRUE. When enabled, subscribed threads
 * are fetched after each conversation sync and emitted via "threads-synced".
 */
void gnalk_sync_coordinator_set_has_threads (GnalkSyncCoordinator *self,
                                             gboolean              has_threads);

G_END_DECLS
