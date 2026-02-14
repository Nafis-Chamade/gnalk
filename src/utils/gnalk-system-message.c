#include "gnalk-system-message.h"
#include <glib/gi18n.h>

static const char *
get_param_name (JsonObject *params, const char *key)
{
  if (!params || !json_object_has_member (params, key))
    return NULL;

  JsonNode *n = json_object_get_member (params, key);
  if (!n || !JSON_NODE_HOLDS_OBJECT (n))
    return NULL;

  JsonObject *obj = json_node_get_object (n);
  return json_object_get_string_member_with_default (obj, "name", NULL);
}

static gboolean
param_equals_actor (JsonObject *params, const char *param_key)
{
  /* Check if a parameter's "id" matches the actor by comparing param key "id" field.
   * Used for user_added/user_removed self-join/leave detection:
   * when the user parameter refers to the same person as the actor. */
  if (!params)
    return FALSE;

  if (!json_object_has_member (params, param_key) ||
      !json_object_has_member (params, "actor"))
    return FALSE;

  JsonNode *pn = json_object_get_member (params, param_key);
  JsonNode *an = json_object_get_member (params, "actor");
  if (!pn || !an || !JSON_NODE_HOLDS_OBJECT (pn) || !JSON_NODE_HOLDS_OBJECT (an))
    return FALSE;

  JsonObject *po = json_node_get_object (pn);
  JsonObject *ao = json_node_get_object (an);
  const char *pid = json_object_get_string_member_with_default (po, "id", "");
  const char *aid = json_object_get_string_member_with_default (ao, "id", "");

  return g_strcmp0 (pid, aid) == 0 && *pid != '\0';
}

static char *
format_call_duration (JsonObject *params)
{
  if (!params || !json_object_has_member (params, "call"))
    return NULL;

  JsonNode *cn = json_object_get_member (params, "call");
  if (!cn || !JSON_NODE_HOLDS_OBJECT (cn))
    return NULL;

  JsonObject *call = json_node_get_object (cn);
  if (!json_object_has_member (call, "duration"))
    return NULL;

  gint64 duration = json_object_get_int_member (call, "duration");
  if (duration <= 0)
    return NULL;

  gint64 minutes = duration / 60;
  gint64 seconds = duration % 60;

  if (minutes > 0)
    return g_strdup_printf ("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT,
                            minutes, seconds);
  else
    return g_strdup_printf ("0:%02" G_GINT64_FORMAT, seconds);
}

