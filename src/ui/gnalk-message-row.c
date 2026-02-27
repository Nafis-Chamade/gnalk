#include "gnalk-message-row.h"
#include "gnalk-message-data.h"
#include "gnalk-markdown.h"
#include "gnalk-message-utils.h"
#include "gnalk-audio-player.h"
#include "gnalk-video-thumbnail.h"
#include "gnalk-date-separator.h"
#include "gnalk-quote-widget.h"
#include "gnalk-upload-preview.h"
#include "gnalk-file-preview.h"
#include "gnalk-system-message.h"
#include <adwaita.h>
#include <glib/gi18n.h>
#include <time.h>

struct _GnalkMessageRow
{
  GtkBox parent_instance;

  GtkWidget *avatar;
  GtkWidget *avatar_box;
  GtkWidget *avatar_frame;
  GtkWidget *message_box;
  GtkWidget *username_label;
  GtkWidget *message_label;
  GtkWidget *timestamp_label;
  GtkWidget *reactions_label;
  GtkWidget *edited_label;
  GtkWidget *message_bubble;
  GtkWidget *content_box;
  GtkWidget *status_icon;

  /* Date separator widget */
  GnalkDateSeparator *date_separator;

  /* Quote/reply widget */
  GnalkQuoteWidget *quote_widget;

  /* Upload preview widget */
  GnalkUploadPreview *upload_preview;

  /* File preview widget */
  GnalkFilePreview *file_preview;

  /* Grouped file previews */
  GtkWidget        *file_group_flow;                 /* GtkFlowBox from template */
  GnalkFilePreview *group_file_pool[12];             /* Pre-created compact previews */

  /* Audio player */
  GtkWidget *audio_player;

  /* Video thumbnail */
  GtkWidget *video_thumbnail;

  /* Footer (timestamp, status, reactions) */
  GtkWidget *footer_box;

  /* Context menu */
  GtkWidget *reaction_popover;
  GtkWidget *edit_button;
  GtkWidget *delete_button;

  gint64 message_id;
  gint64 message_timestamp;
  gint64 parent_message_id;  /* ID of quoted/parent message for scroll-to */
  gint64 edit_timeout;       /* Max age in seconds for editing (0 = disabled) */
  gint64 delete_timeout;     /* Max age in seconds for deleting (0 = disabled, default 6h) */
  char  *author_name;
  char  *author_id;
  char  *message;
  char  *timestamp;
  char  *reference_id;
  GnalkMessageStatus status;
  gboolean is_self;
  gboolean is_system;
  gboolean is_bot;
  gboolean is_one_to_one;  /* 1-to-1 conversation (no username shown) */
  gboolean show_avatar;    /* For message grouping (first of consecutive from same author) */
  GnalkMessageDisplayMode display_mode;

  /* Dynamic bubble color from avatar */
  GtkCssProvider *bubble_css_provider;
  GdkRGBA         bubble_color;
  gboolean        has_bubble_color;
  gulong          dark_mode_handler_id;

  /* Bound data for signal handling */
  GnalkMessageData *bound_data;
  gulong             unread_handler_id;

  /* Thread support */
  GtkWidget *thread_box;
  GtkWidget *thread_title_label;
  GtkWidget *thread_description_label;
  GtkWidget *thread_button;
  GtkWidget *thread_label;
  GtkWidget *start_thread_button;  /* In context menu */
  gboolean   threads_supported;    /* Set from session capability */
};

G_DEFINE_FINAL_TYPE (GnalkMessageRow, gnalk_message_row, GTK_TYPE_BOX)

enum {
  PROP_0,
  PROP_AUTHOR_NAME,
  PROP_MESSAGE,
  PROP_TIMESTAMP,
  N_PROPS
};

enum {
  SIGNAL_REACTION_REQUESTED,
  SIGNAL_QUOTE_REQUESTED,
  SIGNAL_SCROLL_TO_MESSAGE_REQUESTED,
  SIGNAL_FILE_CLICKED,
  SIGNAL_EDIT_REQUESTED,
  SIGNAL_DELETE_REQUESTED,
  SIGNAL_THREAD_OPEN_REQUESTED,
  SIGNAL_THREAD_CREATE_REQUESTED,
  N_SIGNALS
};

static GParamSpec *properties[N_PROPS];
static guint signals[N_SIGNALS];

/* Forward declarations */
static void update_status_icon (GnalkMessageRow *self);
static char *format_timestamp (gint64 timestamp);
static gboolean is_single_emoji (const char *text);
static void on_avatar_loaded (GnalkAvatarCache *cache, GdkTexture *texture, gpointer user_data);
static void apply_layout (GnalkMessageRow *self);

/* Callback for when any bound data property changes (notify-based refresh) */
static void
on_bound_data_notify (GObject    *data_obj,
                      GParamSpec *pspec,
                      gpointer    user_data)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (user_data);
  GnalkMessageData *data = GNALK_MESSAGE_DATA (data_obj);
  const char *name = pspec->name;

  if (g_strcmp0 (name, "is-unread") == 0)
    {
      gnalk_message_row_set_unread (self, gnalk_message_data_get_is_unread (data));
    }
  else if (g_strcmp0 (name, "message") == 0)
    {
      const char *message_text = gnalk_message_data_get_message (data);
      JsonObject *params = gnalk_message_data_get_message_parameters (data);
      g_autofree char *display_message = gnalk_message_utils_replace_parameters (
          message_text, params, self->is_system ? self->author_name : NULL);

      g_free (self->message);
      self->message = g_strdup (display_message);

      gboolean render_md = gnalk_message_data_get_render_markdown (data);
      if (render_md && display_message && *display_message)
        {
          g_autofree char *markup = gnalk_markdown_to_pango (display_message, params);
          gtk_label_set_markup (GTK_LABEL (self->message_label), markup);
        }
      else
        {
          g_autofree char *escaped = gnalk_markdown_escape_pango (display_message ? display_message : "");
          gtk_label_set_markup (GTK_LABEL (self->message_label), escaped);
        }

      gtk_widget_remove_css_class (self->message_label, "message-emoji-only");
      if (!self->is_system && is_single_emoji (display_message))
        gtk_widget_add_css_class (self->message_label, "message-emoji-only");

      /* When server response arrives with file parameters, update file_preview
       * for click handling (gallery vs external viewer) and hide bubble if media-only */
      if (params && json_object_has_member (params, "file"))
        {
          JsonNode *file_node = json_object_get_member (params, "file");
          if (file_node && JSON_NODE_HOLDS_OBJECT (file_node))
            {
              JsonObject *file = json_node_get_object (file_node);
              const char *file_id = json_object_get_string_member_with_default (file, "id", NULL);

              if (file_id && *file_id && !gnalk_file_preview_get_file_id (self->file_preview))
                {
                  const char *file_name = json_object_get_string_member_with_default (file, "name", "");
                  const char *mimetype = json_object_get_string_member_with_default (file, "mimetype", "");
                  const char *file_link = json_object_get_string_member_with_default (file, "link", "");
                  const char *file_path = json_object_get_string_member_with_default (file, "path", "");

                  /* Store file info (NULL avatar_cache = no preview load, upload_preview still shows image) */
                  gnalk_file_preview_set_file (self->file_preview, file_id, file_name, mimetype,
                                                file_link, file_path, NULL);
                }

              /* Hide message label for media-only file messages (bubble stays visible for file inside it) */
              if (g_strcmp0 (message_text, "{file}") == 0)
                {
                  gtk_widget_set_visible (self->message_label, FALSE);
                  gtk_widget_set_visible (self->message_bubble, TRUE);
                }
            }
        }
    }
  else if (g_strcmp0 (name, "status") == 0)
    {
      GnalkMessageDataStatus data_status = gnalk_message_data_get_status (data);
      switch (data_status)
        {
        case GNALK_MESSAGE_DATA_STATUS_SENDING:
          self->status = GNALK_MESSAGE_STATUS_SENDING;
          break;
        case GNALK_MESSAGE_DATA_STATUS_SENT:
          self->status = GNALK_MESSAGE_STATUS_SENT;
          break;
        case GNALK_MESSAGE_DATA_STATUS_READ:
          self->status = GNALK_MESSAGE_STATUS_READ;
          break;
        case GNALK_MESSAGE_DATA_STATUS_FAILED:
          self->status = GNALK_MESSAGE_STATUS_FAILED;
          break;
        default:
          self->status = GNALK_MESSAGE_STATUS_NONE;
          break;
        }
      if (self->is_self && !self->is_system)
        update_status_icon (self);
    }
  else if (g_strcmp0 (name, "reactions") == 0)
    {
      JsonObject *reactions = gnalk_message_data_get_reactions (data);
      if (reactions)
        gnalk_message_row_set_reactions (self, reactions);
      else
        gtk_widget_set_visible (self->reactions_label, FALSE);
    }
  else if (g_strcmp0 (name, "last-edit-ts") == 0)
    {
      gint64 last_edit = gnalk_message_data_get_last_edit_timestamp (data);
      gtk_widget_set_visible (self->edited_label, last_edit > 0);
    }
  else if (g_strcmp0 (name, "is-uploading") == 0)
    {
      if (!gnalk_message_data_get_is_uploading (data))
        gnalk_upload_preview_set_complete (self->upload_preview);
    }
  else if (g_strcmp0 (name, "file-cached") == 0)
    {
      /* Skip cache overlay for inline images - no need for green icon on visible images */
      const char *mimetype = gnalk_file_preview_get_file_mimetype (self->file_preview);
      if (mimetype && g_str_has_prefix (mimetype, "image/"))
        return;

      gboolean cached = gnalk_message_data_get_file_cached (data);
      gnalk_file_preview_set_cached (self->file_preview, cached);
    }
  else if (g_strcmp0 (name, "thread-replies") == 0)
    {
      const char *sm = gnalk_message_data_get_system_message (data);
      gboolean is_tc = sm && g_strcmp0 (sm, "thread_created") == 0;
      gboolean is_entry = is_tc && gnalk_message_data_get_is_thread (data);
      if (is_entry)
        {
          gint replies = gnalk_message_data_get_thread_replies (data);
          g_autofree char *label = NULL;
          if (replies > 0)
            label = g_strdup_printf (ngettext ("View thread (%d reply)",
                                               "View thread (%d replies)", replies), replies);
          else
            label = g_strdup (_("View thread"));
          gtk_label_set_text (GTK_LABEL (self->thread_label), label);

          /* Show thread title if available */
          const char *thread_title = gnalk_message_data_get_thread_title (data);
          if (thread_title && *thread_title)
            {
              gtk_label_set_text (GTK_LABEL (self->thread_title_label), thread_title);
              gtk_widget_set_visible (self->thread_title_label, TRUE);
            }

          /* Show thread description (first message text) */
          const char *msg_text = gnalk_message_data_get_thread_first_message (data);
          if (msg_text && *msg_text)
            {
              gtk_label_set_text (GTK_LABEL (self->thread_description_label), msg_text);
              gtk_widget_set_visible (self->thread_description_label, TRUE);
            }

          gtk_widget_set_visible (self->thread_box, TRUE);
        }
    }
}

/* Helper to set all content widgets to a given alignment */
static void
set_content_alignment (GnalkMessageRow *self, GtkAlign align)
{
  gtk_widget_set_halign (self->content_box, align == GTK_ALIGN_END ? GTK_ALIGN_END : GTK_ALIGN_FILL);
  gtk_widget_set_halign (self->message_bubble, align);
  gtk_widget_set_halign (GTK_WIDGET (self->upload_preview), align);
  gtk_widget_set_halign (self->audio_player, align);
  gtk_widget_set_halign (self->footer_box, align);
  gtk_widget_set_halign (self->thread_box, align);
}

