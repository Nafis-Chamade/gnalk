#pragma once

#include <glib-object.h>
#include "gnalk-http-client.h"
#include "gnalk-database.h"
#include "gnalk-network-monitor.h"

G_BEGIN_DECLS

#define GNALK_TYPE_SYNC_MANAGER (gnalk_sync_manager_get_type ())
G_DECLARE_FINAL_TYPE (GnalkSyncManager, gnalk_sync_manager, GNALK, SYNC_MANAGER, GObject)

GnalkSyncManager *gnalk_sync_manager_new (GnalkHttpClient     *http_client,
                                             GnalkDatabase       *database,
                                             GnalkNetworkMonitor *network_monitor,
                                             gint64                account_id);

void gnalk_sync_manager_set_account_id (GnalkSyncManager *self,
                                          gint64             account_id);

/* Flush pending message queue - sends all queued messages */
void gnalk_sync_manager_flush_pending_queue (GnalkSyncManager *self);

/* Check if currently online */
gboolean gnalk_sync_manager_is_online (GnalkSyncManager *self);

/* Queue a message for sending (used when offline) */
void gnalk_sync_manager_queue_message (GnalkSyncManager *self,
                                         const char        *conversation_token,
                                         const char        *message,
                                         gint64             reply_to,
                                         const char        *reference_id);

/* Queue a file for sending (used when offline) */
void gnalk_sync_manager_queue_file (GnalkSyncManager *self,
                                      const char        *conversation_token,
                                      const char        *file_path,
                                      const char        *file_name,
                                      const char        *content_type,
                                      const char        *caption,
                                      gint64             reply_to,
                                      const char        *reference_id);

/* Signals:
 * - "network-changed" (gboolean is_online) - Forwarded from network monitor
 * - "message-sent" (const char *reference_id, gint64 message_id) - Pending message was sent
 * - "message-failed" (const char *reference_id, const char *error) - Pending message failed
 */

G_END_DECLS
