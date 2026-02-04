#include "gnalk-data-repository.h"

struct _GnalkDataRepository
{
  GObject parent_instance;

  GnalkDatabase *database;
  gboolean        is_open;
};

G_DEFINE_FINAL_TYPE (GnalkDataRepository, gnalk_data_repository, G_TYPE_OBJECT)

enum {
  SIGNAL_DATA_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Task data structures for passing parameters to thread functions */

typedef struct {
  GnalkDataRepository         *repository;
  GnalkDataRepositoryCallback  callback;
  gpointer                      user_data;
} TaskContext;

typedef struct {
  gint64 account_id;
} AccountIdData;

typedef struct {
  gint64  account_id;
  char   *conversation_token;
} ConversationKeyData;

typedef struct {
  gint64                  account_id;
  char                   *conversation_token;
  gint                    limit;
  gint64                  before_id;
} LoadMessagesData;

typedef struct {
  gint64                  account_id;
  GnalkConversationData *conversation;
} SaveConversationData;

typedef struct {
  gint64             account_id;
  char              *conversation_token;
  GnalkMessageData *message;
} SaveMessageData;

typedef struct {
  gint64     account_id;
  char      *conversation_token;
  JsonArray *participants;
} SaveParticipantsData;

typedef struct {
  gint64  account_id;
  char   *conversation_token;
  gint64  message_id;
} SetSyncStateData;

static TaskContext *
task_context_new (GnalkDataRepository         *repository,
                  GnalkDataRepositoryCallback  callback,
                  gpointer                      user_data)
{
  TaskContext *ctx = g_new0 (TaskContext, 1);
  ctx->repository = g_object_ref (repository);
  ctx->callback = callback;
  ctx->user_data = user_data;
  return ctx;
}

static void
task_context_free (TaskContext *ctx)
{
  g_clear_object (&ctx->repository);
  g_free (ctx);
}

static void
account_id_data_free (AccountIdData *data)
{
  g_free (data);
}

static void
conversation_key_data_free (ConversationKeyData *data)
{
  g_free (data->conversation_token);
  g_free (data);
}

static void
load_messages_data_free (LoadMessagesData *data)
{
  g_free (data->conversation_token);
  g_free (data);
}

static void
save_conversation_data_free (SaveConversationData *data)
{
  g_clear_object (&data->conversation);
  g_free (data);
}

static void
save_message_data_free (SaveMessageData *data)
{
  g_free (data->conversation_token);
  g_clear_object (&data->message);
  g_free (data);
}

static void
save_participants_data_free (SaveParticipantsData *data)
{
  g_free (data->conversation_token);
  if (data->participants)
    json_array_unref (data->participants);
  g_free (data);
}

static void
set_sync_state_data_free (SetSyncStateData *data)
{
  g_free (data->conversation_token);
  g_free (data);
}

/* Generic callback invocation in main thread */
static gboolean
invoke_callback_idle (gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  TaskContext *ctx = g_task_get_source_tag (task);
  GError *error = NULL;
  gpointer result = g_task_propagate_pointer (task, &error);

  if (ctx->callback)
    ctx->callback (ctx->repository, result, error, ctx->user_data);

  g_clear_error (&error);
  task_context_free (ctx);
  g_object_unref (task);

  return G_SOURCE_REMOVE;
}

static void
complete_task_in_main_thread (GTask    *task,
                              gpointer  result,
                              GError   *error)
{
  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, result, NULL);

  g_idle_add (invoke_callback_idle, task);
}

/* Lifecycle */

static void
gnalk_data_repository_finalize (GObject *object)
{
  GnalkDataRepository *self = GNALK_DATA_REPOSITORY (object);

  g_clear_object (&self->database);

  G_OBJECT_CLASS (gnalk_data_repository_parent_class)->finalize (object);
}

