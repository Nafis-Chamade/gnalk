#include "gnalk-network-status-bar.h"
#include <glib/gi18n.h>

struct _GnalkNetworkStatusBar
{
  GtkBox parent_instance;

  GtkWidget *banner;

  gboolean was_offline;
  gboolean had_connection_error;
};

G_DEFINE_FINAL_TYPE (GnalkNetworkStatusBar, gnalk_network_status_bar, GTK_TYPE_BOX)

static void
gnalk_network_status_bar_class_init (GnalkNetworkStatusBarClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-network-status-bar.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkNetworkStatusBar, banner);
}

static void
gnalk_network_status_bar_init (GnalkNetworkStatusBar *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

GnalkNetworkStatusBar *
gnalk_network_status_bar_new (void)
{
  return g_object_new (GNALK_TYPE_NETWORK_STATUS_BAR, NULL);
}

void
gnalk_network_status_bar_set_offline (GnalkNetworkStatusBar *self,
                                        gboolean                offline,
                                        const char             *message)
{
  g_return_if_fail (GNALK_IS_NETWORK_STATUS_BAR (self));

  if (offline)
    {
      self->was_offline = TRUE;
      adw_banner_set_title (ADW_BANNER (self->banner),
                            message ? message : _("No network connection"));
      adw_banner_set_revealed (ADW_BANNER (self->banner), TRUE);
    }
  else
    {
      adw_banner_set_revealed (ADW_BANNER (self->banner), FALSE);
    }
}

void
gnalk_network_status_bar_set_error (GnalkNetworkStatusBar *self,
                                      const char             *error_message)
{
  g_return_if_fail (GNALK_IS_NETWORK_STATUS_BAR (self));

  self->had_connection_error = TRUE;
  adw_banner_set_title (ADW_BANNER (self->banner),
                        error_message ? error_message : _("Connection error"));
  adw_banner_set_revealed (ADW_BANNER (self->banner), TRUE);
}

void
gnalk_network_status_bar_hide (GnalkNetworkStatusBar *self)
{
  g_return_if_fail (GNALK_IS_NETWORK_STATUS_BAR (self));

  adw_banner_set_revealed (ADW_BANNER (self->banner), FALSE);
}

gboolean
gnalk_network_status_bar_is_revealed (GnalkNetworkStatusBar *self)
{
  g_return_val_if_fail (GNALK_IS_NETWORK_STATUS_BAR (self), FALSE);

  return adw_banner_get_revealed (ADW_BANNER (self->banner));
}
