/* gnalk-call-window-private.h
 *
 * Private header for GnalkCallWindow implementation files.
 * This header exposes the internal struct and helper functions for
 * splitting the call window implementation across multiple source files.
 */

#pragma once

#include "gnalk-call-window.h"
#include "gnalk-application.h"
#include "gnalk-participant-tile.h"
#include "gnalk-avatar-cache.h"
#include "gnalk-screen-capture.h"
#include "gnalk-camera-utils.h"
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/app/gstappsink.h>
#endif

G_BEGIN_DECLS

typedef enum {
  GNALK_LAYOUT_GRID,
  GNALK_LAYOUT_SPEAKER_FOCUS
} GnalkLayoutMode;

/* Threshold for mobile detection (in pixels) */
#define MOBILE_WIDTH_THRESHOLD 600

/* The GnalkCallWindow struct - shared across call window implementation files */
struct _GnalkCallWindow
{
  AdwWindow parent_instance;

  GtkWidget *header_bar;
  GtkWidget *video_overlay;
  GtkWidget *video_stack;
  GtkWidget *local_video_fullscreen;  /* Fullscreen preview before call */
  GtkWidget *local_video_box;         /* Container box for small overlay */
  GtkWidget *local_video_frame;       /* Small overlay picture during call */
  GtkWidget *switch_camera_button;
  GtkWidget *mute_audio_button;
  GtkWidget *mute_video_button;
  GtkWidget *screen_share_button;
  GtkWidget *hangup_button;
  GtkWidget *layout_toggle_button;

  /* Multi-participant layout widgets */
  GtkWidget *layout_stack;            /* Stack for grid/speaker modes */
  GtkWidget *video_grid;              /* GtkGrid for grid layout */
  GtkWidget *speaker_view;            /* Container for speaker focus mode */
  GtkWidget *speaker_main;            /* Large area for active speaker */
  GtkWidget *speaker_strip;           /* Strip for other participants */

  /* Connecting state widgets */
  GtkWidget *connecting_spinner;

  /* Multi-participant support */
  GHashTable *participant_tiles;      /* session_id -> GnalkParticipantTile* */
  GnalkParticipantTile *local_tile;  /* Our own tile */
  GnalkLayoutMode layout_mode;
  char *active_speaker_id;

  /* Drag state for preview overlay */
  double drag_start_x;
  double drag_start_y;
  int preview_margin_start;
  int preview_margin_top;

  /* Pending resize recalculation */
  guint resize_idle_id;

  /* Layout recalculation state */
  gboolean layout_in_progress;
  gboolean layout_needs_recalc;
  guint layout_idle_id;

  /* Camera selection for preview */
  GnalkCameraPosition camera_position;

  /* Mobile/desktop mode */
  gboolean is_mobile;

  char *room_token;
  char *room_name;
  char *call_status;
  char *session_id;
  char *signaling_session_id;
  GPtrArray *peer_session_ids;
  GHashTable *peer_display_names;   /* session_id -> display_name */
  GHashTable *peer_user_ids;        /* session_id -> user_id (for avatar loading) */
  GHashTable *user_display_names;   /* userId -> display_name (from HTTP API) */

  gboolean room_joined;
  gboolean offer_sent;
  gboolean has_mcu;

  GnalkHttpClient *http_client;
  GnalkAvatarCache *avatar_cache;
  GnalkWebrtc     *webrtc;           /* For publishing our stream */
  char             *publisher_sid;     /* Client-generated SID for publishing */
  gboolean          publisher_connected;
  gboolean          video_enabled;        /* Whether video is enabled for this call */
  GnalkSignaling  *signaling;
  guint             unmute_timer_id;  /* Periodic timer to send unmute messages */
  guint             start_webrtc_timeout_id; /* Deferred WebRTC start to allow camera release */

#ifdef HAVE_GSTREAMER
  GstElement       *preview_pipeline;  /* Local camera preview pipeline */
  GstElement       *preview_appsink;
#endif

  /* Screen sharing - outgoing */
  GnalkScreenCapture *screen_capture;
  GnalkWebrtc        *screen_publisher;
  char                *screen_publisher_sid;
  gboolean             is_sharing_screen;

  /* Call state */
  gboolean             call_ending;  /* Guard against multiple end_call calls */
  gboolean             is_federated; /* TRUE for federated calls (use /call/{token}/federation) */

  /* Screen sharing - incoming */
  GHashTable          *screen_tiles;      /* session_id -> GnalkParticipantTile* */
  GHashTable          *screen_subscribers; /* session_id -> GnalkWebrtc* */
  GHashTable          *screen_sids;       /* session_id -> subscriber_sid */
  gulong               screen_share_handler_id; /* Handler for screen-share-changed signal */
};

/* Property indices */
enum {
  GNALK_CALL_WINDOW_PROP_0,
  GNALK_CALL_WINDOW_PROP_ROOM_NAME,
  GNALK_CALL_WINDOW_PROP_CALL_STATUS,
  GNALK_CALL_WINDOW_N_PROPS
};

/* Signals */
enum {
  GNALK_CALL_WINDOW_SIGNAL_CALL_ENDED,
  GNALK_CALL_WINDOW_N_SIGNALS
};

extern GParamSpec *gnalk_call_window_properties[];
extern guint gnalk_call_window_signals[];

/* ============ Preview manager functions (gnalk-call-preview-manager.c) ============ */

#ifdef HAVE_GSTREAMER
GstElement *gnalk_call_preview_find_camera_source_by_position (GnalkCameraPosition position,
                                                                  gint *out_rotation);
