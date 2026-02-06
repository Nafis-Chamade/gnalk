#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include "gnalk-database.h"
#include "gnalk-conversation-data.h"
#include "gnalk-message-data.h"

G_BEGIN_DECLS

/* Forward declaration */
typedef struct _GnalkAccount GnalkAccount;

#define GNALK_TYPE_DATA_REPOSITORY (gnalk_data_repository_get_type ())
G_DECLARE_FINAL_TYPE (GnalkDataRepository, gnalk_data_repository, GNALK, DATA_REPOSITORY, GObject)

/**
 * GnalkDataRepository:
 *
 * Async wrapper around GnalkDatabase for non-blocking data access.
 * All methods run database operations in a thread pool and invoke
 * callbacks on the main thread.
 *
 * This enables instant window display - the UI shows immediately
 * while data loads asynchronously in the background.
 */

/**
 * GnalkDataRepositoryCallback:
 * @repository: The data repository
 * @result: The result (type depends on the operation)
 * @error: Error if operation failed, or NULL
 * @user_data: User data passed to the async function
 *
 * Callback signature for async data repository operations.
 */
typedef void (*GnalkDataRepositoryCallback) (GnalkDataRepository *repository,
                                               gpointer              result,
                                               GError               *error,
                                               gpointer              user_data);

/* Constructor */
GnalkDataRepository *gnalk_data_repository_new (void);

/**
 * gnalk_data_repository_open_async:
 * @self: The data repository
 * @callback: Callback when complete (result is gboolean success)
 * @user_data: User data for callback
 *
 * Opens the database asynchronously. Must be called before other operations.
 */
void gnalk_data_repository_open_async (GnalkDataRepository          *self,
                                          GnalkDataRepositoryCallback   callback,
                                          gpointer                       user_data);

/**
 * gnalk_data_repository_is_open:
 * @self: The data repository
 *
 * Returns: TRUE if the database is open
 */
gboolean gnalk_data_repository_is_open (GnalkDataRepository *self);

/**
 * gnalk_data_repository_get_database:
 * @self: The data repository
 *
 * Get the underlying database for synchronous operations when needed.
 * Use with care - prefer async methods.
 *
 * Returns: (transfer none): The database (may be NULL if not open)
 */
GnalkDatabase *gnalk_data_repository_get_database (GnalkDataRepository *self);

/* Account operations */

/**
 * gnalk_data_repository_load_accounts_async:
 * @self: The data repository
 * @callback: Callback when complete (result is GListStore of GnalkAccount)
 * @user_data: User data for callback
 *
 * Loads all accounts from the database asynchronously.
 */
void gnalk_data_repository_load_accounts_async (GnalkDataRepository          *self,
                                                   GnalkDataRepositoryCallback   callback,
                                                   gpointer                       user_data);

/* Conversation operations */

/**
 * gnalk_data_repository_load_conversations_async:
 * @self: The data repository
 * @account_id: The account ID to load conversations for
 * @callback: Callback when complete (result is GListStore of GnalkConversationData)
 * @user_data: User data for callback
 *
 * Loads conversations for an account asynchronously.
 */
void gnalk_data_repository_load_conversations_async (GnalkDataRepository          *self,
                                                        gint64                         account_id,
                                                        GnalkDataRepositoryCallback   callback,
                                                        gpointer                        user_data);

/**
 * gnalk_data_repository_save_conversation_async:
 * @self: The data repository
 * @account_id: The account ID
 * @conversation: The conversation to save
 * @callback: Callback when complete (result is NULL, check error)
 * @user_data: User data for callback
 *
 * Saves a conversation asynchronously.
 */
void gnalk_data_repository_save_conversation_async (GnalkDataRepository          *self,
                                                       gint64                         account_id,
                                                       GnalkConversationData        *conversation,
                                                       GnalkDataRepositoryCallback   callback,
                                                       gpointer                        user_data);

/* Message operations */

/**
 * gnalk_data_repository_load_messages_async:
 * @self: The data repository
 * @account_id: The account ID
 * @conversation_token: The conversation token
 * @limit: Maximum number of messages to load
 * @before_id: Load messages before this ID (0 for latest)
 * @callback: Callback when complete (result is GListStore of GnalkMessageData)
 * @user_data: User data for callback
 *
 * Loads messages for a conversation asynchronously.
 */
void gnalk_data_repository_load_messages_async (GnalkDataRepository          *self,
                                                   gint64                         account_id,
                                                   const char                    *conversation_token,
                                                   gint                           limit,
                                                   gint64                         before_id,
                                                   GnalkDataRepositoryCallback   callback,
                                                   gpointer                        user_data);

/**
 * gnalk_data_repository_save_message_async:
 * @self: The data repository
 * @account_id: The account ID
 * @conversation_token: The conversation token
 * @message: The message to save
 * @callback: Callback when complete (result is NULL, check error)
 * @user_data: User data for callback
 *
 * Saves a message asynchronously.
 */
