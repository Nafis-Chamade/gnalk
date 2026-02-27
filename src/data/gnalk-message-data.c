#include "gnalk-message-data.h"
#include "gnalk-message-utils.h"

struct _GnalkMessageData
{
  GObject parent_instance;

  gint64  id;
  gint64  timestamp;
  gint64  reply_to;
  gint64  last_edit_timestamp;
  char   *message;
  char   *author_id;
  char   *author_name;
  char   *reference_id;
  char   *system_message;    /* Interned via g_intern_string — do not g_free */
  char   *actor_type;        /* Interned via g_intern_string — do not g_free */
  gboolean is_own;
  gboolean show_date_header;
  gboolean show_timestamp;
  gboolean show_avatar;     /* First message of a group (for avatar grouping) */
  GnalkMessageDataStatus status;

  JsonObject *reactions;
  JsonObject *parent;  /* For quoted message */
  JsonObject *message_parameters;

  /* Unread message state */
  gboolean is_unread;

  /* Markdown rendering flag */
  gboolean render_markdown;

  /* File upload support */
  gboolean is_uploading;
  double   upload_progress;
  char    *upload_file_name;
  char    *upload_file_path;
  char    *upload_mime_type;

  /* File cache state (set when file is downloaded to local cache) */
  gboolean file_cached;

  /* Thread support */
  gint64   thread_id;       /* 0 = not in a thread */
  gboolean is_thread;       /* TRUE = thread root message */
  gint     thread_replies;  /* reply count (meaningful when is_thread) */
  char    *thread_title;    /* Server-provided thread title (when is_thread) */
  char    *thread_first_message; /* First message text (on thread_created system msgs) */

  /* Attachment group support (pre-computed by MessageProcessor) */
  gboolean    is_attachment_group_leader;   /* First message of a file group */
  gboolean    is_attachment_group_hidden;   /* Non-leader member (hidden in list) */
  GPtrArray  *attachment_group_files;       /* GnalkMessageData* (unowned) — leader only */
};

