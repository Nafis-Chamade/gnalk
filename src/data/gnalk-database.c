#include "gnalk-database.h"
#include "gnalk-account.h"
#include <sqlite3.h>
#include <json-glib/json-glib.h>

struct _GnalkDatabase
{
  GObject parent_instance;

  sqlite3 *db;
  char *db_path;
};

G_DEFINE_FINAL_TYPE (GnalkDatabase, gnalk_database, G_TYPE_OBJECT)

static const char *SCHEMA_SQL =
  "CREATE TABLE IF NOT EXISTS accounts ("
  "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  server_url TEXT NOT NULL,"
  "  user_id TEXT NOT NULL,"
  "  user_name TEXT,"
  "  avatar_url TEXT,"
  "  enabled INTEGER DEFAULT 1,"
  "  autologin INTEGER DEFAULT 0,"
  "  goa_account_id TEXT,"
  "  UNIQUE(server_url, user_id)"
  ");"

  "CREATE TABLE IF NOT EXISTS conversations ("
  "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,"
  "  token TEXT NOT NULL,"
  "  title TEXT,"
  "  name TEXT,"
  "  user_status TEXT,"
  "  actor_id TEXT,"
  "  room_type INTEGER DEFAULT 0,"
  "  last_activity INTEGER DEFAULT 0,"
  "  last_common_read_message INTEGER DEFAULT 0,"
  "  unread_messages INTEGER DEFAULT 0,"
  "  is_favorite INTEGER DEFAULT 0,"
  "  can_leave INTEGER DEFAULT 1,"
  "  can_delete INTEGER DEFAULT 0,"
  "  description TEXT,"
  "  participant_type INTEGER DEFAULT 0,"
  "  notification_level INTEGER DEFAULT 0,"
  "  call_notification_level INTEGER DEFAULT 0,"
  "  last_read_message INTEGER DEFAULT 0,"
  "  last_message_id INTEGER DEFAULT 0,"
  "  last_message_text TEXT,"
  "  last_message_actor_id TEXT,"
  "  last_message_actor_name TEXT,"
  "  last_message_type TEXT,"
  "  last_message_params_json TEXT,"
  "  UNIQUE(account_id, token)"
  ");"

  "CREATE INDEX IF NOT EXISTS idx_conversations_account ON conversations(account_id);"
  "CREATE INDEX IF NOT EXISTS idx_conversations_token ON conversations(account_id, token);"

  "CREATE TABLE IF NOT EXISTS messages ("
  "  id INTEGER NOT NULL,"
  "  account_id INTEGER NOT NULL,"
  "  conversation_token TEXT NOT NULL,"
  "  timestamp INTEGER NOT NULL,"
  "  reply_to INTEGER,"
  "  last_edit_timestamp INTEGER DEFAULT 0,"
  "  message TEXT,"
  "  author_id TEXT,"
  "  author_name TEXT,"
  "  reference_id TEXT,"
  "  system_message TEXT,"
  "  is_own INTEGER DEFAULT 0,"
  "  status INTEGER DEFAULT 0,"
  "  reactions_json TEXT,"
  "  parent_json TEXT,"
  "  message_parameters_json TEXT,"
  "  PRIMARY KEY (id, account_id)"
  ");"

  "CREATE INDEX IF NOT EXISTS idx_messages_conv ON messages(account_id, conversation_token, timestamp DESC);"
  "CREATE INDEX IF NOT EXISTS idx_messages_ref ON messages(reference_id);"

  "CREATE TABLE IF NOT EXISTS pending_messages ("
  "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  account_id INTEGER NOT NULL,"
  "  conversation_token TEXT NOT NULL,"
  "  message TEXT,"
  "  reply_to INTEGER,"
  "  reference_id TEXT NOT NULL UNIQUE,"
  "  file_path TEXT,"
  "  file_name TEXT,"
  "  content_type TEXT,"
  "  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),"
  "  retry_count INTEGER DEFAULT 0,"
  "  last_error TEXT"
  ");"

  "CREATE INDEX IF NOT EXISTS idx_pending_account ON pending_messages(account_id);"

  "CREATE TABLE IF NOT EXISTS sync_state ("
  "  account_id INTEGER NOT NULL,"
  "  conversation_token TEXT NOT NULL,"
  "  last_known_message_id INTEGER DEFAULT 0,"
  "  oldest_cached_message_id INTEGER,"
  "  has_more_history INTEGER DEFAULT 1,"
  "  PRIMARY KEY(account_id, conversation_token)"
  ");"

  "CREATE TABLE IF NOT EXISTS participants ("
  "  account_id INTEGER NOT NULL,"
  "  conversation_token TEXT NOT NULL,"
  "  actor_id TEXT NOT NULL,"
  "  display_name TEXT,"
  "  status TEXT DEFAULT '',"
  "  in_call INTEGER DEFAULT 0,"
  "  PRIMARY KEY(account_id, conversation_token, actor_id)"
  ");"

  "CREATE INDEX IF NOT EXISTS idx_participants_conv ON participants(account_id, conversation_token);";

static void
gnalk_database_finalize (GObject *object)
{
  GnalkDatabase *self = GNALK_DATABASE (object);

  gnalk_database_close (self);
  g_free (self->db_path);

  G_OBJECT_CLASS (gnalk_database_parent_class)->finalize (object);
}

static void
gnalk_database_class_init (GnalkDatabaseClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gnalk_database_finalize;
}

static void
gnalk_database_init (GnalkDatabase *self)
{
  const char *data_dir = g_get_user_data_dir ();
  char *gnalk_dir = g_build_filename (data_dir, "gnalk", NULL);
  g_mkdir_with_parents (gnalk_dir, 0700);
  self->db_path = g_build_filename (gnalk_dir, "gnalk.db", NULL);
  g_free (gnalk_dir);
}

GnalkDatabase *
gnalk_database_new (void)
{
  return g_object_new (GNALK_TYPE_DATABASE, NULL);
}

