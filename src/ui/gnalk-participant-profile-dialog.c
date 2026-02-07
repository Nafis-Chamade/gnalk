/* gnalk-participant-profile-dialog.c
 *
 * Shows participant profile information: avatar, name, status, role.
 * Supports both local and federated participants.
 */

#include "gnalk-participant-profile-dialog.h"
#include <glib/gi18n.h>

struct _GnalkParticipantProfileDialog
{
  AdwDialog parent_instance;

  GtkWidget *avatar_image;
  GtkWidget *name_label;
  GtkWidget *id_label;
  GtkWidget *role_label;
  GtkWidget *status_box;
  GtkWidget *status_label;
  GtkWidget *server_label;
};

G_DEFINE_FINAL_TYPE (GnalkParticipantProfileDialog, gnalk_participant_profile_dialog, ADW_TYPE_DIALOG)

/* Avatar load callback data */
typedef struct {
  GWeakRef avatar_ref;
} ProfileAvatarData;

static void
on_profile_avatar_loaded (GnalkAvatarCache *cache G_GNUC_UNUSED,
                           GdkTexture       *texture,
                           gpointer          user_data)
{
  ProfileAvatarData *data = user_data;
  GtkWidget *avatar = g_weak_ref_get (&data->avatar_ref);

  if (avatar && texture)
    {
      gtk_image_set_from_paintable (GTK_IMAGE (avatar), GDK_PAINTABLE (texture));
      g_object_unref (avatar);
    }
  else if (avatar)
    g_object_unref (avatar);

  g_weak_ref_clear (&data->avatar_ref);
  g_free (data);
}

static const char *
participant_type_to_string (gint participant_type)
{
  switch (participant_type)
    {
    case 1:  return C_("participant role", "Owner");
    case 2:  return C_("participant role", "Moderator");
    case 3:  return C_("participant role", "User");
    case 4:  return C_("participant role", "Guest");
    case 5:  return C_("participant role", "Guest");
    case 6:  return C_("participant role", "Moderator (Guest)");
    default: return C_("participant role", "User");
    }
}

static void
gnalk_participant_profile_dialog_class_init (GnalkParticipantProfileDialogClass *klass)
{
  (void) klass;
}

static void
gnalk_participant_profile_dialog_init (GnalkParticipantProfileDialog *self)
{
  adw_dialog_set_title (ADW_DIALOG (self), _("Profile"));
  adw_dialog_set_content_width (ADW_DIALOG (self), 320);
  adw_dialog_set_content_height (ADW_DIALOG (self), -1);

  /* Build layout programmatically */
  GtkWidget *toolbar_view = adw_toolbar_view_new ();

  /* Header bar */
  GtkWidget *header = adw_header_bar_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

  /* Content */
  GtkWidget *content_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (content_box, 24);
  gtk_widget_set_margin_end (content_box, 24);
  gtk_widget_set_margin_top (content_box, 12);
  gtk_widget_set_margin_bottom (content_box, 24);
  gtk_widget_set_halign (content_box, GTK_ALIGN_CENTER);

  /* Avatar */
  GtkWidget *avatar_frame = gtk_frame_new (NULL);
  gtk_widget_set_overflow (avatar_frame, GTK_OVERFLOW_HIDDEN);
  gtk_widget_add_css_class (avatar_frame, "profile-avatar");
  gtk_widget_set_halign (avatar_frame, GTK_ALIGN_CENTER);

  self->avatar_image = gtk_image_new_from_icon_name ("avatar-default-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (self->avatar_image), 96);
  gtk_frame_set_child (GTK_FRAME (avatar_frame), self->avatar_image);
  gtk_box_append (GTK_BOX (content_box), avatar_frame);

  /* Display name */
  self->name_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->name_label, "title-1");
  gtk_label_set_wrap (GTK_LABEL (self->name_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (self->name_label), GTK_JUSTIFY_CENTER);
  gtk_box_append (GTK_BOX (content_box), self->name_label);

  /* User ID / Cloud ID */
  self->id_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->id_label, "dim-label");
  gtk_label_set_wrap (GTK_LABEL (self->id_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (self->id_label), GTK_JUSTIFY_CENTER);
  gtk_label_set_selectable (GTK_LABEL (self->id_label), TRUE);
  gtk_box_append (GTK_BOX (content_box), self->id_label);

  /* Role */
  self->role_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->role_label, "dim-label");
  gtk_box_append (GTK_BOX (content_box), self->role_label);

  /* Status box (emoji + message) */
  self->status_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign (self->status_box, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (self->status_box, FALSE);

  self->status_label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (self->status_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (self->status_label), GTK_JUSTIFY_CENTER);
  gtk_box_append (GTK_BOX (self->status_box), self->status_label);
  gtk_box_append (GTK_BOX (content_box), self->status_box);

  /* Server label (for federated users) */
  self->server_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->server_label, "dim-label");
  gtk_widget_set_visible (self->server_label, FALSE);
  gtk_label_set_wrap (GTK_LABEL (self->server_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (self->server_label), GTK_JUSTIFY_CENTER);
  gtk_box_append (GTK_BOX (content_box), self->server_label);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), content_box);
  adw_dialog_set_child (ADW_DIALOG (self), toolbar_view);
}

