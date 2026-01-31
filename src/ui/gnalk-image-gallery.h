#pragma once

#include <adwaita.h>
#include "gnalk-avatar-cache.h"
#include "gnalk-http-client.h"

G_BEGIN_DECLS

#define GNALK_TYPE_IMAGE_GALLERY (gnalk_image_gallery_get_type ())
G_DECLARE_FINAL_TYPE (GnalkImageGallery, gnalk_image_gallery, GNALK, IMAGE_GALLERY, AdwDialog)

/**
 * ImageInfo structure to hold image metadata for gallery navigation
 */
typedef struct {
  char   *file_id;
  char   *file_name;
  char   *file_path;      /* WebDAV path for download */
  char   *file_mimetype;
  gint64  message_id;
} GnalkImageInfo;

GnalkImageInfo *gnalk_image_info_new  (const char *file_id,
                                          const char *file_name,
                                          const char *file_path,
                                          const char *file_mimetype,
                                          gint64      message_id);
void             gnalk_image_info_free (GnalkImageInfo *info);

/**
 * Create a new image gallery widget
 */
GnalkImageGallery *gnalk_image_gallery_new (void);

/**
 * Set the HTTP client for downloading full images
 */
void gnalk_image_gallery_set_http_client (GnalkImageGallery *self,
                                            GnalkHttpClient   *http_client);

/**
 * Set the avatar cache for loading previews
 */
void gnalk_image_gallery_set_avatar_cache (GnalkImageGallery *self,
                                             GnalkAvatarCache  *avatar_cache);

/**
 * Set images to display in the gallery.
 * Takes ownership of the GPtrArray and its contents.
 * @param images GPtrArray of GnalkImageInfo*
 * @param current_index Index of the image to show first
 */
void gnalk_image_gallery_set_images (GnalkImageGallery *self,
                                       GPtrArray          *images,
                                       guint               current_index);

/**
 * Present the gallery as a dialog
 */
void gnalk_image_gallery_present (GnalkImageGallery *self,
                                    GtkWidget          *parent);

/**
 * Close the gallery dialog
 */
void gnalk_image_gallery_close (GnalkImageGallery *self);

/**
 * Navigate to the next image
 */
void gnalk_image_gallery_next (GnalkImageGallery *self);

/**
 * Navigate to the previous image
 */
void gnalk_image_gallery_previous (GnalkImageGallery *self);

G_END_DECLS
