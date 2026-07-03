#include "gnalk-participant-tile.h"
#include <adwaita.h>

#define FROZEN_TIMEOUT_MS 3000  /* Show spinner after 3 seconds without frames */
#define FROZEN_CHECK_INTERVAL_MS 1000  /* Check every second */
#define INITIAL_CONNECTION_TIMEOUT_MS 30000  /* Show avatar after 30 seconds without video */

struct _GnalkParticipantTile
{
  GtkBox parent_instance;

  GtkWidget *video_overlay;
  GtkWidget *video_picture;
  GtkWidget *name_label;
  GtkWidget *mute_indicator;
  GtkWidget *avatar_overlay;
  GtkWidget *avatar;
  GtkWidget *loading_spinner;

  char *session_id;
  char *display_name;
  char *subscriber_sid;

  GnalkWebrtc *webrtc;
  gboolean is_local;
  gboolean video_enabled;
  gboolean audio_enabled;
  gboolean is_active_speaker;
  gboolean is_screen_share;  /* Whether this tile is for a screen share */
  gboolean has_video;  /* Whether we've received any video frame */

  /* Hand raise / reaction overlays (created programmatically) */
  GtkWidget *hand_raised_label;
  GtkWidget *reaction_label;

  gint64 last_frame_time;  /* Monotonic time of last received frame */
  gint64 tile_created_time;  /* Monotonic time when tile was created */
  guint frozen_check_id;   /* Timeout source ID for frozen check */
  gboolean stream_frozen;  /* Whether stream is currently frozen */
  gboolean connection_timed_out;  /* TRUE if initial connection timed out */
};

G_DEFINE_FINAL_TYPE (GnalkParticipantTile, gnalk_participant_tile, GTK_TYPE_BOX)

enum {
  PROP_0,
  PROP_SESSION_ID,
  PROP_DISPLAY_NAME,
  PROP_IS_LOCAL,
  PROP_VIDEO_ENABLED,
  PROP_AUDIO_ENABLED,
  PROP_ACTIVE_SPEAKER,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
gnalk_participant_tile_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  GnalkParticipantTile *self = GNALK_PARTICIPANT_TILE (object);

