#include "gnalk-participant-controller.h"

#define PARTICIPANTS_REFRESH_INTERVAL_SECONDS 60

struct _GnalkParticipantController
{
  GObject parent_instance;

  GnalkDatabase   *database;
  GnalkHttpClient *http_client;

  gint64   account_id;
  char    *active_room_token;
  int      active_room_type;
  guint    poll_timer_id;

  /* Generation counter for stale response detection */
  guint generation;
};

G_DEFINE_FINAL_TYPE (GnalkParticipantController, gnalk_participant_controller, G_TYPE_OBJECT)

enum {
  SIGNAL_PARTICIPANTS_CHANGED,
  SIGNAL_PARTICIPANT_CALL_CHANGED,
  SIGNAL_ALL_CALL_STATES_CLEARED,
  SIGNAL_ERROR,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void do_fetch (GnalkParticipantController *self);

/* Callback data structure */
typedef struct {
  GnalkParticipantController *controller;
  char                        *room_token;
  guint                        generation;
} FetchCallbackData;

static void
fetch_callback_data_free (FetchCallbackData *data)
{
  g_free (data->room_token);
  g_slice_free (FetchCallbackData, data);
}

static void
gnalk_participant_controller_finalize (GObject *object)
{
  GnalkParticipantController *self = GNALK_PARTICIPANT_CONTROLLER (object);

  if (self->poll_timer_id > 0)
    {
      g_source_remove (self->poll_timer_id);
      self->poll_timer_id = 0;
    }

  g_clear_pointer (&self->active_room_token, g_free);
  g_clear_object (&self->database);
  g_clear_object (&self->http_client);

  G_OBJECT_CLASS (gnalk_participant_controller_parent_class)->finalize (object);
}

static void
gnalk_participant_controller_class_init (GnalkParticipantControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_participant_controller_finalize;

  signals[SIGNAL_PARTICIPANTS_CHANGED] =
    g_signal_new ("participants-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);  /* room_token */

  signals[SIGNAL_PARTICIPANT_CALL_CHANGED] =
    g_signal_new ("participant-call-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_STRING,   /* room_token */
                  G_TYPE_STRING,   /* actor_id */
                  G_TYPE_INT64);   /* in_call */

  signals[SIGNAL_ALL_CALL_STATES_CLEARED] =
    g_signal_new ("all-call-states-cleared",
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
}

static void
gnalk_participant_controller_init (GnalkParticipantController *self)
{
  self->generation = 0;
}

GnalkParticipantController *
gnalk_participant_controller_new (GnalkDatabase   *database,
                                    GnalkHttpClient *http_client)
{
  GnalkParticipantController *self = g_object_new (GNALK_TYPE_PARTICIPANT_CONTROLLER, NULL);

  self->database = database ? g_object_ref (database) : NULL;
  self->http_client = http_client ? g_object_ref (http_client) : NULL;

  return self;
}

void
gnalk_participant_controller_set_account_id (GnalkParticipantController *self,
                                                gint64                        account_id)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_CONTROLLER (self));

  self->account_id = account_id;
  self->generation++;  /* Invalidate pending operations from old account */
}

/* ============ Participant Fetching ============ */

static JsonArray *
extract_participants_array (JsonNode *response)
{
  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    return NULL;

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    return NULL;

  return json_object_get_array_member (ocs, "data");
}

/* Serialize participants to a string for comparison */
static char *
serialize_participants (JsonArray *participants)
{
  if (!participants)
    return g_strdup ("");

  g_autoptr(GString) str = g_string_new ("");
  guint len = json_array_get_length (participants);

  for (guint i = 0; i < len; i++)
    {
      JsonObject *p = json_array_get_object_element (participants, i);
      if (!p)
        continue;

      const char *actor_id = json_object_get_string_member_with_default (p, "actorId", "");
      gint64 in_call = json_object_get_int_member_with_default (p, "inCall", 0);
      const char *display_name = json_object_get_string_member_with_default (p, "displayName", "");
      const char *status = json_object_get_string_member_with_default (p, "status", "");

      /* Only compare fields that are stored in DB (actorId, inCall, displayName, status).
       * participantType is NOT stored, so including it would always detect a change. */
      g_string_append_printf (str, "%s:%ld:%s:%s;",
                              actor_id, (long) in_call, display_name, status);
    }

  return g_string_free (g_steal_pointer (&str), FALSE);
}

/* Compare new participants with cached ones */
static gboolean
participants_changed (GnalkParticipantController *self,
                      JsonArray                   *new_participants,
                      const char                  *room_token)
{
  if (!self->database || self->account_id <= 0)
    return TRUE;  /* No cache, assume changed */

  /* Load cached participants */
  JsonArray *cached = gnalk_database_load_participants (self->database,
                                                           self->account_id,
                                                           room_token);
  if (!cached)
    return TRUE;  /* No cache, assume changed */

  /* Compare serialized representations */
  g_autofree char *new_str = serialize_participants (new_participants);
  g_autofree char *cached_str = serialize_participants (cached);

  json_array_unref (cached);

  return g_strcmp0 (new_str, cached_str) != 0;
}

static void
on_fetch_response (GnalkHttpClient *client G_GNUC_UNUSED,
                   JsonNode         *response,
                   GError           *error,
                   gpointer          user_data)
{
  FetchCallbackData *cb_data = user_data;

  /* Check for cancellation before accessing controller */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      fetch_callback_data_free (cb_data);
      return;
    }

