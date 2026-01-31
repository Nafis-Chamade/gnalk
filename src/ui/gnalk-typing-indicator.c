#include "gnalk-typing-indicator.h"

#define TYPING_HIDE_TIMEOUT_SEC 15

struct _GnalkTypingIndicator
{
  GtkBox parent_instance;

  GtkWidget *label;

  GHashTable *typing_participants;  /* session_id -> display_name */
  GHashTable *typing_timeouts;      /* session_id -> timeout_id (GUINT_TO_POINTER) */
};

G_DEFINE_FINAL_TYPE (GnalkTypingIndicator, gnalk_typing_indicator, GTK_TYPE_BOX)

typedef struct {
  GnalkTypingIndicator *indicator;
  char *session_id;
} TypingTimeoutData;

static void
typing_timeout_data_free (gpointer data)
{
  TypingTimeoutData *td = data;
  g_free (td->session_id);
  g_free (td);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (TypingTimeoutData, typing_timeout_data_free)

static void
update_label (GnalkTypingIndicator *self)
{
  guint count = g_hash_table_size (self->typing_participants);

  if (count == 0)
    {
      /* Set to space to maintain height but show nothing */
      gtk_label_set_text (GTK_LABEL (self->label), " ");
      return;
    }

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, self->typing_participants);

  GString *text = g_string_new (NULL);
  guint i = 0;

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *name = value;
      if (!name || !*name)
        name = "Someone";

      if (i > 0)
        {
          if (i == count - 1)
            g_string_append (text, " and ");
          else
            g_string_append (text, ", ");
        }
      g_string_append (text, name);
      i++;

      /* Limit to 3 names */
      if (i >= 3 && count > 3)
        {
          g_string_append_printf (text, " and %u other%s",
                                  count - 3,
                                  count - 3 > 1 ? "s" : "");
          break;
        }
    }

  if (count == 1)
    g_string_append (text, " is typing...");
  else
    g_string_append (text, " are typing...");

  gtk_label_set_text (GTK_LABEL (self->label), text->str);

  g_string_free (text, TRUE);
}

static gboolean
on_typing_hide_timeout (gpointer user_data)
{
  TypingTimeoutData *td = user_data;
  GnalkTypingIndicator *self = td->indicator;

  g_hash_table_remove (self->typing_timeouts, td->session_id);
  g_hash_table_remove (self->typing_participants, td->session_id);
  update_label (self);

  return G_SOURCE_REMOVE;
}

static void
gnalk_typing_indicator_dispose (GObject *object)
{
  GnalkTypingIndicator *self = GNALK_TYPING_INDICATOR (object);

  /* Remove all pending timeouts */
  if (self->typing_timeouts)
    {
      GHashTableIter iter;
      gpointer key, value;

      g_hash_table_iter_init (&iter, self->typing_timeouts);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          guint timeout_id = GPOINTER_TO_UINT (value);
          if (timeout_id > 0)
            g_source_remove (timeout_id);
        }
    }

  G_OBJECT_CLASS (gnalk_typing_indicator_parent_class)->dispose (object);
}

static void
gnalk_typing_indicator_finalize (GObject *object)
{
  GnalkTypingIndicator *self = GNALK_TYPING_INDICATOR (object);

  g_clear_pointer (&self->typing_participants, g_hash_table_unref);
  g_clear_pointer (&self->typing_timeouts, g_hash_table_unref);

  G_OBJECT_CLASS (gnalk_typing_indicator_parent_class)->finalize (object);
}

static void
gnalk_typing_indicator_class_init (GnalkTypingIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnalk_typing_indicator_dispose;
  object_class->finalize = gnalk_typing_indicator_finalize;

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-typing-indicator.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkTypingIndicator, label);
}

static void
gnalk_typing_indicator_init (GnalkTypingIndicator *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->typing_participants = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->typing_timeouts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

GnalkTypingIndicator *
gnalk_typing_indicator_new (void)
{
  return g_object_new (GNALK_TYPE_TYPING_INDICATOR, NULL);
}

void
gnalk_typing_indicator_set_typing (GnalkTypingIndicator *self,
                                     const char            *session_id,
                                     const char            *display_name,
                                     gboolean               is_typing)
{
  g_return_if_fail (GNALK_IS_TYPING_INDICATOR (self));

  if (!session_id)
    return;

  /* Remove existing timeout if any */
  gpointer existing = g_hash_table_lookup (self->typing_timeouts, session_id);
  if (existing)
    {
      guint existing_timeout = GPOINTER_TO_UINT (existing);
      if (existing_timeout > 0)
        g_source_remove (existing_timeout);
      g_hash_table_remove (self->typing_timeouts, session_id);
    }

  if (is_typing)
    {
      /* Add/update typing participant with a new timeout */
      TypingTimeoutData *td = g_new0 (TypingTimeoutData, 1);
      td->indicator = self;
      td->session_id = g_strdup (session_id);

      guint timeout_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
                                                      TYPING_HIDE_TIMEOUT_SEC,
                                                      on_typing_hide_timeout,
                                                      td,
                                                      typing_timeout_data_free);

      /* Store display_name in typing_participants */
      g_hash_table_insert (self->typing_participants,
                           g_strdup (session_id),
                           g_strdup (display_name && *display_name ? display_name : "Someone"));

      /* Store timeout_id in typing_timeouts */
      g_hash_table_insert (self->typing_timeouts,
                           g_strdup (session_id),
                           GUINT_TO_POINTER (timeout_id));
    }
  else
    {
      g_hash_table_remove (self->typing_timeouts, session_id);
      g_hash_table_remove (self->typing_participants, session_id);
    }

  update_label (self);
}

void
gnalk_typing_indicator_clear (GnalkTypingIndicator *self)
{
  g_return_if_fail (GNALK_IS_TYPING_INDICATOR (self));

  /* Remove all pending timeouts */
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, self->typing_timeouts);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      guint timeout_id = GPOINTER_TO_UINT (value);
      if (timeout_id > 0)
        g_source_remove (timeout_id);
    }

  g_hash_table_remove_all (self->typing_timeouts);
  g_hash_table_remove_all (self->typing_participants);
  gtk_label_set_text (GTK_LABEL (self->label), " ");
}
