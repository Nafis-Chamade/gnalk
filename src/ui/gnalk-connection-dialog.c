#include "gnalk-connection-dialog.h"
#include "gnalk-application.h"
#include <glib/gi18n.h>

struct _GnalkConnectionDialog
{
  AdwDialog parent_instance;

  /* Mode toggle */
  GtkWidget *guest_link;
  GtkWidget *user_mode_group;
  GtkWidget *guest_mode_group;

  /* User mode fields */
  GtkWidget *server_entry;
  GtkWidget *user_entry;
  GtkWidget *password_entry;

  /* Guest mode fields */
  GtkWidget *room_url_entry;
  GtkWidget *display_name_entry;
  GtkWidget *guest_password_entry;

  /* Common */
  GtkWidget *connect_button;
  GtkWidget *cancel_button;
  GtkWidget *spinner;
  GtkWidget *status_label;

  GnalkHttpClient *http_client;
  int               step;
  gboolean          is_guest_mode;
  char             *room_token;
};

G_DEFINE_FINAL_TYPE (GnalkConnectionDialog, gnalk_connection_dialog, ADW_TYPE_DIALOG)

enum {
  SIGNAL_CONNECTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void start_connection (GnalkConnectionDialog *self);
static void start_guest_connection (GnalkConnectionDialog *self);
static void on_server_status_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_user_info_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_conversations_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_capabilities_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_signaling_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_guest_room_info_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_guest_join_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_guest_name_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);
static void on_guest_signaling_response (GnalkHttpClient *client, JsonNode *response, GError *error, gpointer user_data);

static void
show_error (GnalkConnectionDialog *self,
            const char             *message)
{
  gtk_widget_set_visible (self->spinner, FALSE);
  gtk_spinner_set_spinning (GTK_SPINNER (self->spinner), FALSE);

  gtk_label_set_text (GTK_LABEL (self->status_label), message);
  gtk_widget_set_visible (self->status_label, TRUE);

  gtk_widget_set_sensitive (self->connect_button, TRUE);
  gtk_widget_set_sensitive (self->guest_link, TRUE);

  /* User mode fields */
  gtk_widget_set_sensitive (self->server_entry, TRUE);
  gtk_widget_set_sensitive (self->user_entry, TRUE);
  gtk_widget_set_sensitive (self->password_entry, TRUE);

  /* Guest mode fields */
  gtk_widget_set_sensitive (self->room_url_entry, TRUE);
  gtk_widget_set_sensitive (self->display_name_entry, TRUE);
  gtk_widget_set_sensitive (self->guest_password_entry, TRUE);
}

static void
set_connecting (GnalkConnectionDialog *self,
                gboolean                connecting)
{
  gtk_widget_set_visible (self->spinner, connecting);
  gtk_spinner_set_spinning (GTK_SPINNER (self->spinner), connecting);
  gtk_widget_set_visible (self->status_label, FALSE);

  gtk_widget_set_sensitive (self->connect_button, !connecting);
  gtk_widget_set_sensitive (self->guest_link, !connecting);

  /* User mode fields */
  gtk_widget_set_sensitive (self->server_entry, !connecting);
  gtk_widget_set_sensitive (self->user_entry, !connecting);
  gtk_widget_set_sensitive (self->password_entry, !connecting);

  /* Guest mode fields */
  gtk_widget_set_sensitive (self->room_url_entry, !connecting);
  gtk_widget_set_sensitive (self->display_name_entry, !connecting);
  gtk_widget_set_sensitive (self->guest_password_entry, !connecting);
}

static void
on_connect_clicked (GtkButton              *button,
                    GnalkConnectionDialog *self)
{
  if (self->is_guest_mode)
    start_guest_connection (self);
  else
    start_connection (self);
}

static void
on_cancel_clicked (GtkButton              *button,
                   GnalkConnectionDialog *self)
{
  adw_dialog_close (ADW_DIALOG (self));
}

