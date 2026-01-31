/* gnalk-call-participant-manager.c
 *
 * Participant tile management for the call window.
 * Handles creating, updating, and removing participant tiles,
 * avatar loading, and video frame callbacks.
 */

#include "gnalk-call-window-private.h"

/* Data for tile avatar loading callback */
typedef struct {
  GWeakRef window_ref;
  char *session_id;
} TileAvatarCallbackData;

static void
tile_avatar_callback_data_free (TileAvatarCallbackData *data)
{
  g_weak_ref_clear (&data->window_ref);
  g_free (data->session_id);
  g_free (data);
}

static void
on_tile_avatar_loaded (GnalkAvatarCache *cache G_GNUC_UNUSED,
                       GdkTexture        *texture,
                       gpointer           user_data)
{
  TileAvatarCallbackData *data = user_data;
  GnalkCallWindow *self = g_weak_ref_get (&data->window_ref);

  g_message ("on_tile_avatar_loaded: session=%s, self=%p, texture=%p",
             data->session_id, (void *)self, (void *)texture);

  if (!self || !texture)
    {
      g_message ("on_tile_avatar_loaded: early return (self=%p, texture=%p)", (void *)self, (void *)texture);
      tile_avatar_callback_data_free (data);
      if (self)
        g_object_unref (self);
      return;
    }

  GnalkParticipantTile *tile = g_hash_table_lookup (self->participant_tiles, data->session_id);
  g_message ("on_tile_avatar_loaded: tile=%p for session=%s", (void *)tile, data->session_id);
  if (tile)
    gnalk_participant_tile_set_avatar (tile, GDK_PAINTABLE (texture));

  tile_avatar_callback_data_free (data);
  g_object_unref (self);
}

/* Remote video frame callback for participant tiles */
void
gnalk_call_participant_on_tile_remote_video_frame (GnalkWebrtc *webrtc G_GNUC_UNUSED,
                                                     GdkTexture   *texture,
                                                     gpointer      user_data)
{
  TileVideoCallbackData *cb_data = user_data;
  GnalkCallWindow *self = g_weak_ref_get (&cb_data->window_ref);

  if (!self)
    {
      g_message ("tile_remote_video_frame: window ref is NULL, session_id=%s",
                 cb_data->session_id ? cb_data->session_id : "NULL");
      return;
    }

  GnalkParticipantTile *tile = g_hash_table_lookup (self->participant_tiles, cb_data->session_id);
  if (tile)
    {
      static int frame_count = 0;
      if (frame_count++ % 100 == 0)
        g_message ("tile_remote_video_frame: received frame %d for session=%.30s...", frame_count, cb_data->session_id);
      gnalk_participant_tile_update_video_frame (tile, texture);
    }
  else
    {
      g_message ("tile_remote_video_frame: tile not found for session_id=%s (hash_table has %u entries)",
                 cb_data->session_id ? cb_data->session_id : "NULL",
                 g_hash_table_size (self->participant_tiles));
    }

  g_object_unref (self);
}

/* Callback for screen share video frames */
void
gnalk_call_participant_on_screen_remote_video_frame (GnalkWebrtc *webrtc G_GNUC_UNUSED,
                                                       GdkTexture   *texture,
                                                       gpointer      user_data)
{
  TileVideoCallbackData *cb_data = user_data;
  GnalkCallWindow *self = g_weak_ref_get (&cb_data->window_ref);

  /* Track per-callback-data frame count to detect per-session issues */
  cb_data->frame_count++;

  if (!self)
    {
      if (cb_data->frame_count % 100 == 0)
        g_message ("screen_remote_video_frame: window ref gone, frame %d for session=%.30s...",
                   cb_data->frame_count, cb_data->session_id);
      return;
    }

  GnalkParticipantTile *tile = g_hash_table_lookup (self->screen_tiles, cb_data->session_id);
  if (tile)
    {
      if (cb_data->frame_count % 100 == 0)
        g_message ("screen_remote_video_frame: frame %d for session=%.30s...",
                   cb_data->frame_count, cb_data->session_id);
      gnalk_participant_tile_update_video_frame (tile, texture);
    }
  else
    {
      if (cb_data->frame_count % 100 == 0)
        g_message ("screen_remote_video_frame: tile not found, frame %d for session=%.30s...",
                   cb_data->frame_count, cb_data->session_id);
    }

  g_object_unref (self);
}

