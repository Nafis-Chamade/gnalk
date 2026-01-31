/* gnalk-media-capture-dialog.c
 *
 * Full-screen dialog for photo and video capture with preview mode.
 */

#include "gnalk-media-capture-dialog.h"
#include "gnalk-camera-preview.h"
#include "gnalk-video-player.h"

#include <glib/gstdio.h>
#include <unistd.h>

typedef enum {
  CAPTURE_MODE_CAMERA,   /* Live camera preview */
  CAPTURE_MODE_PREVIEW   /* Playing back recorded video */
} CaptureMode;

struct _GnalkMediaCaptureDialog
{
  AdwDialog parent_instance;

  /* Main overlay container */
  GtkWidget *overlay;

  /* Content stack for camera/preview */
  GtkWidget *content_stack;
  GnalkCameraPreview *camera_preview;
  GnalkVideoPlayer *video_player;

  /* Overlay buttons (top) */
  GtkWidget *close_button;
  GtkWidget *switch_camera_button;

  /* Overlay controls (bottom) */
  GtkWidget *camera_controls;
  GtkWidget *photo_button;
  GtkWidget *video_button;

  /* Preview mode controls */
  GtkWidget *preview_controls;
  GtkWidget *rerecord_button;
  GtkWidget *send_button;

  /* Recording state */
  CaptureMode mode;
  gboolean recording_video;
  GBytes *recorded_video;
  gdouble recorded_duration;
  char *temp_video_path;
};

G_DEFINE_FINAL_TYPE (GnalkMediaCaptureDialog, gnalk_media_capture_dialog, ADW_TYPE_DIALOG)

