#include "gnalk-conversation-controller.h"
#include "gnalk-conversation-data.h"
#include <json-glib/json-glib.h>

struct _GnalkConversationController
{
  GObject parent_instance;

  GnalkDataRepository *repository;
  GnalkHttpClient     *http_client;

  gint64 account_id;
};

G_DEFINE_FINAL_TYPE (GnalkConversationController, gnalk_conversation_controller, G_TYPE_OBJECT)

enum {
  SIGNAL_CONVERSATION_CREATED,
  SIGNAL_CONVERSATION_LEFT,
  SIGNAL_CONVERSATION_DELETED,
  SIGNAL_ERROR,
  SIGNAL_INVITATIONS_UPDATED,
  SIGNAL_INVITATION_ACCEPTED,
  SIGNAL_INVITATION_DECLINED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Callback data structure */
typedef struct {
  GWeakRef                     controller_ref;  /* Weak ref to owner (use-after-free safe) */
  char                        *room_token;
  gboolean                     favorite;  /* Used by set_favorite */
} OperationCallbackData;

static void
operation_callback_data_free (OperationCallbackData *data)
{
  g_weak_ref_clear (&data->controller_ref);
  g_free (data->room_token);
  g_slice_free (OperationCallbackData, data);
}

static void
gnalk_conversation_controller_finalize (GObject *object)
{
  GnalkConversationController *self = GNALK_CONVERSATION_CONTROLLER (object);

  g_clear_object (&self->repository);
  g_clear_object (&self->http_client);

  G_OBJECT_CLASS (gnalk_conversation_controller_parent_class)->finalize (object);
}

static void
gnalk_conversation_controller_class_init (GnalkConversationControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_conversation_controller_finalize;

  signals[SIGNAL_CONVERSATION_CREATED] =
    g_signal_new ("conversation-created",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);  /* room_token */

  signals[SIGNAL_CONVERSATION_LEFT] =
    g_signal_new ("conversation-left",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);  /* room_token */

  signals[SIGNAL_CONVERSATION_DELETED] =
    g_signal_new ("conversation-deleted",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);  /* room_token */

  signals[SIGNAL_ERROR] =
    g_signal_new ("error",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);  /* error message */

  signals[SIGNAL_INVITATIONS_UPDATED] =
    g_signal_new ("invitations-updated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  JSON_TYPE_ARRAY);  /* invitations */

  signals[SIGNAL_INVITATION_ACCEPTED] =
    g_signal_new ("invitation-accepted",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT64);  /* invitation_id */

  signals[SIGNAL_INVITATION_DECLINED] =
    g_signal_new ("invitation-declined",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT64);  /* invitation_id */
}

static void
gnalk_conversation_controller_init (GnalkConversationController *self G_GNUC_UNUSED)
{
}

GnalkConversationController *
gnalk_conversation_controller_new (GnalkDataRepository *repository,
                                   GnalkHttpClient     *http_client)
{
  GnalkConversationController *self = g_object_new (GNALK_TYPE_CONVERSATION_CONTROLLER, NULL);

  self->repository = repository ? g_object_ref (repository) : NULL;
  self->http_client = http_client ? g_object_ref (http_client) : NULL;

  return self;
}

void
gnalk_conversation_controller_set_account_id (GnalkConversationController *self,
                                              gint64                       account_id)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));
  self->account_id = account_id;
}

/* ============ Conversation Operations ============ */

