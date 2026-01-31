#include "gnalk-conversation-sidebar.h"
#include <glib/gi18n.h>

struct _GnalkConversationSidebar
{
  GtkBox parent_instance;

  GtkWidget *search_entry;
  GtkWidget *new_conversation_button;
  GtkWidget *search_results_revealer;
  GtkWidget *search_results_list;
  GtkWidget *conversation_list;

  guint search_debounce_timer_id;
};

G_DEFINE_FINAL_TYPE (GnalkConversationSidebar, gnalk_conversation_sidebar, GTK_TYPE_BOX)

enum
{
  SIGNAL_CONVERSATION_SELECTED,
  SIGNAL_SEARCH_RESULT_SELECTED,
  SIGNAL_SEARCH_CHANGED,
  SIGNAL_NEW_CONVERSATION_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
on_new_conversation_clicked (GtkButton                 *button G_GNUC_UNUSED,
                              GnalkConversationSidebar *self)
{
  g_signal_emit (self, signals[SIGNAL_NEW_CONVERSATION_REQUESTED], 0);
}

static gboolean
on_search_debounce_timeout (gpointer user_data)
{
  GnalkConversationSidebar *self = GNALK_CONVERSATION_SIDEBAR (user_data);
  self->search_debounce_timer_id = 0;

  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
  g_signal_emit (self, signals[SIGNAL_SEARCH_CHANGED], 0, text);

  return G_SOURCE_REMOVE;
}

static void
on_search_entry_changed (GtkSearchEntry           *entry,
                          GnalkConversationSidebar *self)
{
  (void) entry;

  /* Cancel pending debounce */
  if (self->search_debounce_timer_id)
    {
      g_source_remove (self->search_debounce_timer_id);
      self->search_debounce_timer_id = 0;
    }

  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
  if (!text || !*text)
    {
      /* Clear search immediately when empty */
      gtk_revealer_set_reveal_child (GTK_REVEALER (self->search_results_revealer), FALSE);
      return;
    }

  /* Debounce search requests */
  self->search_debounce_timer_id = g_timeout_add (300, on_search_debounce_timeout, self);
}

static void
on_conversation_activated (GtkListView              *list,
                            guint                     position,
                            GnalkConversationSidebar *self)
{
  (void) list;
  g_signal_emit (self, signals[SIGNAL_CONVERSATION_SELECTED], 0, position);
}

static void
on_search_result_activated (GtkListView              *list,
                             guint                     position,
                             GnalkConversationSidebar *self)
{
  (void) list;
  g_signal_emit (self, signals[SIGNAL_SEARCH_RESULT_SELECTED], 0, position);
}

static void
gnalk_conversation_sidebar_dispose (GObject *object)
{
  GnalkConversationSidebar *self = GNALK_CONVERSATION_SIDEBAR (object);

  if (self->search_debounce_timer_id)
    {
      g_source_remove (self->search_debounce_timer_id);
      self->search_debounce_timer_id = 0;
    }

  G_OBJECT_CLASS (gnalk_conversation_sidebar_parent_class)->dispose (object);
}

static void
gnalk_conversation_sidebar_class_init (GnalkConversationSidebarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnalk_conversation_sidebar_dispose;

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-conversation-sidebar.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationSidebar, search_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationSidebar, new_conversation_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationSidebar, search_results_revealer);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationSidebar, search_results_list);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationSidebar, conversation_list);

  signals[SIGNAL_CONVERSATION_SELECTED] =
    g_signal_new ("conversation-selected",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1, G_TYPE_UINT);

  signals[SIGNAL_SEARCH_RESULT_SELECTED] =
    g_signal_new ("search-result-selected",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1, G_TYPE_UINT);

  signals[SIGNAL_SEARCH_CHANGED] =
    g_signal_new ("search-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_NEW_CONVERSATION_REQUESTED] =
    g_signal_new ("new-conversation-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);
}

static void
gnalk_conversation_sidebar_init (GnalkConversationSidebar *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  g_signal_connect (self->search_entry, "search-changed",
                    G_CALLBACK (on_search_entry_changed), self);
  g_signal_connect (self->conversation_list, "activate",
                    G_CALLBACK (on_conversation_activated), self);
  g_signal_connect (self->search_results_list, "activate",
                    G_CALLBACK (on_search_result_activated), self);
  g_signal_connect (self->new_conversation_button, "clicked",
                    G_CALLBACK (on_new_conversation_clicked), self);
}

GnalkConversationSidebar *
gnalk_conversation_sidebar_new (void)
{
  return g_object_new (GNALK_TYPE_CONVERSATION_SIDEBAR, NULL);
}

void
gnalk_conversation_sidebar_set_conversation_model (GnalkConversationSidebar *self,
                                                     GtkSelectionModel         *model,
                                                     GtkListItemFactory        *factory)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_SIDEBAR (self));

  gtk_list_view_set_model (GTK_LIST_VIEW (self->conversation_list), model);
  if (factory)
    gtk_list_view_set_factory (GTK_LIST_VIEW (self->conversation_list), factory);
}

void
gnalk_conversation_sidebar_set_search_model (GnalkConversationSidebar *self,
                                               GtkSelectionModel         *model,
                                               GtkListItemFactory        *factory)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_SIDEBAR (self));

  gtk_list_view_set_model (GTK_LIST_VIEW (self->search_results_list), model);
  if (factory)
    gtk_list_view_set_factory (GTK_LIST_VIEW (self->search_results_list), factory);
}

GtkListView *
gnalk_conversation_sidebar_get_conversation_list (GnalkConversationSidebar *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_SIDEBAR (self), NULL);

  return GTK_LIST_VIEW (self->conversation_list);
}

GtkListView *
gnalk_conversation_sidebar_get_search_results_list (GnalkConversationSidebar *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_SIDEBAR (self), NULL);

  return GTK_LIST_VIEW (self->search_results_list);
}

void
gnalk_conversation_sidebar_show_search_results (GnalkConversationSidebar *self,
                                                  gboolean                   show)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_SIDEBAR (self));

  gtk_revealer_set_reveal_child (GTK_REVEALER (self->search_results_revealer), show);
}

void
gnalk_conversation_sidebar_clear_search (GnalkConversationSidebar *self)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_SIDEBAR (self));

  gtk_editable_set_text (GTK_EDITABLE (self->search_entry), "");
  gtk_revealer_set_reveal_child (GTK_REVEALER (self->search_results_revealer), FALSE);
}

const char *
gnalk_conversation_sidebar_get_search_text (GnalkConversationSidebar *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_SIDEBAR (self), NULL);

  return gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
}
