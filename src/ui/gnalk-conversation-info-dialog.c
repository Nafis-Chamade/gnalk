#include "gnalk-conversation-info-dialog.h"
#include "gnalk-markdown.h"

struct _GnalkConversationInfoDialog
{
  AdwDialog parent_instance;

  /* Navigation */
  AdwNavigationView *navigation_view;
  AdwNavigationPage *info_page;
  AdwNavigationPage *settings_page;

  /* Info page widgets */
  GtkWidget *info_avatar;
  GtkWidget *info_name;
  GtkWidget *info_description;
  GtkWidget *settings_button;
  GtkWidget *media_group;
  GtkWidget *media_flowbox;
  GtkWidget *media_empty_label;
  GtkWidget *participants_group;
  GtkWidget *participants_listbox;
  GtkWidget *participants_spinner;

  /* Settings page widgets */
  GtkWidget *conversation_settings_group;
  GtkWidget *settings_name_entry;
  GtkWidget *settings_description_entry;
  GtkWidget *settings_avatar_row;
  GtkWidget *change_avatar_button;
  GtkWidget *notification_level_row;
  GtkWidget *call_notifications_row;
  GtkWidget *save_button;
  GtkWidget *settings_spinner;

  /* Data */
  GnalkConversationData *conversation;
  GnalkHttpClient       *http_client;
  GnalkAvatarCache      *avatar_cache;

  /* State for settings changes */
  GBytes *new_avatar_bytes;
  char   *new_avatar_content_type;
  gboolean saving;
  gint     pending_saves;

  /* Context menu for participants */
  GtkWidget *context_popover;
  GtkWidget *chat_button;
  GtkWidget *promote_button;
  GtkWidget *demote_button;
  GtkWidget *remove_button;

  /* Context for current action */
  char   *context_actor_id;
  gint64  context_attendee_id;
  gint    context_participant_type;

  /* Current user info */
  char   *my_user_id;
};

G_DEFINE_FINAL_TYPE (GnalkConversationInfoDialog, gnalk_conversation_info_dialog, ADW_TYPE_DIALOG)

