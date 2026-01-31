/* gnalk-camera-utils.c
 *
 * Shared camera detection and rotation utilities.
 */

#include "gnalk-camera-utils.h"

#include <gio/gio.h>
#include <string.h>

/* Resolution presets per camera (before rotation - portrait orientation)
 * After 90° rotation these become landscape.
 *
 * Front camera (imx371 on OP6): supports up to ~3840x2400, can use higher res
 * Back camera (imx376 on OP6): max 2560x1600, must use lower res
 * Generic cameras: use conservative presets that work everywhere
 */
static const struct {
  gint width;
  gint height;
  gint encode_crop;
  gint preview_crop;
} quality_presets_front[] = {
  [GNALK_CAMERA_QUALITY_LOW]    = { 1080, 1440, 30, 20 },
  [GNALK_CAMERA_QUALITY_MEDIUM] = { 1440, 1920, 40, 25 },
  [GNALK_CAMERA_QUALITY_HIGH]   = { 2580, 3440, 50, 30 },
};

static const struct {
  gint width;
  gint height;
  gint encode_crop;
  gint preview_crop;
} quality_presets_back[] = {
  [GNALK_CAMERA_QUALITY_LOW]    = { 1080, 1440, 30, 0 },
  [GNALK_CAMERA_QUALITY_MEDIUM] = { 1200, 1600, 35, 0 },
  [GNALK_CAMERA_QUALITY_HIGH]   = { 1440, 1920, 40, 0 },
};

/* Generic presets for laptops/desktops - conservative resolutions */
static const struct {
  gint width;
  gint height;
  gint encode_crop;
  gint preview_crop;
} quality_presets_generic[] = {
  [GNALK_CAMERA_QUALITY_LOW]    = { 640, 480, 0, 0 },
  [GNALK_CAMERA_QUALITY_MEDIUM] = { 1280, 720, 0, 0 },
  [GNALK_CAMERA_QUALITY_HIGH]   = { 1920, 1080, 0, 0 },
};

GnalkCameraConfig
gnalk_camera_get_config (GnalkCameraQuality quality,
                          GnalkCameraPosition position)
{
  GnalkCameraConfig config = { 0 };

  if (quality > GNALK_CAMERA_QUALITY_HIGH)
    quality = GNALK_CAMERA_QUALITY_HIGH;

  if (position == GNALK_CAMERA_FRONT)
    {
      config.src_width = quality_presets_front[quality].width;
      config.src_height = quality_presets_front[quality].height;
      config.encode_crop = quality_presets_front[quality].encode_crop;
      config.preview_crop = quality_presets_front[quality].preview_crop;
    }
  else if (position == GNALK_CAMERA_BACK)
    {
      config.src_width = quality_presets_back[quality].width;
      config.src_height = quality_presets_back[quality].height;
      config.encode_crop = quality_presets_back[quality].encode_crop;
      config.preview_crop = quality_presets_back[quality].preview_crop;
    }
  else
    {
      /* Generic/unknown camera - use safe defaults */
      config.src_width = quality_presets_generic[quality].width;
      config.src_height = quality_presets_generic[quality].height;
      config.encode_crop = quality_presets_generic[quality].encode_crop;
      config.preview_crop = quality_presets_generic[quality].preview_crop;
    }

  return config;
}

GnalkCameraQuality
gnalk_camera_quality_from_settings (void)
{
  g_autoptr(GSettings) settings = g_settings_new ("net.skilpa.gnalk");
  gint value = g_settings_get_int (settings, "camera-quality");

  if (value < 0 || value > GNALK_CAMERA_QUALITY_HIGH)
    return GNALK_CAMERA_QUALITY_HIGH;

  return (GnalkCameraQuality) value;
}

#ifdef HAVE_GSTREAMER

void
gnalk_camera_get_rotation_from_device (GstDevice *device,
                                         gboolean  *out_is_front,
                                         gint      *out_rotation)
{
  gboolean is_front = TRUE;
  gint rotation = 0;
  gboolean got_rotation = FALSE;

  GstStructure *props = gst_device_get_properties (device);
  g_autofree gchar *name = gst_device_get_display_name (device);

  if (props)
    {
      /* Try api.libcamera.location property first (libcamera <0.6) */
      const gchar *location = gst_structure_get_string (props, "api.libcamera.location");
      if (location)
        is_front = (g_strcmp0 (location, "front") == 0);
      else
        is_front = (name && strstr (name, "Front") != NULL);

      /* Try api.libcamera.rotation property */
      if (gst_structure_get_int (props, "api.libcamera.rotation", &rotation))
        {
          got_rotation = TRUE;
        }
      else
        {
          const gchar *rot_str = gst_structure_get_string (props, "api.libcamera.rotation");
          if (rot_str)
            {
              rotation = atoi (rot_str);
              got_rotation = TRUE;
            }
        }
      gst_structure_free (props);
    }
  else
    {
      is_front = (name && strstr (name, "Front") != NULL);
    }

  /* Fall back to OnePlus 6 defaults if rotation not found */
  if (!got_rotation)
    rotation = is_front ? 90 : 270;

  if (out_is_front)
    *out_is_front = is_front;
  if (out_rotation)
    *out_rotation = rotation;
}

#endif /* HAVE_GSTREAMER */

gint
gnalk_camera_rotation_to_video_direction (gint rotation)
{
  if (rotation == 90)
    return 3;  /* counterclockwise */
  else if (rotation == 180)
    return 2;  /* rotate-180 */
  else if (rotation == 270)
    return 1;  /* clockwise */
  return 0;    /* identity */
}

#ifdef HAVE_GSTREAMER

GstElement *
gnalk_camera_utils_create_audio_source (void)
{
  GstElement *src = gst_element_factory_make ("pulsesrc", "audiosrc");

  if (src)
    {
      /* Configure for low latency recording */
      g_object_set (src,
                    "buffer-time", (gint64) 200000,   /* 200ms buffer */
                    "latency-time", (gint64) 20000,   /* 20ms latency */
                    NULL);
      g_message ("CameraUtils: created pulsesrc audio source");
    }
  else
    {
      /* Fall back to test source for testing on systems without pulseaudio */
      src = gst_element_factory_make ("audiotestsrc", "audiosrc");
      if (src)
        {
          g_object_set (src, "is-live", TRUE, "wave", 0, NULL);  /* silence */
          g_warning ("CameraUtils: pulsesrc not available, using audiotestsrc");
        }
      else
        {
          g_warning ("CameraUtils: failed to create any audio source");
        }
    }

  return src;
}

#endif /* HAVE_GSTREAMER */
