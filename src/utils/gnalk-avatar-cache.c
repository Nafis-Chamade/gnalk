#include "gnalk-avatar-cache.h"
#include <glib/gstdio.h>

static char *
get_avatar_cache_dir (void)
{
  return g_build_filename (g_get_user_cache_dir (), "gnalk", "avatars", NULL);
}

static char *
get_avatar_cache_path (const char *key)
{
  g_autofree char *cache_dir = get_avatar_cache_dir ();
  g_autofree char *safe_key = g_uri_escape_string (key, NULL, FALSE);
  return g_build_filename (cache_dir, safe_key, NULL);
}

static void
ensure_cache_dir (void)
{
  g_autofree char *cache_dir = get_avatar_cache_dir ();
  g_mkdir_with_parents (cache_dir, 0755);
}

static GdkTexture *
load_avatar_from_disk (const char *key)
{
  g_autofree char *path = get_avatar_cache_path (key);

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    return NULL;

  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autofree char *contents = NULL;
  gsize length;

  if (!g_file_get_contents (path, &contents, &length, &error))
    {
      g_debug ("Failed to read avatar cache: %s", error->message);
      return NULL;
    }

  bytes = g_bytes_new_take (g_steal_pointer (&contents), length);
  GdkTexture *texture = gdk_texture_new_from_bytes (bytes, &error);
  if (!texture)
    g_debug ("Failed to create texture from cached avatar: %s", error->message);

  return texture;
}

static void
save_avatar_to_disk (const char *key, GBytes *bytes)
{
  ensure_cache_dir ();
  g_autofree char *path = get_avatar_cache_path (key);

  g_autoptr(GError) error = NULL;
  gsize size;
  gconstpointer data = g_bytes_get_data (bytes, &size);

  if (!g_file_set_contents (path, data, size, &error))
    g_debug ("Failed to save avatar cache: %s", error->message);
}

struct _GnalkAvatarCache
{
  GObject parent_instance;

  GnalkHttpClient *http_client;
  GHashTable       *cache;
  GHashTable       *pending;
  GHashTable       *color_cache;  /* user_id -> GdkRGBA* */
  gboolean          is_online;
  gint64            account_id;   /* For per-account room avatar caching */
};

G_DEFINE_FINAL_TYPE (GnalkAvatarCache, gnalk_avatar_cache, G_TYPE_OBJECT)

typedef struct
{
  GnalkAvatarCallback callback;
  gpointer             user_data;
} PendingCallbackData;

typedef struct
{
  GnalkAvatarCache   *cache;
  char                *key;
  GnalkAvatarCallback callback;
  gpointer             user_data;
} AvatarLoadData;

static void
avatar_load_data_free (AvatarLoadData *data)
{
  g_clear_object (&data->cache);
  g_free (data->key);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AvatarLoadData, avatar_load_data_free)

static void
pending_list_free (gpointer data)
{
  GList *list = data;
  for (GList *l = list; l; l = l->next)
    g_free (l->data);
  g_list_free (list);
}

static void
gnalk_avatar_cache_finalize (GObject *object)
{
  GnalkAvatarCache *self = GNALK_AVATAR_CACHE (object);

  g_hash_table_destroy (self->cache);
  g_hash_table_destroy (self->pending);
  g_hash_table_destroy (self->color_cache);
  g_clear_object (&self->http_client);

  G_OBJECT_CLASS (gnalk_avatar_cache_parent_class)->finalize (object);
}

static void
gnalk_avatar_cache_class_init (GnalkAvatarCacheClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_avatar_cache_finalize;
}

static void
gnalk_avatar_cache_init (GnalkAvatarCache *self)
{
  self->cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                        g_free, g_object_unref);
  self->pending = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, pending_list_free);
  self->color_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, g_free);
  self->is_online = TRUE;  /* Assume online until told otherwise */
}

GnalkAvatarCache *
gnalk_avatar_cache_new (GnalkHttpClient *http_client,
                         gint64            account_id)
{
  GnalkAvatarCache *self = g_object_new (GNALK_TYPE_AVATAR_CACHE, NULL);
  self->http_client = g_object_ref (http_client);
  self->account_id = account_id;
  return self;
}

GdkTexture *
gnalk_avatar_cache_lookup (GnalkAvatarCache *self,
                             const char        *key)
{
  g_return_val_if_fail (GNALK_IS_AVATAR_CACHE (self), NULL);
  g_return_val_if_fail (key != NULL, NULL);

  return g_hash_table_lookup (self->cache, key);
}

