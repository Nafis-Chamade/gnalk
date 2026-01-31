#pragma once

#include <gtk/gtk.h>
#include <adwaita.h>

G_BEGIN_DECLS

#define GNALK_TYPE_NETWORK_STATUS_BAR (gnalk_network_status_bar_get_type ())
G_DECLARE_FINAL_TYPE (GnalkNetworkStatusBar, gnalk_network_status_bar, GNALK, NETWORK_STATUS_BAR, GtkBox)

GnalkNetworkStatusBar *gnalk_network_status_bar_new (void);

void gnalk_network_status_bar_set_offline (GnalkNetworkStatusBar *self,
                                             gboolean                offline,
                                             const char             *message);

void gnalk_network_status_bar_set_error (GnalkNetworkStatusBar *self,
                                           const char             *error_message);

void gnalk_network_status_bar_hide (GnalkNetworkStatusBar *self);

gboolean gnalk_network_status_bar_is_revealed (GnalkNetworkStatusBar *self);

G_END_DECLS
