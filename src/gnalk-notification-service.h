#pragma once

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GNALK_TYPE_NOTIFICATION_SERVICE (gnalk_notification_service_get_type ())
G_DECLARE_FINAL_TYPE (GnalkNotificationService, gnalk_notification_service, GNALK, NOTIFICATION_SERVICE, GObject)

GnalkNotificationService *gnalk_notification_service_new (void);

void gnalk_notification_service_notify_message (GnalkNotificationService *self,
                                                   const char                *room_token,
                                                   const char                *room_name,
                                                   const char                *sender_name,
                                                   const char                *message_text,
                                                   GIcon                     *sender_avatar,
                                                   gboolean                   is_mention);

void gnalk_notification_service_withdraw (GnalkNotificationService *self,
                                            const char                *room_token);

void gnalk_notification_service_notify_call (GnalkNotificationService *self,
                                                const char                *room_token,
                                                const char                *room_name,
                                                const char                *caller_name);

void gnalk_notification_service_withdraw_call (GnalkNotificationService *self,
                                                  const char                *room_token);

void gnalk_notification_service_withdraw_all (GnalkNotificationService *self);

G_END_DECLS