static void
on_entry_activated (GtkEntry               *entry,
                    GnalkConnectionDialog *self)
{
  start_connection (self);
}

static void
gnalk_connection_dialog_finalize (GObject *object)
{
  GnalkConnectionDialog *self = GNALK_CONNECTION_DIALOG (object);

  g_clear_object (&self->http_client);
  g_clear_pointer (&self->room_token, g_free);

  G_OBJECT_CLASS (gnalk_connection_dialog_parent_class)->finalize (object);
}

static void
gnalk_connection_dialog_class_init (GnalkConnectionDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = gnalk_connection_dialog_finalize;

  signals[SIGNAL_CONNECTED] =
    g_signal_new ("connected",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, JSON_TYPE_NODE, JSON_TYPE_NODE);

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-connection-dialog.ui");

  /* Mode toggle and groups */
  gtk_widget_class_bind_template_child (widget_class, GnalkConnectionDialog, guest_link);
  gtk_widget_class_bind_template_child (widget_class, GnalkConnectionDialog, user_mode_group);
  gtk_widget_class_bind_template_child (widget_class, GnalkConnectionDialog, guest_mode_group);

  /* User mode fields */
  gtk_widget_class_bind_template_child (widget_class, GnalkConnectionDialog, server_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkConnectionDialog, user_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkConnectionDialog, password_entry);

  /* Guest mode fields */
  gtk_widget_class_bind_template_child (widget_class, GnalkConnectionDialog, room_url_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkConnectionDialog, display_name_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkConnectionDialog, guest_password_entry);

  /* Common */
  gtk_widget_class_bind_template_child (widget_class, GnalkConnectionDialog, connect_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkConnectionDialog, cancel_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkConnectionDialog, spinner);
  gtk_widget_class_bind_template_child (widget_class, GnalkConnectionDialog, status_label);
}

static void
on_guest_link_clicked (GtkButton              *button,
                       GnalkConnectionDialog *self)
{
  /* Toggle guest mode */
  self->is_guest_mode = !self->is_guest_mode;

  /* Toggle visibility of field groups */
  gtk_widget_set_visible (self->user_mode_group, !self->is_guest_mode);
  gtk_widget_set_visible (self->guest_mode_group, self->is_guest_mode);

  /* Update button labels */
  gtk_button_set_label (GTK_BUTTON (self->connect_button),
                        self->is_guest_mode ? _("Join") : _("Connect"));
  gtk_button_set_label (button,
                        self->is_guest_mode ? _("Or sign in with account") : _("Or join a public room as guest"));

  /* Hide any error message */
  gtk_widget_set_visible (self->status_label, FALSE);
}

static void
gnalk_connection_dialog_init (GnalkConnectionDialog *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->is_guest_mode = FALSE;

  g_signal_connect (self->connect_button, "clicked", G_CALLBACK (on_connect_clicked), self);
  g_signal_connect (self->cancel_button, "clicked", G_CALLBACK (on_cancel_clicked), self);

  /* Connect guest link */
  g_signal_connect (self->guest_link, "clicked", G_CALLBACK (on_guest_link_clicked), self);

  /* User mode entry activation */
  AdwEntryRow *pw_row = ADW_ENTRY_ROW (self->password_entry);
  g_signal_connect (pw_row, "entry-activated", G_CALLBACK (on_entry_activated), self);

  /* Guest mode entry activation */
  AdwEntryRow *guest_pw_row = ADW_ENTRY_ROW (self->guest_password_entry);
  g_signal_connect (guest_pw_row, "entry-activated", G_CALLBACK (on_entry_activated), self);
  AdwEntryRow *display_name_row = ADW_ENTRY_ROW (self->display_name_entry);
  g_signal_connect (display_name_row, "entry-activated", G_CALLBACK (on_entry_activated), self);
}

GnalkConnectionDialog *
gnalk_connection_dialog_new (void)
{
  return g_object_new (GNALK_TYPE_CONNECTION_DIALOG, NULL);
}