G_DEFINE_FINAL_TYPE (GnalkMessageData, gnalk_message_data, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_IS_UNREAD,
  PROP_MESSAGE,
  PROP_STATUS,
  PROP_REACTIONS,
  PROP_LAST_EDIT_TS,
  PROP_IS_UPLOADING,
  PROP_UPLOAD_PROGRESS,
  PROP_FILE_CACHED,
  PROP_THREAD_REPLIES,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
gnalk_message_data_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  GnalkMessageData *self = GNALK_MESSAGE_DATA (object);

  switch (prop_id)
    {
    case PROP_IS_UNREAD:
      g_value_set_boolean (value, self->is_unread);
      break;
    case PROP_MESSAGE:
      g_value_set_string (value, self->message);
      break;
    case PROP_STATUS:
      g_value_set_int (value, (gint) self->status);
      break;
    case PROP_REACTIONS:
      g_value_set_boxed (value, self->reactions);
      break;
    case PROP_LAST_EDIT_TS:
      g_value_set_int64 (value, self->last_edit_timestamp);
      break;
    case PROP_IS_UPLOADING:
      g_value_set_boolean (value, self->is_uploading);
      break;
    case PROP_UPLOAD_PROGRESS:
      g_value_set_double (value, self->upload_progress);
      break;
    case PROP_FILE_CACHED:
      g_value_set_boolean (value, self->file_cached);
      break;
    case PROP_THREAD_REPLIES:
      g_value_set_int (value, self->thread_replies);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gnalk_message_data_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  GnalkMessageData *self = GNALK_MESSAGE_DATA (object);

  switch (prop_id)
    {
    case PROP_IS_UNREAD:
      gnalk_message_data_set_is_unread (self, g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gnalk_message_data_finalize (GObject *object)
{
  GnalkMessageData *self = GNALK_MESSAGE_DATA (object);

  g_free (self->message);
  g_free (self->author_id);
  g_free (self->author_name);
  g_free (self->reference_id);
  /* system_message and actor_type are interned — do not free */
  g_free (self->upload_file_name);
  g_free (self->upload_file_path);
  g_free (self->upload_mime_type);
  g_free (self->thread_title);
  g_free (self->thread_first_message);

  if (self->reactions)
    json_object_unref (self->reactions);
  if (self->parent)
    json_object_unref (self->parent);
  if (self->message_parameters)
    json_object_unref (self->message_parameters);

  g_clear_pointer (&self->attachment_group_files, g_ptr_array_unref);

  G_OBJECT_CLASS (gnalk_message_data_parent_class)->finalize (object);
}

static void
gnalk_message_data_class_init (GnalkMessageDataClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_message_data_finalize;
  object_class->get_property = gnalk_message_data_get_property;
  object_class->set_property = gnalk_message_data_set_property;

  properties[PROP_IS_UNREAD] =
    g_param_spec_boolean ("is-unread", NULL, NULL, FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_MESSAGE] =
    g_param_spec_string ("message", NULL, NULL, NULL,
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_STATUS] =
    g_param_spec_int ("status", NULL, NULL,
                      0, G_MAXINT, 0,
                      G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_REACTIONS] =
    g_param_spec_boxed ("reactions", NULL, NULL,
                        JSON_TYPE_OBJECT,
                        G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_LAST_EDIT_TS] =
    g_param_spec_int64 ("last-edit-timestamp", NULL, NULL,
                        0, G_MAXINT64, 0,
                        G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_IS_UPLOADING] =
    g_param_spec_boolean ("is-uploading", NULL, NULL, FALSE,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_UPLOAD_PROGRESS] =
    g_param_spec_double ("upload-progress", NULL, NULL,
                         0.0, 1.0, 0.0,
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_FILE_CACHED] =
    g_param_spec_boolean ("file-cached", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_THREAD_REPLIES] =
    g_param_spec_int ("thread-replies", NULL, NULL,
                      0, G_MAXINT, 0,
                      G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
gnalk_message_data_init (GnalkMessageData *self)
{
  self->status = GNALK_MESSAGE_DATA_STATUS_NONE;
  self->show_date_header = FALSE;
  self->show_timestamp = TRUE;
  self->render_markdown = TRUE;
}

GnalkMessageData *
gnalk_message_data_new_from_json (JsonObject *message_data,
                                    const char *current_user_id)
{
  GnalkMessageData *self = g_object_new (GNALK_TYPE_MESSAGE_DATA, NULL);

  self->id = json_object_get_int_member_with_default (message_data, "id", 0);
  self->timestamp = json_object_get_int_member_with_default (message_data, "timestamp", 0);
  self->last_edit_timestamp = json_object_get_int_member_with_default (message_data, "lastEditTimestamp", 0);
  self->message = g_strdup (json_object_get_string_member_with_default (message_data, "message", ""));
  self->author_id = g_strdup (json_object_get_string_member_with_default (message_data, "actorId", ""));
  self->author_name = g_strdup (json_object_get_string_member_with_default (message_data, "actorDisplayName", ""));

  const char *ref = json_object_get_string_member_with_default (message_data, "referenceId", "");
  self->reference_id = (ref && *ref) ? g_strdup (ref) : NULL;

  self->system_message = (char *) g_intern_string (json_object_get_string_member_with_default (message_data, "systemMessage", ""));
  self->actor_type = (char *) g_intern_string (json_object_get_string_member_with_default (message_data, "actorType", "users"));

  self->is_own = current_user_id && g_strcmp0 (self->author_id, current_user_id) == 0;

  /* Set status for own messages */
  if (self->is_own && self->id > 0)
    self->status = GNALK_MESSAGE_DATA_STATUS_SENT;

  /* Copy reactions if present and is an object (not empty array) */
  if (json_object_has_member (message_data, "reactions"))
    {
      JsonNode *node = json_object_get_member (message_data, "reactions");
      if (node && JSON_NODE_HOLDS_OBJECT (node))
        {
          JsonObject *reactions = json_node_get_object (node);
          if (reactions)
            self->reactions = json_object_ref (reactions);
        }
    }

  /* Copy parent (quoted message) if present and is an object.
   * Skip when parent.id == threadId for thread-sidebar replies (isThread=true)
   * — those always quote the root which is noise.  Regular replies (isThread
   * absent/false) keep their parent even if it matches threadId. */
  gint64 json_thread_id = json_object_get_int_member_with_default (message_data, "threadId", 0);
  gboolean is_thread_reply = json_object_get_boolean_member_with_default (message_data, "isThread", FALSE);
  if (json_object_has_member (message_data, "parent"))
    {
      JsonNode *node = json_object_get_member (message_data, "parent");
      if (node && JSON_NODE_HOLDS_OBJECT (node))
        {
          JsonObject *parent = json_node_get_object (node);
          if (parent)
            {
              gint64 parent_id = json_object_get_int_member_with_default (parent, "id", 0);
              if (!is_thread_reply || json_thread_id == 0 || parent_id != json_thread_id)
                self->parent = json_object_ref (parent);
            }
        }
    }

  /* Parse markdown flag - default TRUE, but FALSE for system messages */
  self->render_markdown = json_object_get_boolean_member_with_default (message_data, "markdown", TRUE);
  if (self->system_message && *self->system_message)
    self->render_markdown = FALSE;

  /* Copy messageParameters for system message parsing (may be [] instead of {}) */
  if (json_object_has_member (message_data, "messageParameters"))
    {
      JsonNode *node = json_object_get_member (message_data, "messageParameters");
      if (node && JSON_NODE_HOLDS_OBJECT (node))
        {
          JsonObject *params = json_node_get_object (node);
          if (params)
            self->message_parameters = json_object_ref (params);
        }
    }

  /* Thread support */
  self->thread_id = json_object_get_int_member_with_default (message_data, "threadId", 0);
  self->is_thread = json_object_get_boolean_member_with_default (message_data, "isThread", FALSE);
  self->thread_replies = (gint) json_object_get_int_member_with_default (message_data, "threadReplies", 0);
  const char *title = json_object_get_string_member_with_default (message_data, "threadTitle", "");
  self->thread_title = (title && *title) ? g_strdup (title) : NULL;

  return self;
}

GnalkMessageData *
gnalk_message_data_new_pending (const char *message_text,
                                  const char *author_name,
                                  const char *author_id,
                                  const char *reference_id,
                                  gint64      reply_to,
                                  gint64      thread_id)
{
  GnalkMessageData *self = g_object_new (GNALK_TYPE_MESSAGE_DATA, NULL);

  self->id = 0;  /* Will be set when server confirms */
  self->timestamp = g_get_real_time () / G_USEC_PER_SEC;
  self->message = g_strdup (message_text ? message_text : "");
  self->author_id = g_strdup (author_id ? author_id : "");
  self->author_name = g_strdup (author_name ? author_name : "");
  self->reference_id = (reference_id && *reference_id) ? g_strdup (reference_id) : NULL;
  self->reply_to = reply_to;
  self->thread_id = thread_id;
  self->is_own = TRUE;
  self->status = GNALK_MESSAGE_DATA_STATUS_SENDING;

  return self;
}

gint64
gnalk_message_data_get_id (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), 0);
  return self->id;
}

gint64
gnalk_message_data_get_timestamp (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), 0);
  return self->timestamp;
}

const char *
gnalk_message_data_get_message (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return self->message;
}

const char *
gnalk_message_data_get_author_id (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return self->author_id;
}

const char *
gnalk_message_data_get_author_name (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return self->author_name;
}

const char *
gnalk_message_data_get_reference_id (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return self->reference_id;
}

gboolean
gnalk_message_data_get_is_own (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), FALSE);
  return self->is_own;
}

