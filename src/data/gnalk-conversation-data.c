#include "gnalk-conversation-data.h"
#include "gnalk-session.h"
#include "gnalk-account.h"

/* Helper to replace message parameters like {user1}, {actor}, etc. */
static char *
replace_message_parameters (const char *message,
                            JsonObject *params,
                            const char *actor_name)
{
  if (!message || !*message)
    return g_strdup ("");

  char *result = g_strdup (message);

  /* Replace {actor} with actor name */
  if (actor_name && *actor_name)
    {
      GRegex *actor_regex = g_regex_new ("\\{actor\\}", 0, 0, NULL);
      if (actor_regex)
        {
          char *tmp = result;
          result = g_regex_replace_literal (actor_regex, tmp, -1, 0, actor_name, 0, NULL);
          if (!result)
            result = g_strdup (tmp);
          g_free (tmp);
          g_regex_unref (actor_regex);
        }
    }

  /* Replace parameters like {user1}, {file}, etc. */
  if (params)
    {
      JsonObjectIter iter;
      const char *param_key;
      JsonNode *param_node;

      json_object_iter_init (&iter, params);
      while (json_object_iter_next (&iter, &param_key, &param_node))
        {
          if (JSON_NODE_HOLDS_OBJECT (param_node))
            {
              JsonObject *param_obj = json_node_get_object (param_node);
              const char *name = json_object_get_string_member_with_default (param_obj, "name", NULL);

              if (name)
                {
                  char *pattern = g_strdup_printf ("\\{%s\\}", param_key);
                  GRegex *param_regex = g_regex_new (pattern, 0, 0, NULL);
                  g_free (pattern);

                  if (param_regex)
                    {
                      char *tmp = result;
                      result = g_regex_replace_literal (param_regex, tmp, -1, 0, name, 0, NULL);
                      if (!result)
                        result = g_strdup (tmp);
                      g_free (tmp);
                      g_regex_unref (param_regex);
                    }
                }
            }
        }
    }

  return result;
}

/* Extract only the first line from text (up to first newline) */
static char *
extract_first_line (const char *text)
{
  if (!text || !*text)
    return g_strdup ("");

  const char *newline = strchr (text, '\n');
  if (newline)
    return g_strndup (text, newline - text);

  return g_strdup (text);
}

/* Build subtitle from lastMessage with author prefix for group chats */
static char *
build_subtitle (const char *message_text,
                const char *message_type,
                const char *actor_name,
                JsonObject *message_params,
                gint        room_type,
                gboolean    is_own_message)
{
  /* First, replace all message parameters */
  g_autofree char *processed = replace_message_parameters (message_text, message_params, actor_name);

  /* Extract only the first line */
  g_autofree char *first_line = extract_first_line (processed);

  /* For system messages, don't add author prefix */
  if (g_strcmp0 (message_type, "system") == 0)
    return g_strdup (first_line);

  /* For 1:1 conversations (type 1), don't add author prefix */
  if (room_type == 1)
    return g_strdup (first_line);

  /* For group conversations, add author prefix */
  if (is_own_message)
    return g_strdup_printf ("You: %s", first_line);

  if (actor_name && *actor_name)
    return g_strdup_printf ("%s: %s", actor_name, first_line);

  return g_strdup (first_line);
}

struct _GnalkConversationData
{
  GObject parent_instance;

  char   *token;
  char   *title;
  char   *name;
  char   *description;
  char   *user_status;
  char   *actor_id;  /* User ID for 1:1 conversations (for avatar loading) */
  gint64  last_activity;
  gint64  last_common_read_message;
  gint64  last_read_message;  /* Last message ID the user has read */
  gint64  last_message_id;    /* ID of the last message (for sent status) */
  gint    unread_messages;
  gint    room_type;
  gint    participant_type;       /* 1=owner, 2=moderator, 3=user, etc. */
  gint    notification_level;     /* 0=default, 1=all, 2=mentions, 3=never */
  gint    call_notification_level; /* 0=off, 1=on */
  gboolean is_favorite;
  gboolean can_leave;
  gboolean can_delete;
  gboolean is_section_header;

  /* Raw last message data (for computing subtitle at display time) */
  char   *last_message_text;       /* Raw message text */
  char   *last_message_actor_id;   /* Author's actor ID */
  char   *last_message_actor_name; /* Author's display name */
  char   *last_message_type;       /* Message type (system, comment, etc.) */
  char   *last_message_params_json; /* JSON string of message parameters */

