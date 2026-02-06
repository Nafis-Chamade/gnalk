/* gnalk-window-file-handling.c
 *
 * File handling, downloads, image gallery, and media capture for GnalkWindow.
 *
 * Contains:
 * - File attachment dialog and selection
 * - File download pipeline (cache + open)
 * - Image gallery integration
 * - Media capture (camera, voice, video)
 * - Info dialog media click handler
 */

#include "gnalk-window-private.h"
#include "gnalk-conversation-info-dialog.h"
#include "gnalk-video-player-dialog.h"
#include "gnalk-message-data.h"
#include <glib/gi18n.h>

/* ============ File Attachment Functions ============ */

void
gnalk_window_on_attachment_closed (GnalkConversationActions *actions,
                                   GnalkWindow              *self)
{
  (void) actions;
  gnalk_window_clear_attachments (self);
}

static const char *
get_icon_name_for_mime_type (const char *mime_type)
{
  if (!mime_type)
    return "text-x-generic-symbolic";

  if (g_str_has_prefix (mime_type, "image/"))
    return "image-x-generic-symbolic";
  if (g_str_has_prefix (mime_type, "video/"))
    return "video-x-generic-symbolic";
  if (g_str_has_prefix (mime_type, "audio/"))
    return "audio-x-generic-symbolic";
  if (g_str_has_prefix (mime_type, "text/"))
    return "text-x-generic-symbolic";
  if (g_strstr_len (mime_type, -1, "pdf"))
    return "application-pdf-symbolic";
  if (g_strstr_len (mime_type, -1, "zip") ||
      g_strstr_len (mime_type, -1, "tar") ||
      g_strstr_len (mime_type, -1, "gzip") ||
      g_strstr_len (mime_type, -1, "archive"))
    return "package-x-generic-symbolic";
  if (g_strstr_len (mime_type, -1, "word") ||
      g_strstr_len (mime_type, -1, "document"))
    return "x-office-document-symbolic";
  if (g_strstr_len (mime_type, -1, "spreadsheet") ||
      g_strstr_len (mime_type, -1, "excel"))
    return "x-office-spreadsheet-symbolic";
  if (g_strstr_len (mime_type, -1, "presentation") ||
      g_strstr_len (mime_type, -1, "powerpoint"))
    return "x-office-presentation-symbolic";

  return "text-x-generic-symbolic";
}

static void
on_file_dialog_response (GObject      *source,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GListModel) files = NULL;

  files = gtk_file_dialog_open_multiple_finish (GTK_FILE_DIALOG (source), result, &error);
  if (!files)
    {
      if (!g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
          !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
        g_warning ("File dialog error: %s", error->message);
      return;
    }

  /* Clear any previous attachments */
  gnalk_window_clear_attachments (self);

  /* Ensure array exists */
  if (!self->attachments)
    self->attachments = g_ptr_array_new_with_free_func ((GDestroyNotify) attachment_info_free);

  guint n_files = g_list_model_get_n_items (files);

  for (guint i = 0; i < n_files; i++)
    {
      g_autoptr(GFile) file = g_list_model_get_item (files, i);
      if (!file)
        continue;

      g_autofree char *path = g_file_get_path (file);
      g_autofree char *basename = g_file_get_basename (file);
      g_autofree char *content_type = NULL;

      g_autoptr(GFileInfo) info = g_file_query_info (file,
                                                       G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                                       G_FILE_QUERY_INFO_NONE,
                                                       NULL, NULL);
      if (info)
        content_type = g_strdup (g_file_info_get_content_type (info));

      /* Read file contents */
      gchar *contents = NULL;
      gsize length = 0;
      g_autoptr(GError) read_error = NULL;

      if (!g_file_get_contents (path, &contents, &length, &read_error))
        {
          g_warning ("Failed to read file %s: %s", path, read_error->message);
          continue;
        }

      /* Create attachment info */
      AttachmentInfo *att = g_new0 (AttachmentInfo, 1);
      att->file_path = g_strdup (path);
      att->file_bytes = g_bytes_new_take (contents, length);
      att->content_type = g_strdup (content_type);
      att->file_name = g_strdup (basename);

      g_ptr_array_add (self->attachments, att);

      /* Prepare preview texture for images */
      GdkTexture *preview_texture = NULL;
      if (content_type && g_str_has_prefix (content_type, "image/"))
        preview_texture = gdk_texture_new_from_file (file, NULL);

      const char *icon_name = get_icon_name_for_mime_type (content_type);

      /* Add to UI preview */
      gnalk_conversation_actions_add_attachment (self->conversation_actions,
                                                   basename, icon_name, preview_texture);

      if (preview_texture)
        g_object_unref (preview_texture);
    }
}