gint64
gnalk_message_data_get_reply_to (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), 0);
  return self->reply_to;
}

const char *
gnalk_message_data_get_system_message (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return self->system_message;
}

JsonObject *
gnalk_message_data_get_reactions (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return self->reactions;
}

JsonObject *
gnalk_message_data_get_parent (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return self->parent;
}

JsonObject *
gnalk_message_data_get_message_parameters (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return self->message_parameters;
}

gboolean
gnalk_message_data_get_render_markdown (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), FALSE);
  return self->render_markdown;
}

const char *
gnalk_message_data_get_actor_type (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), "users");
  return self->actor_type;
}

gboolean
gnalk_message_data_is_bot (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), FALSE);
  return self->actor_type && g_strcmp0 (self->actor_type, "bots") == 0;
}

gboolean
gnalk_message_data_is_federated (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), FALSE);
  return self->actor_type && g_strcmp0 (self->actor_type, "federated_users") == 0;
}

void
gnalk_message_data_set_message_parameters (GnalkMessageData *self,
                                             JsonObject        *params)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));

  if (self->message_parameters)
    json_object_unref (self->message_parameters);

  self->message_parameters = params ? json_object_ref (params) : NULL;
}

GnalkMessageDataStatus
gnalk_message_data_get_status (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), GNALK_MESSAGE_DATA_STATUS_NONE);
  return self->status;
}