char *
gnalk_system_message_format (const char *system_message_type,
                             const char *actor_name,
                             gboolean    is_self,
                             JsonObject *message_parameters)
{
  if (!system_message_type || *system_message_type == '\0')
    return NULL;

  const char *actor = actor_name ? actor_name : "";

  /* --- Conversation --- */

  if (g_strcmp0 (system_message_type, "conversation_created") == 0)
    {
      if (is_self)
        return g_strdup (_("You created the conversation"));
      return g_strdup_printf (_("%s created the conversation"), actor);
    }

  if (g_strcmp0 (system_message_type, "conversation_renamed") == 0)
    {
      const char *old_name = get_param_name (message_parameters, "oldName");
      const char *new_name = get_param_name (message_parameters, "newName");
      if (!old_name) old_name = "?";
      if (!new_name) new_name = "?";

      if (is_self)
        return g_strdup_printf (_("You renamed the conversation from \"%s\" to \"%s\""),
                                old_name, new_name);
      return g_strdup_printf (_("%s renamed the conversation from \"%s\" to \"%s\""),
                              actor, old_name, new_name);
    }

  if (g_strcmp0 (system_message_type, "description_set") == 0)
    {
      if (is_self)
        return g_strdup (_("You set the description"));
      return g_strdup_printf (_("%s set the description"), actor);
    }

  if (g_strcmp0 (system_message_type, "description_removed") == 0)
    {
      if (is_self)
        return g_strdup (_("You removed the description"));
      return g_strdup_printf (_("%s removed the description"), actor);
    }

  /* --- Calls --- */

  if (g_strcmp0 (system_message_type, "call_started") == 0)
    {
      if (is_self)
        return g_strdup (_("You started a call"));
      return g_strdup_printf (_("%s started a call"), actor);
    }

  if (g_strcmp0 (system_message_type, "call_joined") == 0)
    {
      if (is_self)
        return g_strdup (_("You joined the call"));
      return g_strdup_printf (_("%s joined the call"), actor);
    }

  if (g_strcmp0 (system_message_type, "call_left") == 0)
    {
      if (is_self)
        return g_strdup (_("You left the call"));
      return g_strdup_printf (_("%s left the call"), actor);
    }

  if (g_strcmp0 (system_message_type, "call_missed") == 0)
    {
      return g_strdup (_("You missed a call"));
    }

  if (g_strcmp0 (system_message_type, "call_ended") == 0 ||
      g_strcmp0 (system_message_type, "call_ended_everyone") == 0)
    {
      g_autofree char *dur = format_call_duration (message_parameters);
      if (dur)
        return g_strdup_printf (_("Call ended (Duration: %s)"), dur);
      return g_strdup (_("Call ended"));
    }

  /* --- Participants --- */

  if (g_strcmp0 (system_message_type, "user_added") == 0)
    {
      const char *user = get_param_name (message_parameters, "user");
      if (param_equals_actor (message_parameters, "user"))
        {
          if (is_self)
            return g_strdup (_("You joined the conversation"));
          return g_strdup_printf (_("%s joined the conversation"), actor);
        }
      if (is_self)
        return g_strdup_printf (_("You added %s"), user ? user : "?");
      if (user)
        return g_strdup_printf (_("%s added %s"), actor, user);
      return g_strdup_printf (_("%s added a user"), actor);
    }

  if (g_strcmp0 (system_message_type, "user_removed") == 0)
    {
      const char *user = get_param_name (message_parameters, "user");
      if (param_equals_actor (message_parameters, "user"))
        {
          if (is_self)
            return g_strdup (_("You left the conversation"));
          return g_strdup_printf (_("%s left the conversation"), actor);
        }
      if (is_self)
        return g_strdup_printf (_("You removed %s"), user ? user : "?");
      if (user)
        return g_strdup_printf (_("%s removed %s"), actor, user);
      return g_strdup_printf (_("%s removed a user"), actor);
    }

  if (g_strcmp0 (system_message_type, "federated_user_added") == 0)
    {
      const char *user = get_param_name (message_parameters, "federated_user");
      if (is_self)
        return g_strdup_printf (_("You invited %s"), user ? user : "?");
      if (user)
        return g_strdup_printf (_("%s invited %s"), actor, user);
      return g_strdup_printf (_("%s invited a federated user"), actor);
    }

  if (g_strcmp0 (system_message_type, "federated_user_removed") == 0)
    {
      const char *user = get_param_name (message_parameters, "federated_user");
      if (is_self)
        return g_strdup_printf (_("You removed %s"), user ? user : "?");
      if (user)
        return g_strdup_printf (_("%s removed %s"), actor, user);
      return g_strdup_printf (_("%s removed a federated user"), actor);
    }

  if (g_strcmp0 (system_message_type, "group_added") == 0)
    {
      const char *group = get_param_name (message_parameters, "group");
      if (is_self)
        return g_strdup_printf (_("You added group %s"), group ? group : "?");
      return g_strdup_printf (_("%s added group %s"), actor, group ? group : "?");
    }

  if (g_strcmp0 (system_message_type, "group_removed") == 0)
    {
      const char *group = get_param_name (message_parameters, "group");
      if (is_self)
        return g_strdup_printf (_("You removed group %s"), group ? group : "?");
      return g_strdup_printf (_("%s removed group %s"), actor, group ? group : "?");
    }

  if (g_strcmp0 (system_message_type, "circle_added") == 0)
    {
      const char *circle = get_param_name (message_parameters, "circle");
      if (is_self)
        return g_strdup_printf (_("You added team %s"), circle ? circle : "?");
      return g_strdup_printf (_("%s added team %s"), actor, circle ? circle : "?");
    }

  if (g_strcmp0 (system_message_type, "circle_removed") == 0)
    {
      const char *circle = get_param_name (message_parameters, "circle");
      if (is_self)
        return g_strdup_printf (_("You removed team %s"), circle ? circle : "?");
      return g_strdup_printf (_("%s removed team %s"), actor, circle ? circle : "?");
    }

  /* --- Moderator --- */

  if (g_strcmp0 (system_message_type, "moderator_promoted") == 0 ||
      g_strcmp0 (system_message_type, "guest_moderator_promoted") == 0)
    {
      const char *user = get_param_name (message_parameters, "user");
      if (is_self)
        return g_strdup_printf (_("You promoted %s to moderator"), user ? user : "?");
      if (user)
        return g_strdup_printf (_("%s promoted %s to moderator"), actor, user);
      return g_strdup_printf (_("%s promoted a user to moderator"), actor);
    }

  if (g_strcmp0 (system_message_type, "moderator_demoted") == 0 ||
      g_strcmp0 (system_message_type, "guest_moderator_demoted") == 0)
    {
      const char *user = get_param_name (message_parameters, "user");
      if (is_self)
        return g_strdup_printf (_("You demoted %s from moderator"), user ? user : "?");
      if (user)
        return g_strdup_printf (_("%s demoted %s from moderator"), actor, user);
      return g_strdup_printf (_("%s demoted a user from moderator"), actor);
    }

  /* --- Permissions --- */

  if (g_strcmp0 (system_message_type, "read_only") == 0)
    {
      if (is_self)
        return g_strdup (_("You locked the conversation"));
      return g_strdup_printf (_("%s locked the conversation"), actor);
    }

  if (g_strcmp0 (system_message_type, "read_only_off") == 0)
    {
      if (is_self)
        return g_strdup (_("You unlocked the conversation"));
      return g_strdup_printf (_("%s unlocked the conversation"), actor);
    }

  if (g_strcmp0 (system_message_type, "guests_allowed") == 0)
    {
      if (is_self)
        return g_strdup (_("You allowed guests"));
      return g_strdup_printf (_("%s allowed guests"), actor);
    }

  if (g_strcmp0 (system_message_type, "guests_disallowed") == 0)
    {
      if (is_self)
        return g_strdup (_("You disallowed guests"));
      return g_strdup_printf (_("%s disallowed guests"), actor);
    }

  if (g_strcmp0 (system_message_type, "password_set") == 0)
    {
      if (is_self)
        return g_strdup (_("You set a password"));
      return g_strdup_printf (_("%s set a password"), actor);
    }

  if (g_strcmp0 (system_message_type, "password_removed") == 0)
    {
      if (is_self)
        return g_strdup (_("You removed the password"));
      return g_strdup_printf (_("%s removed the password"), actor);
    }

  /* --- Lobby --- */

  if (g_strcmp0 (system_message_type, "lobby_timer_reached") == 0)
    return g_strdup (_("The conversation is now open to everyone"));

  if (g_strcmp0 (system_message_type, "lobby_none") == 0)
    {
      if (is_self)
        return g_strdup (_("You opened the conversation to everyone"));
      return g_strdup_printf (_("%s opened the conversation to everyone"), actor);
    }

  if (g_strcmp0 (system_message_type, "lobby_non_moderators") == 0)
    {
      if (is_self)
        return g_strdup (_("You restricted the conversation to moderators"));
      return g_strdup_printf (_("%s restricted the conversation to moderators"), actor);
    }

  /* --- Visibility --- */

  if (g_strcmp0 (system_message_type, "listable_none") == 0)
    {
      if (is_self)
        return g_strdup (_("You limited the conversation to the current participants"));
      return g_strdup_printf (_("%s limited the conversation to the current participants"), actor);
    }

  if (g_strcmp0 (system_message_type, "listable_users") == 0)
    {
      if (is_self)
        return g_strdup (_("You opened the conversation to registered users"));
      return g_strdup_printf (_("%s opened the conversation to registered users"), actor);
    }

  if (g_strcmp0 (system_message_type, "listable_all") == 0)
    {
      if (is_self)
        return g_strdup (_("You opened the conversation to everyone"));
      return g_strdup_printf (_("%s opened the conversation to everyone"), actor);
    }

  /* --- Messages --- */

  if (g_strcmp0 (system_message_type, "message_deleted") == 0)
    {
      if (is_self)
        return g_strdup (_("You deleted a message"));
      return g_strdup_printf (_("%s deleted a message"), actor);
    }

  if (g_strcmp0 (system_message_type, "message_edited") == 0)
    {
      if (is_self)
        return g_strdup (_("You edited a message"));
      return g_strdup_printf (_("%s edited a message"), actor);
    }

  if (g_strcmp0 (system_message_type, "message_pinned") == 0)
    {
      if (is_self)
        return g_strdup (_("You pinned a message"));
      return g_strdup_printf (_("%s pinned a message"), actor);
    }

  if (g_strcmp0 (system_message_type, "message_unpinned") == 0)
    {
      if (is_self)
        return g_strdup (_("You unpinned a message"));
      return g_strdup_printf (_("%s unpinned a message"), actor);
    }

  if (g_strcmp0 (system_message_type, "reaction_revoked") == 0)
    {
      if (is_self)
        return g_strdup (_("You deleted a reaction"));
      return g_strdup_printf (_("%s deleted a reaction"), actor);
    }

  if (g_strcmp0 (system_message_type, "history_cleared") == 0)
    {
      if (is_self)
        return g_strdup (_("You cleared the history of the conversation"));
      return g_strdup_printf (_("%s cleared the history of the conversation"), actor);
    }

  /* --- Message expiration --- */

  if (g_strcmp0 (system_message_type, "message_expiration_enabled") == 0)
    {
      if (is_self)
        return g_strdup (_("You enabled message expiration"));
      return g_strdup_printf (_("%s enabled message expiration"), actor);
    }

  if (g_strcmp0 (system_message_type, "message_expiration_disabled") == 0)
    {
      if (is_self)
        return g_strdup (_("You disabled message expiration"));
      return g_strdup_printf (_("%s disabled message expiration"), actor);
    }

  /* --- Avatar --- */

  if (g_strcmp0 (system_message_type, "avatar_set") == 0)
    {
      if (is_self)
        return g_strdup (_("You set the conversation picture"));
      return g_strdup_printf (_("%s set the conversation picture"), actor);
    }

  if (g_strcmp0 (system_message_type, "avatar_removed") == 0)
    {
      if (is_self)
        return g_strdup (_("You removed the conversation picture"));
      return g_strdup_printf (_("%s removed the conversation picture"), actor);
    }

  /* --- Recording --- */

  if (g_strcmp0 (system_message_type, "recording_started") == 0)
    {
      if (is_self)
        return g_strdup (_("You started the recording"));
      return g_strdup_printf (_("%s started the recording"), actor);
    }

  if (g_strcmp0 (system_message_type, "recording_stopped") == 0)
    {
      if (is_self)
        return g_strdup (_("You stopped the recording"));
      return g_strdup_printf (_("%s stopped the recording"), actor);
    }

  if (g_strcmp0 (system_message_type, "audio_recording_started") == 0)
    {
      if (is_self)
        return g_strdup (_("You started the audio recording"));
      return g_strdup_printf (_("%s started the audio recording"), actor);
    }

  if (g_strcmp0 (system_message_type, "audio_recording_stopped") == 0)
    {
      if (is_self)
        return g_strdup (_("You stopped the audio recording"));
      return g_strdup_printf (_("%s stopped the audio recording"), actor);
    }

  if (g_strcmp0 (system_message_type, "recording_failed") == 0)
    return g_strdup (_("The recording failed"));

  /* --- Breakout rooms --- */

  if (g_strcmp0 (system_message_type, "breakout_rooms_started") == 0)
    {
      if (is_self)
        return g_strdup (_("You started breakout rooms"));
      return g_strdup_printf (_("%s started breakout rooms"), actor);
    }

  if (g_strcmp0 (system_message_type, "breakout_rooms_stopped") == 0)
    {
      if (is_self)
        return g_strdup (_("You stopped breakout rooms"));
      return g_strdup_printf (_("%s stopped breakout rooms"), actor);
    }

  /* --- Polls --- */

  if (g_strcmp0 (system_message_type, "poll_voted") == 0)
    {
      const char *poll = get_param_name (message_parameters, "poll");
      if (poll)
        return g_strdup_printf (_("Someone voted on the poll \"%s\""), poll);
      return g_strdup (_("Someone voted on a poll"));
    }

  if (g_strcmp0 (system_message_type, "poll_closed") == 0)
    {
      const char *poll = get_param_name (message_parameters, "poll");
      if (is_self)
        {
          if (poll)
            return g_strdup_printf (_("You ended the poll \"%s\""), poll);
          return g_strdup (_("You ended a poll"));
        }
      if (poll)
        return g_strdup_printf (_("%s ended the poll \"%s\""), actor, poll);
      return g_strdup_printf (_("%s ended a poll"), actor);
    }

  /* --- Matterbridge --- */

  if (g_strcmp0 (system_message_type, "matterbridge_config_added") == 0)
    {
      if (is_self)
        return g_strdup (_("You set up Matterbridge to synchronize this conversation with other chats"));
      return g_strdup_printf (_("%s set up Matterbridge to synchronize this conversation with other chats"), actor);
    }

  if (g_strcmp0 (system_message_type, "matterbridge_config_edited") == 0)
    {
      if (is_self)
        return g_strdup (_("You updated the Matterbridge configuration"));
      return g_strdup_printf (_("%s updated the Matterbridge configuration"), actor);
    }

  if (g_strcmp0 (system_message_type, "matterbridge_config_removed") == 0)
    {
      if (is_self)
        return g_strdup (_("You removed the Matterbridge configuration"));
      return g_strdup_printf (_("%s removed the Matterbridge configuration"), actor);
    }

  if (g_strcmp0 (system_message_type, "matterbridge_config_enabled") == 0)
    {
      if (is_self)
        return g_strdup (_("You started Matterbridge"));
      return g_strdup_printf (_("%s started Matterbridge"), actor);
    }

  if (g_strcmp0 (system_message_type, "matterbridge_config_disabled") == 0)
    {
      if (is_self)
        return g_strdup (_("You stopped Matterbridge"));
      return g_strdup_printf (_("%s stopped Matterbridge"), actor);
    }

  /* Unknown type — return NULL so caller uses server text as fallback */
  return NULL;
}
