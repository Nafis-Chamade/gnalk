#include "gnalk-new-conversation-dialog.h"
#include "gnalk-conversation-row.h"
#include "gnalk-conversation-data.h"

#define SEARCH_DEBOUNCE_MS 500

struct _GnalkNewConversationDialog
{
  AdwDialog parent_instance;

  /* Form fields */
  GtkWidget *name_entry;
  GtkWidget *description_entry;

  /* Avatar picker */
  GtkWidget *avatar_button;
  GtkWidget *avatar_preview;
  GBytes    *selected_avatar_bytes;
  char      *selected_avatar_content_type;

  /* Search section */
  GtkWidget *search_entry;
  GtkWidget *search_results_revealer;
  GtkWidget *search_results_list;
  GListStore *search_results_store;
  GtkNoSelection *search_selection;

  /* Selected members section */
  GtkWidget *members_group;
  GtkWidget *members_list;
  GListStore *members_store;

  /* Buttons */
  GtkWidget *create_button;
  GtkWidget *cancel_button;

  /* Status/feedback */
  GtkWidget *spinner;
  GtkWidget *status_label;

  /* Dependencies */
  GnalkHttpClient  *http_client;
  GnalkAvatarCache *avatar_cache;

  /* State */
  guint  search_debounce_timer_id;
  char  *pending_search_term;
  char  *created_room_token;
  int    pending_participants;
  gboolean avatar_upload_pending;
  gboolean has_federation_outgoing;
};

G_DEFINE_FINAL_TYPE (GnalkNewConversationDialog, gnalk_new_conversation_dialog, ADW_TYPE_DIALOG)

