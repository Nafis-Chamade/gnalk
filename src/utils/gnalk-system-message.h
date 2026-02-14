#pragma once

#include <json-glib/json-glib.h>
#include <glib.h>

G_BEGIN_DECLS

char *gnalk_system_message_format (const char *system_message_type,
                                   const char *actor_name,
                                   gboolean    is_self,
                                   JsonObject *message_parameters);

G_END_DECLS