static void
on_create_response (GnalkHttpClient *client G_GNUC_UNUSED,
                    JsonNode         *response,
                    GError           *error,
                    gpointer          user_data)
{
  OperationCallbackData *cb_data = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      operation_callback_data_free (cb_data);
      return;
    }

  /* Owner may have been destroyed while the request was in flight; g_autoptr
   * balances g_weak_ref_get on every return path. */
  g_autoptr(GnalkConversationController) self = g_weak_ref_get (&cb_data->controller_ref);
  operation_callback_data_free (cb_data);

  if (!self)
    return;

  if (error)
    {
      g_warning ("ConversationController: Create failed: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
      return;
    }

  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    return;

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    return;

  JsonObject *data = json_object_get_object_member (ocs, "data");
  if (!data)
    return;

  const char *token = json_object_get_string_member_with_default (data, "token", NULL);
  if (!token)
    return;

  /* Save the new conversation to database */
  if (self->repository && self->account_id > 0)
    {
      GnalkConversationData *conv_data = gnalk_conversation_data_new_from_json (data, NULL);
      if (conv_data)
        {
          gnalk_data_repository_save_conversation (self->repository, self->account_id, conv_data);
          g_object_unref (conv_data);
        }
    }

  g_debug ("ConversationController: Created conversation %s", token);
  g_signal_emit (self, signals[SIGNAL_CONVERSATION_CREATED], 0, token);
}

void
gnalk_conversation_controller_create (GnalkConversationController *self,
                                         int                           room_type,
                                         const char                   *invite,
                                         const char                   *room_name)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);

  OperationCallbackData *cb_data = g_slice_new0 (OperationCallbackData);
  g_weak_ref_init (&cb_data->controller_ref, self);
  cb_data->room_token = NULL;

  gnalk_http_client_create_conversation (self->http_client,
                                            room_type,
                                            invite,
                                            room_name,
                                            on_create_response,
                                            cb_data);
}

static void
on_leave_response (GnalkHttpClient *client G_GNUC_UNUSED,
                   JsonNode         *response G_GNUC_UNUSED,
                   GError           *error,
                   gpointer          user_data)
{
  OperationCallbackData *cb_data = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      operation_callback_data_free (cb_data);
      return;
    }

  /* Owner may have been destroyed while the request was in flight; g_autoptr
   * balances g_weak_ref_get on every return path. */
  g_autoptr(GnalkConversationController) self = g_weak_ref_get (&cb_data->controller_ref);
  char *room_token = g_strdup (cb_data->room_token);
  operation_callback_data_free (cb_data);

  if (!self)
    {
      g_free (room_token);
      return;
    }

  if (error)
    {
      g_warning ("ConversationController: Leave failed: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
      g_free (room_token);
      return;
    }

  /* Delete from database */
  if (self->repository && self->account_id > 0 && room_token)
    gnalk_data_repository_delete_conversation (self->repository, self->account_id, room_token);

  g_debug ("ConversationController: Left conversation %s", room_token);
  g_signal_emit (self, signals[SIGNAL_CONVERSATION_LEFT], 0, room_token);

  g_free (room_token);
}

void
gnalk_conversation_controller_leave (GnalkConversationController *self,
                                        const char                   *room_token)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (room_token != NULL);

  OperationCallbackData *cb_data = g_slice_new0 (OperationCallbackData);
  g_weak_ref_init (&cb_data->controller_ref, self);
  cb_data->room_token = g_strdup (room_token);

  gnalk_http_client_leave_conversation (self->http_client,
                                           room_token,
                                           on_leave_response,
                                           cb_data);
}

static void
on_delete_response (GnalkHttpClient *client G_GNUC_UNUSED,
                    JsonNode         *response G_GNUC_UNUSED,
                    GError           *error,
                    gpointer          user_data)
{
  OperationCallbackData *cb_data = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      operation_callback_data_free (cb_data);
      return;
    }

  /* Owner may have been destroyed while the request was in flight; g_autoptr
   * balances g_weak_ref_get on every return path. */
  g_autoptr(GnalkConversationController) self = g_weak_ref_get (&cb_data->controller_ref);
  char *room_token = g_strdup (cb_data->room_token);
  operation_callback_data_free (cb_data);

  if (!self)
    {
      g_free (room_token);
      return;
    }

  if (error)
    {
      g_warning ("ConversationController: Delete failed: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
      g_free (room_token);
      return;
    }

  /* Delete from database */
  if (self->repository && self->account_id > 0 && room_token)
    gnalk_data_repository_delete_conversation (self->repository, self->account_id, room_token);

  g_debug ("ConversationController: Deleted conversation %s", room_token);
  g_signal_emit (self, signals[SIGNAL_CONVERSATION_DELETED], 0, room_token);

  g_free (room_token);
}