enum {
  SIGNAL_CONVERSATION_CREATED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void start_adding_participants (GnalkNewConversationDialog *self);
static void start_uploading_avatar (GnalkNewConversationDialog *self);
static void finish_creation (GnalkNewConversationDialog *self);

static void
show_error (GnalkNewConversationDialog *self,
            const char                  *message)
{
  gtk_widget_set_visible (self->spinner, FALSE);
  gtk_spinner_set_spinning (GTK_SPINNER (self->spinner), FALSE);
  gtk_label_set_text (GTK_LABEL (self->status_label), message);
  gtk_widget_set_visible (self->status_label, TRUE);
  gtk_widget_set_sensitive (self->create_button, TRUE);
  gtk_widget_set_sensitive (self->name_entry, TRUE);
  gtk_widget_set_sensitive (self->description_entry, TRUE);
  gtk_widget_set_sensitive (self->search_entry, TRUE);
}

static void
set_creating (GnalkNewConversationDialog *self,
              gboolean                     creating)
{
  gtk_widget_set_visible (self->spinner, creating);
  gtk_spinner_set_spinning (GTK_SPINNER (self->spinner), creating);
  gtk_widget_set_visible (self->status_label, FALSE);
  gtk_widget_set_sensitive (self->create_button, !creating);
  gtk_widget_set_sensitive (self->name_entry, !creating);
  gtk_widget_set_sensitive (self->description_entry, !creating);
  gtk_widget_set_sensitive (self->search_entry, !creating);
}

static void
update_create_button_sensitivity (GnalkNewConversationDialog *self)
{
  const char *name = gtk_editable_get_text (GTK_EDITABLE (self->name_entry));
  gboolean can_create = (name && *name);
  gtk_widget_set_sensitive (self->create_button, can_create);
}

/* ============ Member Avatar Loading ============ */

typedef struct {
  GtkWidget *avatar_image;
} MemberAvatarData;

static void
on_member_avatar_loaded (GnalkAvatarCache *cache,
                         GdkTexture        *texture,
                         gpointer           user_data)
{
  MemberAvatarData *data = user_data;

  if (data->avatar_image)
    {
      if (texture)
        gtk_image_set_from_paintable (GTK_IMAGE (data->avatar_image), GDK_PAINTABLE (texture));
      /* Remove the weak pointer on EVERY path where the widget is still alive —
       * doing it only when texture!=NULL left a dangling weak pointer into the
       * freed `data` on the no-texture path (heap corruption). */
      g_object_remove_weak_pointer (G_OBJECT (data->avatar_image), (gpointer *)&data->avatar_image);
    }

  g_free (data);
}

/* ============ Search Handlers ============ */

static void
on_search_list_item_setup (GtkSignalListItemFactory *factory,
                           GtkListItem              *list_item,
                           gpointer                  user_data)
{
  GtkWidget *row = g_object_new (GNALK_TYPE_CONVERSATION_ROW, NULL);
  gtk_list_item_set_child (list_item, row);
}

static void
on_search_list_item_bind (GtkSignalListItemFactory    *factory,
                          GtkListItem                 *list_item,
                          GnalkNewConversationDialog *self)
{
  GnalkConversationRow *row = GNALK_CONVERSATION_ROW (gtk_list_item_get_child (list_item));
  GnalkConversationData *data = gtk_list_item_get_item (list_item);

  gnalk_conversation_row_set_from_data (row, data, self->avatar_cache);
}

static void
on_search_list_item_unbind (GtkSignalListItemFactory *factory,
                            GtkListItem              *list_item,
                            gpointer                  user_data)
{
  GnalkConversationRow *row = GNALK_CONVERSATION_ROW (gtk_list_item_get_child (list_item));
  gnalk_conversation_row_reset (row);
}

static void
on_search_users_response (GnalkHttpClient *client,
                          JsonNode         *response,
                          GError           *error,
                          gpointer          user_data)
{
  GnalkNewConversationDialog *self = GNALK_NEW_CONVERSATION_DIALOG (user_data);

  if (error)
    {
      g_debug ("User search failed: %s", error->message);
      return;
    }

  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    return;

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    return;

  JsonArray *data = json_object_get_array_member (ocs, "data");
  if (!data)
    return;

  g_list_store_remove_all (self->search_results_store);

  guint n_results = json_array_get_length (data);
  for (guint i = 0; i < n_results; i++)
    {
      JsonObject *item = json_array_get_object_element (data, i);
      const char *user_id = json_object_get_string_member_with_default (item, "id", "");
      const char *label = json_object_get_string_member_with_default (item, "label", "");
      const char *source = json_object_get_string_member_with_default (item, "source", "users");
      const char *status = NULL;

      /* Get status if available */
      if (json_object_has_member (item, "status"))
        {
          JsonObject *status_obj = json_object_get_object_member (item, "status");
          if (status_obj)
            status = json_object_get_string_member_with_default (status_obj, "status", NULL);
        }

      /* Check if already in members list */
      gboolean already_added = FALSE;
      guint n_members = g_list_model_get_n_items (G_LIST_MODEL (self->members_store));
      for (guint j = 0; j < n_members; j++)
        {
          GnalkConversationData *member = g_list_model_get_item (G_LIST_MODEL (self->members_store), j);
          if (g_strcmp0 (gnalk_conversation_data_get_actor_id (member), user_id) == 0)
            {
              already_added = TRUE;
              g_object_unref (member);
              break;
            }
          g_object_unref (member);
        }

      if (already_added)
        continue;

      /* Map "remotes" source to "federated_users" for the token encoding */
      const char *token_source = g_strcmp0 (source, "remotes") == 0 ? "federated_users" : source;
      g_autofree char *display_label = NULL;
      if (g_strcmp0 (source, "remotes") == 0)
        display_label = g_strdup_printf ("%s (%s)", label, user_id);

      /* Create search result data */
      g_autoptr(JsonBuilder) builder = json_builder_new ();
      json_builder_begin_object (builder);
      g_autofree char *token_val = g_strdup_printf ("user:%s:%s", token_source, user_id);
      json_builder_set_member_name (builder, "token");
      json_builder_add_string_value (builder, token_val);
      json_builder_set_member_name (builder, "displayName");
      json_builder_add_string_value (builder, display_label ? display_label : label);
      json_builder_set_member_name (builder, "type");
      json_builder_add_int_value (builder, 1);
      json_builder_set_member_name (builder, "actorId");
      json_builder_add_string_value (builder, user_id);
      /* For federated users, set remoteServer so federation badge shows */
      if (g_strcmp0 (source, "remotes") == 0)
        {
          const char *at = strchr (user_id, '@');
          if (at)
            {
              json_builder_set_member_name (builder, "remoteServer");
              json_builder_add_string_value (builder, at + 1);
            }
        }
      if (status)
        {
          json_builder_set_member_name (builder, "status");
          json_builder_add_string_value (builder, status);
        }
      json_builder_set_member_name (builder, "lastMessage");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "message");
      json_builder_add_string_value (builder, "Click to add");
      json_builder_end_object (builder);
      json_builder_end_object (builder);

      g_autoptr(JsonNode) node = json_builder_get_root (builder);
      GnalkConversationData *conv_data = gnalk_conversation_data_new_from_json (json_node_get_object (node), NULL);
      g_list_store_append (self->search_results_store, conv_data);
      g_object_unref (conv_data);
    }

