#include "gnalk-message-view.h"
#include "gnalk-background-generator.h"
#include "gnalk-log.h"
#include <gio/gio.h>
#include <glib/gi18n.h>

struct _GnalkMessageView
{
  GtkBox parent_instance;

  GtkWidget *stack;
  GtkWidget *scrolled_window;
  GtkWidget *list_view;
  GtkWidget *scroll_to_bottom_button;

  gboolean loading_older;
  gboolean in_context_mode;
  gboolean in_thread_mode;

  /* Thread mode banner (created once in init) */
  GtkWidget *thread_banner;
  GtkWidget *thread_banner_title;
  GtkWidget *thread_banner_description;

  GSettings *settings;
  gulong bg_style_handler_id;

  /* Mosaic background */
  GtkCssProvider *mosaic_provider;
};

G_DEFINE_FINAL_TYPE (GnalkMessageView, gnalk_message_view, GTK_TYPE_BOX)

enum
{
  SIGNAL_LOAD_OLDER_REQUESTED,
  SIGNAL_SCROLL_CHANGED,
  SIGNAL_SCROLL_TO_BOTTOM_CLICKED,
  SIGNAL_JUMP_TO_PRESENT_CLICKED,
  SIGNAL_EXIT_THREAD_CLICKED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static gboolean
scroll_to_bottom_idle (gpointer user_data)
{
  GtkScrolledWindow *scroll = GTK_SCROLLED_WINDOW (user_data);
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (scroll);
  gdouble upper = gtk_adjustment_get_upper (adj);
  gdouble page_size = gtk_adjustment_get_page_size (adj);
  gdouble target = upper - page_size;

  g_message ("scroll_to_bottom_idle: upper=%.1f page_size=%.1f target=%.1f",
             upper, page_size, target);

  gtk_adjustment_set_value (adj, target);
  return G_SOURCE_REMOVE;
}

static gboolean
is_scrolled_to_bottom (GtkScrolledWindow *scroll)
{
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (scroll);
  gdouble value = gtk_adjustment_get_value (adj);
  gdouble upper = gtk_adjustment_get_upper (adj);
  gdouble page_size = gtk_adjustment_get_page_size (adj);

  /* Consider "at bottom" if within 50px of the bottom */
  return (upper - page_size - value) < 50.0;
}

static void
on_scroll_adjustment_value_changed (GtkAdjustment     *adj,
                                     GnalkMessageView *self)
{
  gdouble value = gtk_adjustment_get_value (adj);
  gdouble upper = gtk_adjustment_get_upper (adj);
  gdouble page_size = gtk_adjustment_get_page_size (adj);

  g_message ("SCROLL value-changed: value=%.1f upper=%.1f page_size=%.1f (bottom=%.1f)",
             value, upper, page_size, upper - page_size);

  gboolean at_bottom = is_scrolled_to_bottom (GTK_SCROLLED_WINDOW (self->scrolled_window));
  if (!self->in_context_mode)
    gtk_widget_set_visible (self->scroll_to_bottom_button, !at_bottom);

  g_signal_emit (self, signals[SIGNAL_SCROLL_CHANGED], 0);
}

static void
on_scroll_adjustment_changed (GtkAdjustment     *adj,
                               GnalkMessageView *self)
{
  gdouble value = gtk_adjustment_get_value (adj);
  gdouble upper = gtk_adjustment_get_upper (adj);
  gdouble page_size = gtk_adjustment_get_page_size (adj);

  g_message ("SCROLL changed: value=%.1f upper=%.1f page_size=%.1f (bottom=%.1f)",
             value, upper, page_size, upper - page_size);

  gboolean at_bottom = is_scrolled_to_bottom (GTK_SCROLLED_WINDOW (self->scrolled_window));
  if (!self->in_context_mode)
    gtk_widget_set_visible (self->scroll_to_bottom_button, !at_bottom);

  g_signal_emit (self, signals[SIGNAL_SCROLL_CHANGED], 0);
}

static void
on_exit_thread_clicked (GtkButton         *button G_GNUC_UNUSED,
                        GnalkMessageView *self)
{
  g_signal_emit (self, signals[SIGNAL_EXIT_THREAD_CLICKED], 0);
}

static void
on_scroll_to_bottom_clicked (GtkButton         *button,
                              GnalkMessageView *self)
{
  (void) button;

  if (self->in_context_mode)
    {
      g_signal_emit (self, signals[SIGNAL_JUMP_TO_PRESENT_CLICKED], 0);
    }
  else
    {
      g_signal_emit (self, signals[SIGNAL_SCROLL_TO_BOTTOM_CLICKED], 0);
      gnalk_message_view_scroll_to_bottom (self);
    }
}

static void
on_edge_reached (GtkScrolledWindow *scrolled_window,
                 GtkPositionType    pos,
                 GnalkMessageView *self)
{
  (void) scrolled_window;

  if (pos == GTK_POS_TOP)
    {
      g_message ("edge-reached TOP: loading_older=%d", self->loading_older);
      if (!self->loading_older)
        g_signal_emit (self, signals[SIGNAL_LOAD_OLDER_REQUESTED], 0);
    }
}

static const char *bg_style_classes[] = {
  NULL,           /* 0: Flat */
  "bg-grid",      /* 1 */
  "bg-hexagons",  /* 2 */
  "bg-isometric", /* 3 */
  "bg-rhombus",   /* 4 */
  "bg-zigzag",    /* 5 */
  "bg-houndstooth", /* 6 */
  "bg-herringbone", /* 7 */
  "bg-stars",     /* 8 */
  "bg-triangles", /* 9 */
  "bg-waves",     /* 10 */
  "bg-scales",    /* 11 */
  "bg-cross",     /* 12 */
  "bg-mosaic",    /* 13 */
};

static void
update_background_style (GnalkMessageView *self)
{
  gint style = g_settings_get_int (self->settings, "conversation-background");

  /* Remove all background style classes */
  for (gsize i = 1; i < G_N_ELEMENTS (bg_style_classes); i++)
    gtk_widget_remove_css_class (self->list_view, bg_style_classes[i]);

  /* Add the selected style class */
  if (style > 0 && style < (gint) G_N_ELEMENTS (bg_style_classes))
    gtk_widget_add_css_class (self->list_view, bg_style_classes[style]);
}

static void
on_background_style_setting_changed (GSettings        *settings G_GNUC_UNUSED,
                                      const gchar      *key G_GNUC_UNUSED,
                                      GnalkMessageView *self)
{
  update_background_style (self);
}

static void
gnalk_message_view_dispose (GObject *object)
{
  GnalkMessageView *self = GNALK_MESSAGE_VIEW (object);

  if (self->settings && self->bg_style_handler_id)
    {
      g_signal_handler_disconnect (self->settings, self->bg_style_handler_id);
      self->bg_style_handler_id = 0;
    }

  if (self->mosaic_provider)
    {
      gtk_style_context_remove_provider_for_display (
        gtk_widget_get_display (GTK_WIDGET (self)),
        GTK_STYLE_PROVIDER (self->mosaic_provider));
      g_clear_object (&self->mosaic_provider);
    }

  g_clear_object (&self->settings);

  G_OBJECT_CLASS (gnalk_message_view_parent_class)->dispose (object);
}

static void
gnalk_message_view_class_init (GnalkMessageViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnalk_message_view_dispose;

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-message-view.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageView, stack);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageView, scrolled_window);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageView, list_view);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageView, scroll_to_bottom_button);

  signals[SIGNAL_LOAD_OLDER_REQUESTED] =
    g_signal_new ("load-older-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_SCROLL_CHANGED] =
    g_signal_new ("scroll-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_SCROLL_TO_BOTTOM_CLICKED] =
    g_signal_new ("scroll-to-bottom-clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_JUMP_TO_PRESENT_CLICKED] =
    g_signal_new ("jump-to-present-clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_EXIT_THREAD_CLICKED] =
    g_signal_new ("exit-thread-clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);
}

static void
gnalk_message_view_init (GnalkMessageView *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  /* Connect scroll button */
  g_signal_connect (self->scroll_to_bottom_button, "clicked",
                    G_CALLBACK (on_scroll_to_bottom_clicked), self);

  /* Connect scroll adjustment signals */
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled_window));
  g_signal_connect (adj, "value-changed", G_CALLBACK (on_scroll_adjustment_value_changed), self);
  g_signal_connect (adj, "changed", G_CALLBACK (on_scroll_adjustment_changed), self);

  /* Connect edge-reached for lazy loading */
  g_signal_connect (self->scrolled_window, "edge-reached",
                    G_CALLBACK (on_edge_reached), self);

  /* Set up background style from settings */
  self->settings = g_settings_new ("net.skilpa.gnalk");
  self->bg_style_handler_id = g_signal_connect (self->settings, "changed::conversation-background",
                                                 G_CALLBACK (on_background_style_setting_changed), self);
  update_background_style (self);

  /* Create thread banner (hidden by default) */
  {
    self->thread_banner = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class (self->thread_banner, "thread-banner");
    gtk_widget_set_visible (self->thread_banner, FALSE);

    GtkWidget *back_btn = gtk_button_new_from_icon_name ("go-previous-symbolic");
    gtk_button_set_has_frame (GTK_BUTTON (back_btn), FALSE);
    gtk_widget_set_valign (back_btn, GTK_ALIGN_CENTER);
    g_signal_connect (back_btn, "clicked", G_CALLBACK (on_exit_thread_clicked), self);

    /* Vertical box for title + description */
    GtkWidget *info_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand (info_box, TRUE);

    self->thread_banner_title = gtk_label_new (_("Thread"));
    gtk_widget_add_css_class (self->thread_banner_title, "heading");
    gtk_widget_set_halign (self->thread_banner_title, GTK_ALIGN_START);
    gtk_label_set_ellipsize (GTK_LABEL (self->thread_banner_title), PANGO_ELLIPSIZE_END);

    self->thread_banner_description = gtk_label_new (NULL);
    gtk_widget_add_css_class (self->thread_banner_description, "dim-label");
    gtk_widget_add_css_class (self->thread_banner_description, "thread-banner-description");
    gtk_widget_set_halign (self->thread_banner_description, GTK_ALIGN_START);
    gtk_label_set_ellipsize (GTK_LABEL (self->thread_banner_description), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (self->thread_banner_description), 60);
    gtk_widget_set_visible (self->thread_banner_description, FALSE);

    gtk_box_append (GTK_BOX (info_box), self->thread_banner_title);
    gtk_box_append (GTK_BOX (info_box), self->thread_banner_description);

    gtk_box_append (GTK_BOX (self->thread_banner), back_btn);
    gtk_box_append (GTK_BOX (self->thread_banner), info_box);

    /* Insert banner before the stack */
    gtk_box_prepend (GTK_BOX (self), self->thread_banner);
  }
}

GnalkMessageView *
gnalk_message_view_new (void)
{
  return g_object_new (GNALK_TYPE_MESSAGE_VIEW, NULL);
}

void
gnalk_message_view_set_model (GnalkMessageView  *self,
                                GtkSelectionModel  *model,
                                GtkListItemFactory *factory)
{
  g_return_if_fail (GNALK_IS_MESSAGE_VIEW (self));

  gtk_list_view_set_model (GTK_LIST_VIEW (self->list_view), model);
  if (factory)
    gtk_list_view_set_factory (GTK_LIST_VIEW (self->list_view), factory);
}

void
gnalk_message_view_show_empty (GnalkMessageView *self)
{
  g_return_if_fail (GNALK_IS_MESSAGE_VIEW (self));

  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "empty");
}

void
gnalk_message_view_show_messages (GnalkMessageView *self)
{
  g_return_if_fail (GNALK_IS_MESSAGE_VIEW (self));

  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "messages");
}

