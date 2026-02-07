#include "gnalk-notification-service.h"
#include "gnalk-config.h"
#include "gnalk-log.h"

#ifdef HAVE_LIBPORTAL
#include <libportal-gtk4/portal-gtk4.h>
#endif

struct _GnalkNotificationService
{
  GObject parent_instance;

#ifdef HAVE_LIBPORTAL
  XdpPortal *portal;
#endif

  GHashTable *active_notifications;  /* room_token -> notification_id */
};

G_DEFINE_FINAL_TYPE (GnalkNotificationService, gnalk_notification_service, G_TYPE_OBJECT)

static void
gnalk_notification_service_finalize (GObject *object)
{
  GnalkNotificationService *self = GNALK_NOTIFICATION_SERVICE (object);

#ifdef HAVE_LIBPORTAL
  if (self->portal)
    g_signal_handlers_disconnect_by_data (self->portal, NULL);
  g_clear_object (&self->portal);
#endif

  g_clear_pointer (&self->active_notifications, g_hash_table_unref);

  G_OBJECT_CLASS (gnalk_notification_service_parent_class)->finalize (object);
}

static void
gnalk_notification_service_class_init (GnalkNotificationServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_notification_service_finalize;
}

#ifdef HAVE_LIBPORTAL
static void
on_notification_action_invoked (XdpPortal   *portal G_GNUC_UNUSED,
                                 const char  *id G_GNUC_UNUSED,
                                 const char  *action,
                                 GVariant    *parameter,
                                 gpointer     user_data G_GNUC_UNUSED)
{
  gnalk_log_notification ("Action invoked: %s (param type: %s)",
                          action, parameter ? g_variant_get_type_string (parameter) : "none");

  GApplication *app = g_application_get_default ();
  if (!app)
    return;

  /* Strip "app." prefix if present - portal passes the full action name */
  const char *action_name = action;
  if (g_str_has_prefix (action, "app."))
    action_name = action + 4;

  /* Activate the action on the application */
  g_action_group_activate_action (G_ACTION_GROUP (app), action_name, parameter);
}
#endif

static void
gnalk_notification_service_init (GnalkNotificationService *self)
{
#ifdef HAVE_LIBPORTAL
  self->portal = xdp_portal_new ();
  g_signal_connect (self->portal, "notification-action-invoked",
                    G_CALLBACK (on_notification_action_invoked), NULL);
#endif

  self->active_notifications = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}

GnalkNotificationService *
gnalk_notification_service_new (void)
{
  return g_object_new (GNALK_TYPE_NOTIFICATION_SERVICE, NULL);
}