  if (g_list_model_get_n_items (G_LIST_MODEL (self->search_results_store)) > 0)
    gtk_revealer_set_reveal_child (GTK_REVEALER (self->search_results_revealer), TRUE);
}

static gboolean
on_search_debounce_timeout (gpointer user_data)
{
  GnalkNewConversationDialog *self = GNALK_NEW_CONVERSATION_DIALOG (user_data);

  self->search_debounce_timer_id = 0;

  if (!self->pending_search_term || !self->http_client)
    return G_SOURCE_REMOVE;

  gnalk_http_client_search_users (self->http_client,
                                    self->pending_search_term,
                                    self->has_federation_outgoing,
                                    on_search_users_response,
                                    self);

  return G_SOURCE_REMOVE;
}

static void
on_search_changed (GtkSearchEntry              *entry,
                   GnalkNewConversationDialog *self)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));

  g_list_store_remove_all (self->search_results_store);
  gtk_revealer_set_reveal_child (GTK_REVEALER (self->search_results_revealer), FALSE);

  if (self->search_debounce_timer_id > 0)
    {
      g_source_remove (self->search_debounce_timer_id);
      self->search_debounce_timer_id = 0;
    }

  g_free (self->pending_search_term);
  self->pending_search_term = g_strdup (text);

  if (!text || g_utf8_strlen (text, -1) < 2)
    return;

  self->search_debounce_timer_id = g_timeout_add (SEARCH_DEBOUNCE_MS,
                                                   on_search_debounce_timeout, self);
}

static void
on_member_remove_clicked (GtkButton                   *button,
                          GnalkNewConversationDialog *self)
{
  GtkWidget *row = g_object_get_data (G_OBJECT (button), "row");
  if (!row)
    return;

  const char *user_id = g_object_get_data (G_OBJECT (row), "user-id");
  if (!user_id)
    return;

  /* Remove from members store */
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->members_store));
  for (guint i = 0; i < n_items; i++)
    {
      GnalkConversationData *member = g_list_model_get_item (G_LIST_MODEL (self->members_store), i);
      if (g_strcmp0 (gnalk_conversation_data_get_actor_id (member), user_id) == 0)
        {
          g_list_store_remove (self->members_store, i);
          g_object_unref (member);
          break;
        }
      g_object_unref (member);
    }

  /* Remove from UI */
  gtk_list_box_remove (GTK_LIST_BOX (self->members_list), row);

  /* Hide members group if empty */
  if (g_list_model_get_n_items (G_LIST_MODEL (self->members_store)) == 0)
    gtk_widget_set_visible (self->members_group, FALSE);
}