enum {
  SIGNAL_MEDIA_CLICKED,
  SIGNAL_CHAT_REQUESTED,
  SIGNAL_PROMOTE_REQUESTED,
  SIGNAL_DEMOTE_REQUESTED,
  SIGNAL_REMOVE_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void load_shared_media (GnalkConversationInfoDialog *self);
static void load_participants (GnalkConversationInfoDialog *self);
static void update_info_page (GnalkConversationInfoDialog *self);
static void update_settings_page (GnalkConversationInfoDialog *self);

static void
on_settings_button_clicked (GtkButton                    *button G_GNUC_UNUSED,
                            GnalkConversationInfoDialog *self)
{
  update_settings_page (self);
  adw_navigation_view_push_by_tag (self->navigation_view, "settings");
}

static void
on_save_complete (GnalkConversationInfoDialog *self)
{
  self->pending_saves--;

  if (self->pending_saves <= 0)
    {
      self->saving = FALSE;
      gtk_widget_set_visible (self->settings_spinner, FALSE);
      gtk_spinner_set_spinning (GTK_SPINNER (self->settings_spinner), FALSE);
      gtk_widget_set_sensitive (self->save_button, TRUE);

      /* Go back to info page */
      adw_navigation_view_pop (self->navigation_view);
    }
}

static void
on_name_saved (GnalkHttpClient *client G_GNUC_UNUSED,
               JsonNode         *response G_GNUC_UNUSED,
               GError           *error,
               gpointer          user_data)
{
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (user_data);

  if (error)
    g_warning ("Failed to save name: %s", error->message);

  on_save_complete (self);
}

static void
on_description_saved (GnalkHttpClient *client G_GNUC_UNUSED,
                      JsonNode         *response G_GNUC_UNUSED,
                      GError           *error,
                      gpointer          user_data)
{
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (user_data);

  if (error)
    g_warning ("Failed to save description: %s", error->message);

  on_save_complete (self);
}

static void
on_avatar_saved (GnalkHttpClient *client G_GNUC_UNUSED,
                 gboolean          result,
                 GError           *error,
                 gpointer          user_data)
{
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (user_data);

  if (error)
    g_warning ("Failed to save avatar: %s", error->message);
  else if (!result)
    g_warning ("Avatar save returned false");

  on_save_complete (self);
}

static void
on_notification_level_saved (GnalkHttpClient *client G_GNUC_UNUSED,
                             JsonNode         *response G_GNUC_UNUSED,
                             GError           *error,
                             gpointer          user_data)
{
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (user_data);

  if (error)
    g_warning ("Failed to save notification level: %s", error->message);

  on_save_complete (self);
}

static void
on_call_notifications_saved (GnalkHttpClient *client G_GNUC_UNUSED,
                             JsonNode         *response G_GNUC_UNUSED,
                             GError           *error,
                             gpointer          user_data)
{
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (user_data);

  if (error)
    g_warning ("Failed to save call notifications: %s", error->message);

  on_save_complete (self);
}

static void
on_save_button_clicked (GtkButton                    *button G_GNUC_UNUSED,
                        GnalkConversationInfoDialog *self)
{
  if (!self->conversation || !self->http_client || self->saving)
    return;

  const char *token = gnalk_conversation_data_get_token (self->conversation);
  if (!token)
    return;

  self->saving = TRUE;
  self->pending_saves = 0;

  gtk_widget_set_visible (self->settings_spinner, TRUE);
  gtk_spinner_set_spinning (GTK_SPINNER (self->settings_spinner), TRUE);
  gtk_widget_set_sensitive (self->save_button, FALSE);

  gint room_type = gnalk_conversation_data_get_room_type (self->conversation);
  gboolean can_moderate = gnalk_conversation_data_can_moderate (self->conversation);
  gboolean is_group = (room_type == 2 || room_type == 3);

  /* Save name if changed (group rooms + moderator) */
  if (is_group && can_moderate)
    {
      const char *new_name = gtk_editable_get_text (GTK_EDITABLE (self->settings_name_entry));
      const char *old_name = gnalk_conversation_data_get_title (self->conversation);

      if (g_strcmp0 (new_name, old_name) != 0)
        {
          self->pending_saves++;
          gnalk_http_client_set_room_name (self->http_client, token, new_name,
                                             on_name_saved, self);
        }

      /* Save description if changed */
      const char *new_desc = gtk_editable_get_text (GTK_EDITABLE (self->settings_description_entry));
      const char *old_desc = gnalk_conversation_data_get_description (self->conversation);

      if (g_strcmp0 (new_desc, old_desc) != 0)
        {
          self->pending_saves++;
          gnalk_http_client_set_room_description (self->http_client, token, new_desc,
                                                    on_description_saved, self);
        }

      /* Save avatar if changed */
      if (self->new_avatar_bytes)
        {
          self->pending_saves++;
          gnalk_http_client_upload_room_avatar (self->http_client, token,
                                                  self->new_avatar_bytes,
                                                  self->new_avatar_content_type,
                                                  on_avatar_saved, self);
        }
    }

  /* Save notification level */
  guint selected = adw_combo_row_get_selected (ADW_COMBO_ROW (self->notification_level_row));
  gint new_level;
  switch (selected)
    {
    case 0: new_level = 1; break;  /* All messages */
    case 1: new_level = 2; break;  /* Mentions only */
    case 2: new_level = 3; break;  /* Off */
    default: new_level = 1; break;
    }

  gint old_level = gnalk_conversation_data_get_notification_level (self->conversation);
  if (new_level != old_level)
    {
      self->pending_saves++;
      gnalk_http_client_set_notification_level (self->http_client, token, new_level,
                                                  on_notification_level_saved, self);
    }

  /* Save call notifications */
  gboolean calls_on = adw_switch_row_get_active (ADW_SWITCH_ROW (self->call_notifications_row));
  gint new_calls = calls_on ? 1 : 0;
  gint old_calls = gnalk_conversation_data_get_call_notification_level (self->conversation);

  if (new_calls != old_calls)
    {
      self->pending_saves++;
      gnalk_http_client_set_call_notifications (self->http_client, token, new_calls,
                                                  on_call_notifications_saved, self);
    }

  /* If nothing to save, just go back */
  if (self->pending_saves == 0)
    {
      self->saving = FALSE;
      gtk_widget_set_visible (self->settings_spinner, FALSE);
      gtk_widget_set_sensitive (self->save_button, TRUE);
      adw_navigation_view_pop (self->navigation_view);
    }
}

static void
on_avatar_file_opened (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (user_data);

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
  g_clear_pointer (&self->new_avatar_bytes, g_bytes_unref);
  self->new_avatar_bytes = g_bytes_new_take (contents, length);

  /* Determine content type */
  g_free (self->new_avatar_content_type);
  g_autofree char *basename = g_file_get_basename (file);
  if (g_str_has_suffix (basename, ".png"))
    self->new_avatar_content_type = g_strdup ("image/png");
  else if (g_str_has_suffix (basename, ".jpg") || g_str_has_suffix (basename, ".jpeg"))
    self->new_avatar_content_type = g_strdup ("image/jpeg");
  else if (g_str_has_suffix (basename, ".gif"))
    self->new_avatar_content_type = g_strdup ("image/gif");
  else if (g_str_has_suffix (basename, ".webp"))
    self->new_avatar_content_type = g_strdup ("image/webp");
  else
    self->new_avatar_content_type = g_strdup ("image/png");

  /* Update the info avatar preview */
  g_autoptr(GdkTexture) texture = gdk_texture_new_from_bytes (self->new_avatar_bytes, NULL);
  if (texture)
    gtk_image_set_from_paintable (GTK_IMAGE (self->info_avatar), GDK_PAINTABLE (texture));
}

static void
on_change_avatar_clicked (GtkButton                    *button G_GNUC_UNUSED,
                          GnalkConversationInfoDialog *self)
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

static void
on_room_avatar_loaded (GnalkAvatarCache *cache G_GNUC_UNUSED,
                       GdkTexture        *texture,
                       gpointer           user_data)
{
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (user_data);

  if (texture)
    gtk_image_set_from_paintable (GTK_IMAGE (self->info_avatar), GDK_PAINTABLE (texture));
}

static void
update_info_page (GnalkConversationInfoDialog *self)
{
  if (!self->conversation)
    return;

  const char *name = gnalk_conversation_data_get_title (self->conversation);
  const char *description = gnalk_conversation_data_get_description (self->conversation);
  gint room_type = gnalk_conversation_data_get_room_type (self->conversation);

  gtk_label_set_text (GTK_LABEL (self->info_name), name ? name : "");

  /* Only show description for group rooms */
  gboolean is_group = (room_type == 2 || room_type == 3);
  if (is_group && description && *description)
    {
      g_autofree char *markup = gnalk_markdown_to_pango (description, NULL);
      gtk_label_set_markup (GTK_LABEL (self->info_description), markup);
      gtk_widget_set_visible (self->info_description, TRUE);
    }
  else
    {
      gtk_widget_set_visible (self->info_description, FALSE);
    }

  /* Load avatar - use user avatar for 1:1, room avatar for groups */
  if (self->avatar_cache)
    {
      if (room_type == 1)
        {
          /* 1:1 conversation - use user avatar */
          const char *actor_id = gnalk_conversation_data_get_actor_id (self->conversation);
          if (actor_id && *actor_id)
            gnalk_avatar_cache_get_user_avatar (self->avatar_cache, actor_id, 96,
                                                  on_room_avatar_loaded, self);
        }
      else
        {
          /* Group conversation - use room avatar */
          const char *token = gnalk_conversation_data_get_token (self->conversation);
          gnalk_avatar_cache_get_room_avatar (self->avatar_cache, token,
                                                on_room_avatar_loaded, self);
        }
    }

  /* Load shared media */
  load_shared_media (self);

  /* Load participants */
  load_participants (self);
}

static void
update_settings_page (GnalkConversationInfoDialog *self)
{
  if (!self->conversation)
    return;

  gint room_type = gnalk_conversation_data_get_room_type (self->conversation);
  gboolean can_moderate = gnalk_conversation_data_can_moderate (self->conversation);
  gboolean is_group = (room_type == 2 || room_type == 3);

  /* Show conversation settings only for group rooms with moderator permission */
  gtk_widget_set_visible (self->conversation_settings_group, is_group && can_moderate);

  if (is_group && can_moderate)
    {
      const char *name = gnalk_conversation_data_get_title (self->conversation);
      const char *description = gnalk_conversation_data_get_description (self->conversation);

      gtk_editable_set_text (GTK_EDITABLE (self->settings_name_entry), name ? name : "");
      gtk_editable_set_text (GTK_EDITABLE (self->settings_description_entry), description ? description : "");
    }

  /* Notification level: 0=default, 1=all, 2=mentions, 3=never */
  gint level = gnalk_conversation_data_get_notification_level (self->conversation);
  guint selected;
  switch (level)
    {
    case 1: selected = 0; break;  /* All messages */
    case 2: selected = 1; break;  /* Mentions only */
    case 3: selected = 2; break;  /* Off */
    default: selected = 0; break; /* Default to all */
    }
  adw_combo_row_set_selected (ADW_COMBO_ROW (self->notification_level_row), selected);

  /* Call notifications: 0=off, 1=on */
  gint call_level = gnalk_conversation_data_get_call_notification_level (self->conversation);
  adw_switch_row_set_active (ADW_SWITCH_ROW (self->call_notifications_row), call_level != 0);

  /* Clear any pending avatar change */
  g_clear_pointer (&self->new_avatar_bytes, g_bytes_unref);
  g_clear_pointer (&self->new_avatar_content_type, g_free);
}

typedef struct {
  GnalkConversationInfoDialog *dialog;
  GtkWidget *image;
} MediaPreviewData;

/* Data for media item click handling */
typedef struct {
  char *file_id;
  char *file_name;
  char *file_path;
  char *file_mimetype;
  GnalkConversationInfoDialog *dialog;
} MediaItemData;

static void
media_item_data_free (gpointer data)
{
  MediaItemData *item = data;
  g_free (item->file_id);
  g_free (item->file_name);
  g_free (item->file_path);
  g_free (item->file_mimetype);
  g_free (item);
}

static void
on_media_item_clicked (GtkButton *button G_GNUC_UNUSED,
                       gpointer   user_data)
{
  MediaItemData *item = user_data;

  if (!item->dialog)
    return;

  /* Emit signal for the window to handle (open gallery for images, download for others) */
  g_signal_emit (item->dialog, signals[SIGNAL_MEDIA_CLICKED], 0,
                 item->file_id, item->file_mimetype, item->file_path, item->file_name);
}

static void
on_media_preview_loaded (GnalkHttpClient *client G_GNUC_UNUSED,
                         GBytes           *bytes,
                         const char       *content_type G_GNUC_UNUSED,
                         GError           *error,
                         gpointer          user_data)
{
  MediaPreviewData *data = user_data;

  if (error || !bytes)
    {
      g_free (data);
      return;
    }

  g_autoptr(GdkTexture) texture = gdk_texture_new_from_bytes (bytes, NULL);
  if (texture && data->image)
    gtk_image_set_from_paintable (GTK_IMAGE (data->image), GDK_PAINTABLE (texture));

  g_free (data);
}

static void
on_shared_media_response (GnalkHttpClient *client,
                          JsonNode         *response,
                          GError           *error,
                          gpointer          user_data)
{
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (user_data);

  /* Clear existing items */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->media_flowbox)) != NULL)
    gtk_flow_box_remove (GTK_FLOW_BOX (self->media_flowbox), child);

  if (error)
    {
      g_debug ("Failed to load shared media: %s", error->message);
      gtk_widget_set_visible (self->media_empty_label, TRUE);
      return;
    }

  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    {
      gtk_widget_set_visible (self->media_empty_label, TRUE);
      return;
    }

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    {
      gtk_widget_set_visible (self->media_empty_label, TRUE);
      return;
    }

  JsonObject *data = json_object_get_object_member (ocs, "data");
  if (!data)
    {
      gtk_widget_set_visible (self->media_empty_label, TRUE);
      return;
    }

  /* data is an object with object_type keys, each containing an array */
  JsonArray *media_array = NULL;
  if (json_object_has_member (data, "media"))
    media_array = json_object_get_array_member (data, "media");

  if (!media_array || json_array_get_length (media_array) == 0)
    {
      gtk_widget_set_visible (self->media_empty_label, TRUE);
      return;
    }

  gtk_widget_set_visible (self->media_empty_label, FALSE);

  guint n_items = MIN (json_array_get_length (media_array), 12);
  for (guint i = 0; i < n_items; i++)
    {
      JsonObject *item = json_array_get_object_element (media_array, i);
      if (!item)
        continue;

      /* Get messageParameters to find file info */
      if (!json_object_has_member (item, "messageParameters"))
        continue;

      JsonObject *params = json_object_get_object_member (item, "messageParameters");
      if (!params)
        continue;

      /* Find the file parameter (usually "file" but could be "object0" etc.) */
      const char *file_id = NULL;
      const char *file_name = NULL;
      const char *file_path = NULL;
      const char *file_mimetype = NULL;
      JsonObjectIter iter;
      const char *key;
      JsonNode *node;

      json_object_iter_init (&iter, params);
      while (json_object_iter_next (&iter, &key, &node))
        {
          if (JSON_NODE_HOLDS_OBJECT (node))
            {
              JsonObject *param = json_node_get_object (node);
              const char *type = json_object_get_string_member_with_default (param, "type", "");
              if (g_strcmp0 (type, "file") == 0)
                {
                  file_id = json_object_get_string_member_with_default (param, "id", NULL);
                  file_name = json_object_get_string_member_with_default (param, "name", NULL);
                  file_path = json_object_get_string_member_with_default (param, "path", NULL);
                  file_mimetype = json_object_get_string_member_with_default (param, "mimetype", NULL);
                  break;
                }
            }
        }

      if (!file_id)
        continue;

      /* Create preview image */
      GtkWidget *image = gtk_image_new_from_icon_name ("image-x-generic-symbolic");
      gtk_image_set_pixel_size (GTK_IMAGE (image), 64);
      gtk_widget_set_size_request (image, 72, 72);

      /* For videos, wrap in overlay with play icon */
      GtkWidget *frame_child = image;
      gboolean is_video = file_mimetype && g_str_has_prefix (file_mimetype, "video/");
      if (is_video)
        {
          GtkWidget *overlay = gtk_overlay_new ();
          gtk_overlay_set_child (GTK_OVERLAY (overlay), image);

          /* Play icon overlay */
          GtkWidget *play_icon = gtk_image_new_from_icon_name ("media-playback-start-symbolic");
          gtk_image_set_pixel_size (GTK_IMAGE (play_icon), 24);
          gtk_widget_add_css_class (play_icon, "video-play-overlay");
          gtk_widget_set_halign (play_icon, GTK_ALIGN_CENTER);
          gtk_widget_set_valign (play_icon, GTK_ALIGN_CENTER);
          gtk_overlay_add_overlay (GTK_OVERLAY (overlay), play_icon);

          frame_child = overlay;
        }

      GtkWidget *frame = gtk_frame_new (NULL);
      gtk_widget_add_css_class (frame, "media-preview-frame");
      gtk_frame_set_child (GTK_FRAME (frame), frame_child);

      /* Make it clickable with a GtkButton wrapping the frame */
      GtkWidget *button = gtk_button_new ();
      gtk_widget_add_css_class (button, "flat");
      gtk_widget_add_css_class (button, "media-button");
      gtk_button_set_child (GTK_BUTTON (button), frame);

      /* Store file info for click handling */
      MediaItemData *item_data = g_new0 (MediaItemData, 1);
      item_data->file_id = g_strdup (file_id);
      item_data->file_name = g_strdup (file_name);
      item_data->file_path = g_strdup (file_path);
      item_data->file_mimetype = g_strdup (file_mimetype);
      item_data->dialog = self;

      g_object_set_data_full (G_OBJECT (button), "media-item-data", item_data, media_item_data_free);

      /* Connect click handler */
      g_signal_connect (button, "clicked",
                        G_CALLBACK (on_media_item_clicked), item_data);

      gtk_flow_box_append (GTK_FLOW_BOX (self->media_flowbox), button);

      /* Load preview thumbnail */
      if (self->http_client)
        {
          MediaPreviewData *preview_data = g_new0 (MediaPreviewData, 1);
          preview_data->dialog = self;
          preview_data->image = image;

          gnalk_http_client_get_file_preview (client, file_id, 128, 128,
                                                on_media_preview_loaded, preview_data);
        }
    }
}

