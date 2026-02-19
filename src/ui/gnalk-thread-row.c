#include "gnalk-thread-row.h"

struct _GnalkThreadRow
{
  GtkBox parent_instance;

  GtkWidget *thread_icon;
  GtkWidget *title_label;
  GtkWidget *subtitle_label;
  GtkWidget *unread_dot;

  GnalkThreadData *bound_data;
  gulong           updated_handler_id;
};

G_DEFINE_FINAL_TYPE (GnalkThreadRow, gnalk_thread_row, GTK_TYPE_BOX)

static void
update_from_data (GnalkThreadRow  *self,
                  GnalkThreadData *data)
{
  const char *title = gnalk_thread_data_get_title (data);
  gtk_label_set_text (GTK_LABEL (self->title_label), title ? title : "");

  gint num_replies = gnalk_thread_data_get_num_replies (data);
  const char *last_text = gnalk_thread_data_get_last_message_text (data);

  g_autofree char *subtitle = NULL;
  if (last_text && *last_text)
    {
      const char *actor = gnalk_thread_data_get_last_message_actor_name (data);
      if (actor && *actor)
        subtitle = g_strdup_printf ("%d replies \302\267 %s: %s", num_replies, actor, last_text);
      else
        subtitle = g_strdup_printf ("%d replies \302\267 %s", num_replies, last_text);
    }
  else
    subtitle = g_strdup_printf ("%d replies", num_replies);

  gtk_label_set_text (GTK_LABEL (self->subtitle_label), subtitle);

  gboolean has_unread = gnalk_thread_data_get_has_unread (data);
  gtk_widget_set_visible (self->unread_dot, has_unread);

  if (has_unread)
    gtk_widget_add_css_class (GTK_WIDGET (self), "thread-has-unread");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "thread-has-unread");
}

static void
on_data_updated (GnalkThreadData *data,
                 GnalkThreadRow  *self)
{
  update_from_data (self, data);
}

static void
gnalk_thread_row_dispose (GObject *object)
{
  GnalkThreadRow *self = GNALK_THREAD_ROW (object);

  if (self->bound_data && self->updated_handler_id > 0)
    {
      g_signal_handler_disconnect (self->bound_data, self->updated_handler_id);
      self->updated_handler_id = 0;
    }
  g_clear_object (&self->bound_data);

  G_OBJECT_CLASS (gnalk_thread_row_parent_class)->dispose (object);
}

static void
gnalk_thread_row_class_init (GnalkThreadRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnalk_thread_row_dispose;

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-thread-row.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkThreadRow, thread_icon);
  gtk_widget_class_bind_template_child (widget_class, GnalkThreadRow, title_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkThreadRow, subtitle_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkThreadRow, unread_dot);
}

static void
gnalk_thread_row_init (GnalkThreadRow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

GnalkThreadRow *
gnalk_thread_row_new (void)
{
  return g_object_new (GNALK_TYPE_THREAD_ROW, NULL);
}

void
gnalk_thread_row_set_from_data (GnalkThreadRow  *self,
                                GnalkThreadData *data)
{
  g_return_if_fail (GNALK_IS_THREAD_ROW (self));
  g_return_if_fail (GNALK_IS_THREAD_DATA (data));

  /* Disconnect from previous */
  if (self->bound_data && self->updated_handler_id > 0)
    {
      g_signal_handler_disconnect (self->bound_data, self->updated_handler_id);
      self->updated_handler_id = 0;
    }
  g_clear_object (&self->bound_data);

  self->bound_data = g_object_ref (data);
  self->updated_handler_id = g_signal_connect (data, "updated",
                                                G_CALLBACK (on_data_updated), self);

  update_from_data (self, data);
}

void
gnalk_thread_row_reset (GnalkThreadRow *self)
{
  g_return_if_fail (GNALK_IS_THREAD_ROW (self));

  if (self->bound_data && self->updated_handler_id > 0)
    {
      g_signal_handler_disconnect (self->bound_data, self->updated_handler_id);
      self->updated_handler_id = 0;
    }
  g_clear_object (&self->bound_data);

  gtk_label_set_text (GTK_LABEL (self->title_label), "");
  gtk_label_set_text (GTK_LABEL (self->subtitle_label), "");
  gtk_widget_set_visible (self->unread_dot, FALSE);
  gtk_widget_remove_css_class (GTK_WIDGET (self), "thread-has-unread");
}
