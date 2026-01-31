#include "gnalk-app-state.h"
#include "gnalk-session.h"

struct _GnalkAppState
{
  GObject parent_instance;

  /* Merged conversations from all sessions */
  GListStore *conversations;

  /* Messages and participants for active room only */
  GListStore *messages;
  GListStore *participants;

  /* Active room tracking */
  char          *active_room_token;
  GnalkSession *active_session;  /* weak reference */
  guint64        room_generation;  /* incremented on room switch */

  /* Status */
  gboolean         is_online;
  gboolean         is_loading;
  GnalkSyncStatus sync_status;
  char            *sync_message;
};

G_DEFINE_FINAL_TYPE (GnalkAppState, gnalk_app_state, G_TYPE_OBJECT)

enum {
  SIGNAL_CONVERSATIONS_CHANGED,
  SIGNAL_MESSAGES_CHANGED,
  SIGNAL_PARTICIPANTS_CHANGED,
  SIGNAL_ACTIVE_ROOM_CHANGED,
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

  g_clear_object (&self->conversations);
  g_clear_object (&self->messages);
  g_clear_object (&self->participants);
  g_free (self->active_room_token);
  g_free (self->sync_message);

  G_OBJECT_CLASS (gnalk_app_state_parent_class)->finalize (object);
}

static void
gnalk_app_state_class_init (GnalkAppStateClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gnalk_app_state_finalize;

  signals[SIGNAL_CONVERSATIONS_CHANGED] =
    g_signal_new ("conversations-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  signals[SIGNAL_MESSAGES_CHANGED] =
    g_signal_new ("messages-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  signals[SIGNAL_PARTICIPANTS_CHANGED] =
    g_signal_new ("participants-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  signals[SIGNAL_ACTIVE_ROOM_CHANGED] =
    g_signal_new ("active-room-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

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
  self->conversations = g_list_store_new (GNALK_TYPE_CONVERSATION_DATA);
  self->messages = g_list_store_new (GNALK_TYPE_MESSAGE_DATA);
  self->participants = g_list_store_new (G_TYPE_OBJECT);  /* Generic for now */
  self->room_generation = 0;
  self->is_online = FALSE;
  self->is_loading = FALSE;
  self->sync_status = GNALK_SYNC_STATUS_IDLE;
}

GnalkAppState *
gnalk_app_state_new (void)
{
  return g_object_new (GNALK_TYPE_APP_STATE, NULL);
}

/* Conversations */

GListStore *
gnalk_app_state_get_conversations (GnalkAppState *self)
{
  g_return_val_if_fail (GNALK_IS_APP_STATE (self), NULL);
  return self->conversations;
}

/* Comparison function for sorting conversations by last activity (descending) */
static int
compare_conversations_by_activity (gconstpointer a, gconstpointer b, gpointer user_data G_GNUC_UNUSED)
{
  GnalkConversationData *conv_a = GNALK_CONVERSATION_DATA ((gpointer) a);
  GnalkConversationData *conv_b = GNALK_CONVERSATION_DATA ((gpointer) b);

  /* Favorites first */
  gboolean fav_a = gnalk_conversation_data_get_is_favorite (conv_a);
  gboolean fav_b = gnalk_conversation_data_get_is_favorite (conv_b);

  if (fav_a && !fav_b)
    return -1;
  if (!fav_a && fav_b)
    return 1;

  /* Then by last activity (descending) */
  gint64 time_a = gnalk_conversation_data_get_last_activity (conv_a);
  gint64 time_b = gnalk_conversation_data_get_last_activity (conv_b);

  if (time_a > time_b)
    return -1;
  if (time_a < time_b)
    return 1;

  return 0;
}

void
gnalk_app_state_set_conversations (GnalkAppState *self,
                                     GListStore     *conversations,
                                     GnalkSession  *session)
{
  g_return_if_fail (GNALK_IS_APP_STATE (self));

  /* For now, we do a simple approach:
   * - Clear conversations from this session
   * - Add all new conversations
   * - Re-sort
   *
   * In future we could do smarter diffing.
   */

  /* Remove conversations from this session */
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->conversations));
  for (guint i = n; i > 0; i--)
    {
      GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (self->conversations), i - 1);
      if (gnalk_conversation_data_get_session (conv) == session)
        g_list_store_remove (self->conversations, i - 1);
      g_object_unref (conv);
    }

  /* Add new conversations */
  if (conversations)
    {
      guint count = g_list_model_get_n_items (G_LIST_MODEL (conversations));
      for (guint i = 0; i < count; i++)
        {
          GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (conversations), i);
          /* Ensure session is set */
          gnalk_conversation_data_set_session (conv, session);
          g_list_store_append (self->conversations, conv);
          g_object_unref (conv);
        }
    }

  /* Sort by activity */
  g_list_store_sort (self->conversations, compare_conversations_by_activity, NULL);

  g_signal_emit (self, signals[SIGNAL_CONVERSATIONS_CHANGED], 0);
}