static void
on_search_result_activated (GtkListView                 *list_view,
                            guint                        position,
                            GnalkNewConversationDialog *self)
{
  GnalkConversationData *data = g_list_model_get_item (G_LIST_MODEL (self->search_results_store), position);
  if (!data)
    return;

  const char *token = gnalk_conversation_data_get_token (data);

  /* Parse user:source:userId token */
  if (g_str_has_prefix (token, "user:"))
    {
      /* Extract source from token (user:source:userId) */
      g_auto(GStrv) parts = g_strsplit (token, ":", 3);
      const char *source = (parts && parts[0] && parts[1]) ? parts[1] : "users";

      const char *user_id = gnalk_conversation_data_get_actor_id (data);
      const char *display_name = gnalk_conversation_data_get_title (data);
      const char *user_status = gnalk_conversation_data_get_user_status (data);

      /* Encode source:userId in token so add_participant knows the source */
      g_autofree char *member_token = g_strdup_printf ("%s:%s", source, user_id);

      /* Create member data with status */
      g_autoptr(JsonBuilder) builder = json_builder_new ();
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "token");
      json_builder_add_string_value (builder, member_token);
      json_builder_set_member_name (builder, "displayName");
      json_builder_add_string_value (builder, display_name);
      json_builder_set_member_name (builder, "type");
      json_builder_add_int_value (builder, 1);
      json_builder_set_member_name (builder, "actorId");
      json_builder_add_string_value (builder, user_id);
      if (user_status)
        {
          json_builder_set_member_name (builder, "status");
          json_builder_add_string_value (builder, user_status);
        }
      json_builder_end_object (builder);

      g_autoptr(JsonNode) node = json_builder_get_root (builder);
      GnalkConversationData *member_data = gnalk_conversation_data_new_from_json (json_node_get_object (node), NULL);
      g_list_store_append (self->members_store, member_data);
      g_object_unref (member_data);

      /* Show members group */
      gtk_widget_set_visible (self->members_group, TRUE);

      /* Create member row with avatar, name, status, and remove button */
      GtkWidget *row = gtk_list_box_row_new ();
      gtk_widget_set_margin_top (row, 6);
      gtk_widget_set_margin_bottom (row, 6);
      gtk_widget_set_margin_start (row, 12);
      gtk_widget_set_margin_end (row, 12);

      GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), hbox);

      /* Create avatar with status overlay */
      GtkWidget *overlay = gtk_overlay_new ();
      gtk_widget_set_size_request (overlay, 32, 32);
      gtk_widget_set_valign (overlay, GTK_ALIGN_CENTER);

      GtkWidget *avatar = gtk_image_new_from_icon_name ("avatar-default-symbolic");
      gtk_image_set_pixel_size (GTK_IMAGE (avatar), 32);
      gtk_widget_add_css_class (avatar, "avatar-round");
      gtk_overlay_set_child (GTK_OVERLAY (overlay), avatar);

      /* Load user avatar */
      if (self->avatar_cache)
        {
          MemberAvatarData *avatar_data = g_new0 (MemberAvatarData, 1);
          avatar_data->avatar_image = avatar;
          g_object_add_weak_pointer (G_OBJECT (avatar), (gpointer *)&avatar_data->avatar_image);
          gnalk_avatar_cache_get_user_avatar (self->avatar_cache, user_id, 64,
                                                on_member_avatar_loaded, avatar_data);
        }

      /* Add status icon - only create if we have a known status */
      if (user_status && *user_status)
        {
          const char *icon_name = NULL;
          const char *css_class = NULL;

          if (g_strcmp0 (user_status, "online") == 0)
            {
              icon_name = "user-available-symbolic";
              css_class = "status-online";
            }
          else if (g_strcmp0 (user_status, "away") == 0)
            {
              icon_name = "user-away-symbolic";
              css_class = "status-away";
            }
          else if (g_strcmp0 (user_status, "dnd") == 0)
            {
              icon_name = "user-busy-symbolic";
              css_class = "status-dnd";
            }

          if (icon_name)
            {
              GtkWidget *status_icon = gtk_image_new_from_icon_name (icon_name);
              gtk_image_set_pixel_size (GTK_IMAGE (status_icon), 12);
              gtk_widget_set_halign (status_icon, GTK_ALIGN_END);
              gtk_widget_set_valign (status_icon, GTK_ALIGN_END);
              gtk_widget_add_css_class (status_icon, css_class);
              gtk_overlay_add_overlay (GTK_OVERLAY (overlay), status_icon);
            }
        }

      gtk_box_append (GTK_BOX (hbox), overlay);

      /* Name label */
      GtkWidget *name_label = gtk_label_new (display_name ? display_name : "");
      gtk_widget_set_hexpand (name_label, TRUE);
      gtk_widget_set_halign (name_label, GTK_ALIGN_START);
      gtk_widget_set_valign (name_label, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (hbox), name_label);

      /* Remove button */
      GtkWidget *remove_button = gtk_button_new_from_icon_name ("window-close-symbolic");
      gtk_widget_set_valign (remove_button, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (remove_button, "flat");
      gtk_widget_add_css_class (remove_button, "circular");
      g_object_set_data_full (G_OBJECT (row), "user-id", g_strdup (user_id), g_free);
      g_signal_connect (remove_button, "clicked", G_CALLBACK (on_member_remove_clicked), self);
      g_object_set_data (G_OBJECT (remove_button), "row", row);

      gtk_box_append (GTK_BOX (hbox), remove_button);

      gtk_list_box_append (GTK_LIST_BOX (self->members_list), row);
    }

  g_object_unref (data);

  /* Clear search */
  gtk_editable_set_text (GTK_EDITABLE (self->search_entry), "");
  gtk_revealer_set_reveal_child (GTK_REVEALER (self->search_results_revealer), FALSE);
}

