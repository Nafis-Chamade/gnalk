/* gnalk-window-private.h
 *
 * Private header for GnalkWindow implementation files.
 * This header exposes the internal struct and helper functions for
 * splitting the window implementation across multiple source files.
 */

#pragma once

#include "gnalk-window.h"
#include "gnalk-login-view.h"
#include "gnalk-conversation-sidebar.h"
#include "gnalk-conversation-row.h"
#include "gnalk-message-view.h"
#include "gnalk-message-input.h"
#include "gnalk-conversation-header.h"
#include "gnalk-conversation-actions.h"
#include "gnalk-user-status-menu.h"
#include "gnalk-typing-indicator.h"
#include "gnalk-incoming-call-banner.h"
#include "gnalk-participants-list.h"
#include "gnalk-network-status-bar.h"
#include "gnalk-image-gallery.h"
#include "gnalk-media-capture-dialog.h"
#include "gnalk-http-client.h"
#include "gnalk-avatar-cache.h"
#include "gnalk-signaling.h"
#include "gnalk-webrtc.h"
#include "gnalk-push-client.h"
#include "gnalk-sync-manager.h"
#include "gnalk-message-controller.h"
#include "gnalk-conversation-controller.h"
#include "gnalk-participant-controller.h"
#include "gnalk-session-manager.h"
#include "gnalk-session.h"
#include "gnalk-account-switcher.h"
#include "gnalk-call-window.h"
#include "gnalk-message-row.h"
#include "gnalk-room-tab-manager.h"
#include "gnalk-room-state.h"
#include "gnalk-data-repository.h"
#include "gnalk-sync-coordinator.h"
#include "gnalk-room-controller.h"
#include "gnalk-message-processor.h"
#include "gnalk-conversation-info-dialog.h"
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Attachment info for multi-file upload */
typedef struct {
  char   *file_path;
  GBytes *file_bytes;
  char   *content_type;
  char   *file_name;
} AttachmentInfo;

void attachment_info_free (AttachmentInfo *info);

/* Upload queue item for sequential uploads */
typedef struct {
  AttachmentInfo *attachment;  /* Takes ownership */
  char           *caption;
  gint64          reply_to;
  char           *reference_id;
} UploadQueueItem;

void upload_queue_item_free (UploadQueueItem *item);

/* The GnalkWindow struct - shared across window implementation files */
struct _GnalkWindow
{
  AdwApplicationWindow parent_instance;

  /* Main stack for login/app switching */
  GtkWidget *main_stack;

  /* Login view */
  GnalkLoginView *login_view;
  char      *login_room_token;  /* For guest mode */

  GtkWidget *split_view;
  GnalkConversationSidebar *conversation_sidebar;
  GnalkMessageView *message_view;
  GtkWidget *content_page;
  GnalkMessageInput *message_input;
  GnalkConversationHeader *conversation_header;
  GtkWidget *participants_toggle_button;
  GtkWidget *audio_call_button;
  GtkWidget *video_call_button;
  GnalkConversationActions *conversation_actions;
  GnalkUserStatusMenu *user_status_menu;
  GnalkTypingIndicator *typing_indicator;
  GnalkIncomingCallBanner *incoming_call_banner;
  GnalkParticipantsList *participants_list;
  GtkWidget *toast_overlay;
  GnalkNetworkStatusBar *network_status_bar;
  GtkWidget *connecting_spinner;

  /* Image gallery for viewing images in-app */
  GnalkImageGallery *image_gallery;

  /* Media capture dialog (prevent double-open) */
  GnalkMediaCaptureDialog *media_capture_dialog;

  GnalkHttpClient      *http_client;
  GnalkAvatarCache     *avatar_cache;
  GnalkWebrtc          *webrtc;
  GnalkPushClient      *push_client;
  GnalkSyncManager     *sync_manager;
  /* Cached from active session (not owned - Session manages lifecycle) */
  GnalkMessageController      *message_controller;
  GnalkConversationController *conversation_controller;
  GnalkParticipantController  *participant_controller;
  GnalkRoomController         *room_controller;
  GnalkMessageProcessor       *message_processor;

