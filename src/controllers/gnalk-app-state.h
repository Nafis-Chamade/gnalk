#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include "gnalk-conversation-data.h"
#include "gnalk-message-data.h"

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
 * This enables:
 * - Clean separation between data layer and presentation
 * - UI updates only when data actually changes
 * - Consistent state across different data sources (cache vs server)
 *
 * Signals:
 * - "conversations-changed": Emitted when conversation list changes
 * - "messages-changed": Emitted when messages for active room change
 * - "participants-changed": Emitted when participants for active room change
 * - "active-room-changed": Emitted when active room token changes
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

/* Messages (active room only) */

/**
 * gnalk_app_state_get_messages:
 * @self: The app state
 *
 * Get the messages for the currently active room.
 *
 * Returns: (transfer none): The messages GListStore
 */
GListStore *gnalk_app_state_get_messages (GnalkAppState *self);

/**
 * gnalk_app_state_set_messages:
 * @self: The app state
 * @messages: The messages to set
 *
 * Replace messages for the active room.
 */
void gnalk_app_state_set_messages (GnalkAppState *self,
                                     GListStore     *messages);

/**
 * gnalk_app_state_append_message:
 * @self: The app state
 * @message: The message to append
 *
 * Append a new message to the active room.
 */
void gnalk_app_state_append_message (GnalkAppState    *self,
                                       GnalkMessageData *message);

/**
 * gnalk_app_state_prepend_messages:
 * @self: The app state
 * @messages: Older messages to prepend
 *
 * Prepend older messages (for history loading).
 */
void gnalk_app_state_prepend_messages (GnalkAppState *self,
                                         GListStore     *messages);

/**
 * gnalk_app_state_update_message:
 * @self: The app state
 * @message: The message to update
 *
 * Update a message (e.g., reactions changed, edited).
 */
void gnalk_app_state_update_message (GnalkAppState    *self,
                                       GnalkMessageData *message);

/**
 * gnalk_app_state_clear_messages:
 * @self: The app state
 *
 * Clear messages (when switching rooms).
 */
void gnalk_app_state_clear_messages (GnalkAppState *self);

/* Participants (active room only) */

/**
 * gnalk_app_state_get_participants:
 * @self: The app state
 *
 * Get the participants for the currently active room.
 *
 * Returns: (transfer none): The participants GListStore
 */
GListStore *gnalk_app_state_get_participants (GnalkAppState *self);

/**
 * gnalk_app_state_set_participants:
 * @self: The app state
 * @participants: The participants to set
 *
 * Set participants for the active room.
 */
void gnalk_app_state_set_participants (GnalkAppState *self,
                                         GListStore     *participants);

/**
 * gnalk_app_state_clear_participants:
 * @self: The app state
 *
 * Clear participants (when switching rooms).
 */
void gnalk_app_state_clear_participants (GnalkAppState *self);

/* Active room */

/**
 * gnalk_app_state_get_active_room_token:
 * @self: The app state
 *
 * Returns: (transfer none) (nullable): The active room token
 */
const char *gnalk_app_state_get_active_room_token (GnalkAppState *self);

/**
 * gnalk_app_state_get_active_session:
 * @self: The app state
 *
 * Returns: (transfer none) (nullable): The session for the active room
 */
GnalkSession *gnalk_app_state_get_active_session (GnalkAppState *self);

/**
 * gnalk_app_state_set_active_room:
 * @self: The app state
 * @token: The room token (or NULL to clear)
 * @session: The session this room belongs to
 *
 * Set the active room. Clears messages and participants.
 */
void gnalk_app_state_set_active_room (GnalkAppState *self,
                                        const char     *token,
                                        GnalkSession  *session);

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

/* Room switch generation counter (for cancelling stale operations) */

/**
 * gnalk_app_state_get_room_generation:
 * @self: The app state
 *
 * Get the current room switch generation. Use this to detect
 * if the room changed during an async operation.
 *
 * Returns: The current generation counter
 */
guint64 gnalk_app_state_get_room_generation (GnalkAppState *self);

G_END_DECLS