/* ============ Creation Flow ============ */

static void
on_add_participant_response (GnalkHttpClient *client,
                             JsonNode         *response,
                             GError           *error,
                             gpointer          user_data)
{
  GnalkNewConversationDialog *self = user_data;  /* owns a ref */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) { g_object_unref (self); return; }

  if (error)
    g_warning ("Failed to add participant: %s", error->message);

  self->pending_participants--;

  if (self->pending_participants == 0)
    finish_creation (self);

  g_object_unref (self);
}

static void
start_adding_participants (GnalkNewConversationDialog *self)
{
  guint n_members = g_list_model_get_n_items (G_LIST_MODEL (self->members_store));

  if (n_members == 0)
    {
      finish_creation (self);
      return;
    }

  self->pending_participants = n_members;

  for (guint i = 0; i < n_members; i++)
    {
      GnalkConversationData *member = g_list_model_get_item (G_LIST_MODEL (self->members_store), i);
      const char *user_id = gnalk_conversation_data_get_actor_id (member);

      /* Token is encoded as source:userId — parse to get source for add_participant */
      const char *member_token = gnalk_conversation_data_get_token (member);
      const char *source = "users";
      if (member_token)
        {
          const char *colon = strchr (member_token, ':');
          if (colon)
            {
              g_autofree char *parsed_source = g_strndup (member_token, colon - member_token);
              /* Pass the parsed source; for federated_users the API expects that value */
              gnalk_http_client_add_participant (self->http_client,
                                                   self->created_room_token,
                                                   user_id,
                                                   parsed_source,
                                                   on_add_participant_response,
                                                   g_object_ref (self));
              g_object_unref (member);
              continue;
            }
        }

      gnalk_http_client_add_participant (self->http_client,
                                           self->created_room_token,
                                           user_id,
                                           source,
                                           on_add_participant_response,
                                           g_object_ref (self));
      g_object_unref (member);
    }
}

