#include "gnalk-login-view.h"
#include "gnalk-goa.h"
#include <adwaita.h>
#include <glib/gi18n.h>

struct _GnalkLoginView
{
  GtkBox parent_instance;

  GtkWidget *header_icon;
  GtkWidget *header_label;
  GtkWidget *mode_selector_box;
  GtkWidget *goa_mode_button;
  GtkWidget *browser_mode_button;
  GtkWidget *manual_mode_button;
  GtkWidget *guest_mode_button;
  GtkWidget *goa_accounts_group;
  GtkWidget *server_url_group;
  GtkWidget *credentials_group;
  GtkWidget *browser_waiting_box;
  GtkWidget *browser_waiting_spinner;
  GtkWidget *browser_waiting_label;
  GtkWidget *login_url_box;
  GtkWidget *login_url_label;
  GtkWidget *login_url_copy_button;
  GtkWidget *guest_mode_group;
  GtkWidget *server_entry;
  GtkWidget *user_entry;
  GtkWidget *password_entry;
  GtkWidget *room_url_entry;
  GtkWidget *display_name_entry;
  GtkWidget *guest_password_entry;
  GtkWidget *button_box;
  GtkWidget *cancel_button;
  GtkWidget *connect_button;
  GtkWidget *spinner;
  GtkWidget *status_label;

  GnalkLoginMode login_mode;
  gboolean has_goa_accounts;
  gboolean browser_waiting;
  gboolean updating_mode;  /* Prevent recursive toggle handling */
};

G_DEFINE_FINAL_TYPE (GnalkLoginView, gnalk_login_view, GTK_TYPE_BOX)

enum
{
  SIGNAL_CONNECT_CLICKED,
  SIGNAL_CANCEL_CLICKED,
  SIGNAL_MODE_TOGGLED,
  SIGNAL_GOA_ACCOUNT_SELECTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
update_mode_ui (GnalkLoginView *self)
{
  GnalkLoginMode mode = self->login_mode;

  /* Update header */
  if (mode == GNALK_LOGIN_MODE_GUEST)
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (self->header_icon), "system-users-symbolic");
      gtk_label_set_text (GTK_LABEL (self->header_label), _("Join a public room as guest"));
    }
  else
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (self->header_icon), "network-server-symbolic");
      gtk_label_set_text (GTK_LABEL (self->header_label), _("Sign in to your Nextcloud Talk server"));
    }

  /* GOA tab visibility (always reflects has_goa_accounts) */
  gtk_widget_set_visible (self->goa_mode_button, self->has_goa_accounts);

  /* Tab content visibility */
  gtk_widget_set_visible (self->goa_accounts_group, mode == GNALK_LOGIN_MODE_GOA);

  gboolean show_server = (mode == GNALK_LOGIN_MODE_BROWSER && !self->browser_waiting) ||
                          mode == GNALK_LOGIN_MODE_MANUAL;
  gtk_widget_set_visible (self->server_url_group, show_server);

  gtk_widget_set_visible (self->credentials_group, mode == GNALK_LOGIN_MODE_MANUAL);

  gtk_widget_set_visible (self->browser_waiting_box,
                          mode == GNALK_LOGIN_MODE_BROWSER && self->browser_waiting);

  gtk_widget_set_visible (self->guest_mode_group, mode == GNALK_LOGIN_MODE_GUEST);

  /* Button box: visible for Browser, Manual, Guest (not GOA — GOA uses row click) */
  gboolean show_buttons = (mode != GNALK_LOGIN_MODE_GOA);
  gtk_widget_set_visible (self->button_box, show_buttons);

  /* Connect button: hidden during browser waiting (only cancel visible) */
  gtk_widget_set_visible (self->connect_button,
                          show_buttons && !(mode == GNALK_LOGIN_MODE_BROWSER && self->browser_waiting));

  /* Button label */
  gtk_button_set_label (GTK_BUTTON (self->connect_button),
                        mode == GNALK_LOGIN_MODE_GUEST ? _("Join") : _("Connect"));

  /* Update toggle button states without triggering handlers */
  self->updating_mode = TRUE;
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->goa_mode_button),
                                 mode == GNALK_LOGIN_MODE_GOA);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->browser_mode_button),
                                 mode == GNALK_LOGIN_MODE_BROWSER);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->manual_mode_button),
                                 mode == GNALK_LOGIN_MODE_MANUAL);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->guest_mode_button),
                                 mode == GNALK_LOGIN_MODE_GUEST);
  self->updating_mode = FALSE;

  /* Clear any error when switching modes */
  gtk_widget_set_visible (self->status_label, FALSE);
}

