#include "gnalk-app-state.h"

struct _GnalkAppState
{
  GObject parent_instance;

  /* Status */
  gboolean         is_online;
  gboolean         is_loading;
  GnalkSyncStatus sync_status;
  char            *sync_message;
};

G_DEFINE_FINAL_TYPE (GnalkAppState, gnalk_app_state, G_TYPE_OBJECT)

enum {
  SIGNAL_ONLINE_CHANGED,
  SIGNAL_LOADING_CHANGED,
  SIGNAL_SYNC_STATUS_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gnalk_app_state_finalize (GObject *object)
{
  GnalkAppState *self = GNALK_APP_STATE (object);

  g_free (self->sync_message);

  G_OBJECT_CLASS (gnalk_app_state_parent_class)->finalize (object);
}

static void
gnalk_app_state_class_init (GnalkAppStateClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gnalk_app_state_finalize;

  signals[SIGNAL_ONLINE_CHANGED] =
    g_signal_new ("online-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  signals[SIGNAL_LOADING_CHANGED] =
    g_signal_new ("loading-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  signals[SIGNAL_SYNC_STATUS_CHANGED] =
    g_signal_new ("sync-status-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_STRING);
}

static void
gnalk_app_state_init (GnalkAppState *self)
{
  self->is_online = FALSE;
  self->is_loading = FALSE;
  self->sync_status = GNALK_SYNC_STATUS_IDLE;
}

GnalkAppState *
gnalk_app_state_new (void)
{
  return g_object_new (GNALK_TYPE_APP_STATE, NULL);
}

/* Online status */

gboolean
gnalk_app_state_is_online (GnalkAppState *self)
{
  g_return_val_if_fail (GNALK_IS_APP_STATE (self), FALSE);
  return self->is_online;
}

void
gnalk_app_state_set_online (GnalkAppState *self,
                              gboolean        is_online)
{
  g_return_if_fail (GNALK_IS_APP_STATE (self));

  if (self->is_online == is_online)
    return;

  self->is_online = is_online;
  g_signal_emit (self, signals[SIGNAL_ONLINE_CHANGED], 0, is_online);
}

/* Loading status */

gboolean
gnalk_app_state_is_loading (GnalkAppState *self)
{
  g_return_val_if_fail (GNALK_IS_APP_STATE (self), FALSE);
  return self->is_loading;
}

void
gnalk_app_state_set_loading (GnalkAppState *self,
                               gboolean        is_loading)
{
  g_return_if_fail (GNALK_IS_APP_STATE (self));

  if (self->is_loading == is_loading)
    return;

  self->is_loading = is_loading;
  g_signal_emit (self, signals[SIGNAL_LOADING_CHANGED], 0, is_loading);
}

/* Sync status */

GnalkSyncStatus
gnalk_app_state_get_sync_status (GnalkAppState *self)
{
  g_return_val_if_fail (GNALK_IS_APP_STATE (self), GNALK_SYNC_STATUS_IDLE);
  return self->sync_status;
}

void
gnalk_app_state_set_sync_status (GnalkAppState   *self,
                                   GnalkSyncStatus  status,
                                   const char       *message)
{
  g_return_if_fail (GNALK_IS_APP_STATE (self));

  gboolean changed = (self->sync_status != status) ||
                     (g_strcmp0 (self->sync_message, message) != 0);

  if (!changed)
    return;

  self->sync_status = status;
  g_free (self->sync_message);
  self->sync_message = g_strdup (message);

  g_signal_emit (self, signals[SIGNAL_SYNC_STATUS_CHANGED], 0, (int) status, message);
}

const char *
gnalk_app_state_get_sync_message (GnalkAppState *self)
{
  g_return_val_if_fail (GNALK_IS_APP_STATE (self), NULL);
  return self->sync_message;
}