/* Apply layout based on message type (system/self/other) and display mode */
static void
apply_layout (GnalkMessageRow *self)
{
  /* Clear existing CSS classes */
  gtk_widget_remove_css_class (GTK_WIDGET (self), "message-self");
  gtk_widget_remove_css_class (GTK_WIDGET (self), "message-other");
  gtk_widget_remove_css_class (GTK_WIDGET (self), "message-system");
  gtk_widget_remove_css_class (GTK_WIDGET (self), "display-aligned");
  gtk_widget_remove_css_class (GTK_WIDGET (self), "display-bubbles");

  if (self->is_system)
    {
      /* System messages: centered, no avatar */
      gtk_widget_add_css_class (GTK_WIDGET (self), "message-system");
      gtk_widget_set_visible (self->avatar_box, FALSE);
      gtk_widget_set_visible (self->username_label, FALSE);
      gtk_widget_set_halign (GTK_WIDGET (self), GTK_ALIGN_CENTER);
      gtk_widget_set_halign (self->message_box, GTK_ALIGN_CENTER);
      gtk_widget_set_halign (self->message_bubble, GTK_ALIGN_CENTER);
      return;
    }

  gtk_widget_set_halign (GTK_WIDGET (self), GTK_ALIGN_FILL);
  gtk_widget_set_halign (self->message_box, GTK_ALIGN_FILL);

  if (self->display_mode == GNALK_MESSAGE_DISPLAY_ALIGNED)
    {
      /* Aligned mode: all messages left-aligned with avatars (desktop/wide view) */
      gtk_widget_add_css_class (GTK_WIDGET (self), "display-aligned");
      gtk_widget_add_css_class (GTK_WIDGET (self), self->is_self ? "message-self" : "message-other");
      /* Show avatar box for consistent spacing, but show/hide frame based on grouping */
      gtk_widget_set_visible (self->avatar_box, TRUE);
      gtk_widget_set_visible (self->avatar_frame, self->show_avatar);
      /* Fixed width for consistent alignment when avatar is hidden (message grouping) */
      gtk_widget_set_size_request (self->avatar_box, 32, -1);
      /* In group chats, show username for other participants on first message of group */
      gtk_widget_set_visible (self->username_label, !self->is_one_to_one && !self->is_self && self->show_avatar);
      gtk_widget_set_halign (self->message_box, GTK_ALIGN_START);
      set_content_alignment (self, GTK_ALIGN_START);
    }
  else
    {
      /* Bubble mode: own messages right, others left (mobile/narrow view) */
      gtk_widget_add_css_class (GTK_WIDGET (self), "display-bubbles");
      /* Reset avatar box size request for bubble mode */
      gtk_widget_set_size_request (self->avatar_box, -1, -1);
      if (self->is_self)
        {
          gtk_widget_add_css_class (GTK_WIDGET (self), "message-self");
          gtk_widget_set_visible (self->avatar_box, FALSE);
          gtk_widget_set_visible (self->username_label, FALSE);
          /* Align entire message box to right for own messages */
          gtk_widget_set_halign (self->message_box, GTK_ALIGN_END);
          set_content_alignment (self, GTK_ALIGN_END);
        }
      else
        {
          gtk_widget_add_css_class (GTK_WIDGET (self), "message-other");
          /* In bubble mode 1-to-1: no avatar; group chat: show avatar based on grouping */
          gtk_widget_set_visible (self->avatar_box, !self->is_one_to_one);
          gtk_widget_set_visible (self->avatar_frame, !self->is_one_to_one && self->show_avatar);
          /* Fixed width for avatar box in group chats for consistent alignment */
          if (!self->is_one_to_one)
            gtk_widget_set_size_request (self->avatar_box, 32, -1);
          /* In group chats, show username for first message of group */
          gtk_widget_set_visible (self->username_label, !self->is_one_to_one && self->show_avatar);
          gtk_widget_set_halign (self->message_box, GTK_ALIGN_START);
          set_content_alignment (self, GTK_ALIGN_START);
        }
    }
}

static void
gnalk_message_row_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (object);

  switch (prop_id)
    {
    case PROP_AUTHOR_NAME:
      g_value_set_string (value, self->author_name);
      break;
    case PROP_MESSAGE:
      g_value_set_string (value, self->message);
      break;
    case PROP_TIMESTAMP:
      g_value_set_string (value, self->timestamp);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gnalk_message_row_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (object);

  switch (prop_id)
    {
    case PROP_AUTHOR_NAME:
      g_free (self->author_name);
      self->author_name = g_value_dup_string (value);
      break;
    case PROP_MESSAGE:
      g_free (self->message);
      self->message = g_value_dup_string (value);
      break;
    case PROP_TIMESTAMP:
      g_free (self->timestamp);
      self->timestamp = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gnalk_message_row_dispose (GObject *object)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (object);

  /* Disconnect from style manager dark mode signal */
  if (self->dark_mode_handler_id)
    {
      AdwStyleManager *style_manager = adw_style_manager_get_default ();
      g_signal_handler_disconnect (style_manager, self->dark_mode_handler_id);
      self->dark_mode_handler_id = 0;
    }

  /* Disconnect from bound data signals */
  if (self->bound_data)
    {
      g_signal_handler_disconnect (self->bound_data, self->unread_handler_id);
      g_clear_object (&self->bound_data);
      self->unread_handler_id = 0;
    }

  g_clear_pointer (&self->reaction_popover, gtk_widget_unparent);

  G_OBJECT_CLASS (gnalk_message_row_parent_class)->dispose (object);
}

static void
gnalk_message_row_finalize (GObject *object)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (object);

  g_free (self->author_name);
  g_free (self->author_id);
  g_free (self->message);
  g_free (self->timestamp);
  g_free (self->reference_id);
  g_clear_object (&self->bubble_css_provider);

  G_OBJECT_CLASS (gnalk_message_row_parent_class)->finalize (object);
}

static void
gnalk_message_row_class_init (GnalkMessageRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = gnalk_message_row_get_property;
  object_class->set_property = gnalk_message_row_set_property;
  object_class->dispose = gnalk_message_row_dispose;
  object_class->finalize = gnalk_message_row_finalize;

  properties[PROP_AUTHOR_NAME] =
    g_param_spec_string ("author-name", NULL, NULL, "",
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_MESSAGE] =
    g_param_spec_string ("message", NULL, NULL, "",
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_TIMESTAMP] =
    g_param_spec_string ("timestamp", NULL, NULL, "",
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  /* Ensure custom widget types are registered before parsing the template */
  g_type_ensure (GNALK_TYPE_DATE_SEPARATOR);
  g_type_ensure (GNALK_TYPE_QUOTE_WIDGET);
  g_type_ensure (GNALK_TYPE_UPLOAD_PREVIEW);
  g_type_ensure (GNALK_TYPE_FILE_PREVIEW);
  g_type_ensure (GNALK_TYPE_AUDIO_PLAYER);
  g_type_ensure (GNALK_TYPE_VIDEO_THUMBNAIL);

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-message-row.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, avatar);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, avatar_box);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, avatar_frame);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, message_box);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, username_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, message_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, timestamp_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, reactions_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, edited_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, message_bubble);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, content_box);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, quote_widget);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, upload_preview);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, file_preview);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, file_group_flow);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, audio_player);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, video_thumbnail);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, date_separator);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, footer_box);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, status_icon);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, thread_box);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, thread_title_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, thread_description_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, thread_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkMessageRow, thread_label);

  /* Signal emitted when user requests to add a reaction */
  signals[SIGNAL_REACTION_REQUESTED] =
    g_signal_new ("reaction-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_INT64,    /* message_id */
                  G_TYPE_STRING);  /* reaction emoji */

  /* Signal emitted when user wants to quote/reply to a message */
  signals[SIGNAL_QUOTE_REQUESTED] =
    g_signal_new ("quote-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_INT64,    /* message_id */
                  G_TYPE_STRING,   /* author_name */
                  G_TYPE_STRING);  /* message_text */

  /* Signal emitted when user clicks on a quote to scroll to original message */
  signals[SIGNAL_SCROLL_TO_MESSAGE_REQUESTED] =
    g_signal_new ("scroll-to-message-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT64);   /* message_id */

  /* Signal emitted when user clicks on a file preview */
  signals[SIGNAL_FILE_CLICKED] =
    g_signal_new ("file-clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 5,
                  G_TYPE_STRING,   /* file_id */
                  G_TYPE_STRING,   /* file_mimetype */
                  G_TYPE_STRING,   /* file_link */
                  G_TYPE_STRING,   /* file_name */
                  G_TYPE_STRING);  /* file_path */

  /* Signal emitted when user wants to edit a message */
  signals[SIGNAL_EDIT_REQUESTED] =
    g_signal_new ("edit-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_INT64,    /* message_id */
                  G_TYPE_STRING);  /* current message text */

  /* Signal emitted when user wants to delete a message */
  signals[SIGNAL_DELETE_REQUESTED] =
    g_signal_new ("delete-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT64);   /* message_id */

  signals[SIGNAL_THREAD_OPEN_REQUESTED] =
    g_signal_new ("thread-open-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT64);   /* thread_id (= message_id of root) */

  signals[SIGNAL_THREAD_CREATE_REQUESTED] =
    g_signal_new ("thread-create-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT64);   /* message_id */
}

static void
on_reaction_button_clicked (GtkButton *button,
                            gpointer   user_data)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (user_data);
  const char *emoji = g_object_get_data (G_OBJECT (button), "emoji");

  if (emoji && self->message_id > 0)
    {
      g_signal_emit (self, signals[SIGNAL_REACTION_REQUESTED], 0, self->message_id, emoji);
      gtk_popover_popdown (GTK_POPOVER (self->reaction_popover));
    }
}

static void
on_quote_button_clicked (GtkButton *button,
                         gpointer   user_data)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (user_data);

  if (self->message_id > 0)
    {
      g_signal_emit (self, signals[SIGNAL_QUOTE_REQUESTED], 0,
                     self->message_id, self->author_name, self->message);
      gtk_popover_popdown (GTK_POPOVER (self->reaction_popover));
    }
}

static void
on_edit_button_clicked (GtkButton *button,
                        gpointer   user_data)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (user_data);

  if (self->message_id > 0 && self->is_self)
    {
      g_signal_emit (self, signals[SIGNAL_EDIT_REQUESTED], 0,
                     self->message_id, self->message);
      gtk_popover_popdown (GTK_POPOVER (self->reaction_popover));
    }
}

static void
on_delete_button_clicked (GtkButton *button,
                          gpointer   user_data)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (user_data);

  if (self->message_id > 0 && self->is_self)
    {
      g_signal_emit (self, signals[SIGNAL_DELETE_REQUESTED], 0,
                     self->message_id);
      gtk_popover_popdown (GTK_POPOVER (self->reaction_popover));
    }
}

static void
on_thread_button_clicked (GtkButton *button G_GNUC_UNUSED,
                          gpointer   user_data)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (user_data);
  if (!self->bound_data)
    return;

  /* Use thread_id (not message_id) — the system message "thread_created"
   * has is_thread=TRUE but its id != the actual thread root id. */
  gint64 tid = gnalk_message_data_get_thread_id (self->bound_data);
  if (tid > 0)
    g_signal_emit (self, signals[SIGNAL_THREAD_OPEN_REQUESTED], 0, tid);
}

static void
on_start_thread_clicked (GtkButton *button G_GNUC_UNUSED,
                         gpointer   user_data)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (user_data);
  if (self->message_id > 0)
    {
      g_signal_emit (self, signals[SIGNAL_THREAD_CREATE_REQUESTED], 0, self->message_id);
      gtk_popover_popdown (GTK_POPOVER (self->reaction_popover));
    }
}

static void
show_context_menu (GnalkMessageRow *self,
                   GtkGesture       *gesture,
                   double            x,
                   double            y)
{
  if (self->is_system)
    return;

  /* Claim the sequence to prevent the label's context menu from appearing */
  gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_CLAIMED);

  /* Calculate message age once */
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  gint64 message_age = (self->message_timestamp > 0) ? (now - self->message_timestamp) : 0;

  /* Show edit button only for own messages that are still editable */
  gboolean can_edit = self->is_self;
  if (can_edit && self->edit_timeout > 0 && self->message_timestamp > 0)
    can_edit = (message_age < self->edit_timeout);
  gtk_widget_set_visible (self->edit_button, can_edit);

  /* Show delete button only for own messages that are still deletable (6 hours by default) */
  gboolean can_delete = self->is_self;
  if (can_delete && self->delete_timeout > 0 && self->message_timestamp > 0)
    can_delete = (message_age < self->delete_timeout);
  gtk_widget_set_visible (self->delete_button, can_delete);

  /* Show "Start a thread" only when threads are supported, message is not already
   * a thread root, and message is not inside a thread */
  gboolean can_start_thread = self->threads_supported
      && self->bound_data
      && !gnalk_message_data_get_is_thread (self->bound_data)
      && gnalk_message_data_get_thread_id (self->bound_data) == 0;
  gtk_widget_set_visible (self->start_thread_button, can_start_thread);

  /* Position and show the popover */
  GdkRectangle rect = { (int) x, (int) y, 1, 1 };
  gtk_popover_set_pointing_to (GTK_POPOVER (self->reaction_popover), &rect);
  gtk_popover_popup (GTK_POPOVER (self->reaction_popover));
}