void
gnalk_conversation_controller_delete (GnalkConversationController *self,
                                      const char                  *room_token)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (room_token != NULL);

  OperationCallbackData *cb_data = g_slice_new0 (OperationCallbackData);
  g_weak_ref_init (&cb_data->controller_ref, self);
  cb_data->room_token = g_strdup (room_token);

  gnalk_http_client_delete_conversation (self->http_client,
                                            room_token,
                                            on_delete_response,
                                            cb_data);
}

static void
on_favorite_response (GnalkHttpClient *client G_GNUC_UNUSED,
                      JsonNode         *response G_GNUC_UNUSED,
                      GError           *error,
                      gpointer          user_data)
{
  OperationCallbackData *cb_data = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      operation_callback_data_free (cb_data);
      return;
    }

  /* Owner may have been destroyed while the request was in flight; g_autoptr
   * balances g_weak_ref_get on every return path. */
  g_autoptr(GnalkConversationController) self = g_weak_ref_get (&cb_data->controller_ref);
  char *room_token = g_strdup (cb_data->room_token);
  gboolean favorite = cb_data->favorite;
  operation_callback_data_free (cb_data);

  if (!self)
    {
      g_free (room_token);
      return;
    }

  if (error)
    {
      g_warning ("ConversationController: Set favorite failed: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
      g_free (room_token);
      return;
    }

  /* Update DB immediately so data-changed fires and UI updates */
  if (self->repository && self->account_id > 0 && room_token)
    gnalk_data_repository_update_conversation_favorite (self->repository, self->account_id,
                                                        room_token, favorite);

  g_free (room_token);
}

void
gnalk_conversation_controller_set_favorite (GnalkConversationController *self,
                                            const char                  *room_token,
                                            gboolean                     favorite)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (room_token != NULL);

  OperationCallbackData *cb_data = g_slice_new0 (OperationCallbackData);
  g_weak_ref_init (&cb_data->controller_ref, self);
  cb_data->room_token = g_strdup (room_token);
  cb_data->favorite = favorite;

  gnalk_http_client_set_conversation_favorite (self->http_client,
                                                  room_token,
                                                  favorite,
                                                  on_favorite_response,
                                                  cb_data);
}

/* ============ Search Contacts ============ */

typedef struct {
  GWeakRef                      controller_ref;  /* Weak ref to owner (use-after-free safe) */
  GnalkSearchContactsCallback   callback;
  gpointer                       user_data;
} SearchContactsCallbackData;

static void
on_search_contacts_response (GnalkHttpClient *client G_GNUC_UNUSED,
                             JsonNode         *response,
                             GError           *error,
                             gpointer          user_data)
{
  SearchContactsCallbackData *cb_data = user_data;

  /* Owner may have been destroyed while the request was in flight; only invoke
   * the user callback while the controller is alive to avoid passing a
   * dangling pointer. g_autoptr balances g_weak_ref_get. */
  g_autoptr(GnalkConversationController) self = g_weak_ref_get (&cb_data->controller_ref);
  if (self && cb_data->callback)
    cb_data->callback (self, response, error, cb_data->user_data);

  g_weak_ref_clear (&cb_data->controller_ref);
  g_slice_free (SearchContactsCallbackData, cb_data);
}

void
gnalk_conversation_controller_search_contacts (GnalkConversationController  *self,
                                               const char                   *search_term,
                                               gboolean                      include_federated,
                                               GnalkSearchContactsCallback   callback,
                                               gpointer                      user_data)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (search_term != NULL);

  SearchContactsCallbackData *cb_data = g_slice_new0 (SearchContactsCallbackData);
  g_weak_ref_init (&cb_data->controller_ref, self);
  cb_data->callback = callback;
  cb_data->user_data = user_data;

  gnalk_http_client_search_contacts (self->http_client,
                                     search_term,
                                     include_federated,
                                     on_search_contacts_response,
                                     cb_data);
}

