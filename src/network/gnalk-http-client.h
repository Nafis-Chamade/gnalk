#pragma once

#include <glib-object.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define GNALK_TYPE_HTTP_CLIENT (gnalk_http_client_get_type ())
G_DECLARE_FINAL_TYPE (GnalkHttpClient, gnalk_http_client, GNALK, HTTP_CLIENT, GObject)

/**
 * GnalkHttpErrorType:
 * @GNALK_HTTP_ERROR_NONE: No error
 * @GNALK_HTTP_ERROR_TIMEOUT: Request timed out (G_IO_ERROR_TIMED_OUT)
 * @GNALK_HTTP_ERROR_SERVER: Server error (HTTP 5xx)
 * @GNALK_HTTP_ERROR_AUTH: Authentication error (HTTP 401/403)
 * @GNALK_HTTP_ERROR_NETWORK: Network error (connection refused, etc.)
 * @GNALK_HTTP_ERROR_OTHER: Other error
 *
 * Categorizes HTTP errors for offline mode detection.
 * TIMEOUT and SERVER errors should trigger offline mode.
 */
typedef enum {
  GNALK_HTTP_ERROR_NONE,
  GNALK_HTTP_ERROR_TIMEOUT,
  GNALK_HTTP_ERROR_SERVER,
  GNALK_HTTP_ERROR_AUTH,
  GNALK_HTTP_ERROR_NETWORK,
  GNALK_HTTP_ERROR_OTHER
} GnalkHttpErrorType;

/**
 * gnalk_http_error_type_from_error:
 * @error: (nullable): The GError to categorize
 * @http_status: The HTTP status code (0 if not available)
 *
 * Categorizes an error for offline mode detection.
 *
 * Returns: The error type category
 */
GnalkHttpErrorType gnalk_http_error_type_from_error (GError *error,
                                                        guint   http_status);

typedef void (*GnalkHttpCallback) (GnalkHttpClient *client,
                                     JsonNode         *response,
                                     GError           *error,
                                     gpointer          user_data);

typedef void (*GnalkHttpBytesCallback) (GnalkHttpClient *client,
                                          GBytes           *bytes,
                                          const char       *content_type,
                                          GError           *error,
                                          gpointer          user_data);

GnalkHttpClient *gnalk_http_client_new (void);

void gnalk_http_client_set_credentials (GnalkHttpClient *self,
                                          const char       *server_url,
                                          const char       *user_id,
                                          const char       *password);

void gnalk_http_client_set_guest_mode (GnalkHttpClient *self,
                                         const char       *server_url,
                                         const char       *room_token);

gboolean gnalk_http_client_is_guest (GnalkHttpClient *self);

gboolean gnalk_http_client_is_ready (GnalkHttpClient *self);

const char *gnalk_http_client_get_room_token (GnalkHttpClient *self);

void gnalk_http_client_get_server_status (GnalkHttpClient   *self,
                                            GnalkHttpCallback  callback,
                                            gpointer            user_data);

void gnalk_http_client_get_server_status_with_timeout (GnalkHttpClient   *self,
                                                         guint               timeout_seconds,
                                                         GnalkHttpCallback  callback,
                                                         gpointer            user_data);

void gnalk_http_client_get_user_info (GnalkHttpClient   *self,
                                        GnalkHttpCallback  callback,
                                        gpointer            user_data);

void gnalk_http_client_get_conversations (GnalkHttpClient   *self,
                                            GnalkHttpCallback  callback,
                                            gpointer            user_data);

void gnalk_http_client_get_capabilities (GnalkHttpClient   *self,
                                           GnalkHttpCallback  callback,
                                           gpointer            user_data);

void gnalk_http_client_get_signaling_settings (GnalkHttpClient   *self,
                                                 GnalkHttpCallback  callback,
                                                 gpointer            user_data);

void gnalk_http_client_get_messages (GnalkHttpClient   *self,
                                       const char         *room_token,
                                       gint64              last_known_message_id,
                                       gboolean            look_into_future,
                                       gint                timeout,
                                       GnalkHttpCallback  callback,
                                       gpointer            user_data);

void gnalk_http_client_send_message (GnalkHttpClient   *self,
                                       const char         *room_token,
                                       const char         *message,
                                       gint64              reply_to,
                                       const char         *reference_id,
                                       GnalkHttpCallback  callback,
                                       gpointer            user_data);