/* Callback to restore opacity after scroll is processed */
static gboolean
restore_opacity_idle (gpointer user_data)
{
  GtkWidget *widget = GTK_WIDGET (user_data);
  gtk_widget_set_opacity (widget, 1.0);
  GNALK_TRACE_UI ("restore_opacity_idle: restored opacity");
  return G_SOURCE_REMOVE;
}

void
gnalk_message_view_show_messages_at_bottom (GnalkMessageView *self)
{
  g_return_if_fail (GNALK_IS_MESSAGE_VIEW (self));

  GNALK_TRACE_UI ("show_messages_at_bottom: hiding view, showing, scrolling, then revealing");

  /* Hide the scrolled window to prevent visual jump */
  gtk_widget_set_opacity (self->scrolled_window, 0.0);

  /* Show the messages page */
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "messages");

  /* Scroll to bottom - schedule for after layout */
  g_idle_add (scroll_to_bottom_idle, self->scrolled_window);

  /* Restore opacity after GTK has processed the layout and scroll */
  g_idle_add (restore_opacity_idle, self->scrolled_window);
}

void
gnalk_message_view_show_messages_at_item (GnalkMessageView *self,
                                            guint              position)
{
  g_return_if_fail (GNALK_IS_MESSAGE_VIEW (self));

  GNALK_TRACE_UI ("show_messages_at_item: hiding view, showing at position %u, then revealing", position);

  /* Hide the scrolled window to prevent visual jump */
  gtk_widget_set_opacity (self->scrolled_window, 0.0);

  /* Show the messages page */
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "messages");

  /* Scroll to the specified item */
  gtk_list_view_scroll_to (GTK_LIST_VIEW (self->list_view), position, GTK_LIST_SCROLL_NONE, NULL);

  /* Restore opacity after GTK has processed the layout and scroll */
  g_idle_add (restore_opacity_idle, self->scrolled_window);
}

