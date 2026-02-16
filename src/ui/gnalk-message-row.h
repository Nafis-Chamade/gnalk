#pragma once

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include "gnalk-avatar-cache.h"
#include "gnalk-message-data.h"

G_BEGIN_DECLS

typedef enum {
  GNALK_MESSAGE_STATUS_NONE,      /* Status not applicable (e.g., other's messages) */
  GNALK_MESSAGE_STATUS_SENDING,   /* Message being sent */
  GNALK_MESSAGE_STATUS_SENT,      /* Message confirmed by server */
  GNALK_MESSAGE_STATUS_READ,      /* Message read by all participants */
  GNALK_MESSAGE_STATUS_FAILED     /* Message failed to send */
} GnalkMessageStatus;

typedef enum {
  GNALK_MESSAGE_DISPLAY_BUBBLES,  /* Bubble chat: own messages right, others left (narrow/mobile) */
  GNALK_MESSAGE_DISPLAY_ALIGNED   /* Aligned: all messages left with avatars (wide/desktop) */
} GnalkMessageDisplayMode;

#define GNALK_TYPE_MESSAGE_ROW (gnalk_message_row_get_type ())
G_DECLARE_FINAL_TYPE (GnalkMessageRow, gnalk_message_row, GNALK, MESSAGE_ROW, GtkBox)

GnalkMessageRow *gnalk_message_row_new (void);

void gnalk_message_row_set_data (GnalkMessageRow  *self,
                                   JsonObject        *message_data,
                                   const char        *current_user_id,
                                   GnalkAvatarCache *avatar_cache);

/* Set row data from GnalkMessageData object (for list factory pattern) */
void gnalk_message_row_set_from_data (GnalkMessageRow  *self,
                                        GnalkMessageData *data,
                                        GnalkAvatarCache *avatar_cache,
                                        const char        *room_token);

/* Reset row to initial state (for unbind in list factory) */
void gnalk_message_row_reset (GnalkMessageRow *self);

gint64 gnalk_message_row_get_id (GnalkMessageRow *self);
gint64 gnalk_message_row_get_timestamp (GnalkMessageRow *self);

void gnalk_message_row_set_reactions (GnalkMessageRow *self,
                                       JsonObject       *reactions);
void gnalk_message_row_add_reaction (GnalkMessageRow *self,
                                      const char       *emoji);

void gnalk_message_row_get_date (GnalkMessageRow *self,
                                  int              *year,
                                  int              *month,
                                  int              *day);
void gnalk_message_row_show_date_header (GnalkMessageRow *self,
                                          gboolean          show);

/* Avatar visibility for message grouping */
void gnalk_message_row_set_show_avatar (GnalkMessageRow *self,
                                          gboolean          show);

/* Display mode: bubbles (narrow) or aligned (wide) */
void gnalk_message_row_set_display_mode (GnalkMessageRow        *self,
                                           GnalkMessageDisplayMode mode);

/* Set whether this is a 1-to-1 conversation (affects username visibility) */
void gnalk_message_row_set_one_to_one (GnalkMessageRow *self,
                                         gboolean          is_one_to_one);

/* Bubble color from avatar */
void gnalk_message_row_set_bubble_color (GnalkMessageRow *self,
                                           const GdkRGBA    *color);

void gnalk_message_row_set_status (GnalkMessageRow   *self,
                                    GnalkMessageStatus status);
GnalkMessageStatus gnalk_message_row_get_status (GnalkMessageRow *self);

void gnalk_message_row_set_reference_id (GnalkMessageRow *self,
                                          const char       *reference_id);
const char *gnalk_message_row_get_reference_id (GnalkMessageRow *self);

/* Create a pending message row for optimistic UI */
GnalkMessageRow *gnalk_message_row_new_pending (const char        *message_text,
                                                   const char        *author_name,
                                                   const char        *author_id,
                                                   const char        *reference_id,
                                                   gint64             reply_to,
                                                   GnalkAvatarCache *avatar_cache);

/* Update pending message with server response */
void gnalk_message_row_confirm_sent (GnalkMessageRow *self,
                                       gint64            message_id,
                                       gint64            timestamp);

/* File download status */
void gnalk_message_row_set_downloading (GnalkMessageRow *self,
                                          gboolean          downloading);
void gnalk_message_row_set_file_cached (GnalkMessageRow *self,
                                          gboolean          cached);
const char *gnalk_message_row_get_file_id (GnalkMessageRow *self);
const char *gnalk_message_row_get_file_name (GnalkMessageRow *self);
const char *gnalk_message_row_get_file_mimetype (GnalkMessageRow *self);

/* Audio file handling */
gboolean gnalk_message_row_has_audio_file (GnalkMessageRow *self);
void gnalk_message_row_set_audio_file (GnalkMessageRow *self,
                                         const char       *file_path);
void gnalk_message_row_set_audio_loading (GnalkMessageRow *self,
                                            gboolean          loading);

/* Video file handling */
gboolean gnalk_message_row_has_video_file (GnalkMessageRow *self);
void gnalk_message_row_set_video_file (GnalkMessageRow *self,
                                         const char       *file_path);

/* File upload progress */
void gnalk_message_row_set_upload_progress (GnalkMessageRow *self,
                                              double            progress);
void gnalk_message_row_set_upload_complete (GnalkMessageRow *self);

/* Edit/delete timeout (for disabling edit/delete on old messages) */
void gnalk_message_row_set_edit_timeout (GnalkMessageRow *self,
                                           gint64            timeout);
void gnalk_message_row_set_delete_timeout (GnalkMessageRow *self,
                                             gint64            timeout);

/* Unread message visual state */
void gnalk_message_row_set_unread (GnalkMessageRow *self,
                                     gboolean          is_unread);

/* Timestamp visibility for consecutive messages with same time */
void gnalk_message_row_set_show_timestamp (GnalkMessageRow *self,
                                             gboolean          show);

/* Thread support: set whether threads capability is available */
void gnalk_message_row_set_threads_supported (GnalkMessageRow *self,
                                               gboolean          supported);

G_END_DECLS