static void
load_shared_media (GnalkConversationInfoDialog *self)
{
  if (!self->conversation || !self->http_client)
    {
      gtk_widget_set_visible (self->media_empty_label, TRUE);
      return;
    }

  const char *token = gnalk_conversation_data_get_token (self->conversation);
  if (!token)
    {
      gtk_widget_set_visible (self->media_empty_label, TRUE);
      return;
    }

  gnalk_http_client_get_shared_items (self->http_client, token, "media", 12,
                                        on_shared_media_response, self);
}

static const char *
get_participant_role (gint participant_type)
{
  /* participantType: 1=owner, 2=moderator, 3=user, 4=guest, 5=user following link, 6=guest moderator */
  switch (participant_type)
    {
    case 1: return "Owner";
    case 2: return "Moderator";
    case 6: return "Moderator";
    default: return NULL;
    }
}

static void
on_participant_avatar_loaded (GnalkAvatarCache *cache G_GNUC_UNUSED,
                              GdkTexture        *texture,
                              gpointer           user_data)
{
  GtkWidget *image = GTK_WIDGET (user_data);
  if (texture && GTK_IS_IMAGE (image))
    gtk_image_set_from_paintable (GTK_IMAGE (image), GDK_PAINTABLE (texture));
}

/* Context menu click handlers */
static void
on_dialog_chat_clicked (GtkButton *button G_GNUC_UNUSED,
                        gpointer   user_data)
{
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (user_data);

  gtk_popover_popdown (GTK_POPOVER (self->context_popover));

  if (self->context_actor_id && *self->context_actor_id)
    g_signal_emit (self, signals[SIGNAL_CHAT_REQUESTED], 0, self->context_actor_id);
}

