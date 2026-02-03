#include "gnalk-conversation-data.h"

struct _GnalkConversationData
{
  GObject parent_instance;

  char   *token;
  char   *title;
  char   *name;
  char   *subtitle;
  char   *description;
  char   *user_status;
  char   *actor_id;  /* User ID for 1:1 conversations (for avatar loading) */
  gint64  last_activity;
  gint64  last_common_read_message;
  gint64  last_read_message;  /* Last message ID the user has read */
  gint    unread_messages;
  gint    room_type;
  gint    participant_type;       /* 1=owner, 2=moderator, 3=user, etc. */
  gint    notification_level;     /* 0=default, 1=all, 2=mentions, 3=never */
  gint    call_notification_level; /* 0=off, 1=on */
  gboolean is_favorite;
  gboolean can_leave;
  gboolean can_delete;
  gboolean is_section_header;

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
  g_free (self->subtitle);
  g_free (self->description);
  g_free (self->user_status);
  g_free (self->actor_id);

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

GnalkConversationData *
gnalk_conversation_data_new_from_json (JsonObject    *room_data,
                                         GnalkSession *session)
{
  GnalkConversationData *self = g_object_new (GNALK_TYPE_CONVERSATION_DATA, NULL);

  self->session = session;  /* Weak reference, not owned */
  self->token = g_strdup (json_object_get_string_member_with_default (room_data, "token", ""));
  self->title = g_strdup (json_object_get_string_member_with_default (room_data, "displayName", ""));
  self->name = g_strdup (json_object_get_string_member_with_default (room_data, "name", ""));
  self->description = g_strdup (json_object_get_string_member_with_default (room_data, "description", ""));
  self->room_type = (gint) json_object_get_int_member_with_default (room_data, "type", 0);
  self->user_status = g_strdup (json_object_get_string_member_with_default (room_data, "status", ""));
  self->actor_id = g_strdup (json_object_get_string_member_with_default (room_data, "actorId", ""));

  JsonNode *last_message_node = json_object_get_member (room_data, "lastMessage");
  if (last_message_node && JSON_NODE_HOLDS_OBJECT (last_message_node))
    {
      JsonObject *last_message = json_node_get_object (last_message_node);
      const char *message_text = json_object_get_string_member_with_default (last_message, "message", "");
      const char *message_type = json_object_get_string_member_with_default (last_message, "messageType", "comment");
      const char *actor_name = json_object_get_string_member_with_default (last_message, "actorDisplayName", "");

      if (g_strcmp0 (message_type, "system") == 0)
        {
          /* Start with the original message */
          self->subtitle = g_strdup (message_text);

          /* Replace {actor} with actor name */
          GRegex *actor_regex = g_regex_new ("\\{actor\\}", 0, 0, NULL);
          if (actor_regex)
            {
              char *tmp = self->subtitle;
              self->subtitle = g_regex_replace_literal (actor_regex, tmp, -1, 0, actor_name, 0, NULL);
              if (!self->subtitle)
                self->subtitle = g_strdup (tmp);
              g_free (tmp);
              g_regex_unref (actor_regex);
            }

          /* Parse messageParameters for user1, user2, etc. */
          if (json_object_has_member (last_message, "messageParameters"))
            {
              JsonNode *params_node = json_object_get_member (last_message, "messageParameters");
              if (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
                {
                  JsonObject *params = json_node_get_object (params_node);
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
                                      char *tmp = self->subtitle;
                                      self->subtitle = g_regex_replace_literal (param_regex, tmp, -1, 0, name, 0, NULL);
                                      if (!self->subtitle)
                                        self->subtitle = g_strdup (tmp);
                                      g_free (tmp);
                                      g_regex_unref (param_regex);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
      else
        {
          self->subtitle = g_strdup (message_text);
        }
    }
  else
    self->subtitle = g_strdup ("");

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
gnalk_conversation_data_new_from_db (const char    *token,
                                       const char    *title,
                                       const char    *name,
                                       const char    *subtitle,
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
                                       GnalkSession *session)
{
  GnalkConversationData *self = g_object_new (GNALK_TYPE_CONVERSATION_DATA, NULL);

  self->session = session;  /* Weak reference, not owned */
  self->token = g_strdup (token ? token : "");
  self->title = g_strdup (title ? title : "");
  self->name = g_strdup (name ? name : "");
  self->subtitle = g_strdup (subtitle ? subtitle : "");
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

const char *
gnalk_conversation_data_get_subtitle (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return self->subtitle;
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

  /* Parse subtitle from lastMessage (same logic as new_from_json) */
  g_free (self->subtitle);
  JsonNode *last_message_node = json_object_get_member (room_data, "lastMessage");
  if (last_message_node && JSON_NODE_HOLDS_OBJECT (last_message_node))
    {
      JsonObject *last_message = json_node_get_object (last_message_node);
      const char *message_text = json_object_get_string_member_with_default (last_message, "message", "");
      const char *message_type = json_object_get_string_member_with_default (last_message, "messageType", "comment");
      const char *actor_name = json_object_get_string_member_with_default (last_message, "actorDisplayName", "");

      if (g_strcmp0 (message_type, "system") == 0)
        {
          /* Start with the original message */
          self->subtitle = g_strdup (message_text);

          /* Replace {actor} with actor name */
          GRegex *actor_regex = g_regex_new ("\\{actor\\}", 0, 0, NULL);
          if (actor_regex)
            {
              char *tmp = self->subtitle;
              self->subtitle = g_regex_replace_literal (actor_regex, tmp, -1, 0, actor_name, 0, NULL);
              if (!self->subtitle)
                self->subtitle = g_strdup (tmp);
              g_free (tmp);
              g_regex_unref (actor_regex);
            }

          /* Parse messageParameters for user1, user2, etc. */
          if (json_object_has_member (last_message, "messageParameters"))
            {
              JsonNode *params_node = json_object_get_member (last_message, "messageParameters");
              if (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
                {
                  JsonObject *params = json_node_get_object (params_node);
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
                                      char *tmp = self->subtitle;
                                      self->subtitle = g_regex_replace_literal (param_regex, tmp, -1, 0, name, 0, NULL);
                                      if (!self->subtitle)
                                        self->subtitle = g_strdup (tmp);
                                      g_free (tmp);
                                      g_regex_unref (param_regex);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
      else
        {
          self->subtitle = g_strdup (message_text);
        }
    }
  else
    self->subtitle = g_strdup ("");

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

  if (g_strcmp0 (self->subtitle, source->subtitle) != 0)
    {
      g_free (self->subtitle);
      self->subtitle = g_strdup (source->subtitle);
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

  /* Update session reference (don't count as change for UI purposes) */
  self->session = source->session;

  /* Only emit signal if something actually changed */
  if (changed)
    g_signal_emit (self, signals[SIGNAL_UPDATED], 0);

  return changed;
}