static void
finish_creation (GnalkNewConversationDialog *self)
{
  set_creating (self, FALSE);
  g_signal_emit (self, signals[SIGNAL_CONVERSATION_CREATED], 0, self->created_room_token);
  adw_dialog_close (ADW_DIALOG (self));
}

static void
on_avatar_upload_response (GnalkHttpClient *client G_GNUC_UNUSED,
                           gboolean          result,
                           GError           *error,
                           gpointer          user_data)
{
  GnalkNewConversationDialog *self = GNALK_NEW_CONVERSATION_DIALOG (user_data);

  if (error)
    g_warning ("Failed to upload avatar: %s", error->message);
  else if (!result)
    g_warning ("Avatar upload returned false");

  /* Continue to adding participants regardless of avatar result */
  start_adding_participants (self);
}

static void
start_uploading_avatar (GnalkNewConversationDialog *self)
{
  if (!self->selected_avatar_bytes)
    {
      start_adding_participants (self);
      return;
    }

  gnalk_http_client_upload_room_avatar (self->http_client,
                                          self->created_room_token,
                                          self->selected_avatar_bytes,
                                          self->selected_avatar_content_type,
                                          on_avatar_upload_response,
                                          self);
}

static void
on_set_description_response (GnalkHttpClient *client G_GNUC_UNUSED,
                             JsonNode         *response G_GNUC_UNUSED,
                             GError           *error,
                             gpointer          user_data)
{
  GnalkNewConversationDialog *self = GNALK_NEW_CONVERSATION_DIALOG (user_data);

  if (error)
    g_warning ("Failed to set description: %s", error->message);

  /* Continue to uploading avatar (or adding participants if no avatar) */
  start_uploading_avatar (self);
}

static void
on_create_room_response (GnalkHttpClient *client,
                         JsonNode         *response,
                         GError           *error,
                         gpointer          user_data)
{
  GnalkNewConversationDialog *self = user_data;  /* owns a ref */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) { g_object_unref (self); return; }

  if (error)
    {
      show_error (self, error->message);
      g_object_unref (self);
      return;
    }

  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    {
      show_error (self, "Invalid response from server");
      g_object_unref (self);
      return;
    }

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    {
      show_error (self, "Invalid response format");
      g_object_unref (self);
      return;
    }

  JsonObject *data = json_object_get_object_member (ocs, "data");
  if (!data)
    {
      show_error (self, "No room data in response");
      g_object_unref (self);
      return;
    }

  const char *token = json_object_get_string_member_with_default (data, "token", NULL);
  if (!token)
    {
      show_error (self, "No room token in response");
      g_object_unref (self);
      return;
    }

  g_free (self->created_room_token);
  self->created_room_token = g_strdup (token);

  /* Set description if provided */
  const char *description = gtk_editable_get_text (GTK_EDITABLE (self->description_entry));
  if (description && *description)
    {
      gnalk_http_client_set_room_description (self->http_client,
                                                token,
                                                description,
                                                on_set_description_response,
                                                self);
    }
  else
    {
      /* Skip to uploading avatar (or adding participants if no avatar) */
      start_uploading_avatar (self);
    }

  g_object_unref (self);
}

static void
on_create_clicked (GtkButton                   *button,
                   GnalkNewConversationDialog *self)
{
  const char *name = gtk_editable_get_text (GTK_EDITABLE (self->name_entry));

  if (!name || !*name)
    {
      show_error (self, "Please enter a group name");
      return;
    }

  if (!self->http_client)
    {
      show_error (self, "Not connected");
      return;
    }

  set_creating (self, TRUE);

  /* Create group conversation (roomType=2), no invite, with name */
  gnalk_http_client_create_conversation (self->http_client,
                                           2,    /* roomType = group */
                                           "",   /* no initial invite */
                                           name,
                                           on_create_room_response,
                                           g_object_ref (self));
}

