#include "gnalk-conversation-data.h"
#include "gnalk-thread-data.h"
#include "gnalk-session.h"
#include "gnalk-account.h"
#include "gnalk-message-utils.h"

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
  g_autofree char *processed = gnalk_message_utils_replace_parameters (message_text, message_params, actor_name);

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
  char   *actor_id;      /* User ID for 1:1 conversations (for avatar loading) */
  char   *remote_server; /* Non-empty for federated conversations */
  char   *remote_token;  /* Token on the remote server */
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
  char       *last_message_text;       /* Raw message text */
  char       *last_message_actor_id;   /* Author's actor ID */
  char       *last_message_actor_name; /* Author's display name */
  char       *last_message_type;       /* Interned via g_intern_string — do not g_free */
  JsonObject *last_message_params;     /* Parsed message parameters (owned, parsed once) */

  /* Cached subtitle (computed on first get_subtitle(), invalidated on update) */
  char   *cached_subtitle;

  /* Federation invitation ID (>0 means this is a pending invitation) */
  gint64  invitation_id;

  /* Thread sub-list for sidebar tree (lazily created) */
  GListStore *threads_store;
  gboolean    has_unread_threads;

  /* Session that owns this conversation (weak reference, not owned) */
  GWeakRef session_ref;  /* Weak ref to owning session (may outlive it) */
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
  g_free (self->remote_server);
  g_free (self->remote_token);
  g_free (self->last_message_text);
  g_free (self->last_message_actor_id);
  g_free (self->last_message_actor_name);
  /* last_message_type is interned — do not free */
  g_clear_pointer (&self->last_message_params, json_object_unref);
  g_free (self->cached_subtitle);
  g_clear_object (&self->threads_store);
  g_weak_ref_clear (&self->session_ref);

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
  g_weak_ref_init (&self->session_ref, NULL);
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

  self->token = g_strdup (json_object_get_string_member_with_default (room_data, "token", ""));
  self->title = g_strdup (json_object_get_string_member_with_default (room_data, "displayName", ""));
  self->name = g_strdup (json_object_get_string_member_with_default (room_data, "name", ""));
  self->room_type = (gint) json_object_get_int_member_with_default (room_data, "type", 0);
  self->actor_id = g_strdup (json_object_get_string_member_with_default (room_data, "actorId", ""));

  /* Often-empty fields: store NULL instead of "" to avoid allocations */
  const char *desc = json_object_get_string_member_with_default (room_data, "description", "");
  self->description = (desc && *desc) ? g_strdup (desc) : NULL;
  const char *status = json_object_get_string_member_with_default (room_data, "status", "");
  self->user_status = (status && *status) ? g_strdup (status) : NULL;
  const char *remote_srv = json_object_get_string_member_with_default (room_data, "remoteServer", "");
  self->remote_server = (remote_srv && *remote_srv) ? g_strdup (remote_srv) : NULL;
  const char *remote_tok = json_object_get_string_member_with_default (room_data, "remoteToken", "");
  self->remote_token = (remote_tok && *remote_tok) ? g_strdup (remote_tok) : NULL;

  /* Extract raw last message data */
  JsonNode *last_message_node = json_object_get_member (room_data, "lastMessage");
  if (last_message_node && JSON_NODE_HOLDS_OBJECT (last_message_node))
    {
      JsonObject *last_message = json_node_get_object (last_message_node);

      self->last_message_id = json_object_get_int_member_with_default (last_message, "id", 0);
      self->last_message_text = g_strdup (json_object_get_string_member_with_default (last_message, "message", ""));
      self->last_message_type = (char *) g_intern_string (json_object_get_string_member_with_default (last_message, "messageType", "comment"));
      self->last_message_actor_name = g_strdup (json_object_get_string_member_with_default (last_message, "actorDisplayName", ""));
      self->last_message_actor_id = g_strdup (json_object_get_string_member_with_default (last_message, "actorId", ""));

      /* Store parsed message parameters directly (no serialize/deserialize round-trip) */
      self->last_message_params = NULL;
      if (json_object_has_member (last_message, "messageParameters"))
        {
          JsonNode *params_node = json_object_get_member (last_message, "messageParameters");
          if (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
            self->last_message_params = json_object_ref (json_node_get_object (params_node));
        }
    }
  else
    {
      self->last_message_id = 0;
      self->last_message_text = NULL;
      self->last_message_type = NULL;
      self->last_message_actor_name = NULL;
      self->last_message_actor_id = NULL;
      self->last_message_params = NULL;
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
    g_weak_ref_set (&self->session_ref, session);  /* Set session reference */

  return self;
}

GnalkConversationData *
gnalk_conversation_data_new_from_db (const GnalkConversationDataInit *init)
{
  GnalkConversationData *self = g_object_new (GNALK_TYPE_CONVERSATION_DATA, NULL);

  g_weak_ref_set (&self->session_ref, init->session);  /* Weak reference, not owned */
  self->token = g_strdup (init->token ? init->token : "");
  self->title = g_strdup (init->title ? init->title : "");
  self->name = g_strdup (init->name ? init->name : "");
  self->actor_id = g_strdup (init->actor_id ? init->actor_id : "");
  self->room_type = init->room_type;
  self->last_activity = init->last_activity;
  self->last_common_read_message = init->last_common_read_message;
  self->unread_messages = init->unread_messages;
  self->is_favorite = init->is_favorite;
  self->can_leave = init->can_leave;
  self->can_delete = init->can_delete;
  self->participant_type = init->participant_type;
  self->notification_level = init->notification_level;
  self->call_notification_level = init->call_notification_level;
  self->last_read_message = init->last_read_message;
  self->invitation_id = init->invitation_id;
  self->is_section_header = FALSE;

  /* Often-empty fields: store NULL to avoid allocations */
  self->description = (init->description && *init->description) ? g_strdup (init->description) : NULL;
  self->user_status = (init->user_status && *init->user_status) ? g_strdup (init->user_status) : NULL;
  self->remote_server = (init->remote_server && *init->remote_server) ? g_strdup (init->remote_server) : NULL;
  self->remote_token = (init->remote_token && *init->remote_token) ? g_strdup (init->remote_token) : NULL;

  /* Raw last message data */
  self->last_message_id = init->last_message_id;
  self->last_message_text = (init->last_message_text && *init->last_message_text) ? g_strdup (init->last_message_text) : NULL;
  self->last_message_actor_id = (init->last_message_actor_id && *init->last_message_actor_id) ? g_strdup (init->last_message_actor_id) : NULL;
  self->last_message_actor_name = (init->last_message_actor_name && *init->last_message_actor_name) ? g_strdup (init->last_message_actor_name) : NULL;
  self->last_message_type = (init->last_message_type && *init->last_message_type) ? (char *) g_intern_string (init->last_message_type) : NULL;
  self->last_message_params = deserialize_message_params (init->last_message_params_json);

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

GnalkConversationData *
gnalk_conversation_data_new_invitation_header (void)
{
  GnalkConversationData *self = g_object_new (GNALK_TYPE_CONVERSATION_DATA, NULL);

  self->title = g_strdup ("Invitations");
  self->is_section_header = TRUE;
  self->invitation_id = 1;  /* Marks this header as belonging to the invitations section */
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
 * The caller must free the returned string.
 * The result is cached and only recomputed when the underlying data changes. */
char *
gnalk_conversation_data_get_subtitle (GnalkConversationData *self,
                                        const char             *current_user_id)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);

  /* Return cached result if available */
  if (self->cached_subtitle)
    return g_strdup (self->cached_subtitle);

  /* If no last message text, return empty */
  if (!self->last_message_text || !*self->last_message_text)
    return g_strdup ("");

  /* Determine if last message is from the current user */
  gboolean is_own_message = (current_user_id && self->last_message_actor_id &&
                             g_strcmp0 (current_user_id, self->last_message_actor_id) == 0);

  /* Build and cache the subtitle (params already parsed — no deserialization needed) */
  self->cached_subtitle = build_subtitle (self->last_message_text,
                                          self->last_message_type,
                                          self->last_message_actor_name,
                                          self->last_message_params,
                                          self->room_type,
                                          is_own_message);

  return g_strdup (self->cached_subtitle);
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

/* Returns a strong reference (transfer full) or NULL if the session is gone —
 * caller must g_object_unref() when done. */
GnalkSession *
gnalk_conversation_data_get_session (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return g_weak_ref_get (&self->session_ref);
}

void
gnalk_conversation_data_set_session (GnalkConversationData *self,
                                       GnalkSession          *session)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_DATA (self));
  g_weak_ref_set (&self->session_ref, session);  /* Weak reference, not owned */
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

const char *
gnalk_conversation_data_get_remote_server (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return self->remote_server;
}

const char *
gnalk_conversation_data_get_remote_token (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return self->remote_token;
}

gboolean
gnalk_conversation_data_is_federated (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), FALSE);
  return self->remote_server && self->remote_server[0] != '\0';
}

