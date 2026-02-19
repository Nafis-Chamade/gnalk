#include "gnalk-thread-data.h"

struct _GnalkThreadData
{
  GObject parent_instance;

  gint64  thread_id;
  char   *room_token;
  char   *title;
  gint64  last_message_id;
  gint64  last_activity;
  gint    num_replies;
  gint    notification_level;
  gboolean has_unread;

  char   *last_message_text;
  char   *last_message_actor_name;
};

enum {
  SIGNAL_UPDATED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (GnalkThreadData, gnalk_thread_data, G_TYPE_OBJECT)

static void
gnalk_thread_data_finalize (GObject *object)
{
  GnalkThreadData *self = GNALK_THREAD_DATA (object);

  g_free (self->room_token);
  g_free (self->title);
  g_free (self->last_message_text);
  g_free (self->last_message_actor_name);

  G_OBJECT_CLASS (gnalk_thread_data_parent_class)->finalize (object);
}

static void
gnalk_thread_data_class_init (GnalkThreadDataClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gnalk_thread_data_finalize;

  signals[SIGNAL_UPDATED] =
    g_signal_new ("updated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
gnalk_thread_data_init (GnalkThreadData *self)
{
  (void) self;
}

GnalkThreadData *
gnalk_thread_data_new_from_json (JsonObject *thread_info)
{
  g_return_val_if_fail (thread_info != NULL, NULL);

  /*
   * TalkThreadInfo is nested:
   *   { "thread": TalkThread, "attendee": TalkThreadAttendee,
   *     "first": ?TalkChatMessage, "last": ?TalkChatMessage }
   *
   * TalkThread: { id, roomToken, title, lastMessageId, lastActivity, numReplies }
   * TalkThreadAttendee: { notificationLevel }
   */
  JsonNode *thread_node = json_object_get_member (thread_info, "thread");
  if (!thread_node || !JSON_NODE_HOLDS_OBJECT (thread_node))
    return NULL;

  JsonObject *thread = json_node_get_object (thread_node);

  GnalkThreadData *self = g_object_new (GNALK_TYPE_THREAD_DATA, NULL);

  self->thread_id = json_object_get_int_member_with_default (thread, "id", 0);
  self->room_token = g_strdup (json_object_get_string_member_with_default (thread, "roomToken", ""));
  self->num_replies = (gint) json_object_get_int_member_with_default (thread, "numReplies", 0);
  self->last_activity = json_object_get_int_member_with_default (thread, "lastActivity", 0);
  self->last_message_id = json_object_get_int_member_with_default (thread, "lastMessageId", 0);

  /* Notification level from attendee sub-object */
  JsonNode *attendee_node = json_object_get_member (thread_info, "attendee");
  if (attendee_node && JSON_NODE_HOLDS_OBJECT (attendee_node))
    {
      JsonObject *attendee = json_node_get_object (attendee_node);
      self->notification_level = (gint) json_object_get_int_member_with_default (attendee, "notificationLevel", 0);
    }

  /* Thread title: use "title" field, fall back to first message text */
  const char *title = json_object_get_string_member_with_default (thread, "title", "");
  if (title && *title)
    {
      self->title = g_strdup (title);
    }
  else
    {
      /* Try first message (the thread root) */
      JsonNode *first_node = json_object_get_member (thread_info, "first");
      if (first_node && JSON_NODE_HOLDS_OBJECT (first_node))
        {
          JsonObject *first = json_node_get_object (first_node);
          const char *message = json_object_get_string_member_with_default (first, "message", "");
          const char *newline = message ? strchr (message, '\n') : NULL;
          self->title = newline ? g_strndup (message, newline - message) : g_strdup (message ? message : "");
        }
      else
        {
          self->title = g_strdup ("");
        }
    }

  /* Last message from "last" sub-object (TalkChatMessage) */
  JsonNode *last_node = json_object_get_member (thread_info, "last");
  if (last_node && JSON_NODE_HOLDS_OBJECT (last_node))
    {
      JsonObject *last_msg = json_node_get_object (last_node);
      /* Override lastMessageId with the actual last message's id */
      gint64 last_id = json_object_get_int_member_with_default (last_msg, "id", 0);
      if (last_id > 0)
        self->last_message_id = last_id;
      const char *text = json_object_get_string_member_with_default (last_msg, "message", "");
      self->last_message_text = (text && *text) ? g_strdup (text) : NULL;
      const char *actor = json_object_get_string_member_with_default (last_msg, "actorDisplayName", "");
      self->last_message_actor_name = (actor && *actor) ? g_strdup (actor) : NULL;
    }

  return self;
}

gboolean
gnalk_thread_data_update_from_data (GnalkThreadData *self,
                                    GnalkThreadData *source)
{
  g_return_val_if_fail (GNALK_IS_THREAD_DATA (self), FALSE);
  g_return_val_if_fail (GNALK_IS_THREAD_DATA (source), FALSE);

  if (self == source)
    return FALSE;

  gboolean changed = FALSE;

  if (g_strcmp0 (self->title, source->title) != 0)
    {
      g_free (self->title);
      self->title = g_strdup (source->title);
      changed = TRUE;
    }

  if (self->last_message_id != source->last_message_id)
    {
      self->last_message_id = source->last_message_id;
      changed = TRUE;
    }

  if (self->last_activity != source->last_activity)
    {
      self->last_activity = source->last_activity;
      changed = TRUE;
    }

  if (self->num_replies != source->num_replies)
    {
      self->num_replies = source->num_replies;
      changed = TRUE;
    }

  if (self->notification_level != source->notification_level)
    {
      self->notification_level = source->notification_level;
      changed = TRUE;
    }

  if (g_strcmp0 (self->last_message_text, source->last_message_text) != 0)
    {
      g_free (self->last_message_text);
      self->last_message_text = g_strdup (source->last_message_text);
      changed = TRUE;
    }

  if (g_strcmp0 (self->last_message_actor_name, source->last_message_actor_name) != 0)
    {
      g_free (self->last_message_actor_name);
      self->last_message_actor_name = g_strdup (source->last_message_actor_name);
      changed = TRUE;
    }

  if (self->has_unread != source->has_unread)
    {
      self->has_unread = source->has_unread;
      changed = TRUE;
    }

  if (changed)
    g_signal_emit (self, signals[SIGNAL_UPDATED], 0);

  return changed;
}

gint64
gnalk_thread_data_get_thread_id (GnalkThreadData *self)
{
  g_return_val_if_fail (GNALK_IS_THREAD_DATA (self), 0);
  return self->thread_id;
}

const char *
gnalk_thread_data_get_room_token (GnalkThreadData *self)
{
  g_return_val_if_fail (GNALK_IS_THREAD_DATA (self), NULL);
  return self->room_token;
}

const char *
gnalk_thread_data_get_title (GnalkThreadData *self)
{
  g_return_val_if_fail (GNALK_IS_THREAD_DATA (self), NULL);
  return self->title;
}

gint64
gnalk_thread_data_get_last_message_id (GnalkThreadData *self)
{
  g_return_val_if_fail (GNALK_IS_THREAD_DATA (self), 0);
  return self->last_message_id;
}

gint64
gnalk_thread_data_get_last_activity (GnalkThreadData *self)
{
  g_return_val_if_fail (GNALK_IS_THREAD_DATA (self), 0);
  return self->last_activity;
}

gint
gnalk_thread_data_get_num_replies (GnalkThreadData *self)
{
  g_return_val_if_fail (GNALK_IS_THREAD_DATA (self), 0);
  return self->num_replies;
}

gint
gnalk_thread_data_get_notification_level (GnalkThreadData *self)
{
  g_return_val_if_fail (GNALK_IS_THREAD_DATA (self), 0);
  return self->notification_level;
}

gboolean
gnalk_thread_data_get_has_unread (GnalkThreadData *self)
{
  g_return_val_if_fail (GNALK_IS_THREAD_DATA (self), FALSE);
  return self->has_unread;
}

void
gnalk_thread_data_set_has_unread (GnalkThreadData *self,
                                  gboolean         has_unread)
{
  g_return_if_fail (GNALK_IS_THREAD_DATA (self));

  if (self->has_unread == has_unread)
    return;

  self->has_unread = has_unread;
  g_signal_emit (self, signals[SIGNAL_UPDATED], 0);
}

void
gnalk_thread_data_update_last_message (GnalkThreadData *self,
                                       gint64           message_id,
                                       const char      *message_text,
                                       const char      *actor_name)
{
  g_return_if_fail (GNALK_IS_THREAD_DATA (self));

  gboolean changed = FALSE;

  if (message_id > self->last_message_id)
    {
      self->last_message_id = message_id;
      changed = TRUE;
    }

  if (g_strcmp0 (self->last_message_text, message_text) != 0)
    {
      g_free (self->last_message_text);
      self->last_message_text = g_strdup (message_text);
      changed = TRUE;
    }

  if (g_strcmp0 (self->last_message_actor_name, actor_name) != 0)
    {
      g_free (self->last_message_actor_name);
      self->last_message_actor_name = g_strdup (actor_name);
      changed = TRUE;
    }

  self->num_replies++;
  changed = TRUE;

  if (changed)
    g_signal_emit (self, signals[SIGNAL_UPDATED], 0);
}

const char *
gnalk_thread_data_get_last_message_text (GnalkThreadData *self)
{
  g_return_val_if_fail (GNALK_IS_THREAD_DATA (self), NULL);
  return self->last_message_text;
}

const char *
gnalk_thread_data_get_last_message_actor_name (GnalkThreadData *self)
{
  g_return_val_if_fail (GNALK_IS_THREAD_DATA (self), NULL);
  return self->last_message_actor_name;
}