/* Speaking callback for participant tiles - updates active speaker */
void
gnalk_call_participant_on_tile_speaking (GnalkWebrtc *webrtc G_GNUC_UNUSED,
                                           gboolean      speaking,
                                           gpointer      user_data)
{
  TileVideoCallbackData *cb_data = user_data;
  GnalkCallWindow *self = g_weak_ref_get (&cb_data->window_ref);

  if (!self)
    return;

  if (speaking)
    {
      /* Update active speaker - recalculate layout if in speaker mode */
      g_free (self->active_speaker_id);
      self->active_speaker_id = g_strdup (cb_data->session_id);

      if (self->layout_mode == GNALK_LAYOUT_SPEAKER_FOCUS)
        gnalk_call_layout_schedule_recalculation (self);
    }

  g_object_unref (self);
}

/* Local video frame callback - updates overlay preview */
void
gnalk_call_participant_on_local_video_frame (GnalkWebrtc *webrtc G_GNUC_UNUSED,
                                               GdkTexture   *texture,
                                               gpointer      user_data)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (user_data);
  static int display_count = 0;

  if (display_count++ % 30 == 0)
    g_message ("Local video callback: frame %d, texture=%p, local_video_frame=%p",
               display_count, (void*)texture, (void*)self->local_video_frame);

  /* Update the small overlay preview (shown during call) */
  if (self->local_video_frame && GTK_IS_PICTURE (self->local_video_frame))
    gtk_picture_set_paintable (GTK_PICTURE (self->local_video_frame), GDK_PAINTABLE (texture));
}

/* Forward declaration for subscriber signal handler - will be in signaling manager */
static void on_tile_subscriber_signal_wrapper (GnalkWebrtc *webrtc,
                                                const char   *type,
                                                JsonNode     *data,
                                                gpointer      user_data);

