#include "gnalk-account-switcher.h"
#include "gnalk-session.h"
#include "gnalk-account.h"
#include <glib/gi18n.h>

struct _GnalkAccountSwitcher
{
  GtkWidget parent_instance;

  GtkWidget *menu_button;
  GtkWidget *button_label;
  GtkWidget *button_subtitle;
  GtkWidget *popover;
  GtkWidget *accounts_box;
  GtkWidget *add_account_button;

  GnalkSessionManager *session_manager;
  gulong active_changed_handler;
  gulong session_added_handler;
  gulong session_removed_handler;
  gulong state_changed_handler;
};

G_DEFINE_FINAL_TYPE (GnalkAccountSwitcher, gnalk_account_switcher, GTK_TYPE_WIDGET)

enum {
  SIGNAL_ADD_ACCOUNT_REQUESTED,
  SIGNAL_SIGN_OUT_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void update_ui (GnalkAccountSwitcher *self);
static void rebuild_accounts_list (GnalkAccountSwitcher *self);

static void
on_account_row_clicked (GtkButton             *button,
                        GnalkAccountSwitcher *self)
{
  gint64 account_id = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "account-id"));

  if (self->session_manager)
    {
      gnalk_session_manager_set_active_by_account_id (self->session_manager, account_id);
      gtk_popover_popdown (GTK_POPOVER (self->popover));
    }
}

static void
on_add_account_clicked (GtkButton             *button G_GNUC_UNUSED,
                        GnalkAccountSwitcher *self)
{
  gtk_popover_popdown (GTK_POPOVER (self->popover));
  g_signal_emit (self, signals[SIGNAL_ADD_ACCOUNT_REQUESTED], 0);
}

static void
on_sign_out_clicked (GtkButton             *button,
                     GnalkAccountSwitcher *self)
{
  gint64 account_id = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "account-id"));

  gtk_popover_popdown (GTK_POPOVER (self->popover));
  g_signal_emit (self, signals[SIGNAL_SIGN_OUT_REQUESTED], 0, account_id);
}

static void
on_active_session_changed (GnalkSessionManager  *manager G_GNUC_UNUSED,
                           GnalkSession         *session G_GNUC_UNUSED,
                           GnalkAccountSwitcher *self)
{
  update_ui (self);
  rebuild_accounts_list (self);  /* Update checkmarks */
}

static void
on_session_added (GnalkSessionManager  *manager G_GNUC_UNUSED,
                  GnalkSession         *session G_GNUC_UNUSED,
                  GnalkAccountSwitcher *self)
{
  rebuild_accounts_list (self);
}

static void
on_session_removed (GnalkSessionManager  *manager G_GNUC_UNUSED,
                    gint64                 account_id G_GNUC_UNUSED,
                    GnalkAccountSwitcher *self)
{
  rebuild_accounts_list (self);
}

static void
on_any_session_state_changed (GnalkSessionManager  *manager G_GNUC_UNUSED,
                              GnalkSession         *session G_GNUC_UNUSED,
                              int                    old_state G_GNUC_UNUSED,
                              int                    new_state G_GNUC_UNUSED,
                              GnalkAccountSwitcher *self)
{
  /* Rebuild list to update status icons, and update the button label
   * (display name may have been fetched since the session was first set active) */
  update_ui (self);
  rebuild_accounts_list (self);
}

static void
update_ui (GnalkAccountSwitcher *self)
{
  if (!self->session_manager || !self->button_label)
    return;

  GnalkSession *active = gnalk_session_manager_get_active_session (self->session_manager);
  if (!active)
    {
      gtk_label_set_label (GTK_LABEL (self->button_label), "");
      gtk_label_set_label (GTK_LABEL (self->button_subtitle), "");
      return;
    }

  GnalkAccount *account = gnalk_session_get_account (active);
  if (!account)
    return;

  const char *display_name = gnalk_account_get_display_name (account);
  const char *user_id = gnalk_account_get_user_id (account);
  const char *server_url = gnalk_account_get_server_url (account);

  gtk_label_set_label (GTK_LABEL (self->button_label),
                       display_name && *display_name ? display_name : user_id);

  /* Extract hostname from server URL */
  if (server_url)
    {
      g_autoptr(GUri) uri = g_uri_parse (server_url, G_URI_FLAGS_NONE, NULL);
      const char *host = uri ? g_uri_get_host (uri) : server_url;
      gtk_label_set_label (GTK_LABEL (self->button_subtitle), host);
    }
  else
    {
      gtk_label_set_label (GTK_LABEL (self->button_subtitle), "");
    }
}

