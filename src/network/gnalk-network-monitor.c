#include "gnalk-network-monitor.h"

struct _GnalkNetworkMonitor
{
  GObject parent_instance;

  GNetworkMonitor *monitor;
  gboolean is_online;
  guint debounce_timeout_id;
};

G_DEFINE_FINAL_TYPE (GnalkNetworkMonitor, gnalk_network_monitor, G_TYPE_OBJECT)

enum
{
  SIGNAL_NETWORK_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static gboolean
emit_network_changed (gpointer user_data)
{
  GnalkNetworkMonitor *self = GNALK_NETWORK_MONITOR (user_data);

  self->debounce_timeout_id = 0;

  gboolean current_online = g_network_monitor_get_network_available (self->monitor);

  if (current_online != self->is_online)
    {
      self->is_online = current_online;
      g_signal_emit (self, signals[SIGNAL_NETWORK_CHANGED], 0, self->is_online);
      g_debug ("Network state changed: %s", self->is_online ? "online" : "offline");
    }

  return G_SOURCE_REMOVE;
}

static void
on_network_changed (GNetworkMonitor      *monitor G_GNUC_UNUSED,
                    gboolean              network_available G_GNUC_UNUSED,
                    GnalkNetworkMonitor *self)
{
  /* Debounce rapid changes (500ms) */
  if (self->debounce_timeout_id > 0)
    g_source_remove (self->debounce_timeout_id);

  self->debounce_timeout_id = g_timeout_add (500, emit_network_changed, self);
}

static void
gnalk_network_monitor_dispose (GObject *object)
{
  GnalkNetworkMonitor *self = GNALK_NETWORK_MONITOR (object);

  if (self->debounce_timeout_id > 0)
    {
      g_source_remove (self->debounce_timeout_id);
      self->debounce_timeout_id = 0;
    }

  if (self->monitor)
    {
      g_signal_handlers_disconnect_by_func (self->monitor, on_network_changed, self);
      self->monitor = NULL;
    }

  G_OBJECT_CLASS (gnalk_network_monitor_parent_class)->dispose (object);
}

static void
gnalk_network_monitor_class_init (GnalkNetworkMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gnalk_network_monitor_dispose;

  signals[SIGNAL_NETWORK_CHANGED] =
    g_signal_new ("network-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE,
                  1,
                  G_TYPE_BOOLEAN);
}

static void
gnalk_network_monitor_init (GnalkNetworkMonitor *self)
{
  self->monitor = g_network_monitor_get_default ();
  self->is_online = g_network_monitor_get_network_available (self->monitor);

  g_signal_connect (self->monitor, "network-changed",
                    G_CALLBACK (on_network_changed), self);

  g_debug ("Network monitor initialized: %s", self->is_online ? "online" : "offline");
}

GnalkNetworkMonitor *
gnalk_network_monitor_new (void)
{
  return g_object_new (GNALK_TYPE_NETWORK_MONITOR, NULL);
}

gboolean
gnalk_network_monitor_is_online (GnalkNetworkMonitor *self)
{
  g_return_val_if_fail (GNALK_IS_NETWORK_MONITOR (self), FALSE);
  return self->is_online;
}

gboolean
gnalk_network_monitor_is_metered (GnalkNetworkMonitor *self)
{
  g_return_val_if_fail (GNALK_IS_NETWORK_MONITOR (self), FALSE);
  return g_network_monitor_get_network_metered (self->monitor);
}
