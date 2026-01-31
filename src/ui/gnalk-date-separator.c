/* gnalk-date-separator.c
 *
 * Date separator widget for message list.
 */

#include "gnalk-date-separator.h"

#include <time.h>

struct _GnalkDateSeparator
{
  GtkBox parent_instance;

  GtkWidget *label;

  gint64 timestamp;
  int    year;
  int    month;
  int    day;
};

G_DEFINE_FINAL_TYPE (GnalkDateSeparator, gnalk_date_separator, GTK_TYPE_BOX)

static void
gnalk_date_separator_class_init (GnalkDateSeparatorClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/net/skilpa/gnalk/gnalk-date-separator.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkDateSeparator, label);
}

static void
gnalk_date_separator_init (GnalkDateSeparator *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

GnalkDateSeparator *
gnalk_date_separator_new (void)
{
  return g_object_new (GNALK_TYPE_DATE_SEPARATOR, NULL);
}

void
gnalk_date_separator_set_timestamp (GnalkDateSeparator *self,
                                     gint64               timestamp)
{
  g_return_if_fail (GNALK_IS_DATE_SEPARATOR (self));

  if (timestamp <= 0)
    {
      gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
      return;
    }

  self->timestamp = timestamp;

  /* Parse the timestamp */
  time_t t = (time_t) timestamp;
  struct tm tm_info_buf;
  struct tm *tm_info = localtime_r (&t, &tm_info_buf);

  if (!tm_info)
    {
      gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
      return;
    }

  /* Store date components for comparison */
  self->year = tm_info->tm_year + 1900;
  self->month = tm_info->tm_mon + 1;
  self->day = tm_info->tm_mday;

  /* Get current date for comparison */
  time_t now = time (NULL);
  struct tm now_tm_buf;
  struct tm *now_tm = localtime_r (&now, &now_tm_buf);

  char buffer[64];

  if (now_tm && tm_info->tm_year == now_tm->tm_year &&
      tm_info->tm_yday == now_tm->tm_yday)
    {
      g_strlcpy (buffer, "Today", sizeof (buffer));
    }
  else if (now_tm && tm_info->tm_year == now_tm->tm_year &&
           tm_info->tm_yday == now_tm->tm_yday - 1)
    {
      g_strlcpy (buffer, "Yesterday", sizeof (buffer));
    }
  else
    {
      strftime (buffer, sizeof (buffer), "%A, %B %d, %Y", tm_info);
    }

  gtk_label_set_text (GTK_LABEL (self->label), buffer);
  gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
}

void
gnalk_date_separator_get_date (GnalkDateSeparator *self,
                                int                 *year,
                                int                 *month,
                                int                 *day)
{
  g_return_if_fail (GNALK_IS_DATE_SEPARATOR (self));

  if (year)
    *year = self->year;
  if (month)
    *month = self->month;
  if (day)
    *day = self->day;
}
