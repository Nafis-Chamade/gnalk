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
 * Handles conversation CRUD operations:
 * - Creating, leaving, and deleting conversations
 * - Setting conversation favorites
 *
 * Note: Periodic sync is handled by GnalkSyncCoordinator.
 *
 * The controller saves data to the database before emitting signals.
 * Window loads cached data directly from DB for instant display.
 *
 * Signals:
 * - "conversation-created" (const char *room_token): New conversation was created
 * - "conversation-left" (const char *room_token): User left a conversation
 * - "conversation-deleted" (const char *room_token): Conversation was deleted
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
                                                   gint64                       account_id);

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
                                           int                          room_type,
                                           const char                  *invite,
                                           const char                  *room_name);

/**
 * gnalk_conversation_controller_leave:
 * @self: The controller
 * @room_token: The room token to leave
 *
 * Leaves a conversation.
 * Emits "conversation-left" on success.
 */
void gnalk_conversation_controller_leave (GnalkConversationController *self,
                                          const char                  *room_token);

/**
 * gnalk_conversation_controller_delete:
 * @self: The controller
 * @room_token: The room token to delete
 *
 * Deletes a conversation.
 * Emits "conversation-deleted" on success.
 */
void gnalk_conversation_controller_delete (GnalkConversationController *self,
                                           const char                  *room_token);

/**
 * gnalk_conversation_controller_set_favorite:
 * @self: The controller
 * @room_token: The room token
 * @favorite: TRUE to mark as favorite, FALSE to unmark
 *
 * Sets or clears the favorite status of a conversation.
 */
void gnalk_conversation_controller_set_favorite (GnalkConversationController *self,
                                                 const char                  *room_token,
                                                 gboolean                     favorite);

/**
 * GnalkSearchContactsCallback:
 * @controller: The conversation controller
 * @response: The JSON response (nullable)
 * @error: Error if operation failed (nullable)
 * @user_data: User data
 *
 * Callback for search_contacts results.
 */
typedef void (*GnalkSearchContactsCallback) (GnalkConversationController *controller,
                                             JsonNode                    *response,
                                             GError                      *error,
                                             gpointer                     user_data);

/**
 * gnalk_conversation_controller_search_contacts:
 * @self: The controller
 * @search_term: The search term
 * @callback: Callback for results
 * @user_data: User data for callback
 *
 * Searches for contacts on the server.
 */
void gnalk_conversation_controller_search_contacts (GnalkConversationController  *self,
                                                    const char                   *search_term,
                                                    gboolean                      include_federated,
                                                    GnalkSearchContactsCallback   callback,
                                                    gpointer                      user_data);

void gnalk_conversation_controller_create_federated (GnalkConversationController *self,
                                                      const char                  *cloud_id);

/**
 * gnalk_conversation_controller_fetch_invitations:
 * @self: The controller
 *
 * Fetches pending federation invitations from the server.
 * Emits "invitations-updated" with a JsonArray of invitations.
 */
void gnalk_conversation_controller_fetch_invitations (GnalkConversationController *self);

/**
 * gnalk_conversation_controller_accept_invitation:
 * @self: The controller
 * @invitation_id: The invitation ID to accept
 *
 * Accepts a federation invitation.
 * Emits "invitation-accepted" on success.
 */
void gnalk_conversation_controller_accept_invitation (GnalkConversationController *self,
                                                      gint64                       invitation_id);

/**
 * gnalk_conversation_controller_decline_invitation:
 * @self: The controller
 * @invitation_id: The invitation ID to decline
 *
 * Declines a federation invitation.
 * Emits "invitation-declined" on success.
 */
void gnalk_conversation_controller_decline_invitation (GnalkConversationController *self,
                                                       gint64                       invitation_id);

G_END_DECLS
