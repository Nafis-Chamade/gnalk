/* gnalk-date-separator.h
 *
 * Date separator widget for message list.
 * Displays "Today", "Yesterday", or full date.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNALK_TYPE_DATE_SEPARATOR (gnalk_date_separator_get_type ())
G_DECLARE_FINAL_TYPE (GnalkDateSeparator, gnalk_date_separator, GNALK, DATE_SEPARATOR, GtkBox)

GnalkDateSeparator *gnalk_date_separator_new (void);

/**
 * gnalk_date_separator_set_timestamp:
 * @self: the date separator widget
 * @timestamp: Unix timestamp in seconds
 *
 * Sets the timestamp and updates the displayed date.
 * Shows "Today", "Yesterday", or full date format.
 */
void gnalk_date_separator_set_timestamp (GnalkDateSeparator *self,
                                          gint64               timestamp);

/**
 * gnalk_date_separator_get_date:
 * @self: the date separator widget
 * @year: (out): the year
 * @month: (out): the month (1-12)
 * @day: (out): the day (1-31)
 *
 * Gets the date components for comparison.
 */
void gnalk_date_separator_get_date (GnalkDateSeparator *self,
                                     int                 *year,
                                     int                 *month,
                                     int                 *day);

G_END_DECLS
