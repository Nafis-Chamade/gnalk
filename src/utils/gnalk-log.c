#include "gnalk-log.h"
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>

static gboolean initialized = FALSE;

/* Separate file handles for HTTP, WebSocket, and notification logs */
static FILE *http_log_file = NULL;
static FILE *ws_log_file = NULL;
static FILE *notif_log_file = NULL;

/* Request/response body logging is opt-in (GNALK_LOG_HTTP_BODY=1). Bodies can
 * carry private conversation content, so they are OFF by default. Secret
 * fields are additionally redacted regardless of this flag (see redact_secrets). */
static gboolean log_http_body = FALSE;

/*
 * redact_secrets:
 * @text: JSON-ish text that may contain sensitive fields (may be NULL)
 *
 * Returns a newly-allocated copy of @text with the value of any known-sensitive
 * key ("password", "ticket", a JWT, etc.) replaced by "[redacted]". The key name
 * itself is dropped too so that grepping the log for "password"/"ticket"/... stays
 * clean. Returns NULL if @text is NULL/empty. Caller frees.
 */
static char *
redact_secrets (const char *text)
{
  static gsize once = 0;
  static GRegex *re = NULL;

  if (!text || !*text)
    return NULL;

  if (g_once_init_enter (&once))
    {
      /* Match "<key>" : "<value>" for sensitive keys, tolerating escaped quotes
       * inside the value. Case-insensitive so PassWord/Ticket are caught too. */
      re = g_regex_new (
        "\"(password|ticket|jwt|app_?password|proxytoken|secret|authorization)\""
        "\\s*:\\s*\"(?:\\\\.|[^\"\\\\])*\"",
        G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
      g_once_init_leave (&once, 1);
    }

  if (!re)
    return g_strdup (text);

  return g_regex_replace_literal (re, text, -1, 0, "\"[redacted]\"", 0, NULL);
}

/* Trace logging enabled flags per category */
static gboolean trace_enabled[GNALK_LOG_CAT_COUNT] = { FALSE };

/* Minimum severity to emit (GNALK_LOG_LEVEL env). GLib level flags are ordered
 * so that a *higher* numeric value means *less* severe (ERROR=4 … DEBUG=128);
 * a record is dropped when its level flag exceeds this threshold. Default keeps
 * G_LOG_LEVEL_MESSAGE so the startup sequence stays visible; set
 * GNALK_LOG_LEVEL=warning to silence the g_message flood. */
static GLogLevelFlags log_threshold = G_LOG_LEVEL_MESSAGE;

static GLogWriterOutput
gnalk_log_writer (GLogLevelFlags   log_level,
                  const GLogField *fields,
                  gsize            n_fields,
                  gpointer         user_data)
{
  GLogLevelFlags severity = log_level & G_LOG_LEVEL_MASK;

  /* Drop records less severe than the configured threshold. Errors/criticals/
   * warnings are never below G_LOG_LEVEL_MESSAGE, so they always pass. */
  if (severity > log_threshold)
    return G_LOG_WRITER_HANDLED;

  return g_log_writer_default (log_level, fields, n_fields, user_data);
}

static void
parse_log_level (const char *level)
{
  if (!level || !*level)
    return;

  if (g_ascii_strcasecmp (level, "error") == 0)
    log_threshold = G_LOG_LEVEL_ERROR;
  else if (g_ascii_strcasecmp (level, "critical") == 0)
    log_threshold = G_LOG_LEVEL_CRITICAL;
  else if (g_ascii_strcasecmp (level, "warning") == 0)
    log_threshold = G_LOG_LEVEL_WARNING;
  else if (g_ascii_strcasecmp (level, "message") == 0)
    log_threshold = G_LOG_LEVEL_MESSAGE;
  else if (g_ascii_strcasecmp (level, "info") == 0)
    log_threshold = G_LOG_LEVEL_INFO;
  else if (g_ascii_strcasecmp (level, "debug") == 0)
    log_threshold = G_LOG_LEVEL_DEBUG;
}

/* Category names for parsing GNALK_TRACE env var */
static const char *category_names[GNALK_LOG_CAT_COUNT] = {
  "messages",
  "room_switch",
  "scroll",
  "store",
  "polling",
  "database",
  "ui",
  "network"
};

/* Get formatted timestamp string */
static void
get_timestamp (char *buffer, size_t size)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  time_t now = tv.tv_sec;
  struct tm tm_buf;
  struct tm *tm_info = localtime_r (&now, &tm_buf);
  char time_str[32];
  strftime (time_str, sizeof (time_str), "%H:%M:%S", tm_info);
  g_snprintf (buffer, size, "[%s.%03ld]", time_str, tv.tv_usec / 1000);
}

