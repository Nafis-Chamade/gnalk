/* gnalk-login-flow-v2.h
 *
 * Nextcloud Login Flow v2 protocol implementation.
 * Opens the browser for authentication and polls for credentials.
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define GNALK_TYPE_LOGIN_FLOW_V2 (gnalk_login_flow_v2_get_type ())

G_DECLARE_FINAL_TYPE (GnalkLoginFlowV2, gnalk_login_flow_v2, GNALK, LOGIN_FLOW_V2, GObject)

GnalkLoginFlowV2 *gnalk_login_flow_v2_new       (void);
void               gnalk_login_flow_v2_start     (GnalkLoginFlowV2 *self,
                                                   const char       *server_url);
void               gnalk_login_flow_v2_cancel    (GnalkLoginFlowV2 *self);
gboolean           gnalk_login_flow_v2_is_active (GnalkLoginFlowV2 *self);

G_END_DECLS
