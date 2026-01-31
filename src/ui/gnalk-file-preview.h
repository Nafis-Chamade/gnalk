/* gnalk-file-preview.h
 *
 * Widget displaying a server-hosted file preview.
 * Shows download state, cached status, and preview thumbnail.
 */

#pragma once

#include <gtk/gtk.h>
#include "gnalk-avatar-cache.h"

G_BEGIN_DECLS

#define GNALK_TYPE_FILE_PREVIEW (gnalk_file_preview_get_type ())
G_DECLARE_FINAL_TYPE (GnalkFilePreview, gnalk_file_preview, GNALK, FILE_PREVIEW, GtkWidget)

GnalkFilePreview *gnalk_file_preview_new (void);

/**
 * gnalk_file_preview_set_file:
 * @self: the file preview widget
 * @file_id: server file ID
 * @file_name: display name
 * @file_mimetype: MIME type
 * @file_link: download link
 * @file_path: file path on server
 * @avatar_cache: avatar cache for loading preview
 *
 * Sets the file to preview and starts loading the thumbnail.
 */
void gnalk_file_preview_set_file (GnalkFilePreview *self,
                                   const char        *file_id,
                                   const char        *file_name,
                                   const char        *file_mimetype,
                                   const char        *file_link,
                                   const char        *file_path,
                                   GnalkAvatarCache *avatar_cache);

/**
 * gnalk_file_preview_set_downloading:
 * @self: the file preview widget
 * @downloading: whether the file is being downloaded
 *
 * Shows/hides the download spinner.
 */
void gnalk_file_preview_set_downloading (GnalkFilePreview *self,
                                          gboolean           downloading);

/**
 * gnalk_file_preview_set_cached:
 * @self: the file preview widget
 * @cached: whether the file is locally cached
 *
 * Shows cached icon or download overlay.
 */
void gnalk_file_preview_set_cached (GnalkFilePreview *self,
                                     gboolean           cached);

/**
 * gnalk_file_preview_get_file_id:
 * @self: the file preview widget
 *
 * Returns: (nullable): the file ID
 */
const char *gnalk_file_preview_get_file_id (GnalkFilePreview *self);

/**
 * gnalk_file_preview_get_file_name:
 * @self: the file preview widget
 *
 * Returns: (nullable): the file name
 */
const char *gnalk_file_preview_get_file_name (GnalkFilePreview *self);

/**
 * gnalk_file_preview_get_file_mimetype:
 * @self: the file preview widget
 *
 * Returns: (nullable): the MIME type
 */
const char *gnalk_file_preview_get_file_mimetype (GnalkFilePreview *self);

/**
 * gnalk_file_preview_get_file_link:
 * @self: the file preview widget
 *
 * Returns: (nullable): the download link
 */
const char *gnalk_file_preview_get_file_link (GnalkFilePreview *self);

/**
 * gnalk_file_preview_get_file_path:
 * @self: the file preview widget
 *
 * Returns: (nullable): the server file path
 */
const char *gnalk_file_preview_get_file_path (GnalkFilePreview *self);

/**
 * gnalk_file_preview_reset:
 * @self: the file preview widget
 *
 * Clears the preview and hides the widget.
 */
void gnalk_file_preview_reset (GnalkFilePreview *self);

G_END_DECLS