static void
extract_average_color (GdkTexture *texture, GdkRGBA *color_out)
{
  int width = gdk_texture_get_width (texture);
  int height = gdk_texture_get_height (texture);
  gsize stride = width * 4;  /* BGRA (GDK_MEMORY_DEFAULT) */
  guchar *pixels = g_malloc (stride * height);

  gdk_texture_download (texture, pixels, stride);

  guint64 r = 0, g = 0, b = 0;
  int count = 0;

  /* Sample every 4th pixel for performance
   * GDK_MEMORY_DEFAULT is B8G8R8A8 on little-endian systems
   * Skip transparent pixels (alpha < 128) */
  for (int i = 0; i < width * height * 4; i += 16)
    {
      guchar alpha = pixels[i + 3];  /* Alpha */
      if (alpha < 128)
        continue;  /* Skip transparent pixels */

      b += pixels[i];      /* Blue */
      g += pixels[i + 1];  /* Green */
      r += pixels[i + 2];  /* Red */
      count++;
    }

  if (count > 0)
    {
      color_out->red = (gdouble)(r / count) / 255.0;
      color_out->green = (gdouble)(g / count) / 255.0;
      color_out->blue = (gdouble)(b / count) / 255.0;
      color_out->alpha = 1.0;
    }
  else
    {
      color_out->red = 0.5;
      color_out->green = 0.5;
      color_out->blue = 0.5;
      color_out->alpha = 1.0;
    }

  g_free (pixels);
}

static void
on_avatar_bytes_received (GnalkHttpClient *client,
                           GBytes           *bytes,
                           const char       *content_type,
                           GError           *error,
                           gpointer          user_data)
{
  AvatarLoadData *data = user_data;
  GdkTexture *texture = NULL;

  if (error)
    {
      g_debug ("AvatarCache: Failed to fetch %s: %s", data->key, error->message);
    }
  else if (!bytes)
    {
      g_debug ("AvatarCache: No data received for %s", data->key);
    }

  if (!error && bytes)
    {
      g_autoptr(GError) tex_error = NULL;
      texture = gdk_texture_new_from_bytes (bytes, &tex_error);

      if (texture)
        {
          g_hash_table_insert (data->cache->cache,
                               g_strdup (data->key),
                               g_object_ref (texture));

          /* Save to disk cache */
          save_avatar_to_disk (data->key, bytes);

          /* Extract and cache color for user avatars */
          if (g_str_has_prefix (data->key, "user:"))
            {
              /* Parse user_id from key format "user:{user_id}:{size}" */
              const char *user_start = data->key + 5;  /* Skip "user:" */
              const char *size_sep = strrchr (user_start, ':');
              if (size_sep)
                {
                  g_autofree char *user_id = g_strndup (user_start, size_sep - user_start);
                  GdkRGBA *color = g_new0 (GdkRGBA, 1);
                  extract_average_color (texture, color);
                  g_hash_table_insert (data->cache->color_cache,
                                       g_strdup (user_id),
                                       color);
                }
            }
        }
    }

  data->callback (data->cache, texture, data->user_data);

  GList *pending_list = g_hash_table_lookup (data->cache->pending, data->key);
  for (GList *l = pending_list; l; l = l->next)
    {
      PendingCallbackData *pending = l->data;
      pending->callback (data->cache, texture, pending->user_data);
    }

  g_hash_table_remove (data->cache->pending, data->key);

  if (texture)
    g_object_unref (texture);

  avatar_load_data_free (data);
}

