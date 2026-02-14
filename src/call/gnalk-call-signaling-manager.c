/* gnalk-call-signaling-manager.c
 *
 * WebRTC signaling message construction and routing for the call window.
 * Handles MCU/peer messaging, SDP/ICE exchange, and screen share signaling.
 */

#include "gnalk-call-window-private.h"

/* Generate a random session ID for WebRTC connections */
char *
gnalk_call_signaling_generate_random_sid (void)
{
  return g_uuid_string_random ();
}

/* Send signaling message to a peer for P2P mode */
void
gnalk_call_signaling_send_to_peer (GnalkCallWindow *self,
                                    const char       *peer_session_id,
                                    const char       *type,
                                    JsonNode         *data)
{
  g_return_if_fail (GNALK_IS_CALL_WINDOW (self));
  g_return_if_fail (peer_session_id != NULL);

  if (!self->signaling)
    return;

  g_autoptr(JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "message");
  json_builder_set_member_name (builder, "message");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "recipient");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "session");
  json_builder_set_member_name (builder, "sessionid");
  json_builder_add_string_value (builder, peer_session_id);
  json_builder_end_object (builder);
  json_builder_set_member_name (builder, "data");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, type);
  json_builder_set_member_name (builder, "payload");
  json_builder_add_value (builder, json_node_copy (data));
  json_builder_end_object (builder);
  json_builder_end_object (builder);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  gnalk_signaling_send_message (self->signaling, root);
}

/* Send message to MCU (our own session) for publishing */
void
gnalk_call_signaling_send_to_mcu (GnalkCallWindow *self,
                                   const char       *type,
                                   JsonNode         *data)
{
  g_return_if_fail (GNALK_IS_CALL_WINDOW (self));

  if (!self->signaling || !self->signaling_session_id)
    return;

  /* Generate publisher SID if not already done */
  if (!self->publisher_sid)
    self->publisher_sid = gnalk_call_signaling_generate_random_sid ();

  g_autoptr(JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "message");
  json_builder_set_member_name (builder, "message");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "recipient");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "session");
  json_builder_set_member_name (builder, "sessionid");
  json_builder_add_string_value (builder, self->signaling_session_id);
  json_builder_end_object (builder);
  json_builder_set_member_name (builder, "data");
  json_builder_begin_object (builder);

  /* Common fields for all message types */
  json_builder_set_member_name (builder, "to");
  json_builder_add_string_value (builder, self->signaling_session_id);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, type);
  json_builder_set_member_name (builder, "roomType");
  json_builder_add_string_value (builder, "video");
  json_builder_set_member_name (builder, "sid");
  json_builder_add_string_value (builder, self->publisher_sid);

  /* Build payload based on type */
  json_builder_set_member_name (builder, "payload");
  if (g_strcmp0 (type, "candidate") == 0)
    {
      /* Candidate needs to be nested: payload.candidate.{...} */
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "candidate");
      json_builder_add_value (builder, json_node_copy (data));
      json_builder_end_object (builder);
    }
  else if (g_strcmp0 (type, "offer") == 0 || g_strcmp0 (type, "answer") == 0)
    {
      /* Offer/answer: payload needs nick and the SDP data */
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "nick");
      json_builder_add_string_value (builder, "Gnalk User");
      /* Copy type and sdp from data */
      if (JSON_NODE_HOLDS_OBJECT (data))
        {
          JsonObject *data_obj = json_node_get_object (data);
          const char *sdp_type = json_object_get_string_member_with_default (data_obj, "type", type);
          const char *sdp = json_object_get_string_member_with_default (data_obj, "sdp", "");
          json_builder_set_member_name (builder, "type");
          json_builder_add_string_value (builder, sdp_type);
          json_builder_set_member_name (builder, "sdp");
          json_builder_add_string_value (builder, sdp);
        }
      json_builder_end_object (builder);
    }
  else
    {
      json_builder_add_value (builder, json_node_copy (data));
    }

  json_builder_end_object (builder);
  json_builder_end_object (builder);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  gnalk_signaling_send_message (self->signaling, root);
}

