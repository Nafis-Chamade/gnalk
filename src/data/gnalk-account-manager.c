#include "gnalk-account-manager.h"

struct _GnalkAccountManager
{
  GObject parent_instance;

  GnalkDatabase *database;
  GListStore     *accounts;
  GHashTable     *accounts_by_id;  /* gint64 -> GnalkAccount */
};

G_DEFINE_FINAL_TYPE (GnalkAccountManager, gnalk_account_manager, G_TYPE_OBJECT)

enum {
  SIGNAL_ACCOUNT_ADDED,
  SIGNAL_ACCOUNT_REMOVED,
  SIGNAL_ACCOUNT_UPDATED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gnalk_account_manager_finalize (GObject *object)
{
  GnalkAccountManager *self = GNALK_ACCOUNT_MANAGER (object);

  g_clear_pointer (&self->accounts_by_id, g_hash_table_unref);
  g_clear_object (&self->accounts);
  g_clear_object (&self->database);

  G_OBJECT_CLASS (gnalk_account_manager_parent_class)->finalize (object);
}

static void
gnalk_account_manager_class_init (GnalkAccountManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_account_manager_finalize;

  signals[SIGNAL_ACCOUNT_ADDED] =
    g_signal_new ("account-added",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  GNALK_TYPE_ACCOUNT);

  signals[SIGNAL_ACCOUNT_REMOVED] =
    g_signal_new ("account-removed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT64);

  signals[SIGNAL_ACCOUNT_UPDATED] =
    g_signal_new ("account-updated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  GNALK_TYPE_ACCOUNT);
}

static void
gnalk_account_manager_init (GnalkAccountManager *self)
{
  self->accounts = g_list_store_new (GNALK_TYPE_ACCOUNT);
  self->accounts_by_id = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                                 g_free, g_object_unref);
}

GnalkAccountManager *
gnalk_account_manager_new (GnalkDatabase *database)
{
  GnalkAccountManager *self = g_object_new (GNALK_TYPE_ACCOUNT_MANAGER, NULL);
  self->database = g_object_ref (database);
  return self;
}

static void
on_account_notify (GnalkAccount        *account,
                   GParamSpec           *pspec,
                   GnalkAccountManager *self)
{
  /* Save changes to database */
  gnalk_account_manager_save_account (self, account);

  /* Emit updated signal */
  g_signal_emit (self, signals[SIGNAL_ACCOUNT_UPDATED], 0, account);
}

static void
add_account_internal (GnalkAccountManager *self,
                      GnalkAccount        *account,
                      gboolean              emit_signal)
{
  gint64 *id_key = g_new (gint64, 1);
  *id_key = gnalk_account_get_id (account);

  g_hash_table_insert (self->accounts_by_id, id_key, g_object_ref (account));
  g_list_store_append (self->accounts, account);

  /* Watch for property changes */
  g_signal_connect (account, "notify::display-name",
                    G_CALLBACK (on_account_notify), self);
  g_signal_connect (account, "notify::avatar-url",
                    G_CALLBACK (on_account_notify), self);
  g_signal_connect (account, "notify::enabled",
                    G_CALLBACK (on_account_notify), self);
  g_signal_connect (account, "notify::autologin",
                    G_CALLBACK (on_account_notify), self);

  if (emit_signal)
    g_signal_emit (self, signals[SIGNAL_ACCOUNT_ADDED], 0, account);
}

void
gnalk_account_manager_load_accounts (GnalkAccountManager *self)
{
  g_return_if_fail (GNALK_IS_ACCOUNT_MANAGER (self));

  /* Query accounts from database */
  /* Note: This requires adding enabled/autologin columns to the accounts table.
   * For migration, we treat missing columns as enabled=TRUE, autologin=FALSE */
  GListStore *db_accounts = gnalk_database_load_all_accounts (self->database);
  if (!db_accounts)
    return;

  guint n = g_list_model_get_n_items (G_LIST_MODEL (db_accounts));
  for (guint i = 0; i < n; i++)
    {
      GnalkAccount *account = g_list_model_get_item (G_LIST_MODEL (db_accounts), i);
      add_account_internal (self, account, FALSE);
      g_object_unref (account);
    }

  g_object_unref (db_accounts);
  g_debug ("Loaded %u accounts from database", n);
}

GListStore *
gnalk_account_manager_get_accounts (GnalkAccountManager *self)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT_MANAGER (self), NULL);
  return self->accounts;
}

guint
gnalk_account_manager_get_account_count (GnalkAccountManager *self)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT_MANAGER (self), 0);
  return g_list_model_get_n_items (G_LIST_MODEL (self->accounts));
}

GnalkAccount *
gnalk_account_manager_get_account_by_id (GnalkAccountManager *self,
                                           gint64                account_id)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT_MANAGER (self), NULL);
  return g_hash_table_lookup (self->accounts_by_id, &account_id);
}

GnalkAccount *
gnalk_account_manager_find_account (GnalkAccountManager *self,
                                      const char           *server_url,
                                      const char           *user_id)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT_MANAGER (self), NULL);
  g_return_val_if_fail (server_url != NULL, NULL);
  g_return_val_if_fail (user_id != NULL, NULL);

  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->accounts));
  for (guint i = 0; i < n; i++)
    {
      GnalkAccount *account = g_list_model_get_item (G_LIST_MODEL (self->accounts), i);
      gboolean matches = gnalk_account_matches (account, server_url, user_id);
      g_object_unref (account);

      if (matches)
        return g_list_model_get_item (G_LIST_MODEL (self->accounts), i);
    }

  return NULL;
}