  /* Session that owns this conversation (weak reference, not owned) */
  GnalkSession *session;
};

enum {
  SIGNAL_UPDATED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (GnalkConversationData, gnalk_conversation_data, G_TYPE_OBJECT)

static void
gnalk_conversation_data_finalize (GObject *object)
{
  GnalkConversationData *self = GNALK_CONVERSATION_DATA (object);

  g_free (self->token);
  g_free (self->title);
  g_free (self->name);
  g_free (self->description);
  g_free (self->user_status);
  g_free (self->actor_id);
  g_free (self->last_message_text);
  g_free (self->last_message_actor_id);
  g_free (self->last_message_actor_name);
  g_free (self->last_message_type);
  g_free (self->last_message_params_json);

  G_OBJECT_CLASS (gnalk_conversation_data_parent_class)->finalize (object);
}

static void
gnalk_conversation_data_class_init (GnalkConversationDataClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gnalk_conversation_data_finalize;

  signals[SIGNAL_UPDATED] =
    g_signal_new ("updated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
gnalk_conversation_data_init (GnalkConversationData *self)
{
}

/* Helper to serialize message parameters to JSON string */
static char *
serialize_message_params (JsonObject *params)
{
  if (!params)
    return NULL;

  JsonNode *node = json_node_new (JSON_NODE_OBJECT);
  json_node_set_object (node, params);

  JsonGenerator *gen = json_generator_new ();
  json_generator_set_root (gen, node);
  char *json_str = json_generator_to_data (gen, NULL);

  g_object_unref (gen);
  json_node_free (node);

  return json_str;
}

/* Helper to deserialize message parameters from JSON string */
static JsonObject *
deserialize_message_params (const char *json_str)
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
  if (!root || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_object_unref (parser);
      return NULL;
    }

  JsonObject *obj = json_object_ref (json_node_get_object (root));
  g_object_unref (parser);
  return obj;
}

GnalkConversationData *
gnalk_conversation_data_new_from_json_with_user (JsonObject *room_data,
                                                  const char *current_user_id)
{
  (void) current_user_id;  /* Not needed anymore - subtitle computed at display time */

  GnalkConversationData *self = g_object_new (GNALK_TYPE_CONVERSATION_DATA, NULL);

  self->session = NULL;  /* Will be set later if needed */
  self->token = g_strdup (json_object_get_string_member_with_default (room_data, "token", ""));
  self->title = g_strdup (json_object_get_string_member_with_default (room_data, "displayName", ""));
  self->name = g_strdup (json_object_get_string_member_with_default (room_data, "name", ""));
  self->description = g_strdup (json_object_get_string_member_with_default (room_data, "description", ""));
  self->room_type = (gint) json_object_get_int_member_with_default (room_data, "type", 0);
  self->user_status = g_strdup (json_object_get_string_member_with_default (room_data, "status", ""));
  self->actor_id = g_strdup (json_object_get_string_member_with_default (room_data, "actorId", ""));

  /* Extract raw last message data */
  JsonNode *last_message_node = json_object_get_member (room_data, "lastMessage");
  if (last_message_node && JSON_NODE_HOLDS_OBJECT (last_message_node))
    {
      JsonObject *last_message = json_node_get_object (last_message_node);

      self->last_message_id = json_object_get_int_member_with_default (last_message, "id", 0);
      self->last_message_text = g_strdup (json_object_get_string_member_with_default (last_message, "message", ""));
      self->last_message_type = g_strdup (json_object_get_string_member_with_default (last_message, "messageType", "comment"));
      self->last_message_actor_name = g_strdup (json_object_get_string_member_with_default (last_message, "actorDisplayName", ""));
      self->last_message_actor_id = g_strdup (json_object_get_string_member_with_default (last_message, "actorId", ""));

      /* Serialize message parameters to JSON string for storage */
      JsonObject *params = NULL;
      if (json_object_has_member (last_message, "messageParameters"))
        {
          JsonNode *params_node = json_object_get_member (last_message, "messageParameters");
          if (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
            params = json_node_get_object (params_node);
        }
      self->last_message_params_json = serialize_message_params (params);
    }
  else
    {
      self->last_message_id = 0;
      self->last_message_text = g_strdup ("");
      self->last_message_type = g_strdup ("");
      self->last_message_actor_name = g_strdup ("");
      self->last_message_actor_id = g_strdup ("");
      self->last_message_params_json = NULL;
    }

  self->last_activity = json_object_get_int_member_with_default (room_data, "lastActivity", 0);
  self->last_common_read_message = json_object_get_int_member_with_default (room_data, "lastCommonReadMessage", 0);
  self->last_read_message = json_object_get_int_member_with_default (room_data, "lastReadMessage", 0);
  self->unread_messages = (gint) json_object_get_int_member_with_default (room_data, "unreadMessages", 0);
  self->participant_type = (gint) json_object_get_int_member_with_default (room_data, "participantType", 3);
  self->notification_level = (gint) json_object_get_int_member_with_default (room_data, "notificationLevel", 0);
  self->call_notification_level = (gint) json_object_get_int_member_with_default (room_data, "notificationCalls", 1);
  self->is_favorite = json_object_get_boolean_member_with_default (room_data, "isFavorite", FALSE);
  self->can_leave = json_object_get_boolean_member_with_default (room_data, "canLeaveConversation", TRUE);
  self->can_delete = json_object_get_boolean_member_with_default (room_data, "canDeleteConversation", FALSE);
  self->is_section_header = FALSE;

  return self;
}

GnalkConversationData *
gnalk_conversation_data_new_from_json (JsonObject    *room_data,
                                         GnalkSession *session)
{
  const char *current_user_id = NULL;
  if (session)
    {
      GnalkAccount *account = gnalk_session_get_account (session);
      if (account)
        current_user_id = gnalk_account_get_user_id (account);
    }

  GnalkConversationData *self = gnalk_conversation_data_new_from_json_with_user (room_data, current_user_id);
  if (self)
    self->session = session;  /* Set session reference */

  return self;
}

GnalkConversationData *
gnalk_conversation_data_new_from_db (const char    *token,
                                       const char    *title,
                                       const char    *name,
                                       const char    *user_status,
                                       const char    *actor_id,
                                       gint           room_type,
                                       gint64         last_activity,
                                       gint64         last_common_read_message,
                                       gint           unread_messages,
                                       gboolean       is_favorite,
                                       gboolean       can_leave,
                                       gboolean       can_delete,
                                       const char    *description,
                                       gint           participant_type,
                                       gint           notification_level,
                                       gint           call_notification_level,
                                       gint64         last_read_message,
                                       gint64         last_message_id,
                                       const char    *last_message_text,
                                       const char    *last_message_actor_id,
                                       const char    *last_message_actor_name,
                                       const char    *last_message_type,
                                       const char    *last_message_params_json,
                                       GnalkSession *session)
{
  GnalkConversationData *self = g_object_new (GNALK_TYPE_CONVERSATION_DATA, NULL);

  self->session = session;  /* Weak reference, not owned */
  self->token = g_strdup (token ? token : "");
  self->title = g_strdup (title ? title : "");
  self->name = g_strdup (name ? name : "");
  self->user_status = g_strdup (user_status ? user_status : "");
  self->actor_id = g_strdup (actor_id ? actor_id : "");
  self->room_type = room_type;
  self->last_activity = last_activity;
  self->last_common_read_message = last_common_read_message;
  self->unread_messages = unread_messages;
  self->is_favorite = is_favorite;
  self->can_leave = can_leave;
  self->can_delete = can_delete;
  self->description = g_strdup (description ? description : "");
  self->participant_type = participant_type;
  self->notification_level = notification_level;
  self->call_notification_level = call_notification_level;
  self->last_read_message = last_read_message;
  self->is_section_header = FALSE;

  /* Raw last message data */
  self->last_message_id = last_message_id;
  self->last_message_text = g_strdup (last_message_text ? last_message_text : "");
  self->last_message_actor_id = g_strdup (last_message_actor_id ? last_message_actor_id : "");
  self->last_message_actor_name = g_strdup (last_message_actor_name ? last_message_actor_name : "");
  self->last_message_type = g_strdup (last_message_type ? last_message_type : "");
  self->last_message_params_json = g_strdup (last_message_params_json);

  return self;
}

GnalkConversationData *
gnalk_conversation_data_new_section_header (const char *title, gboolean is_favorite_section)
{
  GnalkConversationData *self = g_object_new (GNALK_TYPE_CONVERSATION_DATA, NULL);

  self->title = g_strdup (title);
  self->is_section_header = TRUE;
  self->is_favorite = is_favorite_section;
  /* Set last_activity to max so headers stay at top of their section */
  self->last_activity = G_MAXINT64;

  return self;
}

gboolean
gnalk_conversation_data_is_section_header (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), FALSE);
  return self->is_section_header;
}

const char *
gnalk_conversation_data_get_token (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return self->token;
}

const char *
gnalk_conversation_data_get_title (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return self->title;
}

const char *
gnalk_conversation_data_get_name (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return self->name;
}

/* Compute and return the formatted subtitle.
 * The caller must free the returned string. */
char *
gnalk_conversation_data_get_subtitle (GnalkConversationData *self,
                                        const char             *current_user_id)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);

  /* If no last message text, return empty */
  if (!self->last_message_text || !*self->last_message_text)
    return g_strdup ("");

  /* Determine if last message is from the current user */
  gboolean is_own_message = (current_user_id && self->last_message_actor_id &&
                             g_strcmp0 (current_user_id, self->last_message_actor_id) == 0);

  /* Deserialize message parameters */
  JsonObject *params = deserialize_message_params (self->last_message_params_json);

  /* Build the subtitle using the common helper */
  char *subtitle = build_subtitle (self->last_message_text,
                                   self->last_message_type,
                                   self->last_message_actor_name,
                                   params,
                                   self->room_type,
                                   is_own_message);

  if (params)
    json_object_unref (params);

  return subtitle;
}