static void
on_dialog_promote_clicked (GtkButton *button G_GNUC_UNUSED,
                           gpointer   user_data)
{
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (user_data);

  gtk_popover_popdown (GTK_POPOVER (self->context_popover));

  g_signal_emit (self, signals[SIGNAL_PROMOTE_REQUESTED], 0,
                 self->context_attendee_id, self->context_actor_id);
}

static void
on_dialog_demote_clicked (GtkButton *button G_GNUC_UNUSED,
                          gpointer   user_data)
{
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (user_data);

  gtk_popover_popdown (GTK_POPOVER (self->context_popover));

  g_signal_emit (self, signals[SIGNAL_DEMOTE_REQUESTED], 0,
                 self->context_attendee_id, self->context_actor_id);
}

static void
on_dialog_remove_clicked (GtkButton *button G_GNUC_UNUSED,
                          gpointer   user_data)
{
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (user_data);

  gtk_popover_popdown (GTK_POPOVER (self->context_popover));

  g_signal_emit (self, signals[SIGNAL_REMOVE_REQUESTED], 0,
                 self->context_attendee_id, self->context_actor_id);
}

static void
show_dialog_participant_context_menu (GnalkConversationInfoDialog *self,
                                      GtkWidget                    *row,
                                      GtkGesture                   *gesture,
                                      double                        x,
                                      double                        y)
{
  /* Claim the sequence */
  gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_CLAIMED);

  /* Get participant data from row */
  const char *actor_id = g_object_get_data (G_OBJECT (row), "actor-id");
  gint64 attendee_id = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "attendee-id"));
  gint participant_type = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "participant-type"));

  /* Store context for action handlers */
  g_free (self->context_actor_id);
  self->context_actor_id = g_strdup (actor_id);
  self->context_attendee_id = attendee_id;
  self->context_participant_type = participant_type;

  /* Get caller's participant type from conversation */
  gint my_participant_type = 3;  /* default to regular user */
  gint room_type = 2;            /* default to group */
  if (self->conversation)
    {
      my_participant_type = gnalk_conversation_data_get_participant_type (self->conversation);
      room_type = gnalk_conversation_data_get_room_type (self->conversation);
    }

  /* Calculate visibility for each action */
  gboolean is_self = (g_strcmp0 (actor_id, self->my_user_id) == 0);
  gboolean caller_is_mod = (my_participant_type == 1 || my_participant_type == 2);
  gboolean target_is_mod = (participant_type == 1 || participant_type == 2 || participant_type == 6);
  gboolean target_is_owner = (participant_type == 1);
  gboolean is_one_to_one = (room_type == 1);

  /* Chat directly: not self, not 1-to-1, target is a user (has actorId) */
  gtk_widget_set_visible (self->chat_button, !is_self && !is_one_to_one && actor_id && *actor_id);

  /* Promote: caller is mod, not self, target is NOT already mod */
  gtk_widget_set_visible (self->promote_button, caller_is_mod && !is_self && !target_is_mod);

  /* Demote: caller is mod, not self, target IS mod but not owner */
  gtk_widget_set_visible (self->demote_button, caller_is_mod && !is_self && target_is_mod && !target_is_owner);

  /* Remove: caller is mod, not self, target not owner, not 1-to-1 */
  gtk_widget_set_visible (self->remove_button, caller_is_mod && !is_self && !target_is_owner && !is_one_to_one);

  /* Check if any buttons are visible */
  gboolean any_visible = gtk_widget_get_visible (self->chat_button) ||
                         gtk_widget_get_visible (self->promote_button) ||
                         gtk_widget_get_visible (self->demote_button) ||
                         gtk_widget_get_visible (self->remove_button);

  if (!any_visible)
    return;

  /* Reparent popover to this row (unparent from previous if needed) */
  GtkWidget *current_parent = gtk_widget_get_parent (self->context_popover);
  if (current_parent && current_parent != row)
    gtk_widget_unparent (self->context_popover);
  if (!gtk_widget_get_parent (self->context_popover))
    gtk_widget_set_parent (self->context_popover, row);

  /* Position and show the popover relative to the participant row */
  GdkRectangle rect = { (int) x, (int) y, 1, 1 };
  gtk_popover_set_pointing_to (GTK_POPOVER (self->context_popover), &rect);

  gtk_popover_popup (GTK_POPOVER (self->context_popover));
}