static void
gnalk_data_repository_class_init (GnalkDataRepositoryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gnalk_data_repository_finalize;

  /**
   * GnalkDataRepository::data-changed:
   * @repository: The data repository
   * @scope: The scope of the change ("conversations", "messages:TOKEN", "participants:TOKEN")
   *
   * Emitted when data has been modified through this repository.
   */
  signals[SIGNAL_DATA_CHANGED] =
    g_signal_new ("data-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);
}

static void
gnalk_data_repository_init (GnalkDataRepository *self)
{
  self->database = gnalk_database_new ();
  self->is_open = FALSE;
}

GnalkDataRepository *
gnalk_data_repository_new (void)
{
  return g_object_new (GNALK_TYPE_DATA_REPOSITORY, NULL);
}

gboolean
gnalk_data_repository_is_open (GnalkDataRepository *self)
{
  g_return_val_if_fail (GNALK_IS_DATA_REPOSITORY (self), FALSE);
  return self->is_open;
}

GnalkDatabase *
gnalk_data_repository_get_database (GnalkDataRepository *self)
{
  g_return_val_if_fail (GNALK_IS_DATA_REPOSITORY (self), NULL);
  return self->database;
}

/* Open async */

static void
open_thread_func (GTask        *task,
                  gpointer      source_object,
                  gpointer      task_data G_GNUC_UNUSED,
                  GCancellable *cancellable G_GNUC_UNUSED)
{
  GnalkDataRepository *self = GNALK_DATA_REPOSITORY (source_object);
  GError *error = NULL;

  gboolean success = gnalk_database_open (self->database, &error);
  if (success)
    self->is_open = TRUE;

  complete_task_in_main_thread (task, GINT_TO_POINTER (success), error);
}

void
gnalk_data_repository_open_async (GnalkDataRepository          *self,
                                    GnalkDataRepositoryCallback   callback,
                                    gpointer                       user_data)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));

  TaskContext *ctx = task_context_new (self, callback, user_data);
  GTask *task = g_task_new (self, NULL, NULL, ctx);
  g_task_set_source_tag (task, ctx);
  g_task_run_in_thread (task, open_thread_func);
}

/* Load accounts async */

static void
load_accounts_thread_func (GTask        *task,
                           gpointer      source_object,
                           gpointer      task_data G_GNUC_UNUSED,
                           GCancellable *cancellable G_GNUC_UNUSED)
{
  GnalkDataRepository *self = GNALK_DATA_REPOSITORY (source_object);

  if (!self->is_open)
    {
      GError *error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                                           "Database not open");
      complete_task_in_main_thread (task, NULL, error);
      return;
    }

  GListStore *accounts = gnalk_database_load_all_accounts (self->database);
  complete_task_in_main_thread (task, accounts, NULL);
}

void
gnalk_data_repository_load_accounts_async (GnalkDataRepository          *self,
                                              GnalkDataRepositoryCallback   callback,
                                              gpointer                       user_data)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));

  TaskContext *ctx = task_context_new (self, callback, user_data);
  GTask *task = g_task_new (self, NULL, NULL, ctx);
  g_task_set_source_tag (task, ctx);
  g_task_run_in_thread (task, load_accounts_thread_func);
}

/* Load conversations async */

static void
load_conversations_thread_func (GTask        *task,
                                 gpointer      source_object,
                                 gpointer      task_data,
                                 GCancellable *cancellable G_GNUC_UNUSED)
{
  GnalkDataRepository *self = GNALK_DATA_REPOSITORY (source_object);
  AccountIdData *data = task_data;

  if (!self->is_open)
    {
      GError *error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                                           "Database not open");
      complete_task_in_main_thread (task, NULL, error);
      return;
    }

  GListStore *conversations = gnalk_database_load_conversations (self->database, data->account_id);
  complete_task_in_main_thread (task, conversations, NULL);
}

void
gnalk_data_repository_load_conversations_async (GnalkDataRepository          *self,
                                                   gint64                         account_id,
                                                   GnalkDataRepositoryCallback   callback,
                                                   gpointer                        user_data)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));

  AccountIdData *data = g_new0 (AccountIdData, 1);
  data->account_id = account_id;

  TaskContext *ctx = task_context_new (self, callback, user_data);
  GTask *task = g_task_new (self, NULL, NULL, ctx);
  g_task_set_source_tag (task, ctx);
  g_task_set_task_data (task, data, (GDestroyNotify) account_id_data_free);
  g_task_run_in_thread (task, load_conversations_thread_func);
}

