#pragma once

#include <glib.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

/* Log categories for trace logging */
typedef enum {
  GNALK_LOG_CAT_MESSAGES,    /* Message loading, display, storage */
  GNALK_LOG_CAT_ROOM_SWITCH, /* Room switching, tab management */
  GNALK_LOG_CAT_SCROLL,      /* Scroll position, anchors */
  GNALK_LOG_CAT_STORE,       /* GListStore operations */
  GNALK_LOG_CAT_POLLING,     /* Message polling */
  GNALK_LOG_CAT_DATABASE,    /* Database operations */
  GNALK_LOG_CAT_UI,          /* UI updates */
  GNALK_LOG_CAT_NETWORK,     /* Network state */
  GNALK_LOG_CAT_COUNT
} GnalkLogCategory;

/**
 * gnalk_log_init:
 *
 * Initialize the logging system. Call once at startup.
 * Opens log files based on environment variables:
 * - GNALK_HTTP_LOG: Path for HTTP traffic log (default: none, goes to main log)
 * - GNALK_WS_LOG: Path for WebSocket traffic log (default: none, goes to main log)
 * - GNALK_TRACE: Enable trace logging (set to "1" or category names like "messages,room_switch")
 */
void gnalk_log_init (void);

/**
 * gnalk_log_shutdown:
 *
 * Close log files and clean up. Call at application exit.
 */
void gnalk_log_shutdown (void);

/**
 * gnalk_log_http_request:
 * @method: HTTP method (GET, POST, etc.)
 * @url: Request URL
 * @body: Request body (may be NULL)
 *
 * Log an outgoing HTTP request.
 */
void gnalk_log_http_request (const char *method,
                               const char *url,
                               const char *body);

/**
 * gnalk_log_http_response:
 * @status_code: HTTP status code
 * @reason_phrase: Status reason phrase
 * @body: Response body (may be NULL)
 * @url: Request URL for context
 *
 * Log an incoming HTTP response.
 */
void gnalk_log_http_response (guint       status_code,
                                const char *reason_phrase,
                                const char *body,
                                const char *url);

/**
 * gnalk_log_ws_send:
 * @message: WebSocket message being sent
 *
 * Log an outgoing WebSocket message.
 */
void gnalk_log_ws_send (const char *message);

/**
 * gnalk_log_ws_recv:
 * @message: WebSocket message received
 *
 * Log an incoming WebSocket message.
 */
void gnalk_log_ws_recv (const char *message);

/**
 * gnalk_log_webrtc:
 * @format: printf-style format string
 * @...: Format arguments
 *
 * Log WebRTC-related messages.
 */
void gnalk_log_webrtc (const char *format, ...) G_GNUC_PRINTF (1, 2);

/**
 * gnalk_log_trace:
 * @category: Log category
 * @format: printf-style format string
 * @...: Format arguments
 *
 * Log a trace message for detailed debugging.
 * Only outputs if trace logging is enabled for this category.
 */
void gnalk_log_trace (GnalkLogCategory category,
                        const char       *format,
                        ...) G_GNUC_PRINTF (2, 3);

/**
 * gnalk_log_trace_enabled:
 * @category: Log category
 *
 * Check if trace logging is enabled for a category.
 *
 * Returns: TRUE if trace is enabled for this category
 */
gboolean gnalk_log_trace_enabled (GnalkLogCategory category);

/* Convenience macros for trace logging */
#define GNALK_TRACE_MESSAGES(fmt, ...) \
  gnalk_log_trace (GNALK_LOG_CAT_MESSAGES, fmt, ##__VA_ARGS__)

#define GNALK_TRACE_ROOM_SWITCH(fmt, ...) \
  gnalk_log_trace (GNALK_LOG_CAT_ROOM_SWITCH, fmt, ##__VA_ARGS__)

#define GNALK_TRACE_SCROLL(fmt, ...) \
  gnalk_log_trace (GNALK_LOG_CAT_SCROLL, fmt, ##__VA_ARGS__)

#define GNALK_TRACE_STORE(fmt, ...) \
  gnalk_log_trace (GNALK_LOG_CAT_STORE, fmt, ##__VA_ARGS__)

#define GNALK_TRACE_POLLING(fmt, ...) \
  gnalk_log_trace (GNALK_LOG_CAT_POLLING, fmt, ##__VA_ARGS__)

#define GNALK_TRACE_DATABASE(fmt, ...) \
  gnalk_log_trace (GNALK_LOG_CAT_DATABASE, fmt, ##__VA_ARGS__)

#define GNALK_TRACE_UI(fmt, ...) \
  gnalk_log_trace (GNALK_LOG_CAT_UI, fmt, ##__VA_ARGS__)

#define GNALK_TRACE_NETWORK(fmt, ...) \
  gnalk_log_trace (GNALK_LOG_CAT_NETWORK, fmt, ##__VA_ARGS__)

G_END_DECLS
