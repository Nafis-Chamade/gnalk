/* gnalk-message-search.c
 *
 * Search messages in a conversation using Nextcloud unified search.
 * Non-modal bottom panel: search entry + scrollable results with timestamps.
 *
 * Signals:
 * - "search-requested" (const char *search_term, gint cursor) - User typed a search query
 * - "result-activated" (gint64 message_id) - User clicked a search result
 * - "load-more-requested" (const char *search_term, gint cursor) - Pagination
 * - "close-requested" () - User clicked close button
 */

#include "gnalk-message-search.h"
#include <json-glib/json-glib.h>
#include <glib/gi18n.h>

struct _GnalkMessageSearch
{
  GtkBox parent_instance;

  GtkWidget *search_entry;
  GtkWidget *close_button;
  GtkWidget *scrolled_window;
  GtkWidget *results_box;
  GtkWidget *status_label;
  GtkWidget *load_more_button;

  guint debounce_id;
  gint  last_cursor;
  gboolean has_more;
};

G_DEFINE_FINAL_TYPE (GnalkMessageSearch, gnalk_message_search, GTK_TYPE_BOX)

enum {
  SIGNAL_SEARCH_REQUESTED,
  SIGNAL_RESULT_ACTIVATED,
  SIGNAL_LOAD_MORE_REQUESTED,
  SIGNAL_CLOSE_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
clear_results (GnalkMessageSearch *self)
{
  GtkWidget *child = gtk_widget_get_first_child (self->results_box);
  while (child)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);
      gtk_list_box_remove (GTK_LIST_BOX (self->results_box), child);
      child = next;
    }
  self->last_cursor = 0;
  self->has_more = FALSE;
  gtk_widget_set_visible (self->load_more_button, FALSE);
  gtk_widget_set_visible (self->status_label, FALSE);
  gtk_widget_set_visible (self->scrolled_window, FALSE);
}

static void
on_result_row_activated (GtkListBox         *box G_GNUC_UNUSED,
                         GtkListBoxRow      *row,
                         GnalkMessageSearch *self)
{
  gint64 *id_ptr = g_object_get_data (G_OBJECT (row), "message-id");
  gint64 message_id = id_ptr ? *id_ptr : 0;
  if (message_id > 0)
    g_signal_emit (self, signals[SIGNAL_RESULT_ACTIVATED], 0, message_id);
}

static gboolean
debounce_search (gpointer user_data)
{
  GnalkMessageSearch *self = GNALK_MESSAGE_SEARCH (user_data);
  self->debounce_id = 0;

  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
  if (text && g_utf8_strlen (text, -1) >= 2)
    {
      clear_results (self);
      gtk_label_set_text (GTK_LABEL (self->status_label), _("Searching..."));
      gtk_widget_set_visible (self->status_label, TRUE);
      g_signal_emit (self, signals[SIGNAL_SEARCH_REQUESTED], 0, text, 0);
    }
  else
    {
      clear_results (self);
    }

  return G_SOURCE_REMOVE;
}

static void
on_search_changed (GtkSearchEntry     *entry G_GNUC_UNUSED,
                   GnalkMessageSearch *self)
{
  if (self->debounce_id)
    g_source_remove (self->debounce_id);
  self->debounce_id = g_timeout_add (300, debounce_search, self);
}

static void
on_load_more_clicked (GtkButton          *button G_GNUC_UNUSED,
                      GnalkMessageSearch *self)
{
  if (!self->has_more)
    return;

  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
  if (text && *text)
    g_signal_emit (self, signals[SIGNAL_LOAD_MORE_REQUESTED], 0, text, self->last_cursor);
}

static void
on_close_clicked (GtkButton          *button G_GNUC_UNUSED,
                  GnalkMessageSearch *self)
{
  g_signal_emit (self, signals[SIGNAL_CLOSE_REQUESTED], 0);
}

