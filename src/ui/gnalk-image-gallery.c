#include "gnalk-image-gallery.h"
#include <adwaita.h>
#include <math.h>

#define SWIPE_THRESHOLD 50.0   /* Minimum swipe distance in pixels */

struct _GnalkImageGallery
{
  AdwDialog parent_instance;

  /* UI widgets */
  GtkWidget *overlay;
  GtkWidget *stack;           /* GtkStack for slide transitions */
  GtkWidget *current_picture; /* Current GtkPicture in stack */
  GtkWidget *next_picture;    /* Next GtkPicture for transitions */
  GtkWidget *spinner;
  GtkWidget *close_button;
  GtkWidget *prev_button;
  GtkWidget *next_button;
  GtkWidget *counter_label;   /* "3 / 10" style counter */
  GtkWidget *header_box;      /* Header with close button and counter */

  /* Gesture controllers */
  GtkGesture *drag_gesture;
  GtkGesture *click_gesture;
  GtkGesture *zoom_gesture;
  GtkEventController *key_controller;

  /* Drag state */
  gboolean drag_is_horizontal;

  /* Zoom state */
  double zoom_target;      /* Target zoom level from gesture */
  double zoom_display;     /* Smoothed zoom level for display */
  double zoom_start_scale; /* Scale at start of pinch gesture */
  GtkCssProvider *zoom_css_provider;
  guint zoom_tick_id;      /* Frame clock tick callback */

  /* Data */
  GnalkHttpClient  *http_client;
  GnalkAvatarCache *avatar_cache;
  GPtrArray         *images;       /* Array of GnalkImageInfo* */
  guint              current_index;

  /* Loading state */
  gboolean is_loading;
  char    *loading_file_id;        /* Track which image is being loaded */
};

G_DEFINE_FINAL_TYPE (GnalkImageGallery, gnalk_image_gallery, ADW_TYPE_DIALOG)

