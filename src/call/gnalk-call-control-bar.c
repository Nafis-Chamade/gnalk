/* gnalk-call-control-bar.c
 *
 * Control bar button handlers for the call window.
 * Handles hangup, mute audio/video, and camera switching.
 */

#include "gnalk-call-window-private.h"

/* Hangup button clicked - end call and close window */
void
gnalk_call_control_on_hangup_clicked (GtkButton        *button G_GNUC_UNUSED,
                                        GnalkCallWindow *self)
{
  g_message ("end_call triggered by: hangup button clicked");
  gnalk_call_window_end_call (self);
  g_signal_emit (self, gnalk_call_window_signals[GNALK_CALL_WINDOW_SIGNAL_CALL_ENDED], 0);
  gtk_window_close (GTK_WINDOW (self));
}

/* Audio mute toggle handler */
void
gnalk_call_control_on_mute_audio_toggled (GtkToggleButton  *button,
                                            GnalkCallWindow *self)
{
  gboolean muted = gtk_toggle_button_get_active (button);

  if (muted)
    gtk_button_set_icon_name (GTK_BUTTON (button), "microphone-sensitivity-muted-symbolic");
  else
    gtk_button_set_icon_name (GTK_BUTTON (button), "microphone-sensitivity-high-symbolic");

  if (self->webrtc)
    gnalk_webrtc_set_audio_enabled (self->webrtc, !muted);
}

/* Video mute toggle handler */
void
gnalk_call_control_on_mute_video_toggled (GtkToggleButton  *button,
                                            GnalkCallWindow *self)
{
  gboolean muted = gtk_toggle_button_get_active (button);

  /* Update video enabled state */
  self->video_enabled = !muted;

  if (muted)
    gtk_button_set_icon_name (GTK_BUTTON (button), "camera-disabled-symbolic");
  else
    gtk_button_set_icon_name (GTK_BUTTON (button), "camera-photo-symbolic");

  /* Hide/show local preview based on video state */
  if (self->local_video_box)
    gtk_widget_set_visible (self->local_video_box, !muted);

  if (self->webrtc)
    gnalk_webrtc_set_video_enabled (self->webrtc, !muted);
}

/* Switch camera button handler */
void
gnalk_call_control_on_switch_camera_clicked (GtkButton        *button G_GNUC_UNUSED,
                                               GnalkCallWindow *self)
{
#ifdef HAVE_GSTREAMER
  if (self->publisher_connected && self->webrtc)
    {
      /* During streaming: use input-selector to switch camera without renegotiation.
       * This keeps the WebRTC session alive - no need to restart the call. */
      g_message ("Switching camera using input-selector (no renegotiation)");
      gnalk_webrtc_switch_camera (self->webrtc);

      /* Update our local state to match */
      self->camera_position = gnalk_webrtc_get_camera_position (self->webrtc);
    }
  else if (self->preview_pipeline)
    {
      /* Preview mode: restart preview with new camera */
      if (self->camera_position == GNALK_CAMERA_FRONT)
        self->camera_position = GNALK_CAMERA_BACK;
      else
        self->camera_position = GNALK_CAMERA_FRONT;

      g_message ("Camera switched to %s (preview mode)",
                 self->camera_position == GNALK_CAMERA_FRONT ? "front" : "back");

      gnalk_call_preview_stop_local_preview (self);
      gnalk_call_preview_start_local_preview (self);
    }
  else
#endif
    {
      /* No active stream - just toggle preference for next call */
      if (self->camera_position == GNALK_CAMERA_FRONT)
        self->camera_position = GNALK_CAMERA_BACK;
      else
        self->camera_position = GNALK_CAMERA_FRONT;

      g_message ("Camera preference switched to %s",
                 self->camera_position == GNALK_CAMERA_FRONT ? "front" : "back");

      if (self->webrtc)
        gnalk_webrtc_set_camera_position (self->webrtc, self->camera_position);
    }
}