GnalkAccount *
gnalk_account_manager_add_account (GnalkAccountManager *self,
                                     const char           *server_url,
                                     const char           *user_id)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT_MANAGER (self), NULL);
  g_return_val_if_fail (server_url != NULL, NULL);
  g_return_val_if_fail (user_id != NULL, NULL);

  /* Check if account already exists */
  GnalkAccount *existing = gnalk_account_manager_find_account (self, server_url, user_id);
  if (existing)
    {
      /* Return a BORROWED ref (like the create path below via
       * get_account_by_id) — find_account transfers full, which leaked here
       * since callers never unref. */
      gint64 id = gnalk_account_get_id (existing);
      g_object_unref (existing);
      return gnalk_account_manager_get_account_by_id (self, id);
    }

  /* Create in database */
  gint64 account_id = gnalk_database_create_account (self->database, server_url, user_id);
  if (account_id <= 0)
    {
      g_warning ("Failed to create account in database");
      return NULL;
    }

  /* Create account object */
  GnalkAccount *account = gnalk_account_new (account_id, server_url, user_id);
  add_account_internal (self, account, TRUE);
  g_object_unref (account);

  return gnalk_account_manager_get_account_by_id (self, account_id);
}

gboolean
gnalk_account_manager_remove_account (GnalkAccountManager *self,
                                        gint64                account_id)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT_MANAGER (self), FALSE);

  GnalkAccount *account = gnalk_account_manager_get_account_by_id (self, account_id);
  if (!account)
    return FALSE;

  /* Disconnect signals */
  g_signal_handlers_disconnect_by_func (account, on_account_notify, self);

  /* Remove from list store */
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->accounts));
  for (guint i = 0; i < n; i++)
    {
      GnalkAccount *item = g_list_model_get_item (G_LIST_MODEL (self->accounts), i);
      gboolean is_match = (gnalk_account_get_id (item) == account_id);
      g_object_unref (item);

      if (is_match)
        {
          g_list_store_remove (self->accounts, i);
          break;
        }
    }

  /* Remove from hash table */
  g_hash_table_remove (self->accounts_by_id, &account_id);

  /* Delete from database (cascades to conversations, messages, etc.) */
  gnalk_database_delete_account (self->database, account_id);

  g_signal_emit (self, signals[SIGNAL_ACCOUNT_REMOVED], 0, account_id);
  return TRUE;
}

void
gnalk_account_manager_save_account (GnalkAccountManager *self,
                                      GnalkAccount        *account)
{
  g_return_if_fail (GNALK_IS_ACCOUNT_MANAGER (self));
  g_return_if_fail (GNALK_IS_ACCOUNT (account));

  gnalk_database_save_account (self->database, account);
}

GnalkAccount *
gnalk_account_manager_add_guest_account (GnalkAccountManager *self,
                                         const char           *server_url,
                                         const char           *room_token,
                                         const char           *display_name)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT_MANAGER (self), NULL);
  g_return_val_if_fail (server_url != NULL, NULL);
  g_return_val_if_fail (room_token != NULL, NULL);

  g_autofree char *user_id = g_strdup_printf ("guest:%s", room_token);

  /* Check if guest account for this room already exists */
  GnalkAccount *existing = gnalk_account_manager_find_account (self, server_url, user_id);
  if (existing)
    {
      /* Update display name if changed */
      if (display_name)
        gnalk_account_set_display_name (existing, display_name);
      /* Return a BORROWED ref, consistent with the create path (see above). */
      gint64 id = gnalk_account_get_id (existing);
      g_object_unref (existing);
      return gnalk_account_manager_get_account_by_id (self, id);
    }

  /* Create in database */
  gint64 account_id = gnalk_database_create_account (self->database, server_url, user_id);
  if (account_id <= 0)
    {
      g_warning ("Failed to create guest account in database");
      return NULL;
    }

  /* Create account object with guest fields */
  GnalkAccount *account = gnalk_account_new_full (account_id, server_url, user_id,
                                                     display_name, NULL,
                                                     TRUE, TRUE, NULL,
                                                     TRUE, room_token, room_token);
  gnalk_database_save_account (self->database, account);
  add_account_internal (self, account, TRUE);
  g_object_unref (account);

  return gnalk_account_manager_get_account_by_id (self, account_id);
}

GList *
gnalk_account_manager_get_autologin_accounts (GnalkAccountManager *self)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT_MANAGER (self), NULL);

  GList *result = NULL;
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->accounts));

  for (guint i = 0; i < n; i++)
    {
      GnalkAccount *account = g_list_model_get_item (G_LIST_MODEL (self->accounts), i);
      if (gnalk_account_get_autologin (account) && gnalk_account_get_enabled (account))
        result = g_list_prepend (result, account);
      else
        g_object_unref (account);
    }

  return g_list_reverse (result);
}

GList *
gnalk_account_manager_get_enabled_accounts (GnalkAccountManager *self)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT_MANAGER (self), NULL);

  GList *result = NULL;
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->accounts));

  for (guint i = 0; i < n; i++)
    {
      GnalkAccount *account = g_list_model_get_item (G_LIST_MODEL (self->accounts), i);
      if (gnalk_account_get_enabled (account))
        result = g_list_prepend (result, account);
      else
        g_object_unref (account);
    }

  return g_list_reverse (result);
}