static void
on_right_click (GtkGestureClick *gesture,
                int              n_press,
                double           x,
                double           y,
                gpointer         user_data)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (user_data);
  show_context_menu (self, GTK_GESTURE (gesture), x, y);
}

static void
on_long_press (GtkGestureLongPress *gesture,
               double               x,
               double               y,
               gpointer             user_data)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (user_data);
  show_context_menu (self, GTK_GESTURE (gesture), x, y);
}

static void
on_quote_scroll_requested (GnalkQuoteWidget *quote_widget G_GNUC_UNUSED,
                           gint64             message_id,
                           gpointer           user_data)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (user_data);

  if (message_id > 0)
    g_signal_emit (self, signals[SIGNAL_SCROLL_TO_MESSAGE_REQUESTED], 0, message_id);
}

/* Called when file preview widget is clicked */
static void
on_file_preview_clicked (GnalkFilePreview *widget,
                         gpointer           user_data)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (user_data);

  const char *file_id = gnalk_file_preview_get_file_id (widget);
  if (file_id)
    {
      g_signal_emit (self, signals[SIGNAL_FILE_CLICKED], 0,
                     file_id,
                     gnalk_file_preview_get_file_mimetype (widget),
                     gnalk_file_preview_get_file_link (widget),
                     gnalk_file_preview_get_file_name (widget),
                     gnalk_file_preview_get_file_path (widget));
    }
}

/* Called when audio player's play button is clicked but no file is loaded */
static void
on_audio_play_requested (GnalkAudioPlayer *player G_GNUC_UNUSED,
                         gpointer           user_data)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (user_data);

  /* Emit file-clicked signal to trigger download */
  const char *file_id = gnalk_file_preview_get_file_id (self->file_preview);
  if (file_id)
    {
      g_signal_emit (self, signals[SIGNAL_FILE_CLICKED], 0,
                     file_id,
                     gnalk_file_preview_get_file_mimetype (self->file_preview),
                     gnalk_file_preview_get_file_link (self->file_preview),
                     gnalk_file_preview_get_file_name (self->file_preview),
                     gnalk_file_preview_get_file_path (self->file_preview));
    }
}

/* Called when video thumbnail is clicked */
static void
on_video_thumbnail_clicked (GnalkVideoThumbnail *thumbnail G_GNUC_UNUSED,
                            gpointer              user_data)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (user_data);

  /* Emit file-clicked signal to trigger download/playback */
  const char *file_id = gnalk_file_preview_get_file_id (self->file_preview);
  if (file_id)
    {
      g_signal_emit (self, signals[SIGNAL_FILE_CLICKED], 0,
                     file_id,
                     gnalk_file_preview_get_file_mimetype (self->file_preview),
                     gnalk_file_preview_get_file_link (self->file_preview),
                     gnalk_file_preview_get_file_name (self->file_preview),
                     gnalk_file_preview_get_file_path (self->file_preview));
    }
}

/* Called when a grouped file preview is clicked */
static void
on_group_file_clicked (GnalkFilePreview *widget,
                       gpointer           user_data)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (user_data);

  const char *file_id = gnalk_file_preview_get_file_id (widget);
  if (file_id)
    {
      g_signal_emit (self, signals[SIGNAL_FILE_CLICKED], 0,
                     file_id,
                     gnalk_file_preview_get_file_mimetype (widget),
                     gnalk_file_preview_get_file_link (widget),
                     gnalk_file_preview_get_file_name (widget),
                     gnalk_file_preview_get_file_path (widget));
    }
}

/* Called when upload preview widget is clicked */
static void
on_upload_preview_clicked (GnalkUploadPreview *widget,
                           gpointer             user_data)
{
  GnalkMessageRow *self = GNALK_MESSAGE_ROW (user_data);

  /* If we have server file info (upload completed), use the file-clicked signal
   * which will open the gallery for images */
  const char *file_id = gnalk_file_preview_get_file_id (self->file_preview);
  const char *file_mimetype = gnalk_file_preview_get_file_mimetype (self->file_preview);
  if (file_id && file_mimetype)
    {
      g_signal_emit (self, signals[SIGNAL_FILE_CLICKED], 0,
                     file_id, file_mimetype,
                     gnalk_file_preview_get_file_link (self->file_preview),
                     gnalk_file_preview_get_file_name (self->file_preview),
                     gnalk_file_preview_get_file_path (self->file_preview));
      return;
    }

  /* Fallback: open the local file directly */
  const char *upload_file_path = gnalk_upload_preview_get_file_path (widget);
  if (upload_file_path && *upload_file_path)
    {
      g_autofree char *uri = g_filename_to_uri (upload_file_path, NULL, NULL);
      if (uri)
        {
          g_autoptr(GError) error = NULL;
          if (!g_app_info_launch_default_for_uri (uri, NULL, &error))
            g_warning ("Failed to open uploaded file: %s", error->message);
        }
    }
}

static void
gnalk_message_row_init (GnalkMessageRow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  /* CSS provider for dynamic bubble color - will be initialized on first use */
  self->bubble_css_provider = NULL;

  /* Create context menu popover */
  self->reaction_popover = gtk_popover_new ();
  gtk_widget_set_parent (self->reaction_popover, GTK_WIDGET (self));

  GtkWidget *main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class (main_box, "context-menu");

  /* Reaction emojis row */
  GtkWidget *reaction_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class (reaction_box, "reaction-picker");

  const char *emojis[] = { "👍", "👎", "😄", "🎉", "😕", "❤️", "🚀", "👀", NULL };
  for (int i = 0; emojis[i] != NULL; i++)
    {
      GtkWidget *btn = gtk_button_new_with_label (emojis[i]);
      gtk_button_set_has_frame (GTK_BUTTON (btn), FALSE);
      gtk_widget_add_css_class (btn, "reaction-button");
      g_object_set_data_full (G_OBJECT (btn), "emoji", g_strdup (emojis[i]), g_free);
      g_signal_connect (btn, "clicked", G_CALLBACK (on_reaction_button_clicked), self);
      gtk_box_append (GTK_BOX (reaction_box), btn);
    }
  gtk_box_append (GTK_BOX (main_box), reaction_box);

  /* Separator */
  GtkWidget *separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append (GTK_BOX (main_box), separator);

  /* Quote button */
  GtkWidget *quote_btn = gtk_button_new_with_label ("Quote");
  gtk_button_set_has_frame (GTK_BUTTON (quote_btn), FALSE);
  gtk_widget_add_css_class (quote_btn, "context-menu-item");
  g_signal_connect (quote_btn, "clicked", G_CALLBACK (on_quote_button_clicked), self);
  gtk_box_append (GTK_BOX (main_box), quote_btn);

  /* Edit button (only visible for own messages) */
  self->edit_button = gtk_button_new_with_label ("Edit");
  gtk_button_set_has_frame (GTK_BUTTON (self->edit_button), FALSE);
  gtk_widget_add_css_class (self->edit_button, "context-menu-item");
  gtk_widget_set_visible (self->edit_button, FALSE);
  g_signal_connect (self->edit_button, "clicked", G_CALLBACK (on_edit_button_clicked), self);
  gtk_box_append (GTK_BOX (main_box), self->edit_button);

  /* Delete button (only visible for own messages within time limit) */
  self->delete_button = gtk_button_new_with_label ("Delete");
  gtk_button_set_has_frame (GTK_BUTTON (self->delete_button), FALSE);
  gtk_widget_add_css_class (self->delete_button, "context-menu-item");
  gtk_widget_set_visible (self->delete_button, FALSE);
  g_signal_connect (self->delete_button, "clicked", G_CALLBACK (on_delete_button_clicked), self);
  gtk_box_append (GTK_BOX (main_box), self->delete_button);

  /* Start a thread button (visible when threads supported and message is threadable) */
  self->start_thread_button = gtk_button_new_with_label ("Start a thread");
  gtk_button_set_has_frame (GTK_BUTTON (self->start_thread_button), FALSE);
  gtk_widget_add_css_class (self->start_thread_button, "context-menu-item");
  gtk_widget_set_visible (self->start_thread_button, FALSE);
  g_signal_connect (self->start_thread_button, "clicked", G_CALLBACK (on_start_thread_clicked), self);
  gtk_box_append (GTK_BOX (main_box), self->start_thread_button);

  gtk_popover_set_child (GTK_POPOVER (self->reaction_popover), main_box);

  /* Connect thread button (in-template) */
  g_signal_connect (self->thread_button, "clicked", G_CALLBACK (on_thread_button_clicked), self);

  /* Add right-click gesture with CAPTURE phase to intercept before label context menu */
  GtkGesture *gesture = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), GDK_BUTTON_SECONDARY);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (gesture), GTK_PHASE_CAPTURE);
  g_signal_connect (gesture, "pressed", G_CALLBACK (on_right_click), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (gesture));

  /* Add long press gesture for touch devices */
  GtkGesture *long_press = gtk_gesture_long_press_new ();
  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (long_press), TRUE);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (long_press), GTK_PHASE_CAPTURE);
  g_signal_connect (long_press, "pressed", G_CALLBACK (on_long_press), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (long_press));

  /* Connect quote widget signal for scrolling to quoted message */
  g_signal_connect (self->quote_widget, "scroll-to-message-requested",
                    G_CALLBACK (on_quote_scroll_requested), self);

  /* Connect file preview clicked signal for opening files */
  g_signal_connect (self->file_preview, "clicked",
                    G_CALLBACK (on_file_preview_clicked), self);

  /* Connect upload preview clicked signal for opening uploaded files */
  g_signal_connect (self->upload_preview, "clicked",
                    G_CALLBACK (on_upload_preview_clicked), self);

  /* Connect audio player play-requested signal to trigger file download */
  g_signal_connect (self->audio_player, "play-requested",
                    G_CALLBACK (on_audio_play_requested), self);

  /* Connect video thumbnail clicked signal to trigger file download/playback */
  g_signal_connect (self->video_thumbnail, "clicked",
                    G_CALLBACK (on_video_thumbnail_clicked), self);

  /* Create pre-allocated pool of 12 compact file previews for grouped rendering */
  for (int i = 0; i < 12; i++)
    {
      self->group_file_pool[i] = gnalk_file_preview_new ();
      gnalk_file_preview_set_thumbnail_size (self->group_file_pool[i], 130, 100);
      gtk_widget_add_css_class (GTK_WIDGET (self->group_file_pool[i]), "file-group-item");
      g_signal_connect (self->group_file_pool[i], "clicked",
                        G_CALLBACK (on_group_file_clicked), self);
      gtk_flow_box_insert (GTK_FLOW_BOX (self->file_group_flow),
                           GTK_WIDGET (self->group_file_pool[i]), -1);
      /* Hide the GtkFlowBoxChild wrapper (not just the inner widget) */
      gtk_widget_set_visible (gtk_widget_get_parent (GTK_WIDGET (self->group_file_pool[i])), FALSE);
    }
}

GnalkMessageRow *
gnalk_message_row_new (void)
{
  return g_object_new (GNALK_TYPE_MESSAGE_ROW, NULL);
}

static char *
format_timestamp (gint64 timestamp)
{
  if (timestamp <= 0)
    return g_strdup ("");

  time_t t = (time_t) timestamp;
  struct tm tm_info_buf;
  struct tm *tm_info = localtime_r (&t, &tm_info_buf);
  if (!tm_info)
    return g_strdup ("");

  char buffer[64];
  strftime (buffer, sizeof (buffer), "%H:%M", tm_info);

  return g_strdup (buffer);
}