static void
on_dialog_participant_right_click (GtkGestureClick *gesture,
                                   int              n_press G_GNUC_UNUSED,
                                   double           x,
                                   double           y,
                                   gpointer         user_data)
{
  GtkWidget *row = GTK_WIDGET (user_data);
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (
    g_object_get_data (G_OBJECT (row), "info-dialog"));

  if (self)
    show_dialog_participant_context_menu (self, row, GTK_GESTURE (gesture), x, y);
}

static void
on_dialog_participant_long_press (GtkGestureLongPress *gesture,
                                  double               x,
                                  double               y,
                                  gpointer             user_data)
{
  GtkWidget *row = GTK_WIDGET (user_data);
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (
    g_object_get_data (G_OBJECT (row), "info-dialog"));

  if (self)
    show_dialog_participant_context_menu (self, row, GTK_GESTURE (gesture), x, y);
}

static void
on_participants_response (GnalkHttpClient *client G_GNUC_UNUSED,
                          JsonNode         *response,
                          GError           *error,
                          gpointer          user_data)
{
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (user_data);

  /* Stop spinner */
  gtk_spinner_stop (GTK_SPINNER (self->participants_spinner));
  gtk_widget_set_visible (self->participants_spinner, FALSE);

  /* Clear existing items */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->participants_listbox)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->participants_listbox), child);

  if (error)
    {
      g_debug ("Failed to load participants: %s", error->message);
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

  guint n_participants = json_array_get_length (data);
  for (guint i = 0; i < n_participants; i++)
    {
      JsonObject *participant = json_array_get_object_element (data, i);
      if (!participant)
        continue;

      const char *display_name = json_object_get_string_member_with_default (participant, "displayName", "");
      gint64 participant_type = json_object_get_int_member_with_default (participant, "participantType", 3);
      const char *actor_type = json_object_get_string_member_with_default (participant, "actorType", "");
      const char *actor_id = json_object_get_string_member_with_default (participant, "actorId", "");
      const char *user_status = json_object_get_string_member_with_default (participant, "status", "");
      const char *status_icon_str = json_object_get_string_member_with_default (participant, "statusIcon", "");
      const char *status_message = json_object_get_string_member_with_default (participant, "statusMessage", "");
      gint64 in_call = json_object_get_int_member_with_default (participant, "inCall", 0);

      /* Skip if no display name */
      if (!display_name || !*display_name)
        continue;

      /* Create row */
      GtkWidget *row = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), display_name);

      /* Build subtitle: role or status message */
      const char *role = get_participant_role ((gint) participant_type);
      if (status_message && *status_message)
        {
          if (role)
            {
              g_autofree char *subtitle = g_strdup_printf ("%s • %s", role, status_message);
              adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
            }
          else
            {
              adw_action_row_set_subtitle (ADW_ACTION_ROW (row), status_message);
            }
        }
      else if (role)
        {
          adw_action_row_set_subtitle (ADW_ACTION_ROW (row), role);
        }

      /* Create avatar with overlay for status indicator */
      GtkWidget *overlay = gtk_overlay_new ();

      GtkWidget *avatar_frame = gtk_frame_new (NULL);
      gtk_widget_add_css_class (avatar_frame, "participant-avatar");

      GtkWidget *avatar_image = gtk_image_new_from_icon_name ("avatar-default-symbolic");
      gtk_image_set_pixel_size (GTK_IMAGE (avatar_image), 32);
      gtk_frame_set_child (GTK_FRAME (avatar_frame), avatar_image);

      gtk_overlay_set_child (GTK_OVERLAY (overlay), avatar_frame);

      /* Add status indicator overlay */
      if (user_status && *user_status && g_strcmp0 (user_status, "offline") != 0)
        {
          GtkWidget *status_icon = gtk_image_new ();
          gtk_image_set_pixel_size (GTK_IMAGE (status_icon), 12);
          gtk_widget_set_halign (status_icon, GTK_ALIGN_END);
          gtk_widget_set_valign (status_icon, GTK_ALIGN_END);
          gtk_widget_add_css_class (status_icon, "status-indicator");

          if (g_strcmp0 (user_status, "online") == 0)
            {
              gtk_image_set_from_icon_name (GTK_IMAGE (status_icon), "user-available-symbolic");
              gtk_widget_add_css_class (status_icon, "status-online");
              gtk_overlay_add_overlay (GTK_OVERLAY (overlay), status_icon);
            }
          else if (g_strcmp0 (user_status, "away") == 0)
            {
              gtk_image_set_from_icon_name (GTK_IMAGE (status_icon), "user-away-symbolic");
              gtk_widget_add_css_class (status_icon, "status-away");
              gtk_overlay_add_overlay (GTK_OVERLAY (overlay), status_icon);
            }
          else if (g_strcmp0 (user_status, "dnd") == 0 || g_strcmp0 (user_status, "busy") == 0)
            {
              gtk_image_set_from_icon_name (GTK_IMAGE (status_icon), "user-busy-symbolic");
              gtk_widget_add_css_class (status_icon, "status-dnd");
              gtk_overlay_add_overlay (GTK_OVERLAY (overlay), status_icon);
            }
        }

      /* Add call indicator overlay */
      if (in_call != 0)
        {
          GtkWidget *call_icon = gtk_image_new_from_icon_name ("call-start-symbolic");
          gtk_image_set_pixel_size (GTK_IMAGE (call_icon), 12);
          gtk_widget_set_halign (call_icon, GTK_ALIGN_START);
          gtk_widget_set_valign (call_icon, GTK_ALIGN_START);
          gtk_widget_add_css_class (call_icon, "call-indicator");
          gtk_overlay_add_overlay (GTK_OVERLAY (overlay), call_icon);
        }

      adw_action_row_add_prefix (ADW_ACTION_ROW (row), overlay);

      /* Add status emoji suffix if present */
      if (status_icon_str && *status_icon_str)
        {
          GtkWidget *emoji_label = gtk_label_new (status_icon_str);
          gtk_widget_set_valign (emoji_label, GTK_ALIGN_CENTER);
          adw_action_row_add_suffix (ADW_ACTION_ROW (row), emoji_label);
        }

      /* Store data for context menu */
      gint64 attendee_id = json_object_get_int_member_with_default (participant, "attendeeId", 0);
      g_object_set_data_full (G_OBJECT (row), "actor-id", g_strdup (actor_id), g_free);
      g_object_set_data (G_OBJECT (row), "attendee-id", GINT_TO_POINTER ((gint) attendee_id));
      g_object_set_data (G_OBJECT (row), "participant-type", GINT_TO_POINTER ((gint) participant_type));
      g_object_set_data (G_OBJECT (row), "info-dialog", self);

      /* Add right-click gesture */
      GtkGesture *right_click = gtk_gesture_click_new ();
      gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (right_click), GDK_BUTTON_SECONDARY);
      gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (right_click), GTK_PHASE_CAPTURE);
      g_signal_connect (right_click, "pressed", G_CALLBACK (on_dialog_participant_right_click), row);
      gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (right_click));

      /* Add long press gesture for touch devices */
      GtkGesture *long_press = gtk_gesture_long_press_new ();
      gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (long_press), TRUE);
      g_signal_connect (long_press, "pressed", G_CALLBACK (on_dialog_participant_long_press), row);
      gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (long_press));

      gtk_list_box_append (GTK_LIST_BOX (self->participants_listbox), row);

      /* Load avatar for users */
      if (self->avatar_cache && g_strcmp0 (actor_type, "users") == 0 && actor_id && *actor_id)
        {
          gnalk_avatar_cache_get_user_avatar (self->avatar_cache, actor_id, 32,
                                                on_participant_avatar_loaded, avatar_image);
        }
    }
}

