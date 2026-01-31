#include "gnalk-conversation-actions.h"
#include "gnalk-markdown.h"
#include <glib/gi18n.h>

struct _GnalkConversationActions
{
  GtkBox parent_instance;

  /* Quote/Reply preview widgets */
  GtkWidget *quote_preview_box;
  GtkWidget *quote_preview_image;
  GtkWidget *quote_preview_author;
  GtkWidget *quote_preview_text;
  GtkWidget *quote_close_button;

  /* Attachment preview widgets (multi-file scrollable) */
  GtkWidget *attachment_preview_box;
  GtkWidget *attachment_scroll;
  GtkWidget *attachment_items_box;
  GtkWidget *attachment_close_button;

  /* State */
  gint64 reply_to_message_id;
  gint64 edit_message_id;
  guint  attachment_count;
};

G_DEFINE_FINAL_TYPE (GnalkConversationActions, gnalk_conversation_actions, GTK_TYPE_BOX)

enum {
  SIGNAL_QUOTE_CLOSED,
  SIGNAL_ATTACHMENT_CLOSED,
  SIGNAL_SCROLL_TO_MESSAGE_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
on_quote_preview_clicked (GtkGestureClick           *gesture,
                          int                        n_press,
                          double                     x,
                          double                     y,
                          GnalkConversationActions *self)
{
  (void) gesture;
  (void) n_press;
  (void) x;
  (void) y;

  /* Only scroll for replies, not edits */
  if (self->reply_to_message_id > 0)
    g_signal_emit (self, signals[SIGNAL_SCROLL_TO_MESSAGE_REQUESTED], 0, self->reply_to_message_id);
}

static void
on_quote_close_clicked (GtkButton                 *button,
                        GnalkConversationActions *self)
{
  (void) button;
  g_signal_emit (self, signals[SIGNAL_QUOTE_CLOSED], 0, self->edit_message_id > 0);
}

static void
on_attachment_close_clicked (GtkButton                 *button,
                             GnalkConversationActions *self)
{
  (void) button;
  g_signal_emit (self, signals[SIGNAL_ATTACHMENT_CLOSED], 0);
}

static void
gnalk_conversation_actions_class_init (GnalkConversationActionsClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  signals[SIGNAL_QUOTE_CLOSED] =
    g_signal_new ("quote-closed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  signals[SIGNAL_ATTACHMENT_CLOSED] =
    g_signal_new ("attachment-closed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_SCROLL_TO_MESSAGE_REQUESTED] =
    g_signal_new ("scroll-to-message-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_INT64);

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-conversation-actions.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationActions, quote_preview_box);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationActions, quote_preview_image);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationActions, quote_preview_author);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationActions, quote_preview_text);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationActions, quote_close_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationActions, attachment_preview_box);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationActions, attachment_scroll);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationActions, attachment_items_box);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationActions, attachment_close_button);
}

static void
gnalk_conversation_actions_init (GnalkConversationActions *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  g_signal_connect (self->quote_close_button, "clicked", G_CALLBACK (on_quote_close_clicked), self);
  g_signal_connect (self->attachment_close_button, "clicked", G_CALLBACK (on_attachment_close_clicked), self);

  /* Add click gesture on quote preview to scroll to quoted message */
  /* TODO: requires two clicks currently - fix during scroll refactor */
  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
  g_signal_connect (click, "pressed", G_CALLBACK (on_quote_preview_clicked), self);
  gtk_widget_add_controller (self->quote_preview_box, GTK_EVENT_CONTROLLER (click));
}

GnalkConversationActions *
gnalk_conversation_actions_new (void)
{
  return g_object_new (GNALK_TYPE_CONVERSATION_ACTIONS, NULL);
}

void
gnalk_conversation_actions_set_reply (GnalkConversationActions *self,
                                        gint64                     message_id,
                                        const char                *author,
                                        const char                *text)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_ACTIONS (self));

  self->reply_to_message_id = message_id;
  self->edit_message_id = 0;

  gtk_label_set_text (GTK_LABEL (self->quote_preview_author), author ? author : "");
  if (text && *text)
    {
      g_autofree char *markup = gnalk_markdown_to_pango (text, NULL);
      gtk_label_set_markup (GTK_LABEL (self->quote_preview_text), markup);
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (self->quote_preview_text), "");
    }

  /* Reset image visibility until set explicitly */
  gtk_widget_set_visible (self->quote_preview_image, FALSE);

  gtk_widget_set_visible (self->quote_preview_box, TRUE);
}