static gboolean
is_single_emoji (const char *text)
{
  if (!text || !*text)
    return FALSE;

  const char *p = text;
  while (*p == ' ')
    p++;

  if (!*p)
    return FALSE;

  gunichar first = g_utf8_get_char (p);

  if (first < 0x1F300)
    return FALSE;

  int grapheme_count = 0;
  const char *cur = p;

  while (*cur)
    {
      gunichar c = g_utf8_get_char (cur);

      if (c == 0xFE0F || c == 0x200D)
        {
          cur = g_utf8_next_char (cur);
          continue;
        }

      if (c >= 0x1F3FB && c <= 0x1F3FF)
        {
          cur = g_utf8_next_char (cur);
          continue;
        }

      gboolean is_emoji = (c >= 0x1F300 && c <= 0x1FAFF) ||
                          (c >= 0x2600 && c <= 0x27BF) ||
                          (c >= 0x2300 && c <= 0x23FF) ||
                          (c >= 0xE0020 && c <= 0xE007F);

      if (!is_emoji)
        {
          if (c == ' ' || c == '\n' || c == '\t')
            {
              cur = g_utf8_next_char (cur);
              continue;
            }
          return FALSE;
        }

      grapheme_count++;
      if (grapheme_count > 3)
        return FALSE;

      cur = g_utf8_next_char (cur);
    }

  return grapheme_count >= 1 && grapheme_count <= 3;
}

/* Weak reference wrapper for async avatar callbacks */
typedef struct
{
  GWeakRef weak_ref;
} AvatarCallbackData;

static AvatarCallbackData *
avatar_callback_data_new (GnalkMessageRow *self)
{
  AvatarCallbackData *data = g_new0 (AvatarCallbackData, 1);
  g_weak_ref_init (&data->weak_ref, self);
  return data;
}

static void
on_avatar_loaded (GnalkAvatarCache *cache,
                  GdkTexture        *texture,
                  gpointer           user_data)
{
  AvatarCallbackData *data = user_data;
  GnalkMessageRow *self = g_weak_ref_get (&data->weak_ref);

  if (self)
    {
      if (texture)
        gtk_image_set_from_paintable (GTK_IMAGE (self->avatar), GDK_PAINTABLE (texture));
      else
        gtk_image_set_from_icon_name (GTK_IMAGE (self->avatar), "avatar-default-symbolic");

      /* Also set bubble color when avatar loads */
      if (self->author_id && *self->author_id)
        {
          GdkRGBA color;
          if (gnalk_avatar_cache_get_user_color (cache, self->author_id, &color))
            gnalk_message_row_set_bubble_color (self, &color);
        }

      g_object_unref (self);
    }

  g_weak_ref_clear (&data->weak_ref);
  g_free (data);
}

/* Callback data for video thumbnail preview */
typedef struct {
  GWeakRef weak_ref;
} VideoThumbnailCallbackData;

static VideoThumbnailCallbackData *
video_thumbnail_callback_data_new (GnalkMessageRow *self)
{
  VideoThumbnailCallbackData *data = g_new0 (VideoThumbnailCallbackData, 1);
  g_weak_ref_init (&data->weak_ref, self);
  return data;
}

static void
on_video_thumbnail_loaded (GnalkAvatarCache *cache G_GNUC_UNUSED,
                           GdkTexture        *texture,
                           gpointer           user_data)
{
  VideoThumbnailCallbackData *data = user_data;
  GnalkMessageRow *self = g_weak_ref_get (&data->weak_ref);

  if (self)
    {
      if (texture)
        {
          gnalk_video_thumbnail_set_texture (GNALK_VIDEO_THUMBNAIL (self->video_thumbnail), texture);
        }
      else
        {
          /* No server preview - show placeholder with play icon */
          gnalk_video_thumbnail_set_loading (GNALK_VIDEO_THUMBNAIL (self->video_thumbnail), FALSE);
        }
      g_object_unref (self);
    }

  g_weak_ref_clear (&data->weak_ref);
  g_free (data);
}

void
gnalk_message_row_set_data (GnalkMessageRow  *self,
                              JsonObject        *message_data,
                              const char        *current_user_id,
                              GnalkAvatarCache *avatar_cache)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));
  g_return_if_fail (message_data != NULL);

  gint64 new_message_id = json_object_get_int_member_with_default (message_data, "id", 0);

  /* Skip if same message already bound */
  if (self->message_id == new_message_id && new_message_id != 0)
    return;

  self->message_id = new_message_id;
  self->message_timestamp = json_object_get_int_member_with_default (message_data, "timestamp", 0);

  const char *actor_id = json_object_get_string_member_with_default (message_data, "actorId", "");
  const char *actor_name = json_object_get_string_member_with_default (message_data, "actorDisplayName", "");
  const char *message_text = json_object_get_string_member_with_default (message_data, "message", "");
  const char *message_type = json_object_get_string_member_with_default (message_data, "messageType", "comment");
  const char *ref_id = json_object_get_string_member_with_default (message_data, "referenceId", "");
  gint64 timestamp = self->message_timestamp;

  g_free (self->author_id);
  self->author_id = g_strdup (actor_id);

  g_free (self->reference_id);
  self->reference_id = (*ref_id) ? g_strdup (ref_id) : NULL;

  self->is_self = g_strcmp0 (actor_id, current_user_id) == 0;
  self->is_system = g_strcmp0 (message_type, "system") == 0;

  g_autofree char *ts_str = format_timestamp (timestamp);
  g_autofree char *display_message = NULL;

  if (self->is_system)
    {
      const char *sys_type = json_object_get_string_member_with_default (
          message_data, "systemMessage", "");
      JsonObject *params = NULL;
      if (json_object_has_member (message_data, "messageParameters"))
        {
          JsonNode *n = json_object_get_member (message_data, "messageParameters");
          if (n && JSON_NODE_HOLDS_OBJECT (n))
            params = json_node_get_object (n);
        }
      display_message = gnalk_system_message_format (
          sys_type, actor_name, self->is_self, params);
      if (!display_message)
        display_message = g_strdup (message_text);
    }
  else
    {
      /* Replace {file} and other parameters using centralized utility */
      JsonObject *params = NULL;
      if (json_object_has_member (message_data, "messageParameters"))
        {
          JsonNode *params_node = json_object_get_member (message_data, "messageParameters");
          if (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
            params = json_node_get_object (params_node);
        }
      display_message = gnalk_message_utils_replace_parameters (message_text, params, NULL);
    }

  g_free (self->author_name);
  g_free (self->message);
  g_free (self->timestamp);
  self->author_name = g_strdup (actor_name);
  self->message = g_strdup (display_message);
  self->timestamp = g_strdup (ts_str);

  gtk_label_set_text (GTK_LABEL (self->username_label), actor_name);
  gtk_label_set_text (GTK_LABEL (self->timestamp_label), ts_str);

  /* Render message with markdown if enabled */
  gboolean render_md = json_object_get_boolean_member_with_default (message_data, "markdown", TRUE);
  if (!self->is_system && render_md && display_message && *display_message)
    {
      JsonObject *params = NULL;
      if (json_object_has_member (message_data, "messageParameters"))
        {
          JsonNode *params_node = json_object_get_member (message_data, "messageParameters");
          if (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
            params = json_node_get_object (params_node);
        }
      g_autofree char *markup = gnalk_markdown_to_pango (display_message, params);
      gtk_label_set_markup (GTK_LABEL (self->message_label), markup);
    }
  else
    {
      g_autofree char *escaped = gnalk_markdown_escape_pango (display_message ? display_message : "");
      gtk_label_set_markup (GTK_LABEL (self->message_label), escaped);
    }

  gtk_widget_remove_css_class (self->message_label, "message-emoji-only");
  if (!self->is_system && is_single_emoji (display_message))
    gtk_widget_add_css_class (self->message_label, "message-emoji-only");

  /* Apply layout based on message type and display mode */
  apply_layout (self);

  /* Set status for own messages - SENT since they came from server */
  if (self->is_self && !self->is_system)
    {
      self->status = GNALK_MESSAGE_STATUS_SENT;
      update_status_icon (self);
    }
  else
    {
      self->status = GNALK_MESSAGE_STATUS_NONE;
      gtk_widget_set_visible (self->status_icon, FALSE);
    }

  if (avatar_cache && *actor_id && !self->is_system)
    gnalk_avatar_cache_get_user_avatar (avatar_cache, actor_id, 64, on_avatar_loaded,
                                         avatar_callback_data_new (self));

  /* Parse reactions if present and is an object */
  if (json_object_has_member (message_data, "reactions"))
    {
      JsonNode *reactions_node = json_object_get_member (message_data, "reactions");
      if (reactions_node && JSON_NODE_HOLDS_OBJECT (reactions_node))
        {
          JsonObject *reactions = json_node_get_object (reactions_node);
          gnalk_message_row_set_reactions (self, reactions);
        }
    }

  /* Parse parent/quoted message if present (but not for reaction messages) */
  /* Set up quote widget if parent message exists */
  const char *system_message = json_object_get_string_member_with_default (message_data, "systemMessage", "");
  if (json_object_has_member (message_data, "parent") && *system_message == '\0')
    {
      JsonNode *parent_node = json_object_get_member (message_data, "parent");
      if (parent_node && JSON_NODE_HOLDS_OBJECT (parent_node))
        {
          JsonObject *parent = json_node_get_object (parent_node);
          if (parent)
            {
              const char *parent_actor_id = json_object_get_string_member_with_default (parent, "actorId", "");
              const char *parent_actor = json_object_get_string_member_with_default (parent, "actorDisplayName", "");
              const char *parent_message = json_object_get_string_member_with_default (parent, "message", "");
              gint64 parent_id = json_object_get_int_member_with_default (parent, "id", 0);

              if (*parent_actor && *parent_message)
                {
                  self->parent_message_id = parent_id;

                  /* Hide text for media-only quoted messages */
                  const char *quote_text = g_strcmp0 (parent_message, "{file}") == 0 ? "" : parent_message;
                  gnalk_quote_widget_set_quote (self->quote_widget, parent_id, parent_actor_id,
                                                 parent_actor, quote_text, avatar_cache);

                  /* Load quote file preview if parent has a file */
                  if (avatar_cache && json_object_has_member (parent, "messageParameters"))
                    {
                      JsonNode *parent_params_node = json_object_get_member (parent, "messageParameters");
                      if (parent_params_node && JSON_NODE_HOLDS_OBJECT (parent_params_node))
                        {
                          JsonObject *parent_params = json_node_get_object (parent_params_node);
                          if (parent_params && json_object_has_member (parent_params, "file"))
                            {
                              JsonNode *parent_file_node = json_object_get_member (parent_params, "file");
                              if (parent_file_node && JSON_NODE_HOLDS_OBJECT (parent_file_node))
                                {
                                  JsonObject *parent_file = json_node_get_object (parent_file_node);
                                  const char *parent_file_id = json_object_get_string_member_with_default (parent_file, "id", NULL);
                                  const char *parent_preview_avail = json_object_get_string_member_with_default (parent_file, "preview-available", "no");

                                  if (parent_file_id && g_strcmp0 (parent_preview_avail, "yes") == 0)
                                    gnalk_quote_widget_set_file_preview (self->quote_widget, parent_file_id, avatar_cache);
                                }
                            }
                        }
                    }
                }
              else
                {
                  gnalk_quote_widget_reset (self->quote_widget);
                }
            }
          else
            {
              gnalk_quote_widget_reset (self->quote_widget);
            }
        }
      else
        {
          gnalk_quote_widget_reset (self->quote_widget);
        }
    }
  else
    {
      gnalk_quote_widget_reset (self->quote_widget);
    }

  /* Parse file preview if present in messageParameters */
  gnalk_file_preview_reset (self->file_preview);
  gtk_widget_set_visible (self->audio_player, FALSE);
  gtk_widget_set_visible (self->video_thumbnail, FALSE);

  if (json_object_has_member (message_data, "messageParameters") && !self->is_system)
    {
      JsonNode *params_node = json_object_get_member (message_data, "messageParameters");
      if (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
        {
          JsonObject *params = json_node_get_object (params_node);
          if (params && json_object_has_member (params, "file"))
            {
              JsonNode *file_node = json_object_get_member (params, "file");
              if (file_node && JSON_NODE_HOLDS_OBJECT (file_node))
                {
                  JsonObject *file = json_node_get_object (file_node);
                  const char *file_id = json_object_get_string_member_with_default (file, "id", NULL);
                  const char *preview_available = json_object_get_string_member_with_default (file, "preview-available", "no");
                  const char *mimetype = json_object_get_string_member_with_default (file, "mimetype", "");
                  const char *file_link = json_object_get_string_member_with_default (file, "link", "");
                  const char *file_name = json_object_get_string_member_with_default (file, "name", "");
                  const char *file_path = json_object_get_string_member_with_default (file, "path", "");

                  /* Check if this is an audio file */
                  if (mimetype && g_str_has_prefix (mimetype, "audio/"))
                    {
                      /* Show audio player instead of file preview */
                      gtk_widget_set_visible (self->audio_player, TRUE);

                      /* Store file info in file_preview widget for click handling */
                      gnalk_file_preview_set_file (self->file_preview, file_id, file_name, mimetype,
                                                    file_link, file_path, NULL);

                      /* Check if file is already cached - if so, load waveform immediately */
                      const char *cache_base = g_get_user_cache_dir ();
                      g_autofree char *cache_dir = g_build_filename (cache_base, "gnalk", NULL);
                      const char *fname = (file_name && *file_name) ? file_name : "download";
                      g_autofree char *safe_name = g_strdup_printf ("%s_%s", file_id ? file_id : "unknown", fname);
                      g_autofree char *cache_path = g_build_filename (cache_dir, safe_name, NULL);

                      if (g_file_test (cache_path, G_FILE_TEST_EXISTS))
                        {
                          gnalk_audio_player_set_file (GNALK_AUDIO_PLAYER (self->audio_player), cache_path);
                        }
                    }
                  /* Check if this is a video file */
                  else if (mimetype && g_str_has_prefix (mimetype, "video/"))
                    {
                      /* Show video thumbnail instead of file preview */
                      gtk_widget_set_visible (self->video_thumbnail, TRUE);
                      gnalk_video_thumbnail_set_file_id (GNALK_VIDEO_THUMBNAIL (self->video_thumbnail), file_id);

                      /* Store file info in file_preview widget for click handling */
                      gnalk_file_preview_set_file (self->file_preview, file_id, file_name, mimetype,
                                                    file_link, file_path, NULL);

                      /* Fetch server-generated preview thumbnail */
                      if (file_id && avatar_cache)
                        {
                          gnalk_video_thumbnail_set_loading (GNALK_VIDEO_THUMBNAIL (self->video_thumbnail), TRUE);
                          gnalk_avatar_cache_get_file_preview (avatar_cache, file_id, 280, 210,
                                                                on_video_thumbnail_loaded,
                                                                video_thumbnail_callback_data_new (self));
                        }
                      else
                        {
                          /* No file ID or cache - show placeholder */
                          gnalk_video_thumbnail_set_loading (GNALK_VIDEO_THUMBNAIL (self->video_thumbnail), FALSE);
                        }
                    }
                  else if (file_id && g_strcmp0 (preview_available, "yes") == 0 && avatar_cache)
                    {
                      /* Use file preview widget - it handles loading internally */
                      gnalk_file_preview_set_file (self->file_preview, file_id, file_name, mimetype,
                                                    file_link, file_path, avatar_cache);
                    }
                  else if (file_id)
                    {
                      /* Store file info even if no preview available (for click handling) */
                      gnalk_file_preview_set_file (self->file_preview, file_id, file_name, mimetype,
                                                    file_link, file_path, NULL);
                    }
                }
            }
        }
    }

  /* Hide message label for media-only file messages (message is just "{file}"),
   * but keep bubble visible since file_preview/video_thumbnail are inside it */
  gboolean has_inline_media = gtk_widget_get_visible (self->audio_player)
                               || gtk_widget_get_visible (GTK_WIDGET (self->file_preview))
                               || gtk_widget_get_visible (self->video_thumbnail);
  gboolean has_visual_file = gtk_widget_get_visible (GTK_WIDGET (self->file_preview))
                              || gtk_widget_get_visible (self->video_thumbnail);
  if (has_inline_media && g_strcmp0 (message_text, "{file}") == 0)
    {
      gtk_widget_set_visible (self->message_label, FALSE);
      /* Bubble stays visible — file/video are inside it; audio is outside but bubble still has quote/file */
      gtk_widget_set_visible (self->message_bubble,
                              has_visual_file
                              || gtk_widget_get_visible (GTK_WIDGET (self->quote_widget)));
    }
  /* Zero-padding on bubble when it contains media (not audio — audio is outside) */
  if (has_visual_file)
    {
      gtk_widget_add_css_class (self->message_bubble, "message-media-bubble");
      gtk_box_set_spacing (GTK_BOX (self->message_bubble), 0);
    }
}

gint64
gnalk_message_row_get_id (GnalkMessageRow *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_ROW (self), 0);
  return self->message_id;
}

gint64
gnalk_message_row_get_timestamp (GnalkMessageRow *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_ROW (self), 0);
  return self->message_timestamp;
}

