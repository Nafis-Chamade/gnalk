#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNALK_TYPE_LOGIN_VIEW (gnalk_login_view_get_type ())

G_DECLARE_FINAL_TYPE (GnalkLoginView, gnalk_login_view, GNALK, LOGIN_VIEW, GtkBox)

GnalkLoginView *gnalk_login_view_new (void);

/**
 * gnalk_login_view_set_goa_accounts:
 * @self: The login view
 * @accounts: (element-type GnalkGoaAccountInfo) (nullable): List of GOA account infos
 * @active_goa_ids: (element-type utf8) (nullable): List of GOA IDs already connected in gnalk
 *
 * Sets the list of available GOA Nextcloud accounts.
 * Accounts in @active_goa_ids will be shown as disabled/"Already connected".
 */
void gnalk_login_view_set_goa_accounts (GnalkLoginView *self,
                                          GList           *accounts,
                                          GList           *active_goa_ids);

/**
 * gnalk_login_view_clear_goa_accounts:
 * @self: The login view
 *
 * Clears all GOA accounts from the list.
 */
void gnalk_login_view_clear_goa_accounts (GnalkLoginView *self);

/* Mode control */
gboolean gnalk_login_view_get_is_guest_mode (GnalkLoginView *self);
void     gnalk_login_view_set_guest_mode    (GnalkLoginView *self,
                                               gboolean         guest_mode);

/* User mode getters */
const char *gnalk_login_view_get_server   (GnalkLoginView *self);
const char *gnalk_login_view_get_user     (GnalkLoginView *self);
const char *gnalk_login_view_get_password (GnalkLoginView *self);

/* Guest mode getters */
const char *gnalk_login_view_get_room_url      (GnalkLoginView *self);
const char *gnalk_login_view_get_display_name  (GnalkLoginView *self);
const char *gnalk_login_view_get_room_password (GnalkLoginView *self);

/* Pre-populate fields */
void gnalk_login_view_set_server   (GnalkLoginView *self,
                                      const char      *server);
void gnalk_login_view_set_user     (GnalkLoginView *self,
                                      const char      *user);
void gnalk_login_view_set_password (GnalkLoginView *self,
                                      const char      *password);
void gnalk_login_view_set_room_url (GnalkLoginView *self,
                                      const char      *room_url);

/* UI state control */
void gnalk_login_view_set_connecting (GnalkLoginView *self,
                                        gboolean         connecting);
void gnalk_login_view_show_error     (GnalkLoginView *self,
                                        const char      *message);
void gnalk_login_view_clear_error    (GnalkLoginView *self);

G_END_DECLS
