/* gnalk-upload-preview.h
 *
 * Widget displaying a file being uploaded or recently uploaded.
 * Shows image preview for images, file icon+name for other files.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNALK_TYPE_UPLOAD_PREVIEW (gnalk_upload_preview_get_type ())
G_DECLARE_FINAL_TYPE (GnalkUploadPreview, gnalk_upload_preview, GNALK, UPLOAD_PREVIEW, GtkBox)

GnalkUploadPreview *gnalk_upload_preview_new (void);

/**
 * gnalk_upload_preview_set_file:
 * @self: the upload preview widget
 * @file_path: local file path
 * @file_name: display name of the file
 * @mime_type: MIME type of the file
 * @is_uploading: whether the file is currently being uploaded
 *
 * Sets the file to display. Shows image preview for images,
 * file box for other file types.
 */
void gnalk_upload_preview_set_file (GnalkUploadPreview *self,
                                     const char          *file_path,
                                     const char          *file_name,
                                     const char          *mime_type,
                                     gboolean             is_uploading);

/**
 * gnalk_upload_preview_set_complete:
 * @self: the upload preview widget
 *
 * Marks the upload as complete, shows checkmark.
 */
void gnalk_upload_preview_set_complete (GnalkUploadPreview *self);

/**
 * gnalk_upload_preview_get_file_path:
 * @self: the upload preview widget
 *
 * Gets the local file path for opening the file.
 *
 * Returns: (nullable): the file path
 */
const char *gnalk_upload_preview_get_file_path (GnalkUploadPreview *self);

/**
 * gnalk_upload_preview_reset:
 * @self: the upload preview widget
 *
 * Clears the preview and hides the widget.
 */
void gnalk_upload_preview_reset (GnalkUploadPreview *self);

G_END_DECLS