void
gnalk_app_state_clear_conversations (GnalkAppState *self,
                                       GnalkSession  *session)
{
  g_return_if_fail (GNALK_IS_APP_STATE (self));

  if (session == NULL)
    {
      g_list_store_remove_all (self->conversations);
    }
  else
    {
      guint n = g_list_model_get_n_items (G_LIST_MODEL (self->conversations));
      for (guint i = n; i > 0; i--)
        {
          GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (self->conversations), i - 1);
          if (gnalk_conversation_data_get_session (conv) == session)
            g_list_store_remove (self->conversations, i - 1);
          g_object_unref (conv);
        }
    }

  g_signal_emit (self, signals[SIGNAL_CONVERSATIONS_CHANGED], 0);
}

void
gnalk_app_state_update_conversation (GnalkAppState         *self,
                                       GnalkConversationData *conversation)
{
  g_return_if_fail (GNALK_IS_APP_STATE (self));
  g_return_if_fail (GNALK_IS_CONVERSATION_DATA (conversation));

  /* Find and update the conversation */
  const char *token = gnalk_conversation_data_get_token (conversation);
  GnalkSession *session = gnalk_conversation_data_get_session (conversation);

  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->conversations));
  for (guint i = 0; i < n; i++)
    {
      GnalkConversationData *conv = g_list_model_get_item (G_LIST_MODEL (self->conversations), i);
      if (gnalk_conversation_data_get_session (conv) == session &&
          g_strcmp0 (gnalk_conversation_data_get_token (conv), token) == 0)
        {
          /* Replace with updated conversation */
          g_list_store_splice (self->conversations, i, 1, (gpointer *) &conversation, 1);
          g_object_unref (conv);

          /* Re-sort since activity may have changed */
          g_list_store_sort (self->conversations, compare_conversations_by_activity, NULL);

          g_signal_emit (self, signals[SIGNAL_CONVERSATIONS_CHANGED], 0);
          return;
        }
      g_object_unref (conv);
    }

  /* Not found - add it */
  g_list_store_append (self->conversations, conversation);
  g_list_store_sort (self->conversations, compare_conversations_by_activity, NULL);
  g_signal_emit (self, signals[SIGNAL_CONVERSATIONS_CHANGED], 0);
}

/* Messages */

GListStore *
gnalk_app_state_get_messages (GnalkAppState *self)
{
  g_return_val_if_fail (GNALK_IS_APP_STATE (self), NULL);
  return self->messages;
}

void
gnalk_app_state_set_messages (GnalkAppState *self,
                                GListStore     *messages)
{
  g_return_if_fail (GNALK_IS_APP_STATE (self));

  g_list_store_remove_all (self->messages);

  if (messages)
    {
      guint n = g_list_model_get_n_items (G_LIST_MODEL (messages));
      for (guint i = 0; i < n; i++)
        {
          GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (messages), i);
          g_list_store_append (self->messages, msg);
          g_object_unref (msg);
        }
    }

  g_signal_emit (self, signals[SIGNAL_MESSAGES_CHANGED], 0);
}

void
gnalk_app_state_append_message (GnalkAppState    *self,
                                  GnalkMessageData *message)
{
  g_return_if_fail (GNALK_IS_APP_STATE (self));
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (message));

  g_list_store_append (self->messages, message);
  g_signal_emit (self, signals[SIGNAL_MESSAGES_CHANGED], 0);
}