/* Set reactions from a JSON object like {"😆": 1, "👍": 2} */
void
gnalk_message_row_set_reactions (GnalkMessageRow *self,
                                  JsonObject       *reactions)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));

  if (!reactions || json_object_get_size (reactions) == 0)
    {
      gtk_widget_set_visible (self->reactions_label, FALSE);
      return;
    }

  GString *reactions_str = g_string_new (NULL);
  JsonObjectIter iter;
  const char *emoji;
  JsonNode *count_node;

  json_object_iter_init (&iter, reactions);
  while (json_object_iter_next (&iter, &emoji, &count_node))
    {
      if (reactions_str->len > 0)
        g_string_append_c (reactions_str, ' ');
      g_string_append (reactions_str, emoji);
    }

  gtk_label_set_text (GTK_LABEL (self->reactions_label), reactions_str->str);
  gtk_widget_set_visible (self->reactions_label, TRUE);
  g_string_free (reactions_str, TRUE);
}

/* Add a single reaction emoji to existing reactions */
void
gnalk_message_row_add_reaction (GnalkMessageRow *self,
                                 const char       *emoji)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));
  g_return_if_fail (emoji != NULL);

  const char *current = gtk_label_get_text (GTK_LABEL (self->reactions_label));
  g_autofree char *new_reactions = NULL;

  if (current && *current)
    {
      /* Check if emoji already present */
      if (strstr (current, emoji))
        return;
      new_reactions = g_strdup_printf ("%s %s", current, emoji);
    }
  else
    {
      new_reactions = g_strdup (emoji);
    }

  gtk_label_set_text (GTK_LABEL (self->reactions_label), new_reactions);
  gtk_widget_set_visible (self->reactions_label, TRUE);
}

/* Get the date (year, month, day) from the message timestamp for comparison */
void
gnalk_message_row_get_date (GnalkMessageRow *self,
                             int              *year,
                             int              *month,
                             int              *day)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));

  if (self->message_timestamp <= 0)
    {
      *year = 0;
      *month = 0;
      *day = 0;
      return;
    }

  time_t t = (time_t) self->message_timestamp;
  struct tm tm_info_buf;
  struct tm *tm_info = localtime_r (&t, &tm_info_buf);

  if (tm_info)
    {
      *year = tm_info->tm_year + 1900;
      *month = tm_info->tm_mon + 1;
      *day = tm_info->tm_mday;
    }
  else
    {
      *year = 0;
      *month = 0;
      *day = 0;
    }
}

/* Show or hide the date separator header */
void
gnalk_message_row_show_date_header (GnalkMessageRow *self,
                                     gboolean          show)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));

  if (!show)
    {
      gtk_widget_set_visible (GTK_WIDGET (self->date_separator), FALSE);
      return;
    }

  /* Let the date separator widget handle formatting */
  gnalk_date_separator_set_timestamp (self->date_separator, self->message_timestamp);
}

/* Set avatar grouping state (first message of consecutive messages from same author).
 * Actual visibility is determined by apply_layout based on display mode and room type. */
void
gnalk_message_row_set_show_avatar (GnalkMessageRow *self,
                                    gboolean          show)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));
  self->show_avatar = show;
}

/* Set whether this is a 1-to-1 conversation */
void
gnalk_message_row_set_one_to_one (GnalkMessageRow *self,
                                    gboolean          is_one_to_one)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));

  self->is_one_to_one = is_one_to_one;
}

/* Set display mode and re-apply layout */
void
gnalk_message_row_set_display_mode (GnalkMessageRow        *self,
                                      GnalkMessageDisplayMode mode)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));
  self->display_mode = mode;
  apply_layout (self);
}

/* Helper to apply bubble color based on current theme */
static void
apply_bubble_color (GnalkMessageRow *self)
{
  if (!self->has_bubble_color || !self->bubble_css_provider)
    return;

  /* Generate unique CSS class name based on widget pointer */
  g_autofree char *class_name = g_strdup_printf ("bubble-color-%p", (void *)self);

  /* Check if we're in dark mode */
  AdwStyleManager *style_manager = adw_style_manager_get_default ();
  gboolean is_dark = adw_style_manager_get_dark (style_manager);

  gdouble r, g, b;
  const GdkRGBA *color = &self->bubble_color;

  if (is_dark)
    {
      /* Dark mode: darken the color (mix with dark gray for muted tint) */
      r = 0.15 + color->red * 0.25;
      g = 0.15 + color->green * 0.25;
      b = 0.15 + color->blue * 0.25;
    }
  else
    {
      /* Light mode: lighten the color (mix with white for pastel tint) */
      r = 0.7 + color->red * 0.3;
      g = 0.7 + color->green * 0.3;
      b = 0.7 + color->blue * 0.3;
    }

  /* Create CSS rule for this specific class */
  g_autofree char *css = g_strdup_printf (
    ".%s { background-color: rgb(%d, %d, %d); }",
    class_name,
    (int)(r * 255),
    (int)(g * 255),
    (int)(b * 255)
  );

  gtk_css_provider_load_from_string (self->bubble_css_provider, css);
}

static void
on_dark_mode_changed (AdwStyleManager  *style_manager G_GNUC_UNUSED,
                      GParamSpec       *pspec G_GNUC_UNUSED,
                      GnalkMessageRow *self)
{
  apply_bubble_color (self);
}

void
gnalk_message_row_set_bubble_color (GnalkMessageRow *self,
                                      const GdkRGBA    *color)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));

  if (!color || self->is_system)
    return;

  /* Store the original color for theme change updates */
  self->bubble_color = *color;
  self->has_bubble_color = TRUE;

  /* Create CSS provider on first use and add to display */
  if (!self->bubble_css_provider)
    {
      self->bubble_css_provider = gtk_css_provider_new ();
      GdkDisplay *display = gtk_widget_get_display (self->message_bubble);
      gtk_style_context_add_provider_for_display (display,
                                                   GTK_STYLE_PROVIDER (self->bubble_css_provider),
                                                   GTK_STYLE_PROVIDER_PRIORITY_USER);
    }

  /* Generate unique CSS class name based on widget pointer */
  g_autofree char *class_name = g_strdup_printf ("bubble-color-%p", (void *)self);

  /* Add the unique class to the bubble widget */
  gtk_widget_add_css_class (self->message_bubble, class_name);

  /* Connect to dark mode changes (fires for both system and preference changes) */
  if (!self->dark_mode_handler_id)
    {
      AdwStyleManager *style_manager = adw_style_manager_get_default ();
      self->dark_mode_handler_id = g_signal_connect (style_manager, "notify::dark",
                                                      G_CALLBACK (on_dark_mode_changed), self);
    }

  /* Apply the color for current theme */
  apply_bubble_color (self);
}

static void
update_status_icon (GnalkMessageRow *self)
{
  if (!self->is_self || self->is_system)
    {
      gtk_widget_set_visible (self->status_icon, FALSE);
      return;
    }

  const char *icon_name = NULL;

  switch (self->status)
    {
    case GNALK_MESSAGE_STATUS_SENDING:
      icon_name = "content-loading-symbolic";
      break;
    case GNALK_MESSAGE_STATUS_SENT:
      icon_name = "emblem-ok-symbolic";
      break;
    case GNALK_MESSAGE_STATUS_READ:
      icon_name = "mail-read-symbolic";
      break;
    case GNALK_MESSAGE_STATUS_FAILED:
      icon_name = "dialog-error-symbolic";
      break;
    case GNALK_MESSAGE_STATUS_NONE:
    default:
      gtk_widget_set_visible (self->status_icon, FALSE);
      return;
    }

  gtk_image_set_from_icon_name (GTK_IMAGE (self->status_icon), icon_name);
  gtk_widget_set_visible (self->status_icon, TRUE);
}