void
gnalk_window_on_attach_clicked (GtkButton    *button,
                                GnalkWindow *self)
{
  (void) button;

  GtkFileDialog *dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, "Attach Files");
  gtk_file_dialog_set_modal (dialog, TRUE);

  gtk_file_dialog_open_multiple (dialog, GTK_WINDOW (self), NULL,
                                  on_file_dialog_response, self);
  g_object_unref (dialog);
}

/* ============ Media Capture Functions ============ */

void
gnalk_window_on_voice_message_recorded (GnalkMessageInput *input G_GNUC_UNUSED,
                                        GBytes             *audio_data,
                                        gdouble             duration,
                                        GnalkWindow       *self)
{
  g_message ("Window: Voice message recorded, duration=%.1f", duration);

  if (!self->message_controller || !self->current_room)
    {
      g_warning ("Window: Cannot send voice message - no message controller or room");
      return;
    }

  /* Generate filename: Talk recording YYYY-MM-DD HH-mm-ss.ogg */
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *filename = g_date_time_format (now, "Talk recording %Y-%m-%d %H-%M-%S.ogg");

  /* Generate reference ID for tracking */
  g_autofree char *reference_id = g_uuid_string_random ();

  /* Upload as voice message */
  gnalk_message_controller_upload_voice_message (self->message_controller, filename, audio_data, reference_id);
}

static void
on_photo_captured (GnalkMediaCaptureDialog *dialog G_GNUC_UNUSED,
                   GBytes                   *jpeg_data,
                   GnalkWindow             *self)
{
  g_message ("Window: Photo captured");

  if (!self->message_controller || !self->current_room)
    {
      g_warning ("Window: Cannot send photo - no message controller or room");
      return;
    }

  /* Generate filename: Talk photo YYYY-MM-DD HH-mm-ss.jpg */
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *filename = g_date_time_format (now, "Talk photo %Y-%m-%d %H-%M-%S.jpg");

  /* Generate reference ID for tracking */
  g_autofree char *reference_id = g_uuid_string_random ();

  /* Upload photo */
  gnalk_message_controller_upload_file (self->message_controller,
                                          filename,
                                          jpeg_data,
                                          "image/jpeg",
                                          NULL,  /* caption */
                                          0,     /* reply_to */
                                          reference_id);
}

static void
on_video_captured (GnalkMediaCaptureDialog *dialog G_GNUC_UNUSED,
                   GBytes                   *video_data,
                   GnalkWindow             *self)
{
  g_message ("Window: Video captured");

  if (!self->message_controller || !self->current_room)
    {
      g_warning ("Window: Cannot send video - no message controller or room");
      return;
    }

  /* Generate filename: Talk video YYYY-MM-DD HH-mm-ss.webm */
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *filename = g_date_time_format (now, "Talk video %Y-%m-%d %H-%M-%S.webm");

  /* Generate reference ID for tracking */
  g_autofree char *reference_id = g_uuid_string_random ();

  /* Upload video */
  gnalk_message_controller_upload_file (self->message_controller,
                                          filename,
                                          video_data,
                                          "video/webm",
                                          NULL,  /* caption */
                                          0,     /* reply_to */
                                          reference_id);
}

static void
on_media_capture_dialog_closed (GnalkMediaCaptureDialog *dialog G_GNUC_UNUSED,
                                GnalkWindow             *self)
{
  self->media_capture_dialog = NULL;
}

void
gnalk_window_on_camera_clicked (GnalkMessageInput *input G_GNUC_UNUSED,
                                GnalkWindow       *self)
{
  g_message ("Window: Camera button clicked");

  /* Prevent opening multiple dialogs (double-click protection) */
  if (self->media_capture_dialog != NULL)
    {
      g_message ("Window: Media capture dialog already open, ignoring");
      return;
    }

  /* Create and present the media capture dialog */
  self->media_capture_dialog = gnalk_media_capture_dialog_new ();

  g_signal_connect (self->media_capture_dialog, "photo-captured", G_CALLBACK (on_photo_captured), self);
  g_signal_connect (self->media_capture_dialog, "video-captured", G_CALLBACK (on_video_captured), self);
  g_signal_connect (self->media_capture_dialog, "closed", G_CALLBACK (on_media_capture_dialog_closed), self);

  adw_dialog_present (ADW_DIALOG (self->media_capture_dialog), GTK_WIDGET (self));
}

