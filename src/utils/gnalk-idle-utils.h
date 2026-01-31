/* gnalk-idle-utils.h
 *
 * Standardized idle callback helpers for safe async operations.
 *
 * These utilities provide a consistent pattern for scheduling callbacks
 * on the main thread with proper cleanup handling.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * GnalkIdleContext:
 * @callback: The function to call when idle
 * @user_data: User data to pass to the callback
 * @destroy: Destroy notify function for user_data (may be NULL)
 *
 * Context structure for idle callbacks. Use gnalk_idle_add() or
 * gnalk_idle_add_once() to schedule callbacks rather than managing
 * this structure directly.
 */
typedef struct {
  GSourceFunc    callback;
  gpointer       user_data;
  GDestroyNotify destroy;
} GnalkIdleContext;

/**
 * gnalk_idle_add:
 * @callback: Function to call when idle (return TRUE to keep running, FALSE to stop)
 * @data: User data to pass to callback
 * @destroy: Destroy notify function for @data (may be NULL)
 *
 * Schedule a callback to run when the main loop is idle.
 * The @destroy function is called when the source is removed.
 *
 * Returns: The source ID (can be used with g_source_remove())
 */
guint gnalk_idle_add (GSourceFunc    callback,
                       gpointer       data,
                       GDestroyNotify destroy);

/**
 * gnalk_idle_add_once:
 * @callback: Function to call once when idle (must return G_SOURCE_REMOVE)
 * @data: User data to pass to callback (will be freed with g_free after callback)
 *
 * Schedule a one-shot callback to run when the main loop is idle.
 * The @data is automatically freed with g_free() after the callback returns.
 * This is a convenience wrapper for the common case of a single invocation
 * with heap-allocated user data.
 *
 * Note: @callback should return G_SOURCE_REMOVE. If it returns G_SOURCE_CONTINUE,
 * the callback will run again but @data will have been freed.
 *
 * Returns: The source ID (can be used with g_source_remove())
 */
guint gnalk_idle_add_once (GSourceFunc callback,
                            gpointer    data);

/**
 * gnalk_timeout_add_seconds_once:
 * @seconds: Timeout in seconds
 * @callback: Function to call after timeout
 * @data: User data to pass to callback (will be freed with g_free after callback)
 *
 * Schedule a one-shot callback to run after a timeout.
 * The @data is automatically freed with g_free() after the callback returns.
 *
 * Returns: The source ID (can be used with g_source_remove())
 */
guint gnalk_timeout_add_seconds_once (guint       seconds,
                                       GSourceFunc callback,
                                       gpointer    data);

G_END_DECLS