gboolean
gnalk_database_open (GnalkDatabase *self, GError **error)
{
  g_return_val_if_fail (GNALK_IS_DATABASE (self), FALSE);

  if (self->db)
    return TRUE;

  int rc = sqlite3_open (self->db_path, &self->db);
  if (rc != SQLITE_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to open database: %s", sqlite3_errmsg (self->db));
      sqlite3_close (self->db);
      self->db = NULL;
      return FALSE;
    }

  /* Enable foreign keys */
  sqlite3_exec (self->db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);

  /* Create schema */
  char *err_msg = NULL;
  rc = sqlite3_exec (self->db, SCHEMA_SQL, NULL, NULL, &err_msg);
  if (rc != SQLITE_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create schema: %s", err_msg);
      sqlite3_free (err_msg);
      sqlite3_close (self->db);
      self->db = NULL;
      return FALSE;
    }

  /* Migration: Add new account columns if they don't exist */
  sqlite3_exec (self->db,
                "ALTER TABLE accounts ADD COLUMN avatar_url TEXT;",
                NULL, NULL, NULL);
  sqlite3_exec (self->db,
                "ALTER TABLE accounts ADD COLUMN enabled INTEGER DEFAULT 1;",
                NULL, NULL, NULL);
  sqlite3_exec (self->db,
                "ALTER TABLE accounts ADD COLUMN autologin INTEGER DEFAULT 0;",
                NULL, NULL, NULL);
  sqlite3_exec (self->db,
                "ALTER TABLE accounts ADD COLUMN goa_account_id TEXT;",
                NULL, NULL, NULL);

  /* Migration: Add status and in_call columns to participants */
  sqlite3_exec (self->db,
                "ALTER TABLE participants ADD COLUMN status TEXT DEFAULT '';",
                NULL, NULL, NULL);
  sqlite3_exec (self->db,
                "ALTER TABLE participants ADD COLUMN in_call INTEGER DEFAULT 0;",
                NULL, NULL, NULL);

  /* Migration: Add new conversation columns */
  sqlite3_exec (self->db,
                "ALTER TABLE conversations ADD COLUMN description TEXT;",
                NULL, NULL, NULL);
  sqlite3_exec (self->db,
                "ALTER TABLE conversations ADD COLUMN participant_type INTEGER DEFAULT 0;",
                NULL, NULL, NULL);
  sqlite3_exec (self->db,
                "ALTER TABLE conversations ADD COLUMN notification_level INTEGER DEFAULT 0;",
                NULL, NULL, NULL);
  sqlite3_exec (self->db,
                "ALTER TABLE conversations ADD COLUMN call_notification_level INTEGER DEFAULT 0;",
                NULL, NULL, NULL);
  sqlite3_exec (self->db,
                "ALTER TABLE conversations ADD COLUMN last_read_message INTEGER DEFAULT 0;",
                NULL, NULL, NULL);

  /* Migration: Add raw last message columns */
  sqlite3_exec (self->db,
                "ALTER TABLE conversations ADD COLUMN last_message_id INTEGER DEFAULT 0;",
                NULL, NULL, NULL);
  sqlite3_exec (self->db,
                "ALTER TABLE conversations ADD COLUMN last_message_text TEXT;",
                NULL, NULL, NULL);
  sqlite3_exec (self->db,
                "ALTER TABLE conversations ADD COLUMN last_message_actor_id TEXT;",
                NULL, NULL, NULL);
  sqlite3_exec (self->db,
                "ALTER TABLE conversations ADD COLUMN last_message_actor_name TEXT;",
                NULL, NULL, NULL);
  sqlite3_exec (self->db,
                "ALTER TABLE conversations ADD COLUMN last_message_type TEXT;",
                NULL, NULL, NULL);
  sqlite3_exec (self->db,
                "ALTER TABLE conversations ADD COLUMN last_message_params_json TEXT;",
                NULL, NULL, NULL);

  /* Migration: Add index for sync_state lookups */
  sqlite3_exec (self->db,
                "CREATE INDEX IF NOT EXISTS idx_sync_state_lookup ON sync_state(account_id, conversation_token);",
                NULL, NULL, NULL);

  /* Migration: Change messages table primary key to (id, account_id) for multi-account support.
   * Check if migration is needed by looking for tables without the composite key. */
  {
    sqlite3_stmt *check_stmt;
    gboolean needs_migration = FALSE;

    /* Check if the old messages table exists and has the old schema (id as sole primary key) */
    const char *check_sql = "SELECT sql FROM sqlite_master WHERE type='table' AND name='messages'";
    if (sqlite3_prepare_v2 (self->db, check_sql, -1, &check_stmt, NULL) == SQLITE_OK)
      {
        if (sqlite3_step (check_stmt) == SQLITE_ROW)
          {
            const char *schema = (const char *)sqlite3_column_text (check_stmt, 0);
            /* Old schema has "id INTEGER PRIMARY KEY," - new has "PRIMARY KEY (id, account_id)" */
            if (schema && strstr (schema, "id INTEGER PRIMARY KEY,") != NULL)
              needs_migration = TRUE;
          }
        sqlite3_finalize (check_stmt);
      }

    if (needs_migration)
      {
        g_message ("Database migration: recreating messages table with composite primary key");

        /* SQLite doesn't support ALTER TABLE to change primary key, so we need to recreate */
        const char *migration_sql =
          "BEGIN TRANSACTION;"
          "CREATE TABLE messages_new ("
          "  id INTEGER NOT NULL,"
          "  account_id INTEGER NOT NULL,"
          "  conversation_token TEXT NOT NULL,"
          "  timestamp INTEGER NOT NULL,"
          "  reply_to INTEGER,"
          "  last_edit_timestamp INTEGER DEFAULT 0,"
          "  message TEXT,"
          "  author_id TEXT,"
          "  author_name TEXT,"
          "  reference_id TEXT,"
          "  system_message TEXT,"
          "  is_own INTEGER DEFAULT 0,"
          "  status INTEGER DEFAULT 0,"
          "  reactions_json TEXT,"
          "  parent_json TEXT,"
          "  message_parameters_json TEXT,"
          "  PRIMARY KEY (id, account_id)"
          ");"
          "INSERT INTO messages_new SELECT * FROM messages;"
          "DROP TABLE messages;"
          "ALTER TABLE messages_new RENAME TO messages;"
          "CREATE INDEX IF NOT EXISTS idx_messages_conv ON messages(account_id, conversation_token, timestamp DESC);"
          "CREATE INDEX IF NOT EXISTS idx_messages_ref ON messages(reference_id);"
          "COMMIT;";

        char *err_msg = NULL;
        if (sqlite3_exec (self->db, migration_sql, NULL, NULL, &err_msg) != SQLITE_OK)
          {
            g_warning ("Messages table migration failed: %s", err_msg ? err_msg : "unknown error");
            sqlite3_free (err_msg);
            /* Rollback if there was a failure */
            sqlite3_exec (self->db, "ROLLBACK;", NULL, NULL, NULL);
          }
        else
          {
            g_message ("Database migration: messages table migration completed successfully");
          }
      }
  }

  g_debug ("Database opened: %s", self->db_path);
  return TRUE;
}

void
gnalk_database_close (GnalkDatabase *self)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));

  if (self->db)
    {
      sqlite3_close (self->db);
      self->db = NULL;
    }
}