/* Save conversation async */

static void
save_conversation_thread_func (GTask        *task,
                                gpointer      source_object,
                                gpointer      task_data,
                                GCancellable *cancellable G_GNUC_UNUSED)
{
  GnalkDataRepository *self = GNALK_DATA_REPOSITORY (source_object);
  SaveConversationData *data = task_data;

  if (!self->is_open)
    {
      GError *error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                                           "Database not open");
      complete_task_in_main_thread (task, NULL, error);
      return;
    }

  gnalk_database_save_conversation (self->database, data->account_id, data->conversation);
  complete_task_in_main_thread (task, NULL, NULL);
}

void
gnalk_data_repository_save_conversation_async (GnalkDataRepository          *self,
                                                  gint64                         account_id,
                                                  GnalkConversationData        *conversation,
                                                  GnalkDataRepositoryCallback   callback,
                                                  gpointer                        user_data)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));
  g_return_if_fail (GNALK_IS_CONVERSATION_DATA (conversation));

  SaveConversationData *data = g_new0 (SaveConversationData, 1);
  data->account_id = account_id;
  data->conversation = g_object_ref (conversation);

  TaskContext *ctx = task_context_new (self, callback, user_data);
  GTask *task = g_task_new (self, NULL, NULL, ctx);
  g_task_set_source_tag (task, ctx);
  g_task_set_task_data (task, data, (GDestroyNotify) save_conversation_data_free);
  g_task_run_in_thread (task, save_conversation_thread_func);
}

/* Load messages async */

static void
load_messages_thread_func (GTask        *task,
                           gpointer      source_object,
                           gpointer      task_data,
                           GCancellable *cancellable G_GNUC_UNUSED)
{
  GnalkDataRepository *self = GNALK_DATA_REPOSITORY (source_object);
  LoadMessagesData *data = task_data;

  if (!self->is_open)
    {
      GError *error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                                           "Database not open");
      complete_task_in_main_thread (task, NULL, error);
      return;
    }

  GListStore *messages = gnalk_database_load_messages (self->database,
                                                          data->account_id,
                                                          data->conversation_token,
                                                          data->limit,
                                                          data->before_id);
  complete_task_in_main_thread (task, messages, NULL);
}

void
gnalk_data_repository_load_messages_async (GnalkDataRepository          *self,
                                              gint64                         account_id,
                                              const char                    *conversation_token,
                                              gint                           limit,
                                              gint64                         before_id,
                                              GnalkDataRepositoryCallback   callback,
                                              gpointer                        user_data)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));
  g_return_if_fail (conversation_token != NULL);

  LoadMessagesData *data = g_new0 (LoadMessagesData, 1);
  data->account_id = account_id;
  data->conversation_token = g_strdup (conversation_token);
  data->limit = limit;
  data->before_id = before_id;

  TaskContext *ctx = task_context_new (self, callback, user_data);
  GTask *task = g_task_new (self, NULL, NULL, ctx);
  g_task_set_source_tag (task, ctx);
  g_task_set_task_data (task, data, (GDestroyNotify) load_messages_data_free);
  g_task_run_in_thread (task, load_messages_thread_func);
}

/* Save message async */

static void
save_message_thread_func (GTask        *task,
                          gpointer      source_object,
                          gpointer      task_data,
                          GCancellable *cancellable G_GNUC_UNUSED)
{
  GnalkDataRepository *self = GNALK_DATA_REPOSITORY (source_object);
  SaveMessageData *data = task_data;

  if (!self->is_open)
    {
      GError *error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                                           "Database not open");
      complete_task_in_main_thread (task, NULL, error);
      return;
    }

  gnalk_database_save_message (self->database,
                                  data->account_id,
                                  data->conversation_token,
                                  data->message);
  complete_task_in_main_thread (task, NULL, NULL);
}

