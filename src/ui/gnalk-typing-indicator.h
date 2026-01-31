#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNALK_TYPE_TYPING_INDICATOR (gnalk_typing_indicator_get_type ())
G_DECLARE_FINAL_TYPE (GnalkTypingIndicator, gnalk_typing_indicator, GNALK, TYPING_INDICATOR, GtkBox)

GnalkTypingIndicator *gnalk_typing_indicator_new (void);

void gnalk_typing_indicator_set_typing (GnalkTypingIndicator *self,
                                          const char            *session_id,
                                          const char            *display_name,
                                          gboolean               is_typing);

void gnalk_typing_indicator_clear (GnalkTypingIndicator *self);

G_END_DECLS