void
gnalk_message_view_scroll_to_bottom (GnalkMessageView *self)
{
  g_return_if_fail (GNALK_IS_MESSAGE_VIEW (self));

  GNALK_TRACE_SCROLL ("message_view_scroll_to_bottom: scheduling scroll");
  g_idle_add (scroll_to_bottom_idle, self->scrolled_window);
}

void
gnalk_message_view_scroll_to_item (GnalkMessageView *self,
                                     guint              position,
                                     GtkListScrollFlags flags)
{
  g_return_if_fail (GNALK_IS_MESSAGE_VIEW (self));

  GNALK_TRACE_SCROLL ("message_view_scroll_to_item: scrolling to position %u, flags=%d", position, flags);
  gtk_list_view_scroll_to (GTK_LIST_VIEW (self->list_view), position, flags, NULL);
}

GtkListView *
gnalk_message_view_get_list_view (GnalkMessageView *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_VIEW (self), NULL);

  return GTK_LIST_VIEW (self->list_view);
}

GtkScrolledWindow *
gnalk_message_view_get_scrolled_window (GnalkMessageView *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_VIEW (self), NULL);

  return GTK_SCROLLED_WINDOW (self->scrolled_window);
}

void
gnalk_message_view_set_loading_older (GnalkMessageView *self,
                                        gboolean           loading)
{
  g_return_if_fail (GNALK_IS_MESSAGE_VIEW (self));

  self->loading_older = loading;
}

