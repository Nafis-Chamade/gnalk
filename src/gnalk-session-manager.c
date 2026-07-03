#include "gnalk-session-manager.h"
#include "gnalk-keyring.h"

struct _GnalkSessionManager
{
  GObject parent_instance;

  GnalkAccountManager      *account_manager;
  GnalkDatabase            *database;
  GnalkNetworkMonitor      *network_monitor;
  GnalkDataRepository      *data_repository;       /* Shared, not owned */
  GnalkNotificationService *notification_service;  /* Shared, not owned */

  GHashTable    *sessions;       /* gint64 account_id -> GnalkSession */
  GnalkSession *active_session;

  int global_unread;
};

G_DEFINE_FINAL_TYPE (GnalkSessionManager, gnalk_session_manager, G_TYPE_OBJECT)

enum {
  SIGNAL_SESSION_ADDED,
  SIGNAL_SESSION_REMOVED,
  SIGNAL_ACTIVE_SESSION_CHANGED,
  SIGNAL_GLOBAL_UNREAD_CHANGED,
  SIGNAL_ANY_SESSION_STATE_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void update_global_unread (GnalkSessionManager *self);

static void
gnalk_session_manager_finalize (GObject *object)
{
  GnalkSessionManager *self = GNALK_SESSION_MANAGER (object);

  gnalk_session_manager_disconnect_all (self);

  g_clear_pointer (&self->sessions, g_hash_table_unref);
  g_clear_object (&self->account_manager);
  g_clear_object (&self->database);
  g_clear_object (&self->network_monitor);

  G_OBJECT_CLASS (gnalk_session_manager_parent_class)->finalize (object);
}

static void
gnalk_session_manager_class_init (GnalkSessionManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gnalk_session_manager_finalize;

  signals[SIGNAL_SESSION_ADDED] =
    g_signal_new ("session-added", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, GNALK_TYPE_SESSION);

  signals[SIGNAL_SESSION_REMOVED] =
    g_signal_new ("session-removed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT64);

  signals[SIGNAL_ACTIVE_SESSION_CHANGED] =
    g_signal_new ("active-session-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, GNALK_TYPE_SESSION);

  signals[SIGNAL_GLOBAL_UNREAD_CHANGED] =
    g_signal_new ("global-unread-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);

  signals[SIGNAL_ANY_SESSION_STATE_CHANGED] =
    g_signal_new ("any-session-state-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                  GNALK_TYPE_SESSION, G_TYPE_INT, G_TYPE_INT);

}

static void
gnalk_session_manager_init (GnalkSessionManager *self)
{
  self->sessions = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                           g_free, g_object_unref);
}

/* Session signal handlers */
static void
on_session_state_changed (GnalkSession        *session,
                          int                   old_state,
                          int                   new_state,
                          GnalkSessionManager *self)
{
  g_signal_emit (self, signals[SIGNAL_ANY_SESSION_STATE_CHANGED], 0,
                 session, old_state, new_state);
}

static void
on_session_unread_changed (GnalkSession        *session G_GNUC_UNUSED,
                           int                   unread G_GNUC_UNUSED,
                           GnalkSessionManager *self)
{
  update_global_unread (self);
}

static void
connect_session_signals (GnalkSessionManager *self,
                         GnalkSession        *session)
{
  g_signal_connect (session, "state-changed",
                    G_CALLBACK (on_session_state_changed), self);
  g_signal_connect (session, "unread-count-changed",
                    G_CALLBACK (on_session_unread_changed), self);
}

static void
disconnect_session_signals (GnalkSessionManager *self,
                            GnalkSession        *session)
{
  g_signal_handlers_disconnect_by_data (session, self);
}

static void
update_global_unread (GnalkSessionManager *self)
{
  int total = 0;
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, self->sessions);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      GnalkSession *session = GNALK_SESSION (value);
      total += gnalk_session_get_total_unread (session);
    }

  if (total != self->global_unread)
    {
      self->global_unread = total;
      g_signal_emit (self, signals[SIGNAL_GLOBAL_UNREAD_CHANGED], 0, total);
    }
}

/* Constructor */
GnalkSessionManager *
gnalk_session_manager_new (GnalkAccountManager      *account_manager,
                           GnalkDatabase            *database,
                           GnalkNetworkMonitor      *network_monitor,
                           GnalkDataRepository      *data_repository,
                           GnalkNotificationService *notification_service)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT_MANAGER (account_manager), NULL);
  g_return_val_if_fail (GNALK_IS_DATABASE (database), NULL);
  g_return_val_if_fail (GNALK_IS_NETWORK_MONITOR (network_monitor), NULL);
  g_return_val_if_fail (GNALK_IS_DATA_REPOSITORY (data_repository), NULL);
  g_return_val_if_fail (GNALK_IS_NOTIFICATION_SERVICE (notification_service), NULL);

