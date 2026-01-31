#pragma once

#include <gtk/gtk.h>
#include "gnalk-http-client.h"

G_BEGIN_DECLS

#define GNALK_TYPE_AVATAR_CACHE (gnalk_avatar_cache_get_type ())
G_DECLARE_FINAL_TYPE (GnalkAvatarCache, gnalk_avatar_cache, GNALK, AVATAR_CACHE, GObject)

typedef void (*GnalkAvatarCallback) (GnalkAvatarCache *cache,
                                       GdkTexture        *texture,
                                       gpointer           user_data);

GnalkAvatarCache *gnalk_avatar_cache_new (GnalkHttpClient *http_client,
                                            gint64            account_id);

void gnalk_avatar_cache_get_room_avatar (GnalkAvatarCache   *self,
                                           const char          *room_token,
                                           GnalkAvatarCallback callback,
                                           gpointer             user_data);

void gnalk_avatar_cache_get_user_avatar (GnalkAvatarCache   *self,
                                           const char          *user_id,
                                           int                  size,
                                           GnalkAvatarCallback callback,
                                           gpointer             user_data);

void gnalk_avatar_cache_get_file_preview (GnalkAvatarCache   *self,
                                            const char          *file_id,
                                            int                  width,
                                            int                  height,
                                            GnalkAvatarCallback callback,
                                            gpointer             user_data);

GdkTexture *gnalk_avatar_cache_lookup (GnalkAvatarCache *self,
                                         const char        *key);

void gnalk_avatar_cache_clear (GnalkAvatarCache *self);

GtkWidget *gnalk_avatar_cache_create_round_avatar (GdkTexture *texture,
                                                     int         size);

gboolean gnalk_avatar_cache_get_user_color (GnalkAvatarCache *self,
                                              const char        *user_id,
                                              GdkRGBA           *color_out);

void gnalk_avatar_cache_set_online (GnalkAvatarCache *self,
                                      gboolean           is_online);

G_END_DECLS