static void
load_participants (GnalkConversationInfoDialog *self)
{
  if (!self->conversation || !self->http_client)
    return;

  const char *token = gnalk_conversation_data_get_token (self->conversation);
  if (!token)
    return;

  /* Show spinner */
  gtk_widget_set_visible (self->participants_spinner, TRUE);
  gtk_spinner_start (GTK_SPINNER (self->participants_spinner));

  gnalk_http_client_get_participants (self->http_client, token,
                                        on_participants_response, self);
}

static void
gnalk_conversation_info_dialog_dispose (GObject *object)
{
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (object);

  if (self->context_popover)
    {
      /* Popover may be parented to a participant row - unparent it first */
      GtkWidget *parent = gtk_widget_get_parent (self->context_popover);
      if (parent)
        gtk_widget_unparent (self->context_popover);
      /* Release our ownership reference */
      g_object_unref (self->context_popover);
      self->context_popover = NULL;
    }

  G_OBJECT_CLASS (gnalk_conversation_info_dialog_parent_class)->dispose (object);
}

static void
gnalk_conversation_info_dialog_finalize (GObject *object)
{
  GnalkConversationInfoDialog *self = GNALK_CONVERSATION_INFO_DIALOG (object);

  g_clear_object (&self->conversation);
  g_clear_object (&self->http_client);
  g_clear_object (&self->avatar_cache);
  g_clear_pointer (&self->new_avatar_bytes, g_bytes_unref);
  g_free (self->new_avatar_content_type);
  g_free (self->context_actor_id);
  g_free (self->my_user_id);

  G_OBJECT_CLASS (gnalk_conversation_info_dialog_parent_class)->finalize (object);
}

