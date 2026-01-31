#include "gnalk-waveform-seekbar.h"

#define DEFAULT_NUM_BARS 50
#define BAR_GAP_RATIO    0.3  /* Gap is 30% of bar width */
#define MIN_BAR_HEIGHT   0.1  /* Minimum bar height as fraction */

struct _GnalkWaveformSeekbar
{
  GtkWidget parent_instance;

  float    *waveform_data;
  gsize     waveform_size;
  double    progress;
  gboolean  is_sensitive;

  /* Gesture controllers */
  GtkGesture *click_gesture;
  GtkGesture *drag_gesture;

  /* Colors from theme */
  GdkRGBA played_color;
  GdkRGBA unplayed_color;
};

G_DEFINE_FINAL_TYPE (GnalkWaveformSeekbar, gnalk_waveform_seekbar, GTK_TYPE_WIDGET)

enum {
  SIGNAL_SEEK,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
update_colors_from_theme (GnalkWaveformSeekbar *self)
{
  GtkWidget *widget = GTK_WIDGET (self);

  /* Get foreground color - this adapts to light/dark theme */
  GdkRGBA fg_color;
  gtk_widget_get_color (widget, &fg_color);

  /* Played portion: use accent blue (matches libadwaita default) */
  self->played_color.red = 0.21f;
  self->played_color.green = 0.52f;
  self->played_color.blue = 0.89f;
  self->played_color.alpha = 1.0f;

  /* Unplayed bars: dimmed foreground color adapts to theme */
  self->unplayed_color = fg_color;
  self->unplayed_color.alpha = 0.3f;
}

static void
gnalk_waveform_seekbar_snapshot (GtkWidget   *widget,
                                  GtkSnapshot *snapshot)
{
  GnalkWaveformSeekbar *self = GNALK_WAVEFORM_SEEKBAR (widget);
  int width = gtk_widget_get_width (widget);
  int height = gtk_widget_get_height (widget);

  if (width <= 0 || height <= 0)
    return;

  update_colors_from_theme (self);

  gsize num_bars = self->waveform_size > 0 ? self->waveform_size : DEFAULT_NUM_BARS;

  /* Calculate bar dimensions */
  double total_bar_space = (double) width / num_bars;
  double bar_width = total_bar_space * (1.0 - BAR_GAP_RATIO);
  double bar_gap = total_bar_space * BAR_GAP_RATIO;
  double max_bar_height = height * 0.8;  /* Leave some margin */
  double center_y = height / 2.0;

  /* Draw each bar */
  for (gsize i = 0; i < num_bars; i++)
    {
      /* Get amplitude for this bar */
      float amplitude = MIN_BAR_HEIGHT;
      if (self->waveform_data && i < self->waveform_size)
        amplitude = MAX (self->waveform_data[i], MIN_BAR_HEIGHT);

      /* Calculate bar position and height */
      double x = i * total_bar_space + bar_gap / 2.0;
      double bar_height = amplitude * max_bar_height;
      double y = center_y - bar_height / 2.0;

      /* Determine color based on progress */
      double bar_progress = (double) (i + 0.5) / num_bars;
      const GdkRGBA *color;
      if (bar_progress <= self->progress)
        color = &self->played_color;
      else
        color = &self->unplayed_color;

      /* Draw rounded rectangle for bar */
      GskRoundedRect rounded_rect;
      graphene_rect_t rect = GRAPHENE_RECT_INIT ((float) x, (float) y,
                                                  (float) bar_width, (float) bar_height);
      float corner_radius = MIN (bar_width / 2.0f, 2.0f);
      gsk_rounded_rect_init_from_rect (&rounded_rect, &rect, corner_radius);

      gtk_snapshot_push_rounded_clip (snapshot, &rounded_rect);
      gtk_snapshot_append_color (snapshot, color, &rect);
      gtk_snapshot_pop (snapshot);
    }
}

static void
gnalk_waveform_seekbar_measure (GtkWidget      *widget G_GNUC_UNUSED,
                                 GtkOrientation  orientation,
                                 int             for_size G_GNUC_UNUSED,
                                 int            *minimum,
                                 int            *natural,
                                 int            *minimum_baseline G_GNUC_UNUSED,
                                 int            *natural_baseline G_GNUC_UNUSED)
{
  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      *minimum = 80;
      *natural = 120;
    }
  else
    {
      *minimum = 24;
      *natural = 32;
    }
}

static double
calculate_progress_from_x (GnalkWaveformSeekbar *self, double x)
{
  int width = gtk_widget_get_width (GTK_WIDGET (self));
  if (width <= 0)
    return 0.0;

  double progress = x / width;
  return CLAMP (progress, 0.0, 1.0);
}

