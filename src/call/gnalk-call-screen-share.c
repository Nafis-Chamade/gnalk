/* gnalk-call-screen-share.c
 *
 * Outgoing screen share management for the call window.
 * Handles screen capture lifecycle, WebRTC screen publisher,
 * and MCU messaging for screen sharing.
 */

#include "gnalk-call-window-private.h"

/* Forward declaration for callback cross-reference */
static void on_screen_share_toggled_internal (GtkToggleButton *button, GnalkCallWindow *self);
static void on_screen_capture_started (GnalkScreenCapture *capture, guint32 node_id, int fd, gpointer user_data);
static void on_screen_capture_stopped (GnalkScreenCapture *capture, gpointer user_data);
static void on_screen_capture_failed (GnalkScreenCapture *capture, const char *error_message, gpointer user_data);

/* Screen share publisher WebRTC signal handler - sends to MCU */
static void
on_screen_publisher_signal (GnalkWebrtc *webrtc G_GNUC_UNUSED,
                            const char   *type,
                            JsonNode     *data,
                            gpointer      user_data)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (user_data);

  if (!self->signaling || !self->has_mcu)
    return;

  gnalk_call_signaling_send_to_mcu_screen (self, type, data);
}

/* Screen capture started callback - create WebRTC screen publisher */
static void
on_screen_capture_started (GnalkScreenCapture *capture G_GNUC_UNUSED,
                           guint32              node_id,
                           int                  fd,
                           gpointer             user_data)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (user_data);

  g_message ("Screen capture started: node_id=%u, fd=%d", node_id, fd);

  /* Create screen publisher WebRTC instance */
  self->screen_publisher = gnalk_webrtc_new ();

  /* Configure STUN/TURN for screen publisher from application settings */
  GtkApplication *gtk_app = gtk_window_get_application (GTK_WINDOW (self));
  if (GNALK_IS_APPLICATION (gtk_app))
    {
      GnalkApplication *app = GNALK_APPLICATION (gtk_app);
      const char *stun = gnalk_application_get_stun_server (app);
      const char *turn = gnalk_application_get_turn_server (app);
      const char *turn_user = gnalk_application_get_turn_username (app);
      const char *turn_cred = gnalk_application_get_turn_credential (app);

      if (stun)
        gnalk_webrtc_set_stun_server (self->screen_publisher, stun);
      if (turn && turn_user && turn_cred)
        gnalk_webrtc_set_turn_server (self->screen_publisher, turn, turn_user, turn_cred);
    }

  /* Set signal callback for SDP/ICE */
  gnalk_webrtc_set_signal_callback (self->screen_publisher,
                                     on_screen_publisher_signal,
                                     self);

  /* Start screen share pipeline */
  gnalk_webrtc_start_screen_share (self->screen_publisher,
                                    self->signaling_session_id,
                                    node_id, fd);

  self->is_sharing_screen = TRUE;

  /* Notify other participants that we're sharing screen.
   * Note: Don't call update_incall here - we already started with flags=7 (audio+video+screen)
   * and the screen share goes through a separate WebRTC connection with roomType="screen". */
  if (self->signaling)
    gnalk_signaling_send_share_screen (self->signaling);
}

/* Screen capture stopped callback - clean up screen publisher */
static void
on_screen_capture_stopped (GnalkScreenCapture *capture G_GNUC_UNUSED,
                           gpointer             user_data)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (user_data);

  g_message ("Screen capture stopped");

  /* Send "remove" message to MCU to unpublish the screen share feed.
   * This tells Janus to close the publisher handle for this session. */
  if (self->signaling && self->signaling_session_id && self->screen_publisher_sid)
    {
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
      json_builder_set_member_name (builder, "to");
      json_builder_add_string_value (builder, self->signaling_session_id);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "unshareScreen");
      json_builder_set_member_name (builder, "roomType");
      json_builder_add_string_value (builder, "screen");
      json_builder_end_object (builder);
      json_builder_end_object (builder);
      json_builder_end_object (builder);

      g_autoptr(JsonNode) root = json_builder_get_root (builder);
      gnalk_signaling_send_message (self->signaling, root);
      g_message ("Sent unshareScreen to MCU for screen share sid=%s", self->screen_publisher_sid);
    }

  /* Clean up screen publisher */
  if (self->screen_publisher)
    {
      gnalk_webrtc_end_call (self->screen_publisher);
      g_clear_object (&self->screen_publisher);
    }

  /* Generate a new sid for the next screen share session.
   * Reusing the old sid causes DTLS to stall because Janus may have stale state. */
  g_clear_pointer (&self->screen_publisher_sid, g_free);
  self->is_sharing_screen = FALSE;

  /* Notify other participants that we stopped sharing screen.
   * Note: Don't change incall flags - keep flags=7 to maintain capabilities. */
  if (self->signaling)
    gnalk_signaling_send_unshare_screen (self->signaling);
}