void gnalk_data_repository_save_message_async (GnalkDataRepository          *self,
                                                  gint64                         account_id,
                                                  const char                    *conversation_token,
                                                  GnalkMessageData             *message,
                                                  GnalkDataRepositoryCallback   callback,
                                                  gpointer                        user_data);

/* Participants operations */

/**
 * gnalk_data_repository_load_participants_async:
 * @self: The data repository
 * @account_id: The account ID
 * @conversation_token: The conversation token
 * @callback: Callback when complete (result is JsonArray)
 * @user_data: User data for callback
 *
 * Loads participants for a conversation asynchronously.
 */
void gnalk_data_repository_load_participants_async (GnalkDataRepository          *self,
                                                       gint64                         account_id,
                                                       const char                    *conversation_token,
                                                       GnalkDataRepositoryCallback   callback,
                                                       gpointer                        user_data);

/**
 * gnalk_data_repository_save_participants_async:
 * @self: The data repository
 * @account_id: The account ID
 * @conversation_token: The conversation token
 * @participants: The participants array to save
 * @callback: Callback when complete (result is NULL, check error)
 * @user_data: User data for callback
 *
 * Saves participants for a conversation asynchronously.
 */
void gnalk_data_repository_save_participants_async (GnalkDataRepository          *self,
                                                       gint64                         account_id,
                                                       const char                    *conversation_token,
                                                       JsonArray                     *participants,
                                                       GnalkDataRepositoryCallback   callback,
                                                       gpointer                        user_data);

/* Synchronous operations - prefer async when possible */

/**
 * gnalk_data_repository_save_conversation:
 * @self: The data repository
 * @account_id: The account ID
 * @conversation: The conversation to save
 *
 * Saves a conversation synchronously and emits data-changed signal.
 */
void gnalk_data_repository_save_conversation (GnalkDataRepository   *self,
                                              gint64                 account_id,
                                              GnalkConversationData *conversation);

/**
 * gnalk_data_repository_save_conversations:
 * @self: The data repository
 * @account_id: The account ID
 * @conversations: Array of GnalkConversationData to save
 *
 * Saves multiple conversations synchronously and emits a single data-changed signal.
 * Use this for batch operations to avoid multiple UI refreshes.
 */
void gnalk_data_repository_save_conversations (GnalkDataRepository *self,
                                               gint64               account_id,
                                               GPtrArray           *conversations);

/**
 * gnalk_data_repository_delete_conversation:
 * @self: The data repository
 * @account_id: The account ID
 * @token: The conversation token
 *
 * Deletes a conversation synchronously and emits data-changed signal.
 */
void gnalk_data_repository_delete_conversation (GnalkDataRepository *self,
                                                gint64               account_id,
                                                const char          *token);

/**
 * gnalk_data_repository_delete_stale_conversations:
 * @self: The data repository
 * @account_id: The account ID
 * @valid_tokens: Array of valid tokens (conversations not in this list are deleted)
 *
 * Deletes conversations not in valid_tokens and emits data-changed signal.
 *
 * Returns: Number of deleted conversations
 */
int gnalk_data_repository_delete_stale_conversations (GnalkDataRepository *self,
                                                      gint64               account_id,
                                                      GPtrArray           *valid_tokens);

/**
 * gnalk_data_repository_save_message:
 * @self: The data repository
 * @account_id: The account ID
 * @conversation_token: The conversation token
 * @message: The message to save
 *
 * Saves a message synchronously and emits data-changed signal.
 */
void gnalk_data_repository_save_message (GnalkDataRepository *self,
                                         gint64               account_id,
                                         const char          *conversation_token,
                                         GnalkMessageData    *message);

/**
 * gnalk_data_repository_get_last_known_message_id:
 * @self: The data repository
 * @account_id: The account ID
 * @conversation_token: The conversation token
 *
 * Gets the last known message ID for a conversation synchronously.
 *
 * Returns: The last known message ID, or 0 if not set
 */
gint64 gnalk_data_repository_get_last_known_message_id (GnalkDataRepository *self,
                                                        gint64               account_id,
                                                        const char          *conversation_token);

/**
 * gnalk_data_repository_set_last_known_message_id:
 * @self: The data repository
 * @account_id: The account ID
 * @conversation_token: The conversation token
 * @message_id: The message ID to set
 *
 * Sets the last known message ID synchronously (no signal emitted - internal state).
 */
void gnalk_data_repository_set_last_known_message_id (GnalkDataRepository *self,
                                                      gint64               account_id,
                                                      const char          *conversation_token,
                                                      gint64               message_id);

/* Sync state operations (async) */

/**
 * gnalk_data_repository_get_last_known_message_id_async:
 * @self: The data repository
 * @account_id: The account ID
 * @conversation_token: The conversation token
 * @callback: Callback when complete (result is gint64* pointing to message ID)
 * @user_data: User data for callback
 *
 * Gets the last known message ID for a conversation asynchronously.
 */