gint64
gnalk_database_get_or_create_account (GnalkDatabase *self,
                                        const char     *server_url,
                                        const char     *user_id)
{
  g_return_val_if_fail (GNALK_IS_DATABASE (self), -1);
  g_return_val_if_fail (self->db != NULL, -1);
  g_return_val_if_fail (server_url != NULL, -1);
  g_return_val_if_fail (user_id != NULL, -1);

  sqlite3_stmt *stmt;
  gint64 account_id = -1;

  /* Try to find existing account */
  const char *select_sql = "SELECT id FROM accounts WHERE server_url = ? AND user_id = ?";
  if (sqlite3_prepare_v2 (self->db, select_sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_text (stmt, 1, server_url, -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 2, user_id, -1, SQLITE_STATIC);

      if (sqlite3_step (stmt) == SQLITE_ROW)
        account_id = sqlite3_column_int64 (stmt, 0);

      sqlite3_finalize (stmt);
    }

  if (account_id > 0)
    return account_id;

  /* Create new account */
  const char *insert_sql = "INSERT INTO accounts (server_url, user_id) VALUES (?, ?)";
  if (sqlite3_prepare_v2 (self->db, insert_sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_text (stmt, 1, server_url, -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 2, user_id, -1, SQLITE_STATIC);

      if (sqlite3_step (stmt) == SQLITE_DONE)
        account_id = sqlite3_last_insert_rowid (self->db);

      sqlite3_finalize (stmt);
    }

  return account_id;
}

void
gnalk_database_update_account_user_name (GnalkDatabase *self,
                                           gint64          account_id,
                                           const char     *user_name)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);

  sqlite3_stmt *stmt;
  const char *sql = "UPDATE accounts SET user_name = ? WHERE id = ?";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_text (stmt, 1, user_name, -1, SQLITE_STATIC);
      sqlite3_bind_int64 (stmt, 2, account_id);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

void
gnalk_database_save_conversation (GnalkDatabase         *self,
                                    gint64                  account_id,
                                    GnalkConversationData *data)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);
  g_return_if_fail (GNALK_IS_CONVERSATION_DATA (data));

  /* Skip section headers */
  if (gnalk_conversation_data_is_section_header (data))
    return;

  sqlite3_stmt *stmt;
  const char *sql =
    "INSERT OR REPLACE INTO conversations "
    "(account_id, token, title, name, user_status, actor_id, "
    "room_type, last_activity, last_common_read_message, unread_messages, "
    "is_favorite, can_leave, can_delete, description, participant_type, "
    "notification_level, call_notification_level, last_read_message, "
    "last_message_id, last_message_text, last_message_actor_id, "
    "last_message_actor_name, last_message_type, last_message_params_json) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_bind_text (stmt, 2, gnalk_conversation_data_get_token (data), -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 3, gnalk_conversation_data_get_title (data), -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 4, gnalk_conversation_data_get_name (data), -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 5, gnalk_conversation_data_get_user_status (data), -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 6, gnalk_conversation_data_get_actor_id (data), -1, SQLITE_STATIC);
      sqlite3_bind_int (stmt, 7, gnalk_conversation_data_get_room_type (data));
      sqlite3_bind_int64 (stmt, 8, gnalk_conversation_data_get_last_activity (data));
      sqlite3_bind_int64 (stmt, 9, gnalk_conversation_data_get_last_common_read_message (data));
      sqlite3_bind_int (stmt, 10, gnalk_conversation_data_get_unread_messages (data));
      sqlite3_bind_int (stmt, 11, gnalk_conversation_data_get_is_favorite (data) ? 1 : 0);
      sqlite3_bind_int (stmt, 12, gnalk_conversation_data_get_can_leave (data) ? 1 : 0);
      sqlite3_bind_int (stmt, 13, gnalk_conversation_data_get_can_delete (data) ? 1 : 0);
      sqlite3_bind_text (stmt, 14, gnalk_conversation_data_get_description (data), -1, SQLITE_STATIC);
      sqlite3_bind_int (stmt, 15, gnalk_conversation_data_get_participant_type (data));
      sqlite3_bind_int (stmt, 16, gnalk_conversation_data_get_notification_level (data));
      sqlite3_bind_int (stmt, 17, gnalk_conversation_data_get_call_notification_level (data));
      sqlite3_bind_int64 (stmt, 18, gnalk_conversation_data_get_last_read_message (data));
      sqlite3_bind_int64 (stmt, 19, gnalk_conversation_data_get_last_message_id (data));
      sqlite3_bind_text (stmt, 20, gnalk_conversation_data_get_last_message_text (data), -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 21, gnalk_conversation_data_get_last_message_actor_id (data), -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 22, gnalk_conversation_data_get_last_message_actor_name (data), -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 23, gnalk_conversation_data_get_last_message_type (data), -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 24, gnalk_conversation_data_get_last_message_params_json (data), -1, SQLITE_STATIC);

      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