/* Request offer from a peer (for subscribing to their stream) */
void
gnalk_call_signaling_request_offer_from_peer (GnalkCallWindow *self,
                                               const char       *peer_session_id)
{
  g_return_if_fail (GNALK_IS_CALL_WINDOW (self));

  if (!self->signaling || !peer_session_id)
    return;

  g_message ("Requesting offer from peer: %s", peer_session_id);

  g_autoptr(JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "message");
  json_builder_set_member_name (builder, "message");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "recipient");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "session");
  json_builder_set_member_name (builder, "sessionid");
  json_builder_add_string_value (builder, peer_session_id);
  json_builder_end_object (builder);
  json_builder_set_member_name (builder, "data");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "requestoffer");
  json_builder_set_member_name (builder, "roomType");
  json_builder_add_string_value (builder, "video");
  json_builder_end_object (builder);
  json_builder_end_object (builder);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root_node = json_builder_get_root (builder);
  gnalk_signaling_send_message (self->signaling, root_node);
}

/* Send answer/candidate to peer for subscription (receiving their stream) */
void
gnalk_call_signaling_send_to_peer_for_subscription (GnalkCallWindow *self,
                                                     const char       *peer_session_id,
                                                     const char       *subscriber_sid,
                                                     const char       *type,
                                                     JsonNode         *data)
{
  g_return_if_fail (GNALK_IS_CALL_WINDOW (self));

  if (!self->signaling || !peer_session_id)
    return;

  /* Use the SID from the received offer */
  const char *sid = subscriber_sid ? subscriber_sid : "unknown";

  g_autoptr(JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "message");
  json_builder_set_member_name (builder, "message");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "recipient");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "session");
  json_builder_set_member_name (builder, "sessionid");
  json_builder_add_string_value (builder, peer_session_id);
  json_builder_end_object (builder);
  json_builder_set_member_name (builder, "data");
  json_builder_begin_object (builder);

  json_builder_set_member_name (builder, "to");
  json_builder_add_string_value (builder, peer_session_id);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, type);
  json_builder_set_member_name (builder, "roomType");
  json_builder_add_string_value (builder, "video");
  json_builder_set_member_name (builder, "sid");
  json_builder_add_string_value (builder, sid);

  json_builder_set_member_name (builder, "payload");
  if (g_strcmp0 (type, "candidate") == 0)
    {
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "candidate");
      json_builder_add_value (builder, json_node_copy (data));
      json_builder_end_object (builder);
    }
  else if (g_strcmp0 (type, "answer") == 0)
    {
      /* Answer payload needs nick and the SDP data */
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "nick");
      json_builder_add_string_value (builder, "Gnalk User");
      if (JSON_NODE_HOLDS_OBJECT (data))
        {
          JsonObject *data_obj = json_node_get_object (data);
          const char *sdp_type = json_object_get_string_member_with_default (data_obj, "type", type);
          const char *sdp = json_object_get_string_member_with_default (data_obj, "sdp", "");
          json_builder_set_member_name (builder, "type");
          json_builder_add_string_value (builder, sdp_type);
          json_builder_set_member_name (builder, "sdp");
          json_builder_add_string_value (builder, sdp);
        }
      json_builder_end_object (builder);
    }
  else
    {
      json_builder_add_value (builder, json_node_copy (data));
    }

  json_builder_end_object (builder);
  json_builder_end_object (builder);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root = json_builder_get_root (builder);
  gnalk_signaling_send_message (self->signaling, root);
}