/* Add a participant tile for a remote peer */
void
gnalk_call_participant_add_tile (GnalkCallWindow *self,
                                   const char       *session_id,
                                   const char       *display_name,
                                   const char       *user_id)
{
  if (!session_id || g_hash_table_contains (self->participant_tiles, session_id))
    return;

  g_message ("Adding participant tile for session: %s (display_name=%s, user_id=%s)",
             session_id, display_name ? display_name : "NULL",
             user_id ? user_id : "NULL");

  /* Create the tile widget */
  GnalkParticipantTile *tile = gnalk_participant_tile_new ();
  gnalk_participant_tile_set_session_id (tile, session_id);
  gnalk_participant_tile_set_display_name (tile, display_name ? display_name : "Participant");
  gnalk_participant_tile_set_is_local (tile, FALSE);

  /* Create WebRTC subscriber instance for this participant */
  GnalkWebrtc *subscriber = gnalk_webrtc_new ();

  /* Configure ICE servers */
  GtkApplication *gtk_app = gtk_window_get_application (GTK_WINDOW (self));
  if (GNALK_IS_APPLICATION (gtk_app))
    {
      GnalkApplication *app = GNALK_APPLICATION (gtk_app);
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

  TileVideoCallbackData *speaking_cb_data = g_new0 (TileVideoCallbackData, 1);
  g_weak_ref_init (&speaking_cb_data->window_ref, self);
  speaking_cb_data->session_id = g_strdup (session_id);

  g_message ("add_participant_tile: setting up subscriber=%p with video_cb session=%.30s...",
             (void *)subscriber, session_id);

  gnalk_webrtc_set_remote_video_callback (subscriber, gnalk_call_participant_on_tile_remote_video_frame, video_cb_data);
  gnalk_webrtc_set_signal_callback (subscriber, on_tile_subscriber_signal_wrapper, signal_cb_data);
  gnalk_webrtc_set_speaking_callback (subscriber, gnalk_call_participant_on_tile_speaking, speaking_cb_data);

  gnalk_participant_tile_set_webrtc (tile, subscriber);
  g_object_unref (subscriber);  /* Tile now owns it */

  /* Store in hash table - take a reference since GTK widgets start with floating ref */
  g_hash_table_insert (self->participant_tiles, g_strdup (session_id), g_object_ref_sink (tile));

  /* Store user_id mapping and load avatar */
  if (user_id && *user_id)
    {
      /* Copy user_id first since inserting into peer_user_ids may free the original
       * if user_id came from a previous lookup in the same hash table */
      g_autofree char *user_id_copy = g_strdup (user_id);

      /* Only insert if not already present (avoid freeing the source pointer) */
      if (!g_hash_table_contains (self->peer_user_ids, session_id))
        g_hash_table_insert (self->peer_user_ids, g_strdup (session_id), g_strdup (user_id_copy));

      g_message ("add_participant_tile: loading avatar for user_id=%s, avatar_cache=%p", user_id_copy, (void *)self->avatar_cache);

      if (self->avatar_cache)
        {
          TileAvatarCallbackData *avatar_data = g_new0 (TileAvatarCallbackData, 1);
          g_weak_ref_init (&avatar_data->window_ref, self);
          avatar_data->session_id = g_strdup (session_id);
          gnalk_avatar_cache_get_user_avatar (self->avatar_cache, user_id_copy, 128,
                                                on_tile_avatar_loaded, avatar_data);
        }
      else
        {
          g_warning ("add_participant_tile: avatar_cache is NULL, cannot load avatar");
        }
    }
  else
    {
      g_message ("add_participant_tile: no user_id provided for session=%s", session_id);
    }

  g_message ("add_participant_tile: stored tile for session=%s, hash_table now has %u entries",
             session_id, g_hash_table_size (self->participant_tiles));

  /* Schedule layout recalculation - deferred to prevent crashes during rapid tile additions */
  gnalk_call_layout_schedule_recalculation (self);

  /* Switch to call view if not already */
  if (self->video_stack)
    gtk_stack_set_visible_child_name (GTK_STACK (self->video_stack), "call");
}

/* Remove a participant tile */
void
gnalk_call_participant_remove_tile (GnalkCallWindow *self,
                                      const char       *session_id)
{
  if (!session_id)
    return;

  GnalkParticipantTile *tile = g_hash_table_lookup (self->participant_tiles, session_id);
  if (!tile)
    {
      /* Log existing keys for debugging */
      GHashTableIter iter;
      gpointer key, value;
      g_message ("Tile not found for session: %s (hash_table has %u entries)",
                 session_id, g_hash_table_size (self->participant_tiles));
      g_hash_table_iter_init (&iter, self->participant_tiles);
      while (g_hash_table_iter_next (&iter, &key, &value))
        g_message ("  existing tile key: %s", (char *)key);
      return;
    }

  g_message ("Removing participant tile for session: %s", session_id);

  /* End WebRTC call if active */
  GnalkWebrtc *webrtc = gnalk_participant_tile_get_webrtc (tile);
  if (webrtc)
    gnalk_webrtc_end_call (webrtc);

  /* Remove tile from its parent container BEFORE removing from hash table
   * to avoid the container holding a reference to a partially destroyed widget */
  gnalk_call_layout_remove_tile_from_parent (self, GTK_WIDGET (tile));

  /* Remove from hash table (this will unref the tile) */
  g_hash_table_remove (self->participant_tiles, session_id);

  /* Clear active speaker if it was this participant */
  if (self->active_speaker_id && g_strcmp0 (self->active_speaker_id, session_id) == 0)
    {
      g_clear_pointer (&self->active_speaker_id, g_free);
    }

  /* Also clean up any screen share from this participant */
  if (self->screen_tiles)
    {
      GnalkParticipantTile *screen_tile = g_hash_table_lookup (self->screen_tiles, session_id);
      if (screen_tile)
        {
          g_message ("Also removing screen share tile for session: %s", session_id);
          GnalkWebrtc *screen_webrtc = gnalk_participant_tile_get_webrtc (screen_tile);
          if (screen_webrtc)
            gnalk_webrtc_end_call (screen_webrtc);
          gnalk_call_layout_remove_tile_from_parent (self, GTK_WIDGET (screen_tile));
          g_hash_table_remove (self->screen_tiles, session_id);
        }
    }

  /* Clean up screen subscriber for this participant */
  if (self->screen_subscribers)
    g_hash_table_remove (self->screen_subscribers, session_id);
  if (self->screen_sids)
    g_hash_table_remove (self->screen_sids, session_id);

  /* Schedule layout recalculation */
  gnalk_call_layout_schedule_recalculation (self);
}

/* Wrapper for tile subscriber signal - calls the signaling manager function.
 * This breaks the circular dependency between participant-manager and signaling-manager. */
static void
on_tile_subscriber_signal_wrapper (GnalkWebrtc *webrtc,
                                    const char   *type,
                                    JsonNode     *data,
                                    gpointer      user_data)
{
  gnalk_call_signaling_on_tile_subscriber_signal (webrtc, type, data, user_data);
}