  switch (prop_id)
    {
    case PROP_SESSION_ID:
      g_value_set_string (value, self->session_id);
      break;
    case PROP_DISPLAY_NAME:
      g_value_set_string (value, self->display_name);
      break;
    case PROP_IS_LOCAL:
      g_value_set_boolean (value, self->is_local);
      break;
    case PROP_VIDEO_ENABLED:
      g_value_set_boolean (value, self->video_enabled);
      break;
    case PROP_AUDIO_ENABLED:
      g_value_set_boolean (value, self->audio_enabled);
      break;
    case PROP_ACTIVE_SPEAKER:
      g_value_set_boolean (value, self->is_active_speaker);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gnalk_participant_tile_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  GnalkParticipantTile *self = GNALK_PARTICIPANT_TILE (object);

  switch (prop_id)
    {
    case PROP_SESSION_ID:
      gnalk_participant_tile_set_session_id (self, g_value_get_string (value));
      break;
    case PROP_DISPLAY_NAME:
      gnalk_participant_tile_set_display_name (self, g_value_get_string (value));
      break;
    case PROP_IS_LOCAL:
      gnalk_participant_tile_set_is_local (self, g_value_get_boolean (value));
      break;
    case PROP_VIDEO_ENABLED:
      gnalk_participant_tile_set_video_enabled (self, g_value_get_boolean (value));
      break;
    case PROP_AUDIO_ENABLED:
      gnalk_participant_tile_set_audio_enabled (self, g_value_get_boolean (value));
      break;
    case PROP_ACTIVE_SPEAKER:
      gnalk_participant_tile_set_active_speaker (self, g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static gboolean
on_frozen_check_timeout (gpointer user_data)
{
  GnalkParticipantTile *self = GNALK_PARTICIPANT_TILE (user_data);

  /* Don't check if local tile or video is disabled */
  if (self->is_local || !self->video_enabled)
    {
      /* Log once when we skip a screen share tile due to video_enabled=FALSE */
      static gboolean logged_skip = FALSE;
      if (self->is_screen_share && !logged_skip)
        {
          g_message ("Tile %s: skipping frozen check (is_local=%d, video_enabled=%d, is_screen_share=%d)",
                     self->session_id ? self->session_id : "unknown",
                     self->is_local, self->video_enabled, self->is_screen_share);
          logged_skip = TRUE;
        }
      if (self->stream_frozen)
        {
          self->stream_frozen = FALSE;
          if (self->loading_spinner)
            {
              gtk_spinner_stop (GTK_SPINNER (self->loading_spinner));
              gtk_widget_set_visible (self->loading_spinner, FALSE);
            }
        }
      return G_SOURCE_CONTINUE;
    }

  gint64 now = g_get_monotonic_time ();

  /* Check for initial connection timeout - if no video has been received
   * within INITIAL_CONNECTION_TIMEOUT_MS, show avatar (peer may not support MCU) */
  if (!self->has_video && !self->connection_timed_out && self->tile_created_time > 0)
    {
      gint64 since_created_ms = (now - self->tile_created_time) / 1000;
      if (since_created_ms > INITIAL_CONNECTION_TIMEOUT_MS)
        {
          self->connection_timed_out = TRUE;
          g_message ("Initial connection timeout for tile %s - showing avatar",
                     self->session_id ? self->session_id : "unknown");

          /* Stop spinner and show avatar */
          if (self->loading_spinner)
            {
              gtk_spinner_stop (GTK_SPINNER (self->loading_spinner));
              gtk_widget_set_visible (self->loading_spinner, FALSE);
            }
          if (self->avatar_overlay)
            gtk_widget_set_visible (self->avatar_overlay, TRUE);
        }
    }

  /* Only check for frozen stream if we have received video before */
  if (self->last_frame_time > 0)
    {
      gint64 elapsed_ms = (now - self->last_frame_time) / 1000;

      if (elapsed_ms > FROZEN_TIMEOUT_MS)
        {
          /* Stream is frozen - show spinner and avatar */
          if (!self->stream_frozen)
            {
              self->stream_frozen = TRUE;
              g_debug ("Tile %s: stream FROZEN (no frames for %ld ms, is_screen_share=%d)",
                         self->session_id ? self->session_id : "unknown",
                         (long)elapsed_ms, self->is_screen_share);
              if (self->loading_spinner)
                {
                  gtk_widget_set_visible (self->loading_spinner, TRUE);
                  gtk_spinner_start (GTK_SPINNER (self->loading_spinner));
                }
              /* Show avatar when stream freezes */
              if (self->avatar_overlay)
                gtk_widget_set_visible (self->avatar_overlay, TRUE);
            }
        }
      else if (self->stream_frozen)
        {
          /* Stream recovered - hide spinner and avatar */
          self->stream_frozen = FALSE;
          if (self->loading_spinner)
            {
              gtk_spinner_stop (GTK_SPINNER (self->loading_spinner));
              gtk_widget_set_visible (self->loading_spinner, FALSE);
            }
          /* Hide avatar if we have video */
          if (self->avatar_overlay && self->has_video)
            gtk_widget_set_visible (self->avatar_overlay, FALSE);
        }
    }

  return G_SOURCE_CONTINUE;
}

static void
gnalk_participant_tile_finalize (GObject *object)
{
  GnalkParticipantTile *self = GNALK_PARTICIPANT_TILE (object);

  if (self->frozen_check_id > 0)
    {
      g_source_remove (self->frozen_check_id);
      self->frozen_check_id = 0;
    }

  g_free (self->session_id);
  g_free (self->display_name);
  g_free (self->subscriber_sid);
  g_clear_object (&self->webrtc);

  G_OBJECT_CLASS (gnalk_participant_tile_parent_class)->finalize (object);
}

static void
gnalk_participant_tile_class_init (GnalkParticipantTileClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = gnalk_participant_tile_get_property;
  object_class->set_property = gnalk_participant_tile_set_property;
  object_class->finalize = gnalk_participant_tile_finalize;

  properties[PROP_SESSION_ID] =
    g_param_spec_string ("session-id", NULL, NULL, NULL,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_DISPLAY_NAME] =
    g_param_spec_string ("display-name", NULL, NULL, NULL,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_IS_LOCAL] =
    g_param_spec_boolean ("is-local", NULL, NULL, FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_VIDEO_ENABLED] =
    g_param_spec_boolean ("video-enabled", NULL, NULL, TRUE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_AUDIO_ENABLED] =
    g_param_spec_boolean ("audio-enabled", NULL, NULL, TRUE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_ACTIVE_SPEAKER] =
    g_param_spec_boolean ("active-speaker", NULL, NULL, FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/net/skilpa/gnalk/gnalk-participant-tile.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkParticipantTile, video_overlay);
  gtk_widget_class_bind_template_child (widget_class, GnalkParticipantTile, video_picture);
  gtk_widget_class_bind_template_child (widget_class, GnalkParticipantTile, name_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkParticipantTile, mute_indicator);
  gtk_widget_class_bind_template_child (widget_class, GnalkParticipantTile, avatar_overlay);
  gtk_widget_class_bind_template_child (widget_class, GnalkParticipantTile, avatar);
  gtk_widget_class_bind_template_child (widget_class, GnalkParticipantTile, loading_spinner);
}

static void
gnalk_participant_tile_init (GnalkParticipantTile *self)
{
  self->video_enabled = TRUE;
  self->audio_enabled = TRUE;
  self->last_frame_time = 0;  /* No frame received yet */
  self->tile_created_time = g_get_monotonic_time ();  /* Track when tile was created */
  self->stream_frozen = TRUE; /* Start in frozen/loading state */
  self->connection_timed_out = FALSE;

  gtk_widget_init_template (GTK_WIDGET (self));

  /* Show spinner immediately until first frame arrives */
  if (self->loading_spinner)
    {
      gtk_widget_set_visible (self->loading_spinner, TRUE);
      gtk_spinner_start (GTK_SPINNER (self->loading_spinner));
    }

  /* Start periodic check for frozen stream */
  self->frozen_check_id = g_timeout_add (FROZEN_CHECK_INTERVAL_MS,
                                          on_frozen_check_timeout,
                                          self);
}

GnalkParticipantTile *
gnalk_participant_tile_new (void)
{
  return g_object_new (GNALK_TYPE_PARTICIPANT_TILE, NULL);
}

void
gnalk_participant_tile_set_session_id (GnalkParticipantTile *self,
                                         const char            *session_id)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_TILE (self));

  if (g_strcmp0 (self->session_id, session_id) == 0)
    return;

  g_free (self->session_id);
  self->session_id = g_strdup (session_id);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SESSION_ID]);
}