/* ============ File Download Pipeline ============ */

typedef struct {
  char     *file_path;
  GWeakRef  row_ref;
  GWeakRef  window_ref;  /* Window reference for info dialog downloads */
  gboolean  is_video;    /* TRUE when downloaded from info dialog for video player */
} FileDownloadData;

static void
file_download_data_free (FileDownloadData *data)
{
  g_free (data->file_path);
  g_weak_ref_clear (&data->row_ref);
  g_weak_ref_clear (&data->window_ref);
  g_free (data);
}

static void
on_file_downloaded (GnalkHttpClient *client,
                    GBytes           *bytes,
                    const char       *content_type,
                    GError           *error,
                    gpointer          user_data)
{
  FileDownloadData *data = user_data;
  GnalkMessageRow *row = g_weak_ref_get (&data->row_ref);

  /* Hide spinner */
  if (row)
    gnalk_message_row_set_downloading (row, FALSE);

  if (error)
    {
      g_warning ("Failed to download file: %s", error->message);
      if (row)
        g_object_unref (row);
      file_download_data_free (data);
      return;
    }

  if (!bytes)
    {
      g_warning ("No data received for file download");
      if (row)
        g_object_unref (row);
      file_download_data_free (data);
      return;
    }

  /* Write bytes to cache file */
  g_autoptr(GError) write_error = NULL;
  gsize size;
  gconstpointer file_data = g_bytes_get_data (bytes, &size);

  if (!g_file_set_contents (data->file_path, file_data, size, &write_error))
    {
      g_warning ("Failed to write cache file: %s", write_error->message);
      if (row)
        g_object_unref (row);
      file_download_data_free (data);
      return;
    }

  /* Show cached icon */
  if (row)
    {
      gnalk_message_row_set_file_cached (row, TRUE);

      /* Check if this is an audio file - set on audio player instead of launching */
      if (gnalk_message_row_has_audio_file (row))
        {
          gnalk_message_row_set_audio_file (row, data->file_path);
          g_object_unref (row);
          file_download_data_free (data);
          return;
        }

      /* Check if this is a video file - open video player dialog */
      if (gnalk_message_row_has_video_file (row))
        {
          /* Get the window from the row's toplevel */
          GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (row));
          if (GTK_IS_WINDOW (root))
            {
              /* Set thumbnail file path so it displays */
              gnalk_message_row_set_video_file (row, data->file_path);

              /* Open video player dialog */
              g_autofree char *file_name = g_path_get_basename (data->file_path);
              GnalkVideoPlayerDialog *dialog = gnalk_video_player_dialog_new ();
              gnalk_video_player_dialog_set_file (dialog, data->file_path, file_name);
              adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (root));
            }
          g_object_unref (row);
          file_download_data_free (data);
          return;
        }

      g_object_unref (row);
    }
  else if (data->is_video)
    {
      /* Video download from info dialog (no row) - open in video player */
      GnalkWindow *window = g_weak_ref_get (&data->window_ref);
      if (window)
        {
          g_autofree char *file_name = g_path_get_basename (data->file_path);
          GnalkVideoPlayerDialog *dialog = gnalk_video_player_dialog_new ();
          gnalk_video_player_dialog_set_file (dialog, data->file_path, file_name);
          adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (window));
          g_object_unref (window);
          file_download_data_free (data);
          return;
        }
    }

  /* Open with default application */
  g_autofree char *uri = g_filename_to_uri (data->file_path, NULL, NULL);
  if (uri)
    {
      g_autoptr(GError) launch_error = NULL;
      if (!g_app_info_launch_default_for_uri (uri, NULL, &launch_error))
        {
          g_warning ("Failed to open file: %s", launch_error->message);
        }
    }

  file_download_data_free (data);
}

/* ============ Image Gallery ============ */

/* Check if a mimetype represents an image */
static gboolean
is_image_mimetype (const char *mimetype)
{
  if (!mimetype)
    return FALSE;

  return g_str_has_prefix (mimetype, "image/");
}