void
gnalk_connection_dialog_set_http_client (GnalkConnectionDialog *self,
                                           GnalkHttpClient       *client)
{
  g_return_if_fail (GNALK_IS_CONNECTION_DIALOG (self));

  g_set_object (&self->http_client, client);
}

void
gnalk_connection_dialog_set_defaults (GnalkConnectionDialog *self,
                                        const char             *server_url,
                                        const char             *user_id)
{
  g_return_if_fail (GNALK_IS_CONNECTION_DIALOG (self));

  if (server_url)
    gtk_editable_set_text (GTK_EDITABLE (self->server_entry), server_url);
  if (user_id)
    gtk_editable_set_text (GTK_EDITABLE (self->user_entry), user_id);
}

static void
start_connection (GnalkConnectionDialog *self)
{
  const char *server = gtk_editable_get_text (GTK_EDITABLE (self->server_entry));
  const char *user = gtk_editable_get_text (GTK_EDITABLE (self->user_entry));
  const char *password = gtk_editable_get_text (GTK_EDITABLE (self->password_entry));

  if (!server || !*server || !user || !*user || !password || !*password)
    {
      show_error (self, _("Please fill in all fields"));
      return;
    }

  set_connecting (self, TRUE);
  self->step = 1;

  gnalk_http_client_set_credentials (self->http_client, server, user, password);
  gnalk_http_client_get_server_status (self->http_client, on_server_status_response, self);
}

/* Parse room URL like https://nc.example.com/call/TOKEN or /index.php/call/TOKEN */
static gboolean
parse_room_url (const char  *url,
                char       **out_server,
                char       **out_token)
{
  if (!url || !*url)
    return FALSE;

  /* Parse the URL */
  g_autoptr(GUri) uri = g_uri_parse (url, G_URI_FLAGS_NONE, NULL);
  if (!uri)
    return FALSE;

  const char *scheme = g_uri_get_scheme (uri);
  const char *host = g_uri_get_host (uri);
  int port = g_uri_get_port (uri);
  const char *path = g_uri_get_path (uri);

  if (!scheme || !host || !path)
    return FALSE;

  /* Find /call/ in the path */
  const char *call_pos = strstr (path, "/call/");
  if (!call_pos)
    return FALSE;

  /* Extract token (everything after /call/) */
  const char *token = call_pos + strlen ("/call/");
  if (!token || !*token)
    return FALSE;

  /* Build server URL (without the path) */
  if (port > 0 && port != 443 && port != 80)
    *out_server = g_strdup_printf ("%s://%s:%d", scheme, host, port);
  else
    *out_server = g_strdup_printf ("%s://%s", scheme, host);

  *out_token = g_strdup (token);

  return TRUE;
}

static void
start_guest_connection (GnalkConnectionDialog *self)
{
  const char *room_url = gtk_editable_get_text (GTK_EDITABLE (self->room_url_entry));
  const char *display_name = gtk_editable_get_text (GTK_EDITABLE (self->display_name_entry));

  if (!room_url || !*room_url)
    {
      show_error (self, _("Please enter a room URL"));
      return;
    }

  if (!display_name || !*display_name)
    {
      show_error (self, _("Please enter a display name"));
      return;
    }

  /* Parse the room URL */
  g_autofree char *server_url = NULL;
  g_autofree char *token = NULL;

  if (!parse_room_url (room_url, &server_url, &token))
    {
      show_error (self, _("Invalid room URL. Expected format: https://server.com/call/TOKEN"));
      return;
    }

  /* Store the token for later use */
  g_clear_pointer (&self->room_token, g_free);
  self->room_token = g_strdup (token);

  set_connecting (self, TRUE);
  self->step = 1;

  /* Set server URL for guest mode (no auth) */
  gnalk_http_client_set_guest_mode (self->http_client, server_url, token);

  /* First, get room info to verify it exists and is public */
  gnalk_http_client_get_room_info (self->http_client, token, on_guest_room_info_response, self);
}