  /* Multi-account session management */
  GnalkSessionManager   *session_manager;  /* Reference from Application, not owned */
  GnalkSession          *active_session;   /* Current session, not owned */
  GnalkAccountSwitcher  *account_switcher; /* Account switching UI */

  GListStore        *conversations_store;
  GListStore        *messages_store;
  GtkNoSelection    *messages_selection;  /* Keep reference to update model */

  /* Tab-based room message management */
  GnalkRoomTabManager *tab_manager;  /* Manages per-room message stores */
  GnalkRoomState *current_room;     /* Points to active tab's state (NULL if no room open) */

  GnalkMessageDisplayMode message_display_mode;  /* Bubbles (narrow) or aligned (wide) */
  char   *user_name;
  guint   typing_send_timer_id;  /* Timer for throttling typing messages */
  gboolean has_active_room;

  /* Connection state */
  gboolean was_offline;              /* Was previously offline (for toast) */
  gboolean had_connection_error;     /* Had a connection error (for banner/toast) */
  gboolean initial_auth_done;        /* First auth attempt completed */
  gboolean initial_conversations_loaded;  /* First conversation list load done */
  gboolean was_active;               /* Previous window active state (to detect app switch vs popover) */

  GnalkCallWindow *active_call_window;  /* Currently active call window, if any */
  gint pending_call_with_video;  /* -1=none, 0=audio, 1=video */

  /* Search results data (UI managed by conversation_sidebar) */
  GListStore *search_results_store;
  GtkNoSelection *search_selection;

  /* Track bound conversation rows for active state updates */
  GHashTable *bound_conversation_rows;  /* token -> GnalkConversationRow* */

  /* Track bound message rows for display mode updates */
  GHashTable *bound_message_rows;  /* GtkWidget* -> GtkWidget* (self-keyed set) */

  /* Scroll-to-message highlight timer (message ID is in current_room state) */
  guint  highlight_clear_timer_id;

  /* File attachment data (UI preview handled by conversation_actions) */
  GPtrArray *attachments;  /* Array of AttachmentInfo* */

  /* Upload queue for sequential file uploads */
  GQueue    *upload_queue;  /* Queue of UploadQueueItem* */
  gboolean   upload_in_progress;

  /* Autologin state */
  char   *autologin_server;
  char   *autologin_user;
  char   *autologin_password;
  gboolean autologin_pending;  /* Waiting to authenticate */

  /* Room switch debouncing + join state now in GnalkRoomController */

  /* Conversation list reload debounce */
  guint    conv_reload_debounce_id;   /* Debounce timer for DataRepository data-changed */
  gboolean conv_load_in_progress;     /* Whether an async DB load is running */
  gboolean conv_load_pending;         /* Reload requested while load was in progress */

  /* Current room's ConversationData tracking (for live user status updates) */
  GnalkConversationData *current_room_conv_data;  /* unowned, for signal tracking */
  gulong conv_data_updated_handler_id;

  /* Unread message tracking (IDs are in current_room state) */
  guint   mark_read_debounce_id;     /* Debounce timer to wait for list to settle */
  guint   mark_read_timer_id;        /* Timer for delayed mark-as-read (glow time) */
  GHashTable *bound_unread_rows;     /* Maps message ID -> GtkWidget* for bound unread rows */
  GArray *visible_unread_ids;        /* Message IDs actually visible (after viewport check) */
};

/* Property specs - shared for property notifications */
extern GParamSpec *gnalk_window_properties[];

/* Property indices */
enum {
  GNALK_WINDOW_PROP_0,
  GNALK_WINDOW_PROP_USER_NAME,
  GNALK_WINDOW_PROP_CURRENT_ROOM_NAME,
  GNALK_WINDOW_PROP_HAS_ACTIVE_ROOM,
  GNALK_WINDOW_N_PROPS
};

/* ============ Login functions (gnalk-window-login.c) ============ */

