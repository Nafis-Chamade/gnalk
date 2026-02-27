#pragma once

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef enum {
  GNALK_MESSAGE_DATA_STATUS_NONE,
  GNALK_MESSAGE_DATA_STATUS_SENDING,
  GNALK_MESSAGE_DATA_STATUS_SENT,
  GNALK_MESSAGE_DATA_STATUS_READ,
  GNALK_MESSAGE_DATA_STATUS_FAILED
} GnalkMessageDataStatus;

#define GNALK_TYPE_MESSAGE_DATA (gnalk_message_data_get_type ())
G_DECLARE_FINAL_TYPE (GnalkMessageData, gnalk_message_data, GNALK, MESSAGE_DATA, GObject)

GnalkMessageData *gnalk_message_data_new_from_json (JsonObject *message_data,
                                                       const char *current_user_id);

GnalkMessageData *gnalk_message_data_new_pending (const char *message_text,
                                                     const char *author_name,
                                                     const char *author_id,
                                                     const char *reference_id,
                                                     gint64      reply_to,
                                                     gint64      thread_id);

/* Create pending message for file upload */
GnalkMessageData *gnalk_message_data_new_pending_file (const char *caption,
                                                          const char *author_name,
                                                          const char *author_id,
                                                          const char *reference_id,
                                                          gint64      reply_to,
                                                          const char *file_name,
                                                          const char *file_path,
                                                          const char *mime_type);

/* Create from database row */
GnalkMessageData *gnalk_message_data_new_from_db (gint64                   id,
                                                     gint64                   timestamp,
                                                     gint64                   reply_to,
                                                     gint64                   last_edit_timestamp,
                                                     const char              *message,
                                                     const char              *author_id,
                                                     const char              *author_name,
                                                     const char              *reference_id,
                                                     const char              *system_message,
                                                     gboolean                 is_own,
                                                     GnalkMessageDataStatus  status,
                                                     const char              *reactions_json,
                                                     const char              *parent_json,
                                                     const char              *message_parameters_json,
                                                     const char              *actor_type,
                                                     gint64                   thread_id,
                                                     gboolean                 is_thread,
                                                     gint                     thread_replies,
                                                     const char              *thread_title);

/* Serialization for database storage */
char *gnalk_message_data_serialize_reactions   (GnalkMessageData *self);
char *gnalk_message_data_serialize_parent      (GnalkMessageData *self);
char *gnalk_message_data_serialize_parameters  (GnalkMessageData *self);

/* Getters */
gint64       gnalk_message_data_get_id           (GnalkMessageData *self);
gint64       gnalk_message_data_get_timestamp    (GnalkMessageData *self);
const char  *gnalk_message_data_get_message      (GnalkMessageData *self);
const char  *gnalk_message_data_get_author_id    (GnalkMessageData *self);
const char  *gnalk_message_data_get_author_name  (GnalkMessageData *self);
const char  *gnalk_message_data_get_reference_id (GnalkMessageData *self);
gboolean     gnalk_message_data_get_is_own       (GnalkMessageData *self);
gint64       gnalk_message_data_get_reply_to     (GnalkMessageData *self);
const char  *gnalk_message_data_get_system_message (GnalkMessageData *self);
JsonObject  *gnalk_message_data_get_reactions    (GnalkMessageData *self);
JsonObject  *gnalk_message_data_get_parent       (GnalkMessageData *self);
JsonObject  *gnalk_message_data_get_message_parameters (GnalkMessageData *self);
void         gnalk_message_data_set_message_parameters (GnalkMessageData *self,
                                                          JsonObject        *params);
gboolean     gnalk_message_data_get_render_markdown (GnalkMessageData *self);
const char  *gnalk_message_data_get_actor_type     (GnalkMessageData *self);
gboolean     gnalk_message_data_is_bot             (GnalkMessageData *self);
gboolean     gnalk_message_data_is_federated       (GnalkMessageData *self);

GnalkMessageDataStatus gnalk_message_data_get_status (GnalkMessageData *self);

/* Date helpers */
void gnalk_message_data_get_date (GnalkMessageData *self,
                                   int               *year,
                                   int               *month,
                                   int               *day);

/* Setters for mutable state */
void gnalk_message_data_set_status       (GnalkMessageData       *self,
                                           GnalkMessageDataStatus  status);
