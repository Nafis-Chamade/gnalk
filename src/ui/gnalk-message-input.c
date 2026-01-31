#include "gnalk-message-input.h"
#include "gnalk-recording-overlay.h"
#include <adwaita.h>

/* Media action types */
enum {
  MEDIA_ACTION_ATTACH = 0,
  MEDIA_ACTION_VOICE = 1,
  MEDIA_ACTION_CAMERA = 2,
};

/* Icon and tooltip for each media action */
static const char *media_icons[] = {
  "mail-attachment-symbolic",
  "audio-input-microphone-symbolic",
  "camera-photo-symbolic",
};

static const char *media_tooltips[] = {
  "Attach File",
  "Record Voice Message",
  "Take Photo or Video",
};

struct _GnalkMessageInput
{
  GtkBox parent_instance;

  /* Standard input widgets */
  GtkWidget *media_button;
  GtkWidget *emoji_button;
  GtkWidget *emoji_chooser;
  GtkWidget *message_entry;
  GtkWidget *send_button;

  /* Settings for default media action */
  GSettings *settings;
  int default_media_action;

  /* Recording overlay (replaces normal input while recording) */
  GnalkRecordingOverlay *recording_overlay;
  gboolean recording;
};

G_DEFINE_FINAL_TYPE (GnalkMessageInput, gnalk_message_input, GTK_TYPE_BOX)

enum {
  SIGNAL_SEND_REQUESTED,
  SIGNAL_TEXT_CHANGED,
  SIGNAL_ATTACH_CLICKED,
  SIGNAL_VOICE_MESSAGE_RECORDED,
  SIGNAL_CAMERA_CLICKED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
update_media_button_appearance (GnalkMessageInput *self)
{
  int action = self->default_media_action;
  if (action < 0 || action > 2)
    action = MEDIA_ACTION_ATTACH;

  adw_split_button_set_icon_name (ADW_SPLIT_BUTTON (self->media_button),
                                   media_icons[action]);
  gtk_widget_set_tooltip_text (self->media_button, media_tooltips[action]);
}

static void
update_button_visibility (GnalkMessageInput *self)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->message_entry));
  gboolean has_text = text && *text;

  /* Hide media button when text is entered (only show send button) */
  gtk_widget_set_visible (self->media_button, !has_text);
}

static void
show_recording_overlay (GnalkMessageInput *self)
{
  /* Hide normal input widgets */
  gtk_widget_set_visible (self->media_button, FALSE);
  gtk_widget_set_visible (self->emoji_button, FALSE);
  gtk_widget_set_visible (self->message_entry, FALSE);
  gtk_widget_set_visible (self->send_button, FALSE);

  /* Show recording overlay */
  gtk_widget_set_visible (GTK_WIDGET (self->recording_overlay), TRUE);
  self->recording = TRUE;
}

static void
hide_recording_overlay (GnalkMessageInput *self)
{
  /* Hide recording overlay */
  gtk_widget_set_visible (GTK_WIDGET (self->recording_overlay), FALSE);
  self->recording = FALSE;

  /* Show normal input widgets */
  gtk_widget_set_visible (self->emoji_button, TRUE);
  gtk_widget_set_visible (self->message_entry, TRUE);
  gtk_widget_set_visible (self->send_button, TRUE);

  /* Update media button visibility based on text */
  update_button_visibility (self);
}

static void
trigger_media_action (GnalkMessageInput *self,
                      int                 action)
{
  switch (action)
    {
    case MEDIA_ACTION_ATTACH:
      g_signal_emit (self, signals[SIGNAL_ATTACH_CLICKED], 0);
      break;

    case MEDIA_ACTION_VOICE:
      g_message ("MessageInput: Voice button clicked, starting recording");
      show_recording_overlay (self);
      if (!gnalk_recording_overlay_start (self->recording_overlay))
        {
          g_warning ("MessageInput: Failed to start recording");
          hide_recording_overlay (self);
        }
      break;

    case MEDIA_ACTION_CAMERA:
      g_signal_emit (self, signals[SIGNAL_CAMERA_CLICKED], 0);
      break;

    default:
      g_warning ("MessageInput: Unknown media action %d", action);
      break;
    }
}