void
gnalk_message_row_set_status (GnalkMessageRow   *self,
                               GnalkMessageStatus status)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));

  if (self->status == status)
    return;

  self->status = status;
  update_status_icon (self);
}

GnalkMessageStatus
gnalk_message_row_get_status (GnalkMessageRow *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_ROW (self), GNALK_MESSAGE_STATUS_NONE);
  return self->status;
}

void
gnalk_message_row_set_reference_id (GnalkMessageRow *self,
                                     const char       *reference_id)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));

  g_free (self->reference_id);
  self->reference_id = g_strdup (reference_id);
}

const char *
gnalk_message_row_get_reference_id (GnalkMessageRow *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_ROW (self), NULL);
  return self->reference_id;
}

GnalkMessageRow *
gnalk_message_row_new_pending (const char        *message_text,
                                const char        *author_name,
                                const char        *author_id,
                                const char        *reference_id,
                                gint64             reply_to,
                                GnalkAvatarCache *avatar_cache)
{
  GnalkMessageRow *self = gnalk_message_row_new ();

  self->message_id = 0;  /* No ID yet - pending */
  self->message_timestamp = (gint64) time (NULL);
  self->is_self = TRUE;
  self->is_system = FALSE;
  self->status = GNALK_MESSAGE_STATUS_SENDING;

  g_free (self->author_name);
  g_free (self->author_id);
  g_free (self->message);
  g_free (self->reference_id);

  self->author_name = g_strdup (author_name);
  self->author_id = g_strdup (author_id);
  self->message = g_strdup (message_text);
  self->reference_id = g_strdup (reference_id);

  /* Set up the visual elements */
  g_autofree char *ts_str = format_timestamp (self->message_timestamp);
  g_free (self->timestamp);
  self->timestamp = g_strdup (ts_str);

  gtk_label_set_text (GTK_LABEL (self->username_label), author_name);
  gtk_label_set_text (GTK_LABEL (self->timestamp_label), ts_str);

  /* Render message with markdown */
  if (message_text && *message_text)
    {
      g_autofree char *markup = gnalk_markdown_to_pango (message_text, NULL);
      gtk_label_set_markup (GTK_LABEL (self->message_label), markup);
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (self->message_label), "");
    }

  gtk_widget_remove_css_class (self->message_label, "message-emoji-only");
  if (is_single_emoji (message_text))
    gtk_widget_add_css_class (self->message_label, "message-emoji-only");

  /* Apply layout based on message type and display mode */
  apply_layout (self);

  /* Load avatar */
  if (avatar_cache && author_id && *author_id)
    gnalk_avatar_cache_get_user_avatar (avatar_cache, author_id, 64, on_avatar_loaded,
                                         avatar_callback_data_new (self));

  /* Hide quote widget for pending messages */
  gnalk_quote_widget_reset (self->quote_widget);

  /* Update status icon */
  update_status_icon (self);

  return self;
}

void
gnalk_message_row_confirm_sent (GnalkMessageRow *self,
                                  gint64            message_id,
                                  gint64            timestamp)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));

  self->message_id = message_id;

  if (timestamp > 0)
    {
      self->message_timestamp = timestamp;
      g_autofree char *ts_str = format_timestamp (timestamp);
      g_free (self->timestamp);
      self->timestamp = g_strdup (ts_str);
      gtk_label_set_text (GTK_LABEL (self->timestamp_label), ts_str);
    }

  self->status = GNALK_MESSAGE_STATUS_SENT;
  update_status_icon (self);
}

