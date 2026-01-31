#pragma once

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GNALK_TYPE_NETWORK_MONITOR (gnalk_network_monitor_get_type ())
G_DECLARE_FINAL_TYPE (GnalkNetworkMonitor, gnalk_network_monitor, GNALK, NETWORK_MONITOR, GObject)

GnalkNetworkMonitor *gnalk_network_monitor_new (void);

gboolean gnalk_network_monitor_is_online (GnalkNetworkMonitor *self);
gboolean gnalk_network_monitor_is_metered (GnalkNetworkMonitor *self);

/* Signals:
 * - "network-changed" (gboolean is_online)
 */

G_END_DECLS