static GtkWidget *
create_account_row (GnalkAccountSwitcher *self,
                    GnalkSession         *session,
                    gboolean               is_active)
{
  GnalkAccount *account = gnalk_session_get_account (session);
  if (!account)
    return NULL;

  const char *display_name = gnalk_account_get_display_name (account);
  const char *user_id = gnalk_account_get_user_id (account);
  const char *server_url = gnalk_account_get_server_url (account);
  gint64 account_id = gnalk_session_get_account_id (session);

  /* Outer box to hold account button and sign-out button side by side */
  GtkWidget *row_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  GtkWidget *button = gtk_button_new ();
  gtk_button_set_has_frame (GTK_BUTTON (button), FALSE);
  gtk_widget_add_css_class (button, "flat");
  gtk_widget_set_hexpand (button, TRUE);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start (box, 6);
  gtk_widget_set_margin_end (box, 6);
  gtk_widget_set_margin_top (box, 4);
  gtk_widget_set_margin_bottom (box, 4);
  gtk_button_set_child (GTK_BUTTON (button), box);

  /* Status icon based on session state */
  GnalkSessionState state = gnalk_session_get_state (session);
  GtkWidget *status_icon;
  const char *tooltip;

  switch (state)
    {
    case GNALK_SESSION_STATE_ONLINE:
      status_icon = gtk_image_new_from_icon_name ("emblem-ok-symbolic");
      gtk_widget_add_css_class (status_icon, "success");
      tooltip = _("Online");
      break;
    case GNALK_SESSION_STATE_CONNECTING:
    case GNALK_SESSION_STATE_RECONNECTING:
      status_icon = gtk_spinner_new ();
      gtk_spinner_set_spinning (GTK_SPINNER (status_icon), TRUE);
      tooltip = _("Connecting...");
      break;
    case GNALK_SESSION_STATE_OFFLINE:
      status_icon = gtk_image_new_from_icon_name ("network-offline-symbolic");
      gtk_widget_add_css_class (status_icon, "warning");
      tooltip = _("Offline");
      break;
    default:  /* DISCONNECTED */
      status_icon = gtk_image_new_from_icon_name ("network-offline-symbolic");
      gtk_widget_add_css_class (status_icon, "dim-label");
      tooltip = _("Disconnected");
      break;
    }

  gtk_widget_set_tooltip_text (status_icon, tooltip);
  gtk_widget_set_valign (status_icon, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), status_icon);

  /* Account info */
  GtkWidget *info_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (info_box, TRUE);
  gtk_box_append (GTK_BOX (box), info_box);

  GtkWidget *name = gtk_label_new (display_name && *display_name ? display_name : user_id);
  gtk_widget_set_halign (name, GTK_ALIGN_START);
  if (is_active)
    gtk_widget_add_css_class (name, "heading");
  gtk_box_append (GTK_BOX (info_box), name);

  /* Server URL as subtitle */
  if (server_url)
    {
      /* Extract just the hostname from URL */
      g_autoptr(GUri) uri = g_uri_parse (server_url, G_URI_FLAGS_NONE, NULL);
      const char *host = uri ? g_uri_get_host (uri) : server_url;

      GtkWidget *server = gtk_label_new (host);
      gtk_widget_set_halign (server, GTK_ALIGN_START);
      gtk_widget_add_css_class (server, "dim-label");
      gtk_widget_add_css_class (server, "caption");
      gtk_box_append (GTK_BOX (info_box), server);
    }

  /* Checkmark for active account */
  if (is_active)
    {
      GtkWidget *check = gtk_image_new_from_icon_name ("object-select-symbolic");
      gtk_box_append (GTK_BOX (box), check);
    }

  /* Store account ID for click handler */
  g_object_set_data (G_OBJECT (button), "account-id", GINT_TO_POINTER ((gint) account_id));
  g_signal_connect (button, "clicked", G_CALLBACK (on_account_row_clicked), self);

  gtk_box_append (GTK_BOX (row_box), button);

  /* Sign out button */
  GtkWidget *sign_out_btn = gtk_button_new_from_icon_name ("system-log-out-symbolic");
  gtk_button_set_has_frame (GTK_BUTTON (sign_out_btn), FALSE);
  gtk_widget_add_css_class (sign_out_btn, "flat");
  gtk_widget_add_css_class (sign_out_btn, "circular");
  gtk_widget_set_tooltip_text (sign_out_btn, _("Sign Out"));
  gtk_widget_set_valign (sign_out_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end (sign_out_btn, 6);
  g_object_set_data (G_OBJECT (sign_out_btn), "account-id", GINT_TO_POINTER ((gint) account_id));
  g_signal_connect (sign_out_btn, "clicked", G_CALLBACK (on_sign_out_clicked), self);

  gtk_box_append (GTK_BOX (row_box), sign_out_btn);

  return row_box;
}

