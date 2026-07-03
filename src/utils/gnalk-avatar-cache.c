#include "gnalk-avatar-cache.h"
#include <glib/gstdio.h>
#include <libsoup/soup.h>

/* In-memory LRU cap: bound the texture hash table (keys multiply per size). */
#define AVATAR_CACHE_MAX_ENTRIES 256

/* Negative cache: remember failed/404 fetches so we don't retry on every bind. */
#define AVATAR_NEGATIVE_TTL_US (5 * 60 * G_USEC_PER_SEC)  /* 5 minutes */
#define AVATAR_NEGATIVE_MAX    512

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

/* Runs on a GTask worker thread: pure disk read + decode, no shared state. */
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
  SoupSession      *remote_session;  /* Lazy-init for unauthenticated remote avatar fetches */
  GHashTable       *cache;           /* key -> CacheEntry* (texture + LRU tick) */
  GHashTable       *pending;         /* key -> GList of PendingCallbackData* */
  GHashTable       *negative;        /* key -> gint64* (monotonic time of last failure) */
  GHashTable       *color_cache;     /* user_id -> GdkRGBA* */
  gint64            cache_tick;       /* Monotonically increasing LRU access counter */
  gboolean          is_online;
  gint64            account_id;      /* For per-account room avatar caching */
};

G_DEFINE_FINAL_TYPE (GnalkAvatarCache, gnalk_avatar_cache, G_TYPE_OBJECT)

/* --- In-memory LRU cache ------------------------------------------------- */

typedef struct
{
  GdkTexture *texture;
  gint64      tick;      /* Last-used LRU tick */
} CacheEntry;

static void
cache_entry_free (gpointer data)
{
  CacheEntry *entry = data;
  g_clear_object (&entry->texture);
  g_free (entry);
}

/* Look up a texture, updating its LRU tick on hit. Returns borrowed ref. */
static GdkTexture *
cache_lookup_texture (GnalkAvatarCache *self, const char *key)
{
  CacheEntry *entry = g_hash_table_lookup (self->cache, key);
  if (!entry)
    return NULL;
  entry->tick = ++self->cache_tick;
  return entry->texture;
}

/* Evict the least-recently-used entry. */
static void
cache_evict_lru (GnalkAvatarCache *self)
{
  GHashTableIter iter;
  gpointer k, v;
  const char *lru_key = NULL;
  gint64 lru_tick = G_MAXINT64;

  g_hash_table_iter_init (&iter, self->cache);
  while (g_hash_table_iter_next (&iter, &k, &v))
    {
      CacheEntry *entry = v;
      if (entry->tick < lru_tick)
        {
          lru_tick = entry->tick;
          lru_key = k;
        }
    }

  if (lru_key)
    g_hash_table_remove (self->cache, lru_key);
}

/* Store a texture, enforcing the LRU cap and clearing any negative entry. */
static void
cache_store_texture (GnalkAvatarCache *self, const char *key, GdkTexture *texture)
{
  g_hash_table_remove (self->negative, key);

  CacheEntry *existing = g_hash_table_lookup (self->cache, key);
  if (existing)
    {
      g_set_object (&existing->texture, texture);
      existing->tick = ++self->cache_tick;
      return;
    }

  if (g_hash_table_size (self->cache) >= AVATAR_CACHE_MAX_ENTRIES)
    cache_evict_lru (self);

  CacheEntry *entry = g_new0 (CacheEntry, 1);
  entry->texture = g_object_ref (texture);
  entry->tick = ++self->cache_tick;
  g_hash_table_insert (self->cache, g_strdup (key), entry);
}

/* --- Negative cache ------------------------------------------------------ */

/* TRUE if this key is known-missing and the record hasn't expired. */
static gboolean
negative_cache_contains (GnalkAvatarCache *self, const char *key)
{
  gint64 *ts = g_hash_table_lookup (self->negative, key);
  if (!ts)
    return FALSE;

  if (g_get_monotonic_time () - *ts > AVATAR_NEGATIVE_TTL_US)
    {
      g_hash_table_remove (self->negative, key);
      return FALSE;
    }
  return TRUE;
}

static void
negative_cache_evict_oldest (GnalkAvatarCache *self)
{
  GHashTableIter iter;
  gpointer k, v;
  const char *oldest_key = NULL;
  gint64 oldest = G_MAXINT64;

  g_hash_table_iter_init (&iter, self->negative);
  while (g_hash_table_iter_next (&iter, &k, &v))
    {
      gint64 *ts = v;
      if (*ts < oldest)
        {
          oldest = *ts;
          oldest_key = k;
        }
    }

  if (oldest_key)
    g_hash_table_remove (self->negative, oldest_key);
}