const char *
gnalk_participant_tile_get_session_id (GnalkParticipantTile *self)
{
  g_return_val_if_fail (GNALK_IS_PARTICIPANT_TILE (self), NULL);
  return self->session_id;
}

void
gnalk_participant_tile_set_display_name (GnalkParticipantTile *self,
                                           const char            *display_name)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_TILE (self));

  if (g_strcmp0 (self->display_name, display_name) == 0)
    return;

  g_free (self->display_name);
  self->display_name = g_strdup (display_name);

  if (self->name_label)
    gtk_label_set_text (GTK_LABEL (self->name_label), display_name ? display_name : "");

  /* Update avatar text */
  if (self->avatar && ADW_IS_AVATAR (self->avatar))
    adw_avatar_set_text (ADW_AVATAR (self->avatar), display_name ? display_name : "");

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_DISPLAY_NAME]);
}

const char *
gnalk_participant_tile_get_display_name (GnalkParticipantTile *self)
{
  g_return_val_if_fail (GNALK_IS_PARTICIPANT_TILE (self), NULL);
  return self->display_name;
}

void
gnalk_participant_tile_set_subscriber_sid (GnalkParticipantTile *self,
                                             const char            *subscriber_sid)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_TILE (self));

  g_free (self->subscriber_sid);
  self->subscriber_sid = g_strdup (subscriber_sid);
}

const char *
gnalk_participant_tile_get_subscriber_sid (GnalkParticipantTile *self)
{
  g_return_val_if_fail (GNALK_IS_PARTICIPANT_TILE (self), NULL);
  return self->subscriber_sid;
}

void
gnalk_participant_tile_set_webrtc (GnalkParticipantTile *self,
                                     GnalkWebrtc          *webrtc)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_TILE (self));
  g_return_if_fail (webrtc == NULL || GNALK_IS_WEBRTC (webrtc));

  g_set_object (&self->webrtc, webrtc);
}