void
gnalk_avatar_cache_get_room_avatar (GnalkAvatarCache   *self,
                                      const char          *room_token,
                                      GnalkAvatarCallback callback,
                                      gpointer             user_data)
{
  g_return_if_fail (GNALK_IS_AVATAR_CACHE (self));
  g_return_if_fail (room_token != NULL);

  /* Include account_id in key for 1-to-1 rooms where avatar differs per account */
  g_autofree char *key = g_strdup_printf ("room:%ld:%s", (long) self->account_id, room_token);

  /* Check memory cache first */
  GdkTexture *cached = g_hash_table_lookup (self->cache, key);
  if (cached)
    {
      callback (self, cached, user_data);
      return;
    }

  /* Check disk cache before making HTTP request (saves bandwidth on app restart) */
  GdkTexture *disk_cached = load_avatar_from_disk (key);
  if (disk_cached)
    {
      g_hash_table_insert (self->cache, g_strdup (key), g_object_ref (disk_cached));
      callback (self, disk_cached, user_data);
      g_object_unref (disk_cached);
      return;
    }

  /* When offline or http_client not ready, nothing more we can do */
  if (!self->is_online || !gnalk_http_client_is_ready (self->http_client))
    {
      g_debug ("AvatarCache: Cannot load room avatar for %s (offline or http not ready)",
               room_token);
      callback (self, NULL, user_data);
      return;
    }

  if (g_hash_table_contains (self->pending, key))
    {
      PendingCallbackData *pending = g_new0 (PendingCallbackData, 1);
      pending->callback = callback;
      pending->user_data = user_data;

      GList *list = g_hash_table_lookup (self->pending, key);
      list = g_list_append (list, pending);
      g_hash_table_steal (self->pending, key);
      g_hash_table_insert (self->pending, g_strdup (key), list);
      return;
    }

  g_hash_table_insert (self->pending, g_strdup (key), NULL);

  AvatarLoadData *data = g_new0 (AvatarLoadData, 1);
  data->cache = g_object_ref (self);
  data->key = g_strdup (key);
  data->callback = callback;
  data->user_data = user_data;

  gnalk_http_client_get_room_avatar (self->http_client, room_token,
                                       on_avatar_bytes_received, data);
}

void
gnalk_avatar_cache_get_user_avatar (GnalkAvatarCache   *self,
                                      const char          *user_id,
                                      int                  size,
                                      GnalkAvatarCallback callback,
                                      gpointer             user_data)
{
  g_return_if_fail (GNALK_IS_AVATAR_CACHE (self));
  g_return_if_fail (user_id != NULL);

  g_autofree char *key = g_strdup_printf ("user:%s:%d", user_id, size);

  /* Check memory cache first */
  GdkTexture *cached = g_hash_table_lookup (self->cache, key);
  if (cached)
    {
      callback (self, cached, user_data);
      return;
    }

  /* Check disk cache before making HTTP request (saves bandwidth on app restart) */
  GdkTexture *disk_cached = load_avatar_from_disk (key);
  if (disk_cached)
    {
      g_hash_table_insert (self->cache, g_strdup (key), g_object_ref (disk_cached));

      /* Extract and cache color */
      GdkRGBA *color = g_new0 (GdkRGBA, 1);
      extract_average_color (disk_cached, color);
      g_hash_table_insert (self->color_cache, g_strdup (user_id), color);

      callback (self, disk_cached, user_data);
      g_object_unref (disk_cached);
      return;
    }

  /* When offline or http_client not ready, nothing more we can do */
  if (!self->is_online || !gnalk_http_client_is_ready (self->http_client))
    {
      g_debug ("AvatarCache: Cannot load user avatar for %s (offline or http not ready)",
               user_id);
      callback (self, NULL, user_data);
      return;
    }

  if (g_hash_table_contains (self->pending, key))
    {
      PendingCallbackData *pending = g_new0 (PendingCallbackData, 1);
      pending->callback = callback;
      pending->user_data = user_data;

      GList *list = g_hash_table_lookup (self->pending, key);
      list = g_list_append (list, pending);
      g_hash_table_steal (self->pending, key);
      g_hash_table_insert (self->pending, g_strdup (key), list);
      return;
    }

  g_hash_table_insert (self->pending, g_strdup (key), NULL);

  AvatarLoadData *data = g_new0 (AvatarLoadData, 1);
  data->cache = g_object_ref (self);
  data->key = g_strdup (key);
  data->callback = callback;
  data->user_data = user_data;

  gnalk_http_client_get_user_avatar (self->http_client, user_id, size,
                                       on_avatar_bytes_received, data);
}

