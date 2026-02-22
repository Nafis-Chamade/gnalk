#pragma once

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GNALK_TYPE_APP_STATE (gnalk_app_state_get_type ())
G_DECLARE_FINAL_TYPE (GnalkAppState, gnalk_app_state, GNALK, APP_STATE, GObject)

/**
 * GnalkAppState:
 *
 * Centralized UI-relevant application state.
 * Observable via GObject signals.
 *
 * Signals:
 * - "online-changed": Emitted when online status changes
 * - "loading-changed": Emitted when loading state changes
 * - "sync-status-changed": Emitted when sync status message changes
 */

/**
 * GnalkSyncStatus:
 * @GNALK_SYNC_STATUS_IDLE: No sync in progress
 * @GNALK_SYNC_STATUS_LOADING_CACHE: Loading from local cache
 * @GNALK_SYNC_STATUS_SYNCING: Syncing with server
 * @GNALK_SYNC_STATUS_ERROR: Sync failed
 *
 * Sync status for UI display.
 */
typedef enum {
  GNALK_SYNC_STATUS_IDLE,
  GNALK_SYNC_STATUS_LOADING_CACHE,
  GNALK_SYNC_STATUS_SYNCING,
  GNALK_SYNC_STATUS_ERROR
} GnalkSyncStatus;

/* Constructor */
GnalkAppState *gnalk_app_state_new (void);

/* Online status */

/**
 * gnalk_app_state_is_online:
 * @self: The app state
 *
 * Returns: TRUE if any session is online
 */
gboolean gnalk_app_state_is_online (GnalkAppState *self);

/**
 * gnalk_app_state_set_online:
 * @self: The app state
 * @is_online: Whether online
 *
 * Set online status.
 */
void gnalk_app_state_set_online (GnalkAppState *self,
                                   gboolean        is_online);

/* Loading/sync status */

/**
 * gnalk_app_state_is_loading:
 * @self: The app state
 *
 * Returns: TRUE if any loading is in progress
 */
gboolean gnalk_app_state_is_loading (GnalkAppState *self);

/**
 * gnalk_app_state_set_loading:
 * @self: The app state
 * @is_loading: Whether loading
 *
 * Set loading status.
 */
void gnalk_app_state_set_loading (GnalkAppState *self,
                                    gboolean        is_loading);

/**
 * gnalk_app_state_get_sync_status:
 * @self: The app state
 *
 * Returns: Current sync status
 */
GnalkSyncStatus gnalk_app_state_get_sync_status (GnalkAppState *self);

/**
 * gnalk_app_state_set_sync_status:
 * @self: The app state
 * @status: The sync status
 * @message: Optional status message (can be NULL)
 *
 * Set sync status with optional message.
 */
void gnalk_app_state_set_sync_status (GnalkAppState   *self,
                                        GnalkSyncStatus  status,
                                        const char       *message);

/**
 * gnalk_app_state_get_sync_message:
 * @self: The app state
 *
 * Returns: (transfer none) (nullable): Current sync status message
 */
const char *gnalk_app_state_get_sync_message (GnalkAppState *self);

G_END_DECLS
