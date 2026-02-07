#pragma once

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define GNALK_TYPE_MESSAGE_SEARCH (gnalk_message_search_get_type ())
G_DECLARE_FINAL_TYPE (GnalkMessageSearch, gnalk_message_search, GNALK, MESSAGE_SEARCH, GtkBox)

GnalkMessageSearch *gnalk_message_search_new (void);

/* Clear search state and results */
void gnalk_message_search_clear (GnalkMessageSearch *self);

/* Focus the search entry */
void gnalk_message_search_grab_focus (GnalkMessageSearch *self);

/* Populate results from unified search entries array */
void gnalk_message_search_set_results (GnalkMessageSearch *self,
                                       JsonArray          *entries,
                                       gboolean            has_more,
                                       gint                cursor);

G_END_DECLS
