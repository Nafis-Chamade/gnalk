#include "gnalk-incoming-call-banner.h"
#include <glib/gi18n.h>

struct _GnalkIncomingCallBanner
{
  GtkBox parent_instance;

  GtkWidget *revealer;
  GtkWidget *label;
  GtkWidget *accept_button;
  GtkWidget *refuse_button;

  gboolean has_incoming_call;
  char    *room_token;
  char    *caller_id;
  char    *responded_caller_id;
};

G_DEFINE_FINAL_TYPE (GnalkIncomingCallBanner, gnalk_incoming_call_banner, GTK_TYPE_BOX)

enum
{
  SIGNAL_ACCEPT_CALL_REQUESTED,
  SIGNAL_REFUSE_CALL_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
on_accept_clicked (GtkButton                *button,
                   GnalkIncomingCallBanner *self)
{
  (void) button;

  if (!self->has_incoming_call || !self->room_token)
    return;

  g_signal_emit (self, signals[SIGNAL_ACCEPT_CALL_REQUESTED], 0);
}

static void
on_refuse_clicked (GtkButton                *button,
                   GnalkIncomingCallBanner *self)
{
  (void) button;

  g_signal_emit (self, signals[SIGNAL_REFUSE_CALL_REQUESTED], 0);
}

static void
gnalk_incoming_call_banner_finalize (GObject *object)
{
  GnalkIncomingCallBanner *self = GNALK_INCOMING_CALL_BANNER (object);

  g_free (self->room_token);
  g_free (self->caller_id);
  g_free (self->responded_caller_id);

  G_OBJECT_CLASS (gnalk_incoming_call_banner_parent_class)->finalize (object);
}

static void
gnalk_incoming_call_banner_class_init (GnalkIncomingCallBannerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = gnalk_incoming_call_banner_finalize;

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-incoming-call-banner.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkIncomingCallBanner, revealer);
  gtk_widget_class_bind_template_child (widget_class, GnalkIncomingCallBanner, label);
  gtk_widget_class_bind_template_child (widget_class, GnalkIncomingCallBanner, accept_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkIncomingCallBanner, refuse_button);

  signals[SIGNAL_ACCEPT_CALL_REQUESTED] =
    g_signal_new ("accept-call-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_REFUSE_CALL_REQUESTED] =
    g_signal_new ("refuse-call-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);
}

static void
gnalk_incoming_call_banner_init (GnalkIncomingCallBanner *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  g_signal_connect (self->accept_button, "clicked",
                    G_CALLBACK (on_accept_clicked), self);
  g_signal_connect (self->refuse_button, "clicked",
                    G_CALLBACK (on_refuse_clicked), self);
}

GnalkIncomingCallBanner *
gnalk_incoming_call_banner_new (void)
{
  return g_object_new (GNALK_TYPE_INCOMING_CALL_BANNER, NULL);
}

void
gnalk_incoming_call_banner_show_call (GnalkIncomingCallBanner *self,
                                        const char               *room_token,
                                        const char               *caller_name)
{
  g_return_if_fail (GNALK_IS_INCOMING_CALL_BANNER (self));

  /* Don't show if we've already responded to this caller */
  if (self->responded_caller_id && caller_name &&
      g_strcmp0 (self->responded_caller_id, caller_name) == 0)
    return;

  /* Don't show if already showing */
  if (self->has_incoming_call)
    return;

  g_free (self->room_token);
  g_free (self->caller_id);
  self->room_token = g_strdup (room_token);
  self->caller_id = g_strdup (caller_name);
  self->has_incoming_call = TRUE;

  g_autofree char *label_text = g_strdup_printf (_("Incoming call from %s"),
                                                  caller_name ? caller_name : _("Unknown"));
  gtk_label_set_text (GTK_LABEL (self->label), label_text);

  gtk_revealer_set_reveal_child (GTK_REVEALER (self->revealer), TRUE);
}

void
gnalk_incoming_call_banner_hide (GnalkIncomingCallBanner *self)
{
  g_return_if_fail (GNALK_IS_INCOMING_CALL_BANNER (self));

  gtk_revealer_set_reveal_child (GTK_REVEALER (self->revealer), FALSE);
  self->has_incoming_call = FALSE;

  /* Remember who we responded to */
  g_free (self->responded_caller_id);
  self->responded_caller_id = g_steal_pointer (&self->caller_id);

  g_clear_pointer (&self->room_token, g_free);
}

const char *
gnalk_incoming_call_banner_get_room_token (GnalkIncomingCallBanner *self)
{
  g_return_val_if_fail (GNALK_IS_INCOMING_CALL_BANNER (self), NULL);
  return self->room_token;
}

const char *
gnalk_incoming_call_banner_get_caller_id (GnalkIncomingCallBanner *self)
{
  g_return_val_if_fail (GNALK_IS_INCOMING_CALL_BANNER (self), NULL);
  return self->caller_id;
}

gboolean
gnalk_incoming_call_banner_has_incoming_call (GnalkIncomingCallBanner *self)
{
  g_return_val_if_fail (GNALK_IS_INCOMING_CALL_BANNER (self), FALSE);
  return self->has_incoming_call;
}

void
gnalk_incoming_call_banner_clear_responded (GnalkIncomingCallBanner *self,
                                              const char               *caller_id)
{
  g_return_if_fail (GNALK_IS_INCOMING_CALL_BANNER (self));

  if (self->responded_caller_id && caller_id &&
      g_strcmp0 (self->responded_caller_id, caller_id) == 0)
    {
      g_clear_pointer (&self->responded_caller_id, g_free);
    }
}

void
gnalk_incoming_call_banner_clear_all_responded (GnalkIncomingCallBanner *self)
{
  g_return_if_fail (GNALK_IS_INCOMING_CALL_BANNER (self));
  g_clear_pointer (&self->responded_caller_id, g_free);
}