static void
on_server_status_response (GnalkHttpClient *client,
                            JsonNode         *response,
                            GError           *error,
                            gpointer          user_data)
{
  GnalkConnectionDialog *self = GNALK_CONNECTION_DIALOG (user_data);

  if (error)
    {
      show_error (self, error->message);
      return;
    }

  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    {
      show_error (self, _("Invalid server response"));
      return;
    }

  JsonObject *obj = json_node_get_object (response);
  gboolean installed = json_object_get_boolean_member_with_default (obj, "installed", FALSE);

  if (!installed)
    {
      show_error (self, _("Nextcloud is not properly installed on this server"));
      return;
    }

  self->step = 2;
  gnalk_http_client_get_user_info (self->http_client, on_user_info_response, self);
}

static void
on_user_info_response (GnalkHttpClient *client,
                        JsonNode         *response,
                        GError           *error,
                        gpointer          user_data)
{
  GnalkConnectionDialog *self = GNALK_CONNECTION_DIALOG (user_data);

  if (error)
    {
      show_error (self, _("Authentication failed. Check your credentials."));
      return;
    }

  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    {
      show_error (self, _("Invalid user info response"));
      return;
    }

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    {
      show_error (self, _("Invalid OCS response"));
      return;
    }

  JsonObject *meta = json_object_get_object_member (ocs, "meta");
  if (meta)
    {
      int status_code = json_object_get_int_member_with_default (meta, "statuscode", 0);
      if (status_code != 100 && status_code != 200)
        {
          const char *message = json_object_get_string_member_with_default (meta, "message", "Unknown error");
          show_error (self, message);
          return;
        }
    }

  JsonObject *data = json_object_get_object_member (ocs, "data");
  if (data)
    {
      const char *display_name = json_object_get_string_member_with_default (data, "displayname", "");

      GtkWindow *win = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self)));
      /* Credentials are stored in Account, not Application */
      (void) win;
    }

  self->step = 3;
  gnalk_http_client_get_conversations (self->http_client, on_conversations_response, self);
}

static JsonNode *conversations_node = NULL;

static void
on_conversations_response (GnalkHttpClient *client,
                            JsonNode         *response,
                            GError           *error,
                            gpointer          user_data)
{
  GnalkConnectionDialog *self = GNALK_CONNECTION_DIALOG (user_data);

  if (error)
    {
      show_error (self, _("Failed to get conversations"));
      return;
    }

  if (response)
    conversations_node = json_node_copy (response);

  self->step = 4;
  gnalk_http_client_get_capabilities (self->http_client, on_capabilities_response, self);
}

