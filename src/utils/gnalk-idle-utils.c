/* gnalk-idle-utils.c
 *
 * Standardized idle callback helpers for safe async operations.
 */

#include "gnalk-idle-utils.h"

guint
gnalk_idle_add (GSourceFunc    callback,
                 gpointer       data,
                 GDestroyNotify destroy)
{
  GSource *source = g_idle_source_new ();
  guint id;

  g_source_set_callback (source, callback, data, destroy);
  id = g_source_attach (source, g_main_context_default ());
  g_source_unref (source);

  return id;
}

guint
gnalk_idle_add_once (GSourceFunc callback,
                      gpointer    data)
{
  return gnalk_idle_add (callback, data, g_free);
}

guint
gnalk_timeout_add_seconds_once (guint       seconds,
                                 GSourceFunc callback,
                                 gpointer    data)
{
  GSource *source = g_timeout_source_new_seconds (seconds);
  guint id;

  g_source_set_callback (source, callback, data, g_free);
  id = g_source_attach (source, g_main_context_default ());
  g_source_unref (source);

  return id;
}
