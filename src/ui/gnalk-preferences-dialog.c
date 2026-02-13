#include "gnalk-preferences-dialog.h"
#include "gnalk-application.h"
#include "gnalk-camera-preview.h"
#include "gnalk-camera-utils.h"
#include <glib/gi18n.h>

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video-info.h>
#include <math.h>
#endif

/* Device info stored in list stores */
typedef struct {
  GObject parent_instance;
  char *display_name;
  char *device_id;
} DeviceInfo;

typedef struct {
  GObjectClass parent_class;
} DeviceInfoClass;

G_DEFINE_TYPE (DeviceInfo, device_info, G_TYPE_OBJECT)

static void
device_info_finalize (GObject *object)
{
  DeviceInfo *self = (DeviceInfo *)object;
  g_free (self->display_name);
  g_free (self->device_id);
  G_OBJECT_CLASS (device_info_parent_class)->finalize (object);
}

static void
device_info_class_init (DeviceInfoClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = device_info_finalize;
}

static void
device_info_init (DeviceInfo *self)
{
}

static DeviceInfo *
device_info_new (const char *display_name, const char *device_id)
{
  DeviceInfo *info = g_object_new (device_info_get_type (), NULL);
  info->display_name = g_strdup (display_name);
  info->device_id = g_strdup (device_id);
  return info;
}

struct _GnalkPreferencesDialog
{
  AdwDialog parent_instance;

  /* UI widgets */
  GtkWidget *close_button;

  /* Appearance section */
  GtkWidget *color_scheme_row;
  GtkStringList *color_scheme_names;
  GtkWidget *background_style_row;
  GtkStringList *background_style_names;
  GtkWidget *language_row;
  GtkStringList *language_names;

  /* Video section */
  GtkWidget *video_source_row;
  GtkWidget *video_quality_row;
  GtkWidget *video_preview_overlay;
  GtkWidget *video_placeholder_label;
  GnalkCameraPreview *camera_preview;
  GListStore *video_devices_store;
  GtkStringList *video_device_names;
  GtkStringList *video_quality_names;

  /* Audio section */
  GtkWidget *audio_source_row;
  GtkWidget *audio_level_bar;
  GListStore *audio_devices_store;
  GtkStringList *audio_device_names;

#ifdef HAVE_GSTREAMER
  /* Audio monitoring pipeline */
  GstElement *audio_pipeline;
  guint audio_bus_watch_id;
#endif

  /* Settings */
  GSettings *settings;
};

G_DEFINE_FINAL_TYPE (GnalkPreferencesDialog, gnalk_preferences_dialog, ADW_TYPE_DIALOG)

static void enumerate_video_devices (GnalkPreferencesDialog *self);
static void enumerate_audio_devices (GnalkPreferencesDialog *self);
static void start_video_preview (GnalkPreferencesDialog *self);
static void stop_video_preview (GnalkPreferencesDialog *self);
static void start_audio_monitoring (GnalkPreferencesDialog *self);
static void stop_audio_monitoring (GnalkPreferencesDialog *self);

#ifdef HAVE_GSTREAMER
typedef struct {
  GtkWidget *level_bar;
  gdouble level;
} LevelUpdate;

static gboolean
update_level_bar_in_main_thread (gpointer user_data)
{
  LevelUpdate *update = user_data;

  if (update->level_bar)
    {
      gtk_level_bar_set_value (GTK_LEVEL_BAR (update->level_bar), update->level);
      g_object_remove_weak_pointer (G_OBJECT (update->level_bar), (gpointer *)&update->level_bar);
    }

  g_free (update);
  return G_SOURCE_REMOVE;
}

