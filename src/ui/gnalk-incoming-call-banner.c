#include "gnalk-incoming-call-banner.h"
#include <glib/gi18n.h>

struct _GnalkIncomingCallBanner
{
  GtkBox parent_instance;

  GtkWidget *revealer;
  GtkWidget *avatar_image;
  GtkWidget *room_label;
  GtkWidget *label;
  GtkWidget *accept_audio_button;
  GtkWidget *accept_video_button;
  GtkWidget *refuse_button;

  gboolean has_incoming_call;
  char    *room_token;
  char    *room_name;
  char    *caller_id;
  char    *responded_caller_id;
};

G_DEFINE_FINAL_TYPE (GnalkIncomingCallBanner, gnalk_incoming_call_banner, GTK_TYPE_BOX)

enum
{
  SIGNAL_ACCEPT_AUDIO_CALL_REQUESTED,
  SIGNAL_ACCEPT_VIDEO_CALL_REQUESTED,
  SIGNAL_REFUSE_CALL_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Weak reference wrapper for async avatar callbacks */
typedef struct
{
  GWeakRef weak_ref;
} BannerAvatarData;

static void
on_avatar_loaded (GnalkAvatarCache *cache,
                  GdkTexture        *texture,
                  gpointer           user_data)
{
  (void) cache;
  BannerAvatarData *data = user_data;
  GnalkIncomingCallBanner *self = g_weak_ref_get (&data->weak_ref);

  if (self)
    {
      if (texture)
        gtk_image_set_from_paintable (GTK_IMAGE (self->avatar_image), GDK_PAINTABLE (texture));
      else
        gtk_image_set_from_icon_name (GTK_IMAGE (self->avatar_image), "avatar-default-symbolic");
      g_object_unref (self);
    }

  g_weak_ref_clear (&data->weak_ref);
  g_free (data);
}

static void
on_accept_audio_clicked (GtkButton                *button,
                         GnalkIncomingCallBanner *self)
{
  (void) button;

  if (!self->has_incoming_call || !self->room_token)
    return;

  g_signal_emit (self, signals[SIGNAL_ACCEPT_AUDIO_CALL_REQUESTED], 0);
}

static void
on_accept_video_clicked (GtkButton                *button,
                         GnalkIncomingCallBanner *self)
{
  (void) button;

  if (!self->has_incoming_call || !self->room_token)
    return;

  g_signal_emit (self, signals[SIGNAL_ACCEPT_VIDEO_CALL_REQUESTED], 0);
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
  g_free (self->room_name);
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
  gtk_widget_class_bind_template_child (widget_class, GnalkIncomingCallBanner, avatar_image);
  gtk_widget_class_bind_template_child (widget_class, GnalkIncomingCallBanner, room_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkIncomingCallBanner, label);
  gtk_widget_class_bind_template_child (widget_class, GnalkIncomingCallBanner, accept_audio_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkIncomingCallBanner, accept_video_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkIncomingCallBanner, refuse_button);

  signals[SIGNAL_ACCEPT_AUDIO_CALL_REQUESTED] =
    g_signal_new ("accept-audio-call-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_ACCEPT_VIDEO_CALL_REQUESTED] =
    g_signal_new ("accept-video-call-requested",
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

  g_signal_connect (self->accept_audio_button, "clicked",
                    G_CALLBACK (on_accept_audio_clicked), self);
  g_signal_connect (self->accept_video_button, "clicked",
                    G_CALLBACK (on_accept_video_clicked), self);
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
                                        const char               *room_name,
                                        const char               *caller_name,
                                        GnalkAvatarCache         *avatar_cache)
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
  g_free (self->room_name);
  g_free (self->caller_id);
  self->room_token = g_strdup (room_token);
  self->room_name = g_strdup (room_name);
  self->caller_id = g_strdup (caller_name);
  self->has_incoming_call = TRUE;

  /* Set room name label */
  gtk_label_set_text (GTK_LABEL (self->room_label),
                      room_name ? room_name : (room_token ? room_token : _("Unknown")));

  /* Set caller info label */
  g_autofree char *label_text = g_strdup_printf (_("Incoming call from %s"),
                                                  caller_name ? caller_name : _("Unknown"));
  gtk_label_set_text (GTK_LABEL (self->label), label_text);

  /* Reset avatar to default, then load async */
  gtk_image_set_from_icon_name (GTK_IMAGE (self->avatar_image), "avatar-default-symbolic");
  if (avatar_cache && room_token)
    {
      BannerAvatarData *cb_data = g_new0 (BannerAvatarData, 1);
      g_weak_ref_init (&cb_data->weak_ref, self);
      gnalk_avatar_cache_get_room_avatar (avatar_cache, room_token, on_avatar_loaded, cb_data);
    }

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
  g_clear_pointer (&self->room_name, g_free);

  /* Reset avatar */
  gtk_image_set_from_icon_name (GTK_IMAGE (self->avatar_image), "avatar-default-symbolic");
}

const char *
gnalk_incoming_call_banner_get_room_token (GnalkIncomingCallBanner *self)
{
  g_return_val_if_fail (GNALK_IS_INCOMING_CALL_BANNER (self), NULL);
  return self->room_token;
}

const char *
gnalk_incoming_call_banner_get_room_name (GnalkIncomingCallBanner *self)
{
  g_return_val_if_fail (GNALK_IS_INCOMING_CALL_BANNER (self), NULL);
  return self->room_name;
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