void
gnalk_message_row_set_from_data (GnalkMessageRow  *self,
                                   GnalkMessageData *data,
                                   GnalkAvatarCache *avatar_cache,
                                   const char        *room_token)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));
  g_return_if_fail (GNALK_IS_MESSAGE_DATA (data));

  gint64 new_message_id = gnalk_message_data_get_id (data);

  /* Skip if same message already bound */
  if (self->message_id == new_message_id && new_message_id != 0)
    return;

  /* Hidden group member: hide the entire row and return immediately */
  if (gnalk_message_data_get_is_attachment_group_hidden (data))
    {
      self->message_id = new_message_id;
      gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
      /* Still connect for live updates so recompute can change hidden state */
      if (self->bound_data)
        {
          g_signal_handler_disconnect (self->bound_data, self->unread_handler_id);
          g_clear_object (&self->bound_data);
        }
      self->bound_data = g_object_ref (data);
      self->unread_handler_id = g_signal_connect (data, "notify",
                                                   G_CALLBACK (on_bound_data_notify), self);
      return;
    }

  self->message_id = new_message_id;
  self->message_timestamp = gnalk_message_data_get_timestamp (data);

  const char *actor_id = gnalk_message_data_get_author_id (data);
  const char *actor_name = gnalk_message_data_get_author_name (data);
  const char *message_text = gnalk_message_data_get_message (data);
  const char *ref_id = gnalk_message_data_get_reference_id (data);
  const char *system_message = gnalk_message_data_get_system_message (data);
  gint64 timestamp = self->message_timestamp;

  g_free (self->author_id);
  self->author_id = g_strdup (actor_id ? actor_id : "");

  g_free (self->reference_id);
  self->reference_id = (ref_id && *ref_id) ? g_strdup (ref_id) : NULL;

  self->is_self = gnalk_message_data_get_is_own (data);
  self->is_system = (system_message && *system_message);
  self->is_bot = gnalk_message_data_is_bot (data);

  g_autofree char *ts_str = format_timestamp (timestamp);
  g_autofree char *display_message = NULL;

  {
    /* Replace all message parameters using centralized utility */
    JsonObject *params = gnalk_message_data_get_message_parameters (data);
    display_message = gnalk_message_utils_replace_parameters (
        message_text, params, self->is_system ? actor_name : NULL);
  }

  g_free (self->author_name);
  g_free (self->message);
  g_free (self->timestamp);
  self->author_name = g_strdup (actor_name);
  self->message = g_strdup (display_message);
  self->timestamp = g_strdup (ts_str);

  gtk_label_set_text (GTK_LABEL (self->username_label), actor_name ? actor_name : "");

  /* Render message with markdown if enabled */
  gboolean render_md = gnalk_message_data_get_render_markdown (data);
  if (render_md && display_message && *display_message)
    {
      JsonObject *md_params = gnalk_message_data_get_message_parameters (data);
      g_autofree char *markup = gnalk_markdown_to_pango (display_message, md_params);
      gtk_label_set_markup (GTK_LABEL (self->message_label), markup);
    }
  else
    {
      g_autofree char *escaped = gnalk_markdown_escape_pango (display_message ? display_message : "");
      gtk_label_set_markup (GTK_LABEL (self->message_label), escaped);
    }

  gtk_label_set_text (GTK_LABEL (self->timestamp_label), ts_str);

  gtk_widget_remove_css_class (self->message_label, "message-emoji-only");
  if (!self->is_system && is_single_emoji (display_message))
    gtk_widget_add_css_class (self->message_label, "message-emoji-only");

  /* Apply layout based on message type and display mode */
  apply_layout (self);

  /* Map status from data */
  GnalkMessageDataStatus data_status = gnalk_message_data_get_status (data);
  switch (data_status)
    {
    case GNALK_MESSAGE_DATA_STATUS_SENDING:
      self->status = GNALK_MESSAGE_STATUS_SENDING;
      break;
    case GNALK_MESSAGE_DATA_STATUS_SENT:
      self->status = GNALK_MESSAGE_STATUS_SENT;
      break;
    case GNALK_MESSAGE_DATA_STATUS_READ:
      self->status = GNALK_MESSAGE_STATUS_READ;
      break;
    case GNALK_MESSAGE_DATA_STATUS_FAILED:
      self->status = GNALK_MESSAGE_STATUS_FAILED;
      break;
    default:
      self->status = GNALK_MESSAGE_STATUS_NONE;
      break;
    }

  if (self->is_self && !self->is_system)
    update_status_icon (self);
  else
    gtk_widget_set_visible (self->status_icon, FALSE);

  /* Load avatar */
  if (!self->is_system)
    {
      if (self->is_bot)
        {
          /* Bots: smaller icon centered in 32x32 round frame with background */
          gtk_widget_set_size_request (self->avatar_frame, 32, 32);
          gtk_image_set_pixel_size (GTK_IMAGE (self->avatar), 18);
          gtk_image_set_from_icon_name (GTK_IMAGE (self->avatar), "computer-symbolic");
          gtk_widget_add_css_class (self->avatar_frame, "bot-avatar");
          GdkRGBA bot_color = { 0.35, 0.55, 0.75, 1.0 };
          gnalk_message_row_set_bubble_color (self, &bot_color);
        }
      else if (avatar_cache && actor_id && *actor_id)
        {
          gtk_widget_set_size_request (self->avatar_frame, -1, -1);
          gtk_image_set_pixel_size (GTK_IMAGE (self->avatar), 32);
          gtk_widget_remove_css_class (self->avatar_frame, "bot-avatar");

          if (gnalk_message_data_is_federated (data) && room_token)
            gnalk_avatar_cache_get_federated_user_avatar (avatar_cache, room_token, actor_id, 64,
                                                            on_avatar_loaded,
                                                            avatar_callback_data_new (self));
          else if (!gnalk_message_data_is_federated (data))
            gnalk_avatar_cache_get_user_avatar (avatar_cache, actor_id, 64, on_avatar_loaded,
                                                 avatar_callback_data_new (self));
        }
    }

  /* Set reactions */
  JsonObject *reactions = gnalk_message_data_get_reactions (data);
  if (reactions)
    gnalk_message_row_set_reactions (self, reactions);
  else
    gtk_widget_set_visible (self->reactions_label, FALSE);

  /* Show edited label if message was edited */
  gint64 last_edit = gnalk_message_data_get_last_edit_timestamp (data);
  gtk_widget_set_visible (self->edited_label, last_edit > 0);

  /* Set parent/quoted message if present */
  JsonObject *parent = gnalk_message_data_get_parent (data);
  if (parent && (!system_message || !*system_message))
    {
      const char *parent_actor_id = json_object_get_string_member_with_default (parent, "actorId", "");
      const char *parent_actor = json_object_get_string_member_with_default (parent, "actorDisplayName", "");
      const char *parent_message = json_object_get_string_member_with_default (parent, "message", "");
      gint64 parent_id = json_object_get_int_member_with_default (parent, "id", 0);

      if (*parent_actor && *parent_message)
        {
          self->parent_message_id = parent_id;

          /* Hide text for media-only quoted messages */
          const char *quote_text = g_strcmp0 (parent_message, "{file}") == 0 ? "" : parent_message;
          gnalk_quote_widget_set_quote (self->quote_widget, parent_id, parent_actor_id,
                                         parent_actor, quote_text, avatar_cache);

          /* Load quote file preview if parent has a file */
          if (avatar_cache && json_object_has_member (parent, "messageParameters"))
            {
              JsonNode *parent_params_node = json_object_get_member (parent, "messageParameters");
              if (parent_params_node && JSON_NODE_HOLDS_OBJECT (parent_params_node))
                {
                  JsonObject *parent_params = json_node_get_object (parent_params_node);
                  if (parent_params && json_object_has_member (parent_params, "file"))
                    {
                      JsonNode *parent_file_node = json_object_get_member (parent_params, "file");
                      if (parent_file_node && JSON_NODE_HOLDS_OBJECT (parent_file_node))
                        {
                          JsonObject *parent_file = json_node_get_object (parent_file_node);
                          const char *parent_file_id = json_object_get_string_member_with_default (parent_file, "id", NULL);
                          const char *parent_preview_avail = json_object_get_string_member_with_default (parent_file, "preview-available", "no");

                          if (parent_file_id && g_strcmp0 (parent_preview_avail, "yes") == 0)
                            gnalk_quote_widget_set_file_preview (self->quote_widget, parent_file_id, avatar_cache);
                        }
                    }
                }
            }
        }
      else
        {
          gnalk_quote_widget_reset (self->quote_widget);
        }
    }
  else
    {
      gnalk_quote_widget_reset (self->quote_widget);
    }

  /* Show/hide date header based on data's flag */
  gboolean show_date = gnalk_message_data_get_show_date_header (data);
  gnalk_message_row_show_date_header (self, show_date);

  /* Parse file preview FIRST so we know if we have server file info
   * (must be done before upload preview logic) */
  gnalk_file_preview_reset (self->file_preview);
  gtk_widget_set_visible (self->audio_player, FALSE);
  gtk_widget_set_visible (self->video_thumbnail, FALSE);
  gtk_widget_set_visible (self->file_group_flow, FALSE);

  gboolean has_server_file_info = FALSE;

  /* === Attachment group leader: show all member files in a flow box === */
  GPtrArray *group_files = gnalk_message_data_get_attachment_group_files (data);
  if (gnalk_message_data_get_is_attachment_group_leader (data) && group_files && group_files->len >= 2)
    {
      has_server_file_info = TRUE;

      /* Hide single-file widgets */
      gtk_widget_set_visible (GTK_WIDGET (self->file_preview), FALSE);
      gtk_widget_set_visible (self->audio_player, FALSE);
      gtk_widget_set_visible (self->video_thumbnail, FALSE);

      /* Show the flow box — set max-children-per-line to actual count
       * so natural width matches content (not the template's default of 4) */
      guint n_files = MIN (group_files->len, 12);
      gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (self->file_group_flow),
                                               CLAMP (n_files, 1, 4));
      gtk_widget_set_visible (self->file_group_flow, TRUE);
      for (guint i = 0; i < n_files; i++)
        {
          GnalkMessageData *member = g_ptr_array_index (group_files, i);
          JsonObject *member_params = gnalk_message_data_get_message_parameters (member);
          if (!member_params || !json_object_has_member (member_params, "file"))
            continue;

          JsonNode *member_file_node = json_object_get_member (member_params, "file");
          if (!member_file_node || !JSON_NODE_HOLDS_OBJECT (member_file_node))
            continue;

          JsonObject *member_file = json_node_get_object (member_file_node);
          const char *m_file_id = json_object_get_string_member_with_default (member_file, "id", NULL);
          const char *m_file_name = json_object_get_string_member_with_default (member_file, "name", "");
          const char *m_mimetype = json_object_get_string_member_with_default (member_file, "mimetype", "");
          const char *m_file_link = json_object_get_string_member_with_default (member_file, "link", "");
          const char *m_file_path = json_object_get_string_member_with_default (member_file, "path", "");
          const char *m_preview = json_object_get_string_member_with_default (member_file, "preview-available", "no");

          GnalkFilePreview *pool_item = self->group_file_pool[i];

          /* For videos, load preview without playing; for images with preview, load normally */
          if (m_file_id && g_strcmp0 (m_preview, "yes") == 0 && avatar_cache)
            gnalk_file_preview_set_file (pool_item, m_file_id, m_file_name, m_mimetype,
                                          m_file_link, m_file_path, avatar_cache);
          else if (m_file_id)
            gnalk_file_preview_set_file (pool_item, m_file_id, m_file_name, m_mimetype,
                                          m_file_link, m_file_path, NULL);

          gtk_widget_set_visible (gtk_widget_get_parent (GTK_WIDGET (pool_item)), TRUE);
        }

      /* Hide unused pool items (hide the GtkFlowBoxChild wrapper) */
      for (guint i = n_files; i < 12; i++)
        gtk_widget_set_visible (gtk_widget_get_parent (GTK_WIDGET (self->group_file_pool[i])), FALSE);

      /* Bubble must be visible — the flow box is inside it; zero-padding for media */
      gtk_widget_set_visible (self->message_bubble, TRUE);
      gtk_widget_add_css_class (self->message_bubble, "message-media-bubble");
      gtk_box_set_spacing (GTK_BOX (self->message_bubble), 0);

      /* Caption: check if the last member has text (not "{file}") */
      GnalkMessageData *last_member = g_ptr_array_index (group_files, group_files->len - 1);
      const char *last_msg = gnalk_message_data_get_message (last_member);
      gboolean has_caption = last_msg && g_strcmp0 (last_msg, "{file}") != 0 && *last_msg;

      if (has_caption)
        {
          /* Show the caption text below the images */
          JsonObject *last_params = gnalk_message_data_get_message_parameters (last_member);
          g_autofree char *caption_display = gnalk_message_utils_replace_parameters (last_msg, last_params, NULL);
          gboolean render_md = gnalk_message_data_get_render_markdown (last_member);
          if (render_md && caption_display && *caption_display)
            {
              g_autofree char *markup = gnalk_markdown_to_pango (caption_display, last_params);
              gtk_label_set_markup (GTK_LABEL (self->message_label), markup);
            }
          else
            {
              g_autofree char *escaped = gnalk_markdown_escape_pango (caption_display ? caption_display : "");
              gtk_label_set_markup (GTK_LABEL (self->message_label), escaped);
            }
          gtk_widget_set_visible (self->message_label, TRUE);
        }
      else
        {
          /* No caption — just images in the bubble */
          gtk_widget_set_visible (self->message_label, FALSE);
        }

      /* Timestamp: show the last member's timestamp */
      gint64 last_ts = gnalk_message_data_get_timestamp (last_member);
      g_autofree char *last_ts_str = format_timestamp (last_ts);
      gtk_label_set_text (GTK_LABEL (self->timestamp_label), last_ts_str);

      /* Store the leader's own file info for click handling (first file) */
      {
        JsonObject *leader_params = gnalk_message_data_get_message_parameters (data);
        if (leader_params && json_object_has_member (leader_params, "file"))
          {
            JsonNode *fn = json_object_get_member (leader_params, "file");
            if (fn && JSON_NODE_HOLDS_OBJECT (fn))
              {
                JsonObject *f = json_node_get_object (fn);
                gnalk_file_preview_set_file (self->file_preview,
                    json_object_get_string_member_with_default (f, "id", NULL),
                    json_object_get_string_member_with_default (f, "name", ""),
                    json_object_get_string_member_with_default (f, "mimetype", ""),
                    json_object_get_string_member_with_default (f, "link", ""),
                    json_object_get_string_member_with_default (f, "path", ""),
                    NULL);
              }
          }
      }
    }
  /* === Normal single-file rendering === */
  else if (!self->is_system)
    {
      JsonObject *params = gnalk_message_data_get_message_parameters (data);
      if (params && json_object_has_member (params, "file"))
        {
          JsonNode *file_node = json_object_get_member (params, "file");
          if (file_node && JSON_NODE_HOLDS_OBJECT (file_node))
            {
              JsonObject *file = json_node_get_object (file_node);
              const char *file_id = json_object_get_string_member_with_default (file, "id", NULL);
              const char *preview_available = json_object_get_string_member_with_default (file, "preview-available", "no");
              const char *mimetype = json_object_get_string_member_with_default (file, "mimetype", "");
              const char *file_link = json_object_get_string_member_with_default (file, "link", "");
              const char *file_name = json_object_get_string_member_with_default (file, "name", "");
              const char *file_path = json_object_get_string_member_with_default (file, "path", "");

              has_server_file_info = (file_id && *file_id);

              /* Check if this is an audio file */
              if (mimetype && g_str_has_prefix (mimetype, "audio/"))
                {
                  /* Show audio player instead of file preview */
                  gtk_widget_set_visible (self->audio_player, TRUE);

                  /* Store file info in file_preview widget for click handling */
                  gnalk_file_preview_set_file (self->file_preview, file_id, file_name, mimetype,
                                                file_link, file_path, NULL);

                  /* Check if file is already cached - if so, load waveform immediately */
                  const char *cache_base = g_get_user_cache_dir ();
                  g_autofree char *cache_dir = g_build_filename (cache_base, "gnalk", NULL);
                  const char *fname = (file_name && *file_name) ? file_name : "download";
                  g_autofree char *safe_name = g_strdup_printf ("%s_%s", file_id ? file_id : "unknown", fname);
                  g_autofree char *cache_path = g_build_filename (cache_dir, safe_name, NULL);

                  if (g_file_test (cache_path, G_FILE_TEST_EXISTS))
                    {
                      gnalk_audio_player_set_file (GNALK_AUDIO_PLAYER (self->audio_player), cache_path);
                    }
                }
              /* Check if this is a video file */
              else if (mimetype && g_str_has_prefix (mimetype, "video/"))
                {
                  /* Show video thumbnail instead of file preview */
                  gtk_widget_set_visible (self->video_thumbnail, TRUE);
                  gnalk_video_thumbnail_set_file_id (GNALK_VIDEO_THUMBNAIL (self->video_thumbnail), file_id);

                  /* Store file info in file_preview widget for click handling */
                  gnalk_file_preview_set_file (self->file_preview, file_id, file_name, mimetype,
                                                file_link, file_path, NULL);

                  /* Fetch server-generated preview thumbnail */
                  if (file_id && avatar_cache)
                    {
                      gnalk_video_thumbnail_set_loading (GNALK_VIDEO_THUMBNAIL (self->video_thumbnail), TRUE);
                      gnalk_avatar_cache_get_file_preview (avatar_cache, file_id, 280, 210,
                                                            on_video_thumbnail_loaded,
                                                            video_thumbnail_callback_data_new (self));
                    }
                  else
                    {
                      /* No file ID or cache - show placeholder */
                      gnalk_video_thumbnail_set_loading (GNALK_VIDEO_THUMBNAIL (self->video_thumbnail), FALSE);
                    }
                }
              else if (file_id && g_strcmp0 (preview_available, "yes") == 0 && avatar_cache)
                {
                  /* Use file preview widget - it handles loading internally */
                  gnalk_file_preview_set_file (self->file_preview, file_id, file_name, mimetype,
                                                file_link, file_path, avatar_cache);
                }
              else if (file_id)
                {
                  /* Store file info even if no preview available (for click handling) */
                  gnalk_file_preview_set_file (self->file_preview, file_id, file_name, mimetype,
                                                file_link, file_path, NULL);
                }
            }
        }
    }

  /* Handle upload preview for pending file uploads */
  const char *upload_file_path = gnalk_message_data_get_upload_file_path (data);
  gboolean is_uploading = gnalk_message_data_get_is_uploading (data);
  gboolean has_local_file = upload_file_path && *upload_file_path;

  /* Hide upload UI when we have server file info */
  if (has_server_file_info)
    {
      gnalk_upload_preview_reset (self->upload_preview);
    }

  if ((is_uploading || has_local_file) && !has_server_file_info)
    {
      const char *upload_file_name = gnalk_message_data_get_upload_file_name (data);
      const char *upload_mime_type = gnalk_message_data_get_upload_mime_type (data);

      /* Use upload preview widget - it handles display internally */
      gnalk_upload_preview_set_file (self->upload_preview, upload_file_path,
                                      upload_file_name, upload_mime_type, is_uploading);

      /* Hide message label if no caption; bubble stays visible for quote */
      const char *msg_text = gnalk_message_data_get_message (data);
      gboolean has_caption = msg_text && *msg_text;
      gboolean has_quote = gtk_widget_get_visible (GTK_WIDGET (self->quote_widget));
      gtk_widget_set_visible (self->message_label, has_caption);
      gtk_widget_set_visible (self->message_bubble, has_caption || has_quote);
    }
  else if (!gnalk_message_data_get_is_attachment_group_leader (data))
    {
      gnalk_upload_preview_reset (self->upload_preview);

      /* Hide message label for media-only file messages (message is just "{file}") */
      const char *raw_msg = gnalk_message_data_get_message (data);
      gboolean is_file_only = has_server_file_info && raw_msg && g_strcmp0 (raw_msg, "{file}") == 0;
      gboolean has_quote = gtk_widget_get_visible (GTK_WIDGET (self->quote_widget));
      gboolean has_visual_file = gtk_widget_get_visible (GTK_WIDGET (self->file_preview))
                                  || gtk_widget_get_visible (self->video_thumbnail);
      gtk_widget_set_visible (self->message_label, !is_file_only);
      /* Bubble always visible when file present (file_preview/video_thumbnail are inside it) */
      gtk_widget_set_visible (self->message_bubble, has_server_file_info || !is_file_only || has_quote);
      /* Zero-padding on bubble when it contains media (not audio — audio is outside) */
      if (has_visual_file)
        {
          gtk_widget_add_css_class (self->message_bubble, "message-media-bubble");
          gtk_box_set_spacing (GTK_BOX (self->message_bubble), 0);
        }
    }
  else
    {
      gnalk_upload_preview_reset (self->upload_preview);
    }

  /* Thread entry: show only on thread_created system messages.
   * Thread roots are filtered from the store by the processor. */
  const char *sys_msg_str = gnalk_message_data_get_system_message (data);
  gboolean is_thread_created_msg = sys_msg_str && g_strcmp0 (sys_msg_str, "thread_created") == 0;
  gboolean is_thread_entry = is_thread_created_msg && gnalk_message_data_get_is_thread (data);
  if (is_thread_entry)
    {
      gint replies = gnalk_message_data_get_thread_replies (data);
      g_autofree char *label = NULL;
      if (replies > 0)
        label = g_strdup_printf (ngettext ("View thread (%d reply)",
                                           "View thread (%d replies)", replies), replies);
      else
        label = g_strdup (_("View thread"));
      gtk_label_set_text (GTK_LABEL (self->thread_label), label);

      /* Title: "You created thread {title}" or "{name} created thread {title}" */
      const char *thread_title = gnalk_message_data_get_thread_title (data);
      if (thread_title && *thread_title)
        {
          g_autofree char *escaped_title = g_markup_escape_text (thread_title, -1);
          g_autofree char *title_markup = NULL;
          if (self->is_self)
            title_markup = g_strdup_printf (_("You created thread <b>%s</b>"), escaped_title);
          else
            {
              const char *name = gnalk_message_data_get_author_name (data);
              g_autofree char *escaped_name = g_markup_escape_text (name ? name : "", -1);
              title_markup = g_strdup_printf (_("%s created thread <b>%s</b>"), escaped_name, escaped_title);
            }
          gtk_label_set_markup (GTK_LABEL (self->thread_title_label), title_markup);
          gtk_widget_set_visible (self->thread_title_label, TRUE);
        }
      else
        {
          gtk_widget_set_visible (self->thread_title_label, FALSE);
        }

      /* Show first message as description */
      const char *first_msg = gnalk_message_data_get_thread_first_message (data);
      if (first_msg && *first_msg)
        {
          gtk_label_set_text (GTK_LABEL (self->thread_description_label), first_msg);
          gtk_widget_set_visible (self->thread_description_label, TRUE);
        }
      else
        {
          gtk_widget_set_visible (self->thread_description_label, FALSE);
        }

      gtk_widget_set_visible (self->thread_box, TRUE);

      /* Hide the message bubble — the thread entry widget replaces the
       * "{actor} started a thread" system text. */
      gtk_widget_set_visible (self->message_bubble, FALSE);

      /* Add CSS class to override system message transparency */
      gtk_widget_add_css_class (GTK_WIDGET (self), "thread-created-row");
    }
  else
    {
      gtk_widget_set_visible (self->thread_box, FALSE);
      gtk_widget_remove_css_class (GTK_WIDGET (self), "thread-created-row");
    }

  /* Connect to data's property changes for live updates */
  if (self->bound_data)
    {
      g_signal_handler_disconnect (self->bound_data, self->unread_handler_id);
      g_clear_object (&self->bound_data);
    }
  self->bound_data = g_object_ref (data);
  self->unread_handler_id = g_signal_connect (data, "notify",
                                               G_CALLBACK (on_bound_data_notify), self);
}