static gboolean
on_audio_bus_message (GstBus     *bus,
                      GstMessage *msg,
                      gpointer    user_data)
{
  GnalkPreferencesDialog *self = GNALK_PREFERENCES_DIALOG (user_data);

  switch (GST_MESSAGE_TYPE (msg))
    {
    case GST_MESSAGE_ERROR:
      {
        GError *err = NULL;
        gchar *debug = NULL;
        gst_message_parse_error (msg, &err, &debug);
        g_warning ("Preferences: Audio pipeline error: %s (%s)", err->message, debug ? debug : "");
        g_error_free (err);
        g_free (debug);
        stop_audio_monitoring (self);
      }
      break;

    case GST_MESSAGE_ELEMENT:
      {
        const GstStructure *s = gst_message_get_structure (msg);
        const gchar *name = gst_structure_get_name (s);

        if (g_strcmp0 (name, "level") == 0)
          {
            const GValue *rms_arr = gst_structure_get_value (s, "rms");
            gdouble rms_db = -60.0;

            if (rms_arr)
              {
                /* Handle GValueArray (GLib), GstList, and GstArray types */
                if (G_VALUE_HOLDS (rms_arr, G_TYPE_VALUE_ARRAY))
                  {
                    /* GLib's GValueArray (deprecated but still used by level element) */
                    GValueArray *arr = g_value_get_boxed (rms_arr);
                    if (arr && arr->n_values > 0)
                      {
                        const GValue *rms_val = g_value_array_get_nth (arr, 0);
                        if (rms_val && G_VALUE_HOLDS_DOUBLE (rms_val))
                          rms_db = g_value_get_double (rms_val);
                      }
                  }
                else if (GST_VALUE_HOLDS_LIST (rms_arr))
                  {
                    const GValue *rms_val = gst_value_list_get_value (rms_arr, 0);
                    if (rms_val && G_VALUE_HOLDS_DOUBLE (rms_val))
                      rms_db = g_value_get_double (rms_val);
                  }
                else if (GST_VALUE_HOLDS_ARRAY (rms_arr))
                  {
                    const GValue *rms_val = gst_value_array_get_value (rms_arr, 0);
                    if (rms_val && G_VALUE_HOLDS_DOUBLE (rms_val))
                      rms_db = g_value_get_double (rms_val);
                  }
              }

            /* Map dB linearly to 0-1 range for better visualization
             * -60 dB -> 0, 0 dB -> 1 */
            gdouble level = (rms_db + 60.0) / 60.0;
            level = CLAMP (level, 0.0, 1.0);

            LevelUpdate *update = g_new (LevelUpdate, 1);
            update->level_bar = self->audio_level_bar;
            update->level = level;
            g_object_add_weak_pointer (G_OBJECT (update->level_bar), (gpointer *)&update->level_bar);
            g_idle_add (update_level_bar_in_main_thread, update);
          }
      }
      break;

    default:
      break;
    }

  return TRUE;
}
#endif /* HAVE_GSTREAMER */

static void
enumerate_video_devices (GnalkPreferencesDialog *self)
{
#ifdef HAVE_GSTREAMER
  gtk_string_list_splice (self->video_device_names, 0,
                          g_list_model_get_n_items (G_LIST_MODEL (self->video_device_names)),
                          NULL);
  g_list_store_remove_all (self->video_devices_store);

  GstDeviceMonitor *monitor = gst_device_monitor_new ();
  gst_device_monitor_add_filter (monitor, "Video/Source", NULL);

  if (!gst_device_monitor_start (monitor))
    {
      g_warning ("Preferences: Failed to start video device monitor");
      gst_object_unref (monitor);
      /* No cameras - add None option */
      gtk_string_list_append (self->video_device_names, _("None"));
      g_list_store_append (self->video_devices_store, device_info_new (_("None"), ""));
      return;
    }

  GList *devices = gst_device_monitor_get_devices (monitor);
  guint front_index = G_MAXUINT;
  guint device_count = 0;

  for (GList *l = devices; l != NULL; l = l->next)
    {
      GstDevice *device = GST_DEVICE (l->data);
      g_autofree gchar *name = gst_device_get_display_name (device);
      GstStructure *props = gst_device_get_properties (device);
      g_autofree gchar *device_id = NULL;

      if (props)
        {
          /* Try to get a unique device identifier */
          const gchar *path = gst_structure_get_string (props, "device.path");
          if (!path)
            path = gst_structure_get_string (props, "object.path");
          if (!path)
            path = gst_structure_get_string (props, "api.libcamera.path");

          device_id = g_strdup (path ? path : name);
          gst_structure_free (props);
        }
      else
        {
          device_id = g_strdup (name);
        }

      gtk_string_list_append (self->video_device_names, name);
      g_list_store_append (self->video_devices_store, device_info_new (name, device_id));

      /* Track front camera index */
      if (name && strstr (name, "Front"))
        front_index = device_count;
      device_count++;
    }

  g_list_free_full (devices, gst_object_unref);
  gst_device_monitor_stop (monitor);
  gst_object_unref (monitor);

  /* If no cameras found, add None option */
  if (device_count == 0)
    {
      gtk_string_list_append (self->video_device_names, _("None"));
      g_list_store_append (self->video_devices_store, device_info_new (_("None"), ""));
      return;
    }

  /* Select saved device, or front camera, or first available */
  g_autofree char *preferred = g_settings_get_string (self->settings, "preferred-video-device");
  guint selected = (front_index != G_MAXUINT) ? front_index : 0;

  if (preferred && *preferred)
    {
      guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->video_devices_store));
      for (guint i = 0; i < n_items; i++)
        {
          DeviceInfo *info = g_list_model_get_item (G_LIST_MODEL (self->video_devices_store), i);
          if (g_strcmp0 (info->device_id, preferred) == 0)
            {
              selected = i;
              g_object_unref (info);
              break;
            }
          g_object_unref (info);
        }
    }

  adw_combo_row_set_selected (ADW_COMBO_ROW (self->video_source_row), selected);