void
gnalk_database_delete_conversation (GnalkDatabase *self,
                                      gint64          account_id,
                                      const char     *token)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);
  g_return_if_fail (token != NULL);

  sqlite3_stmt *stmt;
  const char *sql = "DELETE FROM conversations WHERE account_id = ? AND token = ?";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_bind_text (stmt, 2, token, -1, SQLITE_STATIC);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }

  /* Also delete messages and sync state */
  const char *msg_sql = "DELETE FROM messages WHERE account_id = ? AND conversation_token = ?";
  if (sqlite3_prepare_v2 (self->db, msg_sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_bind_text (stmt, 2, token, -1, SQLITE_STATIC);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }

  const char *sync_sql = "DELETE FROM sync_state WHERE account_id = ? AND conversation_token = ?";
  if (sqlite3_prepare_v2 (self->db, sync_sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_bind_text (stmt, 2, token, -1, SQLITE_STATIC);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

void
gnalk_database_update_conversation_last_message (GnalkDatabase *self,
                                                    gint64          account_id,
                                                    const char     *token,
                                                    gint64          last_activity,
                                                    gint64          last_message_id,
                                                    const char     *last_message_text,
                                                    const char     *last_message_actor_id,
                                                    const char     *last_message_actor_name,
                                                    const char     *last_message_type,
                                                    const char     *last_message_params_json)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);
  g_return_if_fail (token != NULL);

  g_message ("DB: update_conversation_last_message account=%ld token=%s msg_id=%ld text='%.30s'",
             (long)account_id, token, (long)last_message_id,
             last_message_text ? last_message_text : "(null)");

  sqlite3_stmt *stmt;
  const char *sql = "UPDATE conversations SET last_activity = ?, "
                    "last_message_id = ?, last_message_text = ?, "
                    "last_message_actor_id = ?, last_message_actor_name = ?, "
                    "last_message_type = ?, last_message_params_json = ? "
                    "WHERE account_id = ? AND token = ?";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, last_activity);
      sqlite3_bind_int64 (stmt, 2, last_message_id);
      sqlite3_bind_text (stmt, 3, last_message_text ? last_message_text : "", -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 4, last_message_actor_id ? last_message_actor_id : "", -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 5, last_message_actor_name ? last_message_actor_name : "", -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 6, last_message_type ? last_message_type : "", -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 7, last_message_params_json, -1, SQLITE_STATIC);
      sqlite3_bind_int64 (stmt, 8, account_id);
      sqlite3_bind_text (stmt, 9, token, -1, SQLITE_STATIC);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

int
gnalk_database_delete_stale_conversations (GnalkDatabase *self,
                                             gint64          account_id,
                                             GPtrArray      *valid_tokens)
{
  g_return_val_if_fail (GNALK_IS_DATABASE (self), 0);
  g_return_val_if_fail (self->db != NULL, 0);
  g_return_val_if_fail (valid_tokens != NULL, 0);

  if (valid_tokens->len == 0)
    return 0;

  /* Build SQL with placeholders for valid tokens */
  GString *sql = g_string_new ("DELETE FROM conversations WHERE account_id = ? AND token NOT IN (");
  for (guint i = 0; i < valid_tokens->len; i++)
    {
      if (i > 0)
        g_string_append (sql, ", ");
      g_string_append (sql, "?");
    }
  g_string_append (sql, ")");

  sqlite3_stmt *stmt;
  int deleted = 0;

  if (sqlite3_prepare_v2 (self->db, sql->str, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      for (guint i = 0; i < valid_tokens->len; i++)
        {
          const char *token = g_ptr_array_index (valid_tokens, i);
          sqlite3_bind_text (stmt, 2 + i, token, -1, SQLITE_STATIC);
        }

      if (sqlite3_step (stmt) == SQLITE_DONE)
        deleted = sqlite3_changes (self->db);

      sqlite3_finalize (stmt);
    }

  g_string_free (sql, TRUE);

  /* Also clean up related messages and sync_state for deleted conversations */
  if (deleted > 0)
    {
      GString *msg_sql = g_string_new ("DELETE FROM messages WHERE account_id = ? AND conversation_token NOT IN (");
      for (guint i = 0; i < valid_tokens->len; i++)
        {
          if (i > 0)
            g_string_append (msg_sql, ", ");
          g_string_append (msg_sql, "?");
        }
      g_string_append (msg_sql, ")");

      if (sqlite3_prepare_v2 (self->db, msg_sql->str, -1, &stmt, NULL) == SQLITE_OK)
        {
          sqlite3_bind_int64 (stmt, 1, account_id);
          for (guint i = 0; i < valid_tokens->len; i++)
            {
              const char *token = g_ptr_array_index (valid_tokens, i);
              sqlite3_bind_text (stmt, 2 + i, token, -1, SQLITE_STATIC);
            }
          sqlite3_step (stmt);
          sqlite3_finalize (stmt);
        }
      g_string_free (msg_sql, TRUE);

      GString *sync_sql = g_string_new ("DELETE FROM sync_state WHERE account_id = ? AND conversation_token NOT IN (");
      for (guint i = 0; i < valid_tokens->len; i++)
        {
          if (i > 0)
            g_string_append (sync_sql, ", ");
          g_string_append (sync_sql, "?");
        }
      g_string_append (sync_sql, ")");

      if (sqlite3_prepare_v2 (self->db, sync_sql->str, -1, &stmt, NULL) == SQLITE_OK)
        {
          sqlite3_bind_int64 (stmt, 1, account_id);
          for (guint i = 0; i < valid_tokens->len; i++)
            {
              const char *token = g_ptr_array_index (valid_tokens, i);
              sqlite3_bind_text (stmt, 2 + i, token, -1, SQLITE_STATIC);
            }
          sqlite3_step (stmt);
          sqlite3_finalize (stmt);
        }
      g_string_free (sync_sql, TRUE);
    }

  return deleted;
}

GListStore *
gnalk_database_load_conversations (GnalkDatabase *self,
                                     gint64          account_id)
{
  g_return_val_if_fail (GNALK_IS_DATABASE (self), NULL);
  g_return_val_if_fail (self->db != NULL, NULL);

  GListStore *store = g_list_store_new (GNALK_TYPE_CONVERSATION_DATA);
  sqlite3_stmt *stmt;

  const char *sql =
    "SELECT token, title, name, user_status, actor_id, room_type, "
    "last_activity, last_common_read_message, unread_messages, is_favorite, "
    "can_leave, can_delete, description, participant_type, notification_level, "
    "call_notification_level, last_read_message, last_message_id, "
    "last_message_text, last_message_actor_id, last_message_actor_name, "
    "last_message_type, last_message_params_json "
    "FROM conversations WHERE account_id = ? ORDER BY last_activity DESC";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);

      while (sqlite3_step (stmt) == SQLITE_ROW)
        {
          GnalkConversationData *data = gnalk_conversation_data_new_from_db (
            (const char *) sqlite3_column_text (stmt, 0),   /* token */
            (const char *) sqlite3_column_text (stmt, 1),   /* title */
            (const char *) sqlite3_column_text (stmt, 2),   /* name */
            (const char *) sqlite3_column_text (stmt, 3),   /* user_status */
            (const char *) sqlite3_column_text (stmt, 4),   /* actor_id */
            sqlite3_column_int (stmt, 5),                    /* room_type */
            sqlite3_column_int64 (stmt, 6),                  /* last_activity */
            sqlite3_column_int64 (stmt, 7),                  /* last_common_read_message */
            sqlite3_column_int (stmt, 8),                    /* unread_messages */
            sqlite3_column_int (stmt, 9) != 0,               /* is_favorite */
            sqlite3_column_int (stmt, 10) != 0,              /* can_leave */
            sqlite3_column_int (stmt, 11) != 0,              /* can_delete */
            (const char *) sqlite3_column_text (stmt, 12),  /* description */
            sqlite3_column_int (stmt, 13),                   /* participant_type */
            sqlite3_column_int (stmt, 14),                   /* notification_level */
            sqlite3_column_int (stmt, 15),                   /* call_notification_level */
            sqlite3_column_int64 (stmt, 16),                 /* last_read_message */
            sqlite3_column_int64 (stmt, 17),                 /* last_message_id */
            (const char *) sqlite3_column_text (stmt, 18),  /* last_message_text */
            (const char *) sqlite3_column_text (stmt, 19),  /* last_message_actor_id */
            (const char *) sqlite3_column_text (stmt, 20),  /* last_message_actor_name */
            (const char *) sqlite3_column_text (stmt, 21),  /* last_message_type */
            (const char *) sqlite3_column_text (stmt, 22),  /* last_message_params_json */
            NULL);                                           /* session (set later) */

          g_list_store_append (store, data);
          g_object_unref (data);
        }

      sqlite3_finalize (stmt);
    }

  return store;
}

