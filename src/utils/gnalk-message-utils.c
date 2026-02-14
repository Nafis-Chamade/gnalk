#include "gnalk-message-utils.h"

/* Replace all occurrences of needle in haystack with replacement.
 * Uses strstr for literal matching — no regex overhead. */
static char *
replace_literal (const char *haystack,
                 const char *needle,
                 const char *replacement)
{
  GString *result = g_string_new (NULL);
  const char *p = haystack;
  gsize needle_len = strlen (needle);
  const char *found;

  while ((found = strstr (p, needle)))
    {
      g_string_append_len (result, p, found - p);
      g_string_append (result, replacement);
      p = found + needle_len;
    }
  g_string_append (result, p);

  return g_string_free (result, FALSE);
}

char *
gnalk_message_utils_replace_parameters (const char *message,
                                        JsonObject *params,
                                        const char *actor_name)
{
  if (!message || !*message)
    return g_strdup ("");

  char *result = g_strdup (message);

  /* Replace {actor} with actor name */
  if (actor_name && *actor_name)
    {
      char *tmp = result;
      result = replace_literal (tmp, "{actor}", actor_name);
      g_free (tmp);
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
              const char *name = json_object_get_string_member_with_default (
                  param_obj, "name", NULL);

              if (name)
                {
                  g_autofree char *needle = g_strdup_printf ("{%s}", param_key);
                  char *tmp = result;
                  result = replace_literal (tmp, needle, name);
                  g_free (tmp);
                }
            }
        }
    }

  return result;
}

char *
gnalk_message_utils_serialize_json_object (JsonObject *obj)
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