static void
negative_cache_record (GnalkAvatarCache *self, const char *key)
{
  if (g_hash_table_size (self->negative) >= AVATAR_NEGATIVE_MAX &&
      !g_hash_table_contains (self->negative, key))
    negative_cache_evict_oldest (self);

  gint64 *ts = g_new (gint64, 1);
  *ts = g_get_monotonic_time ();
  g_hash_table_insert (self->negative, g_strdup (key), ts);
}

/* --- Request/callback bookkeeping ---------------------------------------- */

typedef struct
{
  GnalkAvatarCallback callback;
  gpointer             user_data;
} PendingCallbackData;

typedef enum
{
  FETCH_ROOM,
  FETCH_USER,
  FETCH_FEDERATED,
  FETCH_REMOTE,
  FETCH_FILE,
} AvatarFetchType;

/* Carries a single request through the async disk load and, on a disk miss,
 * the network fetch that follows. Owned manually (GTask task_data destroy is
 * NULL) so it survives the GTask and hands off to the HTTP/Soup callbacks. */
typedef struct
{
  GnalkAvatarCache   *cache;       /* strong ref */
  char                *key;
  GnalkAvatarCallback callback;
  gpointer             user_data;

  AvatarFetchType      fetch_type;
  char                *room_token; /* FETCH_ROOM, FETCH_FEDERATED */
  char                *user_id;    /* FETCH_USER */
  char                *cloud_id;   /* FETCH_FEDERATED, FETCH_REMOTE */
  char                *file_id;    /* FETCH_FILE */
  int                  size;
  int                  width;
  int                  height;
} AvatarLoadData;

static void
avatar_load_data_free (AvatarLoadData *data)
{
  g_clear_object (&data->cache);
  g_free (data->key);
  g_free (data->room_token);
  g_free (data->user_id);
  g_free (data->cloud_id);
  g_free (data->file_id);
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

/* Register an extra waiter for an in-flight key (deduplicates concurrent
 * requests). Mirrors the original steal/insert dance. */
static void
add_pending_waiter (GnalkAvatarCache   *self,
                    const char          *key,
                    GnalkAvatarCallback callback,
                    gpointer             user_data)
{
  PendingCallbackData *pending = g_new0 (PendingCallbackData, 1);
  pending->callback = callback;
  pending->user_data = user_data;

  GList *list = g_hash_table_lookup (self->pending, key);
  list = g_list_append (list, pending);
  g_hash_table_steal (self->pending, key);
  g_hash_table_insert (self->pending, g_strdup (key), list);
}

/* Deliver a result to the primary callback and all waiters, then clear the
 * pending marker for this key. Does not free `data`. */
static void
deliver_result (GnalkAvatarCache *self, AvatarLoadData *data, GdkTexture *texture)
{
  data->callback (self, texture, data->user_data);

  GList *pending_list = g_hash_table_lookup (self->pending, data->key);
  for (GList *l = pending_list; l; l = l->next)
    {
      PendingCallbackData *pending = l->data;
      pending->callback (self, texture, pending->user_data);
    }

  g_hash_table_remove (self->pending, data->key);
}

static void
gnalk_avatar_cache_finalize (GObject *object)
{
  GnalkAvatarCache *self = GNALK_AVATAR_CACHE (object);

  g_hash_table_destroy (self->cache);
  g_hash_table_destroy (self->pending);
  g_hash_table_destroy (self->negative);
  g_hash_table_destroy (self->color_cache);
  g_clear_object (&self->http_client);
  g_clear_object (&self->remote_session);

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
                                        g_free, cache_entry_free);
  self->pending = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, pending_list_free);
  self->negative = g_hash_table_new_full (g_str_hash, g_str_equal,
                                           g_free, g_free);
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

  return cache_lookup_texture (self, key);
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

/* For "user:{user_id}:{size}" keys, extract and cache the average color. */
static void
maybe_extract_color (GnalkAvatarCache *self, const char *key, GdkTexture *texture)
{
  if (!g_str_has_prefix (key, "user:"))
    return;

  const char *user_start = key + 5;  /* Skip "user:" */
  const char *size_sep = strrchr (user_start, ':');
  if (!size_sep)
    return;

  g_autofree char *user_id = g_strndup (user_start, size_sep - user_start);
  GdkRGBA *color = g_new0 (GdkRGBA, 1);
  extract_average_color (texture, color);
  g_hash_table_insert (self->color_cache, g_strdup (user_id), color);
}