void
gnalk_database_save_message (GnalkDatabase    *self,
                               gint64             account_id,
                               const char        *conversation_token,
                               GnalkMessageData *message)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);
  g_return_if_fail (conversation_token != NULL);
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (message));

  gint64 msg_id = gnalk_message_data_get_id (message);

  /* Skip pending messages (id = 0) */
  if (msg_id <= 0)
    return;

  sqlite3_stmt *stmt;
  const char *sql =
    "INSERT OR REPLACE INTO messages "
    "(id, account_id, conversation_token, timestamp, reply_to, last_edit_timestamp, "
    "message, author_id, author_name, reference_id, system_message, is_own, status, "
    "reactions_json, parent_json, message_parameters_json) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      char *reactions_json = gnalk_message_data_serialize_reactions (message);
      char *parent_json = gnalk_message_data_serialize_parent (message);
      char *params_json = gnalk_message_data_serialize_parameters (message);

      sqlite3_bind_int64 (stmt, 1, msg_id);
      sqlite3_bind_int64 (stmt, 2, account_id);
      sqlite3_bind_text (stmt, 3, conversation_token, -1, SQLITE_STATIC);
      sqlite3_bind_int64 (stmt, 4, gnalk_message_data_get_timestamp (message));
      sqlite3_bind_int64 (stmt, 5, gnalk_message_data_get_reply_to (message));
      sqlite3_bind_int64 (stmt, 6, gnalk_message_data_get_last_edit_timestamp (message));
      sqlite3_bind_text (stmt, 7, gnalk_message_data_get_message (message), -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 8, gnalk_message_data_get_author_id (message), -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 9, gnalk_message_data_get_author_name (message), -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 10, gnalk_message_data_get_reference_id (message), -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 11, gnalk_message_data_get_system_message (message), -1, SQLITE_STATIC);
      sqlite3_bind_int (stmt, 12, gnalk_message_data_get_is_own (message) ? 1 : 0);
      sqlite3_bind_int (stmt, 13, (int) gnalk_message_data_get_status (message));
      sqlite3_bind_text (stmt, 14, reactions_json, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text (stmt, 15, parent_json, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text (stmt, 16, params_json, -1, SQLITE_TRANSIENT);

      sqlite3_step (stmt);
      sqlite3_finalize (stmt);

      g_free (reactions_json);
      g_free (parent_json);
      g_free (params_json);
    }
}

void
gnalk_database_update_message (GnalkDatabase *self,
                                 gint64          message_id,
                                 const char     *message_text,
                                 gint64          last_edit_timestamp)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);

  sqlite3_stmt *stmt;
  const char *sql = "UPDATE messages SET message = ?, last_edit_timestamp = ? WHERE id = ?";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_text (stmt, 1, message_text, -1, SQLITE_STATIC);
      sqlite3_bind_int64 (stmt, 2, last_edit_timestamp);
      sqlite3_bind_int64 (stmt, 3, message_id);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

void
gnalk_database_update_message_reactions (GnalkDatabase *self,
                                           gint64          message_id,
                                           const char     *reactions_json)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);

  sqlite3_stmt *stmt;
  const char *sql = "UPDATE messages SET reactions_json = ? WHERE id = ?";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_text (stmt, 1, reactions_json, -1, SQLITE_STATIC);
      sqlite3_bind_int64 (stmt, 2, message_id);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

void
gnalk_database_update_message_status (GnalkDatabase        *self,
                                        gint64                 message_id,
                                        GnalkMessageDataStatus status)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);

  sqlite3_stmt *stmt;
  const char *sql = "UPDATE messages SET status = ? WHERE id = ?";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int (stmt, 1, (int) status);
      sqlite3_bind_int64 (stmt, 2, message_id);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

GListStore *
gnalk_database_load_messages (GnalkDatabase *self,
                                gint64          account_id,
                                const char     *conversation_token,
                                gint            limit,
                                gint64          before_id)
{
  g_return_val_if_fail (GNALK_IS_DATABASE (self), NULL);
  g_return_val_if_fail (self->db != NULL, NULL);
  g_return_val_if_fail (conversation_token != NULL, NULL);

  GListStore *store = g_list_store_new (GNALK_TYPE_MESSAGE_DATA);
  sqlite3_stmt *stmt;

  const char *sql;
  if (before_id > 0)
    {
      sql =
        "SELECT id, timestamp, reply_to, last_edit_timestamp, message, author_id, "
        "author_name, reference_id, system_message, is_own, status, reactions_json, "
        "parent_json, message_parameters_json "
        "FROM messages WHERE account_id = ? AND conversation_token = ? AND id < ? "
        "ORDER BY timestamp DESC LIMIT ?";
    }
  else
    {
      sql =
        "SELECT id, timestamp, reply_to, last_edit_timestamp, message, author_id, "
        "author_name, reference_id, system_message, is_own, status, reactions_json, "
        "parent_json, message_parameters_json "
        "FROM messages WHERE account_id = ? AND conversation_token = ? "
        "ORDER BY timestamp DESC LIMIT ?";
    }

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_bind_text (stmt, 2, conversation_token, -1, SQLITE_STATIC);

      if (before_id > 0)
        {
          sqlite3_bind_int64 (stmt, 3, before_id);
          sqlite3_bind_int (stmt, 4, limit);
        }
      else
        {
          sqlite3_bind_int (stmt, 3, limit);
        }

      /* Collect messages in reverse order (oldest first) */
      GPtrArray *messages = g_ptr_array_new_with_free_func (g_object_unref);

      while (sqlite3_step (stmt) == SQLITE_ROW)
        {
          GnalkMessageData *data = gnalk_message_data_new_from_db (
            sqlite3_column_int64 (stmt, 0),                  /* id */
            sqlite3_column_int64 (stmt, 1),                  /* timestamp */
            sqlite3_column_int64 (stmt, 2),                  /* reply_to */
            sqlite3_column_int64 (stmt, 3),                  /* last_edit_timestamp */
            (const char *) sqlite3_column_text (stmt, 4),    /* message */
            (const char *) sqlite3_column_text (stmt, 5),    /* author_id */
            (const char *) sqlite3_column_text (stmt, 6),    /* author_name */
            (const char *) sqlite3_column_text (stmt, 7),    /* reference_id */
            (const char *) sqlite3_column_text (stmt, 8),    /* system_message */
            sqlite3_column_int (stmt, 9) != 0,               /* is_own */
            (GnalkMessageDataStatus) sqlite3_column_int (stmt, 10),  /* status */
            (const char *) sqlite3_column_text (stmt, 11),   /* reactions_json */
            (const char *) sqlite3_column_text (stmt, 12),   /* parent_json */
            (const char *) sqlite3_column_text (stmt, 13));  /* message_parameters_json */

          g_ptr_array_add (messages, data);
        }

      sqlite3_finalize (stmt);

      /* Add to store in chronological order */
      for (gint i = messages->len - 1; i >= 0; i--)
        {
          GnalkMessageData *data = g_ptr_array_index (messages, i);
          g_list_store_append (store, data);
        }

      g_ptr_array_unref (messages);
    }

  return store;
}