  GnalkParticipantController *self = cb_data->controller;
  guint expected_generation = cb_data->generation;
  char *room_token = g_strdup (cb_data->room_token);
  fetch_callback_data_free (cb_data);

  /* Discard stale response */
  if (expected_generation != self->generation)
    {
      g_debug ("ParticipantController: Discarding stale response for %s", room_token);
      g_free (room_token);
      return;
    }

  /* Verify this is still the active room */
  if (g_strcmp0 (room_token, self->active_room_token) != 0)
    {
      g_debug ("ParticipantController: Room changed, discarding response for %s", room_token);
      g_free (room_token);
      return;
    }

  if (error)
    {
      g_warning ("ParticipantController: Fetch failed for %s: %s", room_token, error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
      g_free (room_token);
      return;
    }

  JsonArray *participants = extract_participants_array (response);
  if (!participants)
    {
      g_free (room_token);
      return;
    }

  /* Check if data changed */
  gboolean changed = participants_changed (self, participants, room_token);

  /* Save to database */
  if (self->database && self->account_id > 0)
    gnalk_database_save_participants (self->database, self->account_id, room_token, participants);

  /* Only emit signal if data actually changed */
  if (changed)
    {
      g_debug ("ParticipantController: Participants changed for %s", room_token);
      g_signal_emit (self, signals[SIGNAL_PARTICIPANTS_CHANGED], 0, room_token);
    }
  else
    {
      g_debug ("ParticipantController: No change for %s", room_token);
    }

  g_free (room_token);
}

static void
do_fetch (GnalkParticipantController *self)
{
  if (!self->http_client || !self->active_room_token)
    return;

  if (!gnalk_http_client_is_ready (self->http_client))
    return;

  FetchCallbackData *cb_data = g_slice_new0 (FetchCallbackData);
  cb_data->controller = self;
  cb_data->room_token = g_strdup (self->active_room_token);
  cb_data->generation = self->generation;

  gnalk_http_client_get_participants (self->http_client,
                                         self->active_room_token,
                                         on_fetch_response,
                                         cb_data);
}

static gboolean
on_poll_timer (gpointer user_data)
{
  GnalkParticipantController *self = GNALK_PARTICIPANT_CONTROLLER (user_data);

  if (!self->active_room_token)
    {
      self->poll_timer_id = 0;
      return G_SOURCE_REMOVE;
    }

  g_debug ("ParticipantController: Polling participants for %s", self->active_room_token);
  do_fetch (self);

  return G_SOURCE_CONTINUE;
}

static void
start_polling (GnalkParticipantController *self)
{
  /* Only poll for group conversations (not 1-on-1) */
  if (self->active_room_type == 1)
    return;

  if (self->poll_timer_id > 0)
    return;

  g_debug ("ParticipantController: Starting polling for %s (every %ds)",
           self->active_room_token, PARTICIPANTS_REFRESH_INTERVAL_SECONDS);

  self->poll_timer_id = g_timeout_add_seconds (PARTICIPANTS_REFRESH_INTERVAL_SECONDS,
                                                 on_poll_timer, self);
}

static void
stop_polling (GnalkParticipantController *self)
{
  if (self->poll_timer_id > 0)
    {
      g_debug ("ParticipantController: Stopping polling");
      g_source_remove (self->poll_timer_id);
      self->poll_timer_id = 0;
    }
}

/* ============ Public API ============ */

void
gnalk_participant_controller_set_active_room (GnalkParticipantController *self,
                                                 const char                   *room_token,
                                                 int                           room_type)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_CONTROLLER (self));

  /* Stop any previous polling */
  stop_polling (self);

  /* Increment generation to invalidate in-flight requests */
  self->generation++;

  /* Update state */
  g_free (self->active_room_token);
  self->active_room_token = g_strdup (room_token);
  self->active_room_type = room_type;

  if (!room_token)
    {
      g_debug ("ParticipantController: Cleared active room");
      return;
    }

  g_debug ("ParticipantController: Active room set to %s (type %d)", room_token, room_type);

  /* Load cached participants for immediate display */
  if (self->database && self->account_id > 0)
    {
      JsonArray *cached = gnalk_database_load_participants (self->database,
                                                               self->account_id,
                                                               room_token);
      if (cached)
        {
          guint n = json_array_get_length (cached);
          json_array_unref (cached);
          if (n > 0)
            {
              g_debug ("ParticipantController: Emitting cached participants (%u) for %s", n, room_token);
              g_signal_emit (self, signals[SIGNAL_PARTICIPANTS_CHANGED], 0, room_token);
            }
        }
    }

  /* Fetch participants from server for the new room */
  do_fetch (self);

  /* Start polling for group rooms */
  start_polling (self);
}

