#pragma once

#include <adwaita.h>
#include "gnalk-http-client.h"

G_BEGIN_DECLS

#define GNALK_TYPE_CONNECTION_DIALOG (gnalk_connection_dialog_get_type ())
G_DECLARE_FINAL_TYPE (GnalkConnectionDialog, gnalk_connection_dialog, GNALK, CONNECTION_DIALOG, AdwDialog)

GnalkConnectionDialog *gnalk_connection_dialog_new (void);

void gnalk_connection_dialog_set_http_client (GnalkConnectionDialog *self,
                                                GnalkHttpClient       *client);

void gnalk_connection_dialog_set_defaults (GnalkConnectionDialog *self,
                                             const char             *server_url,
                                             const char             *user_id);

G_END_DECLS