static void
on_send_clicked (GtkButton          *button G_GNUC_UNUSED,
                 GnalkMessageInput *self)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->message_entry));
  g_signal_emit (self, signals[SIGNAL_SEND_REQUESTED], 0, text);
}

static void
on_entry_activated (GtkEntry           *entry G_GNUC_UNUSED,
                    GnalkMessageInput *self)
{
  on_send_clicked (GTK_BUTTON (self->send_button), self);
}

static void
on_entry_changed (GtkEditable        *editable G_GNUC_UNUSED,
                  GnalkMessageInput *self)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->message_entry));
  g_signal_emit (self, signals[SIGNAL_TEXT_CHANGED], 0, text);

  update_button_visibility (self);
}

static void
on_emoji_picked (GtkEmojiChooser    *chooser G_GNUC_UNUSED,
                 const char         *emoji,
                 GnalkMessageInput *self)
{
  int cursor_pos = gtk_editable_get_position (GTK_EDITABLE (self->message_entry));
  gtk_editable_insert_text (GTK_EDITABLE (self->message_entry), emoji, -1, &cursor_pos);
  gtk_editable_set_position (GTK_EDITABLE (self->message_entry), cursor_pos);
  gtk_widget_grab_focus (self->message_entry);
}

static void
on_media_button_clicked (AdwSplitButton     *button G_GNUC_UNUSED,
                         GnalkMessageInput *self)
{
  /* Trigger the default media action */
  trigger_media_action (self, self->default_media_action);
}

static void
on_media_action_activated (GSimpleAction *action G_GNUC_UNUSED,
                           GVariant      *parameter,
                           gpointer       user_data)
{
  GnalkMessageInput *self = GNALK_MESSAGE_INPUT (user_data);
  int action_type = g_variant_get_int32 (parameter);

  /* Update default action and save to settings */
  self->default_media_action = action_type;
  g_settings_set_int (self->settings, "default-media-action", action_type);
  update_media_button_appearance (self);

  /* Trigger the action */
  trigger_media_action (self, action_type);
}

static void
on_recording_cancelled (GnalkRecordingOverlay *overlay G_GNUC_UNUSED,
                        GnalkMessageInput     *self)
{
  g_message ("MessageInput: Recording cancelled");
  hide_recording_overlay (self);
}

static void
on_recording_send_requested (GnalkRecordingOverlay *overlay G_GNUC_UNUSED,
                             GBytes                 *audio_data,
                             gdouble                 duration,
                             GnalkMessageInput     *self)
{
  g_message ("MessageInput: Voice message recorded, duration=%.1f", duration);
  hide_recording_overlay (self);

  /* Emit signal for the voice message */
  g_signal_emit (self, signals[SIGNAL_VOICE_MESSAGE_RECORDED], 0, audio_data, duration);
}

static void
gnalk_message_input_dispose (GObject *object)
{
  GnalkMessageInput *self = GNALK_MESSAGE_INPUT (object);

  g_clear_object (&self->settings);
  g_clear_pointer ((GtkWidget **) &self->recording_overlay, gtk_widget_unparent);

  G_OBJECT_CLASS (gnalk_message_input_parent_class)->dispose (object);
}