void
gnalk_database_trim_messages (GnalkDatabase *self,
                                gint64          account_id,
                                const char     *conversation_token,
                                gint            max_count)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);
  g_return_if_fail (conversation_token != NULL);

  sqlite3_stmt *stmt;

  /* Delete oldest messages if count exceeds max */
  const char *sql =
    "DELETE FROM messages WHERE id IN ("
    "  SELECT id FROM messages "
    "  WHERE account_id = ? AND conversation_token = ? "
    "  ORDER BY timestamp ASC "
    "  LIMIT MAX(0, (SELECT COUNT(*) FROM messages "
    "               WHERE account_id = ? AND conversation_token = ?) - ?)"
    ")";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_bind_text (stmt, 2, conversation_token, -1, SQLITE_STATIC);
      sqlite3_bind_int64 (stmt, 3, account_id);
      sqlite3_bind_text (stmt, 4, conversation_token, -1, SQLITE_STATIC);
      sqlite3_bind_int (stmt, 5, max_count);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

gint64
gnalk_database_get_last_known_message_id (GnalkDatabase *self,
                                            gint64          account_id,
                                            const char     *conversation_token)
{
  g_return_val_if_fail (GNALK_IS_DATABASE (self), 0);
  g_return_val_if_fail (self->db != NULL, 0);
  g_return_val_if_fail (conversation_token != NULL, 0);

  gint64 message_id = 0;
  sqlite3_stmt *stmt;

  const char *sql =
    "SELECT last_known_message_id FROM sync_state "
    "WHERE account_id = ? AND conversation_token = ?";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_bind_text (stmt, 2, conversation_token, -1, SQLITE_STATIC);

      if (sqlite3_step (stmt) == SQLITE_ROW)
        message_id = sqlite3_column_int64 (stmt, 0);

      sqlite3_finalize (stmt);
    }

  return message_id;
}

void
gnalk_database_set_last_known_message_id (GnalkDatabase *self,
                                            gint64          account_id,
                                            const char     *conversation_token,
                                            gint64          message_id)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);
  g_return_if_fail (conversation_token != NULL);

  sqlite3_stmt *stmt;
  const char *sql =
    "INSERT INTO sync_state (account_id, conversation_token, last_known_message_id) "
    "VALUES (?, ?, ?) "
    "ON CONFLICT(account_id, conversation_token) DO UPDATE SET last_known_message_id = ?";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_bind_text (stmt, 2, conversation_token, -1, SQLITE_STATIC);
      sqlite3_bind_int64 (stmt, 3, message_id);
      sqlite3_bind_int64 (stmt, 4, message_id);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

gboolean
gnalk_database_has_more_history (GnalkDatabase *self,
                                   gint64          account_id,
                                   const char     *conversation_token)
{
  g_return_val_if_fail (GNALK_IS_DATABASE (self), TRUE);
  g_return_val_if_fail (self->db != NULL, TRUE);
  g_return_val_if_fail (conversation_token != NULL, TRUE);

  gboolean has_more = TRUE;
  sqlite3_stmt *stmt;

  const char *sql =
    "SELECT has_more_history FROM sync_state "
    "WHERE account_id = ? AND conversation_token = ?";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_bind_text (stmt, 2, conversation_token, -1, SQLITE_STATIC);

      if (sqlite3_step (stmt) == SQLITE_ROW)
        has_more = sqlite3_column_int (stmt, 0) != 0;

      sqlite3_finalize (stmt);
    }

  return has_more;
}

void
gnalk_database_set_has_more_history (GnalkDatabase *self,
                                       gint64          account_id,
                                       const char     *conversation_token,
                                       gboolean        has_more)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);
  g_return_if_fail (conversation_token != NULL);

  sqlite3_stmt *stmt;
  const char *sql =
    "INSERT INTO sync_state (account_id, conversation_token, has_more_history) "
    "VALUES (?, ?, ?) "
    "ON CONFLICT(account_id, conversation_token) DO UPDATE SET has_more_history = ?";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_bind_text (stmt, 2, conversation_token, -1, SQLITE_STATIC);
      sqlite3_bind_int (stmt, 3, has_more ? 1 : 0);
      sqlite3_bind_int (stmt, 4, has_more ? 1 : 0);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

void
gnalk_database_queue_pending_message (GnalkDatabase *self,
                                        gint64          account_id,
                                        const char     *conversation_token,
                                        const char     *message,
                                        gint64          reply_to,
                                        const char     *reference_id)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);
  g_return_if_fail (conversation_token != NULL);
  g_return_if_fail (reference_id != NULL);

  sqlite3_stmt *stmt;
  const char *sql =
    "INSERT OR REPLACE INTO pending_messages "
    "(account_id, conversation_token, message, reply_to, reference_id) "
    "VALUES (?, ?, ?, ?, ?)";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_bind_text (stmt, 2, conversation_token, -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 3, message, -1, SQLITE_STATIC);
      sqlite3_bind_int64 (stmt, 4, reply_to);
      sqlite3_bind_text (stmt, 5, reference_id, -1, SQLITE_STATIC);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

void
gnalk_database_queue_pending_file (GnalkDatabase *self,
                                     gint64          account_id,
                                     const char     *conversation_token,
                                     const char     *file_path,
                                     const char     *file_name,
                                     const char     *content_type,
                                     const char     *caption,
                                     gint64          reply_to,
                                     const char     *reference_id)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);
  g_return_if_fail (conversation_token != NULL);
  g_return_if_fail (reference_id != NULL);

  sqlite3_stmt *stmt;
  const char *sql =
    "INSERT OR REPLACE INTO pending_messages "
    "(account_id, conversation_token, message, reply_to, reference_id, "
    "file_path, file_name, content_type) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_bind_text (stmt, 2, conversation_token, -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 3, caption, -1, SQLITE_STATIC);
      sqlite3_bind_int64 (stmt, 4, reply_to);
      sqlite3_bind_text (stmt, 5, reference_id, -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 6, file_path, -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 7, file_name, -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 8, content_type, -1, SQLITE_STATIC);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