void
gnalk_participant_controller_refresh (GnalkParticipantController *self)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_CONTROLLER (self));

  if (!self->active_room_token)
    return;

  g_debug ("ParticipantController: Manual refresh for %s", self->active_room_token);
  do_fetch (self);
}

const char *
gnalk_participant_controller_get_active_room (GnalkParticipantController *self)
{
  g_return_val_if_fail (GNALK_IS_PARTICIPANT_CONTROLLER (self), NULL);
  return self->active_room_token;
}

gboolean
gnalk_participant_controller_is_polling (GnalkParticipantController *self)
{
  g_return_val_if_fail (GNALK_IS_PARTICIPANT_CONTROLLER (self), FALSE);
  return self->poll_timer_id > 0;
}

/* ============ Moderator Actions ============ */

typedef struct {
  GnalkParticipantController *controller;
  char                        *room_token;
} ModeratorActionCallbackData;

static void
moderator_action_callback_data_free (ModeratorActionCallbackData *data)
{
  g_free (data->room_token);
  g_slice_free (ModeratorActionCallbackData, data);
}

static void
on_moderator_action_response (GnalkHttpClient *client G_GNUC_UNUSED,
                              JsonNode         *response G_GNUC_UNUSED,
                              GError           *error,
                              gpointer          user_data)
{
  ModeratorActionCallbackData *cb_data = user_data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      moderator_action_callback_data_free (cb_data);
      return;
    }

  GnalkParticipantController *self = cb_data->controller;

  if (error)
    {
      g_warning ("ParticipantController: Moderator action failed: %s", error->message);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, error->message);
      moderator_action_callback_data_free (cb_data);
      return;
    }

  /* Refresh participants to reflect the change */
  do_fetch (self);
  moderator_action_callback_data_free (cb_data);
}

void
gnalk_participant_controller_promote_moderator (GnalkParticipantController *self,
                                                const char                 *room_token,
                                                gint64                      attendee_id)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (room_token != NULL);

  ModeratorActionCallbackData *cb_data = g_slice_new0 (ModeratorActionCallbackData);
  cb_data->controller = self;
  cb_data->room_token = g_strdup (room_token);

  gnalk_http_client_promote_moderator (self->http_client, room_token,
                                       attendee_id, on_moderator_action_response, cb_data);
}

void
gnalk_participant_controller_demote_moderator (GnalkParticipantController *self,
                                               const char                 *room_token,
                                               gint64                      attendee_id)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (room_token != NULL);

  ModeratorActionCallbackData *cb_data = g_slice_new0 (ModeratorActionCallbackData);
  cb_data->controller = self;
  cb_data->room_token = g_strdup (room_token);

  gnalk_http_client_demote_moderator (self->http_client, room_token,
                                      attendee_id, on_moderator_action_response, cb_data);
}

void
gnalk_participant_controller_remove_attendee (GnalkParticipantController *self,
                                              const char                 *room_token,
                                              gint64                      attendee_id)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_CONTROLLER (self));
  g_return_if_fail (self->http_client != NULL);
  g_return_if_fail (room_token != NULL);

  ModeratorActionCallbackData *cb_data = g_slice_new0 (ModeratorActionCallbackData);
  cb_data->controller = self;
  cb_data->room_token = g_strdup (room_token);

  gnalk_http_client_remove_attendee (self->http_client, room_token,
                                     attendee_id, on_moderator_action_response, cb_data);
}

/* ============ Call State Updates ============ */

void
gnalk_participant_controller_update_call_state (GnalkParticipantController *self,
                                                   const char                   *actor_id,
                                                   gint64                        in_call)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_CONTROLLER (self));
  g_return_if_fail (actor_id != NULL);

  if (!self->active_room_token || !self->database || self->account_id <= 0)
    return;

  gboolean updated = gnalk_database_update_participant_call_state (self->database,
                                                                      self->account_id,
                                                                      self->active_room_token,
                                                                      actor_id,
                                                                      in_call);
  if (updated)
    g_signal_emit (self, signals[SIGNAL_PARTICIPANT_CALL_CHANGED], 0,
                   self->active_room_token, actor_id, in_call);
}

void
gnalk_participant_controller_clear_all_call_states (GnalkParticipantController *self)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_CONTROLLER (self));

  if (!self->active_room_token || !self->database || self->account_id <= 0)
    return;

  gnalk_database_clear_room_call_states (self->database,
                                            self->account_id,
                                            self->active_room_token);

  g_signal_emit (self, signals[SIGNAL_ALL_CALL_STATES_CLEARED], 0,
                 self->active_room_token);
}