/* Send screen share signaling to MCU with roomType: 'screen' */
void
gnalk_call_signaling_send_to_mcu_screen (GnalkCallWindow *self,
                                          const char       *type,
                                          JsonNode         *data)
{
  g_return_if_fail (GNALK_IS_CALL_WINDOW (self));

  if (!self->signaling || !self->signaling_session_id)
    return;

  /* Generate screen publisher SID if not already done */
  if (!self->screen_publisher_sid)
    self->screen_publisher_sid = gnalk_call_signaling_generate_random_sid ();

  g_autoptr(JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "message");
  json_builder_set_member_name (builder, "message");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "recipient");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "session");
  json_builder_set_member_name (builder, "sessionid");
  json_builder_add_string_value (builder, self->signaling_session_id);
  json_builder_end_object (builder);
  json_builder_set_member_name (builder, "data");
  json_builder_begin_object (builder);

  /* Common fields for screen share messages */
  json_builder_set_member_name (builder, "to");
  json_builder_add_string_value (builder, self->signaling_session_id);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, type);
  json_builder_set_member_name (builder, "roomType");
  json_builder_add_string_value (builder, "screen");  /* Screen share uses 'screen' roomType */
  json_builder_set_member_name (builder, "sid");
  json_builder_add_string_value (builder, self->screen_publisher_sid);

  /* Build payload based on type */
  json_builder_set_member_name (builder, "payload");
  if (g_strcmp0 (type, "candidate") == 0)
    {
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "candidate");
      json_builder_add_value (builder, json_node_copy (data));
      json_builder_end_object (builder);
    }
  else if (g_strcmp0 (type, "offer") == 0 || g_strcmp0 (type, "answer") == 0)
    {
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "nick");
      json_builder_add_string_value (builder, "Gnalk User");
      if (JSON_NODE_HOLDS_OBJECT (data))
        {
          JsonObject *data_obj = json_node_get_object (data);
          const char *sdp_type = json_object_get_string_member_with_default (data_obj, "type", type);
          const char *sdp = json_object_get_string_member_with_default (data_obj, "sdp", "");
          json_builder_set_member_name (builder, "type");
          json_builder_add_string_value (builder, sdp_type);
          json_builder_set_member_name (builder, "sdp");
          json_builder_add_string_value (builder, sdp);
        }
      json_builder_end_object (builder);
    }
  else
    {
      json_builder_add_value (builder, json_node_copy (data));
    }

  json_builder_end_object (builder);
  json_builder_end_object (builder);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) screen_root = json_builder_get_root (builder);
  gnalk_signaling_send_message (self->signaling, screen_root);
}

/* Request screen share offer from a peer */
void
gnalk_call_signaling_request_screen_offer_from_peer (GnalkCallWindow *self,
                                                      const char       *peer_session_id)
{
  g_return_if_fail (GNALK_IS_CALL_WINDOW (self));

  if (!self->signaling || !peer_session_id)
    return;

  g_message ("Requesting screen offer from peer: %s", peer_session_id);

  g_autoptr(JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "message");
  json_builder_set_member_name (builder, "message");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "recipient");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "session");
  json_builder_set_member_name (builder, "sessionid");
  json_builder_add_string_value (builder, peer_session_id);
  json_builder_end_object (builder);
  json_builder_set_member_name (builder, "data");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "requestoffer");
  json_builder_set_member_name (builder, "roomType");
  json_builder_add_string_value (builder, "screen");  /* Request screen offer */
  json_builder_end_object (builder);
  json_builder_end_object (builder);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) root_node = json_builder_get_root (builder);
  gnalk_signaling_send_message (self->signaling, root_node);
}

