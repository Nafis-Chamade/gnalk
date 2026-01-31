/* gnalk-sdp-utils.h
 *
 * Stateless SDP parsing and manipulation utilities.
 * These functions handle common SDP transformations needed for
 * WebRTC compatibility with various clients and MCUs.
 */

#pragma once

#include <glib.h>

#ifdef HAVE_GSTREAMER_WEBRTC
#include <gst/sdp/sdp.h>
#endif

G_BEGIN_DECLS

/**
 * gnalk_sdp_validate:
 * @sdp: SDP string to validate
 * @is_offer: TRUE if this is an offer, FALSE if it's an answer
 *
 * Validates that an SDP string contains required WebRTC fields.
 * Checks for: version, origin, session name, at least one media section,
 * ICE credentials (ice-ufrag, ice-pwd), and DTLS fingerprint.
 *
 * Returns: TRUE if valid, FALSE otherwise (logs warnings for failures)
 */
gboolean gnalk_sdp_validate (const char *sdp,
                              gboolean    is_offer);

/**
 * gnalk_sdp_strip_rtx:
 * @sdp: SDP string to process
 *
 * Removes RTX (retransmission) related entries from SDP.
 * This is a workaround for GStreamer's webrtcbin RTX handling issues
 * that can cause packet loss when receiving from some peers.
 *
 * Strips:
 * - a=rtpmap:XX rtx/... lines
 * - a=fmtp:XX apt=... lines (RTX format params)
 * - a=ssrc-group:FID ... lines (RTX SSRC groups)
 * - RTX payload types from m= lines
 *
 * Returns: (transfer full): newly allocated SDP string without RTX,
 *          free with g_free()
 */
char *gnalk_sdp_strip_rtx (const char *sdp);

#ifdef HAVE_GSTREAMER_WEBRTC
/**
 * gnalk_sdp_ensure_bundle_group:
 * @sdp: GstSDPMessage to modify (in-place)
 *
 * Ensures the SDP message has a BUNDLE group attribute.
 * GStreamer's webrtcbin sometimes omits the BUNDLE group in receive-only
 * answers, which causes remote peers to create separate transports.
 *
 * If no BUNDLE group exists, adds one containing all media section mids.
 *
 * Returns: TRUE if BUNDLE group was added, FALSE if already present or
 *          no media sections found
 */
gboolean gnalk_sdp_ensure_bundle_group (GstSDPMessage *sdp);
#endif

G_END_DECLS