/* Collect all images from the messages store and find the index of target file */
static GPtrArray *
collect_conversation_images (GnalkWindow *self,
                             const char   *target_file_id,
                             guint        *out_index)
{
  GPtrArray *images = g_ptr_array_new_with_free_func ((GDestroyNotify) gnalk_image_info_free);
  guint target_idx = 0;

  guint n_messages = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  for (guint i = 0; i < n_messages; i++)
    {
      g_autoptr(GnalkMessageData) data = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
      if (!data)
        continue;

      JsonObject *params = gnalk_message_data_get_message_parameters (data);
      if (!params)
        continue;

      /* Look for file parameter in message parameters */
      JsonObjectIter iter;
      const char *key;
      JsonNode *value;

      json_object_iter_init (&iter, params);
      while (json_object_iter_next (&iter, &key, &value))
        {
          if (!JSON_NODE_HOLDS_OBJECT (value))
            continue;

          JsonObject *param_obj = json_node_get_object (value);
          const char *type = json_object_get_string_member_with_default (param_obj, "type", "");

          if (g_strcmp0 (type, "file") != 0)
            continue;

          const char *mimetype = json_object_get_string_member_with_default (param_obj, "mimetype", "");
          if (!is_image_mimetype (mimetype))
            continue;

          /* Found an image - extract info */
          const char *fid = json_object_get_string_member_with_default (param_obj, "id", "");
          const char *fname = json_object_get_string_member_with_default (param_obj, "name", "");
          const char *fpath = json_object_get_string_member_with_default (param_obj, "path", "");
          gint64 msg_id = gnalk_message_data_get_id (data);

          GnalkImageInfo *info = gnalk_image_info_new (fid, fname, fpath, mimetype, msg_id);
          g_ptr_array_add (images, info);

          /* Check if this is the target */
          if (target_file_id && g_strcmp0 (fid, target_file_id) == 0)
            target_idx = images->len - 1;
        }
    }

  if (out_index)
    *out_index = target_idx;

  return images;
}

/* Handler for when gallery downloads and caches an image */
static void
on_image_gallery_image_cached (GnalkImageGallery *gallery G_GNUC_UNUSED,
                               const char         *file_id,
                               const char         *file_name,
                               GnalkWindow       *self)
{
  /* Find all message rows with this file_id and update their cache state */
  if (!self->messages_store || !file_id || !*file_id)
    return;

  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages_store));
  for (guint i = 0; i < n_items; i++)
    {
      GnalkMessageData *msg = g_list_model_get_item (G_LIST_MODEL (self->messages_store), i);
      if (!msg)
        continue;

      JsonObject *params = gnalk_message_data_get_message_parameters (msg);
      if (params && json_object_has_member (params, "file"))
        {
          JsonNode *file_node = json_object_get_member (params, "file");
          if (file_node && JSON_NODE_HOLDS_OBJECT (file_node))
            {
              JsonObject *file = json_node_get_object (file_node);
              const char *msg_file_id = json_object_get_string_member_with_default (file, "id", NULL);

              if (msg_file_id && g_strcmp0 (msg_file_id, file_id) == 0)
                {
                  /* Force re-bind by remove/insert so cache state is updated */
                  g_object_ref (msg);
                  g_list_store_remove (self->messages_store, i);
                  g_list_store_insert (self->messages_store, i, msg);
                  g_object_unref (msg);
                  g_object_unref (msg);
                  break;
                }
            }
        }
      g_object_unref (msg);
    }
}

/* Called when the image gallery dialog is closed */
static void
on_image_gallery_closed (AdwDialog *dialog G_GNUC_UNUSED,
                         gpointer   user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);
  /* Clear the pointer since AdwDialog destroys itself when closed */
  self->image_gallery = NULL;
}

/* Ensure the image gallery is created */
static void
ensure_image_gallery (GnalkWindow *self)
{
  if (self->image_gallery)
    return;

  self->image_gallery = gnalk_image_gallery_new ();
  gnalk_image_gallery_set_http_client (self->image_gallery, self->http_client);
  gnalk_image_gallery_set_avatar_cache (self->image_gallery, self->avatar_cache);

  /* Connect to image-cached signal to update row cache state */
  g_signal_connect (self->image_gallery, "image-cached",
                    G_CALLBACK (on_image_gallery_image_cached), self);

  /* Clear pointer when dialog closes (AdwDialog destroys itself on close) */
  g_signal_connect (self->image_gallery, "closed",
                    G_CALLBACK (on_image_gallery_closed), self);
}

/* ============ File Click Handlers ============ */