static void
gnalk_conversation_info_dialog_class_init (GnalkConversationInfoDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnalk_conversation_info_dialog_dispose;
  object_class->finalize = gnalk_conversation_info_dialog_finalize;

  /**
   * GnalkConversationInfoDialog::media-clicked:
   * @dialog: the dialog
   * @file_id: the file ID
   * @file_mimetype: the file mimetype
   * @file_path: the WebDAV file path
   * @file_name: the file name
   *
   * Emitted when a media item is clicked.
   */
  signals[SIGNAL_MEDIA_CLICKED] =
    g_signal_new ("media-clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 4,
                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIGNAL_CHAT_REQUESTED] =
    g_signal_new ("chat-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_PROMOTE_REQUESTED] =
    g_signal_new ("promote-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, G_TYPE_INT64, G_TYPE_STRING);

  signals[SIGNAL_DEMOTE_REQUESTED] =
    g_signal_new ("demote-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, G_TYPE_INT64, G_TYPE_STRING);

  signals[SIGNAL_REMOVE_REQUESTED] =
    g_signal_new ("remove-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, G_TYPE_INT64, G_TYPE_STRING);

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-conversation-info-dialog.ui");

  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, navigation_view);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, info_page);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, settings_page);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, info_avatar);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, info_name);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, info_description);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, settings_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, media_group);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, media_flowbox);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, media_empty_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, participants_group);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, participants_listbox);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, participants_spinner);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, conversation_settings_group);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, settings_name_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, settings_description_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, settings_avatar_row);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, change_avatar_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, notification_level_row);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, call_notifications_row);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, save_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationInfoDialog, settings_spinner);
}