static void
on_capabilities_response (GnalkHttpClient *client,
                           JsonNode         *response,
                           GError           *error,
                           gpointer          user_data)
{
  GnalkConnectionDialog *self = GNALK_CONNECTION_DIALOG (user_data);

  if (error)
    {
      g_warning ("Failed to get capabilities: %s", error->message);
    }

  /* Parse notify_push websocket endpoint from capabilities */
  if (response && JSON_NODE_HOLDS_OBJECT (response))
    {
      JsonObject *root = json_node_get_object (response);
      JsonObject *ocs = json_object_get_object_member (root, "ocs");
      if (ocs)
        {
          JsonObject *data = json_object_get_object_member (ocs, "data");
          if (data)
            {
              JsonObject *capabilities = json_object_get_object_member (data, "capabilities");
              if (capabilities)
                {
                  GtkWindow *win = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self)));
                  GApplication *gapp = win ? G_APPLICATION (gtk_window_get_application (win)) : NULL;
                  GnalkApplication *app = GNALK_IS_APPLICATION (gapp) ? GNALK_APPLICATION (gapp) : NULL;

                  JsonObject *notify_push = json_object_get_object_member (capabilities, "notify_push");
                  if (notify_push)
                    {
                      JsonObject *endpoints = json_object_get_object_member (notify_push, "endpoints");
                      if (endpoints)
                        {
                          const char *websocket_url = json_object_get_string_member_with_default (endpoints, "websocket", NULL);
                          if (websocket_url && *websocket_url && app)
                            {
                              gnalk_application_set_push_websocket_url (app, websocket_url);
                              g_message ("Push WebSocket URL: %s", websocket_url);
                            }
                        }
                    }

                  /* Parse chat edit timeout from spreed capabilities */
                  /* Default is 300 seconds (5 minutes) if not specified */
                  gint64 edit_timeout = 300;
                  JsonObject *spreed = json_object_get_object_member (capabilities, "spreed");
                  if (spreed)
                    {
                      JsonObject *config = json_object_get_object_member (spreed, "config");
                      if (config)
                        {
                          JsonObject *chat = json_object_get_object_member (config, "chat");
                          if (chat && json_object_has_member (chat, "max-edit-age"))
                            edit_timeout = json_object_get_int_member (chat, "max-edit-age");
                        }
                    }
                  if (app)
                    {
                      gnalk_application_set_chat_edit_timeout (app, edit_timeout);
                      g_message ("Chat edit timeout: %ld seconds", (long) edit_timeout);

                      /* Delete timeout is 6 hours (21600s) by default */
                      gint64 delete_timeout = 21600;
                      gnalk_application_set_chat_delete_timeout (app, delete_timeout);
                      g_message ("Chat delete timeout: %ld seconds", (long) delete_timeout);
                    }
                }
            }
        }
    }

  self->step = 5;
  gnalk_http_client_get_signaling_settings (self->http_client, on_signaling_response, self);
}

static void
on_signaling_response (GnalkHttpClient *client,
                        JsonNode         *response,
                        GError           *error,
                        gpointer          user_data)
{
  GnalkConnectionDialog *self = GNALK_CONNECTION_DIALOG (user_data);

  if (error)
    {
      g_warning ("Failed to get signaling settings: %s", error->message);
    }

  if (response && JSON_NODE_HOLDS_OBJECT (response))
    {
      JsonObject *root = json_node_get_object (response);
      JsonObject *ocs = json_object_get_object_member (root, "ocs");
      if (ocs)
        {
          JsonObject *data = json_object_get_object_member (ocs, "data");
          if (data)
            {
              const char *server = json_object_get_string_member_with_default (data, "server", "");
              const char *ticket = json_object_get_string_member_with_default (data, "ticket", "");

              const char *stun_url = NULL;
              const char *turn_url = NULL;
              const char *turn_username = NULL;
              const char *turn_credential = NULL;

              JsonArray *stun_arr = json_object_get_array_member (data, "stunservers");
              if (stun_arr && json_array_get_length (stun_arr) > 0)
                {
                  JsonObject *stun_obj = json_array_get_object_element (stun_arr, 0);
                  if (stun_obj)
                    {
                      JsonArray *urls = json_object_get_array_member (stun_obj, "urls");
                      if (urls && json_array_get_length (urls) > 0)
                        stun_url = json_array_get_string_element (urls, 0);
                    }
                }

              JsonArray *turn_arr = json_object_get_array_member (data, "turnservers");
              if (turn_arr && json_array_get_length (turn_arr) > 0)
                {
                  JsonObject *turn_obj = json_array_get_object_element (turn_arr, 0);
                  if (turn_obj)
                    {
                      JsonArray *urls = json_object_get_array_member (turn_obj, "urls");
                      if (urls && json_array_get_length (urls) > 0)
                        turn_url = json_array_get_string_element (urls, 0);
                      turn_username = json_object_get_string_member_with_default (turn_obj, "username", NULL);
                      turn_credential = json_object_get_string_member_with_default (turn_obj, "credential", NULL);
                    }
                }

              GtkWindow *win = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self)));
              if (win)
                {
                  GApplication *app = G_APPLICATION (gtk_window_get_application (win));
                  if (GNALK_IS_APPLICATION (app))
                    {
                      gnalk_application_set_signaling_settings (GNALK_APPLICATION (app), server, ticket);
                      gnalk_application_set_ice_servers (GNALK_APPLICATION (app),
                                                          stun_url, turn_url,
                                                          turn_username, turn_credential);
                      g_print ("ICE servers: STUN=%s, TURN=%s\n",
                               stun_url ? stun_url : "(none)",
                               turn_url ? turn_url : "(none)");
                    }
                }
            }
        }
    }

  set_connecting (self, FALSE);

  g_signal_emit (self, signals[SIGNAL_CONNECTED], 0, conversations_node, response);

  if (conversations_node)
    {
      json_node_unref (conversations_node);
      conversations_node = NULL;
    }

  adw_dialog_close (ADW_DIALOG (self));
}