void
gnalk_window_on_file_clicked (GnalkMessageRow *row,
                              const char       *file_id,
                              const char       *file_mimetype,
                              const char       *file_link G_GNUC_UNUSED,
                              const char       *file_name,
                              const char       *file_path,
                              gpointer          user_data)
{
  GnalkWindow *self = GNALK_WINDOW (user_data);

  if (!file_path || !*file_path)
    {
      g_warning ("No file path available for download");
      return;
    }

  /* Check if this is an image - open in gallery */
  if (is_image_mimetype (file_mimetype))
    {
      ensure_image_gallery (self);

      guint current_index = 0;
      GPtrArray *images = collect_conversation_images (self, file_id, &current_index);

      if (images->len > 0)
        {
          gnalk_image_gallery_set_images (self->image_gallery, images, current_index);
          gnalk_image_gallery_present (self->image_gallery, GTK_WIDGET (self));
          return;
        }

      /* No images found (shouldn't happen), fall through to download */
      g_ptr_array_unref (images);
    }

  /* Build cache path: ~/.cache/gnalk/{file_id}_{filename} */
  g_autofree char *cache_dir = g_build_filename (g_get_user_cache_dir (), "gnalk", NULL);
  g_mkdir_with_parents (cache_dir, 0700);

  const char *name = (file_name && *file_name) ? file_name : "download";
  g_autofree char *safe_name = g_strdup_printf ("%s_%s", file_id ? file_id : "unknown", name);
  g_autofree char *cache_path = g_build_filename (cache_dir, safe_name, NULL);

  /* Check if this is an audio file - handle via audio player */
  gboolean is_audio = gnalk_message_row_has_audio_file (row);
  if (is_audio)
    {
      /* Check if file is already cached */
      if (g_file_test (cache_path, G_FILE_TEST_EXISTS))
        {
          /* Set loading briefly so auto-play triggers */
          gnalk_message_row_set_audio_loading (row, TRUE);
          gnalk_message_row_set_audio_file (row, cache_path);
          return;
        }

      /* Show loading state on audio player */
      gnalk_message_row_set_audio_loading (row, TRUE);

      /* Prepare callback data with weak reference to row */
      FileDownloadData *data = g_new0 (FileDownloadData, 1);
      data->file_path = g_strdup (cache_path);
      g_weak_ref_init (&data->row_ref, row);
      g_weak_ref_init (&data->window_ref, NULL);

      /* Download file via WebDAV */
      gnalk_http_client_download_file (self->http_client, file_path,
                                         on_file_downloaded, data);
      return;
    }

  /* Check if this is a video file - handle via video player dialog */
  gboolean is_video = gnalk_message_row_has_video_file (row);
  if (is_video)
    {
      /* Check if file is already cached */
      if (g_file_test (cache_path, G_FILE_TEST_EXISTS))
        {
          /* Show cached icon and open video player dialog */
          gnalk_message_row_set_file_cached (row, TRUE);

          /* Open video player dialog */
          GnalkVideoPlayerDialog *dialog = gnalk_video_player_dialog_new ();
          gnalk_video_player_dialog_set_file (dialog, cache_path, file_name);
          adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
          return;
        }

      /* Show download spinner */
      gnalk_message_row_set_downloading (row, TRUE);

      /* Prepare callback data with weak reference to row */
      FileDownloadData *data = g_new0 (FileDownloadData, 1);
      data->file_path = g_strdup (cache_path);
      g_weak_ref_init (&data->row_ref, row);
      g_weak_ref_init (&data->window_ref, NULL);

      /* Download file via WebDAV */
      gnalk_http_client_download_file (self->http_client, file_path,
                                         on_file_downloaded, data);
      return;
    }

  /* Non-image, non-audio, non-video file: download and open with default application */

  /* Check if file is already cached */
  if (g_file_test (cache_path, G_FILE_TEST_EXISTS))
    {
      /* Show cached icon and open directly */
      gnalk_message_row_set_file_cached (row, TRUE);

      g_autofree char *uri = g_filename_to_uri (cache_path, NULL, NULL);
      if (uri)
        {
          g_autoptr(GError) error = NULL;
          if (!g_app_info_launch_default_for_uri (uri, NULL, &error))
            g_warning ("Failed to open cached file: %s", error->message);
        }
      return;
    }

  /* Show download spinner */
  gnalk_message_row_set_downloading (row, TRUE);

  /* Prepare callback data with weak reference to row */
  FileDownloadData *data = g_new0 (FileDownloadData, 1);
  data->file_path = g_strdup (cache_path);
  g_weak_ref_init (&data->row_ref, row);
  g_weak_ref_init (&data->window_ref, NULL);

  /* Download file via WebDAV */
  gnalk_http_client_download_file (self->http_client, file_path,
                                     on_file_downloaded, data);
}

