#include "gnalk-room-controller.h"
#include "gnalk-session.h"
#include "gnalk-http-client.h"
#include "gnalk-message-data.h"
#include "gnalk-conversation-data.h"

struct _GnalkRoomController
{
  GObject parent_instance;

  GnalkAppState       *app_state;
  GnalkDataRepository *data_repository;
  GnalkSessionManager *session_manager;

  /* Associated controllers (weak references) */
  GnalkConversationController *conversation_controller;
  GnalkParticipantController  *participant_controller;

  /* Current room state */
  char          *current_room_token;
  GnalkSession *current_session;  /* weak reference */
  gint64         current_account_id;
  int            current_room_type;

  /* Generation counter for cancelling stale async operations */
  guint64 generation;

  /* Loading state */
  gboolean is_loading;
  gboolean is_loading_history;
};

G_DEFINE_FINAL_TYPE (GnalkRoomController, gnalk_room_controller, G_TYPE_OBJECT)

enum {
  SIGNAL_ROOM_OPENED,
  SIGNAL_ROOM_CLOSED,
  SIGNAL_MESSAGES_LOADING,
  SIGNAL_MESSAGES_LOADED,
  SIGNAL_ERROR,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Context for async operations */
typedef struct {
  GnalkRoomController *controller;
  guint64               generation;
  char                 *room_token;
  gint64                account_id;
} AsyncContext;

static AsyncContext *
async_context_new (GnalkRoomController *controller,
                   const char           *room_token)
{
  AsyncContext *ctx = g_new0 (AsyncContext, 1);
  ctx->controller = g_object_ref (controller);
  ctx->generation = controller->generation;
  ctx->room_token = g_strdup (room_token);
  ctx->account_id = controller->current_account_id;
  return ctx;
}

static void
async_context_free (AsyncContext *ctx)
{
  g_clear_object (&ctx->controller);
  g_free (ctx->room_token);
  g_free (ctx);
}

static gboolean
async_context_is_stale (AsyncContext *ctx)
{
  return ctx->generation != ctx->controller->generation;
}

/* Forward declarations */
static void load_messages_from_cache (GnalkRoomController *self);
static void load_participants_from_cache (GnalkRoomController *self);

static void
gnalk_room_controller_finalize (GObject *object)
{
  GnalkRoomController *self = GNALK_ROOM_CONTROLLER (object);

  g_free (self->current_room_token);
  g_clear_object (&self->app_state);
  g_clear_object (&self->data_repository);
  g_clear_object (&self->session_manager);

  G_OBJECT_CLASS (gnalk_room_controller_parent_class)->finalize (object);
}

static void
gnalk_room_controller_class_init (GnalkRoomControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gnalk_room_controller_finalize;

  signals[SIGNAL_ROOM_OPENED] =
    g_signal_new ("room-opened", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_ROOM_CLOSED] =
    g_signal_new ("room-closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  signals[SIGNAL_MESSAGES_LOADING] =
    g_signal_new ("messages-loading", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  signals[SIGNAL_MESSAGES_LOADED] =
    g_signal_new ("messages-loaded", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  signals[SIGNAL_ERROR] =
    g_signal_new ("error", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gnalk_room_controller_init (GnalkRoomController *self)
{
  self->generation = 0;
  self->is_loading = FALSE;
  self->is_loading_history = FALSE;
}

GnalkRoomController *
gnalk_room_controller_new (GnalkAppState       *app_state,
                             GnalkDataRepository *data_repository,
                             GnalkSessionManager *session_manager)
{
  g_return_val_if_fail (GNALK_IS_APP_STATE (app_state), NULL);
  g_return_val_if_fail (GNALK_IS_DATA_REPOSITORY (data_repository), NULL);
  g_return_val_if_fail (GNALK_IS_SESSION_MANAGER (session_manager), NULL);

  GnalkRoomController *self = g_object_new (GNALK_TYPE_ROOM_CONTROLLER, NULL);
  self->app_state = g_object_ref (app_state);
  self->data_repository = g_object_ref (data_repository);
  self->session_manager = g_object_ref (session_manager);

  return self;
}

/* Async callback for loading messages from cache */
static void
on_messages_loaded_from_cache (GnalkDataRepository *repository G_GNUC_UNUSED,
                                gpointer              result,
                                GError               *error,
                                gpointer              user_data)
{
  AsyncContext *ctx = user_data;
  GnalkRoomController *self = ctx->controller;

  /* Check if room changed while loading */
  if (async_context_is_stale (ctx))
    {
      g_debug ("RoomController: Ignoring stale messages load for %s", ctx->room_token);
      if (result)
        g_object_unref (result);
      async_context_free (ctx);
      return;
    }

  if (error)
    {
      g_warning ("RoomController: Failed to load cached messages: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
    }
  else if (result)
    {
      GListStore *messages = G_LIST_STORE (result);
      guint n = g_list_model_get_n_items (G_LIST_MODEL (messages));
      g_debug ("RoomController: Loaded %u cached messages for %s", n, ctx->room_token);

      /* Update app state with cached messages */
      gnalk_app_state_set_messages (self->app_state, messages);
      g_object_unref (messages);
    }

  self->is_loading = FALSE;
  g_signal_emit (self, signals[SIGNAL_MESSAGES_LOADED], 0);

  async_context_free (ctx);
}

static void
load_messages_from_cache (GnalkRoomController *self)
{
  if (!self->current_room_token || !self->current_session)
    return;

  self->is_loading = TRUE;
  g_signal_emit (self, signals[SIGNAL_MESSAGES_LOADING], 0);

  AsyncContext *ctx = async_context_new (self, self->current_room_token);

  gnalk_data_repository_load_messages_async (
    self->data_repository,
    self->current_account_id,
    self->current_room_token,
    50,  /* Load last 50 messages initially */
    0,   /* From latest */
    on_messages_loaded_from_cache,
    ctx);
}

/* Async callback for loading participants from cache */
static void
on_participants_loaded_from_cache (GnalkDataRepository *repository G_GNUC_UNUSED,
                                    gpointer              result,
                                    GError               *error,
                                    gpointer              user_data)
{
  AsyncContext *ctx = user_data;
  GnalkRoomController *self G_GNUC_UNUSED = ctx->controller;

  /* Check if room changed while loading */
  if (async_context_is_stale (ctx))
    {
      g_debug ("RoomController: Ignoring stale participants load for %s", ctx->room_token);
      if (result)
        json_array_unref (result);
      async_context_free (ctx);
      return;
    }

  if (error)
    {
      g_debug ("RoomController: No cached participants: %s", error->message);
    }
  else if (result)
    {
      JsonArray *participants = result;
      guint n = json_array_get_length (participants);
      g_debug ("RoomController: Loaded %u cached participants for %s", n, ctx->room_token);

      /* TODO: Convert JsonArray to GListStore and update app state */
      /* For now, we'll let the session handle participants via its signals */

      json_array_unref (participants);
    }

  async_context_free (ctx);
}

static void
load_participants_from_cache (GnalkRoomController *self)
{
  if (!self->current_room_token || !self->current_session)
    return;

  AsyncContext *ctx = async_context_new (self, self->current_room_token);

  gnalk_data_repository_load_participants_async (
    self->data_repository,
    self->current_account_id,
    self->current_room_token,
    on_participants_loaded_from_cache,
    ctx);
}

/* Public API */

void
gnalk_room_controller_open_room (GnalkRoomController *self,
                                   const char           *token,
                                   GnalkSession        *session,
                                   int                   room_type)
{
  g_return_if_fail (GNALK_IS_ROOM_CONTROLLER (self));
  g_return_if_fail (token != NULL);
  g_return_if_fail (GNALK_IS_SESSION (session));

  /* Check if we're already in this room */
  if (g_strcmp0 (self->current_room_token, token) == 0 &&
      self->current_session == session)
    {
      g_debug ("RoomController: Already in room %s", token);
      return;
    }

  /* Close current room if any */
  if (self->current_room_token)
    gnalk_room_controller_close_room (self);

  g_debug ("RoomController: Opening room %s (type %d)", token, room_type);

  /* Increment generation to invalidate any in-flight async operations */
  self->generation++;

  /* Update state */
  self->current_room_token = g_strdup (token);
  self->current_session = session;
  self->current_account_id = gnalk_session_get_account_id (session);
  self->current_room_type = room_type;

  /* Update app state */
  gnalk_app_state_set_active_room (self->app_state, token, session);

  /* Note: SyncCoordinator handles sync coordination via Application set_active_room */

  /* Notify participant controller to start loading and polling for this room */
  if (self->participant_controller)
    gnalk_participant_controller_set_active_room (self->participant_controller, token, room_type);

  /* Load cached data first (fast, appears immediately) */
  load_messages_from_cache (self);
  load_participants_from_cache (self);

  /* Join the room via session (HTTP call for signaling, mark read, etc.) */
  gnalk_session_join_room (session, token);

  g_signal_emit (self, signals[SIGNAL_ROOM_OPENED], 0, token);
}

void
gnalk_room_controller_close_room (GnalkRoomController *self)
{
  g_return_if_fail (GNALK_IS_ROOM_CONTROLLER (self));

  if (!self->current_room_token)
    return;

  g_debug ("RoomController: Closing room %s", self->current_room_token);

  /* Increment generation to invalidate any in-flight async operations */
  self->generation++;

  /* Notify participant controller to stop loading/polling */
  if (self->participant_controller)
    gnalk_participant_controller_set_active_room (self->participant_controller, NULL, 0);

  /* Note: SyncCoordinator handles sync coordination via Application set_active_room */

  /* Leave the room via session */
  if (self->current_session)
    gnalk_session_leave_room (self->current_session);

  /* Clear state */
  g_free (self->current_room_token);
  self->current_room_token = NULL;
  self->current_session = NULL;
  self->current_account_id = 0;
  self->current_room_type = 0;

  /* Update app state */
  gnalk_app_state_set_active_room (self->app_state, NULL, NULL);

  g_signal_emit (self, signals[SIGNAL_ROOM_CLOSED], 0);
}

const char *
gnalk_room_controller_get_current_room (GnalkRoomController *self)
{
  g_return_val_if_fail (GNALK_IS_ROOM_CONTROLLER (self), NULL);
  return self->current_room_token;
}

GnalkSession *
gnalk_room_controller_get_current_session (GnalkRoomController *self)
{
  g_return_val_if_fail (GNALK_IS_ROOM_CONTROLLER (self), NULL);
  return self->current_session;
}

/* Async callback for loading older messages */
static void
on_older_messages_loaded (GnalkDataRepository *repository G_GNUC_UNUSED,
                           gpointer              result,
                           GError               *error,
                           gpointer              user_data)
{
  AsyncContext *ctx = user_data;
  GnalkRoomController *self = ctx->controller;

  /* Check if room changed while loading */
  if (async_context_is_stale (ctx))
    {
      g_debug ("RoomController: Ignoring stale history load for %s", ctx->room_token);
      if (result)
        g_object_unref (result);
      async_context_free (ctx);
      return;
    }

  self->is_loading_history = FALSE;

  if (error)
    {
      g_warning ("RoomController: Failed to load older messages: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
    }
  else if (result)
    {
      GListStore *messages = G_LIST_STORE (result);
      guint n = g_list_model_get_n_items (G_LIST_MODEL (messages));
      g_debug ("RoomController: Loaded %u older messages for %s", n, ctx->room_token);

      /* Prepend to app state */
      gnalk_app_state_prepend_messages (self->app_state, messages);
      g_object_unref (messages);
    }

  g_signal_emit (self, signals[SIGNAL_MESSAGES_LOADED], 0);
  async_context_free (ctx);
}

void
gnalk_room_controller_load_older_messages (GnalkRoomController *self)
{
  g_return_if_fail (GNALK_IS_ROOM_CONTROLLER (self));

  if (!self->current_room_token || !self->current_session)
    {
      g_debug ("RoomController: No room open, can't load history");
      return;
    }

  if (self->is_loading_history)
    {
      g_debug ("RoomController: Already loading history");
      return;
    }

  /* Get the oldest message ID currently loaded */
  GListStore *messages = gnalk_app_state_get_messages (self->app_state);
  guint n = g_list_model_get_n_items (G_LIST_MODEL (messages));

  if (n == 0)
    {
      g_debug ("RoomController: No messages loaded, can't determine before_id");
      return;
    }

  GnalkMessageData *oldest = g_list_model_get_item (G_LIST_MODEL (messages), 0);
  gint64 before_id = gnalk_message_data_get_id (oldest);
  g_object_unref (oldest);

  g_debug ("RoomController: Loading messages before ID %ld", (long) before_id);

  self->is_loading_history = TRUE;

  AsyncContext *ctx = async_context_new (self, self->current_room_token);

  gnalk_data_repository_load_messages_async (
    self->data_repository,
    self->current_account_id,
    self->current_room_token,
    50,  /* Load 50 more messages */
    before_id,
    on_older_messages_loaded,
    ctx);
}

void
gnalk_room_controller_send_message (GnalkRoomController *self,
                                       const char           *text,
                                       gint64                reply_to)
{
  g_return_if_fail (GNALK_IS_ROOM_CONTROLLER (self));
  g_return_if_fail (text != NULL);

  if (!self->current_room_token || !self->current_session)
    {
      g_warning ("RoomController: No room open, can't send message");
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, "No room open");
      return;
    }

  g_debug ("RoomController: Sending message to %s", self->current_room_token);

  /* Generate a unique reference ID for tracking */
  g_autofree char *reference_id = g_uuid_string_random ();

  /* Delegate to session */
  gnalk_session_send_message (self->current_session,
                                 self->current_room_token,
                                 text,
                                 reply_to,
                                 reference_id);
}

/* Callback for mark read response (currently does nothing) */
static void
on_mark_read_response (GnalkHttpClient *client G_GNUC_UNUSED,
                        JsonNode         *response G_GNUC_UNUSED,
                        GError           *error,
                        gpointer          user_data G_GNUC_UNUSED)
{
  if (error)
    g_debug ("RoomController: Mark read failed: %s", error->message);
}

void
gnalk_room_controller_mark_read (GnalkRoomController *self)
{
  g_return_if_fail (GNALK_IS_ROOM_CONTROLLER (self));

  if (!self->current_room_token || !self->current_session)
    return;

  /* Get the last message ID */
  GListStore *messages = gnalk_app_state_get_messages (self->app_state);
  guint n = g_list_model_get_n_items (G_LIST_MODEL (messages));

  if (n == 0)
    return;

  GnalkMessageData *latest = g_list_model_get_item (G_LIST_MODEL (messages), n - 1);
  gint64 last_msg_id = gnalk_message_data_get_id (latest);
  g_object_unref (latest);

  /* Use HTTP client from session */
  GnalkHttpClient *http_client = gnalk_session_get_http_client (self->current_session);
  if (http_client)
    {
      gnalk_http_client_mark_chat_read (http_client,
                                           self->current_room_token,
                                           last_msg_id,
                                           on_mark_read_response,
                                           NULL);
    }
}

void
gnalk_room_controller_refresh (GnalkRoomController *self)
{
  g_return_if_fail (GNALK_IS_ROOM_CONTROLLER (self));

  if (!self->current_room_token || !self->current_session)
    return;

  g_debug ("RoomController: Refreshing room %s", self->current_room_token);

  /* Re-join room to refresh messages */
  gnalk_session_join_room (self->current_session, self->current_room_token);
}

gboolean
gnalk_room_controller_is_loading (GnalkRoomController *self)
{
  g_return_val_if_fail (GNALK_IS_ROOM_CONTROLLER (self), FALSE);
  return self->is_loading || self->is_loading_history;
}

void
gnalk_room_controller_set_conversation_controller (GnalkRoomController         *self,
                                                      GnalkConversationController *conversation_controller)
{
  g_return_if_fail (GNALK_IS_ROOM_CONTROLLER (self));

  self->conversation_controller = conversation_controller;
}

void
gnalk_room_controller_set_participant_controller (GnalkRoomController        *self,
                                                     GnalkParticipantController *participant_controller)
{
  g_return_if_fail (GNALK_IS_ROOM_CONTROLLER (self));

  self->participant_controller = participant_controller;
}
