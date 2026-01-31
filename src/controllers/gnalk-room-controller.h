#pragma once

#include <glib-object.h>
#include "gnalk-app-state.h"
#include "gnalk-data-repository.h"
#include "gnalk-session-manager.h"
#include "gnalk-conversation-controller.h"
#include "gnalk-participant-controller.h"

G_BEGIN_DECLS

#define GNALK_TYPE_ROOM_CONTROLLER (gnalk_room_controller_get_type ())
G_DECLARE_FINAL_TYPE (GnalkRoomController, gnalk_room_controller, GNALK, ROOM_CONTROLLER, GObject)

/**
 * GnalkRoomController:
 *
 * Orchestrates room operations through Session + AppState + DataRepository.
 * This centralizes room switching, message loading, and message sending logic
 * that was previously scattered throughout the Window.
 *
 * The RoomController:
 * - Manages the currently active room
 * - Loads messages from cache first, then syncs with server
 * - Handles room join/leave operations
 * - Coordinates message sending and reactions
 * - Prevents race conditions via generation counters
 *
 * Signals:
 * - "room-opened" (const char *token): Emitted when a room is successfully opened
 * - "room-closed": Emitted when the current room is closed
 * - "messages-loading": Emitted when messages are being loaded
 * - "messages-loaded": Emitted when messages finish loading
 * - "error" (const char *message): Emitted on errors
 */

/**
 * gnalk_room_controller_new:
 * @app_state: The app state to update
 * @data_repository: The data repository for cache access
 * @session_manager: The session manager for network operations
 *
 * Creates a new room controller.
 *
 * Returns: (transfer full): A new room controller
 */
GnalkRoomController *gnalk_room_controller_new (GnalkAppState       *app_state,
                                                    GnalkDataRepository *data_repository,
                                                    GnalkSessionManager *session_manager);

/**
 * gnalk_room_controller_open_room:
 * @self: The room controller
 * @token: The room token to open
 * @session: The session this room belongs to
 * @room_type: The room type (1=one-to-one, 2=group, etc.)
 *
 * Opens a room. This will:
 * 1. Update AppState with new active room
 * 2. Load messages from cache (fast, appears immediately)
 * 3. Load participants from cache
 * 4. Join the room via HTTP (signals, marks read)
 * 5. Sync messages from server in background
 * 6. Notify ParticipantController to start loading/polling
 * 7. Notify ConversationController to pause polling
 *
 * If a room is already open, it will be closed first.
 */
void gnalk_room_controller_open_room (GnalkRoomController *self,
                                         const char           *token,
                                         GnalkSession        *session,
                                         int                   room_type);

/**
 * gnalk_room_controller_close_room:
 * @self: The room controller
 *
 * Closes the currently open room. This will:
 * 1. Leave the room via HTTP (stops signaling, allows push notifications)
 * 2. Clear messages and participants from AppState
 * 3. Clear active room token
 */
void gnalk_room_controller_close_room (GnalkRoomController *self);

/**
 * gnalk_room_controller_get_current_room:
 * @self: The room controller
 *
 * Returns: (transfer none) (nullable): The current room token, or NULL
 */
const char *gnalk_room_controller_get_current_room (GnalkRoomController *self);

/**
 * gnalk_room_controller_get_current_session:
 * @self: The room controller
 *
 * Returns: (transfer none) (nullable): The current room's session, or NULL
 */
GnalkSession *gnalk_room_controller_get_current_session (GnalkRoomController *self);

/**
 * gnalk_room_controller_load_older_messages:
 * @self: The room controller
 *
 * Loads older messages (history) for the current room.
 * Messages will be prepended to the AppState message list.
 */
void gnalk_room_controller_load_older_messages (GnalkRoomController *self);

/**
 * gnalk_room_controller_send_message:
 * @self: The room controller
 * @text: The message text
 * @reply_to: Message ID to reply to (0 for no reply)
 *
 * Sends a message to the current room.
 */
void gnalk_room_controller_send_message (GnalkRoomController *self,
                                            const char           *text,
                                            gint64                reply_to);

/**
 * gnalk_room_controller_mark_read:
 * @self: The room controller
 *
 * Marks the current room as read up to the last message.
 */
void gnalk_room_controller_mark_read (GnalkRoomController *self);

/**
 * gnalk_room_controller_refresh:
 * @self: The room controller
 *
 * Forces a refresh of the current room's messages from the server.
 */
void gnalk_room_controller_refresh (GnalkRoomController *self);

/**
 * gnalk_room_controller_is_loading:
 * @self: The room controller
 *
 * Returns: TRUE if messages are currently being loaded
 */
gboolean gnalk_room_controller_is_loading (GnalkRoomController *self);

/**
 * gnalk_room_controller_set_conversation_controller:
 * @self: The room controller
 * @conversation_controller: (nullable): The conversation controller
 *
 * Sets the conversation controller. The RoomController will notify it
 * when a room is opened (to pause polling) or closed (to resume polling).
 */
void gnalk_room_controller_set_conversation_controller (GnalkRoomController         *self,
                                                           GnalkConversationController *conversation_controller);

/**
 * gnalk_room_controller_set_participant_controller:
 * @self: The room controller
 * @participant_controller: (nullable): The participant controller
 *
 * Sets the participant controller. The RoomController will notify it
 * when a room is opened (to start loading/polling participants) or closed.
 */
void gnalk_room_controller_set_participant_controller (GnalkRoomController        *self,
                                                          GnalkParticipantController *participant_controller);

G_END_DECLS