static void
rebuild_accounts_list (GnalkAccountSwitcher *self)
{
  /* Remove existing account rows */
  GtkWidget *child = gtk_widget_get_first_child (self->accounts_box);
  while (child)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);
      if (child != self->add_account_button)
        gtk_box_remove (GTK_BOX (self->accounts_box), child);
      child = next;
    }

  if (!self->session_manager)
    return;

  GnalkSession *active = gnalk_session_manager_get_active_session (self->session_manager);
  GList *sessions = gnalk_session_manager_get_all_sessions (self->session_manager);

  for (GList *l = sessions; l; l = l->next)
    {
      GnalkSession *session = GNALK_SESSION (l->data);
      gboolean is_active = (session == active);

      GtkWidget *row = create_account_row (self, session, is_active);
      if (row)
        gtk_box_prepend (GTK_BOX (self->accounts_box), row);

      g_object_unref (session);
    }

  g_list_free (sessions);

  /* Update visibility based on session count */
  guint count = gnalk_session_manager_get_session_count (self->session_manager);
  gtk_widget_set_visible (GTK_WIDGET (self), count > 0 || TRUE);  /* Always visible for add account */
}

static void
gnalk_account_switcher_dispose (GObject *object)
{
  GnalkAccountSwitcher *self = GNALK_ACCOUNT_SWITCHER (object);

  if (self->session_manager)
    {
      if (self->active_changed_handler)
        g_signal_handler_disconnect (self->session_manager, self->active_changed_handler);
      if (self->session_added_handler)
        g_signal_handler_disconnect (self->session_manager, self->session_added_handler);
      if (self->session_removed_handler)
        g_signal_handler_disconnect (self->session_manager, self->session_removed_handler);
      if (self->state_changed_handler)
        g_signal_handler_disconnect (self->session_manager, self->state_changed_handler);
      self->session_manager = NULL;
    }

  gtk_widget_unparent (gtk_widget_get_first_child (GTK_WIDGET (self)));

  G_OBJECT_CLASS (gnalk_account_switcher_parent_class)->dispose (object);
}

static void
gnalk_account_switcher_class_init (GnalkAccountSwitcherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnalk_account_switcher_dispose;

  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);

  signals[SIGNAL_ADD_ACCOUNT_REQUESTED] =
    g_signal_new ("add-account-requested", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  signals[SIGNAL_SIGN_OUT_REQUESTED] =
    g_signal_new ("sign-out-requested", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT64);
}