#endif
}

static void
enumerate_audio_devices (GnalkPreferencesDialog *self)
{
#ifdef HAVE_GSTREAMER
  gtk_string_list_splice (self->audio_device_names, 0,
                          g_list_model_get_n_items (G_LIST_MODEL (self->audio_device_names)),
                          NULL);
  g_list_store_remove_all (self->audio_devices_store);

  /* Add "None" option first */
  gtk_string_list_append (self->audio_device_names, _("None"));
  g_list_store_append (self->audio_devices_store, device_info_new (_("None"), ""));

  GstDeviceMonitor *monitor = gst_device_monitor_new ();
  gst_device_monitor_add_filter (monitor, "Audio/Source", NULL);

  if (!gst_device_monitor_start (monitor))
    {
      g_warning ("Preferences: Failed to start audio device monitor");
      gst_object_unref (monitor);
      return;
    }

  GList *devices = gst_device_monitor_get_devices (monitor);

  guint device_index = 1;  /* Start after "None" */
  for (GList *l = devices; l != NULL; l = l->next)
    {
      GstDevice *device = GST_DEVICE (l->data);
      g_autofree gchar *name = gst_device_get_display_name (device);

      /* Skip monitor devices - they're output loopbacks, not real microphones */
      if (name && g_str_has_prefix (name, "Monitor of "))
        continue;

      /* Store device index as ID - we'll look up the GstDevice when creating pipeline */
      g_autofree gchar *device_id = g_strdup_printf ("%u", device_index);

      gtk_string_list_append (self->audio_device_names, name);
      g_list_store_append (self->audio_devices_store, device_info_new (name, device_id));
      device_index++;
    }

  g_list_free_full (devices, gst_object_unref);
  gst_device_monitor_stop (monitor);
  gst_object_unref (monitor);

  /* Select saved device or default to first real device if available */
  g_autofree char *preferred = g_settings_get_string (self->settings, "preferred-audio-device");
  guint selected = 0;
  guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->audio_devices_store));

  if (preferred && *preferred)
    {
      for (guint i = 0; i < n_items; i++)
        {
          DeviceInfo *info = g_list_model_get_item (G_LIST_MODEL (self->audio_devices_store), i);
          if (g_strcmp0 (info->device_id, preferred) == 0)
            {
              selected = i;
              g_object_unref (info);
              break;
            }
          g_object_unref (info);
        }
    }
  else if (n_items > 1)
    {
      /* Default to first real device (skip "None") */
      selected = 1;
    }

  adw_combo_row_set_selected (ADW_COMBO_ROW (self->audio_source_row), selected);
#endif
}

static void
start_video_preview (GnalkPreferencesDialog *self)
{
  stop_video_preview (self);

  guint selected = adw_combo_row_get_selected (ADW_COMBO_ROW (self->video_source_row));
  DeviceInfo *info = g_list_model_get_item (G_LIST_MODEL (self->video_devices_store), selected);

  if (!info || !info->device_id || !*info->device_id)
    {
      /* "None" selected or invalid - show placeholder, hide preview */
      gtk_widget_set_visible (self->video_placeholder_label, TRUE);
      gtk_widget_set_visible (GTK_WIDGET (self->camera_preview), FALSE);
      if (info)
        g_object_unref (info);
      return;
    }

  /* Hide placeholder, show preview */
  gtk_widget_set_visible (self->video_placeholder_label, FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->camera_preview), TRUE);

  /* Determine camera position from device name */
  GnalkCameraPreviewPosition position = GNALK_CAMERA_PREVIEW_GENERIC;
  if (info->display_name)
    {
      if (strstr (info->display_name, "Front"))
        position = GNALK_CAMERA_PREVIEW_FRONT;
      else if (strstr (info->display_name, "Back"))
        position = GNALK_CAMERA_PREVIEW_BACK;
    }

  g_object_unref (info);

  gnalk_camera_preview_set_position (self->camera_preview, position);
  gnalk_camera_preview_start (self->camera_preview);
}

