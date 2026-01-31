/* gnalk-file-preview.c
 *
 * Widget displaying a server-hosted file preview.
 */

#include "gnalk-file-preview.h"

struct _GnalkFilePreview
{
  GtkWidget parent_instance;

  GtkWidget *overlay;
  GtkWidget *picture;
  GtkWidget *download_spinner;
  GtkWidget *cached_icon;
  GtkWidget *not_cached_overlay;
  GtkWidget *download_icon;

  char *file_id;
  char *file_name;
  char *file_mimetype;
  char *file_link;
  char *file_path;
};

G_DEFINE_FINAL_TYPE (GnalkFilePreview, gnalk_file_preview, GTK_TYPE_WIDGET)

enum {
  SIGNAL_CLICKED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Weak reference wrapper for async preview callback */
typedef struct
{
  GWeakRef weak_ref;
} PreviewCallbackData;

static PreviewCallbackData *
preview_callback_data_new (GnalkFilePreview *self)
{
  PreviewCallbackData *data = g_new0 (PreviewCallbackData, 1);
  g_weak_ref_init (&data->weak_ref, self);
  return data;
}

static void
on_preview_loaded (GnalkAvatarCache *cache G_GNUC_UNUSED,
                   GdkTexture        *texture,
                   gpointer           user_data)
{
  PreviewCallbackData *data = user_data;
  GnalkFilePreview *self = g_weak_ref_get (&data->weak_ref);

  if (self)
    {
      if (texture)
        {
          gtk_picture_set_paintable (GTK_PICTURE (self->picture), GDK_PAINTABLE (texture));
          gtk_widget_set_visible (GTK_WIDGET (self), TRUE);

          /* Hide loading spinner */
          gtk_spinner_stop (GTK_SPINNER (self->download_spinner));
          gtk_widget_set_visible (self->download_spinner, FALSE);
        }
      else
        {
          gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
        }
      g_object_unref (self);
    }

  g_weak_ref_clear (&data->weak_ref);
  g_free (data);
}

static void
on_preview_clicked (GtkGestureClick *gesture,
                    int              n_press G_GNUC_UNUSED,
                    double           x G_GNUC_UNUSED,
                    double           y G_GNUC_UNUSED,
                    gpointer         user_data)
{
  GnalkFilePreview *self = GNALK_FILE_PREVIEW (user_data);

  if (self->file_id)
    {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
      g_signal_emit (self, signals[SIGNAL_CLICKED], 0);
    }
}

static void
gnalk_file_preview_dispose (GObject *object)
{
  GnalkFilePreview *self = GNALK_FILE_PREVIEW (object);

  g_clear_pointer (&self->overlay, gtk_widget_unparent);

  G_OBJECT_CLASS (gnalk_file_preview_parent_class)->dispose (object);
}

static void
gnalk_file_preview_finalize (GObject *object)
{
  GnalkFilePreview *self = GNALK_FILE_PREVIEW (object);

  g_free (self->file_id);
  g_free (self->file_name);
  g_free (self->file_mimetype);
  g_free (self->file_link);
  g_free (self->file_path);

  G_OBJECT_CLASS (gnalk_file_preview_parent_class)->finalize (object);
}

static void
gnalk_file_preview_class_init (GnalkFilePreviewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnalk_file_preview_dispose;
  object_class->finalize = gnalk_file_preview_finalize;

  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);