const char *
gnalk_conversation_data_get_description (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return self->description;
}

const char *
gnalk_conversation_data_get_user_status (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return self->user_status;
}

gint
gnalk_conversation_data_get_room_type (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), 0);
  return self->room_type;
}

gint64
gnalk_conversation_data_get_last_activity (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), 0);
  return self->last_activity;
}

gint64
gnalk_conversation_data_get_last_common_read_message (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), 0);
  return self->last_common_read_message;
}

gint
gnalk_conversation_data_get_unread_messages (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), 0);
  return self->unread_messages;
}

const char *
gnalk_conversation_data_get_actor_id (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return self->actor_id;
}

gboolean
gnalk_conversation_data_get_is_favorite (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), FALSE);
  return self->is_favorite;
}

gboolean
gnalk_conversation_data_get_can_leave (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), TRUE);
  return self->can_leave;
}

gboolean
gnalk_conversation_data_get_can_delete (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), FALSE);
  return self->can_delete;
}

gint64
gnalk_conversation_data_get_last_read_message (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), 0);
  return self->last_read_message;
}

GnalkSession *
gnalk_conversation_data_get_session (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return self->session;
}

void
gnalk_conversation_data_set_session (GnalkConversationData *self,
                                       GnalkSession          *session)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_DATA (self));
  self->session = session;  /* Weak reference, not owned */
}