gboolean
gnalk_message_view_get_loading_older (GnalkMessageView *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_VIEW (self), FALSE);

  return self->loading_older;
}

gboolean
gnalk_message_view_is_at_bottom (GnalkMessageView *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_VIEW (self), TRUE);

  return is_scrolled_to_bottom (GTK_SCROLLED_WINDOW (self->scrolled_window));
}

gboolean
gnalk_message_view_is_item_visible (GnalkMessageView *self,
                                      guint              position)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_VIEW (self), FALSE);

  GtkScrolledWindow *scroll = GTK_SCROLLED_WINDOW (self->scrolled_window);
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (scroll);
  gdouble scroll_top = gtk_adjustment_get_value (adj);
  gdouble page_size = gtk_adjustment_get_page_size (adj);
  gdouble scroll_bottom = scroll_top + page_size;

  /* Get the widget at this position using GtkListView's internal mechanisms.
   * We need to check if the row for this position is within the visible range.
   * Since GtkListView uses a factory pattern, we compute approximate position
   * based on scroll position and estimated row height. */
  GtkListView *list_view = GTK_LIST_VIEW (self->list_view);
  GtkSelectionModel *model = gtk_list_view_get_model (list_view);

  if (!model)
    return FALSE;

  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (model));
  if (position >= n_items)
    return FALSE;

  /* Use scroll_to to check position - if the item is visible, we're scrolled
   * in a position where it would be rendered.
   * Alternative: use gtk_widget_compute_bounds on the child if available.
   * For now, use a heuristic based on scroll position. */

  /* Estimate: average row height is about 60px. This is a reasonable approximation. */
  gdouble estimated_row_height = 60.0;
  gdouble item_top = position * estimated_row_height;
  gdouble item_bottom = item_top + estimated_row_height;

  /* Item is visible if it overlaps with the viewport */
  return (item_bottom > scroll_top) && (item_top < scroll_bottom);
}

void
gnalk_message_view_get_viewport_size (GnalkMessageView *self,
                                        int               *width,
                                        int               *height)
{
  g_return_if_fail (GNALK_IS_MESSAGE_VIEW (self));

  int w = gtk_widget_get_width (self->scrolled_window);
  int h = gtk_widget_get_height (self->scrolled_window);

  if (width)
    *width = w;
  if (height)
    *height = h;

  GNALK_TRACE_SCROLL ("message_view_get_viewport_size: %dx%d", w, h);
}

gdouble
gnalk_message_view_get_scroll_position (GnalkMessageView *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_VIEW (self), 0.0);

  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled_window));
  gdouble pos = gtk_adjustment_get_value (adj);
  GNALK_TRACE_SCROLL ("message_view_get_scroll_position: %.1f", pos);
  return pos;
}