/* Send answer/candidate to peer for screen subscription */
void
gnalk_call_signaling_send_to_peer_for_screen_subscription (GnalkCallWindow *self,
                                                            const char       *peer_session_id,
                                                            const char       *subscriber_sid,
                                                            const char       *type,
                                                            JsonNode         *data)
{
  g_return_if_fail (GNALK_IS_CALL_WINDOW (self));

  if (!self->signaling || !peer_session_id)
    return;

  const char *sid = subscriber_sid ? subscriber_sid : "unknown";

  g_autoptr(JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "message");
  json_builder_set_member_name (builder, "message");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "recipient");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "session");
  json_builder_set_member_name (builder, "sessionid");
  json_builder_add_string_value (builder, peer_session_id);
  json_builder_end_object (builder);
  json_builder_set_member_name (builder, "data");
  json_builder_begin_object (builder);

  json_builder_set_member_name (builder, "to");
  json_builder_add_string_value (builder, peer_session_id);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, type);
  json_builder_set_member_name (builder, "roomType");
  json_builder_add_string_value (builder, "screen");
  json_builder_set_member_name (builder, "sid");
  json_builder_add_string_value (builder, sid);

  json_builder_set_member_name (builder, "payload");
  if (g_strcmp0 (type, "candidate") == 0)
    {
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "candidate");
      json_builder_add_value (builder, json_node_copy (data));
      json_builder_end_object (builder);
    }
  else if (g_strcmp0 (type, "answer") == 0)
    {
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "nick");
      json_builder_add_string_value (builder, "Gnalk User");
      if (JSON_NODE_HOLDS_OBJECT (data))
        {
          JsonObject *data_obj = json_node_get_object (data);
          const char *sdp_type = json_object_get_string_member_with_default (data_obj, "type", type);
          const char *sdp = json_object_get_string_member_with_default (data_obj, "sdp", "");
          json_builder_set_member_name (builder, "type");
          json_builder_add_string_value (builder, sdp_type);
          json_builder_set_member_name (builder, "sdp");
          json_builder_add_string_value (builder, sdp);
        }
      json_builder_end_object (builder);
    }
  else
    {
      json_builder_add_value (builder, json_node_copy (data));
    }

  json_builder_end_object (builder);
  json_builder_end_object (builder);
  json_builder_end_object (builder);

  g_autoptr(JsonNode) screen_root = json_builder_get_root (builder);
  gnalk_signaling_send_message (self->signaling, screen_root);
}

/* Signal handler for tile subscriber WebRTC - sends to the peer we're subscribed to */
void
gnalk_call_signaling_on_tile_subscriber_signal (GnalkWebrtc *webrtc G_GNUC_UNUSED,
                                                 const char   *type,
                                                 JsonNode     *data,
                                                 gpointer      user_data)
{
  TileVideoCallbackData *cb_data = user_data;
  GnalkCallWindow *self = g_weak_ref_get (&cb_data->window_ref);

  if (!self)
    return;

  if (!self->signaling || !cb_data->session_id)
    {
      g_object_unref (self);
      return;
    }

  /* Get the subscriber SID from the tile */
  CallParticipantRecord *record = g_hash_table_lookup (self->participants, cb_data->session_id);
  GnalkParticipantTile *tile = record ? record->tile : NULL;
  const char *subscriber_sid = tile ? gnalk_participant_tile_get_subscriber_sid (tile) : NULL;

  gnalk_call_signaling_send_to_peer_for_subscription (self, cb_data->session_id, subscriber_sid, type, data);
  g_object_unref (self);
}

/* Signal handler for screen subscriber WebRTC - sends answer/candidates to screen sharer */
void
gnalk_call_signaling_on_screen_subscriber_signal (GnalkWebrtc *webrtc,
                                                   const char   *type,
                                                   JsonNode     *data,
                                                   gpointer      user_data)
{
  TileVideoCallbackData *cb_data = user_data;
  GnalkCallWindow *self = g_weak_ref_get (&cb_data->window_ref);

  (void)webrtc;

  if (!self)
    return;

  if (!self->signaling || !cb_data->session_id)
    {
      g_object_unref (self);
      return;
    }

  /* Get the subscriber SID from participant record */
  CallParticipantRecord *screen_record = g_hash_table_lookup (self->participants, cb_data->session_id);
  const char *subscriber_sid = screen_record ? screen_record->screen_subscriber_sid : NULL;

  g_message ("Screen subscriber signal: type=%s to session=%.30s..., sid=%s",
             type, cb_data->session_id, subscriber_sid ? subscriber_sid : "NULL");

  gnalk_call_signaling_send_to_peer_for_screen_subscription (self, cb_data->session_id, subscriber_sid, type, data);
  g_object_unref (self);
}