/* Print timestamp to stdout */
static void
print_timestamp (void)
{
  char buffer[32];
  get_timestamp (buffer, sizeof (buffer));
  g_print ("%s ", buffer);
}

/* Print timestamp to file */
static void
fprint_timestamp (FILE *file)
{
  char buffer[32];
  get_timestamp (buffer, sizeof (buffer));
  fprintf (file, "%s ", buffer);
}

/* Parse GNALK_TRACE environment variable */
static void
parse_trace_config (const char *config)
{
  if (!config || !*config)
    return;

  /* "1" or "all" enables all categories */
  if (g_strcmp0 (config, "1") == 0 || g_ascii_strcasecmp (config, "all") == 0)
    {
      for (int i = 0; i < GNALK_LOG_CAT_COUNT; i++)
        trace_enabled[i] = TRUE;
      return;
    }

  /* Parse comma-separated category names */
  g_auto(GStrv) parts = g_strsplit (config, ",", -1);
  for (int i = 0; parts[i]; i++)
    {
      g_strstrip (parts[i]);
      for (int j = 0; j < GNALK_LOG_CAT_COUNT; j++)
        {
          if (g_ascii_strcasecmp (parts[i], category_names[j]) == 0)
            {
              trace_enabled[j] = TRUE;
              break;
            }
        }
    }
}

void
gnalk_log_init (void)
{
  if (initialized)
    return;

  initialized = TRUE;

  /* Open HTTP log file if specified */
  const char *http_log_path = g_getenv ("GNALK_HTTP_LOG");
  if (http_log_path && *http_log_path)
    {
      http_log_file = fopen (http_log_path, "a");
      if (http_log_file)
        {
          g_print ("=== HTTP traffic logging to: %s ===\n", http_log_path);
          fprint_timestamp (http_log_file);
          fprintf (http_log_file, "=== HTTP Log Session Started ===\n");
          fflush (http_log_file);
        }
      else
        {
          g_warning ("Failed to open HTTP log file: %s", http_log_path);
        }
    }

  /* Open WebSocket log file if specified */
  const char *ws_log_path = g_getenv ("GNALK_WS_LOG");
  if (ws_log_path && *ws_log_path)
    {
      ws_log_file = fopen (ws_log_path, "a");
      if (ws_log_file)
        {
          g_print ("=== WebSocket traffic logging to: %s ===\n", ws_log_path);
          fprint_timestamp (ws_log_file);
          fprintf (ws_log_file, "=== WebSocket Log Session Started ===\n");
          fflush (ws_log_file);
        }
      else
        {
          g_warning ("Failed to open WebSocket log file: %s", ws_log_path);
        }
    }

  /* Open notification log file if specified */
  const char *notif_log_path = g_getenv ("GNALK_NOTIF_LOG");
  if (notif_log_path && *notif_log_path)
    {
      notif_log_file = fopen (notif_log_path, "a");
      if (notif_log_file)
        {
          g_print ("=== Notification logging to: %s ===\n", notif_log_path);
          fprint_timestamp (notif_log_file);
          fprintf (notif_log_file, "=== Notification Log Session Started ===\n");
          fflush (notif_log_file);
        }
      else
        {
          g_warning ("Failed to open notification log file: %s", notif_log_path);
        }
    }

  /* Opt-in HTTP body logging (private content; off by default) */
  const char *http_body = g_getenv ("GNALK_LOG_HTTP_BODY");
  log_http_body = (http_body && (*http_body == '1' || g_ascii_strcasecmp (http_body, "true") == 0));

  /* Severity threshold + custom writer */
  parse_log_level (g_getenv ("GNALK_LOG_LEVEL"));
  g_log_set_writer_func (gnalk_log_writer, NULL, NULL);

  /* Parse trace logging configuration */
  const char *trace_config = g_getenv ("GNALK_TRACE");
  parse_trace_config (trace_config);

  /* Log enabled trace categories */
  GString *enabled = g_string_new (NULL);
  for (int i = 0; i < GNALK_LOG_CAT_COUNT; i++)
    {
      if (trace_enabled[i])
        {
          if (enabled->len > 0)
            g_string_append (enabled, ", ");
          g_string_append (enabled, category_names[i]);
        }
    }

  if (enabled->len > 0)
    g_print ("=== Trace logging enabled for: %s ===\n", enabled->str);

  g_string_free (enabled, TRUE);

  g_print ("=== Gnalk Logger Initialized ===\n");
}