void gnalk_window_start_login        (GnalkWindow *self);
void gnalk_window_start_guest_login  (GnalkWindow *self);
void gnalk_window_show_login_view    (GnalkWindow *self);
void gnalk_window_attempt_autologin  (GnalkWindow *self);
void gnalk_window_show_offline_main_view (GnalkWindow *self);
void gnalk_window_login_set_connecting   (GnalkWindow *self, gboolean connecting);
void gnalk_window_login_show_error       (GnalkWindow *self, const char *message);
void gnalk_window_on_login_view_connect_clicked (GnalkLoginView *view, GnalkWindow *self);
void gnalk_window_on_login_view_cancel_clicked  (GnalkLoginView *view, GnalkWindow *self);
void gnalk_window_on_add_account_requested      (GnalkAccountSwitcher *switcher, GnalkWindow *self);
void gnalk_window_on_sign_out_requested         (GnalkAccountSwitcher *switcher, gint64 account_id, GnalkWindow *self);

/* ============ Internal helper functions called from login code ============ */

/* These are implemented in gnalk-window.c and called from gnalk-window-login.c */
void gnalk_window_load_conversations_from_db (GnalkWindow *self);
void gnalk_window_switch_to_session          (GnalkWindow *self, GnalkSession *session);

/* Session manager signal handlers (used in attempt_autologin to connect signals) */
void gnalk_window_on_session_manager_active_changed (GnalkSessionManager *manager,
                                                      GnalkSession *session,
                                                      GnalkWindow *self);
void gnalk_window_on_session_manager_any_state_changed (GnalkSessionManager *manager,
                                                         GnalkSession *session,
                                                         int old_state,
                                                         int new_state,
                                                         GnalkWindow *self);
/* ============ Read status functions (gnalk-window-read-status.c) ============ */

void gnalk_window_clear_read_status_tracking           (GnalkWindow *self);
void gnalk_window_update_read_status                   (GnalkWindow *self, gint64 last_common_read_message);
void gnalk_window_mark_messages_read_up_to             (GnalkWindow *self, gint64 message_id);
void gnalk_window_track_bound_unread_row               (GnalkWindow *self, gint64 msg_id, GtkWidget *row);
void gnalk_window_untrack_bound_unread_row             (GnalkWindow *self, gint64 msg_id);
void gnalk_window_on_message_scroll_changed            (GnalkMessageView *view, gpointer user_data);
void gnalk_window_on_scroll_to_bottom_clicked_for_unread (GnalkMessageView *view, GnalkWindow *self);

/* ============ Message composer functions (gnalk-window-message-composer.c) ============ */

char *gnalk_window_generate_reference_id                   (const char *message, const char *room_token);
void gnalk_window_process_upload_queue                     (GnalkWindow *self);
void gnalk_window_queue_file_upload                        (GnalkWindow *self, AttachmentInfo *att,
                                                             const char *caption, gint64 reply_to,
                                                             const char *reference_id);
void gnalk_window_clear_attachments                        (GnalkWindow *self);
void gnalk_window_on_message_input_send_requested          (GnalkMessageInput *input,
                                                             const char *text, GnalkWindow *self);

/* ============ Message display functions (gnalk-window-message-display.c) ============ */

void gnalk_window_clear_messages_store       (GnalkWindow *self);
void gnalk_window_update_messages_is_own     (GnalkWindow *self);
void gnalk_window_load_messages_from_db      (GnalkWindow *self, const char *room_token);
void gnalk_window_load_older_messages        (GnalkWindow *self);
void gnalk_window_save_messages_to_db        (GnalkWindow *self);
void gnalk_window_process_messages_array     (GnalkWindow *self, JsonArray *messages, gboolean is_initial_load);

/* Message controller signal handlers */
void gnalk_window_on_message_controller_messages_loaded (GnalkMessageController *controller,
                                                           JsonArray *messages, gboolean is_initial,
                                                           GnalkWindow *self);
void gnalk_window_on_message_controller_poll_response   (GnalkMessageController *controller,
                                                           JsonArray *messages, GnalkWindow *self);
void gnalk_window_on_message_controller_older_messages_loaded (GnalkMessageController *controller,
                                                                  JsonArray *messages, gboolean has_more,
                                                                  GnalkWindow *self);

