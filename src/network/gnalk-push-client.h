#pragma once

#include <glib-object.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

#define GNALK_TYPE_PUSH_CLIENT (gnalk_push_client_get_type ())
G_DECLARE_FINAL_TYPE (GnalkPushClient, gnalk_push_client, GNALK, PUSH_CLIENT, GObject)

typedef void (*GnalkPushNotificationCallback) (GnalkPushClient *client,
                                                 const char       *notification_type,
                                                 gpointer          user_data);

GnalkPushClient *gnalk_push_client_new (void);

void gnalk_push_client_connect (GnalkPushClient *self,
                                  const char       *websocket_url,
                                  const char       *user_id,
                                  const char       *password);

void gnalk_push_client_disconnect (GnalkPushClient *self);

gboolean gnalk_push_client_is_connected (GnalkPushClient *self);

gboolean gnalk_push_client_is_authenticated (GnalkPushClient *self);

void gnalk_push_client_set_notification_callback (GnalkPushClient               *self,
                                                    GnalkPushNotificationCallback  callback,
                                                    gpointer                        user_data);

G_END_DECLS