  GnalkSessionManager *self = g_object_new (GNALK_TYPE_SESSION_MANAGER, NULL);

  self->account_manager = g_object_ref (account_manager);
  self->database = g_object_ref (database);
  self->network_monitor = g_object_ref (network_monitor);
  self->data_repository = data_repository;  /* Not owned */
  self->notification_service = notification_service;  /* Not owned */

  return self;
}

/* Public API */

GnalkSession *
gnalk_session_manager_get_session (GnalkSessionManager *self,
                                     gint64                account_id)
{
  g_return_val_if_fail (GNALK_IS_SESSION_MANAGER (self), NULL);
  return g_hash_table_lookup (self->sessions, &account_id);
}

GList *
gnalk_session_manager_get_all_sessions (GnalkSessionManager *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION_MANAGER (self), NULL);

  GList *list = NULL;
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, self->sessions);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    list = g_list_prepend (list, g_object_ref (value));

  return list;
}

guint
gnalk_session_manager_get_session_count (GnalkSessionManager *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION_MANAGER (self), 0);
  return g_hash_table_size (self->sessions);
}

GnalkSession *
gnalk_session_manager_start_session (GnalkSessionManager *self,
                                        GnalkAccount        *account,
                                        const char           *password)
{
  g_return_val_if_fail (GNALK_IS_SESSION_MANAGER (self), NULL);
  g_return_val_if_fail (GNALK_IS_ACCOUNT (account), NULL);
  g_return_val_if_fail (password != NULL, NULL);

  gint64 account_id = gnalk_account_get_id (account);

  /* Check if session already exists */
  GnalkSession *existing = gnalk_session_manager_get_session (self, account_id);
  if (existing)
    {
      g_debug ("SessionManager: Session for account %ld already exists",
               (long) account_id);
      return existing;
    }

  /* Create new session */
  GnalkSession *session = gnalk_session_new (account, self->database, self->network_monitor,
                                             self->data_repository, self->notification_service);

  /* Store in hash table */
  gint64 *key = g_new (gint64, 1);
  *key = account_id;
  g_hash_table_insert (self->sessions, key, session);

  /* Connect signals */
  connect_session_signals (self, session);

  /* Emit signal */
  g_signal_emit (self, signals[SIGNAL_SESSION_ADDED], 0, session);

  /* Start connecting */
  gnalk_session_connect (session, password);

  /* If no active session, make this one active */
  if (!self->active_session)
    gnalk_session_manager_set_active_session (self, session);

  return session;
}

