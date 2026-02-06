/* gnalk-room-controller.h
 *
 * Room open/close orchestration controller.
 * Manages tab switching, debouncing, and room lifecycle.
 *
 * Signals:
 * - "room-opened" (token, old_token, conv_data): Room switched to a new room
 * - "room-closed" (old_token): Room closed (content page hiding)
 * - "messages-store-changed" (store): Active message store changed
 * - "scroll-requested" (position): Controller wants UI to scroll to position
 * - "join-complete" (token, session_id): Session join confirmed
 */

#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include "gnalk-room-state.h"

G_BEGIN_DECLS

/* Forward declarations */
typedef struct _GnalkRoomTabManager GnalkRoomTabManager;
typedef struct _GnalkSession GnalkSession;
typedef struct _GnalkMessageController GnalkMessageController;
typedef struct _GnalkParticipantController GnalkParticipantController;

#define GNALK_TYPE_ROOM_CONTROLLER (gnalk_room_controller_get_type ())
G_DECLARE_FINAL_TYPE (GnalkRoomController, gnalk_room_controller, GNALK, ROOM_CONTROLLER, GObject)

/**
 * GnalkScrollStateFunc:
 * @user_data: Callback data
 * @anchor_id: (out): Message ID of the first visible message
 * @anchor_offset: (out): Pixel offset of the scroll position
 * @at_bottom: (out): Whether the view is scrolled to the bottom
 * @viewport_height: (out): Viewport height in pixels
 * @viewport_width: (out): Viewport width in pixels
 *
 * Callback to retrieve current scroll state from the UI layer.
 * Called by the controller before backgrounding a room tab.
 */
typedef void (*GnalkScrollStateFunc) (gpointer  user_data,
                                       gint64   *anchor_id,
                                       gdouble  *anchor_offset,
                                       gboolean *at_bottom,
                                       int      *viewport_height,
                                       int      *viewport_width);

/**
 * gnalk_room_controller_new:
 * @tab_manager: (transfer none): Room tab manager (not owned)
 * @conversations_store: (transfer none): Conversation list store (not owned)
 * @settings: (transfer none): Application settings (not owned)
 *
 * Creates a new room controller.
 *
 * Returns: (transfer full): A new #GnalkRoomController
 */
GnalkRoomController *gnalk_room_controller_new (GnalkRoomTabManager *tab_manager,
                                                  GListStore          *conversations_store,
                                                  GSettings           *settings);

/* Dependency setters (dependencies may change over session lifetime) */
void gnalk_room_controller_set_session                (GnalkRoomController        *self,
                                                         GnalkSession               *session);
void gnalk_room_controller_set_message_controller     (GnalkRoomController        *self,
                                                         GnalkMessageController     *mc);
void gnalk_room_controller_set_participant_controller (GnalkRoomController        *self,
                                                         GnalkParticipantController *pc);
void gnalk_room_controller_set_conversations_store    (GnalkRoomController        *self,
                                                         GListStore                 *store);
void gnalk_room_controller_set_scroll_state_func      (GnalkRoomController        *self,
                                                         GnalkScrollStateFunc        func,
                                                         gpointer                    user_data);

/* Core operations */
void gnalk_room_controller_open_room           (GnalkRoomController *self,
                                                  const char          *token);
void gnalk_room_controller_open_room_debounced (GnalkRoomController *self,
                                                  const char          *token);
void gnalk_room_controller_background_current  (GnalkRoomController *self,
                                                  gint64               anchor_id,
                                                  gdouble              anchor_offset,
                                                  gboolean             at_bottom,
                                                  int                  viewport_height,
                                                  int                  viewport_width);
void gnalk_room_controller_close_current       (GnalkRoomController *self);
void gnalk_room_controller_restore_last_room   (GnalkRoomController *self,
                                                  gboolean             is_collapsed);

/* State queries */
GnalkRoomState *gnalk_room_controller_get_current_room       (GnalkRoomController *self);
const char     *gnalk_room_controller_get_current_room_token (GnalkRoomController *self);
gboolean        gnalk_room_controller_has_active_room        (GnalkRoomController *self);
GListStore     *gnalk_room_controller_get_messages_store     (GnalkRoomController *self);

G_END_DECLS