void
gnalk_conversation_data_set_title (GnalkConversationData *self,
                                      const char             *title)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_DATA (self));

  if (g_strcmp0 (self->title, title) == 0)
    return;

  g_free (self->title);
  self->title = g_strdup (title);
  g_signal_emit (self, signals[SIGNAL_UPDATED], 0);
}

void
gnalk_conversation_data_set_description (GnalkConversationData *self,
                                            const char             *description)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_DATA (self));

  if (g_strcmp0 (self->description, description) == 0)
    return;

  g_free (self->description);
  self->description = g_strdup (description);
  g_signal_emit (self, signals[SIGNAL_UPDATED], 0);
}

gint
gnalk_conversation_data_get_participant_type (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), 3);
  return self->participant_type;
}

gint
gnalk_conversation_data_get_notification_level (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), 0);
  return self->notification_level;
}

gint
gnalk_conversation_data_get_call_notification_level (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), 1);
  return self->call_notification_level;
}

gboolean
gnalk_conversation_data_can_moderate (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), FALSE);
  /* Owner=1, Moderator=2, Guest moderator=6 */
  return (self->participant_type == 1 ||
          self->participant_type == 2 ||
          self->participant_type == 6);
}

void
gnalk_conversation_data_update_from_json (GnalkConversationData *self,
                                            JsonObject             *room_data)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_DATA (self));
  g_return_if_fail (room_data != NULL);

  /* Update mutable fields */
  g_free (self->title);
  self->title = g_strdup (json_object_get_string_member_with_default (room_data, "displayName", ""));

  /* Update raw last message data */
  JsonNode *last_message_node = json_object_get_member (room_data, "lastMessage");
  if (last_message_node && JSON_NODE_HOLDS_OBJECT (last_message_node))
    {
      JsonObject *last_message = json_node_get_object (last_message_node);

      self->last_message_id = json_object_get_int_member_with_default (last_message, "id", 0);

      g_free (self->last_message_text);
      self->last_message_text = g_strdup (json_object_get_string_member_with_default (last_message, "message", ""));

      g_free (self->last_message_type);
      self->last_message_type = g_strdup (json_object_get_string_member_with_default (last_message, "messageType", "comment"));

      g_free (self->last_message_actor_name);
      self->last_message_actor_name = g_strdup (json_object_get_string_member_with_default (last_message, "actorDisplayName", ""));

      g_free (self->last_message_actor_id);
      self->last_message_actor_id = g_strdup (json_object_get_string_member_with_default (last_message, "actorId", ""));

      /* Serialize message parameters */
      JsonObject *params = NULL;
      if (json_object_has_member (last_message, "messageParameters"))
        {
          JsonNode *params_node = json_object_get_member (last_message, "messageParameters");
          if (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
            params = json_node_get_object (params_node);
        }
      g_free (self->last_message_params_json);
      self->last_message_params_json = serialize_message_params (params);
    }
  else
    {
      self->last_message_id = 0;
      g_free (self->last_message_text);
      self->last_message_text = g_strdup ("");
      g_free (self->last_message_type);
      self->last_message_type = g_strdup ("");
      g_free (self->last_message_actor_name);
      self->last_message_actor_name = g_strdup ("");
      g_free (self->last_message_actor_id);
      self->last_message_actor_id = g_strdup ("");
      g_free (self->last_message_params_json);
      self->last_message_params_json = NULL;
    }

  g_free (self->user_status);
  self->user_status = g_strdup (json_object_get_string_member_with_default (room_data, "status", ""));

  self->last_activity = json_object_get_int_member_with_default (room_data, "lastActivity", 0);
  self->last_common_read_message = json_object_get_int_member_with_default (room_data, "lastCommonReadMessage", 0);
  self->last_read_message = json_object_get_int_member_with_default (room_data, "lastReadMessage", 0);
  self->unread_messages = (gint) json_object_get_int_member_with_default (room_data, "unreadMessages", 0);
  self->participant_type = (gint) json_object_get_int_member_with_default (room_data, "participantType", self->participant_type);
  self->notification_level = (gint) json_object_get_int_member_with_default (room_data, "notificationLevel", self->notification_level);
  self->call_notification_level = (gint) json_object_get_int_member_with_default (room_data, "notificationCalls", self->call_notification_level);
  self->is_favorite = json_object_get_boolean_member_with_default (room_data, "isFavorite", FALSE);

  /* Emit signal so row can update */
  g_signal_emit (self, signals[SIGNAL_UPDATED], 0);
}