void
gnalk_session_manager_stop_session (GnalkSessionManager *self,
                                      gint64                account_id)
{
  g_return_if_fail (GNALK_IS_SESSION_MANAGER (self));

  GnalkSession *session = gnalk_session_manager_get_session (self, account_id);
  if (!session)
    return;

  gboolean was_active = (self->active_session == session);

  /* If this was the active session, find the next one BEFORE removing.
   * This avoids an intermediate NULL active session that would flash the
   * login view before immediately switching to the remaining account. */
  if (was_active)
    {
      GnalkSession *next = NULL;
      GHashTableIter iter;
      gpointer value;
      g_hash_table_iter_init (&iter, self->sessions);
      while (g_hash_table_iter_next (&iter, NULL, &value))
        {
          if (GNALK_SESSION (value) != session)
            {
              next = GNALK_SESSION (value);
              break;
            }
        }
      /* Switch directly to next session (or NULL if none remain) */
      gnalk_session_manager_set_active_session (self, next);
    }

  /* Disconnect signals */
  disconnect_session_signals (self, session);

  /* Disconnect the session */
  gnalk_session_disconnect (session);

  /* Remove from hash table (will unref) */
  g_hash_table_remove (self->sessions, &account_id);

  /* Emit signal */
  g_signal_emit (self, signals[SIGNAL_SESSION_REMOVED], 0, account_id);

  /* Update global unread */
  update_global_unread (self);
}

/* Context for an outstanding async keyring lookup during autologin.
 * Holds a weak ref to the manager (which may be destroyed mid-lookup) and
 * an owned ref to the account being connected. */
typedef struct
{
  GWeakRef      manager_ref;
  GnalkAccount *account;    /* owned */
  gboolean      is_guest;
} AutologinLookup;

static void
autologin_lookup_free (AutologinLookup *ctx)
{
  g_weak_ref_clear (&ctx->manager_ref);
  g_clear_object (&ctx->account);
  g_free (ctx);
}

static void
on_autologin_password_looked_up (GObject      *source G_GNUC_UNUSED,
                                 GAsyncResult *result,
                                 gpointer      user_data)
{
  AutologinLookup *ctx = user_data;
  GError *error = NULL;

  /* NOTE: never log the returned password. */
  char *password = gnalk_keyring_lookup_password_finish (result, &error);

  GnalkSessionManager *self = g_weak_ref_get (&ctx->manager_ref);
  if (self)
    {
      gint64 account_id = gnalk_account_get_id (ctx->account);

      /* Re-check: the account may have been connected by another path (e.g.
       * an interactive login) while this lookup was in flight. */
      if (!gnalk_session_manager_get_session (self, account_id))
        {
          if (ctx->is_guest)
            {
              /* Guest account: room password may legitimately be NULL. */
              g_debug ("SessionManager: Auto-connecting guest account %s@%s",
                       gnalk_account_get_user_id (ctx->account),
                       gnalk_account_get_server_url (ctx->account));

              gnalk_session_manager_start_guest_session (self, ctx->account, password);
            }
          else if (password)
            {
              g_debug ("SessionManager: Auto-connecting account %s@%s",
                       gnalk_account_get_user_id (ctx->account),
                       gnalk_account_get_server_url (ctx->account));

              gnalk_session_manager_start_session (self, ctx->account, password);
            }
          else
            {
              g_debug ("SessionManager: No password for account %s@%s, skipping autologin",
                       gnalk_account_get_user_id (ctx->account),
                       gnalk_account_get_server_url (ctx->account));
            }
        }

      g_object_unref (self);
    }

  if (error)
    g_clear_error (&error);

  g_free (password);
  autologin_lookup_free (ctx);
}

void
gnalk_session_manager_connect_all (GnalkSessionManager *self)
{
  g_return_if_fail (GNALK_IS_SESSION_MANAGER (self));

  GList *accounts = gnalk_account_manager_get_autologin_accounts (self->account_manager);

  for (GList *l = accounts; l; l = l->next)
    {
      GnalkAccount *account = GNALK_ACCOUNT (l->data);
      gint64 account_id = gnalk_account_get_id (account);

      /* Skip if already connected */
      if (gnalk_session_manager_get_session (self, account_id))
        {
          g_object_unref (account);
          continue;
        }

      /* Load the password asynchronously so a slow Secret Service does not
       * block the main thread / stall the UI at startup. Each account
       * autologins as its password arrives; strict ordering is not required.
       * The account ref is transferred into the lookup context. */
      AutologinLookup *ctx = g_new0 (AutologinLookup, 1);
      g_weak_ref_init (&ctx->manager_ref, self);
      ctx->account = account;  /* transfer ownership */
      ctx->is_guest = gnalk_account_get_is_guest (account);

      gnalk_keyring_lookup_password (
        gnalk_account_get_server_url (account),
        gnalk_account_get_user_id (account),
        on_autologin_password_looked_up,
        ctx);
    }

  g_list_free (accounts);
}