void
gnalk_data_repository_save_message_async (GnalkDataRepository          *self,
                                             gint64                         account_id,
                                             const char                    *conversation_token,
                                             GnalkMessageData             *message,
                                             GnalkDataRepositoryCallback   callback,
                                             gpointer                        user_data)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));
  g_return_if_fail (conversation_token != NULL);
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (message));

  SaveMessageData *data = g_new0 (SaveMessageData, 1);
  data->account_id = account_id;
  data->conversation_token = g_strdup (conversation_token);
  data->message = g_object_ref (message);

  TaskContext *ctx = task_context_new (self, callback, user_data);
  GTask *task = g_task_new (self, NULL, NULL, ctx);
  g_task_set_source_tag (task, ctx);
  g_task_set_task_data (task, data, (GDestroyNotify) save_message_data_free);
  g_task_run_in_thread (task, save_message_thread_func);
}

/* Load participants async */

static void
load_participants_thread_func (GTask        *task,
                               gpointer      source_object,
                               gpointer      task_data,
                               GCancellable *cancellable G_GNUC_UNUSED)
{
  GnalkDataRepository *self = GNALK_DATA_REPOSITORY (source_object);
  ConversationKeyData *data = task_data;

  if (!self->is_open)
    {
      GError *error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                                           "Database not open");
      complete_task_in_main_thread (task, NULL, error);
      return;
    }

  JsonArray *participants = gnalk_database_load_participants (self->database,
                                                                  data->account_id,
                                                                  data->conversation_token);
  complete_task_in_main_thread (task, participants, NULL);
}

void
gnalk_data_repository_load_participants_async (GnalkDataRepository          *self,
                                                  gint64                         account_id,
                                                  const char                    *conversation_token,
                                                  GnalkDataRepositoryCallback   callback,
                                                  gpointer                        user_data)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));
  g_return_if_fail (conversation_token != NULL);

  ConversationKeyData *data = g_new0 (ConversationKeyData, 1);
  data->account_id = account_id;
  data->conversation_token = g_strdup (conversation_token);

  TaskContext *ctx = task_context_new (self, callback, user_data);
  GTask *task = g_task_new (self, NULL, NULL, ctx);
  g_task_set_source_tag (task, ctx);
  g_task_set_task_data (task, data, (GDestroyNotify) conversation_key_data_free);
  g_task_run_in_thread (task, load_participants_thread_func);
}

/* Save participants async */

static void
save_participants_thread_func (GTask        *task,
                               gpointer      source_object,
                               gpointer      task_data,
                               GCancellable *cancellable G_GNUC_UNUSED)
{
  GnalkDataRepository *self = GNALK_DATA_REPOSITORY (source_object);
  SaveParticipantsData *data = task_data;

  if (!self->is_open)
    {
      GError *error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                                           "Database not open");
      complete_task_in_main_thread (task, NULL, error);
      return;
    }

  gnalk_database_save_participants (self->database,
                                       data->account_id,
                                       data->conversation_token,
                                       data->participants);
  complete_task_in_main_thread (task, NULL, NULL);
}

void
gnalk_data_repository_save_participants_async (GnalkDataRepository          *self,
                                                  gint64                         account_id,
                                                  const char                    *conversation_token,
                                                  JsonArray                     *participants,
                                                  GnalkDataRepositoryCallback   callback,
                                                  gpointer                        user_data)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));
  g_return_if_fail (conversation_token != NULL);

  SaveParticipantsData *data = g_new0 (SaveParticipantsData, 1);
  data->account_id = account_id;
  data->conversation_token = g_strdup (conversation_token);
  data->participants = participants ? json_array_ref (participants) : NULL;

  TaskContext *ctx = task_context_new (self, callback, user_data);
  GTask *task = g_task_new (self, NULL, NULL, ctx);
  g_task_set_source_tag (task, ctx);
  g_task_set_task_data (task, data, (GDestroyNotify) save_participants_data_free);
  g_task_run_in_thread (task, save_participants_thread_func);
}