static void
gnalk_account_switcher_init (GnalkAccountSwitcher *self)
{
  /* Create menu button */
  self->menu_button = gtk_menu_button_new ();
  gtk_widget_set_parent (self->menu_button, GTK_WIDGET (self));
  gtk_widget_set_hexpand (self->menu_button, TRUE);

  /* Button content - show account name and arrow */
  GtkWidget *button_content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_hexpand (button_content, TRUE);

  GtkWidget *label_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (label_box, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (label_box, TRUE);
  gtk_box_append (GTK_BOX (button_content), label_box);

  self->button_label = gtk_label_new ("");
  gtk_label_set_ellipsize (GTK_LABEL (self->button_label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign (self->button_label, GTK_ALIGN_END);
  gtk_box_append (GTK_BOX (label_box), self->button_label);

  self->button_subtitle = gtk_label_new ("");
  gtk_label_set_ellipsize (GTK_LABEL (self->button_subtitle), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign (self->button_subtitle, GTK_ALIGN_END);
  gtk_widget_add_css_class (self->button_subtitle, "dim-label");
  gtk_widget_add_css_class (self->button_subtitle, "caption");
  gtk_box_append (GTK_BOX (label_box), self->button_subtitle);

  GtkWidget *arrow = gtk_image_new_from_icon_name ("pan-down-symbolic");
  gtk_box_append (GTK_BOX (button_content), arrow);

  gtk_menu_button_set_child (GTK_MENU_BUTTON (self->menu_button), button_content);

  /* Create popover */
  self->popover = gtk_popover_new ();
  gtk_popover_set_has_arrow (GTK_POPOVER (self->popover), TRUE);

  self->accounts_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_top (self->accounts_box, 6);
  gtk_widget_set_margin_bottom (self->accounts_box, 6);
  gtk_popover_set_child (GTK_POPOVER (self->popover), self->accounts_box);

  /* Add account button */
  self->add_account_button = gtk_button_new ();
  gtk_button_set_has_frame (GTK_BUTTON (self->add_account_button), FALSE);
  gtk_widget_add_css_class (self->add_account_button, "flat");

  GtkWidget *add_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start (add_box, 6);
  gtk_widget_set_margin_end (add_box, 6);
  gtk_widget_set_margin_top (add_box, 4);
  gtk_widget_set_margin_bottom (add_box, 4);

  GtkWidget *add_icon = gtk_image_new_from_icon_name ("list-add-symbolic");
  gtk_box_append (GTK_BOX (add_box), add_icon);

  GtkWidget *add_label = gtk_label_new (_("Add Account"));
  gtk_widget_set_hexpand (add_label, TRUE);
  gtk_widget_set_halign (add_label, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (add_box), add_label);

  gtk_button_set_child (GTK_BUTTON (self->add_account_button), add_box);
  gtk_box_append (GTK_BOX (self->accounts_box), self->add_account_button);

  g_signal_connect (self->add_account_button, "clicked",
                    G_CALLBACK (on_add_account_clicked), self);

  gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->menu_button), self->popover);
}

GnalkAccountSwitcher *
gnalk_account_switcher_new (void)
{
  return g_object_new (GNALK_TYPE_ACCOUNT_SWITCHER, NULL);
}

void
gnalk_account_switcher_set_session_manager (GnalkAccountSwitcher *self,
                                              GnalkSessionManager  *manager)
{
  g_return_if_fail (GNALK_IS_ACCOUNT_SWITCHER (self));

  if (self->session_manager == manager)
    return;

  /* Disconnect from old manager */
  if (self->session_manager)
    {
      if (self->active_changed_handler)
        g_signal_handler_disconnect (self->session_manager, self->active_changed_handler);
      if (self->session_added_handler)
        g_signal_handler_disconnect (self->session_manager, self->session_added_handler);
      if (self->session_removed_handler)
        g_signal_handler_disconnect (self->session_manager, self->session_removed_handler);
      if (self->state_changed_handler)
        g_signal_handler_disconnect (self->session_manager, self->state_changed_handler);
    }

  self->session_manager = manager;

  /* Connect to new manager */
  if (manager)
    {
      self->active_changed_handler = g_signal_connect (manager, "active-session-changed",
                                                         G_CALLBACK (on_active_session_changed), self);
      self->session_added_handler = g_signal_connect (manager, "session-added",
                                                        G_CALLBACK (on_session_added), self);
      self->session_removed_handler = g_signal_connect (manager, "session-removed",
                                                          G_CALLBACK (on_session_removed), self);
      self->state_changed_handler = g_signal_connect (manager, "any-session-state-changed",
                                                        G_CALLBACK (on_any_session_state_changed), self);
    }

  rebuild_accounts_list (self);
  update_ui (self);
}