static void
on_avatar_bytes_received (GnalkHttpClient *client,
                           GBytes           *bytes,
                           const char       *content_type,
                           GError           *error,
                           gpointer          user_data)
{
  AvatarLoadData *data = user_data;
  GnalkAvatarCache *self = data->cache;
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
          cache_store_texture (self, data->key, texture);
          save_avatar_to_disk (data->key, bytes);
          maybe_extract_color (self, data->key, texture);
        }
    }

  /* Remember a failed/missing avatar so we don't refetch it on every bind. */
  if (!texture)
    negative_cache_record (self, data->key);

  deliver_result (self, data, texture);

  if (texture)
    g_object_unref (texture);

  avatar_load_data_free (data);
}

/* --- Async disk load ----------------------------------------------------- */

static void start_remote_fetch (GnalkAvatarCache *self, AvatarLoadData *data);

/* GTask worker thread: read + decode the on-disk cache without blocking the
 * main thread. */
static void
disk_load_thread (GTask        *task,
                  gpointer      source_object,
                  gpointer      task_data,
                  GCancellable *cancellable)
{
  AvatarLoadData *data = task_data;
  GdkTexture *texture = load_avatar_from_disk (data->key);
  g_task_return_pointer (task, texture, texture ? g_object_unref : NULL);
}

/* Main-thread completion: on a disk hit deliver the texture; on a miss fall
 * through to the network fetch appropriate for the request type. */
static void
on_disk_load_done (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  GnalkAvatarCache *self = GNALK_AVATAR_CACHE (source);
  GTask *task = G_TASK (result);
  AvatarLoadData *data = g_task_get_task_data (task);
  GdkTexture *texture = g_task_propagate_pointer (task, NULL);

  if (texture)
    {
      cache_store_texture (self, data->key, texture);
      maybe_extract_color (self, data->key, texture);
      deliver_result (self, data, texture);
      g_object_unref (texture);
      avatar_load_data_free (data);
      return;
    }

  /* Disk miss: fetch from network. Remote (unauthenticated) fetches don't
   * depend on http_client readiness or the online flag. */
  if (data->fetch_type != FETCH_REMOTE &&
      (!self->is_online || !gnalk_http_client_is_ready (self->http_client)))
    {
      g_debug ("AvatarCache: Cannot load %s (offline or http not ready)", data->key);
      deliver_result (self, data, NULL);
      avatar_load_data_free (data);
      return;
    }

  switch (data->fetch_type)
    {
    case FETCH_ROOM:
      gnalk_http_client_get_room_avatar (self->http_client, data->room_token,
                                          on_avatar_bytes_received, data);
      break;
    case FETCH_USER:
      gnalk_http_client_get_user_avatar (self->http_client, data->user_id, data->size,
                                          on_avatar_bytes_received, data);
      break;
    case FETCH_FEDERATED:
      gnalk_http_client_get_federated_user_avatar (self->http_client, data->room_token,
                                                    data->cloud_id, data->size,
                                                    on_avatar_bytes_received, data);
      break;
    case FETCH_FILE:
      gnalk_http_client_get_file_preview (self->http_client, data->file_id,
                                           data->width, data->height,
                                           on_avatar_bytes_received, data);
      break;
    case FETCH_REMOTE:
      start_remote_fetch (self, data);
      break;
    default:
      g_assert_not_reached ();
    }
}

/* Register the pending marker and kick off the async disk load. Takes
 * ownership of `data`. */