/* Screen capture failed callback */
static void
on_screen_capture_failed (GnalkScreenCapture *capture G_GNUC_UNUSED,
                          const char          *error_message,
                          gpointer             user_data)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (user_data);

  g_warning ("Screen capture failed: %s", error_message);

  /* Reset screen sharing state */
  self->is_sharing_screen = FALSE;

  /* Reset the toggle button without triggering the toggled signal again */
  if (self->screen_share_button)
    {
      g_signal_handlers_block_by_func (self->screen_share_button,
                                       on_screen_share_toggled_internal, self);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->screen_share_button), FALSE);
      gtk_button_set_icon_name (GTK_BUTTON (self->screen_share_button),
                                "preferences-desktop-display-symbolic");
      g_signal_handlers_unblock_by_func (self->screen_share_button,
                                         on_screen_share_toggled_internal, self);
    }
}

/* Internal toggle handler for signal blocking */
static void
on_screen_share_toggled_internal (GtkToggleButton  *button,
                                   GnalkCallWindow *self)
{
  gboolean active = gtk_toggle_button_get_active (button);

  if (active)
    {
      /* Start screen sharing */
      if (!self->screen_capture)
        {
          self->screen_capture = gnalk_screen_capture_new ();
          g_signal_connect (self->screen_capture, "capture-started",
                            G_CALLBACK (on_screen_capture_started), self);
          g_signal_connect (self->screen_capture, "capture-stopped",
                            G_CALLBACK (on_screen_capture_stopped), self);
          g_signal_connect (self->screen_capture, "capture-failed",
                            G_CALLBACK (on_screen_capture_failed), self);
        }

      gnalk_screen_capture_start (self->screen_capture);

      gtk_button_set_icon_name (GTK_BUTTON (button), "screen-shared-symbolic");
    }
  else
    {
      /* Stop screen sharing */
      if (self->screen_capture)
        gnalk_screen_capture_stop (self->screen_capture);

      gtk_button_set_icon_name (GTK_BUTTON (button), "preferences-desktop-display-symbolic");
    }
}

/* Public screen share toggle handler - called from UI signal connect */
void
gnalk_call_screen_share_on_toggled (GtkToggleButton  *button,
                                      GnalkCallWindow *self)
{
  on_screen_share_toggled_internal (button, self);
}

/* Send screen share offers to all participants - used after MCU accepts our share */
void
gnalk_call_screen_share_send_offers_to_all (GnalkCallWindow *self)
{
  if (!self->participant_tiles)
    return;

  GHashTableIter iter;
  gpointer key, value;
  int count = 0;

  g_hash_table_iter_init (&iter, self->participant_tiles);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *session_id = (const char *)key;
      /* Don't send to ourselves */
      if (self->signaling_session_id && g_strcmp0 (session_id, self->signaling_session_id) == 0)
        continue;

      /* Send sendoffer message */
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
      json_builder_add_string_value (builder, session_id);
      json_builder_end_object (builder);
      json_builder_set_member_name (builder, "data");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "type");
      json_builder_add_string_value (builder, "sendoffer");
      json_builder_set_member_name (builder, "roomType");
      json_builder_add_string_value (builder, "screen");
      json_builder_end_object (builder);
      json_builder_end_object (builder);
      json_builder_end_object (builder);

      g_autoptr(JsonNode) node = json_builder_get_root (builder);
      gnalk_signaling_send_message (self->signaling, node);
      count++;
    }

  g_message ("Sent screen share offer triggers to %d participants", count);
}

