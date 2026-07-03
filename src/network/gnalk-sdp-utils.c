/* gnalk-sdp-utils.c
 *
 * Stateless SDP parsing and manipulation utilities.
 */

#include "gnalk-sdp-utils.h"

#include <string.h>

gboolean
gnalk_sdp_validate (const char *sdp,
                     gboolean    is_offer)
{
  (void)is_offer; /* Available for future offer-specific checks */

  if (!sdp || strlen (sdp) < 10)
    {
      g_warning ("SDP validation: empty or too short");
      return FALSE;
    }

  /* Check for required SDP lines */
  if (!g_str_has_prefix (sdp, "v=0"))
    {
      g_warning ("SDP validation: missing version line");
      return FALSE;
    }

  if (!strstr (sdp, "o="))
    {
      g_warning ("SDP validation: missing origin line");
      return FALSE;
    }

  if (!strstr (sdp, "s="))
    {
      g_warning ("SDP validation: missing session name line");
      return FALSE;
    }

  /* Check for at least one media section */
  if (!strstr (sdp, "m=audio") && !strstr (sdp, "m=video") && !strstr (sdp, "m=application"))
    {
      g_warning ("SDP validation: no media sections found");
      return FALSE;
    }

  /* Check for ICE credentials (required for WebRTC) */
  if (!strstr (sdp, "a=ice-ufrag:"))
    {
      g_warning ("SDP validation: missing ice-ufrag");
      return FALSE;
    }

  if (!strstr (sdp, "a=ice-pwd:"))
    {
      g_warning ("SDP validation: missing ice-pwd");
      return FALSE;
    }

  /* Check for fingerprint (DTLS required for WebRTC) */
  if (!strstr (sdp, "a=fingerprint:"))
    {
      g_warning ("SDP validation: missing DTLS fingerprint");
      return FALSE;
    }

  return TRUE;
}

char *
gnalk_sdp_strip_rtx (const char *sdp)
{
  if (!sdp)
    return NULL;

  GString *result = g_string_new (NULL);
  /* Split on LF so both "\r\n" (CRLF) and "\n" (LF-only) SDP are handled, then
   * strip any trailing CR left over from CRLF input so line matching is exact. */
  g_auto(GStrv) lines = g_strsplit (sdp, "\n", -1);
  for (int i = 0; lines[i] != NULL; i++)
    {
      size_t len = strlen (lines[i]);
      if (len > 0 && lines[i][len - 1] == '\r')
        lines[i][len - 1] = '\0';
    }

  /* First pass: find RTX payload types */
  GArray *rtx_pts = g_array_new (FALSE, FALSE, sizeof (int));
  for (int i = 0; lines[i] != NULL; i++)
    {
      /* Match a=rtpmap:XX rtx/ */
      if (g_str_has_prefix (lines[i], "a=rtpmap:"))
        {
          const char *rest = lines[i] + 9;
          int pt = atoi (rest);
          if (strstr (lines[i], " rtx/") != NULL)
            {
              g_array_append_val (rtx_pts, pt);
              g_debug ("strip_rtx: found RTX PT %d", pt);
            }
        }
    }

  /* Second pass: filter out RTX-related lines */
  for (int i = 0; lines[i] != NULL; i++)
    {
      gboolean skip = FALSE;

      /* Skip a=rtpmap:XX rtx/ */
      if (strstr (lines[i], " rtx/") != NULL)
        {
          g_debug ("strip_rtx: removing rtpmap line: %s", lines[i]);
          skip = TRUE;
        }

      /* Skip a=fmtp:XX apt= for RTX payload types */
      if (g_str_has_prefix (lines[i], "a=fmtp:") && strstr (lines[i], "apt=") != NULL)
        {
          const char *rest = lines[i] + 7;
          int pt = atoi (rest);
          for (guint j = 0; j < rtx_pts->len; j++)
            {
              if (pt == g_array_index (rtx_pts, int, j))
                {
                  g_debug ("strip_rtx: removing fmtp line: %s", lines[i]);
                  skip = TRUE;
                  break;
                }
            }
        }

      /* Skip a=ssrc-group:FID */
      if (g_str_has_prefix (lines[i], "a=ssrc-group:FID"))
        {
          g_debug ("strip_rtx: removing ssrc-group:FID line: %s", lines[i]);
          skip = TRUE;
        }

      if (!skip)
        {
          /* For m= lines, remove RTX payload types from the format list */
          if (g_str_has_prefix (lines[i], "m=video") || g_str_has_prefix (lines[i], "m=audio"))
            {
              /* Parse and rebuild the m= line without RTX PTs */
              g_auto(GStrv) parts = g_strsplit (lines[i], " ", -1);
              GString *new_mline = g_string_new (NULL);
              for (int p = 0; parts[p] != NULL; p++)
                {
                  gboolean is_rtx_pt = FALSE;
                  /* Check if this part is an RTX payload type (after the 3rd field) */
                  if (p >= 3)
                    {
                      int pt = atoi (parts[p]);
                      for (guint j = 0; j < rtx_pts->len; j++)
                        {
                          if (pt == g_array_index (rtx_pts, int, j))
                            {
                              is_rtx_pt = TRUE;
                              g_debug ("strip_rtx: removing PT %d from m= line", pt);
                              break;
                            }
                        }
                    }
                  if (!is_rtx_pt)
                    {
                      if (new_mline->len > 0)
                        g_string_append_c (new_mline, ' ');
                      g_string_append (new_mline, parts[p]);
                    }
                }
              g_string_append (result, new_mline->str);
              g_string_append (result, "\r\n");
              g_string_free (new_mline, TRUE);
            }
          else
            {
              g_string_append (result, lines[i]);
              g_string_append (result, "\r\n");
            }
        }
    }

  g_array_free (rtx_pts, TRUE);
  g_debug ("strip_rtx: SDP after stripping RTX:\n%s", result->str);
  return g_string_free (result, FALSE);
}

