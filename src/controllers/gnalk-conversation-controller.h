#pragma once

#include <glib-object.h>
#include "gnalk-data-repository.h"
#include "gnalk-http-client.h"

G_BEGIN_DECLS

#define GNALK_TYPE_CONVERSATION_CONTROLLER (gnalk_conversation_controller_get_type ())
G_DECLARE_FINAL_TYPE (GnalkConversationController, gnalk_conversation_controller, GNALK, CONVERSATION_CONTROLLER, GObject)

/**
 * GnalkConversationController:
 *
 * Handles conversation list operations and polling:
 * - Periodic polling for conversation updates (20s when not in room)
 * - Creating, leaving, and deleting conversations
 * - Setting conversation favorites
 * - Detecting new messages for background sync
 *
 * The controller saves data to the database before emitting signals.
 * Window loads cached data directly from DB for instant display.
 * Controller signals notify only when server returns NEW/changed data.
 *
 * Signals:
 * - "conversations-updated": Server returned different data than cache, UI should reload from DB
 * - "conversation-created" (const char *room_token): New conversation was created
 * - "conversation-left" (const char *room_token): User left a conversation
 * - "conversation-deleted" (const char *room_token): Conversation was deleted
 * - "new-messages-detected" (const char *room_token, gint64 last_message_id): Room has new messages to sync
 * - "error" (const char *message): An error occurred
 */

/**
 * gnalk_conversation_controller_new:
 * @repository: The data repository for caching
 * @http_client: The HTTP client for server communication
 *
 * Creates a new conversation controller.
 *
 * Returns: (transfer full): A new conversation controller
 */
GnalkConversationController *gnalk_conversation_controller_new (GnalkDataRepository *repository,
                                                                GnalkHttpClient     *http_client);

/**
 * gnalk_conversation_controller_set_account_id:
 * @self: The controller
 * @account_id: The account ID for database operations
 *
 * Sets the account ID for database operations.
 */
void gnalk_conversation_controller_set_account_id (GnalkConversationController *self,
                                                      gint64                         account_id);

/**
 * gnalk_conversation_controller_start_polling:
 * @self: The controller
 *
 * Starts the 20-second conversation polling timer.
 * Polling is automatically stopped when a room is marked active.
 */
void gnalk_conversation_controller_start_polling (GnalkConversationController *self);

/**
 * gnalk_conversation_controller_stop_polling:
 * @self: The controller
 *
 * Stops the conversation polling timer.
 */
void gnalk_conversation_controller_stop_polling (GnalkConversationController *self);

/**
 * gnalk_conversation_controller_set_room_active:
 * @self: The controller
 * @has_active_room: TRUE if user is viewing a room
 *
 * Notifies the controller whether a room is currently active.
 * When active, conversation polling is paused (message polling handles it).
 * When inactive, conversation polling resumes.
 */
void gnalk_conversation_controller_set_room_active (GnalkConversationController *self,
                                                       gboolean                       has_active_room);

/**
 * gnalk_conversation_controller_refresh:
 * @self: The controller
 *
 * Manually triggers a conversation refresh from the server.
 */
void gnalk_conversation_controller_refresh (GnalkConversationController *self);

/**
 * gnalk_conversation_controller_create:
 * @self: The controller
 * @room_type: The room type (1=one-to-one, 2=group, 3=public)
 * @invite: The user ID to invite (for 1:1) or NULL
 * @room_name: The room name (for group/public) or NULL
 *
 * Creates a new conversation.
 * Emits "conversation-created" on success.
 */
void gnalk_conversation_controller_create (GnalkConversationController *self,
                                              int                           room_type,
                                              const char                   *invite,
                                              const char                   *room_name);

/**
 * gnalk_conversation_controller_leave:
 * @self: The controller
 * @room_token: The room token to leave
 *
 * Leaves a conversation.
 * Emits "conversation-left" on success.
 */
void gnalk_conversation_controller_leave (GnalkConversationController *self,
                                             const char                   *room_token);

/**
 * gnalk_conversation_controller_delete:
 * @self: The controller
 * @room_token: The room token to delete
 *
 * Deletes a conversation.
 * Emits "conversation-deleted" on success.
 */
void gnalk_conversation_controller_delete (GnalkConversationController *self,
                                              const char                   *room_token);

/**
 * gnalk_conversation_controller_set_favorite:
 * @self: The controller
 * @room_token: The room token
 * @favorite: TRUE to mark as favorite, FALSE to unmark
 *
 * Sets or clears the favorite status of a conversation.
 */
void gnalk_conversation_controller_set_favorite (GnalkConversationController *self,
                                                    const char                   *room_token,
                                                    gboolean                       favorite);

/**
 * gnalk_conversation_controller_is_polling:
 * @self: The controller
 *
 * Returns: TRUE if conversation polling is currently active
 */
gboolean gnalk_conversation_controller_is_polling (GnalkConversationController *self);

G_END_DECLS