GnalkWebrtc *
gnalk_participant_tile_get_webrtc (GnalkParticipantTile *self)
{
  g_return_val_if_fail (GNALK_IS_PARTICIPANT_TILE (self), NULL);
  return self->webrtc;
}

void
gnalk_participant_tile_set_is_local (GnalkParticipantTile *self,
                                       gboolean               is_local)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_TILE (self));

  if (self->is_local == is_local)
    return;

  self->is_local = is_local;

  if (is_local)
    gtk_widget_add_css_class (GTK_WIDGET (self), "local-tile");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "local-tile");

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_IS_LOCAL]);
}

gboolean
gnalk_participant_tile_get_is_local (GnalkParticipantTile *self)
{
  g_return_val_if_fail (GNALK_IS_PARTICIPANT_TILE (self), FALSE);
  return self->is_local;
}

void
gnalk_participant_tile_set_video_enabled (GnalkParticipantTile *self,
                                            gboolean               enabled)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_TILE (self));

  if (self->video_enabled == enabled)
    return;

  self->video_enabled = enabled;

  if (!enabled)
    {
      /* Video disabled - show avatar, hide spinner */
      if (self->avatar_overlay)
        gtk_widget_set_visible (self->avatar_overlay, TRUE);
      if (self->loading_spinner)
        {
          gtk_spinner_stop (GTK_SPINNER (self->loading_spinner));
          gtk_widget_set_visible (self->loading_spinner, FALSE);
        }
      self->stream_frozen = FALSE;
    }
  else
    {
      /* Video enabled - keep avatar visible, show spinner until frames arrive */
      if (self->avatar_overlay)
        gtk_widget_set_visible (self->avatar_overlay, TRUE);
      if (self->loading_spinner)
        {
          gtk_widget_set_visible (self->loading_spinner, TRUE);
          gtk_spinner_start (GTK_SPINNER (self->loading_spinner));
        }
      self->stream_frozen = TRUE;
    }

  if (self->video_picture)
    gtk_widget_set_visible (self->video_picture, enabled);

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_VIDEO_ENABLED]);
}

gboolean
gnalk_participant_tile_get_video_enabled (GnalkParticipantTile *self)
{
  g_return_val_if_fail (GNALK_IS_PARTICIPANT_TILE (self), FALSE);
  return self->video_enabled;
}

void
gnalk_participant_tile_set_audio_enabled (GnalkParticipantTile *self,
                                            gboolean               enabled)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_TILE (self));

  if (self->audio_enabled == enabled)
    return;

  self->audio_enabled = enabled;

  if (self->mute_indicator)
    gtk_widget_set_visible (self->mute_indicator, !enabled);

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_AUDIO_ENABLED]);
}

gboolean
gnalk_participant_tile_get_audio_enabled (GnalkParticipantTile *self)
{
  g_return_val_if_fail (GNALK_IS_PARTICIPANT_TILE (self), FALSE);
  return self->audio_enabled;
}

void
gnalk_participant_tile_set_active_speaker (GnalkParticipantTile *self,
                                             gboolean               active)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_TILE (self));

  if (self->is_active_speaker == active)
    return;

  self->is_active_speaker = active;

  if (active)
    gtk_widget_add_css_class (GTK_WIDGET (self), "active-speaker");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "active-speaker");

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ACTIVE_SPEAKER]);
}

gboolean
gnalk_participant_tile_get_active_speaker (GnalkParticipantTile *self)
{
  g_return_val_if_fail (GNALK_IS_PARTICIPANT_TILE (self), FALSE);
  return self->is_active_speaker;
}

void
gnalk_participant_tile_update_video_frame (GnalkParticipantTile *self,
                                             GdkTexture            *texture)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_TILE (self));

  if (self->video_picture && GTK_IS_PICTURE (self->video_picture) && texture)
    {
      gtk_picture_set_paintable (GTK_PICTURE (self->video_picture), GDK_PAINTABLE (texture));

      /* Record frame time for frozen detection */
      self->last_frame_time = g_get_monotonic_time ();
      self->has_video = TRUE;
      self->connection_timed_out = FALSE;  /* Reset timeout if connection recovers */

      /* Only hide avatar and spinner if video is enabled */
      if (self->video_enabled)
        {
          /* Hide avatar overlay when we have video */
          if (self->avatar_overlay && gtk_widget_get_visible (self->avatar_overlay))
            gtk_widget_set_visible (self->avatar_overlay, FALSE);

          /* Hide spinner if stream was frozen */
          if (self->stream_frozen)
            {
              self->stream_frozen = FALSE;
              if (self->loading_spinner)
                {
                  gtk_spinner_stop (GTK_SPINNER (self->loading_spinner));
                  gtk_widget_set_visible (self->loading_spinner, FALSE);
                }
            }
        }
    }
}