/* Processor signal handlers */
void gnalk_window_on_processor_batch_insert_begin     (GnalkMessageProcessor *proc, GnalkWindow *self);
void gnalk_window_on_processor_batch_insert_end       (GnalkMessageProcessor *proc, GnalkWindow *self);
void gnalk_window_on_processor_scroll_to_bottom       (GnalkMessageProcessor *proc, GnalkWindow *self);
void gnalk_window_on_processor_scroll_to_item         (GnalkMessageProcessor *proc, guint position, GnalkWindow *self);
void gnalk_window_on_processor_call_state_left        (GnalkMessageProcessor *proc, const char *actor_id, GnalkWindow *self);
void gnalk_window_on_processor_call_ended             (GnalkMessageProcessor *proc, GnalkWindow *self);
void gnalk_window_on_processor_loading_older_changed  (GnalkMessageProcessor *proc, gboolean loading, GnalkWindow *self);
void gnalk_window_on_processor_state_synced           (GnalkMessageProcessor *proc, guint reason, GnalkWindow *self);

/* ============ Conversation list functions (gnalk-window-conversation-list.c) ============ */

gint gnalk_window_compare_conversations_by_activity (gconstpointer a, gconstpointer b, gpointer user_data);
void gnalk_window_clear_search_results              (GnalkWindow *self);
void gnalk_window_update_conversation_active_states (GnalkWindow *self, const char *old_token, const char *new_token);

/* Search signal handlers */
void gnalk_window_on_sidebar_search_changed         (GnalkConversationSidebar *sidebar,
                                                      const char *text, GnalkWindow *self);
void gnalk_window_on_search_contacts_response       (GnalkConversationController *controller, JsonNode *response,
                                                      GError *error, gpointer user_data);
void gnalk_window_on_sidebar_search_result_selected (GnalkConversationSidebar *sidebar,
                                                      guint position, GnalkWindow *self);

/* List factory callbacks */
void gnalk_window_on_conv_list_item_setup   (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data);
void gnalk_window_on_conv_list_item_bind    (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data);
void gnalk_window_on_conv_list_item_unbind  (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data);
void gnalk_window_on_search_list_item_setup (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data);
void gnalk_window_on_search_list_item_bind  (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data);
void gnalk_window_on_search_list_item_unbind(GtkListItemFactory *factory, GtkListItem *item, gpointer user_data);

/* Controller signal handlers */
void gnalk_window_on_conversation_controller_created     (GnalkConversationController *controller,
                                                          const char *room_token, GnalkWindow *self);
void gnalk_window_on_conversation_controller_left        (GnalkConversationController *controller,
                                                          const char *room_token, GnalkWindow *self);
void gnalk_window_on_conversation_controller_deleted     (GnalkConversationController *controller,
                                                          const char *room_token, GnalkWindow *self);

/* DataRepository signal handlers */
void gnalk_window_on_data_repository_changed (GnalkDataRepository *repository,
                                               const char          *scope,
                                               GnalkWindow         *self);

/* ============ Room switch functions (gnalk-window-room-switch.c) ============ */

void gnalk_window_open_conversation_by_token       (GnalkWindow *self, const char *token);
void gnalk_window_maybe_restore_last_room          (GnalkWindow *self);
void gnalk_window_load_participants                (GnalkWindow *self, const char *token);
void gnalk_window_on_sidebar_conversation_selected (GnalkConversationSidebar *sidebar,
                                                      guint position, GnalkWindow *self);

/* Internal callback needed for signal blocking in room-switch.c */
void gnalk_window_on_participants_toggled          (GtkToggleButton *button, GnalkWindow *self);
void gnalk_window_on_content_page_hiding           (AdwNavigationPage *page, gpointer user_data);

/* ============ Room controller support (gnalk-window-room-switch.c) ============ */

/* Scroll state callback for GnalkRoomController */
void gnalk_window_get_scroll_state (gpointer user_data, gint64 *anchor_id,
                                      gdouble *anchor_offset, gboolean *at_bottom,
                                      int *viewport_height, int *viewport_width);

/* Room controller signal handlers */