void
gnalk_message_data_get_date (GnalkMessageData *self,
                               int               *year,
                               int               *month,
                               int               *day)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));

  if (self->timestamp <= 0)
    {
      if (year) *year = 0;
      if (month) *month = 0;
      if (day) *day = 0;
      return;
    }

  g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local (self->timestamp);
  if (dt)
    {
      if (year) *year = g_date_time_get_year (dt);
      if (month) *month = g_date_time_get_month (dt);
      if (day) *day = g_date_time_get_day_of_month (dt);
    }
  else
    {
      if (year) *year = 0;
      if (month) *month = 0;
      if (day) *day = 0;
    }
}

void
gnalk_message_data_set_status (GnalkMessageData       *self,
                                 GnalkMessageDataStatus  status)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  if (self->status == status)
    return;
  self->status = status;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_STATUS]);
}

void
gnalk_message_data_set_is_own (GnalkMessageData *self,
                                 gboolean           is_own)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  self->is_own = is_own;
}

void
gnalk_message_data_confirm_sent (GnalkMessageData *self,
                                   gint64             message_id,
                                   gint64             timestamp)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));

  g_object_freeze_notify (G_OBJECT (self));
  self->id = message_id;
  if (timestamp > 0)
    self->timestamp = timestamp;
  if (self->status != GNALK_MESSAGE_DATA_STATUS_SENT)
    {
      self->status = GNALK_MESSAGE_DATA_STATUS_SENT;
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_STATUS]);
    }
  g_object_thaw_notify (G_OBJECT (self));
}

void
gnalk_message_data_add_reaction (GnalkMessageData *self,
                                   const char        *emoji)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));

  if (!emoji || !*emoji)
    return;

  if (!self->reactions)
    self->reactions = json_object_new ();

  /* Increment reaction count or add new reaction */
  if (json_object_has_member (self->reactions, emoji))
    {
      gint64 count = json_object_get_int_member (self->reactions, emoji);
      json_object_set_int_member (self->reactions, emoji, count + 1);
    }
  else
    {
      json_object_set_int_member (self->reactions, emoji, 1);
    }

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_REACTIONS]);
}

void
gnalk_message_data_set_show_date_header (GnalkMessageData *self,
                                           gboolean           show)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  self->show_date_header = show;
}

gboolean
gnalk_message_data_get_show_date_header (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), FALSE);
  return self->show_date_header;
}

void
gnalk_message_data_set_show_timestamp (GnalkMessageData *self,
                                         gboolean           show)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  self->show_timestamp = show;
}

gboolean
gnalk_message_data_get_show_timestamp (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), TRUE);
  return self->show_timestamp;
}

void
gnalk_message_data_set_show_avatar (GnalkMessageData *self,
                                      gboolean           show)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  self->show_avatar = show;
}

gboolean
gnalk_message_data_get_show_avatar (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), TRUE);
  return self->show_avatar;
}

void
gnalk_message_data_set_parent (GnalkMessageData *self,
                                 JsonObject        *parent)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));

  if (self->parent)
    json_object_unref (self->parent);
  self->parent = parent ? json_object_ref (parent) : NULL;
}

/* Message editing support */

gint64
gnalk_message_data_get_last_edit_timestamp (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), 0);
  return self->last_edit_timestamp;
}

void
gnalk_message_data_set_message (GnalkMessageData *self,
                                  const char        *message)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  if (g_strcmp0 (self->message, message) == 0)
    return;
  g_free (self->message);
  self->message = g_strdup (message ? message : "");
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_MESSAGE]);
}

void
gnalk_message_data_set_last_edit_timestamp (GnalkMessageData *self,
                                              gint64             timestamp)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  if (self->last_edit_timestamp == timestamp)
    return;
  self->last_edit_timestamp = timestamp;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_LAST_EDIT_TS]);
}

void
gnalk_message_data_set_system_message (GnalkMessageData *self,
                                         const char        *system_message)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));

  /* Interned — no g_free needed */
  self->system_message = (char *) g_intern_string (system_message ? system_message : "");
}