enum {
  SIGNAL_IMAGE_CACHED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void load_current_image (GnalkImageGallery *self);
static void update_navigation_buttons (GnalkImageGallery *self);
static void update_counter_label (GnalkImageGallery *self);

/* ImageInfo implementation */
GnalkImageInfo *
gnalk_image_info_new (const char *file_id,
                        const char *file_name,
                        const char *file_path,
                        const char *file_mimetype,
                        gint64      message_id)
{
  GnalkImageInfo *info = g_new0 (GnalkImageInfo, 1);
  info->file_id = g_strdup (file_id);
  info->file_name = g_strdup (file_name);
  info->file_path = g_strdup (file_path);
  info->file_mimetype = g_strdup (file_mimetype);
  info->message_id = message_id;
  return info;
}

void
gnalk_image_info_free (GnalkImageInfo *info)
{
  if (info)
    {
      g_free (info->file_id);
      g_free (info->file_name);
      g_free (info->file_path);
      g_free (info->file_mimetype);
      g_free (info);
    }
}

static void
on_close_clicked (GtkButton *button G_GNUC_UNUSED,
                  gpointer   user_data)
{
  GnalkImageGallery *self = GNALK_IMAGE_GALLERY (user_data);
  gnalk_image_gallery_close (self);
}

static void
on_prev_clicked (GtkButton *button G_GNUC_UNUSED,
                 gpointer   user_data)
{
  GnalkImageGallery *self = GNALK_IMAGE_GALLERY (user_data);
  gnalk_image_gallery_previous (self);
}

static void
on_next_clicked (GtkButton *button G_GNUC_UNUSED,
                 gpointer   user_data)
{
  GnalkImageGallery *self = GNALK_IMAGE_GALLERY (user_data);
  gnalk_image_gallery_next (self);
}

static void
on_drag_begin (GtkGestureDrag *gesture G_GNUC_UNUSED,
               double          start_x G_GNUC_UNUSED,
               double          start_y G_GNUC_UNUSED,
               gpointer        user_data)
{
  GnalkImageGallery *self = GNALK_IMAGE_GALLERY (user_data);
  self->drag_is_horizontal = FALSE;
}

static void
on_drag_update (GtkGestureDrag *gesture,
                double          offset_x,
                double          offset_y,
                gpointer        user_data)
{
  GnalkImageGallery *self = GNALK_IMAGE_GALLERY (user_data);

  /* Once we've decided direction, stick with it */
  if (self->drag_is_horizontal)
    return;

  /* Need minimum movement to determine direction */
  if (fabs (offset_x) < 10 && fabs (offset_y) < 10)
    return;

  /* Check if this is a horizontal drag */
  if (fabs (offset_x) > fabs (offset_y))
    {
      /* Horizontal - claim it for navigation */
      self->drag_is_horizontal = TRUE;
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    }
  else
    {
      /* Vertical - let AdwDialog handle it for close */
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
    }
}

static void
on_drag_end (GtkGestureDrag *gesture G_GNUC_UNUSED,
             double          offset_x,
             double          offset_y G_GNUC_UNUSED,
             gpointer        user_data)
{
  GnalkImageGallery *self = GNALK_IMAGE_GALLERY (user_data);

  /* Only navigate if we claimed this as a horizontal drag */
  if (!self->drag_is_horizontal)
    return;

  /* Need minimum distance to trigger navigation */
  if (fabs (offset_x) < SWIPE_THRESHOLD)
    return;

  if (offset_x < 0)
    gnalk_image_gallery_next (self);     /* Swipe left = next */
  else
    gnalk_image_gallery_previous (self); /* Swipe right = previous */
}

static void
on_zoom_begin (GtkGestureZoom *gesture G_GNUC_UNUSED,
               GdkEventSequence *sequence G_GNUC_UNUSED,
               gpointer user_data)
{
  GnalkImageGallery *self = GNALK_IMAGE_GALLERY (user_data);
  self->zoom_start_scale = self->zoom_display;
}

static void
apply_zoom_css (GnalkImageGallery *self)
{
  g_autofree char *css = NULL;

  /* Only apply transform when actually zooming to avoid potential rendering issues */
  if (self->zoom_display < 0.99 || self->zoom_display > 1.01)
    {
      css = g_strdup_printf (
        ".gallery-picture { transform-origin: center; transform: scale(%.2f); }",
        self->zoom_display);
    }
  else
    {
      /* No transform at default zoom */
      css = g_strdup (".gallery-picture { }");
    }

  gtk_css_provider_load_from_string (self->zoom_css_provider, css);
}

#define ZOOM_SMOOTHING 0.25  /* Lower = smoother but slower */

static gboolean
on_zoom_tick (GtkWidget     *widget G_GNUC_UNUSED,
              GdkFrameClock *clock G_GNUC_UNUSED,
              gpointer       user_data)
{
  GnalkImageGallery *self = GNALK_IMAGE_GALLERY (user_data);

  /* Exponential smoothing towards target */
  double diff = self->zoom_target - self->zoom_display;

  /* Stop ticking when close enough */
  if (fabs (diff) < 0.005)
    {
      self->zoom_display = self->zoom_target;
      apply_zoom_css (self);
      self->zoom_tick_id = 0;
      return G_SOURCE_REMOVE;
    }

  self->zoom_display += diff * ZOOM_SMOOTHING;
  apply_zoom_css (self);

  return G_SOURCE_CONTINUE;
}

static void
ensure_zoom_tick (GnalkImageGallery *self)
{
  if (self->zoom_tick_id == 0)
    {
      self->zoom_tick_id = gtk_widget_add_tick_callback (
        self->current_picture,
        on_zoom_tick,
        self,
        NULL);
    }
}

static void
on_zoom_scale_changed (GtkGestureZoom *gesture G_GNUC_UNUSED,
                       double          scale,
                       gpointer        user_data)
{
  GnalkImageGallery *self = GNALK_IMAGE_GALLERY (user_data);

  /* Calculate new scale relative to where we started */
  self->zoom_target = CLAMP (self->zoom_start_scale * scale, 0.5, 5.0);

  /* Start smooth animation towards target */
  ensure_zoom_tick (self);
}

static void
reset_zoom (GnalkImageGallery *self)
{
  if (self->zoom_tick_id)
    {
      gtk_widget_remove_tick_callback (self->current_picture, self->zoom_tick_id);
      self->zoom_tick_id = 0;
    }

  self->zoom_target = 1.0;
  self->zoom_display = 1.0;
  self->zoom_start_scale = 1.0;
  apply_zoom_css (self);
}

static void
on_click_released (GtkGestureClick *gesture G_GNUC_UNUSED,
                   int              n_press G_GNUC_UNUSED,
                   double           x,
                   double           y,
                   gpointer         user_data)
{
  GnalkImageGallery *self = GNALK_IMAGE_GALLERY (user_data);

  /* Get overlay width for edge detection */
  int width = gtk_widget_get_width (self->overlay);

  /* Clicking on edges navigates */
  double edge_zone = width * 0.2; /* 20% on each side */

  /* Don't navigate if clicking in header area (top 60px) */
  if (y < 60)
    return;

  if (x < edge_zone)
    gnalk_image_gallery_previous (self);
  else if (x > width - edge_zone)
    gnalk_image_gallery_next (self);
}

static gboolean
on_key_pressed (GtkEventControllerKey *controller G_GNUC_UNUSED,
                guint                  keyval,
                guint                  keycode G_GNUC_UNUSED,
                GdkModifierType        state G_GNUC_UNUSED,
                gpointer               user_data)
{
  GnalkImageGallery *self = GNALK_IMAGE_GALLERY (user_data);

  switch (keyval)
    {
    case GDK_KEY_Escape:
      gnalk_image_gallery_close (self);
      return TRUE;

    case GDK_KEY_Left:
    case GDK_KEY_Up:
      gnalk_image_gallery_previous (self);
      return TRUE;

    case GDK_KEY_Right:
    case GDK_KEY_Down:
    case GDK_KEY_space:
      gnalk_image_gallery_next (self);
      return TRUE;

    default:
      return FALSE;
    }
}

static char *
build_cache_path (const char *file_id, const char *file_name)
{
  g_autofree char *cache_dir = g_build_filename (g_get_user_cache_dir (), "gnalk", NULL);
  g_mkdir_with_parents (cache_dir, 0700);

  const char *name = (file_name && *file_name) ? file_name : "download";
  g_autofree char *safe_name = g_strdup_printf ("%s_%s", file_id ? file_id : "unknown", name);
  return g_build_filename (cache_dir, safe_name, NULL);
}

typedef struct {
  GWeakRef  gallery_ref;  /* Weak ref to detect gallery destruction */
  char     *file_id;
  char     *file_name;
  char     *cache_path;
} ImageLoadData;

static void
image_load_data_free (ImageLoadData *data)
{
  g_weak_ref_clear (&data->gallery_ref);
  g_free (data->file_id);
  g_free (data->file_name);
  g_free (data->cache_path);
  g_free (data);
}

static void
on_image_downloaded (GnalkHttpClient *client G_GNUC_UNUSED,
                     GBytes           *bytes,
                     const char       *content_type G_GNUC_UNUSED,
                     GError           *error,
                     gpointer          user_data)
{
  ImageLoadData *data = user_data;
  g_autoptr(GnalkImageGallery) self = g_weak_ref_get (&data->gallery_ref);

  /* Gallery was destroyed while download was in flight */
  if (!self)
    {
      image_load_data_free (data);
      return;
    }

  /* Check if this is still the image we want */
  if (g_strcmp0 (self->loading_file_id, data->file_id) != 0)
    {
      image_load_data_free (data);
      return;
    }

  self->is_loading = FALSE;
  g_clear_pointer (&self->loading_file_id, g_free);
  gtk_widget_set_visible (self->spinner, FALSE);
  gtk_spinner_stop (GTK_SPINNER (self->spinner));

  if (error || !bytes)
    {
      g_warning ("Failed to download image: %s", error ? error->message : "No data");
      image_load_data_free (data);
      return;
    }

  /* Write to cache */
  g_autoptr(GError) write_error = NULL;
  gsize size;
  gconstpointer file_data = g_bytes_get_data (bytes, &size);

  if (!g_file_set_contents (data->cache_path, file_data, size, &write_error))
    {
      g_warning ("Failed to write cache file: %s", write_error->message);
      image_load_data_free (data);
      return;
    }

  /* Notify that image was cached */
  g_signal_emit (self, signals[SIGNAL_IMAGE_CACHED], 0, data->file_id, data->file_name);

  /* Load and display */
  g_autoptr(GError) load_error = NULL;
  GdkTexture *texture = gdk_texture_new_from_filename (data->cache_path, &load_error);

  if (texture)
    {
      gtk_picture_set_paintable (GTK_PICTURE (self->current_picture), GDK_PAINTABLE (texture));
      g_object_unref (texture);
    }
  else
    {
      g_warning ("Failed to load image texture: %s", load_error->message);
    }

  image_load_data_free (data);
}

static void
load_current_image (GnalkImageGallery *self)
{
  if (!self->images || self->current_index >= self->images->len)
    return;

  GnalkImageInfo *info = g_ptr_array_index (self->images, self->current_index);

  /* Clear previous loading state */
  g_clear_pointer (&self->loading_file_id, g_free);

  /* Build cache path */
  g_autofree char *cache_path = build_cache_path (info->file_id, info->file_name);

  /* Check if cached */
  if (g_file_test (cache_path, G_FILE_TEST_EXISTS))
    {
      g_autoptr(GError) error = NULL;
      GdkTexture *texture = gdk_texture_new_from_filename (cache_path, &error);

      if (texture)
        {
          gtk_picture_set_paintable (GTK_PICTURE (self->current_picture), GDK_PAINTABLE (texture));
          g_object_unref (texture);
          gtk_widget_set_visible (self->spinner, FALSE);
          self->is_loading = FALSE;
          return;
        }
      else
        {
          g_warning ("Failed to load cached image: %s", error->message);
        }
    }

  /* Need to download - show spinner */
  self->is_loading = TRUE;
  self->loading_file_id = g_strdup (info->file_id);
  gtk_widget_set_visible (self->spinner, TRUE);
  gtk_spinner_start (GTK_SPINNER (self->spinner));

  /* Clear current image while loading */
  gtk_picture_set_paintable (GTK_PICTURE (self->current_picture), NULL);

  if (!self->http_client || !info->file_path)
    {
      g_warning ("Cannot download image: no HTTP client or file path");
      return;
    }

  /* Download full image */
  ImageLoadData *data = g_new0 (ImageLoadData, 1);
  g_weak_ref_init (&data->gallery_ref, self);
  data->file_id = g_strdup (info->file_id);
  data->file_name = g_strdup (info->file_name);
  data->cache_path = g_strdup (cache_path);

  gnalk_http_client_download_file (self->http_client, info->file_path,
                                     on_image_downloaded, data);
}

static void
update_navigation_buttons (GnalkImageGallery *self)
{
  gboolean has_prev = self->images && self->current_index > 0;
  gboolean has_next = self->images && self->current_index < self->images->len - 1;

  gtk_widget_set_sensitive (self->prev_button, has_prev);
  gtk_widget_set_sensitive (self->next_button, has_next);

  /* Visual feedback - dim disabled buttons */
  gtk_widget_set_opacity (self->prev_button, has_prev ? 1.0 : 0.3);
  gtk_widget_set_opacity (self->next_button, has_next ? 1.0 : 0.3);
}

static void
update_counter_label (GnalkImageGallery *self)
{
  if (!self->images || self->images->len == 0)
    {
      gtk_label_set_text (GTK_LABEL (self->counter_label), "");
      return;
    }

  g_autofree char *text = g_strdup_printf ("%u / %u",
                                            self->current_index + 1,
                                            self->images->len);
  gtk_label_set_text (GTK_LABEL (self->counter_label), text);
}

static void
navigate_with_transition (GnalkImageGallery *self,
                          gboolean            forward)
{
  /* Reset zoom when switching images */
  reset_zoom (self);

  /* Use stack transition for slide effect */
  if (forward)
    gtk_stack_set_transition_type (GTK_STACK (self->stack),
                                    GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT);
  else
    gtk_stack_set_transition_type (GTK_STACK (self->stack),
                                    GTK_STACK_TRANSITION_TYPE_SLIDE_RIGHT);

  load_current_image (self);
  update_navigation_buttons (self);
  update_counter_label (self);
}

void
gnalk_image_gallery_next (GnalkImageGallery *self)
{
  g_return_if_fail (GNALK_IS_IMAGE_GALLERY (self));

  if (!self->images || self->current_index >= self->images->len - 1)
    return;

  self->current_index++;
  navigate_with_transition (self, TRUE);
}

void
gnalk_image_gallery_previous (GnalkImageGallery *self)
{
  g_return_if_fail (GNALK_IS_IMAGE_GALLERY (self));

  if (!self->images || self->current_index == 0)
    return;

  self->current_index--;
  navigate_with_transition (self, FALSE);
}

void
gnalk_image_gallery_set_images (GnalkImageGallery *self,
                                  GPtrArray          *images,
                                  guint               current_index)
{
  g_return_if_fail (GNALK_IS_IMAGE_GALLERY (self));

  /* Free old images */
  if (self->images)
    g_ptr_array_unref (self->images);

  self->images = images;
  if (images && images->len > 0)
    self->current_index = MIN (current_index, images->len - 1);
  else
    self->current_index = 0;

  /* Reset zoom and transition type */
  reset_zoom (self);
  gtk_stack_set_transition_type (GTK_STACK (self->stack),
                                  GTK_STACK_TRANSITION_TYPE_NONE);

  load_current_image (self);
  update_navigation_buttons (self);
  update_counter_label (self);
}

void
gnalk_image_gallery_set_http_client (GnalkImageGallery *self,
                                       GnalkHttpClient   *http_client)
{
  g_return_if_fail (GNALK_IS_IMAGE_GALLERY (self));
  self->http_client = http_client;
}

void
gnalk_image_gallery_set_avatar_cache (GnalkImageGallery *self,
                                        GnalkAvatarCache  *avatar_cache)
{
  g_return_if_fail (GNALK_IS_IMAGE_GALLERY (self));
  self->avatar_cache = avatar_cache;
}

void
gnalk_image_gallery_present (GnalkImageGallery *self,
                               GtkWidget          *parent)
{
  g_return_if_fail (GNALK_IS_IMAGE_GALLERY (self));
  g_return_if_fail (GTK_IS_WIDGET (parent));

  adw_dialog_present (ADW_DIALOG (self), parent);
}

void
gnalk_image_gallery_close (GnalkImageGallery *self)
{
  g_return_if_fail (GNALK_IS_IMAGE_GALLERY (self));

  /* Stop any ongoing download */
  g_clear_pointer (&self->loading_file_id, g_free);
  self->is_loading = FALSE;
  gtk_spinner_stop (GTK_SPINNER (self->spinner));

  adw_dialog_close (ADW_DIALOG (self));
}

static void
gnalk_image_gallery_dispose (GObject *object)
{
  GnalkImageGallery *self = GNALK_IMAGE_GALLERY (object);

  /* Remove gesture controllers before destruction to prevent stale callbacks */
  if (self->drag_gesture)
    {
      g_signal_handlers_disconnect_by_data (self->drag_gesture, self);
      gtk_widget_remove_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (self->drag_gesture));
      self->drag_gesture = NULL;
    }