static void
gnalk_conversation_info_dialog_init (GnalkConversationInfoDialog *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  g_signal_connect (self->settings_button, "clicked", G_CALLBACK (on_settings_button_clicked), self);
  g_signal_connect (self->save_button, "clicked", G_CALLBACK (on_save_button_clicked), self);
  g_signal_connect (self->change_avatar_button, "clicked", G_CALLBACK (on_change_avatar_clicked), self);

  /* Create context menu popover - we own it and manage its parent */
  self->context_popover = gtk_popover_new ();
  g_object_ref_sink (self->context_popover);  /* Sink floating ref, we own it */
  gtk_popover_set_has_arrow (GTK_POPOVER (self->context_popover), TRUE);

  GtkWidget *menu_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (menu_box, "context-menu");

  /* Chat directly button */
  self->chat_button = gtk_button_new_with_label ("Chat directly");
  gtk_button_set_has_frame (GTK_BUTTON (self->chat_button), FALSE);
  gtk_widget_add_css_class (self->chat_button, "flat");
  g_signal_connect (self->chat_button, "clicked", G_CALLBACK (on_dialog_chat_clicked), self);
  gtk_box_append (GTK_BOX (menu_box), self->chat_button);

  /* Promote to moderator button */
  self->promote_button = gtk_button_new_with_label ("Promote to moderator");
  gtk_button_set_has_frame (GTK_BUTTON (self->promote_button), FALSE);
  gtk_widget_add_css_class (self->promote_button, "flat");
  g_signal_connect (self->promote_button, "clicked", G_CALLBACK (on_dialog_promote_clicked), self);
  gtk_box_append (GTK_BOX (menu_box), self->promote_button);

  /* Demote from moderator button */
  self->demote_button = gtk_button_new_with_label ("Demote from moderator");
  gtk_button_set_has_frame (GTK_BUTTON (self->demote_button), FALSE);
  gtk_widget_add_css_class (self->demote_button, "flat");
  g_signal_connect (self->demote_button, "clicked", G_CALLBACK (on_dialog_demote_clicked), self);
  gtk_box_append (GTK_BOX (menu_box), self->demote_button);

  /* Remove from conversation button */
  self->remove_button = gtk_button_new_with_label ("Remove from conversation");
  gtk_button_set_has_frame (GTK_BUTTON (self->remove_button), FALSE);
  gtk_widget_add_css_class (self->remove_button, "flat");
  gtk_widget_add_css_class (self->remove_button, "error");
  g_signal_connect (self->remove_button, "clicked", G_CALLBACK (on_dialog_remove_clicked), self);
  gtk_box_append (GTK_BOX (menu_box), self->remove_button);

  gtk_popover_set_child (GTK_POPOVER (self->context_popover), menu_box);
}

/* Public API */

GnalkConversationInfoDialog *
gnalk_conversation_info_dialog_new (void)
{
  return g_object_new (GNALK_TYPE_CONVERSATION_INFO_DIALOG, NULL);
}

void
gnalk_conversation_info_dialog_set_conversation (GnalkConversationInfoDialog *self,
                                                   GnalkConversationData       *conversation)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_INFO_DIALOG (self));

  g_set_object (&self->conversation, conversation);
  update_info_page (self);
}

void
gnalk_conversation_info_dialog_set_http_client (GnalkConversationInfoDialog *self,
                                                  GnalkHttpClient             *client)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_INFO_DIALOG (self));

  g_set_object (&self->http_client, client);
}

void
gnalk_conversation_info_dialog_set_avatar_cache (GnalkConversationInfoDialog *self,
                                                   GnalkAvatarCache            *cache)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_INFO_DIALOG (self));

  g_set_object (&self->avatar_cache, cache);
}

void
gnalk_conversation_info_dialog_set_my_user_id (GnalkConversationInfoDialog *self,
                                                 const char                   *user_id)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_INFO_DIALOG (self));

  g_free (self->my_user_id);
  self->my_user_id = g_strdup (user_id);
}