/* Get last known message ID async */

static void
get_last_known_message_id_thread_func (GTask        *task,
                                        gpointer      source_object,
                                        gpointer      task_data,
                                        GCancellable *cancellable G_GNUC_UNUSED)
{
  GnalkDataRepository *self = GNALK_DATA_REPOSITORY (source_object);
  ConversationKeyData *data = task_data;

  if (!self->is_open)
    {
      GError *error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                                           "Database not open");
      complete_task_in_main_thread (task, NULL, error);
      return;
    }

  gint64 message_id = gnalk_database_get_last_known_message_id (self->database,
                                                                    data->account_id,
                                                                    data->conversation_token);

  gint64 *result = g_new (gint64, 1);
  *result = message_id;
  complete_task_in_main_thread (task, result, NULL);
}

void
gnalk_data_repository_get_last_known_message_id_async (GnalkDataRepository          *self,
                                                          gint64                         account_id,
                                                          const char                    *conversation_token,
                                                          GnalkDataRepositoryCallback   callback,
                                                          gpointer                        user_data)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));
  g_return_if_fail (conversation_token != NULL);

  ConversationKeyData *data = g_new0 (ConversationKeyData, 1);
  data->account_id = account_id;
  data->conversation_token = g_strdup (conversation_token);

  TaskContext *ctx = task_context_new (self, callback, user_data);
  GTask *task = g_task_new (self, NULL, NULL, ctx);
  g_task_set_source_tag (task, ctx);
  g_task_set_task_data (task, data, (GDestroyNotify) conversation_key_data_free);
  g_task_run_in_thread (task, get_last_known_message_id_thread_func);
}

/* Set last known message ID async */

static void
set_last_known_message_id_thread_func (GTask        *task,
                                        gpointer      source_object,
                                        gpointer      task_data,
                                        GCancellable *cancellable G_GNUC_UNUSED)
{
  GnalkDataRepository *self = GNALK_DATA_REPOSITORY (source_object);
  SetSyncStateData *data = task_data;

  if (!self->is_open)
    {
      GError *error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                                           "Database not open");
      complete_task_in_main_thread (task, NULL, error);
      return;
    }

  gnalk_database_set_last_known_message_id (self->database,
                                               data->account_id,
                                               data->conversation_token,
                                               data->message_id);
  complete_task_in_main_thread (task, NULL, NULL);
}

void
gnalk_data_repository_set_last_known_message_id_async (GnalkDataRepository          *self,
                                                          gint64                         account_id,
                                                          const char                    *conversation_token,
                                                          gint64                         message_id,
                                                          GnalkDataRepositoryCallback   callback,
                                                          gpointer                        user_data)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));
  g_return_if_fail (conversation_token != NULL);

  SetSyncStateData *data = g_new0 (SetSyncStateData, 1);
  data->account_id = account_id;
  data->conversation_token = g_strdup (conversation_token);
  data->message_id = message_id;

  TaskContext *ctx = task_context_new (self, callback, user_data);
  GTask *task = g_task_new (self, NULL, NULL, ctx);
  g_task_set_source_tag (task, ctx);
  g_task_set_task_data (task, data, (GDestroyNotify) set_sync_state_data_free);
  g_task_run_in_thread (task, set_last_known_message_id_thread_func);
}

/* Synchronous operations */

void
gnalk_data_repository_save_conversation (GnalkDataRepository   *self,
                                         gint64                 account_id,
                                         GnalkConversationData *conversation)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));
  g_return_if_fail (self->is_open);
  g_return_if_fail (GNALK_IS_CONVERSATION_DATA (conversation));

  gnalk_database_save_conversation (self->database, account_id, conversation);
  g_signal_emit (self, signals[SIGNAL_DATA_CHANGED], 0, "conversations");
}

