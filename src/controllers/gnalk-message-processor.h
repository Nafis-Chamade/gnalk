/* gnalk-message-processor.h
 *
 * Message data processing controller.
 * Handles message JSON parsing, GListStore manipulation, DB save/load,
 * date separators, and unread tracking - NO GTK widget dependencies.
 *
 * Signals:
 * - "batch-insert-begin" ()              - UI should disconnect model from view
 * - "batch-insert-end" ()                - UI should reconnect model to view
 * - "scroll-to-bottom" ()                - UI should scroll to bottom (non-initial)
 * - "scroll-to-item" (guint position)    - UI should show/scroll to item position
 * - "call-state-left" (const char *actor_id) - call_left system message processed
 * - "call-ended" ()                      - call_ended system message processed
 * - "loading-older-changed" (gboolean)   - Older messages loading state changed
 * - "state-synced" (guint reason)        - State updated, UI may need action
 */

#pragma once

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Forward declarations - no GTK/widget headers */
typedef struct _GnalkDatabase GnalkDatabase;
typedef struct _GnalkMessageController GnalkMessageController;
typedef struct _GnalkMessageData GnalkMessageData;
typedef struct _GnalkRoomState GnalkRoomState;

#define GNALK_TYPE_MESSAGE_PROCESSOR (gnalk_message_processor_get_type ())
G_DECLARE_FINAL_TYPE (GnalkMessageProcessor, gnalk_message_processor, GNALK, MESSAGE_PROCESSOR, GObject)

/* State sync reasons for the "state-synced" signal */
#define GNALK_PROCESSOR_SYNC_DB_LOADED       0
#define GNALK_PROCESSOR_SYNC_CONTEXT_LOADED  1

GnalkMessageProcessor *gnalk_message_processor_new (void);

/* Dependency setters (none are owned by the processor) */
void gnalk_message_processor_set_database            (GnalkMessageProcessor  *self,
                                                       GnalkDatabase          *db);
void gnalk_message_processor_set_account_id           (GnalkMessageProcessor  *self,
                                                       gint64                  account_id);
void gnalk_message_processor_set_current_user         (GnalkMessageProcessor  *self,
                                                       const char             *user_id);
void gnalk_message_processor_set_room_state           (GnalkMessageProcessor  *self,
                                                       GnalkRoomState         *state);
void gnalk_message_processor_set_message_controller   (GnalkMessageProcessor  *self,
                                                       GnalkMessageController *mc);
void gnalk_message_processor_set_conversations_store  (GnalkMessageProcessor  *self,
                                                       GListStore             *store);

/* Core operations (moved from GnalkWindow) */
void gnalk_message_processor_process_json       (GnalkMessageProcessor *self,
                                                  JsonArray             *messages,
                                                  gboolean               is_initial_load,
                                                  gboolean               view_is_at_bottom);
void gnalk_message_processor_load_from_db       (GnalkMessageProcessor *self,
                                                  const char            *room_token);
void gnalk_message_processor_load_older         (GnalkMessageProcessor *self);
void gnalk_message_processor_save_to_db         (GnalkMessageProcessor *self);
void gnalk_message_processor_update_separators  (GnalkMessageProcessor *self);
void gnalk_message_processor_update_is_own      (GnalkMessageProcessor *self);

/* Process older messages received from server (MC "older-messages-loaded" signal) */
void gnalk_message_processor_process_older_from_server (GnalkMessageProcessor *self,
                                                         JsonArray             *messages,
                                                         gboolean               has_more);

/* Message sorting comparator (used by GListStore insert_sorted / sort) */
gint gnalk_message_processor_compare_by_id (gconstpointer a,
                                             gconstpointer b,
                                             gpointer      user_data);

/* Context mode (jump-to-message) */
void gnalk_message_processor_load_context  (GnalkMessageProcessor *self,
                                            JsonArray             *messages,
                                            gint64                 target_message_id);
void gnalk_message_processor_restore_live  (GnalkMessageProcessor *self);

/* Index a message that was added to the store externally (e.g. pending messages) */
void gnalk_message_processor_index_message (GnalkMessageProcessor *self,
                                             GnalkMessageData      *data);

/* State queries */
gboolean gnalk_message_processor_get_loading_older (GnalkMessageProcessor *self);

G_END_DECLS