/* Unread message support */

gboolean
gnalk_message_data_get_is_unread (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), FALSE);
  return self->is_unread;
}

void
gnalk_message_data_set_is_unread (GnalkMessageData *self,
                                    gboolean           is_unread)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));

  if (self->is_unread == is_unread)
    return;

  self->is_unread = is_unread;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_IS_UNREAD]);
}

/* File upload support */

GnalkMessageData *
gnalk_message_data_new_pending_file (const char *caption,
                                       const char *author_name,
                                       const char *author_id,
                                       const char *reference_id,
                                       gint64      reply_to,
                                       const char *file_name,
                                       const char *file_path,
                                       const char *mime_type)
{
  GnalkMessageData *self = g_object_new (GNALK_TYPE_MESSAGE_DATA, NULL);

  self->id = 0;
  self->timestamp = g_get_real_time () / G_USEC_PER_SEC;
  /* Use caption if provided, otherwise NULL (filename shown by upload preview) */
  self->message = (caption && *caption) ? g_strdup (caption) : NULL;
  self->author_id = g_strdup (author_id ? author_id : "");
  self->author_name = g_strdup (author_name ? author_name : "");
  self->reference_id = (reference_id && *reference_id) ? g_strdup (reference_id) : NULL;
  self->reply_to = reply_to;
  self->is_own = TRUE;
  self->status = GNALK_MESSAGE_DATA_STATUS_SENDING;

  /* File upload fields */
  self->is_uploading = TRUE;
  self->upload_progress = 0.0;
  self->upload_file_name = g_strdup (file_name);
  self->upload_file_path = g_strdup (file_path);
  self->upload_mime_type = g_strdup (mime_type);

  return self;
}

gboolean
gnalk_message_data_get_is_uploading (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), FALSE);
  return self->is_uploading;
}

void
gnalk_message_data_set_is_uploading (GnalkMessageData *self,
                                       gboolean           uploading)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  if (self->is_uploading == uploading)
    return;
  self->is_uploading = uploading;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_IS_UPLOADING]);
}

double
gnalk_message_data_get_upload_progress (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), 0.0);
  return self->upload_progress;
}

void
gnalk_message_data_set_upload_progress (GnalkMessageData *self,
                                          double             progress)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  double clamped = CLAMP (progress, 0.0, 1.0);
  if (self->upload_progress == clamped)
    return;
  self->upload_progress = clamped;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_UPLOAD_PROGRESS]);
}

gboolean
gnalk_message_data_get_file_cached (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), FALSE);
  return self->file_cached;
}

void
gnalk_message_data_set_file_cached (GnalkMessageData *self,
                                       gboolean           cached)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  if (self->file_cached == cached)
    return;
  self->file_cached = cached;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_FILE_CACHED]);
}

gint64
gnalk_message_data_get_thread_id (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), 0);
  return self->thread_id;
}

gboolean
gnalk_message_data_get_is_thread (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), FALSE);
  return self->is_thread;
}

gint
gnalk_message_data_get_thread_replies (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), 0);
  return self->thread_replies;
}

void
gnalk_message_data_set_thread_replies (GnalkMessageData *self,
                                          gint               replies)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  if (self->thread_replies == replies)
    return;
  self->thread_replies = replies;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_THREAD_REPLIES]);
}

const char *
gnalk_message_data_get_thread_title (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return self->thread_title;
}

void
gnalk_message_data_set_thread_title (GnalkMessageData *self,
                                     const char        *title)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  g_free (self->thread_title);
  self->thread_title = (title && *title) ? g_strdup (title) : NULL;
}

const char *
gnalk_message_data_get_thread_first_message (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return self->thread_first_message;
}

void
gnalk_message_data_set_thread_first_message (GnalkMessageData *self,
                                             const char        *message)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  g_free (self->thread_first_message);
  self->thread_first_message = (message && *message) ? g_strdup (message) : NULL;
}

void
gnalk_message_data_mark_as_thread (GnalkMessageData *self,
                                   gint64             thread_id)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  self->is_thread = TRUE;
  self->thread_id = thread_id;
}

const char *
gnalk_message_data_get_upload_file_name (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return self->upload_file_name;
}

const char *
gnalk_message_data_get_upload_file_path (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return self->upload_file_path;
}