void
gnalk_avatar_cache_get_file_preview (GnalkAvatarCache   *self,
                                       const char          *file_id,
                                       int                  width,
                                       int                  height,
                                       GnalkAvatarCallback callback,
                                       gpointer             user_data)
{
  g_return_if_fail (GNALK_IS_AVATAR_CACHE (self));
  g_return_if_fail (file_id != NULL);

  g_autofree char *key = g_strdup_printf ("file:%s:%dx%d", file_id, width, height);

  /* Check memory cache first */
  GdkTexture *cached = g_hash_table_lookup (self->cache, key);
  if (cached)
    {
      callback (self, cached, user_data);
      return;
    }

  /* Check disk cache before making HTTP request (saves bandwidth on app restart) */
  GdkTexture *disk_cached = load_avatar_from_disk (key);
  if (disk_cached)
    {
      g_hash_table_insert (self->cache, g_strdup (key), g_object_ref (disk_cached));
      callback (self, disk_cached, user_data);
      g_object_unref (disk_cached);
      return;
    }

  /* When offline or http_client not ready, nothing more we can do */
  if (!self->is_online || !gnalk_http_client_is_ready (self->http_client))
    {
      g_debug ("AvatarCache: Cannot load file preview for %s (offline or http not ready)",
               file_id);
      callback (self, NULL, user_data);
      return;
    }

  if (g_hash_table_contains (self->pending, key))
    {
      PendingCallbackData *pending = g_new0 (PendingCallbackData, 1);
      pending->callback = callback;
      pending->user_data = user_data;

      GList *list = g_hash_table_lookup (self->pending, key);
      list = g_list_append (list, pending);
      g_hash_table_steal (self->pending, key);
      g_hash_table_insert (self->pending, g_strdup (key), list);
      return;
    }

  g_hash_table_insert (self->pending, g_strdup (key), NULL);

  AvatarLoadData *data = g_new0 (AvatarLoadData, 1);
  data->cache = g_object_ref (self);
  data->key = g_strdup (key);
  data->callback = callback;
  data->user_data = user_data;

  gnalk_http_client_get_file_preview (self->http_client, file_id, width, height,
                                        on_avatar_bytes_received, data);
}

void
gnalk_avatar_cache_clear (GnalkAvatarCache *self)
{
  g_return_if_fail (GNALK_IS_AVATAR_CACHE (self));
  g_hash_table_remove_all (self->cache);
  g_hash_table_remove_all (self->color_cache);
}

gboolean
gnalk_avatar_cache_get_user_color (GnalkAvatarCache *self,
                                     const char        *user_id,
                                     GdkRGBA           *color_out)
{
  g_return_val_if_fail (GNALK_IS_AVATAR_CACHE (self), FALSE);
  g_return_val_if_fail (user_id != NULL, FALSE);
  g_return_val_if_fail (color_out != NULL, FALSE);

  GdkRGBA *cached = g_hash_table_lookup (self->color_cache, user_id);
  if (cached)
    {
      *color_out = *cached;
      return TRUE;
    }
  return FALSE;
}

static void
snapshot_round_avatar (GtkSnapshot *snapshot,
                        GdkTexture  *texture,
                        double       width,
                        double       height)
{
  GskRoundedRect rounded_rect;
  graphene_rect_t bounds;
  graphene_size_t corner_size;

  graphene_rect_init (&bounds, 0, 0, width, height);
  graphene_size_init (&corner_size, width / 2, height / 2);

  gsk_rounded_rect_init (&rounded_rect, &bounds,
                          &corner_size, &corner_size,
                          &corner_size, &corner_size);

  gtk_snapshot_push_rounded_clip (snapshot, &rounded_rect);
  gtk_snapshot_append_texture (snapshot, texture, &bounds);
  gtk_snapshot_pop (snapshot);
}

typedef struct
{
  GdkTexture *texture;
  int         size;
} RoundAvatarData;

static void
round_avatar_snapshot (GtkWidget   *widget,
                        GtkSnapshot *snapshot)
{
  RoundAvatarData *data = g_object_get_data (G_OBJECT (widget), "avatar-data");
  if (data && data->texture)
    snapshot_round_avatar (snapshot, data->texture, data->size, data->size);
}

static void
round_avatar_data_free (gpointer data)
{
  RoundAvatarData *avatar_data = data;
  g_clear_object (&avatar_data->texture);
  g_free (avatar_data);
}

GtkWidget *
gnalk_avatar_cache_create_round_avatar (GdkTexture *texture,
                                          int         size)
{
  GtkWidget *widget = gtk_drawing_area_new ();

  RoundAvatarData *data = g_new0 (RoundAvatarData, 1);
  data->texture = texture ? g_object_ref (texture) : NULL;
  data->size = size;

  g_object_set_data_full (G_OBJECT (widget), "avatar-data", data, round_avatar_data_free);

  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (widget),
                                   (GtkDrawingAreaDrawFunc) round_avatar_snapshot,
                                   NULL, NULL);

  gtk_widget_set_size_request (widget, size, size);

  return widget;
}

void
gnalk_avatar_cache_set_online (GnalkAvatarCache *self,
                                 gboolean           is_online)
{
  g_return_if_fail (GNALK_IS_AVATAR_CACHE (self));
  self->is_online = is_online;
}