/* ============ Guest Mode Callbacks ============ */

static JsonNode *guest_room_node = NULL;

static void
on_guest_room_info_response (GnalkHttpClient *client,
                              JsonNode         *response,
                              GError           *error,
                              gpointer          user_data)
{
  GnalkConnectionDialog *self = GNALK_CONNECTION_DIALOG (user_data);

  if (error)
    {
      show_error (self, _("Could not access room. Check the URL."));
      return;
    }

  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    {
      show_error (self, _("Invalid room response"));
      return;
    }

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    {
      show_error (self, _("Invalid OCS response"));
      return;
    }

  JsonObject *meta = json_object_get_object_member (ocs, "meta");
  if (meta)
    {
      int status_code = json_object_get_int_member_with_default (meta, "statuscode", 0);
      if (status_code != 100 && status_code != 200)
        {
          const char *message = json_object_get_string_member_with_default (meta, "message", "Room not found");
          show_error (self, message);
          return;
        }
    }

  JsonObject *data = json_object_get_object_member (ocs, "data");
  if (!data)
    {
      show_error (self, _("Room data not found"));
      return;
    }

  /* Check room type - type 3 is public */
  int room_type = json_object_get_int_member_with_default (data, "type", 0);
  if (room_type != 3)
    {
      show_error (self, _("This room does not allow guest access"));
      return;
    }

  /* Store room info for later */
  if (guest_room_node)
    json_node_unref (guest_room_node);
  guest_room_node = json_node_copy (response);

  /* Check if password is required */
  gboolean has_password = json_object_get_boolean_member_with_default (data, "hasPassword", FALSE);
  const char *entered_password = gtk_editable_get_text (GTK_EDITABLE (self->guest_password_entry));

  self->step = 2;
  gnalk_http_client_join_room_as_guest (self->http_client, self->room_token,
                                          has_password ? entered_password : NULL,
                                          on_guest_join_response, self);
}

static void
on_guest_join_response (GnalkHttpClient *client,
                         JsonNode         *response,
                         GError           *error,
                         gpointer          user_data)
{
  GnalkConnectionDialog *self = GNALK_CONNECTION_DIALOG (user_data);

  if (error)
    {
      show_error (self, _("Failed to join room. Check the password."));
      return;
    }

  /* Check for password error */
  if (response && JSON_NODE_HOLDS_OBJECT (response))
    {
      JsonObject *root = json_node_get_object (response);
      JsonObject *ocs = json_object_get_object_member (root, "ocs");
      if (ocs)
        {
          JsonObject *meta = json_object_get_object_member (ocs, "meta");
          if (meta)
            {
              int status_code = json_object_get_int_member_with_default (meta, "statuscode", 0);
              if (status_code == 403)
                {
                  show_error (self, _("Wrong password"));
                  return;
                }
              if (status_code != 100 && status_code != 200)
                {
                  const char *message = json_object_get_string_member_with_default (meta, "message", "Failed to join");
                  show_error (self, message);
                  return;
                }
            }
        }
    }

  /* Successfully joined, now set display name */
  const char *display_name = gtk_editable_get_text (GTK_EDITABLE (self->display_name_entry));

  self->step = 3;
  gnalk_http_client_set_guest_name (self->http_client, self->room_token,
                                      display_name, on_guest_name_response, self);
}

