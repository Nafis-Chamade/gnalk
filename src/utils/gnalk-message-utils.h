#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Replace message parameters like {actor}, {user1}, {file} with their names.
 * Uses literal string replacement (strstr), no regex.
 * Returns a newly allocated string. */
char *gnalk_message_utils_replace_parameters (const char *message,
                                              JsonObject *params,
                                              const char *actor_name);

/* Serialize a JsonObject to a JSON string.
 * Returns NULL if obj is NULL. Caller must free with g_free(). */
char *gnalk_message_utils_serialize_json_object (JsonObject *obj);

G_END_DECLS