/* ============ Federated Conversation Creation ============ */

static void
on_create_federated_response (GnalkHttpClient *client G_GNUC_UNUSED,
                               JsonNode         *response,
                               GError           *error,
                               gpointer          user_data)
{
  OperationCallbackData *cb_data = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      operation_callback_data_free (cb_data);
      return;
    }

  /* Owner may have been destroyed while the request was in flight; g_autoptr
   * balances g_weak_ref_get on every return path. */
  g_autoptr(GnalkConversationController) self = g_weak_ref_get (&cb_data->controller_ref);
  operation_callback_data_free (cb_data);

  if (!self)
    return;

  if (error)
    {
      g_warning ("ConversationController: Create federated failed: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
      return;
    }

  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    return;

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    return;

  JsonObject *data = json_object_get_object_member (ocs, "data");
  if (!data)
    return;

  const char *token = json_object_get_string_member_with_default (data, "token", NULL);
  if (!token)
    return;

  /* Save the new conversation to database */
  if (self->repository && self->account_id > 0)
    {
      GnalkConversationData *conv_data = gnalk_conversation_data_new_from_json (data, NULL);
      if (conv_data)
        {
          gnalk_data_repository_save_conversation (self->repository, self->account_id, conv_data);
          g_object_unref (conv_data);
        }
    }

  g_debug ("ConversationController: Created federated conversation %s", token);
  g_signal_emit (self, signals[SIGNAL_CONVERSATION_CREATED], 0, token);
}

void
gnalk_conversation_controller_create_federated (GnalkConversationController *self,
                                                 const char                  *cloud_id)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (cloud_id != NULL);

  /* Check if a group conversation for this federated user already exists */
  if (self->repository && self->account_id > 0)
    {
      GnalkDatabase *db = gnalk_data_repository_get_database (self->repository);
      if (db)
        {
          g_autofree char *existing_token =
            gnalk_database_find_conversation_token (db, self->account_id, cloud_id, 2);
          if (existing_token)
            {
              g_debug ("ConversationController: Found existing federated conversation %s for %s",
                       existing_token, cloud_id);
              g_signal_emit (self, signals[SIGNAL_CONVERSATION_CREATED], 0, existing_token);
              return;
            }
        }
    }

  OperationCallbackData *cb_data = g_slice_new0 (OperationCallbackData);
  g_weak_ref_init (&cb_data->controller_ref, self);
  cb_data->room_token = NULL;

  /* Use cloud_id as roomName so we can find the conversation later
   * by matching name == cloud_id (acts as unique identifier) */
  gnalk_http_client_create_conversation_with_participants (self->http_client,
                                                            2,  /* roomType = group */
                                                            cloud_id,
                                                            "federated_users",
                                                            cloud_id,
                                                            on_create_federated_response,
                                                            cb_data);
}

/* ============ Federation Invitations ============ */

typedef struct {
  GWeakRef                     controller_ref;  /* Weak ref to owner (use-after-free safe) */
  gint64                       invitation_id;
} InvitationCallbackData;

static void
invitation_callback_data_free (InvitationCallbackData *data)
{
  g_weak_ref_clear (&data->controller_ref);
  g_slice_free (InvitationCallbackData, data);
}

static void
on_fetch_invitations_response (GnalkHttpClient *client G_GNUC_UNUSED,
                               JsonNode         *response,
                               GError           *error,
                               gpointer          user_data)
{
  InvitationCallbackData *cb_data = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      invitation_callback_data_free (cb_data);
      return;
    }

  /* Owner may have been destroyed while the request was in flight; g_autoptr
   * balances g_weak_ref_get on every return path. */
  g_autoptr(GnalkConversationController) self = g_weak_ref_get (&cb_data->controller_ref);
  invitation_callback_data_free (cb_data);

  if (!self)
    return;

  if (error)
    {
      g_debug ("ConversationController: Fetch invitations failed: %s", error->message);
      return;
    }

  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    return;

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    return;

  JsonNode *data_node = json_object_get_member (ocs, "data");
  if (!data_node || !JSON_NODE_HOLDS_ARRAY (data_node))
    return;

  JsonArray *invitations = json_node_get_array (data_node);
  g_debug ("ConversationController: Fetched %u federation invitations",
           json_array_get_length (invitations));

  g_signal_emit (self, signals[SIGNAL_INVITATIONS_UPDATED], 0, invitations);
}