/* Public pending message free function */
void
gnalk_pending_message_free (GnalkPendingMessage *pm)
{
  if (!pm)
    return;
  g_free (pm->conversation_token);
  g_free (pm->message);
  g_free (pm->reference_id);
  g_free (pm->file_path);
  g_free (pm->file_name);
  g_free (pm->content_type);
  g_free (pm->last_error);
  g_free (pm);
}

GPtrArray *
gnalk_database_get_pending_messages (GnalkDatabase *self,
                                       gint64          account_id)
{
  g_return_val_if_fail (GNALK_IS_DATABASE (self), NULL);
  g_return_val_if_fail (self->db != NULL, NULL);

  GPtrArray *array = g_ptr_array_new_with_free_func ((GDestroyNotify) gnalk_pending_message_free);
  sqlite3_stmt *stmt;

  const char *sql =
    "SELECT id, conversation_token, message, reply_to, reference_id, "
    "file_path, file_name, content_type, retry_count, last_error "
    "FROM pending_messages WHERE account_id = ? ORDER BY created_at ASC";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);

      while (sqlite3_step (stmt) == SQLITE_ROW)
        {
          GnalkPendingMessage *pm = g_new0 (GnalkPendingMessage, 1);
          pm->id = sqlite3_column_int64 (stmt, 0);
          pm->conversation_token = g_strdup ((const char *) sqlite3_column_text (stmt, 1));
          pm->message = g_strdup ((const char *) sqlite3_column_text (stmt, 2));
          pm->reply_to = sqlite3_column_int64 (stmt, 3);
          pm->reference_id = g_strdup ((const char *) sqlite3_column_text (stmt, 4));
          pm->file_path = g_strdup ((const char *) sqlite3_column_text (stmt, 5));
          pm->file_name = g_strdup ((const char *) sqlite3_column_text (stmt, 6));
          pm->content_type = g_strdup ((const char *) sqlite3_column_text (stmt, 7));
          pm->retry_count = sqlite3_column_int (stmt, 8);
          pm->last_error = g_strdup ((const char *) sqlite3_column_text (stmt, 9));
          g_ptr_array_add (array, pm);
        }

      sqlite3_finalize (stmt);
    }

  return array;
}

void
gnalk_database_remove_pending_message (GnalkDatabase *self,
                                         const char     *reference_id)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);
  g_return_if_fail (reference_id != NULL);

  sqlite3_stmt *stmt;
  const char *sql = "DELETE FROM pending_messages WHERE reference_id = ?";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_text (stmt, 1, reference_id, -1, SQLITE_STATIC);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

void
gnalk_database_update_pending_error (GnalkDatabase *self,
                                       const char     *reference_id,
                                       const char     *error)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);
  g_return_if_fail (reference_id != NULL);

  sqlite3_stmt *stmt;
  const char *sql =
    "UPDATE pending_messages SET retry_count = retry_count + 1, last_error = ? "
    "WHERE reference_id = ?";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_text (stmt, 1, error, -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 2, reference_id, -1, SQLITE_STATIC);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

/* Multi-account API */

GListStore *
gnalk_database_load_all_accounts (GnalkDatabase *self)
{
  g_return_val_if_fail (GNALK_IS_DATABASE (self), NULL);
  g_return_val_if_fail (self->db != NULL, NULL);

  GListStore *store = g_list_store_new (GNALK_TYPE_ACCOUNT);
  sqlite3_stmt *stmt;

  const char *sql =
    "SELECT id, server_url, user_id, user_name, avatar_url, "
    "COALESCE(enabled, 1), COALESCE(autologin, 0), goa_account_id "
    "FROM accounts ORDER BY id";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      while (sqlite3_step (stmt) == SQLITE_ROW)
        {
          gint64 id = sqlite3_column_int64 (stmt, 0);
          const char *server_url = (const char *) sqlite3_column_text (stmt, 1);
          const char *user_id = (const char *) sqlite3_column_text (stmt, 2);
          const char *display_name = (const char *) sqlite3_column_text (stmt, 3);
          const char *avatar_url = (const char *) sqlite3_column_text (stmt, 4);
          gboolean enabled = sqlite3_column_int (stmt, 5) != 0;
          gboolean autologin = sqlite3_column_int (stmt, 6) != 0;
          const char *goa_account_id = (const char *) sqlite3_column_text (stmt, 7);

          GnalkAccount *account = gnalk_account_new_full (id, server_url, user_id,
                                                             display_name, avatar_url,
                                                             enabled, autologin,
                                                             goa_account_id);
          g_list_store_append (store, account);
          g_object_unref (account);
        }

      sqlite3_finalize (stmt);
    }

  return store;
}

