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
  GtkWidget *text_overlay;
  GtkWidget *scroll;
  GtkWidget *message_entry;
  GtkWidget *placeholder_label;
  GtkWidget *send_button;

  /* Cached text for get_text (returns const char *) */
  char *cached_text;

  /* Single-line height for auto-grow */
  int line_height;

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
  SIGNAL_PASTE_ATTACHMENTS,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

#define MAX_INPUT_HEIGHT 180  /* ~6 lines */

static GtkTextBuffer *
get_buffer (GnalkMessageInput *self)
{
  return gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->message_entry));
}

static void
update_scroll_height (GnalkMessageInput *self)
{
  GtkTextView *tv = GTK_TEXT_VIEW (self->message_entry);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (tv);
  GtkTextIter end;
  GdkRectangle rect;

  gtk_text_buffer_get_end_iter (buffer, &end);
  gtk_text_view_get_iter_location (tv, &end, &rect);

  int content = rect.y + rect.height
              + gtk_text_view_get_top_margin (tv)
              + gtk_text_view_get_bottom_margin (tv);
  int capped = CLAMP (content, self->line_height, MAX_INPUT_HEIGHT);

  gtk_widget_set_size_request (self->scroll, -1, capped);

  /* Show scrollbar only when content exceeds max height */
  GtkPolicyType policy = (content > MAX_INPUT_HEIGHT)
                        ? GTK_POLICY_AUTOMATIC
                        : GTK_POLICY_EXTERNAL;
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scroll),
                                  GTK_POLICY_NEVER, policy);
}

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
  gboolean has_text = gtk_text_buffer_get_char_count (get_buffer (self)) > 0;

  /* Hide media button when text is entered (only show send button) */
  gtk_widget_set_visible (self->media_button, !has_text);
}

static void
show_recording_overlay (GnalkMessageInput *self)
{
  /* Hide normal input widgets */
  gtk_widget_set_visible (self->media_button, FALSE);
  gtk_widget_set_visible (self->emoji_button, FALSE);
  gtk_widget_set_visible (self->text_overlay, FALSE);
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
  gtk_widget_set_visible (self->text_overlay, TRUE);
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
  GtkTextBuffer *buffer = get_buffer (self);
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds (buffer, &start, &end);
  g_autofree char *text = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
  g_strchomp (text);

  g_signal_emit (self, signals[SIGNAL_SEND_REQUESTED], 0, text);
}

static gboolean
on_key_pressed (GtkEventControllerKey *controller G_GNUC_UNUSED,
                guint                  keyval,
                guint                  keycode G_GNUC_UNUSED,
                GdkModifierType        state,
                gpointer               user_data)
{
  /* Ctrl+V: intercept paste when clipboard has files or textures */
  if (keyval == GDK_KEY_v && (state & GDK_CONTROL_MASK))
    {
      GnalkMessageInput *self = GNALK_MESSAGE_INPUT (user_data);
      GdkClipboard *clipboard = gtk_widget_get_clipboard (GTK_WIDGET (self));
      GdkContentFormats *formats = gdk_clipboard_get_formats (clipboard);

      if (gdk_content_formats_contain_gtype (formats, GDK_TYPE_FILE_LIST) ||
          gdk_content_formats_contain_gtype (formats, GDK_TYPE_TEXTURE))
        {
          g_signal_emit (self, signals[SIGNAL_PASTE_ATTACHMENTS], 0, clipboard);
          return TRUE;
        }
      /* Fall through → normal text paste */
    }

  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
    {
      if (state & GDK_SHIFT_MASK)
        return FALSE;  /* Shift+Enter → let GTK insert the newline */
      on_send_clicked (NULL, GNALK_MESSAGE_INPUT (user_data));
      return TRUE;     /* Enter → send, consume the event */
    }
  return FALSE;
}

