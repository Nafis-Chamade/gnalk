/* gnalk-room-tab-manager.h
 *
 * Tab-based room message management.
 * Each room has its own persistent message store with scroll state.
 */

#pragma once

#include <gtk/gtk.h>
#include "gnalk-message-data.h"

G_BEGIN_DECLS

/* GnalkRoomTab - Per-room state (not a GObject, just a struct) */
typedef struct _GnalkRoomTab GnalkRoomTab;

struct _GnalkRoomTab
{
  char       *room_token;
  GListStore *messages_store;       /* This tab's messages */
  gint64      oldest_loaded_id;     /* For history loading */
  gint64      newest_loaded_id;     /* For polling */
  gint64      anchor_message_id;    /* Scroll anchor when backgrounded */
  gdouble     anchor_offset;        /* Pixel offset from anchor */
  gboolean    is_at_bottom;         /* Was at bottom when backgrounded */
  gint64      last_access_time;     /* For LRU eviction (monotonic microseconds) */
  gboolean    has_more_history;     /* Server has older messages */
  gboolean    initial_load_done;    /* Messages have been loaded at least once */
  gint64      last_read_message_id; /* For unread message marking */
};

/* GnalkRoomTabManager - Manages multiple room tabs */
#define GNALK_TYPE_ROOM_TAB_MANAGER (gnalk_room_tab_manager_get_type ())
G_DECLARE_FINAL_TYPE (GnalkRoomTabManager, gnalk_room_tab_manager, GNALK, ROOM_TAB_MANAGER, GObject)

/**
 * gnalk_room_tab_manager_new:
 * @max_tabs: Maximum number of tabs to keep (0 for unlimited)
 * @height_multiplier: Viewports worth of messages to keep in backgrounded tabs
 *
 * Creates a new room tab manager.
 *
 * Returns: (transfer full): A new #GnalkRoomTabManager
 */
GnalkRoomTabManager *gnalk_room_tab_manager_new (guint   max_tabs,
                                                    gdouble height_multiplier);

/**
 * gnalk_room_tab_manager_get_tab:
 * @self: The tab manager
 * @room_token: The room token
 *
 * Gets or creates a tab for a room. Creates a new tab with an empty
 * GListStore if the tab doesn't exist.
 *
 * Returns: (transfer none): The tab for the room (owned by manager)
 */
GnalkRoomTab *gnalk_room_tab_manager_get_tab (GnalkRoomTabManager *self,
                                                 const char           *room_token);

/**
 * gnalk_room_tab_manager_has_tab:
 * @self: The tab manager
 * @room_token: The room token
 *
 * Checks if a tab exists for a room without creating one.
 *
 * Returns: TRUE if a tab exists for the room
 */
gboolean gnalk_room_tab_manager_has_tab (GnalkRoomTabManager *self,
                                           const char           *room_token);

/**
 * gnalk_room_tab_manager_background_tab:
 * @self: The tab manager
 * @room_token: The room token
 * @anchor_id: Message ID to anchor scroll position
 * @anchor_offset: Pixel offset from anchor message top
 * @at_bottom: Whether the view was at the bottom
 * @viewport_height: Current viewport height for trimming
 * @viewport_width: Current viewport width for measuring
 *
 * Called when leaving a room. Saves scroll anchor and optionally
 * trims messages to keep only those within the configured height
 * window around the anchor.
 */
void gnalk_room_tab_manager_background_tab (GnalkRoomTabManager *self,
                                              const char           *room_token,
                                              gint64                anchor_id,
                                              gdouble               anchor_offset,
                                              gboolean              at_bottom,
                                              int                   viewport_height,
                                              int                   viewport_width);

/**
 * gnalk_room_tab_manager_foreground_tab:
 * @self: The tab manager
 * @room_token: The room token
 * @anchor_id: (out): Message ID to restore scroll to
 * @anchor_offset: (out): Pixel offset for scroll restoration
 * @was_at_bottom: (out): Whether view should scroll to bottom
 *
 * Called when entering a room. Returns anchor info for scroll restoration.
 * Updates the tab's last access time for LRU tracking.
 */
void gnalk_room_tab_manager_foreground_tab (GnalkRoomTabManager *self,
                                              const char           *room_token,
                                              gint64               *anchor_id,
                                              gdouble              *anchor_offset,
                                              gboolean             *was_at_bottom);

/**
 * gnalk_room_tab_manager_enforce_limit:
 * @self: The tab manager
 *
 * Evicts oldest tabs (by last_access_time) when over the max_tabs limit.
 * Call this after adding a new tab.
 */
void gnalk_room_tab_manager_enforce_limit (GnalkRoomTabManager *self);

/**
 * gnalk_room_tab_manager_append_messages:
 * @self: The tab manager
 * @room_token: The room token
 * @messages: (element-type GnalkMessageData): Messages to append
 *
 * Appends new messages to a tab's store. Messages are sorted by ID.
 * Creates the tab if it doesn't exist.
 */
void gnalk_room_tab_manager_append_messages (GnalkRoomTabManager *self,
                                               const char           *room_token,
                                               GPtrArray            *messages);

/**
 * gnalk_room_tab_manager_prepend_messages:
 * @self: The tab manager
 * @room_token: The room token
 * @messages: (element-type GnalkMessageData): Messages to prepend
 *
 * Prepends older messages to a tab's store (for history loading).
 * Messages should be in chronological order (oldest first).
 * Creates the tab if it doesn't exist.
 */
void gnalk_room_tab_manager_prepend_messages (GnalkRoomTabManager *self,
                                                const char           *room_token,
                                                GPtrArray            *messages);

/**
 * gnalk_room_tab_manager_remove_tab:
 * @self: The tab manager
 * @room_token: The room token
 *
 * Removes a tab and frees its resources.
 */
void gnalk_room_tab_manager_remove_tab (GnalkRoomTabManager *self,
                                          const char           *room_token);

/**
 * gnalk_room_tab_manager_get_tab_count:
 * @self: The tab manager
 *
 * Returns: The number of active tabs
 */
guint gnalk_room_tab_manager_get_tab_count (GnalkRoomTabManager *self);

/**
 * gnalk_room_tab_manager_set_max_tabs:
 * @self: The tab manager
 * @max_tabs: New maximum number of tabs (0 for unlimited)
 *
 * Updates the maximum tabs limit and enforces it.
 */
void gnalk_room_tab_manager_set_max_tabs (GnalkRoomTabManager *self,
                                            guint                 max_tabs);

/**
 * gnalk_room_tab_manager_set_height_multiplier:
 * @self: The tab manager
 * @multiplier: Viewports worth of messages to keep
 *
 * Updates the height multiplier for message trimming.
 */
void gnalk_room_tab_manager_set_height_multiplier (GnalkRoomTabManager *self,
                                                     gdouble               multiplier);

/**
 * gnalk_room_tab_manager_clear_all:
 * @self: The tab manager
 *
 * Removes all tabs. Call this when switching accounts to avoid
 * displaying messages from the wrong account's perspective.
 */
void gnalk_room_tab_manager_clear_all (GnalkRoomTabManager *self);

G_END_DECLS