void
gnalk_conversation_data_update_from_json (GnalkConversationData *self,
                                            JsonObject             *room_data)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_DATA (self));
  g_return_if_fail (room_data != NULL);

  /* Invalidate cached subtitle */
  g_clear_pointer (&self->cached_subtitle, g_free);

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

      /* Interned — no g_free needed */
      self->last_message_type = (char *) g_intern_string (json_object_get_string_member_with_default (last_message, "messageType", "comment"));

      g_free (self->last_message_actor_name);
      self->last_message_actor_name = g_strdup (json_object_get_string_member_with_default (last_message, "actorDisplayName", ""));

      g_free (self->last_message_actor_id);
      self->last_message_actor_id = g_strdup (json_object_get_string_member_with_default (last_message, "actorId", ""));

      /* Store parsed message parameters directly */
      g_clear_pointer (&self->last_message_params, json_object_unref);
      if (json_object_has_member (last_message, "messageParameters"))
        {
          JsonNode *params_node = json_object_get_member (last_message, "messageParameters");
          if (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
            self->last_message_params = json_object_ref (json_node_get_object (params_node));
        }
    }
  else
    {
      self->last_message_id = 0;
      g_free (self->last_message_text);
      self->last_message_text = NULL;
      /* Interned — no g_free needed */
      self->last_message_type = NULL;
      g_free (self->last_message_actor_name);
      self->last_message_actor_name = NULL;
      g_free (self->last_message_actor_id);
      self->last_message_actor_id = NULL;
      g_clear_pointer (&self->last_message_params, json_object_unref);
    }

  const char *us = json_object_get_string_member_with_default (room_data, "status", "");
  g_free (self->user_status);
  self->user_status = (us && *us) ? g_strdup (us) : NULL;

  const char *rs = json_object_get_string_member_with_default (room_data, "remoteServer", "");
  g_free (self->remote_server);
  self->remote_server = (rs && *rs) ? g_strdup (rs) : NULL;
  const char *rt = json_object_get_string_member_with_default (room_data, "remoteToken", "");
  g_free (self->remote_token);
  self->remote_token = (rt && *rt) ? g_strdup (rt) : NULL;

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

  /* Invalidate cached subtitle */
  g_clear_pointer (&self->cached_subtitle, g_free);

  self->last_activity = last_activity;
  self->last_message_id = last_message_id;

  g_free (self->last_message_text);
  self->last_message_text = (last_message_text && *last_message_text) ? g_strdup (last_message_text) : NULL;

  g_free (self->last_message_actor_id);
  self->last_message_actor_id = (last_message_actor_id && *last_message_actor_id) ? g_strdup (last_message_actor_id) : NULL;

  g_free (self->last_message_actor_name);
  self->last_message_actor_name = (last_message_actor_name && *last_message_actor_name) ? g_strdup (last_message_actor_name) : NULL;

  /* Interned — no g_free needed */
  self->last_message_type = (last_message_type && *last_message_type) ? (char *) g_intern_string (last_message_type) : NULL;

  g_clear_pointer (&self->last_message_params, json_object_unref);
  self->last_message_params = deserialize_message_params (last_message_params_json);

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

  /* Invalidate cached subtitle (will be recomputed on next get_subtitle) */
  g_clear_pointer (&self->cached_subtitle, g_free);

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

  if (g_strcmp0 (self->remote_server, source->remote_server) != 0)
    {
      g_free (self->remote_server);
      self->remote_server = g_strdup (source->remote_server);
      changed = TRUE;
    }

  if (g_strcmp0 (self->remote_token, source->remote_token) != 0)
    {
      g_free (self->remote_token);
      self->remote_token = g_strdup (source->remote_token);
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

  if (self->last_message_type != source->last_message_type)
    {
      /* Interned — pointer comparison suffices, no g_free needed */
      self->last_message_type = source->last_message_type;
      changed = TRUE;
    }

  if (self->last_message_params != source->last_message_params)
    {
      g_clear_pointer (&self->last_message_params, json_object_unref);
      self->last_message_params = source->last_message_params
        ? json_object_ref (source->last_message_params) : NULL;
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

      /* Re-compute thread unread states with updated last_read_message */
      if (self->threads_store)
        {
          gboolean any_unread = FALSE;
          guint n = g_list_model_get_n_items (G_LIST_MODEL (self->threads_store));
          for (guint i = 0; i < n; i++)
            {
              GnalkThreadData *td = g_list_model_get_item (G_LIST_MODEL (self->threads_store), i);
              gboolean unread = gnalk_thread_data_get_last_message_id (td) > self->last_read_message;
              gnalk_thread_data_set_has_unread (td, unread);
              if (unread)
                any_unread = TRUE;
              g_object_unref (td);
            }
          self->has_unread_threads = any_unread;
        }
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

  if (self->invitation_id != source->invitation_id)
    {
      self->invitation_id = source->invitation_id;
      changed = TRUE;
    }

  if (self->last_message_id != source->last_message_id)
    {
      self->last_message_id = source->last_message_id;
      changed = TRUE;
    }

  /* Update session reference (don't count as change for UI purposes) */
  {
    g_autoptr(GnalkSession) src_session = g_weak_ref_get (&source->session_ref);
    g_weak_ref_set (&self->session_ref, src_session);
  }

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

char *
gnalk_conversation_data_get_last_message_params_json (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);
  return gnalk_message_utils_serialize_json_object (self->last_message_params);
}

/* ============ Thread Sub-List ============ */

GListStore *
gnalk_conversation_data_get_threads_store (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), NULL);

  if (!self->threads_store)
    self->threads_store = g_list_store_new (GNALK_TYPE_THREAD_DATA);

  return self->threads_store;
}