static void
on_connect_button_clicked (GtkButton       *button,
                            GnalkLoginView *self)
{
  (void) button;
  g_signal_emit (self, signals[SIGNAL_CONNECT_CLICKED], 0);
}

static void
on_cancel_button_clicked (GtkButton       *button,
                           GnalkLoginView *self)
{
  (void) button;
  g_signal_emit (self, signals[SIGNAL_CANCEL_CLICKED], 0);
}

static void
on_entry_activated (GtkEntry        *entry,
                    GnalkLoginView *self)
{
  (void) entry;
  g_signal_emit (self, signals[SIGNAL_CONNECT_CLICKED], 0);
}

static void
on_mode_button_toggled (GtkToggleButton *button,
                         GnalkLoginView  *self)
{
  if (self->updating_mode)
    return;

  if (!gtk_toggle_button_get_active (button))
    return;

  GnalkLoginMode new_mode;
  if (GTK_WIDGET (button) == self->goa_mode_button)
    new_mode = GNALK_LOGIN_MODE_GOA;
  else if (GTK_WIDGET (button) == self->browser_mode_button)
    new_mode = GNALK_LOGIN_MODE_BROWSER;
  else if (GTK_WIDGET (button) == self->manual_mode_button)
    new_mode = GNALK_LOGIN_MODE_MANUAL;
  else
    new_mode = GNALK_LOGIN_MODE_GUEST;

  if (self->login_mode == new_mode)
    return;

  self->login_mode = new_mode;
  self->browser_waiting = FALSE;
  gtk_spinner_set_spinning (GTK_SPINNER (self->browser_waiting_spinner), FALSE);
  update_mode_ui (self);
  g_signal_emit (self, signals[SIGNAL_MODE_TOGGLED], 0);
}

static void
on_goa_account_row_activated (AdwActionRow    *row,
                               GnalkLoginView *self)
{
  const char *goa_id = g_object_get_data (G_OBJECT (row), "goa-id");

  if (!goa_id)
    return;

  /* Check if account is disabled (already connected) */
  gboolean is_active = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "is-active"));
  if (is_active)
    return;

  g_signal_emit (self, signals[SIGNAL_GOA_ACCOUNT_SELECTED], 0, goa_id);
}

static void
on_login_url_copy_clicked (GtkButton       *button,
                            GnalkLoginView *self)
{
  (void) button;
  const char *url = gtk_label_get_text (GTK_LABEL (self->login_url_label));
  if (url && *url)
    {
      GdkClipboard *clipboard = gtk_widget_get_clipboard (GTK_WIDGET (self));
      gdk_clipboard_set_text (clipboard, url);
    }
}

