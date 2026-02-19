#pragma once

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define GNALK_TYPE_THREAD_DATA (gnalk_thread_data_get_type ())
G_DECLARE_FINAL_TYPE (GnalkThreadData, gnalk_thread_data, GNALK, THREAD_DATA, GObject)

GnalkThreadData *gnalk_thread_data_new_from_json (JsonObject *thread_info);

gboolean gnalk_thread_data_update_from_data (GnalkThreadData *self,
                                             GnalkThreadData *source);

gint64       gnalk_thread_data_get_thread_id              (GnalkThreadData *self);
const char  *gnalk_thread_data_get_room_token             (GnalkThreadData *self);
const char  *gnalk_thread_data_get_title                  (GnalkThreadData *self);
gint64       gnalk_thread_data_get_last_message_id        (GnalkThreadData *self);
gint64       gnalk_thread_data_get_last_activity          (GnalkThreadData *self);
gint         gnalk_thread_data_get_num_replies            (GnalkThreadData *self);
gint         gnalk_thread_data_get_notification_level     (GnalkThreadData *self);
gboolean     gnalk_thread_data_get_has_unread             (GnalkThreadData *self);
void         gnalk_thread_data_set_has_unread             (GnalkThreadData *self,
                                                           gboolean         has_unread);
void         gnalk_thread_data_update_last_message         (GnalkThreadData *self,
                                                           gint64           message_id,
                                                           const char      *message_text,
                                                           const char      *actor_name);
const char  *gnalk_thread_data_get_last_message_text      (GnalkThreadData *self);
const char  *gnalk_thread_data_get_last_message_actor_name(GnalkThreadData *self);

G_END_DECLS
