#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNALK_TYPE_CONVERSATION_SIDEBAR (gnalk_conversation_sidebar_get_type ())
G_DECLARE_FINAL_TYPE (GnalkConversationSidebar, gnalk_conversation_sidebar, GNALK, CONVERSATION_SIDEBAR, GtkBox)

GnalkConversationSidebar *gnalk_conversation_sidebar_new (void);

void gnalk_conversation_sidebar_set_conversation_model (GnalkConversationSidebar *self,
                                                          GtkSelectionModel         *model,
                                                          GtkListItemFactory        *factory);

void gnalk_conversation_sidebar_set_search_model (GnalkConversationSidebar *self,
                                                    GtkSelectionModel         *model,
                                                    GtkListItemFactory        *factory);

GtkListView *gnalk_conversation_sidebar_get_conversation_list (GnalkConversationSidebar *self);

GtkListView *gnalk_conversation_sidebar_get_search_results_list (GnalkConversationSidebar *self);

void gnalk_conversation_sidebar_show_search_results (GnalkConversationSidebar *self,
                                                       gboolean                   show);

void gnalk_conversation_sidebar_clear_search (GnalkConversationSidebar *self);

const char *gnalk_conversation_sidebar_get_search_text (GnalkConversationSidebar *self);

G_END_DECLS