enum {
  SIGNAL_PHOTO_CAPTURED,
  SIGNAL_VIDEO_CAPTURED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void switch_to_camera_mode (GnalkMediaCaptureDialog *self);
static void switch_to_preview_mode (GnalkMediaCaptureDialog *self);

static void
on_close_clicked (GtkButton                *button G_GNUC_UNUSED,
                  GnalkMediaCaptureDialog *self)
{
  adw_dialog_close (ADW_DIALOG (self));
}

static void
on_switch_camera_clicked (GtkButton                *button G_GNUC_UNUSED,
                          GnalkMediaCaptureDialog *self)
{
  GnalkCameraPreviewPosition current = gnalk_camera_preview_get_position (self->camera_preview);

  /* Toggle between front and back camera */
  if (current == GNALK_CAMERA_PREVIEW_FRONT)
    gnalk_camera_preview_set_position (self->camera_preview, GNALK_CAMERA_PREVIEW_BACK);
  else
    gnalk_camera_preview_set_position (self->camera_preview, GNALK_CAMERA_PREVIEW_FRONT);
}

static void
on_photo_clicked (GtkButton                *button G_GNUC_UNUSED,
                  GnalkMediaCaptureDialog *self)
{
  g_message ("MediaCaptureDialog: Photo button clicked");

  GBytes *jpeg = gnalk_camera_preview_capture_photo (self->camera_preview);
  if (jpeg)
    {
      g_signal_emit (self, signals[SIGNAL_PHOTO_CAPTURED], 0, jpeg);
      g_bytes_unref (jpeg);

      /* Close dialog after capture */
      gnalk_camera_preview_stop (self->camera_preview);
      adw_dialog_close (ADW_DIALOG (self));
    }
  else
    {
      g_warning ("MediaCaptureDialog: Failed to capture photo");
    }
}

static void
on_video_clicked (GtkButton                *button G_GNUC_UNUSED,
                  GnalkMediaCaptureDialog *self)
{
  if (!self->recording_video)
    {
      /* Start recording */
      g_message ("MediaCaptureDialog: Starting video recording");

      /* Generate temp file path */
      g_clear_pointer (&self->temp_video_path, g_free);
      GError *error = NULL;
      int fd = g_file_open_tmp ("gnalk-video-XXXXXX.webm", &self->temp_video_path, &error);
      if (fd < 0)
        {
          g_warning ("MediaCaptureDialog: Failed to create temp file: %s", error->message);
          g_error_free (error);
          return;
        }
      close (fd);

      if (!gnalk_camera_preview_start_video_recording (self->camera_preview, self->temp_video_path))
        {
          g_warning ("MediaCaptureDialog: Failed to start video recording");
          g_unlink (self->temp_video_path);
          g_clear_pointer (&self->temp_video_path, g_free);
          return;
        }

      self->recording_video = TRUE;

      /* Update UI to show recording state */
      gtk_button_set_icon_name (GTK_BUTTON (self->video_button), "media-playback-stop-symbolic");
      gtk_widget_set_tooltip_text (self->video_button, "Stop Recording");

      /* Disable photo and switch buttons during recording */
      gtk_widget_set_sensitive (self->photo_button, FALSE);
      gtk_widget_set_sensitive (self->switch_camera_button, FALSE);
    }
  else
    {
      /* Stop recording */
      g_message ("MediaCaptureDialog: Stopping video recording");

      GBytes *video = gnalk_camera_preview_stop_video_recording (self->camera_preview);
      self->recording_video = FALSE;

      /* Restore UI state */
      gtk_button_set_icon_name (GTK_BUTTON (self->video_button), "media-record-symbolic");
      gtk_widget_set_tooltip_text (self->video_button, "Record Video");
      gtk_widget_set_sensitive (self->photo_button, TRUE);
      gtk_widget_set_sensitive (self->switch_camera_button, TRUE);

      if (video)
        {
          /* Store recorded video and switch to preview mode */
          g_clear_pointer (&self->recorded_video, g_bytes_unref);
          self->recorded_video = video;
          self->recorded_duration = gnalk_video_player_get_duration (self->video_player);

          switch_to_preview_mode (self);
        }
      else
        {
          g_warning ("MediaCaptureDialog: Recording returned no data");
          if (self->temp_video_path)
            {
              g_unlink (self->temp_video_path);
              g_clear_pointer (&self->temp_video_path, g_free);
            }
        }
    }
}

static void
on_rerecord_clicked (GtkButton                *button G_GNUC_UNUSED,
                     GnalkMediaCaptureDialog *self)
{
  g_message ("MediaCaptureDialog: Re-record button clicked");

  /* Stop video player */
  gnalk_video_player_stop (self->video_player);

  /* Discard recorded video */
  g_clear_pointer (&self->recorded_video, g_bytes_unref);

  /* Clean up temp file */
  if (self->temp_video_path)
    {
      g_unlink (self->temp_video_path);
      g_clear_pointer (&self->temp_video_path, g_free);
    }

  /* Switch back to camera mode */
  switch_to_camera_mode (self);
}

static void
on_send_clicked (GtkButton                *button G_GNUC_UNUSED,
                 GnalkMediaCaptureDialog *self)
{
  g_message ("MediaCaptureDialog: Send button clicked");

  if (self->recorded_video)
    {
      /* Emit video-captured signal with the recorded data */
      g_signal_emit (self, signals[SIGNAL_VIDEO_CAPTURED], 0,
                     self->recorded_video, self->recorded_duration);

      /* Clean up */
      gnalk_video_player_stop (self->video_player);
      gnalk_camera_preview_stop (self->camera_preview);

      if (self->temp_video_path)
        {
          g_unlink (self->temp_video_path);
          g_clear_pointer (&self->temp_video_path, g_free);
        }

      g_clear_pointer (&self->recorded_video, g_bytes_unref);

      adw_dialog_close (ADW_DIALOG (self));
    }
}

static void
switch_to_camera_mode (GnalkMediaCaptureDialog *self)
{
  self->mode = CAPTURE_MODE_CAMERA;

  /* Show camera preview, hide video player */
  gtk_stack_set_visible_child_name (GTK_STACK (self->content_stack), "camera");

  /* Show camera controls, hide preview controls */
  gtk_widget_set_visible (self->camera_controls, TRUE);
  gtk_widget_set_visible (self->preview_controls, FALSE);

  /* Show switch camera button */
  gtk_widget_set_visible (self->switch_camera_button, TRUE);

  /* Restart camera preview */
  gnalk_camera_preview_start (self->camera_preview);
}

static void
switch_to_preview_mode (GnalkMediaCaptureDialog *self)
{
  self->mode = CAPTURE_MODE_PREVIEW;

  /* Load video into player */
  if (self->recorded_video)
    {
      gnalk_video_player_set_data (self->video_player, self->recorded_video);
      /* Get duration after loading */
      self->recorded_duration = gnalk_video_player_get_duration (self->video_player);
      /* Auto-play for preview */
      gnalk_video_player_play (self->video_player);
    }

  /* Show video player, hide camera preview */
  gtk_stack_set_visible_child_name (GTK_STACK (self->content_stack), "player");

  /* Show preview controls, hide camera controls */
  gtk_widget_set_visible (self->camera_controls, FALSE);
  gtk_widget_set_visible (self->preview_controls, TRUE);

  /* Hide switch camera button in preview mode */
  gtk_widget_set_visible (self->switch_camera_button, FALSE);

  /* Stop camera to save resources */
  gnalk_camera_preview_stop (self->camera_preview);
}

static void
gnalk_media_capture_dialog_closed (AdwDialog *dialog)
{
  GnalkMediaCaptureDialog *self = GNALK_MEDIA_CAPTURE_DIALOG (dialog);

  /* Stop any recording in progress */
  if (self->recording_video)
    {
      GBytes *video = gnalk_camera_preview_stop_video_recording (self->camera_preview);
      g_clear_pointer (&video, g_bytes_unref);
      self->recording_video = FALSE;
    }

  /* Stop camera preview and video player when dialog closes */
  gnalk_camera_preview_stop (self->camera_preview);
  gnalk_video_player_stop (self->video_player);

  /* Clean up temp file */
  if (self->temp_video_path)
    {
      g_unlink (self->temp_video_path);
      g_clear_pointer (&self->temp_video_path, g_free);
    }

  g_clear_pointer (&self->recorded_video, g_bytes_unref);

  /* Chain up only if parent class has a closed method */
  AdwDialogClass *parent_class = ADW_DIALOG_CLASS (gnalk_media_capture_dialog_parent_class);
  if (parent_class->closed)
    parent_class->closed (dialog);
}

static void
gnalk_media_capture_dialog_class_init (GnalkMediaCaptureDialogClass *klass)
{
  AdwDialogClass *dialog_class = ADW_DIALOG_CLASS (klass);

  dialog_class->closed = gnalk_media_capture_dialog_closed;

  /**
   * GnalkMediaCaptureDialog::photo-captured:
   * @self: the dialog
   * @jpeg_data: JPEG-encoded photo data
   *
   * Emitted when a photo is captured.
   */
  signals[SIGNAL_PHOTO_CAPTURED] = g_signal_new (
      "photo-captured",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_BYTES);

  /**
   * GnalkMediaCaptureDialog::video-captured:
   * @self: the dialog
   * @webm_data: WebM video data
   * @duration: video duration in seconds
   *
   * Emitted when video recording is stopped.
   */
  signals[SIGNAL_VIDEO_CAPTURED] = g_signal_new (
      "video-captured",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL, NULL,
      G_TYPE_NONE, 2, G_TYPE_BYTES, G_TYPE_DOUBLE);
}

static void
gnalk_media_capture_dialog_init (GnalkMediaCaptureDialog *self)
{
  self->mode = CAPTURE_MODE_CAMERA;

  /* No title - we use overlay controls */
  adw_dialog_set_title (ADW_DIALOG (self), "");
  /* Larger default size */
  adw_dialog_set_content_width (ADW_DIALOG (self), 360);
  adw_dialog_set_content_height (ADW_DIALOG (self), 480);
  adw_dialog_set_follows_content_size (ADW_DIALOG (self), TRUE);

  /* Main overlay container - camera fills the space, buttons overlay on top */
  self->overlay = gtk_overlay_new ();
  gtk_widget_add_css_class (self->overlay, "camera-capture-overlay");

  /* Content stack for camera preview and video player (main child of overlay) */
  self->content_stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (self->content_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_widget_set_vexpand (self->content_stack, TRUE);
  gtk_widget_set_hexpand (self->content_stack, TRUE);

  /* Camera preview */
  self->camera_preview = gnalk_camera_preview_new ();
  gtk_widget_set_size_request (GTK_WIDGET (self->camera_preview), 360, 480);
  gtk_stack_add_named (GTK_STACK (self->content_stack),
                       GTK_WIDGET (self->camera_preview), "camera");

  /* Video player for preview */
  self->video_player = gnalk_video_player_new ();
  gtk_stack_add_named (GTK_STACK (self->content_stack),
                       GTK_WIDGET (self->video_player), "player");

  gtk_stack_set_visible_child_name (GTK_STACK (self->content_stack), "camera");
  gtk_overlay_set_child (GTK_OVERLAY (self->overlay), self->content_stack);

  /* Close button (top-left, overlaid on camera) */
  self->close_button = gtk_button_new_from_icon_name ("window-close-symbolic");
  gtk_widget_add_css_class (self->close_button, "circular");
  gtk_widget_add_css_class (self->close_button, "osd");
  gtk_widget_set_halign (self->close_button, GTK_ALIGN_START);
  gtk_widget_set_valign (self->close_button, GTK_ALIGN_START);
  gtk_widget_set_margin_top (self->close_button, 8);
  gtk_widget_set_margin_start (self->close_button, 8);
  gtk_widget_set_tooltip_text (self->close_button, "Close");
  g_signal_connect (self->close_button, "clicked",
                    G_CALLBACK (on_close_clicked), self);
  gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay), self->close_button);

