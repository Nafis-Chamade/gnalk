/* gnalk-upload-preview.c
 *
 * Widget displaying a file being uploaded or recently uploaded.
 */

#include "gnalk-upload-preview.h"

#include <gio/gio.h>

struct _GnalkUploadPreview
{
  GtkBox parent_instance;

  /* Image preview widgets */
  GtkWidget *image_overlay;
  GtkWidget *image_preview;
  GtkWidget *upload_spinner;
  GtkWidget *complete_icon;

  /* Non-image file widgets */
  GtkWidget *file_box;
  GtkWidget *file_icon;
  GtkWidget *file_name_label;
  GtkWidget *file_spinner;

  char     *file_path;
  gboolean  is_uploading;
};

G_DEFINE_FINAL_TYPE (GnalkUploadPreview, gnalk_upload_preview, GTK_TYPE_BOX)

enum {
  SIGNAL_CLICKED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
on_preview_clicked (GtkGestureClick *gesture,
                    int              n_press G_GNUC_UNUSED,
                    double           x G_GNUC_UNUSED,
                    double           y G_GNUC_UNUSED,
                    gpointer         user_data)
{
  GnalkUploadPreview *self = GNALK_UPLOAD_PREVIEW (user_data);

  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  g_signal_emit (self, signals[SIGNAL_CLICKED], 0);
}

static void
gnalk_upload_preview_finalize (GObject *object)
{
  GnalkUploadPreview *self = GNALK_UPLOAD_PREVIEW (object);

  g_free (self->file_path);

  G_OBJECT_CLASS (gnalk_upload_preview_parent_class)->finalize (object);
}

static void
gnalk_upload_preview_class_init (GnalkUploadPreviewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = gnalk_upload_preview_finalize;

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/net/skilpa/gnalk/gnalk-upload-preview.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkUploadPreview, image_overlay);
  gtk_widget_class_bind_template_child (widget_class, GnalkUploadPreview, image_preview);
  gtk_widget_class_bind_template_child (widget_class, GnalkUploadPreview, upload_spinner);
  gtk_widget_class_bind_template_child (widget_class, GnalkUploadPreview, complete_icon);
  gtk_widget_class_bind_template_child (widget_class, GnalkUploadPreview, file_box);
  gtk_widget_class_bind_template_child (widget_class, GnalkUploadPreview, file_icon);
  gtk_widget_class_bind_template_child (widget_class, GnalkUploadPreview, file_name_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkUploadPreview, file_spinner);

  /* Signal emitted when user clicks on the preview */
  signals[SIGNAL_CLICKED] =
    g_signal_new ("clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
gnalk_upload_preview_init (GnalkUploadPreview *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  /* Add click gesture to image overlay */
  GtkGesture *image_click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (image_click), GDK_BUTTON_PRIMARY);
  g_signal_connect (image_click, "released", G_CALLBACK (on_preview_clicked), self);
  gtk_widget_add_controller (self->image_overlay, GTK_EVENT_CONTROLLER (image_click));

  /* Add click gesture to file box */
  GtkGesture *file_click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (file_click), GDK_BUTTON_PRIMARY);
  g_signal_connect (file_click, "released", G_CALLBACK (on_preview_clicked), self);
  gtk_widget_add_controller (self->file_box, GTK_EVENT_CONTROLLER (file_click));
}

GnalkUploadPreview *
gnalk_upload_preview_new (void)
{
  return g_object_new (GNALK_TYPE_UPLOAD_PREVIEW, NULL);
}

void
gnalk_upload_preview_set_file (GnalkUploadPreview *self,
                                const char          *file_path,
                                const char          *file_name,
                                const char          *mime_type,
                                gboolean             is_uploading)
{
  g_return_if_fail (GNALK_IS_UPLOAD_PREVIEW (self));

  if (!file_path || !*file_path)
    {
      gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
      return;
    }

  g_free (self->file_path);
  self->file_path = g_strdup (file_path);
  self->is_uploading = is_uploading;

  /* Check if this is an image */
  gboolean is_image = mime_type && g_str_has_prefix (mime_type, "image/");

  if (is_image)
    {
      /* Try to load local image preview */
      g_autoptr(GdkTexture) texture = gdk_texture_new_from_filename (file_path, NULL);
      if (texture)
        {
          gtk_picture_set_paintable (GTK_PICTURE (self->image_preview), GDK_PAINTABLE (texture));
          gtk_widget_set_visible (self->image_overlay, TRUE);
          gtk_widget_set_visible (self->file_box, FALSE);

          /* Show spinner while uploading, checkmark when complete */
          if (is_uploading)
            {
              gtk_spinner_start (GTK_SPINNER (self->upload_spinner));
              gtk_widget_set_visible (self->upload_spinner, TRUE);
              gtk_widget_set_visible (self->complete_icon, FALSE);
            }
          else
            {
              gtk_spinner_stop (GTK_SPINNER (self->upload_spinner));
              gtk_widget_set_visible (self->upload_spinner, FALSE);
              gtk_widget_set_visible (self->complete_icon, TRUE);
            }

          gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
          return;
        }
    }

  /* Non-image or failed to load: show file box */
  gtk_widget_set_visible (self->image_overlay, FALSE);
  gtk_label_set_text (GTK_LABEL (self->file_name_label), file_name ? file_name : "");
  gtk_widget_set_visible (self->file_box, TRUE);

  if (is_uploading)
    {
      gtk_spinner_start (GTK_SPINNER (self->file_spinner));
      gtk_widget_set_visible (self->file_spinner, TRUE);
    }
  else
    {
      gtk_spinner_stop (GTK_SPINNER (self->file_spinner));
      gtk_widget_set_visible (self->file_spinner, FALSE);
    }

  gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
}

void
gnalk_upload_preview_set_complete (GnalkUploadPreview *self)
{
  g_return_if_fail (GNALK_IS_UPLOAD_PREVIEW (self));

  self->is_uploading = FALSE;

  /* Stop spinners and show complete icon */
  gtk_spinner_stop (GTK_SPINNER (self->upload_spinner));
  gtk_widget_set_visible (self->upload_spinner, FALSE);
  gtk_widget_set_visible (self->complete_icon, TRUE);

  gtk_spinner_stop (GTK_SPINNER (self->file_spinner));
  gtk_widget_set_visible (self->file_spinner, FALSE);
}

const char *
gnalk_upload_preview_get_file_path (GnalkUploadPreview *self)
{
  g_return_val_if_fail (GNALK_IS_UPLOAD_PREVIEW (self), NULL);
  return self->file_path;
}

void
gnalk_upload_preview_reset (GnalkUploadPreview *self)
{
  g_return_if_fail (GNALK_IS_UPLOAD_PREVIEW (self));

  self->is_uploading = FALSE;
  g_clear_pointer (&self->file_path, g_free);

  gtk_picture_set_paintable (GTK_PICTURE (self->image_preview), NULL);
  gtk_spinner_stop (GTK_SPINNER (self->upload_spinner));
  gtk_widget_set_visible (self->upload_spinner, FALSE);
  gtk_widget_set_visible (self->complete_icon, FALSE);
  gtk_widget_set_visible (self->image_overlay, FALSE);

  gtk_label_set_text (GTK_LABEL (self->file_name_label), "");
  gtk_spinner_stop (GTK_SPINNER (self->file_spinner));
  gtk_widget_set_visible (self->file_spinner, FALSE);
  gtk_widget_set_visible (self->file_box, FALSE);

  gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
}