void
gnalk_conversation_data_update_last_message (GnalkConversationData *self,
                                               gint64                  last_activity,
                                               gint64                  last_message_id,
                                               const char             *last_message_text,
                                               const char             *last_message_actor_id,
                                               const char             *last_message_actor_name,
                                               const char             *last_message_type,
                                               const char             *last_message_params_json)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_DATA (self));

  self->last_activity = last_activity;
  self->last_message_id = last_message_id;

  g_free (self->last_message_text);
  self->last_message_text = g_strdup (last_message_text ? last_message_text : "");

  g_free (self->last_message_actor_id);
  self->last_message_actor_id = g_strdup (last_message_actor_id ? last_message_actor_id : "");

  g_free (self->last_message_actor_name);
  self->last_message_actor_name = g_strdup (last_message_actor_name ? last_message_actor_name : "");

  g_free (self->last_message_type);
  self->last_message_type = g_strdup (last_message_type ? last_message_type : "");

  g_free (self->last_message_params_json);
  self->last_message_params_json = g_strdup (last_message_params_json);

  /* Emit signal so row can update */
  g_signal_emit (self, signals[SIGNAL_UPDATED], 0);
}

gboolean
gnalk_conversation_data_update_from_data (GnalkConversationData *self,
                                            GnalkConversationData *source)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), FALSE);
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (source), FALSE);

  /* Don't update from self */
  if (self == source)
    return FALSE;

  gboolean changed = FALSE;

  /* Compare and update string fields */
  if (g_strcmp0 (self->title, source->title) != 0)
    {
      g_free (self->title);
      self->title = g_strdup (source->title);
      changed = TRUE;
    }

  if (g_strcmp0 (self->user_status, source->user_status) != 0)
    {
      g_free (self->user_status);
      self->user_status = g_strdup (source->user_status);
      changed = TRUE;
    }

  if (g_strcmp0 (self->description, source->description) != 0)
    {
      g_free (self->description);
      self->description = g_strdup (source->description);
      changed = TRUE;
    }

  /* Compare and update raw last message fields */
  if (g_strcmp0 (self->last_message_text, source->last_message_text) != 0)
    {
      g_free (self->last_message_text);
      self->last_message_text = g_strdup (source->last_message_text);
      changed = TRUE;
    }

  if (g_strcmp0 (self->last_message_actor_id, source->last_message_actor_id) != 0)
    {
      g_free (self->last_message_actor_id);
      self->last_message_actor_id = g_strdup (source->last_message_actor_id);
      changed = TRUE;
    }

  if (g_strcmp0 (self->last_message_actor_name, source->last_message_actor_name) != 0)
    {
      g_free (self->last_message_actor_name);
      self->last_message_actor_name = g_strdup (source->last_message_actor_name);
      changed = TRUE;
    }

  if (g_strcmp0 (self->last_message_type, source->last_message_type) != 0)
    {
      g_free (self->last_message_type);
      self->last_message_type = g_strdup (source->last_message_type);
      changed = TRUE;
    }

  if (g_strcmp0 (self->last_message_params_json, source->last_message_params_json) != 0)
    {
      g_free (self->last_message_params_json);
      self->last_message_params_json = g_strdup (source->last_message_params_json);
      changed = TRUE;
    }

  /* Compare and update numeric fields */
  if (self->last_activity != source->last_activity)
    {
      self->last_activity = source->last_activity;
      changed = TRUE;
    }

  if (self->last_common_read_message != source->last_common_read_message)
    {
      self->last_common_read_message = source->last_common_read_message;
      changed = TRUE;
    }

  if (self->last_read_message != source->last_read_message)
    {
      self->last_read_message = source->last_read_message;
      changed = TRUE;
    }

  if (self->unread_messages != source->unread_messages)
    {
      self->unread_messages = source->unread_messages;
      changed = TRUE;
    }

  if (self->participant_type != source->participant_type)
    {
      self->participant_type = source->participant_type;
      changed = TRUE;
    }

  if (self->notification_level != source->notification_level)
    {
      self->notification_level = source->notification_level;
      changed = TRUE;
    }

  if (self->call_notification_level != source->call_notification_level)
    {
      self->call_notification_level = source->call_notification_level;
      changed = TRUE;
    }

  if (self->is_favorite != source->is_favorite)
    {
      self->is_favorite = source->is_favorite;
      changed = TRUE;
    }

  if (self->can_leave != source->can_leave)
    {
      self->can_leave = source->can_leave;
      changed = TRUE;
    }

  if (self->can_delete != source->can_delete)
    {
      self->can_delete = source->can_delete;
      changed = TRUE;
    }

  if (self->last_message_id != source->last_message_id)
    {
      self->last_message_id = source->last_message_id;
      changed = TRUE;
    }

  /* Update session reference (don't count as change for UI purposes) */
  self->session = source->session;

  /* Only emit signal if something actually changed */
  if (changed)
    g_signal_emit (self, signals[SIGNAL_UPDATED], 0);

  return changed;
}

gint64
gnalk_conversation_data_get_last_message_id (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), 0);
  return self->last_message_id;
}

gboolean
gnalk_conversation_data_get_last_message_is_own (GnalkConversationData *self,
                                                   const char             *current_user_id)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), FALSE);

  if (!current_user_id || !self->last_message_actor_id)
    return FALSE;

  return g_strcmp0 (current_user_id, self->last_message_actor_id) == 0;
}

/* Raw last message data getters (for database storage) */

const char *
gnalk_conversation_data_get_last_message_text (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return self->last_message_text;
}

const char *
gnalk_conversation_data_get_last_message_actor_id (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return self->last_message_actor_id;
}

const char *
gnalk_conversation_data_get_last_message_actor_name (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return self->last_message_actor_name;
}

const char *
gnalk_conversation_data_get_last_message_type (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return self->last_message_type;
}

const char *
gnalk_conversation_data_get_last_message_params_json (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return self->last_message_params_json;
}