/* Handle screen share signal from signaling - incoming screen share notification */
void
gnalk_call_screen_share_on_signaling_changed (GnalkSignaling  *signaling G_GNUC_UNUSED,
                                                const char       *session_id,
                                                gboolean          is_sharing,
                                                GnalkCallWindow *self)
{
  g_message ("Screen share %s from session %s",
             is_sharing ? "started" : "stopped", session_id);

  if (is_sharing)
    {
      /* Create screen tile for incoming screen share */
      if (!g_hash_table_contains (self->screen_tiles, session_id))
        {
          /* Create a tile for the screen share */
          GnalkParticipantTile *tile = gnalk_participant_tile_new ();
          gnalk_participant_tile_set_is_screen_share (tile, TRUE);
          gnalk_participant_tile_set_video_enabled (tile, TRUE);
          gnalk_participant_tile_set_session_id (tile, session_id);

          const char *display_name = g_hash_table_lookup (self->peer_display_names, session_id);
          if (display_name)
            {
              g_autofree char *label = g_strdup_printf ("%s's screen", display_name);
              gnalk_participant_tile_set_display_name (tile, label);
            }
          else
            {
              gnalk_participant_tile_set_display_name (tile, "Screen Share");
            }

          g_hash_table_insert (self->screen_tiles,
                               g_strdup (session_id),
                               tile);

          /* Create subscriber WebRTC for the screen stream */
          GnalkWebrtc *subscriber = gnalk_webrtc_new ();

          /* Configure STUN/TURN servers */
          GnalkApplication *app = GNALK_APPLICATION (g_application_get_default ());
          if (app)
            {
              const char *stun = gnalk_application_get_stun_server (app);
              const char *turn = gnalk_application_get_turn_server (app);
              const char *turn_user = gnalk_application_get_turn_username (app);
              const char *turn_cred = gnalk_application_get_turn_credential (app);

              if (stun)
                gnalk_webrtc_set_stun_server (subscriber, stun);
              if (turn && turn_user && turn_cred)
                gnalk_webrtc_set_turn_server (subscriber, turn, turn_user, turn_cred);
            }

          /* Set up callbacks with session_id context */
          TileVideoCallbackData *video_cb_data = g_new0 (TileVideoCallbackData, 1);
          g_weak_ref_init (&video_cb_data->window_ref, self);
          video_cb_data->session_id = g_strdup (session_id);

          TileVideoCallbackData *signal_cb_data = g_new0 (TileVideoCallbackData, 1);
          g_weak_ref_init (&signal_cb_data->window_ref, self);
          signal_cb_data->session_id = g_strdup (session_id);

          TileVideoCallbackData *conn_cb_data = g_new0 (TileVideoCallbackData, 1);
          g_weak_ref_init (&conn_cb_data->window_ref, self);
          conn_cb_data->session_id = g_strdup (session_id);

          gnalk_webrtc_set_remote_video_callback (subscriber, gnalk_call_participant_on_screen_remote_video_frame, video_cb_data);
          gnalk_webrtc_set_signal_callback (subscriber, gnalk_call_signaling_on_screen_subscriber_signal, signal_cb_data);
          gnalk_webrtc_set_connection_state_callback (subscriber, gnalk_call_signaling_on_screen_subscriber_connection_state, conn_cb_data);

          /* Enable continuous keyframe requests for screen share quality */
          gnalk_webrtc_enable_continuous_pli (subscriber, TRUE, 2000);

          g_hash_table_insert (self->screen_subscribers,
                               g_strdup (session_id),
                               subscriber);

          /* Request screen offer from the peer (for non-MCU mode) */
          gnalk_call_signaling_request_screen_offer_from_peer (self, session_id);

          gnalk_call_layout_recalculate (self);
        }
    }
  else
    {
      /* Remove screen tile */
      GnalkParticipantTile *tile = g_hash_table_lookup (self->screen_tiles, session_id);
      if (tile)
        {
          /* Use remove_tile_from_parent to handle grid/speaker layouts */
          gnalk_call_layout_remove_tile_from_parent (self, GTK_WIDGET (tile));
          g_hash_table_remove (self->screen_tiles, session_id);
        }

      /* Clean up subscriber */
      GnalkWebrtc *subscriber = g_hash_table_lookup (self->screen_subscribers, session_id);
      if (subscriber)
        {
          gnalk_webrtc_end_call (subscriber);
          g_hash_table_remove (self->screen_subscribers, session_id);
        }

      g_hash_table_remove (self->screen_sids, session_id);

      gnalk_call_layout_recalculate (self);
    }
}