static void
stop_video_preview (GnalkPreferencesDialog *self)
{
  gnalk_camera_preview_stop (self->camera_preview);
}

static void
start_audio_monitoring (GnalkPreferencesDialog *self)
{
#ifdef HAVE_GSTREAMER
  stop_audio_monitoring (self);

  guint selected = adw_combo_row_get_selected (ADW_COMBO_ROW (self->audio_source_row));
  if (selected == 0)
    {
      /* "None" selected */
      gtk_level_bar_set_value (GTK_LEVEL_BAR (self->audio_level_bar), 0.0);
      return;
    }

  /* Find the selected device using GstDeviceMonitor */
  GstDeviceMonitor *monitor = gst_device_monitor_new ();
  gst_device_monitor_add_filter (monitor, "Audio/Source", NULL);

  if (!gst_device_monitor_start (monitor))
    {
      g_warning ("Preferences: Failed to start audio device monitor for playback");
      gst_object_unref (monitor);
      return;
    }

  GList *devices = gst_device_monitor_get_devices (monitor);
  GstDevice *target_device = NULL;
  guint device_index = 1;

  for (GList *l = devices; l != NULL; l = l->next)
    {
      GstDevice *device = GST_DEVICE (l->data);
      g_autofree gchar *name = gst_device_get_display_name (device);

      /* Skip monitor devices */
      if (name && g_str_has_prefix (name, "Monitor of "))
        continue;

      if (device_index == selected)
        {
          target_device = gst_object_ref (device);
          break;
        }
      device_index++;
    }

  g_list_free_full (devices, gst_object_unref);
  gst_device_monitor_stop (monitor);
  gst_object_unref (monitor);

  if (!target_device)
    {
      g_warning ("Preferences: Could not find audio device at index %u", selected);
      return;
    }

  /* Get device properties to construct the PulseAudio device name */
  GstStructure *props = gst_device_get_properties (target_device);
  g_autofree gchar *pa_device_name = NULL;

  if (props)
    {
      const gchar *bus_path = gst_structure_get_string (props, "device.bus_path");
      const gchar *profile_name = gst_structure_get_string (props, "device.profile.name");

      if (bus_path && profile_name)
        {
          /* Convert profile name: "HiFi: Headset: source" -> "HiFi__Headset__source"
           * Each ": " becomes "__" (double underscore) */
          g_autofree gchar *profile_converted = g_strdup (profile_name);

          /* Replace ": " with "__" */
          GString *result = g_string_new ("");
          for (gchar *p = profile_converted; *p; p++)
            {
              if (*p == ':' && *(p + 1) == ' ')
                {
                  g_string_append (result, "__");
                  p++; /* Skip the space */
                }
              else
                {
                  g_string_append_c (result, *p);
                }
            }

          pa_device_name = g_strdup_printf ("alsa_input.%s.%s", bus_path, result->str);
          g_string_free (result, TRUE);
        }

      gst_structure_free (props);
    }

  GstElement *src;

  if (pa_device_name)
    {
      /* Create pulsesrc with explicit device name */
      src = gst_element_factory_make ("pulsesrc", "audiosrc");
      if (src)
        g_object_set (src, "device", pa_device_name, NULL);
    }
  else
    {
      /* Fallback to gst_device_create_element */
      src = gst_device_create_element (target_device, "audiosrc");
    }

  gst_object_unref (target_device);

  if (!src)
    {
      g_warning ("Preferences: Failed to create audio source element");
      return;
    }

  GstElement *pipeline = gst_pipeline_new ("audio-monitor");
  GstElement *convert = gst_element_factory_make ("audioconvert", "convert");
  GstElement *resample = gst_element_factory_make ("audioresample", "resample");
  GstElement *level = gst_element_factory_make ("level", "level");
  GstElement *sink = gst_element_factory_make ("fakesink", "sink");

  if (!convert || !resample || !level || !sink)
    {
      g_warning ("Preferences: Failed to create audio monitoring elements");
      gst_object_unref (src);
      if (convert) gst_object_unref (convert);
      if (resample) gst_object_unref (resample);
      if (level) gst_object_unref (level);
      if (sink) gst_object_unref (sink);
      gst_object_unref (pipeline);
      return;
    }

  /* Configure level element - emit messages every 100ms */
  g_object_set (level,
                "interval", (guint64) 100000000,  /* 100ms in nanoseconds */
                "post-messages", TRUE,
                NULL);

  /* Configure fakesink to not sync to clock */
  g_object_set (sink, "sync", FALSE, NULL);

  gst_bin_add_many (GST_BIN (pipeline), src, convert, resample, level, sink, NULL);

  if (!gst_element_link_many (src, convert, resample, level, sink, NULL))
    {
      g_warning ("Preferences: Failed to link audio monitoring pipeline");
      gst_object_unref (pipeline);
      return;
    }

  /* Watch bus for level messages */
  GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  self->audio_bus_watch_id = gst_bus_add_watch (bus, on_audio_bus_message, self);
  gst_object_unref (bus);

  self->audio_pipeline = pipeline;

  GstStateChangeReturn ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    {
      g_warning ("Preferences: Failed to start audio monitoring pipeline");
      stop_audio_monitoring (self);
    }
#endif
}

