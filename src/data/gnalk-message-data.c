#include "gnalk-message-data.h"

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
  char   *system_message;
  gboolean is_own;
  gboolean show_date_header;
  gboolean show_timestamp;
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
};

G_DEFINE_FINAL_TYPE (GnalkMessageData, gnalk_message_data, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_IS_UNREAD,
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
  g_free (self->system_message);
  g_free (self->upload_file_name);
  g_free (self->upload_file_path);
  g_free (self->upload_mime_type);

  if (self->reactions)
    json_object_unref (self->reactions);
  if (self->parent)
    json_object_unref (self->parent);
  if (self->message_parameters)
    json_object_unref (self->message_parameters);

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
    g_param_spec_boolean ("is-unread",
                          "Is Unread",
                          "Whether the message is unread",
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

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
  self->reference_id = g_strdup (json_object_get_string_member_with_default (message_data, "referenceId", ""));
  self->system_message = g_strdup (json_object_get_string_member_with_default (message_data, "systemMessage", ""));

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

  /* Copy parent (quoted message) if present and is an object */
  if (json_object_has_member (message_data, "parent"))
    {
      JsonNode *node = json_object_get_member (message_data, "parent");
      if (node && JSON_NODE_HOLDS_OBJECT (node))
        {
          JsonObject *parent = json_node_get_object (node);
          if (parent)
            self->parent = json_object_ref (parent);
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

  return self;
}

GnalkMessageData *
gnalk_message_data_new_pending (const char *message_text,
                                  const char *author_name,
                                  const char *author_id,
                                  const char *reference_id,
                                  gint64      reply_to)
{
  GnalkMessageData *self = g_object_new (GNALK_TYPE_MESSAGE_DATA, NULL);

  self->id = 0;  /* Will be set when server confirms */
  self->timestamp = g_get_real_time () / G_USEC_PER_SEC;
  self->message = g_strdup (message_text ? message_text : "");
  self->author_id = g_strdup (author_id ? author_id : "");
  self->author_name = g_strdup (author_name ? author_name : "");
  self->reference_id = g_strdup (reference_id ? reference_id : "");
  self->reply_to = reply_to;
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
  self->status = status;
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

  self->id = message_id;
  if (timestamp > 0)
    self->timestamp = timestamp;
  self->status = GNALK_MESSAGE_DATA_STATUS_SENT;
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

  g_free (self->message);
  self->message = g_strdup (message ? message : "");
}

void
gnalk_message_data_set_last_edit_timestamp (GnalkMessageData *self,
                                              gint64             timestamp)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));
  self->last_edit_timestamp = timestamp;
}

void
gnalk_message_data_set_system_message (GnalkMessageData *self,
                                         const char        *system_message)
{
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (self));

  g_free (self->system_message);
  self->system_message = g_strdup (system_message);
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
  /* Use caption if provided, otherwise show filename */
  self->message = g_strdup ((caption && *caption) ? caption : (file_name ? file_name : ""));
  self->author_id = g_strdup (author_id ? author_id : "");
  self->author_name = g_strdup (author_name ? author_name : "");
  self->reference_id = g_strdup (reference_id ? reference_id : "");
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
  self->is_uploading = uploading;
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
  self->upload_progress = CLAMP (progress, 0.0, 1.0);
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

/* Database serialization */

static char *
serialize_json_object (JsonObject *obj)
{
  if (!obj)
    return NULL;

  JsonNode *node = json_node_new (JSON_NODE_OBJECT);
  json_node_set_object (node, obj);

  JsonGenerator *gen = json_generator_new ();
  json_generator_set_root (gen, node);
  char *json = json_generator_to_data (gen, NULL);

  g_object_unref (gen);
  json_node_free (node);

  return json;
}

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
  return serialize_json_object (self->reactions);
}

char *
gnalk_message_data_serialize_parent (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return serialize_json_object (self->parent);
}

char *
gnalk_message_data_serialize_parameters (GnalkMessageData *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_DATA (self), NULL);
  return serialize_json_object (self->message_parameters);
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
                                  const char              *message_parameters_json)
{
  GnalkMessageData *self = g_object_new (GNALK_TYPE_MESSAGE_DATA, NULL);

  self->id = id;
  self->timestamp = timestamp;
  self->reply_to = reply_to;
  self->last_edit_timestamp = last_edit_timestamp;
  self->message = g_strdup (message ? message : "");
  self->author_id = g_strdup (author_id ? author_id : "");
  self->author_name = g_strdup (author_name ? author_name : "");
  self->reference_id = g_strdup (reference_id ? reference_id : "");
  self->system_message = g_strdup (system_message ? system_message : "");
  self->is_own = is_own;
  self->status = status;

  /* Parse JSON fields */
  self->reactions = parse_json_object (reactions_json);
  self->parent = parse_json_object (parent_json);
  self->message_parameters = parse_json_object (message_parameters_json);

  /* Default render_markdown to TRUE unless system message */
  self->render_markdown = !(self->system_message && *self->system_message);

  return self;
}