void        gnalk_call_preview_start_local_preview             (GnalkCallWindow *self);
void        gnalk_call_preview_stop_local_preview              (GnalkCallWindow *self);
#endif

gboolean    gnalk_call_preview_on_start_webrtc_after_preview_stop (gpointer user_data);

/* ============ Layout functions (gnalk-call-layout.c) ============ */

void gnalk_call_layout_schedule_recalculation  (GnalkCallWindow *self);
void gnalk_call_layout_recalculate             (GnalkCallWindow *self);
void gnalk_call_layout_remove_tile_from_parent (GnalkCallWindow *self, GtkWidget *tile);
void gnalk_call_layout_on_toggle_clicked       (GtkButton *button, GnalkCallWindow *self);

/* ============ Participant manager functions (gnalk-call-participant-manager.c) ============ */

/* Video callback data for participant tiles */
typedef struct {
  GWeakRef window_ref;
  char *session_id;
  int frame_count;  /* Per-callback frame counter for debugging */
} TileVideoCallbackData;

void gnalk_call_participant_add_tile    (GnalkCallWindow *self,
                                           const char *session_id,
                                           const char *display_name,
                                           const char *user_id);
void gnalk_call_participant_remove_tile (GnalkCallWindow *self,
                                           const char *session_id);

/* Video frame callbacks - need to be accessible for WebRTC setup */
void gnalk_call_participant_on_tile_remote_video_frame  (GnalkWebrtc *webrtc,
                                                           GdkTexture *texture,
                                                           gpointer user_data);
void gnalk_call_participant_on_screen_remote_video_frame (GnalkWebrtc *webrtc,
                                                            GdkTexture *texture,
                                                            gpointer user_data);
void gnalk_call_participant_on_tile_speaking            (GnalkWebrtc *webrtc,
                                                           gboolean speaking,
                                                           gpointer user_data);
void gnalk_call_participant_on_local_video_frame        (GnalkWebrtc *webrtc,
                                                           GdkTexture *texture,
                                                           gpointer user_data);

/* ============ Signaling manager functions (gnalk-call-signaling-manager.c) ============ */

char *gnalk_call_signaling_generate_random_sid (void);

/* P2P signaling */
void gnalk_call_signaling_send_to_peer              (GnalkCallWindow *self,
                                                       const char *peer_session_id,
                                                       const char *type,
                                                       JsonNode *data);

/* MCU signaling */
void gnalk_call_signaling_send_to_mcu               (GnalkCallWindow *self,
                                                       const char *type,
                                                       JsonNode *data);
void gnalk_call_signaling_send_to_mcu_screen        (GnalkCallWindow *self,
                                                       const char *type,
                                                       JsonNode *data);

/* Subscription signaling */
void gnalk_call_signaling_request_offer_from_peer   (GnalkCallWindow *self,
                                                       const char *peer_session_id);
void gnalk_call_signaling_request_screen_offer_from_peer (GnalkCallWindow *self,
                                                            const char *peer_session_id);
void gnalk_call_signaling_send_to_peer_for_subscription (GnalkCallWindow *self,
                                                           const char *peer_session_id,
                                                           const char *subscriber_sid,
                                                           const char *type,
                                                           JsonNode *data);
void gnalk_call_signaling_send_to_peer_for_screen_subscription (GnalkCallWindow *self,
                                                                   const char *peer_session_id,
                                                                   const char *subscriber_sid,
                                                                   const char *type,
                                                                   JsonNode *data);

/* WebRTC signal handlers */
void gnalk_call_signaling_on_webrtc_signal          (GnalkWebrtc *webrtc,
                                                       const char *type,
                                                       JsonNode *data,
                                                       gpointer user_data);
void gnalk_call_signaling_on_tile_subscriber_signal (GnalkWebrtc *webrtc,
                                                       const char *type,
                                                       JsonNode *data,
                                                       gpointer user_data);
void gnalk_call_signaling_on_screen_subscriber_signal (GnalkWebrtc *webrtc,
                                                         const char *type,
                                                         JsonNode *data,
                                                         gpointer user_data);
void gnalk_call_signaling_on_screen_subscriber_connection_state (GnalkWebrtc *webrtc,
                                                                    gboolean connected,
                                                                    gpointer user_data);
void gnalk_call_signaling_on_publisher_connection_state (GnalkWebrtc *webrtc,
                                                           gboolean connected,
                                                           gpointer user_data);

/* Signaling message handler (still in call-window.c due to deep integration) */
void gnalk_call_signaling_on_message                (GnalkSignaling *signaling,
                                                       JsonNode *message,
                                                       gpointer user_data);

/* ============ Screen share functions (gnalk-call-screen-share.c) ============ */

void gnalk_call_screen_share_on_toggled          (GtkToggleButton *button,
                                                    GnalkCallWindow *self);
void gnalk_call_screen_share_send_offers_to_all  (GnalkCallWindow *self);
void gnalk_call_screen_share_on_signaling_changed (GnalkSignaling *signaling,
                                                     const char *session_id,
                                                     gboolean is_sharing,
                                                     GnalkCallWindow *self);

/* ============ Control bar functions (gnalk-call-control-bar.c) ============ */

void gnalk_call_control_on_hangup_clicked       (GtkButton *button, GnalkCallWindow *self);
void gnalk_call_control_on_mute_audio_toggled   (GtkToggleButton *button, GnalkCallWindow *self);
void gnalk_call_control_on_mute_video_toggled   (GtkToggleButton *button, GnalkCallWindow *self);
void gnalk_call_control_on_switch_camera_clicked (GtkButton *button, GnalkCallWindow *self);

G_END_DECLS
