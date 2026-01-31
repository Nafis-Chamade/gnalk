#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define GNALK_TYPE_WINDOW (gnalk_window_get_type ())
G_DECLARE_FINAL_TYPE (GnalkWindow, gnalk_window, GNALK, WINDOW, AdwApplicationWindow)

GnalkWindow *gnalk_window_new (GtkApplication *app);

/* Open a conversation by token (for notification actions) */
void gnalk_window_open_room (GnalkWindow *self, const char *room_token);

G_END_DECLS