static void
on_cancel_clicked (GtkButton                   *button,
                   GnalkNewConversationDialog *self)
{
  adw_dialog_close (ADW_DIALOG (self));
}

static void
on_name_changed (GtkEditable                 *editable G_GNUC_UNUSED,
                 GnalkNewConversationDialog *self)
{
  update_create_button_sensitivity (self);
}

static void
on_avatar_file_opened (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
  GnalkNewConversationDialog *self = GNALK_NEW_CONVERSATION_DIALOG (user_data);

  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = gtk_file_dialog_open_finish (dialog, result, &error);

  if (!file)
    {
      if (!g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
        g_warning ("Failed to open file: %s", error->message);
      return;
    }

  /* Load file contents */
  g_autoptr(GError) load_error = NULL;
  char *contents = NULL;
  gsize length = 0;

  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, &load_error))
    {
      g_warning ("Failed to load file: %s", load_error->message);
      return;
    }

  /* Store the bytes */
  g_clear_pointer (&self->selected_avatar_bytes, g_bytes_unref);
  self->selected_avatar_bytes = g_bytes_new_take (contents, length);

  /* Determine content type */
  g_free (self->selected_avatar_content_type);
  g_autofree char *basename = g_file_get_basename (file);
  if (g_str_has_suffix (basename, ".png"))
    self->selected_avatar_content_type = g_strdup ("image/png");
  else if (g_str_has_suffix (basename, ".jpg") || g_str_has_suffix (basename, ".jpeg"))
    self->selected_avatar_content_type = g_strdup ("image/jpeg");
  else if (g_str_has_suffix (basename, ".gif"))
    self->selected_avatar_content_type = g_strdup ("image/gif");
  else if (g_str_has_suffix (basename, ".webp"))
    self->selected_avatar_content_type = g_strdup ("image/webp");
  else
    self->selected_avatar_content_type = g_strdup ("image/png");

  /* Update preview with the selected image */
  g_autoptr(GdkTexture) texture = gdk_texture_new_from_bytes (self->selected_avatar_bytes, NULL);
  if (texture)
    gtk_image_set_from_paintable (GTK_IMAGE (self->avatar_preview), GDK_PAINTABLE (texture));
  else
    gtk_image_set_from_icon_name (GTK_IMAGE (self->avatar_preview), "emblem-ok-symbolic");
}

static void
on_avatar_button_clicked (GtkButton                   *button G_GNUC_UNUSED,
                          GnalkNewConversationDialog *self)
{
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, "Select Avatar");

  /* Set up file filter for images */
  GtkFileFilter *filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (filter, "Images");
  gtk_file_filter_add_mime_type (filter, "image/*");

  g_autoptr(GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  g_list_store_append (filters, filter);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_default_filter (dialog, filter);
  g_object_unref (filter);

  gtk_file_dialog_open (dialog, GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))),
                        NULL, on_avatar_file_opened, self);
  g_object_unref (dialog);
}

/* ============ GObject ============ */

static void
gnalk_new_conversation_dialog_finalize (GObject *object)
{
  GnalkNewConversationDialog *self = GNALK_NEW_CONVERSATION_DIALOG (object);

  if (self->search_debounce_timer_id > 0)
    g_source_remove (self->search_debounce_timer_id);

  g_free (self->pending_search_term);
  g_free (self->created_room_token);
  g_free (self->selected_avatar_content_type);
  g_clear_pointer (&self->selected_avatar_bytes, g_bytes_unref);

  g_clear_object (&self->search_results_store);
  g_clear_object (&self->members_store);
  g_clear_object (&self->http_client);
  g_clear_object (&self->avatar_cache);

  G_OBJECT_CLASS (gnalk_new_conversation_dialog_parent_class)->finalize (object);
}