static void
stop_audio_monitoring (GnalkPreferencesDialog *self)
{
#ifdef HAVE_GSTREAMER
  if (self->audio_bus_watch_id)
    {
      g_source_remove (self->audio_bus_watch_id);
      self->audio_bus_watch_id = 0;
    }

  if (self->audio_pipeline)
    {
      gst_element_set_state (self->audio_pipeline, GST_STATE_NULL);
      gst_element_get_state (self->audio_pipeline, NULL, NULL, GST_SECOND);
      gst_object_unref (self->audio_pipeline);
      self->audio_pipeline = NULL;
    }

  gtk_level_bar_set_value (GTK_LEVEL_BAR (self->audio_level_bar), 0.0);
#endif
}

static void
on_color_scheme_changed (AdwComboRow             *row,
                         GParamSpec              *pspec G_GNUC_UNUSED,
                         GnalkPreferencesDialog *self)
{
  guint selected = adw_combo_row_get_selected (row);
  g_settings_set_int (self->settings, "color-scheme", (gint) selected);
}

static void
on_background_style_changed (AdwComboRow             *row,
                              GParamSpec              *pspec G_GNUC_UNUSED,
                              GnalkPreferencesDialog *self)
{
  guint selected = adw_combo_row_get_selected (row);
  g_settings_set_int (self->settings, "conversation-background", (gint) selected);
}

static const char *language_codes[] = { "", "en", "fr", "nl", "es", "it", "de" };
static const int n_languages = G_N_ELEMENTS (language_codes);

static void
on_language_changed (AdwComboRow             *row,
                     GParamSpec              *pspec G_GNUC_UNUSED,
                     GnalkPreferencesDialog *self)
{
  guint selected = adw_combo_row_get_selected (row);
  if (selected < (guint) n_languages)
    g_settings_set_string (self->settings, "preferred-language", language_codes[selected]);
}

static void
on_video_source_changed (AdwComboRow             *row,
                         GParamSpec              *pspec,
                         GnalkPreferencesDialog *self)
{
  guint selected = adw_combo_row_get_selected (row);

  /* Save preference */
  DeviceInfo *info = g_list_model_get_item (G_LIST_MODEL (self->video_devices_store), selected);
  if (info)
    {
      g_settings_set_string (self->settings, "preferred-video-device", info->device_id);

      /* Save camera position based on device name (Front=0, Back=1, Generic=2) */
      gint position = 2;  /* Default to generic */
      if (info->display_name)
        {
          if (strstr (info->display_name, "Front"))
            position = 0;
          else if (strstr (info->display_name, "Back"))
            position = 1;
        }
      g_settings_set_int (self->settings, "camera-position", position);

      g_object_unref (info);
    }

  start_video_preview (self);
}