  /* Switch camera button (top-right, overlaid on camera) */
  self->switch_camera_button = gtk_button_new_from_icon_name ("camera-switch-symbolic");
  gtk_widget_add_css_class (self->switch_camera_button, "circular");
  gtk_widget_add_css_class (self->switch_camera_button, "osd");
  gtk_widget_set_halign (self->switch_camera_button, GTK_ALIGN_END);
  gtk_widget_set_valign (self->switch_camera_button, GTK_ALIGN_START);
  gtk_widget_set_margin_top (self->switch_camera_button, 8);
  gtk_widget_set_margin_end (self->switch_camera_button, 8);
  gtk_widget_set_tooltip_text (self->switch_camera_button, "Switch Camera");
  g_signal_connect (self->switch_camera_button, "clicked",
                    G_CALLBACK (on_switch_camera_clicked), self);
  gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay), self->switch_camera_button);

  /* Bottom controls overlay - camera mode */
  self->camera_controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign (self->camera_controls, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->camera_controls, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (self->camera_controls, 16);

  /* Photo button */
  self->photo_button = gtk_button_new_from_icon_name ("camera-photo-symbolic");
  gtk_widget_add_css_class (self->photo_button, "circular");
  gtk_widget_add_css_class (self->photo_button, "osd");
  gtk_widget_set_tooltip_text (self->photo_button, "Take Photo");
  g_signal_connect (self->photo_button, "clicked", G_CALLBACK (on_photo_clicked), self);
  gtk_box_append (GTK_BOX (self->camera_controls), self->photo_button);

  /* Video button - force square aspect ratio via CSS */
  self->video_button = gtk_button_new_from_icon_name ("media-record-symbolic");
  gtk_widget_add_css_class (self->video_button, "circular");
  gtk_widget_add_css_class (self->video_button, "osd");
  gtk_widget_add_css_class (self->video_button, "destructive-action");
  gtk_widget_add_css_class (self->video_button, "capture-record-button");
  gtk_widget_set_tooltip_text (self->video_button, "Record Video");
  g_signal_connect (self->video_button, "clicked", G_CALLBACK (on_video_clicked), self);
  gtk_box_append (GTK_BOX (self->camera_controls), self->video_button);

  gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay), self->camera_controls);

  /* Bottom controls overlay - preview mode (above video player controls) */
  self->preview_controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign (self->preview_controls, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->preview_controls, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (self->preview_controls, 56);  /* Above video player controls */
  gtk_widget_set_visible (self->preview_controls, FALSE);

  /* Re-record button */
  self->rerecord_button = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  gtk_widget_add_css_class (self->rerecord_button, "circular");
  gtk_widget_add_css_class (self->rerecord_button, "osd");
  gtk_widget_set_tooltip_text (self->rerecord_button, "Re-record");
  g_signal_connect (self->rerecord_button, "clicked", G_CALLBACK (on_rerecord_clicked), self);
  gtk_box_append (GTK_BOX (self->preview_controls), self->rerecord_button);

  /* Send button */
  self->send_button = gtk_button_new_from_icon_name ("send-to-symbolic");
  gtk_widget_add_css_class (self->send_button, "circular");
  gtk_widget_add_css_class (self->send_button, "osd");
  gtk_widget_add_css_class (self->send_button, "suggested-action");
  gtk_widget_set_tooltip_text (self->send_button, "Send Video");
  g_signal_connect (self->send_button, "clicked", G_CALLBACK (on_send_clicked), self);
  gtk_box_append (GTK_BOX (self->preview_controls), self->send_button);

  gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay), self->preview_controls);

  adw_dialog_set_child (ADW_DIALOG (self), self->overlay);

  /* Start camera preview automatically */
  gnalk_camera_preview_start (self->camera_preview);
}

GnalkMediaCaptureDialog *
gnalk_media_capture_dialog_new (void)
{
  return g_object_new (GNALK_TYPE_MEDIA_CAPTURE_DIALOG, NULL);
}