void gnalk_data_repository_get_last_known_message_id_async (GnalkDataRepository          *self,
                                                               gint64                         account_id,
                                                               const char                    *conversation_token,
                                                               GnalkDataRepositoryCallback   callback,
                                                               gpointer                        user_data);

/**
 * gnalk_data_repository_set_last_known_message_id_async:
 * @self: The data repository
 * @account_id: The account ID
 * @conversation_token: The conversation token
 * @message_id: The message ID to set
 * @callback: Callback when complete (result is NULL, check error)
 * @user_data: User data for callback
 *
 * Sets the last known message ID for a conversation asynchronously.
 */
void gnalk_data_repository_set_last_known_message_id_async (GnalkDataRepository          *self,
                                                               gint64                         account_id,
                                                               const char                    *conversation_token,
                                                               gint64                         message_id,
                                                               GnalkDataRepositoryCallback   callback,
                                                               gpointer                        user_data);

/**
 * gnalk_data_repository_set_has_more_history:
 * @self: The data repository
 * @account_id: The account ID
 * @conversation_token: The conversation token
 * @has_more: Whether there is more history to load
 *
 * Sets the has_more_history flag for a conversation synchronously.
 */
void gnalk_data_repository_set_has_more_history (GnalkDataRepository *self,
                                                  gint64               account_id,
                                                  const char          *conversation_token,
                                                  gboolean             has_more);

/**
 * gnalk_data_repository_update_conversation_favorite:
 * @self: The data repository
 * @account_id: The account ID
 * @conversation_token: The conversation token
 * @is_favorite: Whether the conversation is a favorite
 *
 * Updates the conversation's favorite status synchronously.
 * Emits data-changed signal with scope "conversations".
 */
void gnalk_data_repository_update_conversation_favorite (GnalkDataRepository *self,
                                                            gint64               account_id,
                                                            const char          *conversation_token,
                                                            gboolean             is_favorite);

/**
 * gnalk_data_repository_update_conversation_last_message:
 * @self: The data repository
 * @account_id: The account ID
 * @conversation_token: The conversation token
 * @last_activity: The timestamp for last activity
 * @last_message_id: ID of the last message
 * @last_message_text: Raw text of the last message
 * @last_message_actor_id: Actor ID of the message author
 * @last_message_actor_name: Display name of the message author
 * @last_message_type: Message type (system, comment, etc.)
 * @last_message_params_json: JSON string of message parameters (nullable)
 *
 * Updates the conversation's last message data synchronously.
 * Emits data-changed signal with scope "conversations".
 */
void gnalk_data_repository_update_conversation_last_message (GnalkDataRepository *self,
                                                               gint64               account_id,
                                                               const char          *conversation_token,
                                                               gint64               last_activity,
                                                               gint64               last_message_id,
                                                               const char          *last_message_text,
                                                               const char          *last_message_actor_id,
                                                               const char          *last_message_actor_name,
                                                               const char          *last_message_type,
                                                               const char          *last_message_params_json);

/**
 * gnalk_data_repository_update_conversation_read_status:
 * @self: The data repository
 * @account_id: The account ID
 * @conversation_token: The conversation token
 * @last_read_message: ID of the last read message
 *
 * Updates the conversation's read status (last_read_message and sets unread_messages to 0).
 * Emits data-changed signal with scope "conversations".
 */
void gnalk_data_repository_update_conversation_read_status (GnalkDataRepository *self,
                                                               gint64               account_id,
                                                               const char          *conversation_token,
                                                               gint64               last_read_message);

/**
 * gnalk_data_repository_get_account_id:
 * @self: The data repository
 * @server_url: The server URL
 * @user_id: The user ID
 *
 * Looks up or creates an account ID synchronously.
 *
 * Returns: The account ID, or 0 on failure
 */
gint64 gnalk_data_repository_get_account_id (GnalkDataRepository *self,
                                             const char          *server_url,
                                             const char          *user_id);

/**
 * gnalk_data_repository_reset_participants_status:
 * @self: The data repository
 * @account_id: The account ID
 *
 * Resets all participants' online status for an account synchronously.
 */
void gnalk_data_repository_reset_participants_status (GnalkDataRepository *self,
                                                      gint64               account_id);

/**
 * gnalk_data_repository_load_conversations:
 * @self: The data repository
 * @account_id: The account ID
 *
 * Loads conversations synchronously.
 *
 * Returns: (transfer full) (nullable): A #GListStore of #GnalkConversationData
 */
GListStore *gnalk_data_repository_load_conversations (GnalkDataRepository *self,
                                                      gint64               account_id);

/**
 * gnalk_data_repository_load_participants:
 * @self: The data repository
 * @account_id: The account ID
 * @conversation_token: The conversation token
 *
 * Loads participants for a conversation synchronously.
 *
 * Returns: (transfer full) (nullable): A #JsonArray of participants
 */
JsonArray *gnalk_data_repository_load_participants (GnalkDataRepository *self,
                                                    gint64               account_id,
                                                    const char          *conversation_token);

G_END_DECLS