void
gnalk_app_state_prepend_messages (GnalkAppState *self,
                                    GListStore     *messages)
{
  g_return_if_fail (GNALK_IS_APP_STATE (self));

  if (!messages)
    return;

  guint n = g_list_model_get_n_items (G_LIST_MODEL (messages));
  for (guint i = 0; i < n; i++)
    {
      GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (messages), i);
      g_list_store_insert (self->messages, i, msg);
      g_object_unref (msg);
    }

  if (n > 0)
    g_signal_emit (self, signals[SIGNAL_MESSAGES_CHANGED], 0);
}

void
gnalk_app_state_update_message (GnalkAppState    *self,
                                  GnalkMessageData *message)
{
  g_return_if_fail (GNALK_IS_APP_STATE (self));
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (message));

  gint64 msg_id = gnalk_message_data_get_id (message);

  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->messages));
  for (guint i = 0; i < n; i++)
    {
      GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (self->messages), i);
      if (gnalk_message_data_get_id (msg) == msg_id)
        {
          g_list_store_splice (self->messages, i, 1, (gpointer *) &message, 1);
          g_object_unref (msg);
          g_signal_emit (self, signals[SIGNAL_MESSAGES_CHANGED], 0);
          return;
        }
      g_object_unref (msg);
    }
}

void
gnalk_app_state_clear_messages (GnalkAppState *self)
{
  g_return_if_fail (GNALK_IS_APP_STATE (self));

  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->messages));
  if (n > 0)
    {
      g_list_store_remove_all (self->messages);
      g_signal_emit (self, signals[SIGNAL_MESSAGES_CHANGED], 0);
    }
}

/* Participants */

GListStore *
gnalk_app_state_get_participants (GnalkAppState *self)
{
  g_return_val_if_fail (GNALK_IS_APP_STATE (self), NULL);
  return self->participants;
}

void
gnalk_app_state_set_participants (GnalkAppState *self,
                                    GListStore     *participants)
{
  g_return_if_fail (GNALK_IS_APP_STATE (self));

  g_list_store_remove_all (self->participants);

  if (participants)
    {
      guint n = g_list_model_get_n_items (G_LIST_MODEL (participants));
      for (guint i = 0; i < n; i++)
        {
          GObject *p = g_list_model_get_item (G_LIST_MODEL (participants), i);
          g_list_store_append (self->participants, p);
          g_object_unref (p);
        }
    }

  g_signal_emit (self, signals[SIGNAL_PARTICIPANTS_CHANGED], 0);
}

void
gnalk_app_state_clear_participants (GnalkAppState *self)
{
  g_return_if_fail (GNALK_IS_APP_STATE (self));

  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->participants));
  if (n > 0)
    {
      g_list_store_remove_all (self->participants);
      g_signal_emit (self, signals[SIGNAL_PARTICIPANTS_CHANGED], 0);
    }
}

/* Active room */

const char *
gnalk_app_state_get_active_room_token (GnalkAppState *self)
{
  g_return_val_if_fail (GNALK_IS_APP_STATE (self), NULL);
  return self->active_room_token;
}

GnalkSession *
gnalk_app_state_get_active_session (GnalkAppState *self)
{
  g_return_val_if_fail (GNALK_IS_APP_STATE (self), NULL);
  return self->active_session;
}

void
gnalk_app_state_set_active_room (GnalkAppState *self,
                                   const char     *token,
                                   GnalkSession  *session)
{
  g_return_if_fail (GNALK_IS_APP_STATE (self));

  /* Check if actually changed */
  if (g_strcmp0 (self->active_room_token, token) == 0 &&
      self->active_session == session)
    return;

  /* Update state */
  g_free (self->active_room_token);
  self->active_room_token = g_strdup (token);
  self->active_session = session;

  /* Increment generation to invalidate in-flight async operations */
  self->room_generation++;

  /* Clear messages and participants for new room */
  gnalk_app_state_clear_messages (self);
  gnalk_app_state_clear_participants (self);

  g_signal_emit (self, signals[SIGNAL_ACTIVE_ROOM_CHANGED], 0);
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

guint64
gnalk_app_state_get_room_generation (GnalkAppState *self)
{
  g_return_val_if_fail (GNALK_IS_APP_STATE (self), 0);
  return self->room_generation;
}
