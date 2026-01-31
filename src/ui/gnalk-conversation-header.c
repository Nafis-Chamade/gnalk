#include "gnalk-conversation-header.h"

struct _GnalkConversationHeader
{
  GtkBox parent_instance;

  GtkWidget *room_header_button;
  GtkWidget *room_avatar;
  GtkWidget *room_status_icon;
  GtkWidget *room_call_icon;
  GtkWidget *room_title;

  char *room_token;
  gint  room_type;
};

G_DEFINE_FINAL_TYPE (GnalkConversationHeader, gnalk_conversation_header, GTK_TYPE_BOX)

enum {
  SIGNAL_INFO_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
on_header_button_clicked (GtkButton                *button,
                          GnalkConversationHeader *self)
{
  (void) button;
  g_signal_emit (self, signals[SIGNAL_INFO_REQUESTED], 0);
}

static void
update_status_icon (GnalkConversationHeader *self,
                    const char               *user_status)
{
  /* Only show status for 1:1 conversations (type 1) */
  if (self->room_type != 1 || !user_status || !*user_status)
    {
      gtk_widget_set_visible (self->room_status_icon, FALSE);
      return;
    }

  /* Remove previous status classes */
  gtk_widget_remove_css_class (self->room_status_icon, "status-online");
  gtk_widget_remove_css_class (self->room_status_icon, "status-away");
  gtk_widget_remove_css_class (self->room_status_icon, "status-dnd");

  if (g_strcmp0 (user_status, "online") == 0)
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (self->room_status_icon), "user-available-symbolic");
      gtk_widget_add_css_class (self->room_status_icon, "status-online");
      gtk_widget_set_visible (self->room_status_icon, TRUE);
    }
  else if (g_strcmp0 (user_status, "away") == 0)
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (self->room_status_icon), "user-away-symbolic");
      gtk_widget_add_css_class (self->room_status_icon, "status-away");
      gtk_widget_set_visible (self->room_status_icon, TRUE);
    }
  else if (g_strcmp0 (user_status, "dnd") == 0 ||
           g_strcmp0 (user_status, "busy") == 0)
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (self->room_status_icon), "user-busy-symbolic");
      gtk_widget_add_css_class (self->room_status_icon, "status-dnd");
      gtk_widget_set_visible (self->room_status_icon, TRUE);
    }
  else
    {
      /* offline or unknown - hide indicator */
      gtk_widget_set_visible (self->room_status_icon, FALSE);
    }
}

static void
gnalk_conversation_header_finalize (GObject *object)
{
  GnalkConversationHeader *self = GNALK_CONVERSATION_HEADER (object);

  g_free (self->room_token);

  G_OBJECT_CLASS (gnalk_conversation_header_parent_class)->finalize (object);
}

static void
gnalk_conversation_header_class_init (GnalkConversationHeaderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = gnalk_conversation_header_finalize;

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-conversation-header.ui");
  signals[SIGNAL_INFO_REQUESTED] =
    g_signal_new ("info-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  gtk_widget_class_bind_template_child (widget_class, GnalkConversationHeader, room_header_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationHeader, room_avatar);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationHeader, room_status_icon);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationHeader, room_call_icon);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationHeader, room_title);
}

static void
gnalk_conversation_header_init (GnalkConversationHeader *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  g_signal_connect (self->room_header_button, "clicked", G_CALLBACK (on_header_button_clicked), self);

  /* Hide header button initially until a room is selected */
  gtk_widget_set_visible (self->room_header_button, FALSE);
}

GnalkConversationHeader *
gnalk_conversation_header_new (void)
{
  return g_object_new (GNALK_TYPE_CONVERSATION_HEADER, NULL);
}

void
gnalk_conversation_header_set_room (GnalkConversationHeader *self,
                                      const char               *token,
                                      const char               *name,
                                      const char               *description G_GNUC_UNUSED,
                                      gint                      room_type)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_HEADER (self));

  g_free (self->room_token);
  self->room_token = g_strdup (token);
  self->room_type = room_type;

  gtk_label_set_text (GTK_LABEL (self->room_title), name ? name : "");

  /* Hide call icon initially when switching rooms */
  gtk_widget_set_visible (self->room_call_icon, FALSE);
}

void
gnalk_conversation_header_set_avatar (GnalkConversationHeader *self,
                                        GdkTexture               *texture)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_HEADER (self));

  if (texture)
    gtk_image_set_from_paintable (GTK_IMAGE (self->room_avatar), GDK_PAINTABLE (texture));
  else
    gtk_image_set_from_icon_name (GTK_IMAGE (self->room_avatar), "avatar-default-symbolic");
}

void
gnalk_conversation_header_set_user_status (GnalkConversationHeader *self,
                                             const char               *status)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_HEADER (self));

  update_status_icon (self, status);
}

void
gnalk_conversation_header_set_call_active (GnalkConversationHeader *self,
                                             gboolean                  active)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_HEADER (self));

  /* Only show call indicator for 1:1 conversations */
  if (self->room_type != 1)
    {
      gtk_widget_set_visible (self->room_call_icon, FALSE);
      return;
    }

  /* Remove previous call classes */
  gtk_widget_remove_css_class (self->room_call_icon, "call-audio");
  gtk_widget_remove_css_class (self->room_call_icon, "call-video");
  gtk_widget_remove_css_class (self->room_call_icon, "call-screen");

  if (active)
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (self->room_call_icon), "call-start-symbolic");
      gtk_widget_add_css_class (self->room_call_icon, "call-audio");
      gtk_widget_set_visible (self->room_call_icon, TRUE);
    }
  else
    {
      gtk_widget_set_visible (self->room_call_icon, FALSE);
    }
}

void
gnalk_conversation_header_set_visible (GnalkConversationHeader *self,
                                         gboolean                  visible)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_HEADER (self));

  gtk_widget_set_visible (self->room_header_button, visible);
}
