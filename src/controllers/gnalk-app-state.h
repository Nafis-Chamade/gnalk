#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include "gnalk-conversation-data.h"

G_BEGIN_DECLS

/* Forward declarations */
typedef struct _GnalkSession GnalkSession;

#define GNALK_TYPE_APP_STATE (gnalk_app_state_get_type ())
G_DECLARE_FINAL_TYPE (GnalkAppState, gnalk_app_state, GNALK, APP_STATE, GObject)

/**
 * GnalkAppState:
 *
 * Single source of truth for UI-relevant application state.
 * Observable via GObject signals - the Window observes this
 * instead of directly accessing sessions or database.
 *
 * Signals:
 * - "conversations-changed": Emitted when conversation list changes
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

/* Conversations */

/**
 * gnalk_app_state_get_conversations:
 * @self: The app state
 *
 * Get the merged list of conversations from all accounts.
 * Sorted by last activity.
 *
 * Returns: (transfer none): The conversations GListStore
 */
GListStore *gnalk_app_state_get_conversations (GnalkAppState *self);

/**
 * gnalk_app_state_set_conversations:
 * @self: The app state
 * @conversations: The conversations to set (will be merged)
 * @session: The session these conversations belong to
 *
 * Replace conversations for a session. Used when loading from cache
 * or receiving server data.
 */
void gnalk_app_state_set_conversations (GnalkAppState *self,
                                          GListStore     *conversations,
                                          GnalkSession  *session);

/**
 * gnalk_app_state_clear_conversations:
 * @self: The app state
 * @session: The session to clear conversations for (or NULL for all)
 *
 * Clear conversations for a session (e.g., when logging out).
 */
void gnalk_app_state_clear_conversations (GnalkAppState *self,
                                            GnalkSession  *session);

/**
 * gnalk_app_state_update_conversation:
 * @self: The app state
 * @conversation: The conversation to update
 *
 * Update a single conversation (e.g., unread count changed).
 */
void gnalk_app_state_update_conversation (GnalkAppState         *self,
                                            GnalkConversationData *conversation);

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