static void
gnalk_new_conversation_dialog_class_init (GnalkNewConversationDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = gnalk_new_conversation_dialog_finalize;

  signals[SIGNAL_CONVERSATION_CREATED] =
    g_signal_new ("conversation-created",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-new-conversation-dialog.ui");

  gtk_widget_class_bind_template_child (widget_class, GnalkNewConversationDialog, avatar_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkNewConversationDialog, avatar_preview);
  gtk_widget_class_bind_template_child (widget_class, GnalkNewConversationDialog, name_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkNewConversationDialog, description_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkNewConversationDialog, search_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkNewConversationDialog, search_results_revealer);
  gtk_widget_class_bind_template_child (widget_class, GnalkNewConversationDialog, search_results_list);
  gtk_widget_class_bind_template_child (widget_class, GnalkNewConversationDialog, members_group);
  gtk_widget_class_bind_template_child (widget_class, GnalkNewConversationDialog, members_list);
  gtk_widget_class_bind_template_child (widget_class, GnalkNewConversationDialog, create_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkNewConversationDialog, cancel_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkNewConversationDialog, spinner);
  gtk_widget_class_bind_template_child (widget_class, GnalkNewConversationDialog, status_label);
}

static void
gnalk_new_conversation_dialog_init (GnalkNewConversationDialog *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  /* Initialize stores */
  self->search_results_store = g_list_store_new (GNALK_TYPE_CONVERSATION_DATA);
  self->members_store = g_list_store_new (GNALK_TYPE_CONVERSATION_DATA);

  /* Set up search results list */
  self->search_selection = gtk_no_selection_new (G_LIST_MODEL (g_object_ref (self->search_results_store)));
  GtkListItemFactory *search_factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (search_factory, "setup", G_CALLBACK (on_search_list_item_setup), self);
  g_signal_connect (search_factory, "bind", G_CALLBACK (on_search_list_item_bind), self);
  g_signal_connect (search_factory, "unbind", G_CALLBACK (on_search_list_item_unbind), self);

  gtk_list_view_set_model (GTK_LIST_VIEW (self->search_results_list), GTK_SELECTION_MODEL (self->search_selection));
  gtk_list_view_set_factory (GTK_LIST_VIEW (self->search_results_list), search_factory);

  /* Connect signals */
  g_signal_connect (self->avatar_button, "clicked", G_CALLBACK (on_avatar_button_clicked), self);
  g_signal_connect (self->create_button, "clicked", G_CALLBACK (on_create_clicked), self);
  g_signal_connect (self->cancel_button, "clicked", G_CALLBACK (on_cancel_clicked), self);
  g_signal_connect (self->search_entry, "search-changed", G_CALLBACK (on_search_changed), self);
  g_signal_connect (self->search_results_list, "activate", G_CALLBACK (on_search_result_activated), self);
  g_signal_connect (self->name_entry, "changed", G_CALLBACK (on_name_changed), self);
}

/* ============ Public API ============ */

GnalkNewConversationDialog *
gnalk_new_conversation_dialog_new (void)
{
  return g_object_new (GNALK_TYPE_NEW_CONVERSATION_DIALOG, NULL);
}

void
gnalk_new_conversation_dialog_set_http_client (GnalkNewConversationDialog *self,
                                                 GnalkHttpClient            *client)
{
  g_return_if_fail (GNALK_IS_NEW_CONVERSATION_DIALOG (self));

  g_set_object (&self->http_client, client);
}

void
gnalk_new_conversation_dialog_set_avatar_cache (GnalkNewConversationDialog *self,
                                                  GnalkAvatarCache           *cache)
{
  g_return_if_fail (GNALK_IS_NEW_CONVERSATION_DIALOG (self));

  g_set_object (&self->avatar_cache, cache);
}

void
gnalk_new_conversation_dialog_set_federation_outgoing (GnalkNewConversationDialog *self,
                                                         gboolean                     enabled)
{
  g_return_if_fail (GNALK_IS_NEW_CONVERSATION_DIALOG (self));

  self->has_federation_outgoing = enabled;
}