static void
on_audio_source_changed (AdwComboRow             *row,
                         GParamSpec              *pspec,
                         GnalkPreferencesDialog *self)
{
  guint selected = adw_combo_row_get_selected (row);

  /* Save preference */
  if (selected > 0)
    {
      DeviceInfo *info = g_list_model_get_item (G_LIST_MODEL (self->audio_devices_store), selected);
      if (info)
        {
          g_settings_set_string (self->settings, "preferred-audio-device", info->device_id);
          g_object_unref (info);
        }
    }
  else
    {
      g_settings_set_string (self->settings, "preferred-audio-device", "");
    }

  start_audio_monitoring (self);
}

static void
on_video_quality_changed (AdwComboRow             *row,
                          GParamSpec              *pspec,
                          GnalkPreferencesDialog *self)
{
  guint selected = adw_combo_row_get_selected (row);

  /* Save preference - quality value maps directly to selected index */
  g_settings_set_int (self->settings, "camera-quality", (gint) selected);
  g_message ("Preferences: camera quality changed to %d", selected);
}

static void
on_close_clicked (GtkButton               *button,
                  GnalkPreferencesDialog *self)
{
  adw_dialog_close (ADW_DIALOG (self));
}

static void
gnalk_preferences_dialog_dispose (GObject *object)
{
  GnalkPreferencesDialog *self = GNALK_PREFERENCES_DIALOG (object);

  stop_video_preview (self);
  stop_audio_monitoring (self);

  G_OBJECT_CLASS (gnalk_preferences_dialog_parent_class)->dispose (object);
}

static void
gnalk_preferences_dialog_finalize (GObject *object)
{
  GnalkPreferencesDialog *self = GNALK_PREFERENCES_DIALOG (object);

  g_clear_object (&self->settings);
  g_clear_object (&self->color_scheme_names);
  g_clear_object (&self->background_style_names);
  g_clear_object (&self->language_names);
  g_clear_object (&self->video_devices_store);
  g_clear_object (&self->video_device_names);
  g_clear_object (&self->video_quality_names);
  g_clear_object (&self->audio_devices_store);
  g_clear_object (&self->audio_device_names);

  G_OBJECT_CLASS (gnalk_preferences_dialog_parent_class)->finalize (object);
}

static void
gnalk_preferences_dialog_class_init (GnalkPreferencesDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnalk_preferences_dialog_dispose;
  object_class->finalize = gnalk_preferences_dialog_finalize;

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-preferences-dialog.ui");

  gtk_widget_class_bind_template_child (widget_class, GnalkPreferencesDialog, close_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkPreferencesDialog, color_scheme_row);
  gtk_widget_class_bind_template_child (widget_class, GnalkPreferencesDialog, background_style_row);
  gtk_widget_class_bind_template_child (widget_class, GnalkPreferencesDialog, language_row);
  gtk_widget_class_bind_template_child (widget_class, GnalkPreferencesDialog, video_source_row);
  gtk_widget_class_bind_template_child (widget_class, GnalkPreferencesDialog, video_quality_row);
  gtk_widget_class_bind_template_child (widget_class, GnalkPreferencesDialog, video_preview_overlay);
  gtk_widget_class_bind_template_child (widget_class, GnalkPreferencesDialog, video_placeholder_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkPreferencesDialog, audio_source_row);
  gtk_widget_class_bind_template_child (widget_class, GnalkPreferencesDialog, audio_level_bar);
}