#ifdef HAVE_GSTREAMER_WEBRTC
gboolean
gnalk_sdp_ensure_bundle_group (GstSDPMessage *sdp)
{
  g_return_val_if_fail (sdp != NULL, FALSE);

  /* Check if BUNDLE group already exists */
  for (guint i = 0; i < gst_sdp_message_attributes_len (sdp); i++)
    {
      const GstSDPAttribute *attr = gst_sdp_message_get_attribute (sdp, i);
      if (g_strcmp0 (attr->key, "group") == 0 &&
          g_str_has_prefix (attr->value, "BUNDLE"))
        {
          g_debug ("ensure_bundle_group: SDP already has BUNDLE group: %s", attr->value);
          return FALSE;
        }
    }

  /* No BUNDLE group found, need to add one */
  if (gst_sdp_message_medias_len (sdp) == 0)
    {
      g_debug ("ensure_bundle_group: no media sections, cannot add BUNDLE");
      return FALSE;
    }

  /* Build list of mids from media sections */
  g_autoptr(GString) mids = g_string_new (NULL);
  for (guint i = 0; i < gst_sdp_message_medias_len (sdp); i++)
    {
      const GstSDPMedia *media = gst_sdp_message_get_media (sdp, i);
      const char *mid = gst_sdp_media_get_attribute_val (media, "mid");
      if (mid)
        {
          if (mids->len > 0)
            g_string_append_c (mids, ' ');
          g_string_append (mids, mid);
        }
    }

  if (mids->len == 0)
    {
      g_debug ("ensure_bundle_group: no mids found in media sections");
      return FALSE;
    }

  /* Add the BUNDLE group attribute */
  g_autofree char *bundle_attr = g_strdup_printf ("BUNDLE %s", mids->str);
  gst_sdp_message_add_attribute (sdp, "group", bundle_attr);
  g_message ("ensure_bundle_group: added a=group:%s", bundle_attr);

  return TRUE;
}
#endif
