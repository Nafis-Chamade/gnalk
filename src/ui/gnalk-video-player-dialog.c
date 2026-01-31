/* gnalk-video-player-dialog.c
 *
 * Dialog wrapper for playing received video files.
 */

#include "gnalk-video-player-dialog.h"
#include "gnalk-video-player.h"

struct _GnalkVideoPlayerDialog
{
  AdwDialog parent_instance;

  GtkWidget *header_bar;
  GnalkVideoPlayer *video_player;
};

G_DEFINE_FINAL_TYPE (GnalkVideoPlayerDialog, gnalk_video_player_dialog, ADW_TYPE_DIALOG)

static void
gnalk_video_player_dialog_closed (AdwDialog *dialog)
{
  GnalkVideoPlayerDialog *self = GNALK_VIDEO_PLAYER_DIALOG (dialog);

  /* Stop playback when dialog closes */
  gnalk_video_player_stop (self->video_player);

  /* Chain up */
  AdwDialogClass *parent_class = ADW_DIALOG_CLASS (gnalk_video_player_dialog_parent_class);
  if (parent_class->closed)
    parent_class->closed (dialog);
}

static void
gnalk_video_player_dialog_class_init (GnalkVideoPlayerDialogClass *klass)
{
  AdwDialogClass *dialog_class = ADW_DIALOG_CLASS (klass);

  dialog_class->closed = gnalk_video_player_dialog_closed;
}

static void
gnalk_video_player_dialog_init (GnalkVideoPlayerDialog *self)
{
  adw_dialog_set_content_width (ADW_DIALOG (self), 640);
  adw_dialog_set_content_height (ADW_DIALOG (self), 520);

  /* Main content */
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  /* Header bar */
  self->header_bar = adw_header_bar_new ();
  adw_header_bar_set_show_title (ADW_HEADER_BAR (self->header_bar), TRUE);
  gtk_box_append (GTK_BOX (content), self->header_bar);

  /* Video player */
  self->video_player = gnalk_video_player_new ();
  gtk_widget_set_vexpand (GTK_WIDGET (self->video_player), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->video_player), TRUE);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->video_player));

  adw_dialog_set_child (ADW_DIALOG (self), content);
}

GnalkVideoPlayerDialog *
gnalk_video_player_dialog_new (void)
{
  return g_object_new (GNALK_TYPE_VIDEO_PLAYER_DIALOG, NULL);
}

void
gnalk_video_player_dialog_set_file (GnalkVideoPlayerDialog *self,
                                     const char              *file_path,
                                     const char              *title)
{
  g_return_if_fail (GNALK_IS_VIDEO_PLAYER_DIALOG (self));
  g_return_if_fail (file_path != NULL);

  /* Set title */
  if (title && *title)
    adw_dialog_set_title (ADW_DIALOG (self), title);
  else
    adw_dialog_set_title (ADW_DIALOG (self), "Video");

  /* Load and auto-play */
  gnalk_video_player_set_file (self->video_player, file_path);
  gnalk_video_player_play (self->video_player);
}