/* Format a Unix timestamp as a short relative/absolute date string */
static char *
format_timestamp (gint64 timestamp)
{
  if (timestamp <= 0)
    return NULL;

  g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local (timestamp);
  if (!dt)
    return NULL;

  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  gint dt_year, dt_month, dt_day;
  gint now_year, now_month, now_day;
  g_date_time_get_ymd (dt, &dt_year, &dt_month, &dt_day);
  g_date_time_get_ymd (now, &now_year, &now_month, &now_day);

  if (dt_year == now_year && dt_month == now_month && dt_day == now_day)
    return g_date_time_format (dt, "%H:%M");

  /* Yesterday: check day difference */
  g_autoptr(GDateTime) yesterday = g_date_time_add_days (now, -1);
  gint y_year, y_month, y_day;
  g_date_time_get_ymd (yesterday, &y_year, &y_month, &y_day);
  if (dt_year == y_year && dt_month == y_month && dt_day == y_day)
    return g_strdup (_("Yesterday"));

  if (dt_year == now_year)
    return g_date_time_format (dt, "%b %e");

  return g_date_time_format (dt, "%b %e, %Y");
}

static void
gnalk_message_search_dispose (GObject *object)
{
  GnalkMessageSearch *self = GNALK_MESSAGE_SEARCH (object);

  if (self->debounce_id)
    {
      g_source_remove (self->debounce_id);
      self->debounce_id = 0;
    }

  G_OBJECT_CLASS (gnalk_message_search_parent_class)->dispose (object);
}

static void
gnalk_message_search_class_init (GnalkMessageSearchClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = gnalk_message_search_dispose;

  signals[SIGNAL_SEARCH_REQUESTED] =
    g_signal_new ("search-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING,     /* search_term */
                  G_TYPE_INT);       /* cursor */

  signals[SIGNAL_RESULT_ACTIVATED] =
    g_signal_new ("result-activated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT64);     /* message_id */

  signals[SIGNAL_LOAD_MORE_REQUESTED] =
    g_signal_new ("load-more-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING,     /* search_term */
                  G_TYPE_INT);       /* cursor */

  signals[SIGNAL_CLOSE_REQUESTED] =
    g_signal_new ("close-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
gnalk_message_search_init (GnalkMessageSearch *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class (GTK_WIDGET (self), "card");
  gtk_widget_set_margin_start (GTK_WIDGET (self), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (self), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self), 8);

  /* Header: search entry + close button */
  GtkWidget *header_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_margin_start (header_box, 8);
  gtk_widget_set_margin_end (header_box, 4);
  gtk_widget_set_margin_top (header_box, 8);
  gtk_widget_set_margin_bottom (header_box, 4);
  gtk_box_append (GTK_BOX (self), header_box);

  self->search_entry = gtk_search_entry_new ();
  gtk_widget_set_hexpand (self->search_entry, TRUE);
  gtk_box_append (GTK_BOX (header_box), self->search_entry);

  g_signal_connect (self->search_entry, "search-changed",
                    G_CALLBACK (on_search_changed), self);

  self->close_button = gtk_button_new_from_icon_name ("window-close-symbolic");
  gtk_widget_add_css_class (self->close_button, "flat");
  gtk_widget_add_css_class (self->close_button, "circular");
  gtk_box_append (GTK_BOX (header_box), self->close_button);

  g_signal_connect (self->close_button, "clicked",
                    G_CALLBACK (on_close_clicked), self);

  /* Status label */
  self->status_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->status_label, "dim-label");
  gtk_widget_set_margin_top (self->status_label, 8);
  gtk_widget_set_margin_bottom (self->status_label, 8);
  gtk_widget_set_visible (self->status_label, FALSE);
  gtk_box_append (GTK_BOX (self), self->status_label);

  /* Scrolled results (hidden until results arrive) */
  self->scrolled_window = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scrolled_window),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (self->scrolled_window), 280);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (self->scrolled_window), TRUE);
  gtk_widget_set_visible (self->scrolled_window, FALSE);

  self->results_box = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->results_box), GTK_SELECTION_NONE);
  g_signal_connect (self->results_box, "row-activated",
                    G_CALLBACK (on_result_row_activated), self);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (self->scrolled_window),
                                  self->results_box);
  gtk_box_append (GTK_BOX (self), self->scrolled_window);

  /* Load more button */
  self->load_more_button = gtk_button_new_with_label (_("Load more..."));
  gtk_widget_set_margin_start (self->load_more_button, 8);
  gtk_widget_set_margin_end (self->load_more_button, 8);
  gtk_widget_set_margin_bottom (self->load_more_button, 8);
  gtk_widget_add_css_class (self->load_more_button, "flat");
  gtk_widget_set_visible (self->load_more_button, FALSE);
  g_signal_connect (self->load_more_button, "clicked",
                    G_CALLBACK (on_load_more_clicked), self);
  gtk_box_append (GTK_BOX (self), self->load_more_button);
}