GnalkParticipantProfileDialog *
gnalk_participant_profile_dialog_new (void)
{
  return g_object_new (GNALK_TYPE_PARTICIPANT_PROFILE_DIALOG, NULL);
}

void
gnalk_participant_profile_dialog_set_participant (GnalkParticipantProfileDialog *self,
                                                    const char                     *actor_id,
                                                    const char                     *actor_type,
                                                    const char                     *display_name,
                                                    gint                            participant_type,
                                                    const char                     *status,
                                                    const char                     *status_icon,
                                                    const char                     *status_message,
                                                    GnalkAvatarCache              *cache)
{
  g_return_if_fail (GNALK_IS_PARTICIPANT_PROFILE_DIALOG (self));

  gboolean is_federated = (g_strcmp0 (actor_type, "federated_users") == 0);

  /* Name */
  gtk_label_set_text (GTK_LABEL (self->name_label),
                      (display_name && *display_name) ? display_name : actor_id);

  /* ID */
  if (is_federated)
    {
      /* actor_id is already cloud ID: "user@server.com" */
      gtk_label_set_text (GTK_LABEL (self->id_label), actor_id);
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (self->id_label), actor_id);
    }

  /* Role */
  gtk_label_set_text (GTK_LABEL (self->role_label), participant_type_to_string (participant_type));

  /* Status */
  if (status && *status && !is_federated)
    {
      g_autofree char *status_text = NULL;
      if (status_icon && *status_icon && status_message && *status_message)
        status_text = g_strdup_printf ("%s %s", status_icon, status_message);
      else if (status_icon && *status_icon)
        status_text = g_strdup (status_icon);
      else if (status_message && *status_message)
        status_text = g_strdup (status_message);

      if (status_text)
        {
          gtk_label_set_text (GTK_LABEL (self->status_label), status_text);
          gtk_widget_set_visible (self->status_box, TRUE);
        }
    }

  /* Server info for federated users */
  if (is_federated && actor_id)
    {
      const char *at = strchr (actor_id, '@');
      if (at)
        {
          g_autofree char *server_text = g_strdup_printf (_("Server: %s"), at + 1);
          gtk_label_set_text (GTK_LABEL (self->server_label), server_text);
          gtk_widget_set_visible (self->server_label, TRUE);
        }
    }

  /* Load avatar */
  if (cache && actor_id && *actor_id && !is_federated)
    {
      ProfileAvatarData *cb_data = g_new0 (ProfileAvatarData, 1);
      g_weak_ref_init (&cb_data->avatar_ref, self->avatar_image);
      gnalk_avatar_cache_get_user_avatar (cache, actor_id, 256, on_profile_avatar_loaded, cb_data);
    }
}