void
gnalk_log_shutdown (void)
{
  if (http_log_file)
    {
      fprint_timestamp (http_log_file);
      fprintf (http_log_file, "=== HTTP Log Session Ended ===\n");
      fclose (http_log_file);
      http_log_file = NULL;
    }

  if (ws_log_file)
    {
      fprint_timestamp (ws_log_file);
      fprintf (ws_log_file, "=== WebSocket Log Session Ended ===\n");
      fclose (ws_log_file);
      ws_log_file = NULL;
    }

  if (notif_log_file)
    {
      fprint_timestamp (notif_log_file);
      fprintf (notif_log_file, "=== Notification Log Session Ended ===\n");
      fclose (notif_log_file);
      notif_log_file = NULL;
    }

  initialized = FALSE;
}

void
gnalk_log_http_request (const char *account,
                          const char *method,
                          const char *url,
                          const char *body)
{
  FILE *out = http_log_file ? http_log_file : stdout;

  if (http_log_file)
    fprint_timestamp (http_log_file);
  else
    print_timestamp ();

  if (account && *account)
    fprintf (out, "[%s] ", account);
  fprintf (out, ">>> HTTP %s %s", method, url);
  if (body && *body && log_http_body)
    {
      char *safe = redact_secrets (body);
      fprintf (out, "\n    Body: %s", safe ? safe : body);
      g_free (safe);
    }
  fprintf (out, "\n");

  if (http_log_file)
    fflush (http_log_file);
}

void
gnalk_log_http_response (const char *account,
                           guint       status_code,
                           const char *reason_phrase,
                           const char *body,
                           const char *url)
{
  FILE *out = http_log_file ? http_log_file : stdout;

  if (http_log_file)
    fprint_timestamp (http_log_file);
  else
    print_timestamp ();

  if (account && *account)
    fprintf (out, "[%s] ", account);
  fprintf (out, "<<< HTTP %u %s", status_code, reason_phrase ? reason_phrase : "");
  if (url && *url)
    fprintf (out, " %s", url);
  if (body && *body && log_http_body)
    {
      char *safe = redact_secrets (body);
      fprintf (out, "\n    Body: %s", safe ? safe : body);
      g_free (safe);
    }
  fprintf (out, "\n");

  if (http_log_file)
    fflush (http_log_file);
}

void
gnalk_log_ws_send (const char *account,
                     const char *message)
{
  FILE *out = ws_log_file ? ws_log_file : stdout;

  if (ws_log_file)
    fprint_timestamp (ws_log_file);
  else
    print_timestamp ();

  if (account && *account)
    fprintf (out, "[%s] ", account);
  {
    char *safe = redact_secrets (message);
    fprintf (out, ">>> %s\n", safe ? safe : (message ? message : "(null)"));
    g_free (safe);
  }

  if (ws_log_file)
    fflush (ws_log_file);
}

void
gnalk_log_ws_recv (const char *account,
                     const char *message)
{
  FILE *out = ws_log_file ? ws_log_file : stdout;

  if (ws_log_file)
    fprint_timestamp (ws_log_file);
  else
    print_timestamp ();

  if (account && *account)
    fprintf (out, "[%s] ", account);
  {
    char *safe = redact_secrets (message);
    fprintf (out, "<<< %s\n", safe ? safe : (message ? message : "(null)"));
    g_free (safe);
  }

  if (ws_log_file)
    fflush (ws_log_file);
}

void
gnalk_log_webrtc (const char *format, ...)
{
  print_timestamp ();
  g_print ("[WebRTC] ");

  va_list args;
  va_start (args, format);
  vprintf (format, args);
  va_end (args);

  g_print ("\n");
}

void
gnalk_log_notification (const char *format, ...)
{
  FILE *out = notif_log_file ? notif_log_file : stdout;

  if (notif_log_file)
    fprint_timestamp (notif_log_file);
  else
    print_timestamp ();

  fprintf (out, "[Notification] ");

  va_list args;
  va_start (args, format);
  vfprintf (out, format, args);
  va_end (args);

  fprintf (out, "\n");

  if (notif_log_file)
    fflush (notif_log_file);
}

gboolean
gnalk_log_trace_enabled (GnalkLogCategory category)
{
  if (category < 0 || category >= GNALK_LOG_CAT_COUNT)
    return FALSE;
  return trace_enabled[category];
}

void
gnalk_log_trace (GnalkLogCategory category,
                   const char       *format,
                   ...)
{
  if (category < 0 || category >= GNALK_LOG_CAT_COUNT)
    return;

  if (!trace_enabled[category])
    return;

  print_timestamp ();
  g_print ("[TRACE:%s] ", category_names[category]);

  va_list args;
  va_start (args, format);
  vprintf (format, args);
  va_end (args);

  g_print ("\n");
}