static void
start_disk_load (GnalkAvatarCache *self, AvatarLoadData *data)
{
  g_hash_table_insert (self->pending, g_strdup (data->key), NULL);

  GTask *task = g_task_new (self, NULL, on_disk_load_done, NULL);
  g_task_set_task_data (task, data, NULL);  /* freed manually, not by task */
  g_task_run_in_thread (task, disk_load_thread);
  g_object_unref (task);
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

  GdkTexture *cached = cache_lookup_texture (self, key);
  if (cached)
    {
      callback (self, cached, user_data);
      return;
    }

  if (negative_cache_contains (self, key))
    {
      callback (self, NULL, user_data);
      return;
    }

  if (g_hash_table_contains (self->pending, key))
    {
      add_pending_waiter (self, key, callback, user_data);
      return;
    }

  AvatarLoadData *data = g_new0 (AvatarLoadData, 1);
  data->cache = g_object_ref (self);
  data->key = g_strdup (key);
  data->callback = callback;
  data->user_data = user_data;
  data->fetch_type = FETCH_ROOM;
  data->room_token = g_strdup (room_token);

  start_disk_load (self, data);
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

  GdkTexture *cached = cache_lookup_texture (self, key);
  if (cached)
    {
      callback (self, cached, user_data);
      return;
    }

  if (negative_cache_contains (self, key))
    {
      callback (self, NULL, user_data);
      return;
    }

  if (g_hash_table_contains (self->pending, key))
    {
      add_pending_waiter (self, key, callback, user_data);
      return;
    }

  AvatarLoadData *data = g_new0 (AvatarLoadData, 1);
  data->cache = g_object_ref (self);
  data->key = g_strdup (key);
  data->callback = callback;
  data->user_data = user_data;
  data->fetch_type = FETCH_USER;
  data->user_id = g_strdup (user_id);
  data->size = size;

  start_disk_load (self, data);
}

void
gnalk_avatar_cache_get_federated_user_avatar (GnalkAvatarCache   *self,
                                                 const char          *room_token,
                                                 const char          *cloud_id,
                                                 int                  size,
                                                 GnalkAvatarCallback callback,
                                                 gpointer             user_data)
{
  g_return_if_fail (GNALK_IS_AVATAR_CACHE (self));
  g_return_if_fail (room_token != NULL);
  g_return_if_fail (cloud_id != NULL);

  g_autofree char *key = g_strdup_printf ("fed:%s:%d", cloud_id, size);

  GdkTexture *cached = cache_lookup_texture (self, key);
  if (cached)
    {
      callback (self, cached, user_data);
      return;
    }

  if (negative_cache_contains (self, key))
    {
      callback (self, NULL, user_data);
      return;
    }

  if (g_hash_table_contains (self->pending, key))
    {
      add_pending_waiter (self, key, callback, user_data);
      return;
    }

  AvatarLoadData *data = g_new0 (AvatarLoadData, 1);
  data->cache = g_object_ref (self);
  data->key = g_strdup (key);
  data->callback = callback;
  data->user_data = user_data;
  data->fetch_type = FETCH_FEDERATED;
  data->room_token = g_strdup (room_token);
  data->cloud_id = g_strdup (cloud_id);
  data->size = size;

  start_disk_load (self, data);
}

/* Callback for remote (unauthenticated) avatar fetch via SoupSession */
static void
on_remote_avatar_response (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  AvatarLoadData *data = user_data;
  GnalkAvatarCache *self = data->cache;
  g_autoptr(GError) error = NULL;
  GdkTexture *texture = NULL;

  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);

  if (error)
    {
      g_message ("AvatarCache: Remote avatar fetch failed for %s: %s", data->key, error->message);
    }

  if (!error && bytes)
    {
      g_autoptr(GError) tex_error = NULL;
      texture = gdk_texture_new_from_bytes (bytes, &tex_error);

      if (texture)
        {
          cache_store_texture (self, data->key, texture);
          save_avatar_to_disk (data->key, bytes);
          g_message ("AvatarCache: Remote avatar loaded for %s", data->key);
        }
      else
        {
          g_message ("AvatarCache: Remote avatar bytes not a valid image for %s: %s",
                     data->key, tex_error->message);
        }
    }

  if (bytes)
    g_bytes_unref (bytes);

  if (!texture)
    negative_cache_record (self, data->key);

  deliver_result (self, data, texture);

  if (texture)
    g_object_unref (texture);

  avatar_load_data_free (data);
}

/* Issue the remote avatar HTTP request (called after a disk miss). */
static void
start_remote_fetch (GnalkAvatarCache *self, AvatarLoadData *data)
{
  const char *at = strchr (data->cloud_id, '@');

  /* cloud_id was validated before the disk load; guard defensively. */
  if (!at || at == data->cloud_id || *(at + 1) == '\0')
    {
      deliver_result (self, data, NULL);
      avatar_load_data_free (data);
      return;
    }

  if (!self->remote_session)
    self->remote_session = soup_session_new ();

  g_autofree char *user_part = g_strndup (data->cloud_id, at - data->cloud_id);
  const char *server = at + 1;
  g_autofree char *url = g_strdup_printf ("https://%s/index.php/avatar/%s/%d",
                                          server, user_part, data->size);
  g_message ("AvatarCache: Fetching remote avatar from %s", url);

  SoupMessage *msg = soup_message_new ("GET", url);
  if (!msg)
    {
      g_debug ("AvatarCache: Failed to create message for %s", url);
      deliver_result (self, data, NULL);
      avatar_load_data_free (data);
      return;
    }

  soup_session_send_and_read_async (self->remote_session, msg, G_PRIORITY_DEFAULT,
                                    NULL, on_remote_avatar_response, data);
  g_object_unref (msg);
}

