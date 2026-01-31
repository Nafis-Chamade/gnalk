#pragma once

#include <gtk/gtk.h>
#include "gnalk-avatar-cache.h"

G_BEGIN_DECLS

#define GNALK_TYPE_USER_STATUS_MENU (gnalk_user_status_menu_get_type ())
G_DECLARE_FINAL_TYPE (GnalkUserStatusMenu, gnalk_user_status_menu, GNALK, USER_STATUS_MENU, GtkBox)

GnalkUserStatusMenu *gnalk_user_status_menu_new (void);

void gnalk_user_status_menu_set_status (GnalkUserStatusMenu *self,
                                          const char           *status);

const char *gnalk_user_status_menu_get_status (GnalkUserStatusMenu *self);

void gnalk_user_status_menu_set_user (GnalkUserStatusMenu *self,
                                        const char           *user_id,
                                        GnalkAvatarCache    *cache);

void gnalk_user_status_menu_set_visible (GnalkUserStatusMenu *self,
                                           gboolean              visible);

G_END_DECLS
