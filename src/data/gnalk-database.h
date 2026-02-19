#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "gnalk-conversation-data.h"
#include "gnalk-message-data.h"

G_BEGIN_DECLS

/* Forward declaration to avoid circular dependency */
typedef struct _GnalkAccount GnalkAccount;

#define GNALK_TYPE_DATABASE (gnalk_database_get_type ())
G_DECLARE_FINAL_TYPE (GnalkDatabase, gnalk_database, GNALK, DATABASE, GObject)

/* Lifecycle */
GnalkDatabase *gnalk_database_new (void);
gboolean gnalk_database_open (GnalkDatabase *self, GError **error);
void gnalk_database_close (GnalkDatabase *self);

/* Account management - legacy (for backward compat during migration) */
gint64 gnalk_database_get_or_create_account (GnalkDatabase *self,
                                               const char *server_url,
                                               const char *user_id);
void gnalk_database_update_account_user_name (GnalkDatabase *self,
                                                gint64 account_id,
                                                const char *user_name);

/* Account management - new multi-account API */
GListStore *gnalk_database_load_all_accounts (GnalkDatabase *self);
gint64 gnalk_database_create_account (GnalkDatabase *self,
                                         const char *server_url,
                                         const char *user_id);
void gnalk_database_save_account (GnalkDatabase *self,
                                    GnalkAccount *account);
void gnalk_database_delete_account (GnalkDatabase *self,
                                       gint64 account_id);

/* Conversations */
void gnalk_database_save_conversation (GnalkDatabase *self,
                                         gint64 account_id,
                                         GnalkConversationData *data);
void gnalk_database_delete_conversation (GnalkDatabase *self,
                                           gint64 account_id,
                                           const char *token);
int gnalk_database_delete_stale_conversations (GnalkDatabase *self,
                                                  gint64 account_id,
                                                  GPtrArray *valid_tokens);
char *gnalk_database_find_conversation_token (GnalkDatabase *self,
                                               gint64 account_id,
                                               const char *name,
                                               gint room_type);
GListStore *gnalk_database_load_conversations (GnalkDatabase *self,
                                                 gint64 account_id);
void gnalk_database_update_conversation_last_message (GnalkDatabase *self,
                                                         gint64 account_id,
                                                         const char *token,
                                                         gint64 last_activity,
                                                         gint64 last_message_id,
                                                         const char *last_message_text,
                                                         const char *last_message_actor_id,
                                                         const char *last_message_actor_name,
                                                         const char *last_message_type,
                                                         const char *last_message_params_json);
void gnalk_database_update_conversation_read_status (GnalkDatabase *self,
                                                        gint64 account_id,
                                                        const char *token,
                                                        gint64 last_read_message);
void gnalk_database_update_conversation_favorite (GnalkDatabase *self,
                                                     gint64 account_id,
                                                     const char *token,
                                                     gboolean is_favorite);

/* Messages */
void gnalk_database_save_message (GnalkDatabase *self,
                                    gint64 account_id,
                                    const char *conversation_token,
                                    GnalkMessageData *message);
void gnalk_database_update_message (GnalkDatabase *self,
                                      gint64 message_id,
                                      const char *message_text,
                                      gint64 last_edit_timestamp);
void gnalk_database_update_message_reactions (GnalkDatabase *self,
                                                gint64 message_id,
                                                const char *reactions_json);
void gnalk_database_update_message_status (GnalkDatabase *self,
                                             gint64 message_id,
                                             GnalkMessageDataStatus status);
GListStore *gnalk_database_load_messages (GnalkDatabase *self,
                                            gint64 account_id,
                                            const char *conversation_token,
                                            gint limit,
                                            gint64 before_id);
void gnalk_database_trim_messages (GnalkDatabase *self,
                                     gint64 account_id,
                                     const char *conversation_token,
                                     gint max_count);

/* Sync state */
gint64 gnalk_database_get_last_known_message_id (GnalkDatabase *self,
                                                   gint64 account_id,
                                                   const char *conversation_token);
void gnalk_database_set_last_known_message_id (GnalkDatabase *self,
                                                 gint64 account_id,
                                                 const char *conversation_token,
                                                 gint64 message_id);
gint64 gnalk_database_get_last_notified_message_id (GnalkDatabase *self,
                                                      gint64 account_id,
                                                      const char *conversation_token);
void gnalk_database_set_last_notified_message_id (GnalkDatabase *self,
                                                    gint64 account_id,
                                                    const char *conversation_token,
                                                    gint64 message_id);
gboolean gnalk_database_has_more_history (GnalkDatabase *self,
                                            gint64 account_id,
                                            const char *conversation_token);
void gnalk_database_set_has_more_history (GnalkDatabase *self,
                                            gint64 account_id,
                                            const char *conversation_token,
                                            gboolean has_more);

/* Pending messages queue */
typedef struct {
  gint64  id;
  char   *conversation_token;
  char   *message;
  gint64  reply_to;
  char   *reference_id;
  char   *file_path;
  char   *file_name;
  char   *content_type;
  gint64  thread_id;
  gint    retry_count;
  char   *last_error;
} GnalkPendingMessage;

void gnalk_pending_message_free (GnalkPendingMessage *pm);

void gnalk_database_queue_pending_message (GnalkDatabase *self,
                                             gint64 account_id,
                                             const char *conversation_token,
                                             const char *message,
                                             gint64 reply_to,
                                             const char *reference_id,
                                             gint64 thread_id);
void gnalk_database_queue_pending_file (GnalkDatabase *self,
                                          gint64 account_id,
                                          const char *conversation_token,
                                          const char *file_path,
                                          const char *file_name,
                                          const char *content_type,
                                          const char *caption,
                                          gint64 reply_to,
                                          const char *reference_id);
GPtrArray *gnalk_database_get_pending_messages (GnalkDatabase *self,
                                                  gint64 account_id);
void gnalk_database_remove_pending_message (GnalkDatabase *self,
                                              const char *reference_id);
void gnalk_database_update_pending_error (GnalkDatabase *self,
                                            const char *reference_id,
                                            const char *error);

/* Participants */
void gnalk_database_save_participants (GnalkDatabase *self,
                                         gint64 account_id,
                                         const char *conversation_token,
                                         JsonArray *participants);
JsonArray *gnalk_database_load_participants (GnalkDatabase *self,
                                               gint64 account_id,
                                               const char *conversation_token);
void gnalk_database_delete_participants (GnalkDatabase *self,
                                           gint64 account_id,
                                           const char *conversation_token);
void gnalk_database_reset_participants_status (GnalkDatabase *self,
                                                 gint64 account_id);
gboolean gnalk_database_update_participant_call_state (GnalkDatabase *self,
                                                          gint64          account_id,
                                                          const char     *conversation_token,
                                                          const char     *actor_id,
                                                          gint64          in_call);
void gnalk_database_clear_room_call_states (GnalkDatabase *self,
                                               gint64          account_id,
                                               const char     *conversation_token);

G_END_DECLS