void
gnalk_conversation_actions_set_edit (GnalkConversationActions *self,
                                       gint64                     message_id,
                                       const char                *text)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_ACTIONS (self));

  self->reply_to_message_id = 0;
  self->edit_message_id = message_id;

  gtk_label_set_text (GTK_LABEL (self->quote_preview_author), _("Editing message"));
  if (text && *text)
    {
      g_autofree char *markup = gnalk_markdown_to_pango (text, NULL);
      gtk_label_set_markup (GTK_LABEL (self->quote_preview_text), markup);
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (self->quote_preview_text), "");
    }

  gtk_widget_set_visible (self->quote_preview_image, FALSE);
  gtk_widget_set_visible (self->quote_preview_box, TRUE);
}

void
gnalk_conversation_actions_set_quote_image (GnalkConversationActions *self,
                                              GdkTexture                *texture)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_ACTIONS (self));

  if (texture)
    {
      gtk_picture_set_paintable (GTK_PICTURE (self->quote_preview_image), GDK_PAINTABLE (texture));
      gtk_widget_set_visible (self->quote_preview_image, TRUE);
    }
  else
    {
      gtk_widget_set_visible (self->quote_preview_image, FALSE);
    }
}

void
gnalk_conversation_actions_clear_quote (GnalkConversationActions *self)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_ACTIONS (self));

  self->reply_to_message_id = 0;
  self->edit_message_id = 0;

  gtk_widget_set_visible (self->quote_preview_box, FALSE);
  gtk_widget_set_visible (self->quote_preview_image, FALSE);
  gtk_picture_set_paintable (GTK_PICTURE (self->quote_preview_image), NULL);
}

gint64
gnalk_conversation_actions_get_reply_id (GnalkConversationActions *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_ACTIONS (self), 0);

  return self->reply_to_message_id;
}

gint64
gnalk_conversation_actions_get_edit_id (GnalkConversationActions *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_ACTIONS (self), 0);

  return self->edit_message_id;
}

void
gnalk_conversation_actions_add_attachment (GnalkConversationActions *self,
                                             const char                *file_name,
                                             const char                *icon_name,
                                             GdkTexture                *preview_texture)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_ACTIONS (self));

  self->attachment_count++;

  /* Create item widget for this attachment */
  GtkWidget *item_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class (item_box, "attachment-item");

  if (preview_texture)
    {
      /* Image preview */
      GtkWidget *picture = gtk_picture_new ();
      gtk_picture_set_paintable (GTK_PICTURE (picture), GDK_PAINTABLE (preview_texture));
      gtk_picture_set_can_shrink (GTK_PICTURE (picture), TRUE);
      gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_CONTAIN);
      gtk_widget_set_size_request (picture, 64, 64);
      gtk_widget_add_css_class (picture, "attachment-preview-image");
      gtk_box_append (GTK_BOX (item_box), picture);
    }
  else
    {
      /* Icon for non-image files */
      GtkWidget *icon = gtk_image_new_from_icon_name (icon_name ? icon_name : "text-x-generic-symbolic");
      gtk_image_set_pixel_size (GTK_IMAGE (icon), 48);
      gtk_box_append (GTK_BOX (item_box), icon);
    }

  /* File name label */
  GtkWidget *label = gtk_label_new (file_name ? file_name : "");
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 12);
  gtk_widget_add_css_class (label, "attachment-item-name");
  gtk_box_append (GTK_BOX (item_box), label);

  gtk_box_append (GTK_BOX (self->attachment_items_box), item_box);

  gtk_widget_set_visible (self->attachment_preview_box, TRUE);
}

void
gnalk_conversation_actions_clear_attachments (GnalkConversationActions *self)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_ACTIONS (self));

  self->attachment_count = 0;

  /* Remove all children from items box */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->attachment_items_box)) != NULL)
    gtk_box_remove (GTK_BOX (self->attachment_items_box), child);

  gtk_widget_set_visible (self->attachment_preview_box, FALSE);
}

gboolean
gnalk_conversation_actions_has_attachments (GnalkConversationActions *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_ACTIONS (self), FALSE);

  return self->attachment_count > 0;
}

guint
gnalk_conversation_actions_get_attachment_count (GnalkConversationActions *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_ACTIONS (self), 0);

  return self->attachment_count;
}