static void
on_click_pressed (GtkGestureClick       *gesture G_GNUC_UNUSED,
                  int                    n_press G_GNUC_UNUSED,
                  double                 x,
                  double                 y G_GNUC_UNUSED,
                  GnalkWaveformSeekbar *self)
{
  if (!self->is_sensitive)
    return;

  double progress = calculate_progress_from_x (self, x);
  self->progress = progress;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_SEEK], 0, progress);
}

static void
on_drag_update (GtkGestureDrag        *gesture,
                double                 offset_x,
                double                 offset_y G_GNUC_UNUSED,
                GnalkWaveformSeekbar *self)
{
  if (!self->is_sensitive)
    return;

  double start_x, start_y;
  gtk_gesture_drag_get_start_point (gesture, &start_x, &start_y);

  double current_x = start_x + offset_x;
  double progress = calculate_progress_from_x (self, current_x);
  self->progress = progress;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_SEEK], 0, progress);
}

static void
gnalk_waveform_seekbar_dispose (GObject *object)
{
  GnalkWaveformSeekbar *self = GNALK_WAVEFORM_SEEKBAR (object);

  g_clear_pointer (&self->waveform_data, g_free);

  /* Note: gesture controllers are owned by the widget after gtk_widget_add_controller,
   * so they are disposed automatically by the widget's dispose - don't clear them here */

  G_OBJECT_CLASS (gnalk_waveform_seekbar_parent_class)->dispose (object);
}

static void
gnalk_waveform_seekbar_class_init (GnalkWaveformSeekbarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnalk_waveform_seekbar_dispose;

  widget_class->snapshot = gnalk_waveform_seekbar_snapshot;
  widget_class->measure = gnalk_waveform_seekbar_measure;

  /* Signal emitted when user seeks */
  signals[SIGNAL_SEEK] =
    g_signal_new ("seek",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_DOUBLE);

  /* CSS name for styling */
  gtk_widget_class_set_css_name (widget_class, "waveform-seekbar");
}

static void
gnalk_waveform_seekbar_init (GnalkWaveformSeekbar *self)
{
  self->waveform_data = NULL;
  self->waveform_size = 0;
  self->progress = 0.0;
  self->is_sensitive = TRUE;

  /* Default colors (will be updated from theme) */
  self->played_color.red = 0.21f;
  self->played_color.green = 0.52f;
  self->played_color.blue = 0.89f;
  self->played_color.alpha = 1.0f;

  self->unplayed_color.red = 0.5f;
  self->unplayed_color.green = 0.5f;
  self->unplayed_color.blue = 0.5f;
  self->unplayed_color.alpha = 0.4f;

  /* Set up click gesture for seeking */
  self->click_gesture = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (self->click_gesture), GDK_BUTTON_PRIMARY);
  g_signal_connect (self->click_gesture, "pressed",
                    G_CALLBACK (on_click_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self),
                             GTK_EVENT_CONTROLLER (self->click_gesture));

  /* Set up drag gesture for scrubbing */
  self->drag_gesture = gtk_gesture_drag_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (self->drag_gesture), GDK_BUTTON_PRIMARY);
  g_signal_connect (self->drag_gesture, "drag-update",
                    G_CALLBACK (on_drag_update), self);
  gtk_widget_add_controller (GTK_WIDGET (self),
                             GTK_EVENT_CONTROLLER (self->drag_gesture));

  /* Make widget expand horizontally */
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_CENTER);
}

GnalkWaveformSeekbar *
gnalk_waveform_seekbar_new (void)
{
  return g_object_new (GNALK_TYPE_WAVEFORM_SEEKBAR, NULL);
}

void
gnalk_waveform_seekbar_set_waveform (GnalkWaveformSeekbar *self,
                                      const float           *data,
                                      gsize                  size)
{
  g_return_if_fail (GNALK_IS_WAVEFORM_SEEKBAR (self));

  g_clear_pointer (&self->waveform_data, g_free);

  if (data && size > 0)
    {
      self->waveform_data = g_memdup2 (data, size * sizeof (float));
      self->waveform_size = size;
    }
  else
    {
      self->waveform_data = NULL;
      self->waveform_size = 0;
    }

  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
gnalk_waveform_seekbar_set_progress (GnalkWaveformSeekbar *self,
                                      double                 progress)
{
  g_return_if_fail (GNALK_IS_WAVEFORM_SEEKBAR (self));

  self->progress = CLAMP (progress, 0.0, 1.0);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

double
gnalk_waveform_seekbar_get_progress (GnalkWaveformSeekbar *self)
{
  g_return_val_if_fail (GNALK_IS_WAVEFORM_SEEKBAR (self), 0.0);
  return self->progress;
}

void
gnalk_waveform_seekbar_set_sensitive (GnalkWaveformSeekbar *self,
                                       gboolean               sensitive)
{
  g_return_if_fail (GNALK_IS_WAVEFORM_SEEKBAR (self));
  self->is_sensitive = sensitive;
}