void
gnalk_conversation_data_set_threads (GnalkConversationData *self,
                                     GPtrArray             *thread_datas)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_DATA (self));

  GListStore *store = gnalk_conversation_data_get_threads_store (self);

  gboolean changed = FALSE;
  gboolean any_unread = FALSE;

  if (!thread_datas || thread_datas->len == 0)
    {
      /* Remove all threads */
      if (g_list_model_get_n_items (G_LIST_MODEL (store)) > 0)
        {
          g_list_store_remove_all (store);
          changed = TRUE;
        }
    }
  else
    {
      /* Build hash of existing threads by ID for in-place update */
      g_autoptr(GHashTable) existing = g_hash_table_new (g_direct_hash, g_direct_equal);
      guint n_existing = g_list_model_get_n_items (G_LIST_MODEL (store));
      for (guint i = 0; i < n_existing; i++)
        {
          GnalkThreadData *td = g_list_model_get_item (G_LIST_MODEL (store), i);
          g_hash_table_insert (existing,
                               GINT_TO_POINTER ((gint) gnalk_thread_data_get_thread_id (td)),
                               GUINT_TO_POINTER (i));
          g_object_unref (td);
        }

      /* Track which IDs are in the new set */
      g_autoptr(GHashTable) new_ids = g_hash_table_new (g_direct_hash, g_direct_equal);

      for (guint i = 0; i < thread_datas->len; i++)
        {
          GnalkThreadData *new_td = g_ptr_array_index (thread_datas, i);
          gint64 tid = gnalk_thread_data_get_thread_id (new_td);
          g_hash_table_add (new_ids, GINT_TO_POINTER ((gint) tid));

          /* Compute unread: thread's last_message_id > conversation's last_read_message */
          gboolean unread = gnalk_thread_data_get_last_message_id (new_td) > self->last_read_message;
          gnalk_thread_data_set_has_unread (new_td, unread);
          if (unread)
            any_unread = TRUE;

          gpointer idx_ptr;
          if (g_hash_table_lookup_extended (existing, GINT_TO_POINTER ((gint) tid), NULL, &idx_ptr))
            {
              /* Update existing in place */
              guint idx = GPOINTER_TO_UINT (idx_ptr);
              GnalkThreadData *old_td = g_list_model_get_item (G_LIST_MODEL (store), idx);
              if (gnalk_thread_data_update_from_data (old_td, new_td))
                changed = TRUE;
              g_object_unref (old_td);
            }
          else
            {
              /* New thread */
              g_list_store_append (store, new_td);
              changed = TRUE;
            }
        }

      /* Remove threads no longer present (reverse order) */
      for (gint i = (gint) n_existing - 1; i >= 0; i--)
        {
          GnalkThreadData *td = g_list_model_get_item (G_LIST_MODEL (store), i);
          gint64 tid = gnalk_thread_data_get_thread_id (td);
          g_object_unref (td);
          if (!g_hash_table_contains (new_ids, GINT_TO_POINTER ((gint) tid)))
            {
              g_list_store_remove (store, i);
              changed = TRUE;
            }
        }
    }

  /* Also compute any_unread from remaining items (in case we only updated in place) */
  if (!any_unread)
    {
      guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
      for (guint i = 0; i < n; i++)
        {
          GnalkThreadData *td = g_list_model_get_item (G_LIST_MODEL (store), i);
          if (gnalk_thread_data_get_has_unread (td))
            any_unread = TRUE;
          g_object_unref (td);
          if (any_unread)
            break;
        }
    }

  /* Update aggregate flag */
  if (self->has_unread_threads != any_unread)
    {
      self->has_unread_threads = any_unread;
      changed = TRUE;
    }

  if (changed)
    g_signal_emit (self, signals[SIGNAL_UPDATED], 0);
}

gboolean
gnalk_conversation_data_get_has_unread_threads (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), FALSE);
  return self->has_unread_threads;
}

void
gnalk_conversation_data_mark_thread_read (GnalkConversationData *self,
                                          gint64                 thread_id)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_DATA (self));

  if (!self->threads_store)
    return;

  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->threads_store));
  gboolean any_unread = FALSE;

  for (guint i = 0; i < n; i++)
    {
      GnalkThreadData *td = g_list_model_get_item (G_LIST_MODEL (self->threads_store), i);
      if (gnalk_thread_data_get_thread_id (td) == thread_id)
        gnalk_thread_data_set_has_unread (td, FALSE);
      if (gnalk_thread_data_get_has_unread (td))
        any_unread = TRUE;
      g_object_unref (td);
    }

  if (self->has_unread_threads != any_unread)
    {
      self->has_unread_threads = any_unread;
      g_signal_emit (self, signals[SIGNAL_UPDATED], 0);
    }
}

gint64
gnalk_conversation_data_get_invitation_id (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), 0);
  return self->invitation_id;
}

gboolean
gnalk_conversation_data_is_invitation (GnalkConversationData *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_DATA (self), FALSE);
  return self->invitation_id > 0;
}