void
gnalk_participant_tile_set_avatar (GnalkParticipantTile *self,
                                     GdkPaintable          *avatar)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_TILE (self));

  if (self->avatar && ADW_IS_AVATAR (self->avatar))
    adw_avatar_set_custom_image (ADW_AVATAR (self->avatar), avatar);
}

void
gnalk_participant_tile_set_is_screen_share (GnalkParticipantTile *self,
                                              gboolean               is_screen_share)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_TILE (self));

  if (self->is_screen_share == is_screen_share)
    return;

  self->is_screen_share = is_screen_share;

  if (is_screen_share)
    {
      gtk_widget_add_css_class (GTK_WIDGET (self), "screen-share-tile");
      /* Use 'contain' for screen shares to show entire screen with letterboxing */
      if (self->video_picture)
        gtk_picture_set_content_fit (GTK_PICTURE (self->video_picture), GTK_CONTENT_FIT_CONTAIN);
    }
  else
    {
      gtk_widget_remove_css_class (GTK_WIDGET (self), "screen-share-tile");
      /* Use 'cover' for video to fill the tile */
      if (self->video_picture)
        gtk_picture_set_content_fit (GTK_PICTURE (self->video_picture), GTK_CONTENT_FIT_COVER);
    }
}

gboolean
gnalk_participant_tile_get_is_screen_share (GnalkParticipantTile *self)
{
  g_return_val_if_fail (GNALK_IS_PARTICIPANT_TILE (self), FALSE);
  return self->is_screen_share;
}

/* Create an overlay label inside the video_overlay GtkOverlay.
 * Returns a GtkLabel positioned at the specified alignment. */
static GtkWidget *
ensure_overlay_label (GnalkParticipantTile *self,
                      GtkWidget           **label_ptr,
                      GtkAlign              halign,
                      GtkAlign              valign)
{
  if (*label_ptr)
    return *label_ptr;

  *label_ptr = gtk_label_new ("");
  gtk_widget_set_halign (*label_ptr, halign);
  gtk_widget_set_valign (*label_ptr, valign);
  gtk_widget_set_margin_start (*label_ptr, 4);
  gtk_widget_set_margin_end (*label_ptr, 4);
  gtk_widget_set_margin_top (*label_ptr, 4);
  gtk_widget_set_margin_bottom (*label_ptr, 4);
  gtk_widget_add_css_class (*label_ptr, "tile-overlay-emoji");
  gtk_widget_set_visible (*label_ptr, FALSE);

  if (self->video_overlay && GTK_IS_OVERLAY (self->video_overlay))
    gtk_overlay_add_overlay (GTK_OVERLAY (self->video_overlay), *label_ptr);

  return *label_ptr;
}

void
gnalk_participant_tile_set_hand_raised (GnalkParticipantTile *self,
                                          gboolean               raised)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_TILE (self));

  GtkWidget *label = ensure_overlay_label (self, &self->hand_raised_label,
                                            GTK_ALIGN_END, GTK_ALIGN_START);
  if (raised)
    {
      /* Unicode: raised hand emoji */
      gtk_label_set_text (GTK_LABEL (label), "\xE2\x9C\x8B");
      gtk_widget_set_visible (label, TRUE);
    }
  else
    {
      gtk_widget_set_visible (label, FALSE);
    }
}

void
gnalk_participant_tile_set_reaction (GnalkParticipantTile *self,
                                       const char            *emoji)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_TILE (self));

  GtkWidget *label = ensure_overlay_label (self, &self->reaction_label,
                                            GTK_ALIGN_CENTER, GTK_ALIGN_CENTER);
  if (emoji && *emoji)
    {
      gtk_label_set_text (GTK_LABEL (label), emoji);
      gtk_widget_set_visible (label, TRUE);
    }
  else
    {
      gtk_widget_set_visible (label, FALSE);
    }
}