  if (self->click_gesture)
    {
      g_signal_handlers_disconnect_by_data (self->click_gesture, self);
      if (self->overlay)
        gtk_widget_remove_controller (self->overlay, GTK_EVENT_CONTROLLER (self->click_gesture));
      self->click_gesture = NULL;
    }

  if (self->zoom_tick_id && self->current_picture)
    {
      gtk_widget_remove_tick_callback (self->current_picture, self->zoom_tick_id);
      self->zoom_tick_id = 0;
    }

  if (self->zoom_gesture)
    {
      g_signal_handlers_disconnect_by_data (self->zoom_gesture, self);
      if (self->current_picture)
        gtk_widget_remove_controller (self->current_picture, GTK_EVENT_CONTROLLER (self->zoom_gesture));
      self->zoom_gesture = NULL;
    }

  if (self->zoom_css_provider)
    {
      gtk_style_context_remove_provider_for_display (
        gtk_widget_get_display (GTK_WIDGET (self)),
        GTK_STYLE_PROVIDER (self->zoom_css_provider));
      g_clear_object (&self->zoom_css_provider);
    }

  if (self->key_controller)
    {
      g_signal_handlers_disconnect_by_data (self->key_controller, self);
      gtk_widget_remove_controller (GTK_WIDGET (self), self->key_controller);
      self->key_controller = NULL;
    }