gint64
gnalk_database_create_account (GnalkDatabase *self,
                                 const char     *server_url,
                                 const char     *user_id)
{
  g_return_val_if_fail (GNALK_IS_DATABASE (self), -1);
  g_return_val_if_fail (self->db != NULL, -1);
  g_return_val_if_fail (server_url != NULL, -1);
  g_return_val_if_fail (user_id != NULL, -1);

  sqlite3_stmt *stmt;
  gint64 account_id = -1;

  /* Check if account already exists */
  const char *select_sql = "SELECT id FROM accounts WHERE server_url = ? AND user_id = ?";
  if (sqlite3_prepare_v2 (self->db, select_sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_text (stmt, 1, server_url, -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 2, user_id, -1, SQLITE_STATIC);

      if (sqlite3_step (stmt) == SQLITE_ROW)
        account_id = sqlite3_column_int64 (stmt, 0);

      sqlite3_finalize (stmt);
    }

  if (account_id > 0)
    return account_id;

  /* Create new account */
  const char *insert_sql =
    "INSERT INTO accounts (server_url, user_id, enabled, autologin) VALUES (?, ?, 1, 0)";
  if (sqlite3_prepare_v2 (self->db, insert_sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_text (stmt, 1, server_url, -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 2, user_id, -1, SQLITE_STATIC);

      if (sqlite3_step (stmt) == SQLITE_DONE)
        account_id = sqlite3_last_insert_rowid (self->db);

      sqlite3_finalize (stmt);
    }

  return account_id;
}

void
gnalk_database_save_account (GnalkDatabase *self,
                               GnalkAccount  *account)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);
  g_return_if_fail (GNALK_IS_ACCOUNT (account));

  /* Don't persist guest accounts (id=-1) */
  if (gnalk_account_get_id (account) < 0)
    return;

  sqlite3_stmt *stmt;
  const char *sql =
    "UPDATE accounts SET user_name = ?, avatar_url = ?, enabled = ?, autologin = ?, "
    "goa_account_id = ? WHERE id = ?";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_text (stmt, 1, gnalk_account_get_display_name (account), -1, SQLITE_STATIC);
      sqlite3_bind_text (stmt, 2, gnalk_account_get_avatar_url (account), -1, SQLITE_STATIC);
      sqlite3_bind_int (stmt, 3, gnalk_account_get_enabled (account) ? 1 : 0);
      sqlite3_bind_int (stmt, 4, gnalk_account_get_autologin (account) ? 1 : 0);
      sqlite3_bind_text (stmt, 5, gnalk_account_get_goa_account_id (account), -1, SQLITE_STATIC);
      sqlite3_bind_int64 (stmt, 6, gnalk_account_get_id (account));
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

void
gnalk_database_delete_account (GnalkDatabase *self,
                                 gint64          account_id)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);

  sqlite3_stmt *stmt;

  /* Delete account - foreign keys will cascade to delete conversations, etc. */
  const char *sql = "DELETE FROM accounts WHERE id = ?";
  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }

  /* Also clean up messages and sync_state (not linked via FK) */
  const char *msg_sql = "DELETE FROM messages WHERE account_id = ?";
  if (sqlite3_prepare_v2 (self->db, msg_sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }

  const char *sync_sql = "DELETE FROM sync_state WHERE account_id = ?";
  if (sqlite3_prepare_v2 (self->db, sync_sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }

  const char *pending_sql = "DELETE FROM pending_messages WHERE account_id = ?";
  if (sqlite3_prepare_v2 (self->db, pending_sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }

  const char *participants_sql = "DELETE FROM participants WHERE account_id = ?";
  if (sqlite3_prepare_v2 (self->db, participants_sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

/* Participants */

void
gnalk_database_save_participants (GnalkDatabase *self,
                                    gint64          account_id,
                                    const char     *conversation_token,
                                    JsonArray      *participants)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);
  g_return_if_fail (conversation_token != NULL);

  if (!participants)
    return;

  sqlite3_stmt *stmt;

  /* Delete existing participants for this conversation */
  const char *delete_sql = "DELETE FROM participants WHERE account_id = ? AND conversation_token = ?";
  if (sqlite3_prepare_v2 (self->db, delete_sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_bind_text (stmt, 2, conversation_token, -1, SQLITE_STATIC);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }

  /* Insert new participants */
  const char *insert_sql =
    "INSERT INTO participants (account_id, conversation_token, actor_id, display_name, status, in_call) "
    "VALUES (?, ?, ?, ?, ?, ?)";

  guint n_participants = json_array_get_length (participants);
  for (guint i = 0; i < n_participants; i++)
    {
      JsonObject *participant = json_array_get_object_element (participants, i);
      const char *actor_id = json_object_get_string_member_with_default (participant, "actorId", "");

      /* Skip guests (empty actorId) */
      if (!actor_id || !*actor_id)
        continue;

      const char *display_name = json_object_get_string_member_with_default (participant, "displayName", "");
      const char *status = json_object_get_string_member_with_default (participant, "status", "");
      gint64 in_call = json_object_get_int_member_with_default (participant, "inCall", 0);

      if (sqlite3_prepare_v2 (self->db, insert_sql, -1, &stmt, NULL) == SQLITE_OK)
        {
          sqlite3_bind_int64 (stmt, 1, account_id);
          sqlite3_bind_text (stmt, 2, conversation_token, -1, SQLITE_STATIC);
          sqlite3_bind_text (stmt, 3, actor_id, -1, SQLITE_STATIC);
          sqlite3_bind_text (stmt, 4, display_name, -1, SQLITE_STATIC);
          sqlite3_bind_text (stmt, 5, status, -1, SQLITE_STATIC);
          sqlite3_bind_int64 (stmt, 6, in_call);
          sqlite3_step (stmt);
          sqlite3_finalize (stmt);
        }
    }
}

JsonArray *
gnalk_database_load_participants (GnalkDatabase *self,
                                    gint64          account_id,
                                    const char     *conversation_token)
{
  g_return_val_if_fail (GNALK_IS_DATABASE (self), NULL);
  g_return_val_if_fail (self->db != NULL, NULL);
  g_return_val_if_fail (conversation_token != NULL, NULL);

  JsonArray *array = json_array_new ();
  sqlite3_stmt *stmt;

  const char *sql =
    "SELECT actor_id, display_name, status, in_call FROM participants "
    "WHERE account_id = ? AND conversation_token = ? "
    "ORDER BY display_name";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_bind_text (stmt, 2, conversation_token, -1, SQLITE_STATIC);

      while (sqlite3_step (stmt) == SQLITE_ROW)
        {
          JsonObject *obj = json_object_new ();
          json_object_set_string_member (obj, "actorId",
                                          (const char *) sqlite3_column_text (stmt, 0));
          json_object_set_string_member (obj, "displayName",
                                          (const char *) sqlite3_column_text (stmt, 1));
          const char *status = (const char *) sqlite3_column_text (stmt, 2);
          json_object_set_string_member (obj, "status", status ? status : "");
          json_object_set_int_member (obj, "inCall", sqlite3_column_int64 (stmt, 3));
          json_array_add_object_element (array, obj);
        }

      sqlite3_finalize (stmt);
    }

  return array;
}

void
gnalk_database_delete_participants (GnalkDatabase *self,
                                      gint64          account_id,
                                      const char     *conversation_token)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);
  g_return_if_fail (conversation_token != NULL);

  sqlite3_stmt *stmt;
  const char *sql = "DELETE FROM participants WHERE account_id = ? AND conversation_token = ?";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_bind_text (stmt, 2, conversation_token, -1, SQLITE_STATIC);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}

void
gnalk_database_reset_participants_status (GnalkDatabase *self,
                                            gint64          account_id)
{
  g_return_if_fail (GNALK_IS_DATABASE (self));
  g_return_if_fail (self->db != NULL);

  /* Reset status to empty and in_call to 0 for all participants of this account
   * (we can't know real status when offline) */
  sqlite3_stmt *stmt;
  const char *sql = "UPDATE participants SET status = '', in_call = 0 WHERE account_id = ?";

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) == SQLITE_OK)
    {
      sqlite3_bind_int64 (stmt, 1, account_id);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
}