void
gnalk_message_view_set_scroll_position (GnalkMessageView *self,
                                          gdouble            position)
{
  g_return_if_fail (GNALK_IS_MESSAGE_VIEW (self));

  GNALK_TRACE_SCROLL ("message_view_set_scroll_position: %.1f", position);
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled_window));
  gtk_adjustment_set_value (adj, position);
}

guint
gnalk_message_view_get_first_visible_position (GnalkMessageView *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_VIEW (self), 0);

  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled_window));
  gdouble scroll_top = gtk_adjustment_get_value (adj);

  GtkSelectionModel *model = gtk_list_view_get_model (GTK_LIST_VIEW (self->list_view));
  if (!model)
    return 0;

  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (model));
  if (n_items == 0)
    return 0;

  /* Estimate position based on scroll offset and average row height.
   * This is an approximation since row heights vary. */
  gdouble estimated_row_height = 60.0;
  guint estimated_pos = (guint) (scroll_top / estimated_row_height);

  /* Clamp to valid range */
  if (estimated_pos >= n_items)
    estimated_pos = n_items - 1;

  return estimated_pos;
}

void
gnalk_message_view_set_mosaic_avatars (GnalkMessageView *self,
                                        GPtrArray        *textures,
                                        GdkTexture       *room_avatar)
{
  g_return_if_fail (GNALK_IS_MESSAGE_VIEW (self));

  /* Remove old mosaic provider if any */
  if (self->mosaic_provider)
    {
      gtk_style_context_remove_provider_for_display (
        gtk_widget_get_display (GTK_WIDGET (self)),
        GTK_STYLE_PROVIDER (self->mosaic_provider));
      g_clear_object (&self->mosaic_provider);
    }

  if (!textures || textures->len == 0)
    return;

  /* Build combined textures array with room avatar if present */
  GPtrArray *all_textures = g_ptr_array_new_with_free_func (g_object_unref);

  if (room_avatar)
    g_ptr_array_add (all_textures, g_object_ref (room_avatar));

  for (guint i = 0; i < textures->len; i++)
    g_ptr_array_add (all_textures, g_object_ref (g_ptr_array_index (textures, i)));

  /* Generate mosaic with bin-packed avatars of varying sizes (no tiling) */
  GdkTexture *mosaic = gnalk_background_generator_create_avatar_mosaic (
    all_textures, 1920, 1080, 0.17);

  g_ptr_array_unref (all_textures);

  if (!mosaic)
    return;

  /* Create and apply CSS provider */
  self->mosaic_provider = gnalk_background_generator_create_css_provider (mosaic);
  g_object_unref (mosaic);

  if (self->mosaic_provider)
    {
      gtk_style_context_add_provider_for_display (
        gtk_widget_get_display (GTK_WIDGET (self)),
        GTK_STYLE_PROVIDER (self->mosaic_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
}

void
gnalk_message_view_set_context_mode (GnalkMessageView *self,
                                     gboolean          context_mode)
{
  g_return_if_fail (GNALK_IS_MESSAGE_VIEW (self));

  self->in_context_mode = context_mode;

  if (context_mode)
    {
      /* Force button visible — same look, click returns to present */
      gtk_widget_set_visible (self->scroll_to_bottom_button, TRUE);
    }
  else
    {
      /* Let scroll position determine visibility again */
      gboolean at_bottom = is_scrolled_to_bottom (GTK_SCROLLED_WINDOW (self->scrolled_window));
      gtk_widget_set_visible (self->scroll_to_bottom_button, !at_bottom);
    }
}

void
gnalk_message_view_set_thread_mode (GnalkMessageView *self,
                                    gboolean          thread_mode,
                                    const char       *title,
                                    const char       *description)
{
  g_return_if_fail (GNALK_IS_MESSAGE_VIEW (self));

  self->in_thread_mode = thread_mode;
  gtk_widget_set_visible (self->thread_banner, thread_mode);

  if (thread_mode)
    {
      /* Set title: use provided title or fallback to "Thread" */
      if (title && *title)
        gtk_label_set_text (GTK_LABEL (self->thread_banner_title), title);
      else
        gtk_label_set_text (GTK_LABEL (self->thread_banner_title), _("Thread"));

      /* Set description if available */
      if (description && *description)
        {
          gtk_label_set_text (GTK_LABEL (self->thread_banner_description), description);
          gtk_widget_set_visible (self->thread_banner_description, TRUE);
        }
      else
        {
          gtk_widget_set_visible (self->thread_banner_description, FALSE);
        }
    }
}