void gnalk_window_on_room_controller_room_opened         (GnalkRoomController *ctrl,
                                                            const char *token, const char *old_token,
                                                            GnalkConversationData *conv_data,
                                                            GnalkWindow *self);
void gnalk_window_on_room_controller_room_closed         (GnalkRoomController *ctrl,
                                                            const char *old_token,
                                                            GnalkWindow *self);
void gnalk_window_on_room_controller_messages_store_changed (GnalkRoomController *ctrl,
                                                               GListStore *store,
                                                               GnalkWindow *self);
void gnalk_window_on_room_controller_scroll_requested    (GnalkRoomController *ctrl,
                                                            guint position,
                                                            GnalkWindow *self);

/* ============ UI helper functions (gnalk-window.c, called from room-switch) ============ */

void gnalk_window_start_call                     (GnalkWindow *self, gboolean with_video);
void gnalk_window_update_call_button_state      (GnalkWindow *self);
void gnalk_window_update_room_status_icon       (GnalkWindow *self, gint room_type, const char *user_status);
void gnalk_window_update_room_call_icon         (GnalkWindow *self, gint room_type, gint64 in_call);
void gnalk_window_track_conv_data_for_status    (GnalkWindow *self, GnalkConversationData *conv_data);
void gnalk_window_block_participants_toggle     (GnalkWindow *self);
void gnalk_window_unblock_participants_toggle   (GnalkWindow *self);

/* ============ Internal helper functions called from conversation-list code ============ */

/* These are implemented in gnalk-window.c and called from gnalk-window-conversation-list.c */
void gnalk_window_on_conv_leave_requested      (GnalkConversationRow *row, const char *token, GnalkWindow *self);
void gnalk_window_on_conv_delete_requested     (GnalkConversationRow *row, const char *token, GnalkWindow *self);
void gnalk_window_on_conv_favorite_toggled     (GnalkConversationRow *row, const char *token,
                                                 gboolean is_favorite, GnalkWindow *self);

/* ============ Account session functions (gnalk-window-account-session.c) ============ */

/* Session disconnect helper for dispose */
void gnalk_window_disconnect_active_session (GnalkWindow *self);

/* Sync coordinator handlers removed - Session relays sync signals */

/* ============ Session signal handlers (gnalk-window.c, called from account-session.c) ============ */

/* Session signals (called from gnalk-window-account-session.c when connecting signals) */
void gnalk_window_on_session_state_changed            (GnalkSession *session, int old_state, int new_state, GnalkWindow *self);
void gnalk_window_on_session_room_joined              (GnalkSession *session, const char *room_token, const char *session_id, GnalkWindow *self);
void gnalk_window_on_session_signaling_room_joined    (GnalkSession *session, const char *room_token, GnalkWindow *self);
void gnalk_window_on_session_call_capability_changed  (GnalkSession *session, gboolean can_call, GnalkWindow *self);
void gnalk_window_on_session_auth_failed              (GnalkSession *session, const char *error_message, GnalkWindow *self);
void gnalk_window_on_session_incoming_call            (GnalkSession *session, const char *room_token, const char *caller_id, GnalkWindow *self);
void gnalk_window_on_session_typing                   (GnalkSession *session, const char *room_token, const char *session_id, const char *display_name, gboolean is_typing, GnalkWindow *self);
void gnalk_window_on_session_signaling_ready          (GnalkSession *session, GnalkWindow *self);

/* Relayed signaling signals (now via Session facade) */
void gnalk_window_on_session_call_ended               (GnalkSession *session, GnalkWindow *self);
void gnalk_window_on_session_room_disinvited          (GnalkSession *session, const char *room_token, GnalkWindow *self);

/* Participant controller call state signals */
void gnalk_window_on_participant_call_changed         (GnalkParticipantController *controller, const char *room_token, const char *actor_id, gint64 in_call, GnalkWindow *self);
void gnalk_window_on_all_call_states_cleared          (GnalkParticipantController *controller, const char *room_token, GnalkWindow *self);

/* Relayed sync coordinator signals (now via Session facade) */
void gnalk_window_on_session_room_messages_synced     (GnalkSession *session, const char *room_token, guint count, GnalkWindow *self);