GnalkMessageSearch *
gnalk_message_search_new (void)
{
  return g_object_new (GNALK_TYPE_MESSAGE_SEARCH, NULL);
}

void
gnalk_message_search_clear (GnalkMessageSearch *self)
{
  g_return_if_fail (GNALK_IS_MESSAGE_SEARCH (self));

  gtk_editable_set_text (GTK_EDITABLE (self->search_entry), "");
  clear_results (self);
}

void
gnalk_message_search_grab_focus (GnalkMessageSearch *self)
{
  g_return_if_fail (GNALK_IS_MESSAGE_SEARCH (self));
  gtk_widget_grab_focus (self->search_entry);
}

void
gnalk_message_search_set_results (GnalkMessageSearch *self,
                                  JsonArray          *entries,
                                  gboolean            has_more,
                                  gint                cursor)
{
  g_return_if_fail (GNALK_IS_MESSAGE_SEARCH (self));

  gtk_widget_set_visible (self->status_label, FALSE);
  self->has_more = has_more;
  self->last_cursor = cursor;

  guint len = json_array_get_length (entries);
  if (len == 0 && gtk_widget_get_first_child (self->results_box) == NULL)
    {
      gtk_label_set_text (GTK_LABEL (self->status_label), _("No results found"));
      gtk_widget_set_visible (self->status_label, TRUE);
      return;
    }

  for (guint i = 0; i < len; i++)
    {
      JsonObject *entry = json_array_get_object_element (entries, i);
      if (!entry)
        continue;

      const char *title = json_object_get_string_member_with_default (entry, "title", "");
      const char *subline = json_object_get_string_member_with_default (entry, "subline", "");

      /* Extract messageId and timestamp from attributes */
      gint64 message_id = 0;
      gint64 timestamp = 0;
      JsonObject *attributes = json_object_get_object_member (entry, "attributes");
      if (attributes)
        {
          const char *msg_id_str = json_object_get_string_member_with_default (attributes, "messageId", NULL);
          if (msg_id_str)
            message_id = g_ascii_strtoll (msg_id_str, NULL, 10);

          const char *ts_str = json_object_get_string_member_with_default (attributes, "timestamp", NULL);
          if (ts_str)
            timestamp = g_ascii_strtoll (ts_str, NULL, 10);
        }

      /* Build the row */
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      gtk_widget_set_margin_start (box, 10);
      gtk_widget_set_margin_end (box, 10);
      gtk_widget_set_margin_top (box, 6);
      gtk_widget_set_margin_bottom (box, 6);

      /* Top line: author + date */
      GtkWidget *top_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
      gtk_box_append (GTK_BOX (box), top_box);

      GtkWidget *title_label = gtk_label_new (title);
      gtk_label_set_xalign (GTK_LABEL (title_label), 0);
      gtk_widget_add_css_class (title_label, "heading");
      gtk_label_set_ellipsize (GTK_LABEL (title_label), PANGO_ELLIPSIZE_END);
      gtk_widget_set_hexpand (title_label, TRUE);
      gtk_box_append (GTK_BOX (top_box), title_label);

      g_autofree char *date_str = format_timestamp (timestamp);
      if (date_str)
        {
          GtkWidget *date_label = gtk_label_new (date_str);
          gtk_widget_add_css_class (date_label, "dim-label");
          gtk_widget_add_css_class (date_label, "caption");
          gtk_box_append (GTK_BOX (top_box), date_label);
        }

      /* Subline: message excerpt */
      GtkWidget *subline_label = gtk_label_new (subline);
      gtk_label_set_xalign (GTK_LABEL (subline_label), 0);
      gtk_widget_add_css_class (subline_label, "dim-label");
      gtk_label_set_ellipsize (GTK_LABEL (subline_label), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars (GTK_LABEL (subline_label), 60);
      gtk_box_append (GTK_BOX (box), subline_label);

      GtkWidget *row = gtk_list_box_row_new ();
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
      gint64 *id_ptr = g_new (gint64, 1);
      *id_ptr = message_id;
      g_object_set_data_full (G_OBJECT (row), "message-id", id_ptr, g_free);

      gtk_list_box_append (GTK_LIST_BOX (self->results_box), row);
    }

  gtk_widget_set_visible (self->scrolled_window, TRUE);
  gtk_widget_set_visible (self->load_more_button, has_more);
}