static void
gnalk_preferences_dialog_init (GnalkPreferencesDialog *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->settings = g_settings_new ("net.skilpa.gnalk");

  /* Set up color scheme combo row */
  const char *color_scheme_labels[] = { "Auto", "Day", "Night", NULL };
  self->color_scheme_names = gtk_string_list_new (color_scheme_labels);
  adw_combo_row_set_model (ADW_COMBO_ROW (self->color_scheme_row),
                           G_LIST_MODEL (self->color_scheme_names));

  /* Set initial color scheme selection from settings */
  gint current_scheme = g_settings_get_int (self->settings, "color-scheme");
  if (current_scheme >= 0 && current_scheme <= 2)
    adw_combo_row_set_selected (ADW_COMBO_ROW (self->color_scheme_row), (guint) current_scheme);

  g_signal_connect (self->color_scheme_row, "notify::selected",
                    G_CALLBACK (on_color_scheme_changed), self);

  /* Set up background style combo row */
  const char *background_style_labels[] = {
    "Flat", "Grid", "Hexagons", "Isometric", "Rhombus", "ZigZag",
    "Houndstooth", "Herringbone", "Stars", "Triangles", "Waves", "Scales", "Cross",
    "Mosaic", NULL
  };
  self->background_style_names = gtk_string_list_new (background_style_labels);
  adw_combo_row_set_model (ADW_COMBO_ROW (self->background_style_row),
                           G_LIST_MODEL (self->background_style_names));

  /* Set initial background style selection from settings */
  gint current_bg = g_settings_get_int (self->settings, "conversation-background");
  if (current_bg >= 0 && current_bg <= 13)
    adw_combo_row_set_selected (ADW_COMBO_ROW (self->background_style_row), (guint) current_bg);

  g_signal_connect (self->background_style_row, "notify::selected",
                    G_CALLBACK (on_background_style_changed), self);

  /* Set up language combo row */
  const char *language_labels[] = {
    C_("language", "System default"), "English", "Français", "Nederlands",
    "Español", "Italiano", "Deutsch", NULL
  };
  self->language_names = gtk_string_list_new (language_labels);
  adw_combo_row_set_model (ADW_COMBO_ROW (self->language_row),
                           G_LIST_MODEL (self->language_names));

  /* Set initial language selection from settings */
  g_autofree char *current_lang = g_settings_get_string (self->settings, "preferred-language");
  guint lang_index = 0;
  if (current_lang && *current_lang)
    {
      for (int i = 0; i < n_languages; i++)
        {
          if (g_strcmp0 (current_lang, language_codes[i]) == 0)
            {
              lang_index = (guint) i;
              break;
            }
        }
    }
  adw_combo_row_set_selected (ADW_COMBO_ROW (self->language_row), lang_index);

  g_signal_connect (self->language_row, "notify::selected",
                    G_CALLBACK (on_language_changed), self);

  /* Create camera preview widget and add to overlay */
  self->camera_preview = gnalk_camera_preview_new ();
  gtk_widget_set_size_request (GTK_WIDGET (self->camera_preview), 320, 240);
  gtk_overlay_add_overlay (GTK_OVERLAY (self->video_preview_overlay),
                           GTK_WIDGET (self->camera_preview));
  gtk_widget_set_visible (GTK_WIDGET (self->camera_preview), FALSE);

  /* Create list stores for device info */
  self->video_devices_store = g_list_store_new (device_info_get_type ());
  self->audio_devices_store = g_list_store_new (device_info_get_type ());

  /* Create string lists for combo rows */
  self->video_device_names = gtk_string_list_new (NULL);
  self->audio_device_names = gtk_string_list_new (NULL);

  /* Create quality presets list */
  const char *quality_labels[] = { "Low (fast)", "Medium", "High (best FOV)", NULL };
  self->video_quality_names = gtk_string_list_new (quality_labels);

  adw_combo_row_set_model (ADW_COMBO_ROW (self->video_source_row),
                           G_LIST_MODEL (self->video_device_names));
  adw_combo_row_set_model (ADW_COMBO_ROW (self->video_quality_row),
                           G_LIST_MODEL (self->video_quality_names));
  adw_combo_row_set_model (ADW_COMBO_ROW (self->audio_source_row),
                           G_LIST_MODEL (self->audio_device_names));

  /* Set initial quality selection from settings */
  gint current_quality = g_settings_get_int (self->settings, "camera-quality");
  if (current_quality >= 0 && current_quality <= 2)
    adw_combo_row_set_selected (ADW_COMBO_ROW (self->video_quality_row), (guint) current_quality);

  /* Connect signals */
  g_signal_connect (self->close_button, "clicked", G_CALLBACK (on_close_clicked), self);
  g_signal_connect (self->video_source_row, "notify::selected",
                    G_CALLBACK (on_video_source_changed), self);
  g_signal_connect (self->video_quality_row, "notify::selected",
                    G_CALLBACK (on_video_quality_changed), self);
  g_signal_connect (self->audio_source_row, "notify::selected",
                    G_CALLBACK (on_audio_source_changed), self);

  /* Enumerate devices */
  enumerate_video_devices (self);
  enumerate_audio_devices (self);

  /* Start previews after a short delay to let the dialog appear */
  g_idle_add_once ((GSourceOnceFunc) start_video_preview, self);
  g_idle_add_once ((GSourceOnceFunc) start_audio_monitoring, self);
}

GnalkPreferencesDialog *
gnalk_preferences_dialog_new (void)
{
  return g_object_new (GNALK_TYPE_PREFERENCES_DIALOG, NULL);
}