/* Message controller signals */
void gnalk_window_on_message_controller_sent          (GnalkMessageController *controller, const char *reference_id, gint64 message_id, gint64 timestamp, GnalkWindow *self);
void gnalk_window_on_message_controller_send_failed   (GnalkMessageController *controller, const char *reference_id, const char *error, GnalkWindow *self);
void gnalk_window_on_message_controller_edited        (GnalkMessageController *controller, gint64 message_id, GnalkWindow *self);
void gnalk_window_on_message_controller_deleted       (GnalkMessageController *controller, gint64 message_id, GnalkWindow *self);
void gnalk_window_on_message_controller_reaction_added(GnalkMessageController *controller, gint64 message_id, const char *emoji, GnalkWindow *self);
void gnalk_window_on_message_controller_upload_completed(GnalkMessageController *controller, const char *reference_id, GnalkWindow *self);
void gnalk_window_on_message_controller_upload_failed (GnalkMessageController *controller, const char *reference_id, const char *error, GnalkWindow *self);

/* Participant controller signals */
void gnalk_window_on_participant_controller_changed   (GnalkParticipantController *controller, const char *room_token, GnalkWindow *self);

/* Mosaic background update (uses participant avatars) */
void gnalk_window_update_mosaic_background (GnalkWindow *self, JsonArray *participants);

/* ============ Message action functions (gnalk-window-message-actions.c) ============ */

/* Participant controller changed handler */
/* (already declared above as gnalk_window_on_participant_controller_changed) */

/* Participant action handlers */
void gnalk_window_on_participant_chat_requested    (GnalkParticipantsList *list, const char *actor_id, GnalkWindow *self);
void gnalk_window_on_participant_promote_requested (GnalkParticipantsList *list, gint64 attendee_id, const char *actor_id, GnalkWindow *self);
void gnalk_window_on_participant_demote_requested  (GnalkParticipantsList *list, gint64 attendee_id, const char *actor_id, GnalkWindow *self);
void gnalk_window_on_participant_remove_requested  (GnalkParticipantsList *list, gint64 attendee_id, const char *actor_id, GnalkWindow *self);

/* Message row action handlers */
void gnalk_window_on_reaction_requested              (GnalkMessageRow *row, gint64 message_id, const char *emoji, gpointer user_data);
void gnalk_window_on_quote_requested                 (GnalkMessageRow *row, gint64 message_id, const char *author_name, const char *message_text, gpointer user_data);
void gnalk_window_on_edit_requested                  (GnalkMessageRow *row, gint64 message_id, const char *message_text, gpointer user_data);
void gnalk_window_on_delete_requested                (GnalkMessageRow *row, gint64 message_id, gpointer user_data);
void gnalk_window_on_scroll_to_message_requested     (GnalkMessageRow *row, gint64 message_id, gpointer user_data);
void gnalk_window_on_quote_preview_scroll_requested  (GnalkConversationActions *actions, gint64 message_id, GnalkWindow *self);
void gnalk_window_on_quote_closed                    (GnalkConversationActions *actions, gboolean was_editing, GnalkWindow *self);

/* ============ File handling functions (gnalk-window-file-handling.c) ============ */

void gnalk_window_on_attach_clicked             (GtkButton *button, GnalkWindow *self);
void gnalk_window_on_voice_message_recorded     (GnalkMessageInput *input, GBytes *audio_data,
                                                   gdouble duration, GnalkWindow *self);
void gnalk_window_on_camera_clicked             (GnalkMessageInput *input, GnalkWindow *self);
void gnalk_window_on_attachment_closed          (GnalkConversationActions *actions, GnalkWindow *self);
void gnalk_window_on_file_clicked               (GnalkMessageRow *row, const char *file_id,
                                                   const char *file_mimetype, const char *file_link,
                                                   const char *file_name, const char *file_path,
                                                   gpointer user_data);
void gnalk_window_on_info_dialog_media_clicked  (GnalkConversationInfoDialog *dialog,
                                                   const char *file_id, const char *file_mimetype,
                                                   const char *file_path, const char *file_name,
                                                   GnalkWindow *self);

G_END_DECLS
