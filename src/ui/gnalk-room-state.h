/* gnalk-room-state.h
 *
 * Per-room state shared between GnalkRoomTab and GnalkWindow.
 * This is a plain C struct (not a GObject) embedded in each tab.
 * The window holds a pointer to the active tab's state.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _GnalkRoomState GnalkRoomState;

struct _GnalkRoomState
{
  /* Room identity */
  char       *room_token;
  char       *room_name;
  char       *room_description;
  char       *session_id;         /* HTTP session ID from room join */
  gint        room_type;

  /* Message store */
  GListStore *messages_store;

  /* Message loading state */
  gint64      last_known_message_id;
  gint64      oldest_loaded_id;
  gboolean    has_more_history;
  gboolean    initial_load_done;
  gboolean    skip_historical_call_messages;
  guint       message_load_generation;

  /* Scroll state (saved/restored by tab manager) */
  gint64      anchor_message_id;
  gdouble     anchor_offset;
  gboolean    is_at_bottom;
  gboolean    scroll_to_bottom;

  /* Compose state */
  gint64      reply_to_message_id;
  gint64      edit_message_id;
  gboolean    is_typing;

  /* Read status */
  gint64      last_common_read_message;
  gint64      last_read_message_id;
  gint64      first_unread_message_id;

  /* UI state */
  gint64      highlight_message_id;

  /* Context mode (jump-to-message) */
  gboolean    in_context_mode;
  gint64      context_target_message_id;
  gint64      context_newest_id;
};

G_END_DECLS