  /* Signal emitted when user clicks on the preview */
  signals[SIGNAL_CLICKED] =
    g_signal_new ("clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
gnalk_file_preview_init (GnalkFilePreview *self)
{
  /* Create the overlay container */
  self->overlay = gtk_overlay_new ();
  gtk_widget_set_parent (self->overlay, GTK_WIDGET (self));

  /* Create picture */
  self->picture = gtk_picture_new ();
  gtk_picture_set_can_shrink (GTK_PICTURE (self->picture), TRUE);
  gtk_picture_set_content_fit (GTK_PICTURE (self->picture), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_size_request (self->picture, 200, 150);
  gtk_widget_add_css_class (self->picture, "message-file-preview");
  gtk_overlay_set_child (GTK_OVERLAY (self->overlay), self->picture);

  /* Download spinner */
  self->download_spinner = gtk_spinner_new ();
  gtk_widget_set_halign (self->download_spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->download_spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request (self->download_spinner, 32, 32);
  gtk_widget_set_visible (self->download_spinner, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay), self->download_spinner);

  /* Cached icon */
  self->cached_icon = gtk_image_new_from_icon_name ("emblem-ok-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (self->cached_icon), 16);
  gtk_widget_set_halign (self->cached_icon, GTK_ALIGN_END);
  gtk_widget_set_valign (self->cached_icon, GTK_ALIGN_END);
  gtk_widget_set_margin_end (self->cached_icon, 4);
  gtk_widget_set_margin_bottom (self->cached_icon, 4);
  gtk_widget_add_css_class (self->cached_icon, "file-cached-icon");
  gtk_widget_set_visible (self->cached_icon, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay), self->cached_icon);

  /* Not cached overlay (dimmed box) */
  self->not_cached_overlay = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign (self->not_cached_overlay, GTK_ALIGN_FILL);
  gtk_widget_set_valign (self->not_cached_overlay, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand (self->not_cached_overlay, TRUE);
  gtk_widget_set_vexpand (self->not_cached_overlay, TRUE);
  gtk_widget_add_css_class (self->not_cached_overlay, "file-not-cached-overlay");
  gtk_widget_set_visible (self->not_cached_overlay, TRUE);
  gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay), self->not_cached_overlay);

  /* Download icon */
  self->download_icon = gtk_image_new_from_icon_name ("folder-download-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (self->download_icon), 32);
  gtk_widget_set_halign (self->download_icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->download_icon, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (self->download_icon, "file-download-icon");
  gtk_widget_set_visible (self->download_icon, TRUE);
  gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay), self->download_icon);

  /* Add click gesture */
  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
  g_signal_connect (click, "released", G_CALLBACK (on_preview_clicked), self);
  gtk_widget_add_controller (self->overlay, GTK_EVENT_CONTROLLER (click));
}

GnalkFilePreview *
gnalk_file_preview_new (void)
{
  return g_object_new (GNALK_TYPE_FILE_PREVIEW, NULL);
}

void
gnalk_file_preview_set_file (GnalkFilePreview *self,
                              const char        *file_id,
                              const char        *file_name,
                              const char        *file_mimetype,
                              const char        *file_link,
                              const char        *file_path,
                              GnalkAvatarCache *avatar_cache)
{
  g_return_if_fail (GNALK_IS_FILE_PREVIEW (self));

  /* Store file info */
  g_free (self->file_id);
  g_free (self->file_name);
  g_free (self->file_mimetype);
  g_free (self->file_link);
  g_free (self->file_path);

  self->file_id = g_strdup (file_id);
  self->file_name = g_strdup (file_name);
  self->file_mimetype = g_strdup (file_mimetype);
  self->file_link = g_strdup (file_link);
  self->file_path = g_strdup (file_path);

  if (!file_id || !avatar_cache)
    {
      gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
      return;
    }

  /* Show overlay with spinner while loading */
  gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
  gtk_spinner_start (GTK_SPINNER (self->download_spinner));
  gtk_widget_set_visible (self->download_spinner, TRUE);
  gtk_widget_set_visible (self->not_cached_overlay, FALSE);
  gtk_widget_set_visible (self->download_icon, FALSE);

  /* Load preview */
  gnalk_avatar_cache_get_file_preview (avatar_cache, file_id, 400, 300,
                                        on_preview_loaded,
                                        preview_callback_data_new (self));
}

void
gnalk_file_preview_set_downloading (GnalkFilePreview *self,
                                     gboolean           downloading)
{
  g_return_if_fail (GNALK_IS_FILE_PREVIEW (self));

  if (downloading)
    {
      gtk_spinner_start (GTK_SPINNER (self->download_spinner));
      gtk_widget_set_visible (self->download_spinner, TRUE);
      gtk_widget_set_visible (self->not_cached_overlay, FALSE);
      gtk_widget_set_visible (self->download_icon, FALSE);
    }
  else
    {
      gtk_spinner_stop (GTK_SPINNER (self->download_spinner));
      gtk_widget_set_visible (self->download_spinner, FALSE);
    }
}

void
gnalk_file_preview_set_cached (GnalkFilePreview *self,
                                gboolean           cached)
{
  g_return_if_fail (GNALK_IS_FILE_PREVIEW (self));

  gtk_widget_set_visible (self->cached_icon, cached);
  gtk_widget_set_visible (self->not_cached_overlay, !cached);
  gtk_widget_set_visible (self->download_icon, !cached);
}

const char *
gnalk_file_preview_get_file_id (GnalkFilePreview *self)
{
  g_return_val_if_fail (GNALK_IS_FILE_PREVIEW (self), NULL);
  return self->file_id;
}

const char *
gnalk_file_preview_get_file_name (GnalkFilePreview *self)
{
  g_return_val_if_fail (GNALK_IS_FILE_PREVIEW (self), NULL);
  return self->file_name;
}

const char *
gnalk_file_preview_get_file_mimetype (GnalkFilePreview *self)
{
  g_return_val_if_fail (GNALK_IS_FILE_PREVIEW (self), NULL);
  return self->file_mimetype;
}

const char *
gnalk_file_preview_get_file_link (GnalkFilePreview *self)
{
  g_return_val_if_fail (GNALK_IS_FILE_PREVIEW (self), NULL);
  return self->file_link;
}

const char *
gnalk_file_preview_get_file_path (GnalkFilePreview *self)
{
  g_return_val_if_fail (GNALK_IS_FILE_PREVIEW (self), NULL);
  return self->file_path;
}

void
gnalk_file_preview_reset (GnalkFilePreview *self)
{
  g_return_if_fail (GNALK_IS_FILE_PREVIEW (self));

  g_clear_pointer (&self->file_id, g_free);
  g_clear_pointer (&self->file_name, g_free);
  g_clear_pointer (&self->file_mimetype, g_free);
  g_clear_pointer (&self->file_link, g_free);
  g_clear_pointer (&self->file_path, g_free);

  gtk_picture_set_paintable (GTK_PICTURE (self->picture), NULL);
  gtk_spinner_stop (GTK_SPINNER (self->download_spinner));
  gtk_widget_set_visible (self->download_spinner, FALSE);
  gtk_widget_set_visible (self->cached_icon, FALSE);
  gtk_widget_set_visible (self->not_cached_overlay, TRUE);
  gtk_widget_set_visible (self->download_icon, TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
}