void gnalk_message_data_set_is_own       (GnalkMessageData *self,
                                           gboolean           is_own);
void gnalk_message_data_confirm_sent     (GnalkMessageData *self,
                                           gint64             message_id,
                                           gint64             timestamp);
void gnalk_message_data_add_reaction     (GnalkMessageData *self,
                                           const char        *emoji);
void gnalk_message_data_set_show_date_header (GnalkMessageData *self,
                                               gboolean           show);
gboolean gnalk_message_data_get_show_date_header (GnalkMessageData *self);
void gnalk_message_data_set_show_timestamp (GnalkMessageData *self,
                                             gboolean           show);
gboolean gnalk_message_data_get_show_timestamp (GnalkMessageData *self);
void gnalk_message_data_set_show_avatar (GnalkMessageData *self,
                                           gboolean           show);
gboolean gnalk_message_data_get_show_avatar (GnalkMessageData *self);
void gnalk_message_data_set_parent       (GnalkMessageData *self,
                                           JsonObject        *parent);

/* Message editing support */
gint64       gnalk_message_data_get_last_edit_timestamp (GnalkMessageData *self);
void         gnalk_message_data_set_message             (GnalkMessageData *self,
                                                          const char        *message);
void         gnalk_message_data_set_last_edit_timestamp (GnalkMessageData *self,
                                                          gint64             timestamp);
void         gnalk_message_data_set_system_message      (GnalkMessageData *self,
                                                          const char        *system_message);

/* Unread message support */
gboolean     gnalk_message_data_get_is_unread       (GnalkMessageData *self);
void         gnalk_message_data_set_is_unread       (GnalkMessageData *self,
                                                       gboolean           is_unread);

/* File upload support */
gboolean     gnalk_message_data_get_is_uploading     (GnalkMessageData *self);
void         gnalk_message_data_set_is_uploading     (GnalkMessageData *self,
                                                        gboolean           uploading);
double       gnalk_message_data_get_upload_progress  (GnalkMessageData *self);
void         gnalk_message_data_set_upload_progress  (GnalkMessageData *self,
                                                        double             progress);
const char  *gnalk_message_data_get_upload_file_name (GnalkMessageData *self);
const char  *gnalk_message_data_get_upload_file_path (GnalkMessageData *self);
const char  *gnalk_message_data_get_upload_mime_type (GnalkMessageData *self);

/* File cache state */
gboolean     gnalk_message_data_get_file_cached     (GnalkMessageData *self);
void         gnalk_message_data_set_file_cached     (GnalkMessageData *self,
                                                        gboolean           cached);

/* Attachment group support (pre-computed by MessageProcessor) */
gboolean     gnalk_message_data_is_file_only        (GnalkMessageData *self);
gboolean     gnalk_message_data_has_file             (GnalkMessageData *self);
gboolean     gnalk_message_data_get_is_attachment_group_leader (GnalkMessageData *self);
void         gnalk_message_data_set_is_attachment_group_leader (GnalkMessageData *self,
                                                                   gboolean           leader);
gboolean     gnalk_message_data_get_is_attachment_group_hidden (GnalkMessageData *self);
void         gnalk_message_data_set_is_attachment_group_hidden (GnalkMessageData *self,
                                                                   gboolean           hidden);
GPtrArray   *gnalk_message_data_get_attachment_group_files     (GnalkMessageData *self);
void         gnalk_message_data_set_attachment_group_files     (GnalkMessageData *self,
                                                                   GPtrArray         *files);

/* Thread support */
gint64       gnalk_message_data_get_thread_id       (GnalkMessageData *self);
gboolean     gnalk_message_data_get_is_thread       (GnalkMessageData *self);
gint         gnalk_message_data_get_thread_replies   (GnalkMessageData *self);
void         gnalk_message_data_set_thread_replies   (GnalkMessageData *self,
                                                        gint               replies);
const char  *gnalk_message_data_get_thread_title    (GnalkMessageData *self);
void         gnalk_message_data_set_thread_title    (GnalkMessageData *self,
                                                        const char        *title);
const char  *gnalk_message_data_get_thread_first_message (GnalkMessageData *self);
void         gnalk_message_data_set_thread_first_message (GnalkMessageData *self,
                                                             const char        *message);
void         gnalk_message_data_mark_as_thread      (GnalkMessageData *self,
                                                        gint64             thread_id);

G_END_DECLS