static void
gnalk_message_input_class_init (GnalkMessageInputClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gnalk_message_input_dispose;

  signals[SIGNAL_SEND_REQUESTED] =
    g_signal_new ("send-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_TEXT_CHANGED] =
    g_signal_new ("text-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_ATTACH_CLICKED] =
    g_signal_new ("attach-clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_VOICE_MESSAGE_RECORDED] =
    g_signal_new ("voice-message-recorded",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, G_TYPE_BYTES, G_TYPE_DOUBLE);

  signals[SIGNAL_CAMERA_CLICKED] =
    g_signal_new ("camera-clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-message-input.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageInput, media_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageInput, emoji_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageInput, emoji_chooser);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageInput, message_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageInput, send_button);
}

static void
gnalk_message_input_init (GnalkMessageInput *self)
{
  GSimpleActionGroup *action_group;
  GSimpleAction *action;

  gtk_widget_init_template (GTK_WIDGET (self));

  /* Load settings */
  self->settings = g_settings_new ("net.skilpa.gnalk");
  self->default_media_action = g_settings_get_int (self->settings, "default-media-action");

  /* Clamp to valid range */
  if (self->default_media_action < 0 || self->default_media_action > 2)
    self->default_media_action = MEDIA_ACTION_ATTACH;

  /* Set up action for menu items */
  action_group = g_simple_action_group_new ();
  action = g_simple_action_new ("media-action", G_VARIANT_TYPE_INT32);
  g_signal_connect (action, "activate", G_CALLBACK (on_media_action_activated), self);
  g_action_map_add_action (G_ACTION_MAP (action_group), G_ACTION (action));
  g_object_unref (action);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "msg-input", G_ACTION_GROUP (action_group));
  g_object_unref (action_group);

  /* Update button appearance based on saved default */
  update_media_button_appearance (self);

  /* Connect signals */
  g_signal_connect (self->send_button, "clicked", G_CALLBACK (on_send_clicked), self);
  g_signal_connect (self->message_entry, "activate", G_CALLBACK (on_entry_activated), self);
  g_signal_connect (self->message_entry, "changed", G_CALLBACK (on_entry_changed), self);
  g_signal_connect (self->emoji_chooser, "emoji-picked", G_CALLBACK (on_emoji_picked), self);
  g_signal_connect (self->media_button, "clicked", G_CALLBACK (on_media_button_clicked), self);

  /* Create recording overlay (hidden by default) */
  self->recording_overlay = gnalk_recording_overlay_new ();
  gtk_widget_set_visible (GTK_WIDGET (self->recording_overlay), FALSE);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->recording_overlay));

  g_signal_connect (self->recording_overlay, "cancelled",
                    G_CALLBACK (on_recording_cancelled), self);
  g_signal_connect (self->recording_overlay, "send-requested",
                    G_CALLBACK (on_recording_send_requested), self);
}

GnalkMessageInput *
gnalk_message_input_new (void)
{
  return g_object_new (GNALK_TYPE_MESSAGE_INPUT, NULL);
}

void
gnalk_message_input_set_text (GnalkMessageInput *self,
                                const char         *text)
{
  g_return_if_fail (GNALK_IS_MESSAGE_INPUT (self));

  gtk_editable_set_text (GTK_EDITABLE (self->message_entry), text ? text : "");
}

const char *
gnalk_message_input_get_text (GnalkMessageInput *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_INPUT (self), NULL);

  return gtk_editable_get_text (GTK_EDITABLE (self->message_entry));
}

void
gnalk_message_input_clear (GnalkMessageInput *self)
{
  g_return_if_fail (GNALK_IS_MESSAGE_INPUT (self));

  gtk_editable_set_text (GTK_EDITABLE (self->message_entry), "");
}

void
gnalk_message_input_grab_focus (GnalkMessageInput *self)
{
  g_return_if_fail (GNALK_IS_MESSAGE_INPUT (self));

  gtk_widget_grab_focus (self->message_entry);
}

void
gnalk_message_input_set_sensitive (GnalkMessageInput *self,
                                     gboolean            sensitive)
{
  g_return_if_fail (GNALK_IS_MESSAGE_INPUT (self));

  gtk_widget_set_sensitive (self->message_entry, sensitive);
  gtk_widget_set_sensitive (self->send_button, sensitive);
  gtk_widget_set_sensitive (self->emoji_button, sensitive);
  gtk_widget_set_sensitive (self->media_button, sensitive);
}

void
gnalk_message_input_set_attach_visible (GnalkMessageInput *self,
                                          gboolean            visible)
{
  g_return_if_fail (GNALK_IS_MESSAGE_INPUT (self));

  /* This now controls the media button visibility */
  gtk_widget_set_visible (self->media_button, visible);
}

void
gnalk_message_input_set_media_buttons_visible (GnalkMessageInput *self,
                                                 gboolean            visible)
{
  g_return_if_fail (GNALK_IS_MESSAGE_INPUT (self));

  /* Now there's only one media button */
  gtk_widget_set_visible (self->media_button, visible);
}

gboolean
gnalk_message_input_is_recording (GnalkMessageInput *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_INPUT (self), FALSE);

  return self->recording;
}
