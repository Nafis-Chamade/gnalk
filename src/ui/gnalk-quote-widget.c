/* gnalk-quote-widget.c
 *
 * Widget displaying a quoted/replied-to message.
 */

#include "gnalk-quote-widget.h"
#include "gnalk-markdown.h"

struct _GnalkQuoteWidget
{
  GtkBox parent_instance;

  GtkWidget *avatar;
  GtkWidget *username_label;
  GtkWidget *file_preview;
  GtkWidget *message_label;

  gint64 parent_message_id;
};

G_DEFINE_FINAL_TYPE (GnalkQuoteWidget, gnalk_quote_widget, GTK_TYPE_BOX)

enum {
  SIGNAL_SCROLL_TO_MESSAGE_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Weak reference wrapper for async avatar callbacks */
typedef struct
{
  GWeakRef weak_ref;
  gboolean is_file_preview;
} QuoteCallbackData;

static QuoteCallbackData *
quote_callback_data_new (GnalkQuoteWidget *self, gboolean is_file_preview)
{
  QuoteCallbackData *data = g_new0 (QuoteCallbackData, 1);
  g_weak_ref_init (&data->weak_ref, self);
  data->is_file_preview = is_file_preview;
  return data;
}

static void
on_avatar_loaded (GnalkAvatarCache *cache G_GNUC_UNUSED,
                  GdkTexture        *texture,
                  gpointer           user_data)
{
  QuoteCallbackData *data = user_data;
  GnalkQuoteWidget *self = g_weak_ref_get (&data->weak_ref);

  if (self)
    {
      if (data->is_file_preview)
        {
          if (texture)
            {
              gtk_picture_set_paintable (GTK_PICTURE (self->file_preview), GDK_PAINTABLE (texture));
              gtk_widget_set_visible (self->file_preview, TRUE);
            }
          else
            {
              gtk_widget_set_visible (self->file_preview, FALSE);
            }
        }
      else
        {
          if (texture)
            gtk_image_set_from_paintable (GTK_IMAGE (self->avatar), GDK_PAINTABLE (texture));
          else
            gtk_image_set_from_icon_name (GTK_IMAGE (self->avatar), "avatar-default-symbolic");
        }
      g_object_unref (self);
    }

  g_weak_ref_clear (&data->weak_ref);
  g_free (data);
}

static void
on_quote_clicked (GtkGestureClick  *gesture,
                  int               n_press G_GNUC_UNUSED,
                  double            x G_GNUC_UNUSED,
                  double            y G_GNUC_UNUSED,
                  gpointer          user_data)
{
  GnalkQuoteWidget *self = GNALK_QUOTE_WIDGET (user_data);

  if (self->parent_message_id > 0)
    {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
      g_signal_emit (self, signals[SIGNAL_SCROLL_TO_MESSAGE_REQUESTED], 0, self->parent_message_id);
    }
}

static void
gnalk_quote_widget_class_init (GnalkQuoteWidgetClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/net/skilpa/gnalk/gnalk-quote-widget.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkQuoteWidget, avatar);
  gtk_widget_class_bind_template_child (widget_class, GnalkQuoteWidget, username_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkQuoteWidget, file_preview);
  gtk_widget_class_bind_template_child (widget_class, GnalkQuoteWidget, message_label);

  /* Signal emitted when user clicks to scroll to the quoted message */
  signals[SIGNAL_SCROLL_TO_MESSAGE_REQUESTED] =
    g_signal_new ("scroll-to-message-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT64);   /* message_id */
}

static void
gnalk_quote_widget_init (GnalkQuoteWidget *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  /* Add click gesture for scrolling to quoted message */
  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click), GTK_PHASE_CAPTURE);
  g_signal_connect (click, "pressed", G_CALLBACK (on_quote_clicked), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

GnalkQuoteWidget *
gnalk_quote_widget_new (void)
{
  return g_object_new (GNALK_TYPE_QUOTE_WIDGET, NULL);
}

void
gnalk_quote_widget_set_quote (GnalkQuoteWidget *self,
                               gint64             parent_id,
                               const char        *author_id,
                               const char        *author_name,
                               const char        *message,
                               GnalkAvatarCache *avatar_cache)
{
  g_return_if_fail (GNALK_IS_QUOTE_WIDGET (self));

  if (!author_name || !*author_name)
    {
      gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
      return;
    }

  self->parent_message_id = parent_id;

  gtk_label_set_text (GTK_LABEL (self->username_label), author_name);
  if (message && *message)
    {
      g_autofree char *markup = gnalk_markdown_to_pango (message, NULL);
      gtk_label_set_markup (GTK_LABEL (self->message_label), markup);
      gtk_widget_set_visible (self->message_label, TRUE);
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (self->message_label), "");
      gtk_widget_set_visible (self->message_label, FALSE);
    }
  gtk_widget_set_visible (self->file_preview, FALSE);

  /* Load author avatar */
  if (avatar_cache && author_id && *author_id)
    gnalk_avatar_cache_get_user_avatar (avatar_cache, author_id, 32, on_avatar_loaded,
                                         quote_callback_data_new (self, FALSE));

  gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
}

void
gnalk_quote_widget_set_file_preview (GnalkQuoteWidget *self,
                                      const char        *file_id,
                                      GnalkAvatarCache *avatar_cache)
{
  g_return_if_fail (GNALK_IS_QUOTE_WIDGET (self));

  if (!file_id || !*file_id || !avatar_cache)
    {
      gtk_widget_set_visible (self->file_preview, FALSE);
      return;
    }

  gnalk_avatar_cache_get_file_preview (avatar_cache, file_id, 64, 64,
                                        on_avatar_loaded,
                                        quote_callback_data_new (self, TRUE));
}

void
gnalk_quote_widget_reset (GnalkQuoteWidget *self)
{
  g_return_if_fail (GNALK_IS_QUOTE_WIDGET (self));

  self->parent_message_id = 0;
  gtk_label_set_text (GTK_LABEL (self->username_label), "");
  gtk_label_set_text (GTK_LABEL (self->message_label), "");
  gtk_picture_set_paintable (GTK_PICTURE (self->file_preview), NULL);
  gtk_widget_set_visible (self->file_preview, FALSE);
  gtk_image_set_from_icon_name (GTK_IMAGE (self->avatar), "avatar-default-symbolic");
  gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
}