/* Handler for media clicks from the conversation info dialog */
void
gnalk_window_on_info_dialog_media_clicked (GnalkConversationInfoDialog *dialog,
                                           const char                   *file_id,
                                           const char                   *file_mimetype,
                                           const char                   *file_path,
                                           const char                   *file_name,
                                           GnalkWindow                 *self)
{
  if (!file_path || !*file_path)
    {
      g_warning ("No file path available for media");
      return;
    }

  /* Check if this is an image - open in gallery (stacks on top of info dialog) */
  if (file_mimetype && g_str_has_prefix (file_mimetype, "image/"))
    {
      ensure_image_gallery (self);

      guint current_index = 0;
      GPtrArray *images = collect_conversation_images (self, file_id, &current_index);

      if (images->len > 0)
        {
          gnalk_image_gallery_set_images (self->image_gallery, images, current_index);
          gnalk_image_gallery_present (self->image_gallery, GTK_WIDGET (self));
          return;
        }

      /* No images found in conversation, just show this single image */
      g_ptr_array_unref (images);

      /* Create a single-image array */
      images = g_ptr_array_new_with_free_func ((GDestroyNotify) gnalk_image_info_free);
      GnalkImageInfo *info = gnalk_image_info_new (file_id, file_name, file_path, file_mimetype, 0);
      g_ptr_array_add (images, info);

      gnalk_image_gallery_set_images (self->image_gallery, images, 0);
      gnalk_image_gallery_present (self->image_gallery, GTK_WIDGET (self));
      return;
    }

  /* Build cache path */
  g_autofree char *cache_dir = g_build_filename (g_get_user_cache_dir (), "gnalk", NULL);
  g_mkdir_with_parents (cache_dir, 0700);

  const char *name = (file_name && *file_name) ? file_name : "download";
  g_autofree char *safe_name = g_strdup_printf ("%s_%s", file_id ? file_id : "unknown", name);
  g_autofree char *cache_path = g_build_filename (cache_dir, safe_name, NULL);

  /* Check if this is a video - open in in-app player */
  gboolean is_video = file_mimetype && g_str_has_prefix (file_mimetype, "video/");
  if (is_video)
    {
      /* Check if file is already cached */
      if (g_file_test (cache_path, G_FILE_TEST_EXISTS))
        {
          /* Open video player dialog */
          GnalkVideoPlayerDialog *video_dialog = gnalk_video_player_dialog_new ();
          gnalk_video_player_dialog_set_file (video_dialog, cache_path, file_name);
          adw_dialog_present (ADW_DIALOG (video_dialog), GTK_WIDGET (self));
          return;
        }

      /* Download video first, then open in player */
      FileDownloadData *data = g_new0 (FileDownloadData, 1);
      data->file_path = g_strdup (cache_path);
      data->is_video = TRUE;
      g_weak_ref_init (&data->row_ref, NULL);     /* No row to update */
      g_weak_ref_init (&data->window_ref, self);  /* Keep window ref for video player */

      gnalk_http_client_download_file (self->http_client, file_path,
                                         on_file_downloaded, data);
      return;
    }

  /* Non-video file: download and open with default application */

  /* Check if file is already cached */
  if (g_file_test (cache_path, G_FILE_TEST_EXISTS))
    {
      g_autofree char *uri = g_filename_to_uri (cache_path, NULL, NULL);
      if (uri)
        {
          g_autoptr(GError) error = NULL;
          if (!g_app_info_launch_default_for_uri (uri, NULL, &error))
            g_warning ("Failed to open cached file: %s", error->message);
        }
      return;
    }

  /* Download file via WebDAV */
  FileDownloadData *data = g_new0 (FileDownloadData, 1);
  data->file_path = g_strdup (cache_path);
  g_weak_ref_init (&data->row_ref, NULL);     /* No row to update */
  g_weak_ref_init (&data->window_ref, NULL);  /* Not a video, no window needed */

  gnalk_http_client_download_file (self->http_client, file_path,
                                     on_file_downloaded, data);
}
