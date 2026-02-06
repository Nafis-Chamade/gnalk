#pragma once

#include <glib-object.h>
#include "gnalk-database.h"
#include "gnalk-http-client.h"

G_BEGIN_DECLS

#define GNALK_TYPE_PARTICIPANT_CONTROLLER (gnalk_participant_controller_get_type ())
G_DECLARE_FINAL_TYPE (GnalkParticipantController, gnalk_participant_controller, GNALK, PARTICIPANT_CONTROLLER, GObject)

/**
 * GnalkParticipantController:
 *
 * Handles participant list operations for the active room:
 * - Loading participants for a room
 * - Periodic polling (60s) for participant updates in group rooms
 * - Saving participants to database for offline access
 *
 * The controller saves data to the database before emitting signals.
 * Window loads cached data directly from DB for instant display.
 * Controller signals notify only when server returns different data.
 *
 * Signals:
 * - "participants-changed" (const char *room_token): Server returned different data than cache
 * - "error" (const char *message): An error occurred
 */

/**
 * gnalk_participant_controller_new:
 * @database: The database for caching
 * @http_client: The HTTP client for server communication
 *
 * Creates a new participant controller.
 *
 * Returns: (transfer full): A new participant controller
 */
GnalkParticipantController *gnalk_participant_controller_new (GnalkDatabase   *database,
                                                                  GnalkHttpClient *http_client);

/**
 * gnalk_participant_controller_set_account_id:
 * @self: The controller
 * @account_id: The account ID for database operations
 *
 * Sets the account ID for database operations.
 */
void gnalk_participant_controller_set_account_id (GnalkParticipantController *self,
                                                     gint64                        account_id);

/**
 * gnalk_participant_controller_set_active_room:
 * @self: The controller
 * @room_token: The room token to load participants for (NULL to clear)
 * @room_type: The room type (1=one-to-one, 2=group, etc.)
 *
 * Sets the active room for participant management.
 * This will:
 * 1. Stop any previous polling
 * 2. Fetch participants from server for the new room
 * 3. Compare with cache and emit "participants-changed" if different
 * 4. Start 60s polling for group rooms (not 1-on-1)
 *
 * Pass NULL room_token to clear the active room and stop polling.
 */
void gnalk_participant_controller_set_active_room (GnalkParticipantController *self,
                                                      const char                   *room_token,
                                                      int                           room_type);

/**
 * gnalk_participant_controller_refresh:
 * @self: The controller
 *
 * Manually triggers a participant refresh from the server.
 */
void gnalk_participant_controller_refresh (GnalkParticipantController *self);

/**
 * gnalk_participant_controller_get_active_room:
 * @self: The controller
 *
 * Returns: (transfer none) (nullable): The current active room token, or NULL
 */
const char *gnalk_participant_controller_get_active_room (GnalkParticipantController *self);

/**
 * gnalk_participant_controller_is_polling:
 * @self: The controller
 *
 * Returns: TRUE if participant polling is currently active
 */
gboolean gnalk_participant_controller_is_polling (GnalkParticipantController *self);

/**
 * gnalk_participant_controller_promote_moderator:
 * @self: The controller
 * @room_token: The room token
 * @attendee_id: The attendee ID to promote
 */
void gnalk_participant_controller_promote_moderator (GnalkParticipantController *self,
                                                     const char                 *room_token,
                                                     gint64                      attendee_id);

/**
 * gnalk_participant_controller_demote_moderator:
 * @self: The controller
 * @room_token: The room token
 * @attendee_id: The attendee ID to demote
 */
void gnalk_participant_controller_demote_moderator (GnalkParticipantController *self,
                                                    const char                 *room_token,
                                                    gint64                      attendee_id);

/**
 * gnalk_participant_controller_remove_attendee:
 * @self: The controller
 * @room_token: The room token
 * @attendee_id: The attendee ID to remove
 */
void gnalk_participant_controller_remove_attendee (GnalkParticipantController *self,
                                                   const char                 *room_token,
                                                   gint64                      attendee_id);

G_END_DECLS