void
gnalk_data_repository_save_conversations (GnalkDataRepository *self,
                                          gint64               account_id,
                                          GPtrArray           *conversations)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));
  g_return_if_fail (self->is_open);
  g_return_if_fail (conversations != NULL);

  for (guint i = 0; i < conversations->len; i++)
    {
      GnalkConversationData *conv = g_ptr_array_index (conversations, i);
      if (GNALK_IS_CONVERSATION_DATA (conv))
        gnalk_database_save_conversation (self->database, account_id, conv);
    }

  if (conversations->len > 0)
    g_signal_emit (self, signals[SIGNAL_DATA_CHANGED], 0, "conversations");
}

void
gnalk_data_repository_delete_conversation (GnalkDataRepository *self,
                                           gint64               account_id,
                                           const char          *token)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));
  g_return_if_fail (self->is_open);
  g_return_if_fail (token != NULL);

  gnalk_database_delete_conversation (self->database, account_id, token);
  g_signal_emit (self, signals[SIGNAL_DATA_CHANGED], 0, "conversations");
}

int
gnalk_data_repository_delete_stale_conversations (GnalkDataRepository *self,
                                                  gint64               account_id,
                                                  GPtrArray           *valid_tokens)
{
  g_return_val_if_fail (GNALK_IS_DATA_REPOSITORY (self), 0);
  g_return_val_if_fail (self->is_open, 0);

  int deleted = gnalk_database_delete_stale_conversations (self->database, account_id, valid_tokens);
  if (deleted > 0)
    g_signal_emit (self, signals[SIGNAL_DATA_CHANGED], 0, "conversations");
  return deleted;
}

void
gnalk_data_repository_save_message (GnalkDataRepository *self,
                                    gint64               account_id,
                                    const char          *conversation_token,
                                    GnalkMessageData    *message)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));
  g_return_if_fail (self->is_open);
  g_return_if_fail (conversation_token != NULL);
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (message));

  gnalk_database_save_message (self->database, account_id, conversation_token, message);

  g_autofree char *scope = g_strdup_printf ("messages:%s", conversation_token);
  g_signal_emit (self, signals[SIGNAL_DATA_CHANGED], 0, scope);
}

gint64
gnalk_data_repository_get_last_known_message_id (GnalkDataRepository *self,
                                                 gint64               account_id,
                                                 const char          *conversation_token)
{
  g_return_val_if_fail (GNALK_IS_DATA_REPOSITORY (self), 0);
  g_return_val_if_fail (self->is_open, 0);
  g_return_val_if_fail (conversation_token != NULL, 0);

  return gnalk_database_get_last_known_message_id (self->database, account_id, conversation_token);
}

void
gnalk_data_repository_set_last_known_message_id (GnalkDataRepository *self,
                                                 gint64               account_id,
                                                 const char          *conversation_token,
                                                 gint64               message_id)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));
  g_return_if_fail (self->is_open);
  g_return_if_fail (conversation_token != NULL);

  gnalk_database_set_last_known_message_id (self->database, account_id, conversation_token, message_id);
}

void
gnalk_data_repository_set_has_more_history (GnalkDataRepository *self,
                                             gint64               account_id,
                                             const char          *conversation_token,
                                             gboolean             has_more)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));
  g_return_if_fail (self->is_open);
  g_return_if_fail (conversation_token != NULL);

  gnalk_database_set_has_more_history (self->database, account_id, conversation_token, has_more);
}

void
gnalk_data_repository_update_conversation_subtitle (GnalkDataRepository *self,
                                                     gint64               account_id,
                                                     const char          *conversation_token,
                                                     const char          *subtitle,
                                                     gint64               last_activity)
{
  g_return_if_fail (GNALK_IS_DATA_REPOSITORY (self));
  g_return_if_fail (self->is_open);
  g_return_if_fail (conversation_token != NULL);

  gnalk_database_update_conversation_subtitle (self->database, account_id, conversation_token,
                                                subtitle, last_activity);
  g_signal_emit (self, signals[SIGNAL_DATA_CHANGED], 0, "conversations");
}