void
gnalk_message_row_reset (GnalkMessageRow *self)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));

  /* Disconnect from data signals */
  if (self->bound_data)
    {
      g_signal_handler_disconnect (self->bound_data, self->unread_handler_id);
      g_clear_object (&self->bound_data);
      self->unread_handler_id = 0;
    }

  /* Restore visibility (may have been hidden as attachment group member) */
  gtk_widget_set_visible (GTK_WIDGET (self), TRUE);

  /* Clear unread CSS classes */
  gtk_widget_remove_css_class (GTK_WIDGET (self), "message-unread");
  gtk_widget_remove_css_class (GTK_WIDGET (self), "message-marking-read");

  /* Clear all dynamic data */
  self->message_id = 0;
  self->message_timestamp = 0;
  self->parent_message_id = 0;
  self->status = GNALK_MESSAGE_STATUS_NONE;
  self->is_self = FALSE;
  self->is_system = FALSE;

  g_clear_pointer (&self->author_name, g_free);
  g_clear_pointer (&self->author_id, g_free);
  g_clear_pointer (&self->message, g_free);
  g_clear_pointer (&self->timestamp, g_free);
  g_clear_pointer (&self->reference_id, g_free);

  /* Reset visual elements */
  gtk_label_set_text (GTK_LABEL (self->username_label), "");
  gtk_label_set_text (GTK_LABEL (self->message_label), "");
  gtk_label_set_text (GTK_LABEL (self->timestamp_label), "");
  gtk_label_set_text (GTK_LABEL (self->reactions_label), "");

  gtk_widget_remove_css_class (GTK_WIDGET (self), "message-self");
  gtk_widget_remove_css_class (GTK_WIDGET (self), "message-other");
  gtk_widget_remove_css_class (GTK_WIDGET (self), "message-system");
  gtk_widget_remove_css_class (GTK_WIDGET (self), "message-grouped");
  gtk_widget_remove_css_class (GTK_WIDGET (self), "thread-created-row");
  gtk_widget_remove_css_class (self->message_label, "message-emoji-only");
  gtk_widget_remove_css_class (self->message_bubble, "message-media-bubble");
  gtk_box_set_spacing (GTK_BOX (self->message_bubble), 4);

  /* Reset timestamp visibility */
  gtk_widget_set_visible (self->timestamp_label, TRUE);

  gtk_widget_set_visible (self->reactions_label, FALSE);
  gtk_widget_set_visible (self->edited_label, FALSE);
  gnalk_quote_widget_reset (self->quote_widget);
  gtk_widget_set_visible (self->message_label, TRUE);
  gtk_widget_set_visible (self->message_bubble, TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->date_separator), FALSE);
  gtk_widget_set_visible (self->status_icon, FALSE);

  /* Reset thread entry */
  gtk_widget_set_visible (self->thread_box, FALSE);
  gtk_widget_set_visible (self->thread_title_label, FALSE);
  gtk_widget_set_visible (self->thread_description_label, FALSE);

  /* Reset file preview widget */
  gnalk_file_preview_reset (self->file_preview);

  /* Reset grouped file flow box and pool items */
  gtk_widget_set_visible (self->file_group_flow, FALSE);
  for (int i = 0; i < 12; i++)
    {
      gnalk_file_preview_reset (self->group_file_pool[i]);
      gtk_widget_set_visible (gtk_widget_get_parent (GTK_WIDGET (self->group_file_pool[i])), FALSE);
    }

  /* Reset audio player */
  gnalk_audio_player_stop (GNALK_AUDIO_PLAYER (self->audio_player));
  gtk_widget_set_visible (self->audio_player, FALSE);

  /* Reset video thumbnail */
  gnalk_video_thumbnail_set_file (GNALK_VIDEO_THUMBNAIL (self->video_thumbnail), NULL);
  gtk_widget_set_visible (self->video_thumbnail, FALSE);

  /* Reset upload preview widget */
  gnalk_upload_preview_reset (self->upload_preview);

  /* Reset avatar to default */
  gtk_image_set_from_icon_name (GTK_IMAGE (self->avatar), "avatar-default-symbolic");
}

void
gnalk_message_row_set_downloading (GnalkMessageRow *self,
                                     gboolean          downloading)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));

  gnalk_file_preview_set_downloading (self->file_preview, downloading);
}

void
gnalk_message_row_set_file_cached (GnalkMessageRow *self,
                                     gboolean          cached)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));

  /* Skip cache overlay for inline images */
  const char *mimetype = gnalk_file_preview_get_file_mimetype (self->file_preview);
  if (mimetype && g_str_has_prefix (mimetype, "image/"))
    return;

  gnalk_file_preview_set_cached (self->file_preview, cached);
}

const char *
gnalk_message_row_get_file_id (GnalkMessageRow *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_ROW (self), NULL);
  return gnalk_file_preview_get_file_id (self->file_preview);
}

const char *
gnalk_message_row_get_file_name (GnalkMessageRow *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_ROW (self), NULL);
  return gnalk_file_preview_get_file_name (self->file_preview);
}

const char *
gnalk_message_row_get_file_mimetype (GnalkMessageRow *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_ROW (self), NULL);
  return gnalk_file_preview_get_file_mimetype (self->file_preview);
}

gboolean
gnalk_message_row_has_audio_file (GnalkMessageRow *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_ROW (self), FALSE);
  const char *mimetype = gnalk_file_preview_get_file_mimetype (self->file_preview);
  return mimetype && g_str_has_prefix (mimetype, "audio/");
}

void
gnalk_message_row_set_audio_file (GnalkMessageRow *self,
                                    const char       *file_path)
{
  g_return_if_fail (GNALK_IS_AUDIO_PLAYER (self->audio_player));

  gnalk_audio_player_set_file (GNALK_AUDIO_PLAYER (self->audio_player), file_path);
}

void
gnalk_message_row_set_audio_loading (GnalkMessageRow *self,
                                       gboolean          loading)
{
  g_return_if_fail (GNALK_IS_AUDIO_PLAYER (self->audio_player));

  gnalk_audio_player_set_loading (GNALK_AUDIO_PLAYER (self->audio_player), loading);
}

gboolean
gnalk_message_row_has_video_file (GnalkMessageRow *self)
{
  g_return_val_if_fail (GNALK_IS_MESSAGE_ROW (self), FALSE);
  const char *mimetype = gnalk_file_preview_get_file_mimetype (self->file_preview);
  return mimetype && g_str_has_prefix (mimetype, "video/");
}

void
gnalk_message_row_set_video_file (GnalkMessageRow *self,
                                    const char       *file_path)
{
  g_return_if_fail (GNALK_IS_VIDEO_THUMBNAIL (self->video_thumbnail));

  gnalk_video_thumbnail_set_file (GNALK_VIDEO_THUMBNAIL (self->video_thumbnail), file_path);
}

void
gnalk_message_row_set_upload_progress (GnalkMessageRow *self,
                                         double            progress)
{
  /* With spinner-based UI, we don't show numeric progress - just keep spinner running */
  (void) self;
  (void) progress;
}

void
gnalk_message_row_set_upload_complete (GnalkMessageRow *self)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));

  /* Use upload preview widget to show complete state */
  gnalk_upload_preview_set_complete (self->upload_preview);

  /* Update status to sent */
  self->status = GNALK_MESSAGE_STATUS_SENT;
  update_status_icon (self);
}

void
gnalk_message_row_set_edit_timeout (GnalkMessageRow *self,
                                      gint64            timeout)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));
  self->edit_timeout = timeout;
}

void
gnalk_message_row_set_delete_timeout (GnalkMessageRow *self,
                                        gint64            timeout)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));
  self->delete_timeout = timeout;
}

void
gnalk_message_row_set_unread (GnalkMessageRow *self,
                                gboolean          is_unread)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));

  GtkWidget *widget = GTK_WIDGET (self);

  if (is_unread)
    {
      gtk_widget_remove_css_class (widget, "message-marking-read");
      gtk_widget_add_css_class (widget, "message-unread");
    }
  else
    {
      /* Transition through marking-read class for fade animation */
      if (gtk_widget_has_css_class (widget, "message-unread"))
        {
          gtk_widget_remove_css_class (widget, "message-unread");
          gtk_widget_add_css_class (widget, "message-marking-read");
        }
    }
}

void
gnalk_message_row_set_show_timestamp (GnalkMessageRow *self,
                                        gboolean          show)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));

  gtk_widget_set_visible (self->timestamp_label, show);

  /* Add CSS class for reduced spacing when timestamp is hidden */
  if (show)
    gtk_widget_remove_css_class (GTK_WIDGET (self), "message-grouped");
  else
    gtk_widget_add_css_class (GTK_WIDGET (self), "message-grouped");
}

void
gnalk_message_row_set_threads_supported (GnalkMessageRow *self,
                                         gboolean          supported)
{
  g_return_if_fail (GNALK_IS_MESSAGE_ROW (self));
  self->threads_supported = supported;
}