void
gnalk_session_manager_disconnect_all (GnalkSessionManager *self)
{
  g_return_if_fail (GNALK_IS_SESSION_MANAGER (self));

  /* Collect account IDs first to avoid modifying hash table while iterating */
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init (&iter, self->sessions);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      gint64 id = *(gint64 *) key;
      g_array_append_val (ids, id);
    }

  for (guint i = 0; i < ids->len; i++)
    {
      gint64 id = g_array_index (ids, gint64, i);
      gnalk_session_manager_stop_session (self, id);
    }

  g_array_unref (ids);
}

GnalkSession *
gnalk_session_manager_get_active_session (GnalkSessionManager *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION_MANAGER (self), NULL);
  return self->active_session;
}

void
gnalk_session_manager_set_active_session (GnalkSessionManager *self,
                                            GnalkSession        *session)
{
  g_return_if_fail (GNALK_IS_SESSION_MANAGER (self));

  if (self->active_session == session)
    return;

  self->active_session = session;
  g_signal_emit (self, signals[SIGNAL_ACTIVE_SESSION_CHANGED], 0, session);
}

gboolean
gnalk_session_manager_set_active_by_account_id (GnalkSessionManager *self,
                                                  gint64                account_id)
{
  g_return_val_if_fail (GNALK_IS_SESSION_MANAGER (self), FALSE);

  GnalkSession *session = gnalk_session_manager_get_session (self, account_id);
  if (!session)
    return FALSE;

  gnalk_session_manager_set_active_session (self, session);
  return TRUE;
}

int
gnalk_session_manager_get_global_unread (GnalkSessionManager *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION_MANAGER (self), 0);
  return self->global_unread;
}

gboolean
gnalk_session_manager_has_any_online (GnalkSessionManager *self)
{
  g_return_val_if_fail (GNALK_IS_SESSION_MANAGER (self), FALSE);

  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, self->sessions);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      GnalkSession *session = GNALK_SESSION (value);
      if (gnalk_session_is_online (session))
        return TRUE;
    }

  return FALSE;
}

GnalkSession *
gnalk_session_manager_start_guest_session (GnalkSessionManager *self,
                                             GnalkAccount        *account,
                                             const char           *room_password)
{
  g_return_val_if_fail (GNALK_IS_SESSION_MANAGER (self), NULL);
  g_return_val_if_fail (GNALK_IS_ACCOUNT (account), NULL);
  g_return_val_if_fail (gnalk_account_get_is_guest (account), NULL);

  gint64 account_id = gnalk_account_get_id (account);

  /* If session for this account already running, just return it */
  GnalkSession *existing = gnalk_session_manager_get_session (self, account_id);
  if (existing)
    return existing;

  /* Create guest session */
  GnalkSession *session = gnalk_session_new_guest (account, room_password,
                                                   self->database, self->network_monitor,
                                                   self->data_repository, self->notification_service);

  /* Store in hash table like any other session */
  gint64 *key = g_new (gint64, 1);
  *key = account_id;
  g_hash_table_insert (self->sessions, key, session);

  /* Connect signals */
  connect_session_signals (self, session);

  /* Emit signal */
  g_signal_emit (self, signals[SIGNAL_SESSION_ADDED], 0, session);

  /* Start connecting */
  gnalk_session_connect_guest (session);

  /* If no active session, make this one active */
  if (!self->active_session)
    gnalk_session_manager_set_active_session (self, session);

  return session;
}

void
gnalk_session_manager_stop_guest_session (GnalkSessionManager *self,
                                           gint64                account_id)
{
  g_return_if_fail (GNALK_IS_SESSION_MANAGER (self));
  gnalk_session_manager_stop_session (self, account_id);
}