/* Connection state callback for screen subscriber - tracks disconnections */
void
gnalk_call_signaling_on_screen_subscriber_connection_state (GnalkWebrtc *webrtc,
                                                             gboolean      connected,
                                                             gpointer      user_data)
{
  TileVideoCallbackData *cb_data = user_data;
  (void)webrtc;

  g_message ("Screen subscriber connection state: %s for session=%.30s...",
             connected ? "CONNECTED" : "DISCONNECTED", cb_data->session_id);

  if (!connected)
    {
      /* Connection lost - the screen share stream has ended */
      GnalkCallWindow *self = g_weak_ref_get (&cb_data->window_ref);
      if (self)
        {
          g_message ("Screen subscriber disconnected, removing tile for session=%.30s...",
                     cb_data->session_id);
          /* Note: Don't remove here, let the signaling handle cleanup */
          g_object_unref (self);
        }
    }
}

/* Publisher WebRTC signal handler - routes to MCU or peers */
void
gnalk_call_signaling_on_webrtc_signal (GnalkWebrtc *webrtc,
                                        const char   *type,
                                        JsonNode     *data,
                                        gpointer      user_data)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (user_data);

  (void)webrtc;

  if (!self->signaling)
    return;

  if (self->has_mcu)
    {
      gnalk_call_signaling_send_to_mcu (self, type, data);
      return;
    }

  /* P2P mode: send to all peers */
  if (self->peer_session_ids->len == 0)
    return;

  for (guint i = 0; i < self->peer_session_ids->len; i++)
    {
      const char *peer_id = g_ptr_array_index (self->peer_session_ids, i);
      gnalk_call_signaling_send_to_peer (self, peer_id, type, data);
    }
}

/* Publisher connection state callback */
void
gnalk_call_signaling_on_publisher_connection_state (GnalkWebrtc *webrtc,
                                                     gboolean      connected,
                                                     gpointer      user_data)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (user_data);

  (void)webrtc;

  if (connected && !self->publisher_connected)
    {
      self->publisher_connected = TRUE;

      /* Switch from fullscreen preview to call view */
      if (self->video_stack)
        gtk_stack_set_visible_child_name (GTK_STACK (self->video_stack), "call");

      /* Show local video preview only if video is enabled */
      if (self->local_video_box)
        gtk_widget_set_visible (self->local_video_box, self->video_enabled);

      /* Apply video enabled state to WebRTC */
      if (self->webrtc)
        gnalk_webrtc_set_video_enabled (self->webrtc, self->video_enabled);

      /* In MCU mode, request offers from all tracked peers */
      if (self->has_mcu)
        {
          for (guint i = 0; i < self->peer_session_ids->len; i++)
            {
              const char *peer_id = g_ptr_array_index (self->peer_session_ids, i);
              /* Only request if we don't already have a tile for this peer */
              CallParticipantRecord *peer_record = g_hash_table_lookup (self->participants, peer_id);
              if (!peer_record || !peer_record->tile)
                {
                  const char *display_name = peer_record ? peer_record->display_name : NULL;
                  const char *user_id = peer_record ? peer_record->user_id : NULL;
                  gnalk_call_participant_add_tile (self, peer_id, display_name, user_id);
                  gnalk_call_signaling_request_offer_from_peer (self, peer_id);
                }
              /* Note: Don't speculatively request screen offers here.
               * Screen offers are requested in on_signaling_screen_share_changed()
               * when we receive a "shareScreen" message from a peer. */
            }
        }

      /* Schedule layout recalculation now that we're connected */
      gnalk_call_layout_schedule_recalculation (self);
    }
}