const char *
gnalk_message_data_get_upload_mime_type (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return self->upload_mime_type;
}

/* Attachment group support */

gboolean
gnalk_message_data_is_file_only (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), FALSE);

  /* TRUE if message=="{file}" && messageParameters.file && !system && !parent */
  if (self->system_message && *self->system_message)
    return FALSE;
  if (self->parent)
    return FALSE;
  if (!self->message || g_strcmp0 (self->message, "{file}") != 0)
    return FALSE;
  if (!self->message_parameters || !json_object_has_member (self->message_parameters, "file"))
    return FALSE;
  return TRUE;
}

gboolean
gnalk_message_data_has_file (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), FALSE);

  /* TRUE if messageParameters.file present && !system */
  if (self->system_message && *self->system_message)
    return FALSE;
  if (!self->message_parameters || !json_object_has_member (self->message_parameters, "file"))
    return FALSE;
  return TRUE;
}

gboolean
gnalk_message_data_get_is_attachment_group_leader (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), FALSE);
  return self->is_attachment_group_leader;
}

void
gnalk_message_data_set_is_attachment_group_leader (GnalkMessageData *self,
                                                      gboolean           leader)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  self->is_attachment_group_leader = leader;
}

gboolean
gnalk_message_data_get_is_attachment_group_hidden (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), FALSE);
  return self->is_attachment_group_hidden;
}

void
gnalk_message_data_set_is_attachment_group_hidden (GnalkMessageData *self,
                                                      gboolean           hidden)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  self->is_attachment_group_hidden = hidden;
}

GPtrArray *
gnalk_message_data_get_attachment_group_files (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return self->attachment_group_files;
}

void
gnalk_message_data_set_attachment_group_files (GnalkMessageData *self,
                                                  GPtrArray         *files)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  g_clear_pointer (&self->attachment_group_files, g_ptr_array_unref);
  self->attachment_group_files = files ? g_ptr_array_ref (files) : NULL;
}

/* Database serialization */

static JsonObject *
parse_json_object (const char *json_str)
{
  if (!json_str || !*json_str)
    return NULL;

  JsonParser *parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, json_str, -1, NULL))
    {
      g_object_unref (parser);
      return NULL;
    }

  JsonNode *root = json_parser_get_root (parser);
  JsonObject *obj = NULL;

  if (root && JSON_NODE_HOLDS_OBJECT (root))
    obj = json_object_ref (json_node_get_object (root));

  g_object_unref (parser);
  return obj;
}

char *
gnalk_message_data_serialize_reactions (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return gnalk_message_utils_serialize_json_object (self->reactions);
}

char *
gnalk_message_data_serialize_parent (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return gnalk_message_utils_serialize_json_object (self->parent);
}

char *
gnalk_message_data_serialize_parameters (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return gnalk_message_utils_serialize_json_object (self->message_parameters);
}

GnalkMessageData *
gnalk_message_data_new_from_db (gint64                   id,
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
                                  const char              *thread_title)
{
  GnalkMessageData *self = g_object_new (GNALK_TYPE_MESSAGE_DATA, NULL);

  self->id = id;
  self->timestamp = timestamp;
  self->reply_to = reply_to;
  self->last_edit_timestamp = last_edit_timestamp;
  self->message = g_strdup (message ? message : "");
  self->author_id = g_strdup (author_id ? author_id : "");
  self->author_name = g_strdup (author_name ? author_name : "");
  self->reference_id = (reference_id && *reference_id) ? g_strdup (reference_id) : NULL;
  self->system_message = (char *) g_intern_string (system_message ? system_message : "");
  self->actor_type = (char *) g_intern_string (actor_type ? actor_type : "users");
  self->is_own = is_own;
  self->status = status;

  /* Parse JSON fields */
  self->reactions = parse_json_object (reactions_json);
  self->parent = parse_json_object (parent_json);
  self->message_parameters = parse_json_object (message_parameters_json);

  /* Default render_markdown to TRUE unless system message */
  self->render_markdown = !(self->system_message && *self->system_message);

  /* Thread support */
  self->thread_id = thread_id;
  self->is_thread = is_thread;
  self->thread_replies = thread_replies;
  self->thread_title = (thread_title && *thread_title) ? g_strdup (thread_title) : NULL;

  return self;
}