  g_clear_pointer (&self->loading_file_id, g_free);

  if (self->images)
    {
      g_ptr_array_unref (self->images);
      self->images = NULL;
    }

  G_OBJECT_CLASS (gnalk_image_gallery_parent_class)->dispose (object);
}

static void
gnalk_image_gallery_class_init (GnalkImageGalleryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnalk_image_gallery_dispose;

  /* Signal emitted when an image is downloaded and cached */
  signals[SIGNAL_IMAGE_CACHED] =
    g_signal_new ("image-cached",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

  /* Load UI template */
  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-image-gallery.ui");

  /* Bind template children */
  gtk_widget_class_bind_template_child (widget_class, GnalkImageGallery, overlay);
  gtk_widget_class_bind_template_child (widget_class, GnalkImageGallery, stack);
  gtk_widget_class_bind_template_child (widget_class, GnalkImageGallery, current_picture);
  gtk_widget_class_bind_template_child (widget_class, GnalkImageGallery, spinner);
  gtk_widget_class_bind_template_child (widget_class, GnalkImageGallery, close_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkImageGallery, prev_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkImageGallery, next_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkImageGallery, counter_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkImageGallery, header_box);
}

static void
gnalk_image_gallery_init (GnalkImageGallery *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  /* Connect button signals */
  g_signal_connect (self->close_button, "clicked", G_CALLBACK (on_close_clicked), self);
  g_signal_connect (self->prev_button, "clicked", G_CALLBACK (on_prev_clicked), self);
  g_signal_connect (self->next_button, "clicked", G_CALLBACK (on_next_clicked), self);

  /* Add drag gesture for horizontal swipe navigation.
   * We use drag instead of swipe to decide direction early and let
   * AdwDialog handle vertical swipes for close. */
  self->drag_gesture = gtk_gesture_drag_new ();
  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (self->drag_gesture), FALSE);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (self->drag_gesture),
                                               GTK_PHASE_CAPTURE);
  g_signal_connect (self->drag_gesture, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (self->drag_gesture, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (self->drag_gesture, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (self->drag_gesture));

  /* Add click gesture for edge navigation (BUBBLE phase, after swipe) */
  self->click_gesture = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (self->click_gesture), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (self->click_gesture),
                                               GTK_PHASE_BUBBLE);
  g_signal_connect (self->click_gesture, "released", G_CALLBACK (on_click_released), self);
  gtk_widget_add_controller (self->overlay, GTK_EVENT_CONTROLLER (self->click_gesture));

  /* Set up zoom CSS provider */
  self->zoom_target = 1.0;
  self->zoom_display = 1.0;
  self->zoom_start_scale = 1.0;
  self->zoom_tick_id = 0;
  self->zoom_css_provider = gtk_css_provider_new ();
  gtk_style_context_add_provider_for_display (
    gtk_widget_get_display (GTK_WIDGET (self)),
    GTK_STYLE_PROVIDER (self->zoom_css_provider),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  gtk_widget_add_css_class (self->current_picture, "gallery-picture");
  apply_zoom_css (self);

  /* Add pinch-to-zoom gesture on the picture */
  self->zoom_gesture = gtk_gesture_zoom_new ();
  g_signal_connect (self->zoom_gesture, "begin", G_CALLBACK (on_zoom_begin), self);
  g_signal_connect (self->zoom_gesture, "scale-changed", G_CALLBACK (on_zoom_scale_changed), self);
  gtk_widget_add_controller (self->current_picture, GTK_EVENT_CONTROLLER (self->zoom_gesture));

  /* Add keyboard controller to the dialog */
  self->key_controller = gtk_event_controller_key_new ();
  g_signal_connect (self->key_controller, "key-pressed", G_CALLBACK (on_key_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), self->key_controller);

  /* Set up stack transition duration */
  gtk_stack_set_transition_duration (GTK_STACK (self->stack), 200);

  /* Initialize UI state */
  update_navigation_buttons (self);
}

GnalkImageGallery *
gnalk_image_gallery_new (void)
{
  return g_object_new (GNALK_TYPE_IMAGE_GALLERY, NULL);
}
