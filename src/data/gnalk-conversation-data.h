#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

/* Forward declarations */
typedef struct _GnalkThreadData GnalkThreadData;

G_BEGIN_DECLS

/* Forward declaration to avoid circular dependency */
typedef struct _GnalkSession GnalkSession;

#define GNALK_TYPE_CONVERSATION_DATA (gnalk_conversation_data_get_type ())
G_DECLARE_FINAL_TYPE (GnalkConversationData, gnalk_conversation_data, GNALK, CONVERSATION_DATA, GObject)

GnalkConversationData *gnalk_conversation_data_new_from_json (JsonObject    *room_data,
                                                                  GnalkSession *session);
GnalkConversationData *gnalk_conversation_data_new_from_json_with_user (JsonObject *room_data,
                                                                           const char *current_user_id);
typedef struct {
  const char   *token;
  const char   *title;
  const char   *name;
  const char   *user_status;
  const char   *actor_id;
  gint          room_type;
  gint64        last_activity;
  gint64        last_common_read_message;
  gint          unread_messages;
  gboolean      is_favorite;
  gboolean      can_leave;
  gboolean      can_delete;
  const char   *description;
  gint          participant_type;
  gint          notification_level;
  gint          call_notification_level;
  gint64        last_read_message;
  gint64        last_message_id;
  const char   *last_message_text;
  const char   *last_message_actor_id;
  const char   *last_message_actor_name;
  const char   *last_message_type;
  const char   *last_message_params_json;
  const char   *remote_server;
  const char   *remote_token;
  gint64        invitation_id;
  GnalkSession *session;
} GnalkConversationDataInit;

GnalkConversationData *gnalk_conversation_data_new_from_db (const GnalkConversationDataInit *init);
GnalkConversationData *gnalk_conversation_data_new_section_header (const char *title, gboolean is_favorite_section);
GnalkConversationData *gnalk_conversation_data_new_invitation_header (void);

gboolean gnalk_conversation_data_is_section_header (GnalkConversationData *self);
const char *gnalk_conversation_data_get_token (GnalkConversationData *self);
const char *gnalk_conversation_data_get_title (GnalkConversationData *self);
const char *gnalk_conversation_data_get_name (GnalkConversationData *self);
char *gnalk_conversation_data_get_subtitle (GnalkConversationData *self,
                                              const char             *current_user_id);
const char *gnalk_conversation_data_get_description (GnalkConversationData *self);
const char *gnalk_conversation_data_get_user_status (GnalkConversationData *self);
const char *gnalk_conversation_data_get_actor_id (GnalkConversationData *self);
gint gnalk_conversation_data_get_room_type (GnalkConversationData *self);
gint64 gnalk_conversation_data_get_last_activity (GnalkConversationData *self);
gint64 gnalk_conversation_data_get_last_common_read_message (GnalkConversationData *self);
gint gnalk_conversation_data_get_unread_messages (GnalkConversationData *self);
gboolean gnalk_conversation_data_get_is_favorite (GnalkConversationData *self);
gboolean gnalk_conversation_data_get_can_leave (GnalkConversationData *self);
gboolean gnalk_conversation_data_get_can_delete (GnalkConversationData *self);
gint64 gnalk_conversation_data_get_last_read_message (GnalkConversationData *self);
gint64 gnalk_conversation_data_get_last_message_id (GnalkConversationData *self);
gboolean gnalk_conversation_data_get_last_message_is_own (GnalkConversationData *self,
                                                            const char             *current_user_id);
gint gnalk_conversation_data_get_participant_type (GnalkConversationData *self);
gint gnalk_conversation_data_get_notification_level (GnalkConversationData *self);
gint gnalk_conversation_data_get_call_notification_level (GnalkConversationData *self);
gboolean gnalk_conversation_data_can_moderate (GnalkConversationData *self);
const char *gnalk_conversation_data_get_remote_server (GnalkConversationData *self);
const char *gnalk_conversation_data_get_remote_token (GnalkConversationData *self);
gboolean gnalk_conversation_data_is_federated (GnalkConversationData *self);
gint64 gnalk_conversation_data_get_invitation_id (GnalkConversationData *self);
gboolean gnalk_conversation_data_is_invitation (GnalkConversationData *self);

/* Raw last message data getters (for database storage) */
const char *gnalk_conversation_data_get_last_message_text (GnalkConversationData *self);
const char *gnalk_conversation_data_get_last_message_actor_id (GnalkConversationData *self);
const char *gnalk_conversation_data_get_last_message_actor_name (GnalkConversationData *self);
const char *gnalk_conversation_data_get_last_message_type (GnalkConversationData *self);
char *gnalk_conversation_data_get_last_message_params_json (GnalkConversationData *self);

/* Session that owns this conversation (for multi-account support).
 * get_session returns a strong reference (transfer full) or NULL — unref it. */
GnalkSession *gnalk_conversation_data_get_session (GnalkConversationData *self);
void           gnalk_conversation_data_set_session (GnalkConversationData *self,
                                                      GnalkSession          *session);

/* Thread sub-list for conversation sidebar tree */
GListStore *gnalk_conversation_data_get_threads_store      (GnalkConversationData *self);
void         gnalk_conversation_data_set_threads            (GnalkConversationData *self,
                                                             GPtrArray             *thread_datas);
gboolean     gnalk_conversation_data_get_has_unread_threads (GnalkConversationData *self);
void         gnalk_conversation_data_mark_thread_read      (GnalkConversationData *self,
                                                             gint64                 thread_id);

/* Set title in-place (emits "updated" signal if changed) */
void gnalk_conversation_data_set_title (GnalkConversationData *self,
                                          const char             *title);

/* Set description in-place (emits "updated" signal if changed) */
void gnalk_conversation_data_set_description (GnalkConversationData *self,
                                                 const char             *description);

/* Update conversation data in-place from JSON (emits "updated" signal) */
void gnalk_conversation_data_update_from_json (GnalkConversationData *self,
                                                 JsonObject             *room_data);

/* Update last message data in-place (emits "updated" signal) */
void gnalk_conversation_data_update_last_message (GnalkConversationData *self,
                                                    gint64                  last_activity,
                                                    gint64                  last_message_id,
                                                    const char             *last_message_text,
                                                    const char             *last_message_actor_id,
                                                    const char             *last_message_actor_name,
                                                    const char             *last_message_type,
                                                    const char             *last_message_params_json);

/* Update conversation data in-place from another ConversationData.
 * Only emits "updated" signal if something actually changed.
 * Returns TRUE if any field was modified. */
gboolean gnalk_conversation_data_update_from_data (GnalkConversationData *self,
                                                   GnalkConversationData *source);

G_END_DECLS