void gnalk_http_client_edit_message (GnalkHttpClient   *self,
                                       const char         *room_token,
                                       gint64              message_id,
                                       const char         *message,
                                       GnalkHttpCallback  callback,
                                       gpointer            user_data);

void gnalk_http_client_delete_message (GnalkHttpClient   *self,
                                         const char         *room_token,
                                         gint64              message_id,
                                         GnalkHttpCallback  callback,
                                         gpointer            user_data);

void gnalk_http_client_mark_chat_read (GnalkHttpClient   *self,
                                         const char         *room_token,
                                         gint64              last_read_message_id,
                                         GnalkHttpCallback  callback,
                                         gpointer            user_data);

void gnalk_http_client_join_room (GnalkHttpClient   *self,
                                    const char         *room_token,
                                    GnalkHttpCallback  callback,
                                    gpointer            user_data);

void gnalk_http_client_leave_room (GnalkHttpClient   *self,
                                     const char         *room_token,
                                     GnalkHttpCallback  callback,
                                     gpointer            user_data);

void gnalk_http_client_get_participants (GnalkHttpClient   *self,
                                           const char         *room_token,
                                           GnalkHttpCallback  callback,
                                           gpointer            user_data);

void gnalk_http_client_start_call (GnalkHttpClient   *self,
                                     const char         *room_token,
                                     gint                flags,
                                     GnalkHttpCallback  callback,
                                     gpointer            user_data);

void gnalk_http_client_leave_call (GnalkHttpClient   *self,
                                     const char         *room_token,
                                     GnalkHttpCallback  callback,
                                     gpointer            user_data);

void gnalk_http_client_get_room_avatar (GnalkHttpClient        *self,
                                          const char              *room_token,
                                          GnalkHttpBytesCallback  callback,
                                          gpointer                 user_data);

void gnalk_http_client_get_user_avatar (GnalkHttpClient        *self,
                                          const char              *user_id,
                                          int                      size,
                                          GnalkHttpBytesCallback  callback,
                                          gpointer                 user_data);

void gnalk_http_client_get_file_preview (GnalkHttpClient        *self,
                                           const char              *file_id,
                                           int                      width,
                                           int                      height,
                                           GnalkHttpBytesCallback  callback,
                                           gpointer                 user_data);

void gnalk_http_client_download_file (GnalkHttpClient        *self,
                                        const char              *file_path,
                                        GnalkHttpBytesCallback  callback,
                                        gpointer                 user_data);

void gnalk_http_client_cancel_pending (GnalkHttpClient *self);

/**
 * gnalk_http_client_cancel_room_requests:
 * @self: The HTTP client
 *
 * Cancels room-specific pending requests (messages, participants, etc.)
 * without affecting auth or background requests. Use this when switching rooms.
 */
void gnalk_http_client_cancel_room_requests (GnalkHttpClient *self);

void gnalk_http_client_add_reaction (GnalkHttpClient   *self,
                                       const char         *room_token,
                                       gint64              message_id,
                                       const char         *reaction,
                                       GnalkHttpCallback  callback,
                                       gpointer            user_data);

void gnalk_http_client_set_user_status (GnalkHttpClient   *self,
                                          const char         *status_type,
                                          GnalkHttpCallback  callback,
                                          gpointer            user_data);

void gnalk_http_client_send_heartbeat (GnalkHttpClient   *self,
                                         GnalkHttpCallback  callback,
                                         gpointer            user_data);

void gnalk_http_client_search_contacts (GnalkHttpClient   *self,
                                          const char         *search_term,
                                          GnalkHttpCallback  callback,
                                          gpointer            user_data);

void gnalk_http_client_create_conversation (GnalkHttpClient   *self,
                                              int                 room_type,
                                              const char         *invite,
                                              const char         *room_name,
                                              GnalkHttpCallback  callback,
                                              gpointer            user_data);

void gnalk_http_client_leave_conversation (GnalkHttpClient   *self,
                                             const char         *room_token,
                                             GnalkHttpCallback  callback,
                                             gpointer            user_data);

void gnalk_http_client_delete_conversation (GnalkHttpClient   *self,
                                              const char         *room_token,
                                              GnalkHttpCallback  callback,
                                              gpointer            user_data);

void gnalk_http_client_set_conversation_favorite (GnalkHttpClient   *self,
                                                    const char         *room_token,
                                                    gboolean            favorite,
                                                    GnalkHttpCallback  callback,
                                                    gpointer            user_data);

