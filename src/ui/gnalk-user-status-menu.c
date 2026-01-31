#include "gnalk-user-status-menu.h"
#include <adwaita.h>

struct _GnalkUserStatusMenu
{
  GtkBox parent_instance;

  GtkWidget *menu_button;
  GtkWidget *user_avatar;
  GtkWidget *user_status_icon;
  GtkWidget *status_popover;
  GtkWidget *status_online_button;
  GtkWidget *status_away_button;
  GtkWidget *status_dnd_button;
  GtkWidget *status_offline_button;

  char *user_status;
};

G_DEFINE_FINAL_TYPE (GnalkUserStatusMenu, gnalk_user_status_menu, GTK_TYPE_BOX)

enum {
  SIGNAL_STATUS_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
update_status_icon (GnalkUserStatusMenu *self)
{
  if (!self->user_status || !*self->user_status)
    {
      gtk_widget_set_visible (self->user_status_icon, FALSE);
      return;
    }

  /* Show the status icon */
  gtk_widget_set_visible (self->user_status_icon, TRUE);

  /* Remove previous status classes */
  gtk_widget_remove_css_class (self->user_status_icon, "status-online");
  gtk_widget_remove_css_class (self->user_status_icon, "status-away");
  gtk_widget_remove_css_class (self->user_status_icon, "status-dnd");
  gtk_widget_remove_css_class (self->user_status_icon, "status-offline");

  if (g_strcmp0 (self->user_status, "online") == 0)
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (self->user_status_icon), "user-available-symbolic");
      gtk_widget_add_css_class (self->user_status_icon, "status-online");
    }
  else if (g_strcmp0 (self->user_status, "away") == 0)
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (self->user_status_icon), "user-away-symbolic");
      gtk_widget_add_css_class (self->user_status_icon, "status-away");
    }
  else if (g_strcmp0 (self->user_status, "dnd") == 0 ||
           g_strcmp0 (self->user_status, "busy") == 0)
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (self->user_status_icon), "user-busy-symbolic");
      gtk_widget_add_css_class (self->user_status_icon, "status-dnd");
    }
  else /* offline or invisible */
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (self->user_status_icon), "user-offline-symbolic");
      gtk_widget_add_css_class (self->user_status_icon, "status-offline");
    }
}

static void
set_status_internal (GnalkUserStatusMenu *self, const char *status)
{
  g_free (self->user_status);
  self->user_status = g_strdup (status);

  update_status_icon (self);

  /* Close the popover */
  gtk_popover_popdown (GTK_POPOVER (self->status_popover));

  /* Emit signal for parent to handle API call */
  g_signal_emit (self, signals[SIGNAL_STATUS_CHANGED], 0, status);
}

static void
on_status_online_clicked (GtkButton            *button,
                          GnalkUserStatusMenu *self)
{
  (void) button;
  set_status_internal (self, "online");
}

static void
on_status_away_clicked (GtkButton            *button,
                        GnalkUserStatusMenu *self)
{
  (void) button;
  set_status_internal (self, "away");
}

static void
on_status_dnd_clicked (GtkButton            *button,
                       GnalkUserStatusMenu *self)
{
  (void) button;
  set_status_internal (self, "dnd");
}

static void
on_status_offline_clicked (GtkButton            *button,
                           GnalkUserStatusMenu *self)
{
  (void) button;
  set_status_internal (self, "offline");
}

static void
gnalk_user_status_menu_finalize (GObject *object)
{
  GnalkUserStatusMenu *self = GNALK_USER_STATUS_MENU (object);

  g_free (self->user_status);

  G_OBJECT_CLASS (gnalk_user_status_menu_parent_class)->finalize (object);
}

static void
gnalk_user_status_menu_class_init (GnalkUserStatusMenuClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = gnalk_user_status_menu_finalize;

  signals[SIGNAL_STATUS_CHANGED] =
    g_signal_new ("status-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-user-status-menu.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkUserStatusMenu, menu_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkUserStatusMenu, user_avatar);
  gtk_widget_class_bind_template_child (widget_class, GnalkUserStatusMenu, user_status_icon);
  gtk_widget_class_bind_template_child (widget_class, GnalkUserStatusMenu, status_popover);
  gtk_widget_class_bind_template_child (widget_class, GnalkUserStatusMenu, status_online_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkUserStatusMenu, status_away_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkUserStatusMenu, status_dnd_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkUserStatusMenu, status_offline_button);
}

static void
gnalk_user_status_menu_init (GnalkUserStatusMenu *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  g_signal_connect (self->status_online_button, "clicked", G_CALLBACK (on_status_online_clicked), self);
  g_signal_connect (self->status_away_button, "clicked", G_CALLBACK (on_status_away_clicked), self);
  g_signal_connect (self->status_dnd_button, "clicked", G_CALLBACK (on_status_dnd_clicked), self);
  g_signal_connect (self->status_offline_button, "clicked", G_CALLBACK (on_status_offline_clicked), self);
}

GnalkUserStatusMenu *
gnalk_user_status_menu_new (void)
{
  return g_object_new (GNALK_TYPE_USER_STATUS_MENU, NULL);
}

void
gnalk_user_status_menu_set_status (GnalkUserStatusMenu *self,
                                     const char           *status)
{
  g_return_if_fail (GNALK_IS_USER_STATUS_MENU (self));

  g_free (self->user_status);
  self->user_status = g_strdup (status);

  update_status_icon (self);
}

const char *
gnalk_user_status_menu_get_status (GnalkUserStatusMenu *self)
{
  g_return_val_if_fail (GNALK_IS_USER_STATUS_MENU (self), NULL);

  return self->user_status;
}

static void
on_avatar_loaded (GnalkAvatarCache *cache,
                  GdkTexture        *texture,
                  gpointer           user_data)
{
  (void) cache;
  GnalkUserStatusMenu *self = GNALK_USER_STATUS_MENU (user_data);

  if (texture)
    gtk_image_set_from_paintable (GTK_IMAGE (self->user_avatar), GDK_PAINTABLE (texture));
  else
    gtk_image_set_from_icon_name (GTK_IMAGE (self->user_avatar), "avatar-default-symbolic");
}

void
gnalk_user_status_menu_set_user (GnalkUserStatusMenu *self,
                                   const char           *user_id,
                                   GnalkAvatarCache    *cache)
{
  g_return_if_fail (GNALK_IS_USER_STATUS_MENU (self));

  if (cache && user_id && *user_id)
    gnalk_avatar_cache_get_user_avatar (cache, user_id, 64, on_avatar_loaded, self);
  else
    gtk_image_set_from_icon_name (GTK_IMAGE (self->user_avatar), "avatar-default-symbolic");
}

void
gnalk_user_status_menu_set_visible (GnalkUserStatusMenu *self,
                                      gboolean              visible)
{
  g_return_if_fail (GNALK_IS_USER_STATUS_MENU (self));

  gtk_widget_set_visible (self->menu_button, visible);
}