#ifdef HAVE_LIBPORTAL
static void
on_notification_sent (GObject      *source,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  XdpPortal *portal = XDP_PORTAL (source);

  if (!xdp_portal_add_notification_finish (portal, result, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to send notification: %s", error->message);
    }
}

static GVariant *
build_notification_buttons (const char *room_token)
{
  GVariantBuilder buttons;
  g_variant_builder_init (&buttons, G_VARIANT_TYPE ("aa{sv}"));

  /* Mark as read button */
  GVariantBuilder mark_read_button;
  g_variant_builder_init (&mark_read_button, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&mark_read_button, "{sv}", "label", g_variant_new_string ("Mark as Read"));
  g_variant_builder_add (&mark_read_button, "{sv}", "action", g_variant_new_string ("app.mark-read"));
  g_variant_builder_add (&mark_read_button, "{sv}", "target", g_variant_new_string (room_token));
  g_variant_builder_add (&buttons, "a{sv}", &mark_read_button);

  return g_variant_builder_end (&buttons);
}
#endif

void
gnalk_notification_service_notify_message (GnalkNotificationService *self,
                                             const char                *room_token,
                                             const char                *room_name,
                                             const char                *sender_name,
                                             const char                *message_text,
                                             GIcon                     *sender_avatar,
                                             gboolean                   is_mention)
{
  g_return_if_fail (GNALK_IS_NOTIFICATION_SERVICE (self));
  g_return_if_fail (room_token != NULL);

  gnalk_log_notification ("SEND message: room=%s, room_name=%s, sender=%s, text=%.80s, mention=%d",
                          room_token,
                          room_name ? room_name : "(null)",
                          sender_name ? sender_name : "(null)",
                          message_text ? message_text : "(null)",
                          is_mention);

  /* Use room_token as notification ID for grouping */
  g_autofree char *notification_id = g_strdup_printf ("message-%s", room_token);

  /* Track this notification */
  g_hash_table_insert (self->active_notifications,
                       g_strdup (room_token),
                       g_strdup (notification_id));

#ifdef HAVE_LIBPORTAL
  /* Build notification using Portal v2 */
  GVariantBuilder notification;
  g_variant_builder_init (&notification, G_VARIANT_TYPE ("a{sv}"));

  /* Title: room name, sender name, or fallback */
  const char *title = room_name ? room_name : (sender_name ? sender_name : "New message");
  g_variant_builder_add (&notification, "{sv}", "title", g_variant_new_string (title));

  /* Body: if sender provided, prepend "sender: "; otherwise use message as-is */
  g_autofree char *body = NULL;
  if (sender_name && message_text)
    body = g_strdup_printf ("%s: %s", sender_name, message_text);
  else
    body = g_strdup (message_text ? message_text : "");
  g_variant_builder_add (&notification, "{sv}", "body", g_variant_new_string (body));

  /* Priority: high for mentions, normal otherwise */
  g_variant_builder_add (&notification, "{sv}", "priority",
                         g_variant_new_string (is_mention ? "urgent" : "normal"));

  /* Default action: open the room */
  g_variant_builder_add (&notification, "{sv}", "default-action",
                         g_variant_new_string ("app.open-room"));
  g_variant_builder_add (&notification, "{sv}", "default-action-target",
                         g_variant_new_string (room_token));

  /* Buttons */
  g_variant_builder_add (&notification, "{sv}", "buttons",
                         build_notification_buttons (room_token));

  /* Icon from avatar if available */
  if (sender_avatar)
    {
      g_autoptr(GVariant) icon_variant = g_icon_serialize (sender_avatar);
      if (icon_variant)
        g_variant_builder_add (&notification, "{sv}", "icon", icon_variant);
    }

  xdp_portal_add_notification (self->portal,
                               notification_id,
                               g_variant_builder_end (&notification),
                               XDP_NOTIFICATION_FLAG_NONE,
                               NULL,  /* cancellable */
                               on_notification_sent,
                               NULL);
#else
  /* Fallback to GNotification */
  GApplication *app = g_application_get_default ();
  if (!app)
    return;

  const char *notif_title = room_name ? room_name : (sender_name ? sender_name : "New message");
  g_autoptr(GNotification) notification = g_notification_new (notif_title);

  g_autofree char *body = NULL;
  if (sender_name && message_text)
    body = g_strdup_printf ("%s: %s", sender_name, message_text);
  else
    body = g_strdup (message_text ? message_text : "");
  g_notification_set_body (notification, body);

  if (is_mention)
    g_notification_set_priority (notification, G_NOTIFICATION_PRIORITY_URGENT);
  else
    g_notification_set_priority (notification, G_NOTIFICATION_PRIORITY_NORMAL);

  /* Default action opens the room */
  g_notification_set_default_action_and_target (notification, "app.open-room", "s", room_token);

  /* Add mark-read button */
  g_notification_add_button_with_target (notification, "Mark as Read", "app.mark-read", "s", room_token);

  if (sender_avatar)
    g_notification_set_icon (notification, sender_avatar);

  g_application_send_notification (app, notification_id, notification);
#endif
}

void
gnalk_notification_service_withdraw (GnalkNotificationService *self,
                                       const char                *room_token)
{
  g_return_if_fail (GNALK_IS_NOTIFICATION_SERVICE (self));
  g_return_if_fail (room_token != NULL);

  const char *notification_id = g_hash_table_lookup (self->active_notifications, room_token);
  if (!notification_id)
    return;

  gnalk_log_notification ("WITHDRAW message: room=%s, id=%s", room_token, notification_id);

#ifdef HAVE_LIBPORTAL
  xdp_portal_remove_notification (self->portal, notification_id);
#else
  GApplication *app = g_application_get_default ();
  if (app)
    g_application_withdraw_notification (app, notification_id);
#endif

  g_hash_table_remove (self->active_notifications, room_token);
}

void
gnalk_notification_service_notify_call (GnalkNotificationService *self,
                                           const char                *room_token,
                                           const char                *room_name,
                                           const char                *caller_name)
{
  g_return_if_fail (GNALK_IS_NOTIFICATION_SERVICE (self));
  g_return_if_fail (room_token != NULL);

  gnalk_log_notification ("SEND call: room=%s, room_name=%s, caller=%s",
                          room_token,
                          room_name ? room_name : "(null)",
                          caller_name ? caller_name : "(null)");

  g_autofree char *notification_id = g_strdup_printf ("call-%s", room_token);
  g_autofree char *hash_key = g_strdup_printf ("call-%s", room_token);

  /* Track this notification (key distinct from message notifications) */
  g_hash_table_insert (self->active_notifications,
                       g_strdup (hash_key),
                       g_strdup (notification_id));

  const char *title = (room_name && *room_name) ? room_name : "Incoming call";
  const char *body = caller_name ? caller_name : "Active call";

#ifdef HAVE_LIBPORTAL
  GVariantBuilder notification;
  g_variant_builder_init (&notification, G_VARIANT_TYPE ("a{sv}"));

  g_variant_builder_add (&notification, "{sv}", "title", g_variant_new_string (title));
  g_variant_builder_add (&notification, "{sv}", "body", g_variant_new_string (body));
  g_variant_builder_add (&notification, "{sv}", "priority", g_variant_new_string ("urgent"));

  g_variant_builder_add (&notification, "{sv}", "default-action",
                         g_variant_new_string ("app.open-room"));
  g_variant_builder_add (&notification, "{sv}", "default-action-target",
                         g_variant_new_string (room_token));

  xdp_portal_add_notification (self->portal,
                               notification_id,
                               g_variant_builder_end (&notification),
                               XDP_NOTIFICATION_FLAG_NONE,
                               NULL,
                               on_notification_sent,
                               NULL);
#else
  GApplication *app = g_application_get_default ();
  if (!app)
    return;

  g_autoptr(GNotification) notification = g_notification_new (title);
  g_notification_set_body (notification, body);
  g_notification_set_priority (notification, G_NOTIFICATION_PRIORITY_URGENT);
  g_notification_set_default_action_and_target (notification, "app.open-room", "s", room_token);

  g_application_send_notification (app, notification_id, notification);
#endif
}

void
gnalk_notification_service_withdraw_call (GnalkNotificationService *self,
                                             const char                *room_token)
{
  g_return_if_fail (GNALK_IS_NOTIFICATION_SERVICE (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *hash_key = g_strdup_printf ("call-%s", room_token);

  const char *notification_id = g_hash_table_lookup (self->active_notifications, hash_key);
  if (!notification_id)
    return;

  gnalk_log_notification ("WITHDRAW call: room=%s, id=%s", room_token, notification_id);

#ifdef HAVE_LIBPORTAL
  xdp_portal_remove_notification (self->portal, notification_id);
#else
  GApplication *app = g_application_get_default ();
  if (app)
    g_application_withdraw_notification (app, notification_id);
#endif

  g_hash_table_remove (self->active_notifications, hash_key);
}

void
gnalk_notification_service_withdraw_all (GnalkNotificationService *self)
{
  g_return_if_fail (GNALK_IS_NOTIFICATION_SERVICE (self));

  GHashTableIter iter;
  gpointer key, value;

  gnalk_log_notification ("WITHDRAW ALL (%u active)", g_hash_table_size (self->active_notifications));

  g_hash_table_iter_init (&iter, self->active_notifications);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *notification_id = value;
#ifdef HAVE_LIBPORTAL
      xdp_portal_remove_notification (self->portal, notification_id);
#else
      GApplication *app = g_application_get_default ();
      if (app)
        g_application_withdraw_notification (app, notification_id);
#endif
    }

  g_hash_table_remove_all (self->active_notifications);
}