void
gnalk_conversation_controller_fetch_invitations (GnalkConversationController *self)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);

  InvitationCallbackData *cb_data = g_slice_new0 (InvitationCallbackData);
  g_weak_ref_init (&cb_data->controller_ref, self);
  cb_data->invitation_id = 0;

  gnalk_http_client_get_federation_invitations (self->http_client,
                                                on_fetch_invitations_response,
                                                cb_data);
}

static void
on_accept_invitation_response (GnalkHttpClient *client G_GNUC_UNUSED,
                               JsonNode         *response G_GNUC_UNUSED,
                               GError           *error,
                               gpointer          user_data)
{
  InvitationCallbackData *cb_data = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      invitation_callback_data_free (cb_data);
      return;
    }

  /* Owner may have been destroyed while the request was in flight; g_autoptr
   * balances g_weak_ref_get on every return path. */
  g_autoptr(GnalkConversationController) self = g_weak_ref_get (&cb_data->controller_ref);
  gint64 invitation_id = cb_data->invitation_id;
  invitation_callback_data_free (cb_data);

  if (!self)
    return;

  if (error)
    {
      g_warning ("ConversationController: Accept invitation failed: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
      return;
    }

  g_debug ("ConversationController: Accepted invitation %" G_GINT64_FORMAT, invitation_id);
  g_signal_emit (self, signals[SIGNAL_INVITATION_ACCEPTED], 0, invitation_id);
}

void
gnalk_conversation_controller_accept_invitation (GnalkConversationController *self,
                                                 gint64                       invitation_id)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);

  InvitationCallbackData *cb_data = g_slice_new0 (InvitationCallbackData);
  g_weak_ref_init (&cb_data->controller_ref, self);
  cb_data->invitation_id = invitation_id;

  gnalk_http_client_accept_federation_invitation (self->http_client,
                                                  invitation_id,
                                                  on_accept_invitation_response,
                                                  cb_data);
}

static void
on_decline_invitation_response (GnalkHttpClient *client G_GNUC_UNUSED,
                                JsonNode         *response G_GNUC_UNUSED,
                                GError           *error,
                                gpointer          user_data)
{
  InvitationCallbackData *cb_data = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      invitation_callback_data_free (cb_data);
      return;
    }

  /* Owner may have been destroyed while the request was in flight; g_autoptr
   * balances g_weak_ref_get on every return path. */
  g_autoptr(GnalkConversationController) self = g_weak_ref_get (&cb_data->controller_ref);
  gint64 invitation_id = cb_data->invitation_id;
  invitation_callback_data_free (cb_data);

  if (!self)
    return;

  if (error)
    {
      g_warning ("ConversationController: Decline invitation failed: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
      return;
    }

  g_debug ("ConversationController: Declined invitation %" G_GINT64_FORMAT, invitation_id);
  g_signal_emit (self, signals[SIGNAL_INVITATION_DECLINED], 0, invitation_id);
}

void
gnalk_conversation_controller_decline_invitation (GnalkConversationController *self,
                                                   gint64                       invitation_id)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);

  InvitationCallbackData *cb_data = g_slice_new0 (InvitationCallbackData);
  g_weak_ref_init (&cb_data->controller_ref, self);
  cb_data->invitation_id = invitation_id;

  gnalk_http_client_decline_federation_invitation (self->http_client,
                                                    invitation_id,
                                                    on_decline_invitation_response,
                                                    cb_data);
}
