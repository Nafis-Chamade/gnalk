#include "gnalk-log.h"
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>

static gboolean initialized = FALSE;

/* Separate file handles for HTTP and WebSocket logs */
static FILE *http_log_file = NULL;
static FILE *ws_log_file = NULL;

/* Trace logging enabled flags per category */
static gboolean trace_enabled[GNALK_LOG_CAT_COUNT] = { FALSE };

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

  initialized = FALSE;
}

void
gnalk_log_http_request (const char *method,
                          const char *url,
                          const char *body)
{
  FILE *out = http_log_file ? http_log_file : stdout;

  if (http_log_file)
    fprint_timestamp (http_log_file);
  else
    print_timestamp ();

  fprintf (out, ">>> HTTP Request\n");
  fprintf (out, "    Method: %s\n", method);
  fprintf (out, "    URL: %s\n", url);
  if (body && *body)
    fprintf (out, "    Body: %s\n", body);
  fprintf (out, "\n");

  if (http_log_file)
    fflush (http_log_file);
}

void
gnalk_log_http_response (guint       status_code,
                           const char *reason_phrase,
                           const char *body,
                           const char *url)
{
  FILE *out = http_log_file ? http_log_file : stdout;

  if (http_log_file)
    fprint_timestamp (http_log_file);
  else
    print_timestamp ();

  fprintf (out, "<<< HTTP Response\n");
  if (url && *url)
    fprintf (out, "    URL: %s\n", url);
  fprintf (out, "    Status: %u %s\n", status_code, reason_phrase ? reason_phrase : "");
  if (body && *body)
    fprintf (out, "    Body: %s\n", body);
  fprintf (out, "\n");

  if (http_log_file)
    fflush (http_log_file);
}

void
gnalk_log_ws_send (const char *message)
{
  FILE *out = ws_log_file ? ws_log_file : stdout;

  if (ws_log_file)
    fprint_timestamp (ws_log_file);
  else
    print_timestamp ();

  fprintf (out, ">>> WS Sending: %s\n", message ? message : "(null)");

  if (ws_log_file)
    fflush (ws_log_file);
}

void
gnalk_log_ws_recv (const char *message)
{
  FILE *out = ws_log_file ? ws_log_file : stdout;

  if (ws_log_file)
    fprint_timestamp (ws_log_file);
  else
    print_timestamp ();

  fprintf (out, "<<< WS Received: %s\n", message ? message : "(null)");

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
