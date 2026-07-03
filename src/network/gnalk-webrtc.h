#pragma once

#include <glib-object.h>
#include <gdk/gdk.h>
#include <json-glib/json-glib.h>
#include "gnalk-camera-utils.h"  /* For GnalkCameraPosition */

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#endif

#ifdef HAVE_GSTREAMER_WEBRTC
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#endif

G_BEGIN_DECLS

#define GNALK_TYPE_WEBRTC (gnalk_webrtc_get_type ())
G_DECLARE_FINAL_TYPE (GnalkWebrtc, gnalk_webrtc, GNALK, WEBRTC, GObject)

typedef void (*GnalkWebrtcSignalCallback) (GnalkWebrtc *webrtc,
                                              const char   *type,
                                              JsonNode     *data,
                                              gpointer      user_data);

typedef void (*GnalkWebrtcRemoteVideoCallback) (GnalkWebrtc *webrtc,
                                                   GdkTexture   *texture,
                                                   gpointer      user_data);

typedef void (*GnalkWebrtcLocalVideoCallback) (GnalkWebrtc *webrtc,
                                                  GdkTexture   *texture,
                                                  gpointer      user_data);

typedef void (*GnalkWebrtcConnectionStateCallback) (GnalkWebrtc *webrtc,
                                                       gboolean      connected,
                                                       gpointer      user_data);

typedef void (*GnalkWebrtcSpeakingCallback) (GnalkWebrtc *webrtc,
                                                gboolean      speaking,
                                                gpointer      user_data);

/* Call state machine states */
typedef enum {
  GNALK_CALL_STATE_IDLE,
  GNALK_CALL_STATE_CONNECTING,
  GNALK_CALL_STATE_NEGOTIATING,
  GNALK_CALL_STATE_CONNECTED,
  GNALK_CALL_STATE_RECONNECTING,
  GNALK_CALL_STATE_FAILED,
  GNALK_CALL_STATE_CLOSED
} GnalkCallState;

typedef void (*GnalkWebrtcCallStateCallback) (GnalkWebrtc   *webrtc,
                                                  GnalkCallState state,
                                                  gpointer        user_data);

GnalkWebrtc *gnalk_webrtc_new (void);

void gnalk_webrtc_set_stun_server (GnalkWebrtc *self,
                                     const char   *stun_url);

void gnalk_webrtc_set_turn_server (GnalkWebrtc *self,
                                     const char   *turn_url,
                                     const char   *username,
                                     const char   *password);

void gnalk_webrtc_start_call (GnalkWebrtc *self,
                                const char   *peer_id);

void gnalk_webrtc_start_screen_share (GnalkWebrtc *self,
                                        const char   *peer_id,
                                        guint32       pipewire_node_id,
                                        int           pipewire_fd);

gboolean gnalk_webrtc_is_screen_share (GnalkWebrtc *self);

void gnalk_webrtc_end_call (GnalkWebrtc *self);

void gnalk_webrtc_handle_offer (GnalkWebrtc *self,
                                   const char   *sdp);

void gnalk_webrtc_handle_answer (GnalkWebrtc *self,
                                    const char   *sdp);

void gnalk_webrtc_handle_ice_candidate (GnalkWebrtc *self,
                                           const char   *candidate,
                                           int           sdp_mline_index,
                                           const char   *sdp_mid);

void gnalk_webrtc_set_signal_callback (GnalkWebrtc               *self,
                                          GnalkWebrtcSignalCallback  callback,
                                          gpointer                    user_data,
                                          GDestroyNotify              notify);

void gnalk_webrtc_set_remote_video_callback (GnalkWebrtc                    *self,
                                                GnalkWebrtcRemoteVideoCallback  callback,
                                                gpointer                         user_data,
                                                GDestroyNotify                   notify);

void gnalk_webrtc_set_local_video_callback (GnalkWebrtc                   *self,
                                               GnalkWebrtcLocalVideoCallback  callback,
                                               gpointer                        user_data);

void gnalk_webrtc_set_connection_state_callback (GnalkWebrtc                        *self,
                                                    GnalkWebrtcConnectionStateCallback  callback,
                                                    gpointer                              user_data,
                                                    GDestroyNotify                        notify);

void gnalk_webrtc_set_speaking_callback (GnalkWebrtc                  *self,
                                            GnalkWebrtcSpeakingCallback   callback,
                                            gpointer                       user_data,
                                            GDestroyNotify                 notify);

#ifdef HAVE_GSTREAMER
GstElement *gnalk_webrtc_get_video_sink (GnalkWebrtc *self);
GstElement *gnalk_webrtc_get_local_video_sink (GnalkWebrtc *self);
#endif

GdkPaintable *gnalk_webrtc_get_local_video_paintable (GnalkWebrtc *self);

void gnalk_webrtc_set_video_enabled (GnalkWebrtc *self,
                                        gboolean      enabled);

void gnalk_webrtc_set_audio_enabled (GnalkWebrtc *self,
                                        gboolean      enabled);

void gnalk_webrtc_set_camera_position (GnalkWebrtc        *self,
                                          GnalkCameraPosition position);

GnalkCameraPosition gnalk_webrtc_get_camera_position (GnalkWebrtc *self);

void gnalk_webrtc_switch_camera (GnalkWebrtc *self);

void gnalk_webrtc_request_keyframe (GnalkWebrtc *self);

void gnalk_webrtc_enable_continuous_pli (GnalkWebrtc *self,
                                           gboolean      enable,
                                           guint         interval_ms);

void gnalk_webrtc_set_call_state_callback (GnalkWebrtc                  *self,
                                              GnalkWebrtcCallStateCallback  callback,
                                              gpointer                       user_data);

GnalkCallState gnalk_webrtc_get_call_state (GnalkWebrtc *self);

G_END_DECLS
