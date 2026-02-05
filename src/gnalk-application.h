#pragma once

#include <adwaita.h>
#include "gnalk-database.h"
#include "gnalk-data-repository.h"
#include "gnalk-app-state.h"
#include "gnalk-network-monitor.h"
#include "gnalk-push-client.h"
#include "gnalk-http-client.h"

G_BEGIN_DECLS

/* Forward declarations for multi-account support */
typedef struct _GnalkAccountManager GnalkAccountManager;
typedef struct _GnalkSessionManager GnalkSessionManager;

#define GNALK_TYPE_APPLICATION (gnalk_application_get_type ())
G_DECLARE_FINAL_TYPE (GnalkApplication, gnalk_application, GNALK, APPLICATION, AdwApplication)

GnalkApplication *gnalk_application_new (void);

/* Database, network, and settings */
GnalkDatabase         *gnalk_application_get_database         (GnalkApplication *self);
GnalkDataRepository   *gnalk_application_get_data_repository  (GnalkApplication *self);
GnalkAppState         *gnalk_application_get_app_state        (GnalkApplication *self);
GnalkNetworkMonitor   *gnalk_application_get_network_monitor  (GnalkApplication *self);
GSettings             *gnalk_application_get_settings         (GnalkApplication *self);

/* Legacy single-account credential functions removed.
 * Use gnalk_account_get_* via gnalk_session_get_account() instead. */

void gnalk_application_set_signaling_settings (GnalkApplication *self,
                                                 const char        *server,
                                                 const char        *ticket);

void gnalk_application_set_ice_servers (GnalkApplication *self,
                                          const char        *stun_server,
                                          const char        *turn_server,
                                          const char        *turn_username,
                                          const char        *turn_credential);

const char *gnalk_application_get_stun_server     (GnalkApplication *self);
const char *gnalk_application_get_turn_server     (GnalkApplication *self);
const char *gnalk_application_get_turn_username   (GnalkApplication *self);
const char *gnalk_application_get_turn_credential (GnalkApplication *self);

void gnalk_application_set_push_websocket_url (GnalkApplication *self,
                                                 const char        *url);
const char *gnalk_application_get_push_websocket_url (GnalkApplication *self);

/* Guest mode */
void gnalk_application_set_guest_mode (GnalkApplication *self,
                                         gboolean           is_guest,
                                         const char        *room_token);
gboolean gnalk_application_is_guest (GnalkApplication *self);
const char *gnalk_application_get_guest_room_token (GnalkApplication *self);

/* Capabilities */
void   gnalk_application_set_chat_edit_timeout (GnalkApplication *self,
                                                  gint64             timeout);
gint64 gnalk_application_get_chat_edit_timeout (GnalkApplication *self);
void   gnalk_application_set_chat_delete_timeout (GnalkApplication *self,
                                                    gint64             timeout);
gint64 gnalk_application_get_chat_delete_timeout (GnalkApplication *self);

/* Multi-account support */
GnalkAccountManager *gnalk_application_get_account_manager (GnalkApplication *self);
GnalkSessionManager *gnalk_application_get_session_manager (GnalkApplication *self);

/* Push client (moved from window for service mode support) */
GnalkPushClient *gnalk_application_get_push_client    (GnalkApplication *self);
GnalkHttpClient *gnalk_application_get_http_client    (GnalkApplication *self);

/* Window state for notification decisions */
void gnalk_application_set_active_room   (GnalkApplication *self,
                                            const char        *room_token);
void gnalk_application_set_window_focused (GnalkApplication *self,
                                             gboolean           focused);

/* Room navigation (called by notification actions) */
char *gnalk_application_consume_pending_room (GnalkApplication *self);

G_END_DECLS