static void
on_buffer_changed (GtkTextBuffer      *buffer,
                   GnalkMessageInput *self)
{
  gboolean has_text = gtk_text_buffer_get_char_count (buffer) > 0;

  /* Toggle placeholder visibility */
  gtk_widget_set_visible (self->placeholder_label, !has_text);

  /* Invalidate cached text */
  g_clear_pointer (&self->cached_text, g_free);

  /* Emit text-changed with current text */
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  g_autofree char *text = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
  g_signal_emit (self, signals[SIGNAL_TEXT_CHANGED], 0, text);

  update_button_visibility (self);
  update_scroll_height (self);
}

static void
on_emoji_picked (GtkEmojiChooser    *chooser G_GNUC_UNUSED,
                 const char         *emoji,
                 GnalkMessageInput *self)
{
  gtk_text_buffer_insert_at_cursor (get_buffer (self), emoji, -1);
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

  g_clear_pointer (&self->cached_text, g_free);
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

  signals[SIGNAL_PASTE_ATTACHMENTS] =
    g_signal_new ("paste-attachments",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, GDK_TYPE_CLIPBOARD);

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-message-input.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageInput, media_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageInput, emoji_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageInput, emoji_chooser);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageInput, text_overlay);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageInput, scroll);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageInput, message_entry);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageInput, placeholder_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageInput, send_button);
}

static void
gnalk_message_input_init (GnalkMessageInput *self)
{
  GSimpleActionGroup *action_group;
  GSimpleAction *action;
  GtkEventController *key_ctrl;
  GtkTextBuffer *buffer;

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

  /* Connect send button */
  g_signal_connect (self->send_button, "clicked", G_CALLBACK (on_send_clicked), self);

  /* Key controller for Enter/Shift+Enter on text view */
  key_ctrl = gtk_event_controller_key_new ();
  g_signal_connect (key_ctrl, "key-pressed", G_CALLBACK (on_key_pressed), self);
  gtk_widget_add_controller (self->message_entry, key_ctrl);

  /* Buffer changed for placeholder, text-changed signal, and auto-grow */
  buffer = get_buffer (self);
  g_signal_connect (buffer, "changed", G_CALLBACK (on_buffer_changed), self);

  /* Compute single-line height from font metrics */
  {
    PangoContext *pctx = gtk_widget_get_pango_context (self->message_entry);
    PangoFontMetrics *fm = pango_context_get_metrics (pctx, NULL, NULL);
    int font_h = (pango_font_metrics_get_ascent (fm)
                + pango_font_metrics_get_descent (fm)) / PANGO_SCALE;
    pango_font_metrics_unref (fm);
    int top = gtk_text_view_get_top_margin (GTK_TEXT_VIEW (self->message_entry));
    int bot = gtk_text_view_get_bottom_margin (GTK_TEXT_VIEW (self->message_entry));
    self->line_height = font_h + top + bot;
    gtk_widget_set_size_request (self->scroll, -1, self->line_height);
  }

  /* Placeholder label should not intercept clicks */
  gtk_widget_set_can_target (self->placeholder_label, FALSE);

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

  GtkTextBuffer *buffer = get_buffer (self);
  gtk_text_buffer_set_text (buffer, text ? text : "", -1);

  /* Place cursor at the end */
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (buffer, &end);
  gtk_text_buffer_place_cursor (buffer, &end);
}

const char *
gnalk_message_input_get_text (GnalkMessageInput *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_INPUT (self), NULL);

  if (!self->cached_text)
    {
      GtkTextBuffer *buffer = get_buffer (self);
      GtkTextIter start, end;
      gtk_text_buffer_get_bounds (buffer, &start, &end);
      self->cached_text = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
    }

  return self->cached_text;
}

void
gnalk_message_input_clear (GnalkMessageInput *self)
{
  g_return_if_fail (GNALK_IS_MESSAGE_INPUT (self));

  gtk_text_buffer_set_text (get_buffer (self), "", -1);
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