void gnalk_http_client_search_users (GnalkHttpClient   *self,
                                       const char         *search_term,
                                       GnalkHttpCallback  callback,
                                       gpointer            user_data);

void gnalk_http_client_set_room_description (GnalkHttpClient   *self,
                                               const char         *room_token,
                                               const char         *description,
                                               GnalkHttpCallback  callback,
                                               gpointer            user_data);

void gnalk_http_client_add_participant (GnalkHttpClient   *self,
                                          const char         *room_token,
                                          const char         *user_id,
                                          GnalkHttpCallback  callback,
                                          gpointer            user_data);

/* Guest mode API */
void gnalk_http_client_get_room_info (GnalkHttpClient   *self,
                                        const char         *room_token,
                                        GnalkHttpCallback  callback,
                                        gpointer            user_data);

void gnalk_http_client_join_room_as_guest (GnalkHttpClient   *self,
                                             const char         *room_token,
                                             const char         *password,
                                             GnalkHttpCallback  callback,
                                             gpointer            user_data);

void gnalk_http_client_set_guest_name (GnalkHttpClient   *self,
                                         const char         *room_token,
                                         const char         *display_name,
                                         GnalkHttpCallback  callback,
                                         gpointer            user_data);

/* File upload API */
typedef void (*GnalkHttpBoolCallback) (GnalkHttpClient *client,
                                         gboolean          result,
                                         GError           *error,
                                         gpointer          user_data);

void gnalk_http_client_check_file_exists (GnalkHttpClient      *self,
                                            const char            *remote_path,
                                            GnalkHttpBoolCallback callback,
                                            gpointer               user_data);

void gnalk_http_client_upload_file (GnalkHttpClient      *self,
                                      const char            *remote_path,
                                      GBytes                *file_bytes,
                                      const char            *content_type,
                                      GnalkHttpBoolCallback callback,
                                      gpointer               user_data);

/**
 * gnalk_http_client_share_to_conversation:
 * @message_type: (nullable): Message type for special messages (e.g., "voice-message"), or NULL for regular file share
 */
void gnalk_http_client_share_to_conversation (GnalkHttpClient   *self,
                                                const char         *remote_path,
                                                const char         *room_token,
                                                const char         *caption,
                                                gint64              reply_to,
                                                const char         *reference_id,
                                                const char         *message_type,
                                                GnalkHttpCallback  callback,
                                                gpointer            user_data);

const char *gnalk_http_client_get_user_id (GnalkHttpClient *self);

/* Room settings API */
void gnalk_http_client_set_room_name (GnalkHttpClient   *self,
                                        const char         *room_token,
                                        const char         *name,
                                        GnalkHttpCallback  callback,
                                        gpointer            user_data);

void gnalk_http_client_upload_room_avatar (GnalkHttpClient      *self,
                                             const char            *room_token,
                                             GBytes                *avatar_bytes,
                                             const char            *content_type,
                                             GnalkHttpBoolCallback callback,
                                             gpointer               user_data);

void gnalk_http_client_set_notification_level (GnalkHttpClient   *self,
                                                 const char         *room_token,
                                                 gint                level,
                                                 GnalkHttpCallback  callback,
                                                 gpointer            user_data);

void gnalk_http_client_set_call_notifications (GnalkHttpClient   *self,
                                                 const char         *room_token,
                                                 gint                level,
                                                 GnalkHttpCallback  callback,
                                                 gpointer            user_data);

void gnalk_http_client_get_shared_items (GnalkHttpClient   *self,
                                           const char         *room_token,
                                           const char         *object_type,
                                           gint                limit,
                                           GnalkHttpCallback  callback,
                                           gpointer            user_data);

/* Participant moderation API */
void gnalk_http_client_remove_attendee (GnalkHttpClient   *self,
                                          const char         *room_token,
                                          gint64              attendee_id,
                                          GnalkHttpCallback  callback,
                                          gpointer            user_data);

void gnalk_http_client_promote_moderator (GnalkHttpClient   *self,
                                            const char         *room_token,
                                            gint64              attendee_id,
                                            GnalkHttpCallback  callback,
                                            gpointer            user_data);

void gnalk_http_client_demote_moderator (GnalkHttpClient   *self,
                                           const char         *room_token,
                                           gint64              attendee_id,
                                           GnalkHttpCallback  callback,
                                           gpointer            user_data);

G_END_DECLS