static void
on_guest_name_response (GnalkHttpClient *client,
                         JsonNode         *response,
                         GError           *error,
                         gpointer          user_data)
{
  GnalkConnectionDialog *self = GNALK_CONNECTION_DIALOG (user_data);

  if (error)
    {
      g_warning ("Failed to set guest name: %s", error->message);
      /* Continue anyway - name setting is not critical */
    }

  /* Store display name in application */
  GtkWindow *win = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self)));
  if (win)
    {
      GApplication *app = G_APPLICATION (gtk_window_get_application (win));
      if (GNALK_IS_APPLICATION (app))
        gnalk_application_set_guest_mode (GNALK_APPLICATION (app), TRUE, self->room_token);
    }

  /* Get signaling settings for calls */
  self->step = 4;
  gnalk_http_client_get_signaling_settings (self->http_client, on_guest_signaling_response, self);
}

static void
on_guest_signaling_response (GnalkHttpClient *client,
                              JsonNode         *response,
                              GError           *error,
                              gpointer          user_data)
{
  GnalkConnectionDialog *self = GNALK_CONNECTION_DIALOG (user_data);

  if (error)
    {
      g_warning ("Failed to get signaling settings: %s", error->message);
    }

  /* Parse signaling settings same as normal flow */
  if (response && JSON_NODE_HOLDS_OBJECT (response))
    {
      JsonObject *root = json_node_get_object (response);
      JsonObject *ocs = json_object_get_object_member (root, "ocs");
      if (ocs)
        {
          JsonObject *data = json_object_get_object_member (ocs, "data");
          if (data)
            {
              const char *server = json_object_get_string_member_with_default (data, "server", "");
              const char *ticket = json_object_get_string_member_with_default (data, "ticket", "");

              const char *stun_url = NULL;
              const char *turn_url = NULL;
              const char *turn_username = NULL;
              const char *turn_credential = NULL;

              JsonArray *stun_arr = json_object_get_array_member (data, "stunservers");
              if (stun_arr && json_array_get_length (stun_arr) > 0)
                {
                  JsonObject *stun_obj = json_array_get_object_element (stun_arr, 0);
                  if (stun_obj)
                    {
                      JsonArray *urls = json_object_get_array_member (stun_obj, "urls");
                      if (urls && json_array_get_length (urls) > 0)
                        stun_url = json_array_get_string_element (urls, 0);
                    }
                }

              JsonArray *turn_arr = json_object_get_array_member (data, "turnservers");
              if (turn_arr && json_array_get_length (turn_arr) > 0)
                {
                  JsonObject *turn_obj = json_array_get_object_element (turn_arr, 0);
                  if (turn_obj)
                    {
                      JsonArray *urls = json_object_get_array_member (turn_obj, "urls");
                      if (urls && json_array_get_length (urls) > 0)
                        turn_url = json_array_get_string_element (urls, 0);
                      turn_username = json_object_get_string_member_with_default (turn_obj, "username", NULL);
                      turn_credential = json_object_get_string_member_with_default (turn_obj, "credential", NULL);
                    }
                }

              GtkWindow *win = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self)));
              if (win)
                {
                  GApplication *app = G_APPLICATION (gtk_window_get_application (win));
                  if (GNALK_IS_APPLICATION (app))
                    {
                      gnalk_application_set_signaling_settings (GNALK_APPLICATION (app), server, ticket);
                      gnalk_application_set_ice_servers (GNALK_APPLICATION (app),
                                                          stun_url, turn_url,
                                                          turn_username, turn_credential);
                    }
                }
            }
        }
    }

  set_connecting (self, FALSE);

  /* Emit connected signal with single room (wrapped as array for compatibility) */
  g_signal_emit (self, signals[SIGNAL_CONNECTED], 0, guest_room_node, response);

  if (guest_room_node)
    {
      json_node_unref (guest_room_node);
      guest_room_node = NULL;
    }

  adw_dialog_close (ADW_DIALOG (self));
}
