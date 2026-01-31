#pragma once

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GNALK_TYPE_SCREEN_CAPTURE (gnalk_screen_capture_get_type ())
G_DECLARE_FINAL_TYPE (GnalkScreenCapture, gnalk_screen_capture, GNALK, SCREEN_CAPTURE, GObject)

typedef enum {
  GNALK_SCREEN_CAPTURE_SOURCE_MONITOR = 1,
  GNALK_SCREEN_CAPTURE_SOURCE_WINDOW  = 2,
  GNALK_SCREEN_CAPTURE_SOURCE_VIRTUAL = 4,
} GnalkScreenCaptureSourceType;

typedef enum {
  GNALK_SCREEN_CAPTURE_CURSOR_HIDDEN   = 1,
  GNALK_SCREEN_CAPTURE_CURSOR_EMBEDDED = 2,
  GNALK_SCREEN_CAPTURE_CURSOR_METADATA = 4,
} GnalkScreenCaptureCursorMode;

GnalkScreenCapture *gnalk_screen_capture_new (void);

void gnalk_screen_capture_start (GnalkScreenCapture *self);

void gnalk_screen_capture_stop (GnalkScreenCapture *self);

gboolean gnalk_screen_capture_is_active (GnalkScreenCapture *self);

guint32 gnalk_screen_capture_get_pipewire_node_id (GnalkScreenCapture *self);

int gnalk_screen_capture_get_pipewire_fd (GnalkScreenCapture *self);

/* Signals:
 * - "capture-started" (guint32 node_id, int fd)
 *     Emitted when screen capture successfully started with PipeWire info
 * - "capture-stopped" ()
 *     Emitted when screen capture stopped (by user or error)
 * - "capture-failed" (const char *error_message)
 *     Emitted when screen capture fails (portal denied, cancelled, etc.)
 */

G_END_DECLS