static void
gnalk_login_view_class_init (GnalkLoginViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-login-view.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, header_icon);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, header_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, mode_selector_box);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, goa_mode_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, browser_mode_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, manual_mode_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, guest_mode_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, goa_accounts_group);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, server_url_group);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, credentials_group);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, browser_waiting_box);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, browser_waiting_spinner);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, browser_waiting_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, login_url_box);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, login_url_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, login_url_copy_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, guest_mode_group);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, server_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, user_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, password_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, room_url_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, display_name_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, guest_password_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, button_box);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, cancel_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, connect_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, spinner);
  gtk_widget_class_bind_template_child (widget_class, GnalkLoginView, status_label);

  signals[SIGNAL_CONNECT_CLICKED] =
    g_signal_new ("connect-clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_CANCEL_CLICKED] =
    g_signal_new ("cancel-clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_MODE_TOGGLED] =
    g_signal_new ("mode-toggled",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_GOA_ACCOUNT_SELECTED] =
    g_signal_new ("goa-account-selected",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);
}

static void
gnalk_login_view_init (GnalkLoginView *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->login_mode = GNALK_LOGIN_MODE_BROWSER;
  self->has_goa_accounts = FALSE;
  self->browser_waiting = FALSE;
  self->updating_mode = FALSE;

  g_signal_connect (self->connect_button, "clicked",
                    G_CALLBACK (on_connect_button_clicked), self);
  g_signal_connect (self->cancel_button, "clicked",
                    G_CALLBACK (on_cancel_button_clicked), self);
  g_signal_connect (self->goa_mode_button, "toggled",
                    G_CALLBACK (on_mode_button_toggled), self);
  g_signal_connect (self->browser_mode_button, "toggled",
                    G_CALLBACK (on_mode_button_toggled), self);
  g_signal_connect (self->manual_mode_button, "toggled",
                    G_CALLBACK (on_mode_button_toggled), self);
  g_signal_connect (self->guest_mode_button, "toggled",
                    G_CALLBACK (on_mode_button_toggled), self);
  g_signal_connect (self->login_url_copy_button, "clicked",
                    G_CALLBACK (on_login_url_copy_clicked), self);
  g_signal_connect (self->server_entry, "entry-activated",
                    G_CALLBACK (on_entry_activated), self);
  g_signal_connect (self->user_entry, "entry-activated",
                    G_CALLBACK (on_entry_activated), self);
  g_signal_connect (self->password_entry, "entry-activated",
                    G_CALLBACK (on_entry_activated), self);
  g_signal_connect (self->room_url_entry, "entry-activated",
                    G_CALLBACK (on_entry_activated), self);
  g_signal_connect (self->display_name_entry, "entry-activated",
                    G_CALLBACK (on_entry_activated), self);
  g_signal_connect (self->guest_password_entry, "entry-activated",
                    G_CALLBACK (on_entry_activated), self);
}

GnalkLoginView *
gnalk_login_view_new (void)
{
  return g_object_new (GNALK_TYPE_LOGIN_VIEW, NULL);
}

GnalkLoginMode
gnalk_login_view_get_login_mode (GnalkLoginView *self)
{
  g_return_val_if_fail (GNALK_IS_LOGIN_VIEW (self), GNALK_LOGIN_MODE_BROWSER);

  return self->login_mode;
}

gboolean
gnalk_login_view_get_is_guest_mode (GnalkLoginView *self)
{
  g_return_val_if_fail (GNALK_IS_LOGIN_VIEW (self), FALSE);

  return self->login_mode == GNALK_LOGIN_MODE_GUEST;
}

const char *
gnalk_login_view_get_server (GnalkLoginView *self)
{
  g_return_val_if_fail (GNALK_IS_LOGIN_VIEW (self), NULL);

  return gtk_editable_get_text (GTK_EDITABLE (self->server_entry));
}

const char *
gnalk_login_view_get_user (GnalkLoginView *self)
{
  g_return_val_if_fail (GNALK_IS_LOGIN_VIEW (self), NULL);

  return gtk_editable_get_text (GTK_EDITABLE (self->user_entry));
}

const char *
gnalk_login_view_get_password (GnalkLoginView *self)
{
  g_return_val_if_fail (GNALK_IS_LOGIN_VIEW (self), NULL);

  return gtk_editable_get_text (GTK_EDITABLE (self->password_entry));
}

const char *
gnalk_login_view_get_room_url (GnalkLoginView *self)
{
  g_return_val_if_fail (GNALK_IS_LOGIN_VIEW (self), NULL);

  return gtk_editable_get_text (GTK_EDITABLE (self->room_url_entry));
}

const char *
gnalk_login_view_get_display_name (GnalkLoginView *self)
{
  g_return_val_if_fail (GNALK_IS_LOGIN_VIEW (self), NULL);

  return gtk_editable_get_text (GTK_EDITABLE (self->display_name_entry));
}

const char *
gnalk_login_view_get_room_password (GnalkLoginView *self)
{
  g_return_val_if_fail (GNALK_IS_LOGIN_VIEW (self), NULL);

  return gtk_editable_get_text (GTK_EDITABLE (self->guest_password_entry));
}

void
gnalk_login_view_set_server (GnalkLoginView *self,
                               const char      *server)
{
  g_return_if_fail (GNALK_IS_LOGIN_VIEW (self));

  gtk_editable_set_text (GTK_EDITABLE (self->server_entry), server ? server : "");
}

void
gnalk_login_view_set_user (GnalkLoginView *self,
                             const char      *user)
{
  g_return_if_fail (GNALK_IS_LOGIN_VIEW (self));

  gtk_editable_set_text (GTK_EDITABLE (self->user_entry), user ? user : "");
}

void
gnalk_login_view_set_password (GnalkLoginView *self,
                                 const char      *password)
{
  g_return_if_fail (GNALK_IS_LOGIN_VIEW (self));

  gtk_editable_set_text (GTK_EDITABLE (self->password_entry), password ? password : "");
}

void
gnalk_login_view_set_room_url (GnalkLoginView *self,
                                 const char      *room_url)
{
  g_return_if_fail (GNALK_IS_LOGIN_VIEW (self));

  gtk_editable_set_text (GTK_EDITABLE (self->room_url_entry), room_url ? room_url : "");
}

void
gnalk_login_view_set_connecting (GnalkLoginView *self,
                                   gboolean         connecting)
{
  g_return_if_fail (GNALK_IS_LOGIN_VIEW (self));

  gtk_widget_set_visible (self->spinner, connecting);
  gtk_spinner_set_spinning (GTK_SPINNER (self->spinner), connecting);
  gtk_widget_set_visible (self->status_label, FALSE);

  gtk_widget_set_sensitive (self->mode_selector_box, !connecting);
  gtk_widget_set_sensitive (self->cancel_button, !connecting);
  gtk_widget_set_sensitive (self->connect_button, !connecting);
  gtk_widget_set_sensitive (self->server_entry, !connecting);
  gtk_widget_set_sensitive (self->user_entry, !connecting);
  gtk_widget_set_sensitive (self->password_entry, !connecting);
  gtk_widget_set_sensitive (self->room_url_entry, !connecting);
  gtk_widget_set_sensitive (self->display_name_entry, !connecting);
  gtk_widget_set_sensitive (self->guest_password_entry, !connecting);
}

void
gnalk_login_view_set_browser_waiting (GnalkLoginView *self,
                                        gboolean         waiting)
{
  g_return_if_fail (GNALK_IS_LOGIN_VIEW (self));

  self->browser_waiting = waiting;
  gtk_spinner_set_spinning (GTK_SPINNER (self->browser_waiting_spinner), waiting);
  if (!waiting)
    gtk_widget_set_visible (self->login_url_box, FALSE);
  update_mode_ui (self);
}

void
gnalk_login_view_set_login_url (GnalkLoginView *self,
                                  const char      *url)
{
  g_return_if_fail (GNALK_IS_LOGIN_VIEW (self));

  if (url && *url)
    {
      gtk_label_set_text (GTK_LABEL (self->login_url_label), url);
      gtk_widget_set_visible (self->login_url_box, TRUE);
    }
  else
    {
      gtk_widget_set_visible (self->login_url_box, FALSE);
    }
}

void
gnalk_login_view_show_error (GnalkLoginView *self,
                               const char      *message)
{
  g_return_if_fail (GNALK_IS_LOGIN_VIEW (self));

  /* Reset browser waiting state on error */
  self->browser_waiting = FALSE;
  gtk_spinner_set_spinning (GTK_SPINNER (self->browser_waiting_spinner), FALSE);
  gtk_widget_set_visible (self->login_url_box, FALSE);

  gtk_widget_set_visible (self->spinner, FALSE);
  gtk_spinner_set_spinning (GTK_SPINNER (self->spinner), FALSE);

  gtk_label_set_text (GTK_LABEL (self->status_label), message);
  gtk_widget_set_visible (self->status_label, TRUE);

  gtk_widget_set_sensitive (self->mode_selector_box, TRUE);
  gtk_widget_set_sensitive (self->cancel_button, TRUE);
  gtk_widget_set_sensitive (self->connect_button, TRUE);
  gtk_widget_set_sensitive (self->server_entry, TRUE);
  gtk_widget_set_sensitive (self->user_entry, TRUE);
  gtk_widget_set_sensitive (self->password_entry, TRUE);
  gtk_widget_set_sensitive (self->room_url_entry, TRUE);
  gtk_widget_set_sensitive (self->display_name_entry, TRUE);
  gtk_widget_set_sensitive (self->guest_password_entry, TRUE);

  update_mode_ui (self);
}

void
gnalk_login_view_clear_error (GnalkLoginView *self)
{
  g_return_if_fail (GNALK_IS_LOGIN_VIEW (self));

  gtk_widget_set_visible (self->status_label, FALSE);
}

static gboolean
goa_id_in_list (GList *list, const char *goa_id)
{
  for (GList *l = list; l != NULL; l = l->next)
    {
      if (g_strcmp0 ((const char *) l->data, goa_id) == 0)
        return TRUE;
    }
  return FALSE;
}

void
gnalk_login_view_set_goa_accounts (GnalkLoginView *self,
                                     GList           *accounts,
                                     GList           *active_goa_ids)
{
  g_return_if_fail (GNALK_IS_LOGIN_VIEW (self));

  /* Clear existing GOA rows */
  gnalk_login_view_clear_goa_accounts (self);

  if (!accounts)
    {
      self->has_goa_accounts = FALSE;

      /* If we were on GOA mode, switch to Browser */
      if (self->login_mode == GNALK_LOGIN_MODE_GOA)
        self->login_mode = GNALK_LOGIN_MODE_BROWSER;

      update_mode_ui (self);
      return;
    }

  self->has_goa_accounts = TRUE;

  /* Add rows for each GOA account */
  for (GList *l = accounts; l != NULL; l = l->next)
    {
      GnalkGoaAccountInfo *info = (GnalkGoaAccountInfo *) l->data;
      gboolean is_active = goa_id_in_list (active_goa_ids, info->goa_id);

      AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), info->presentation_identity);

      if (is_active)
        {
          adw_action_row_set_subtitle (row, _("Already connected"));
          gtk_widget_set_sensitive (GTK_WIDGET (row), FALSE);
        }
      else
        {
          adw_action_row_set_subtitle (row, info->provider_name);
          gtk_widget_set_sensitive (GTK_WIDGET (row), TRUE);
        }

      /* Add arrow indicator */
      GtkWidget *arrow = gtk_image_new_from_icon_name ("go-next-symbolic");
      adw_action_row_add_suffix (row, arrow);
      adw_action_row_set_activatable_widget (row, arrow);

      /* Store GOA ID on the row */
      g_object_set_data_full (G_OBJECT (row), "goa-id",
                              g_strdup (info->goa_id), g_free);
      g_object_set_data (G_OBJECT (row), "is-active",
                         GINT_TO_POINTER (is_active));

      g_signal_connect (row, "activated",
                        G_CALLBACK (on_goa_account_row_activated), self);

      adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->goa_accounts_group),
                                  GTK_WIDGET (row));
    }

  update_mode_ui (self);
}

void
gnalk_login_view_clear_goa_accounts (GnalkLoginView *self)
{
  g_return_if_fail (GNALK_IS_LOGIN_VIEW (self));

  /* Remove all children from the GOA group except the title/description */
  GtkWidget *child = gtk_widget_get_first_child (self->goa_accounts_group);
  while (child)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);

      /* AdwActionRow instances are the dynamically added account rows */
      if (ADW_IS_ACTION_ROW (child))
        adw_preferences_group_remove (ADW_PREFERENCES_GROUP (self->goa_accounts_group), child);

      child = next;
    }
}
