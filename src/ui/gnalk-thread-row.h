#pragma once

#include <gtk/gtk.h>
#include "gnalk-thread-data.h"

G_BEGIN_DECLS

#define GNALK_TYPE_THREAD_ROW (gnalk_thread_row_get_type ())
G_DECLARE_FINAL_TYPE (GnalkThreadRow, gnalk_thread_row, GNALK, THREAD_ROW, GtkBox)

GnalkThreadRow *gnalk_thread_row_new (void);

void gnalk_thread_row_set_from_data (GnalkThreadRow  *self,
                                     GnalkThreadData *data);

void gnalk_thread_row_reset (GnalkThreadRow *self);

G_END_DECLS