void
gnalk_avatar_cache_get_remote_user_avatar (GnalkAvatarCache   *self,
                                           const char          *cloud_id,
                                           int                  size,
                                           GnalkAvatarCallback callback,
                                           gpointer             user_data)
{
  g_return_if_fail (GNALK_IS_AVATAR_CACHE (self));
  g_return_if_fail (cloud_id != NULL);

  g_autofree char *key = g_strdup_printf ("remote:%s:%d", cloud_id, size);

  GdkTexture *cached = cache_lookup_texture (self, key);
  if (cached)
    {
      callback (self, cached, user_data);
      return;
    }

  if (negative_cache_contains (self, key))
    {
      callback (self, NULL, user_data);
      return;
    }

  /* Parse cloud_id: "user@server" */
  const char *at = strchr (cloud_id, '@');
  if (!at || at == cloud_id || *(at + 1) == '\0')
    {
      g_debug ("AvatarCache: Invalid cloud_id for remote avatar: %s", cloud_id);
      callback (self, NULL, user_data);
      return;
    }

  if (g_hash_table_contains (self->pending, key))
    {
      add_pending_waiter (self, key, callback, user_data);
      return;
    }

  AvatarLoadData *data = g_new0 (AvatarLoadData, 1);
  data->cache = g_object_ref (self);
  data->key = g_strdup (key);
  data->callback = callback;
  data->user_data = user_data;
  data->fetch_type = FETCH_REMOTE;
  data->cloud_id = g_strdup (cloud_id);
  data->size = size;

  start_disk_load (self, data);
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

  GdkTexture *cached = cache_lookup_texture (self, key);
  if (cached)
    {
      callback (self, cached, user_data);
      return;
    }

  if (negative_cache_contains (self, key))
    {
      callback (self, NULL, user_data);
      return;
    }

  if (g_hash_table_contains (self->pending, key))
    {
      add_pending_waiter (self, key, callback, user_data);
      return;
    }

  AvatarLoadData *data = g_new0 (AvatarLoadData, 1);
  data->cache = g_object_ref (self);
  data->key = g_strdup (key);
  data->callback = callback;
  data->user_data = user_data;
  data->fetch_type = FETCH_FILE;
  data->file_id = g_strdup (file_id);
  data->width = width;
  data->height = height;

  start_disk_load (self, data);
}

void
gnalk_avatar_cache_clear (GnalkAvatarCache *self)
{
  g_return_if_fail (GNALK_IS_AVATAR_CACHE (self));
  g_hash_table_remove_all (self->cache);
  g_hash_table_remove_all (self->negative);
  g_hash_table_remove_all (self->color_cache);
}

void
gnalk_avatar_cache_invalidate (GnalkAvatarCache *self,
                                  const char        *key)
{
  g_return_if_fail (GNALK_IS_AVATAR_CACHE (self));
  g_return_if_fail (key != NULL);

  g_hash_table_remove (self->cache, key);
  g_hash_table_remove (self->negative, key);

  /* Delete disk cache file */
  g_autofree char *path = get_avatar_cache_path (key);
  if (g_file_test (path, G_FILE_TEST_EXISTS))
    g_unlink (path);
}

void
gnalk_avatar_cache_invalidate_room (GnalkAvatarCache *self,
                                       const char        *room_token)
{
  g_return_if_fail (GNALK_IS_AVATAR_CACHE (self));
  g_return_if_fail (room_token != NULL);

  g_autofree char *key = g_strdup_printf ("room:%ld:%s", (long) self->account_id, room_token);
  gnalk_avatar_cache_invalidate (self, key);
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

void
gnalk_avatar_cache_set_online (GnalkAvatarCache *self,
                                 gboolean           is_online)
{
  g_return_if_fail (GNALK_IS_AVATAR_CACHE (self));
  self->is_online = is_online;
}
