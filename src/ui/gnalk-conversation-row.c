#include "gnalk-conversation-row.h"
#include "gnalk-markdown.h"
#include "gnalk-session.h"
#include "gnalk-account.h"
#include "gnalk-message-utils.h"
#include <glib/gi18n.h>

struct _GnalkConversationRow
{
  GtkBox parent_instance;

  GtkWidget *avatar;
  GtkWidget *title_label;
  GtkWidget *subtitle_label;
  GtkWidget *sent_status_icon;
  GtkWidget *status_icon;
  GtkWidget *federation_icon;
  GtkWidget *unread_badge;

  /* Context menu */
  GtkWidget *context_popover;
  GtkWidget *leave_button;
  GtkWidget *delete_button;
  GtkWidget *favorite_button;

  char  *token;
  char  *title;
  char  *name;  /* For 1:1 conversations, this is the user ID */
  char  *subtitle;
  char  *user_status;
  char  *actor_id;  /* User ID for 1:1 conversations (for avatar loading) */
  gint64 last_activity;
  gint64 last_common_read_message;
  gint64 last_message_id;
  gint   unread_messages;
  gint   room_type;
  gboolean is_active;
  gboolean is_favorite;
  gboolean can_leave;
  gboolean can_delete;
  gboolean last_message_is_own;

  /* Bound data for in-place updates */
  GnalkConversationData *bound_data;
  gulong                  updated_handler_id;
  GnalkAvatarCache      *avatar_cache;  /* unowned, for avatar re-fetch on data updates */
};

G_DEFINE_FINAL_TYPE (GnalkConversationRow, gnalk_conversation_row, GTK_TYPE_BOX)

enum {
  PROP_0,
  PROP_TITLE,
  PROP_SUBTITLE,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

enum {
  SIGNAL_LEAVE_REQUESTED,
  SIGNAL_DELETE_REQUESTED,
  SIGNAL_FAVORITE_TOGGLED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gnalk_conversation_row_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  GnalkConversationRow *self = GNALK_CONVERSATION_ROW (object);

  switch (prop_id)
    {
    case PROP_TITLE:
      g_value_set_string (value, self->title);
      break;
    case PROP_SUBTITLE:
      g_value_set_string (value, self->subtitle);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gnalk_conversation_row_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  GnalkConversationRow *self = GNALK_CONVERSATION_ROW (object);

  switch (prop_id)
    {
    case PROP_TITLE:
      g_free (self->title);
      self->title = g_value_dup_string (value);
      break;
    case PROP_SUBTITLE:
      g_free (self->subtitle);
      self->subtitle = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gnalk_conversation_row_dispose (GObject *object)
{
  GnalkConversationRow *self = GNALK_CONVERSATION_ROW (object);

  /* Disconnect from bound data if present */
  if (self->bound_data && self->updated_handler_id > 0)
    {
      g_signal_handler_disconnect (self->bound_data, self->updated_handler_id);
      self->updated_handler_id = 0;
    }
  g_clear_object (&self->bound_data);

  g_clear_pointer (&self->context_popover, gtk_widget_unparent);

  G_OBJECT_CLASS (gnalk_conversation_row_parent_class)->dispose (object);
}

static void
gnalk_conversation_row_finalize (GObject *object)
{
  GnalkConversationRow *self = GNALK_CONVERSATION_ROW (object);

  g_free (self->token);
  g_free (self->title);
  g_free (self->name);
  g_free (self->subtitle);
  g_free (self->user_status);
  g_free (self->actor_id);

  G_OBJECT_CLASS (gnalk_conversation_row_parent_class)->finalize (object);
}

static void
gnalk_conversation_row_class_init (GnalkConversationRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = gnalk_conversation_row_get_property;
  object_class->set_property = gnalk_conversation_row_set_property;
  object_class->dispose = gnalk_conversation_row_dispose;
  object_class->finalize = gnalk_conversation_row_finalize;

  properties[PROP_TITLE] =
    g_param_spec_string ("title", NULL, NULL, "",
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_SUBTITLE] =
    g_param_spec_string ("subtitle", NULL, NULL, "",
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  signals[SIGNAL_LEAVE_REQUESTED] =
    g_signal_new ("leave-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_DELETE_REQUESTED] =
    g_signal_new ("delete-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_FAVORITE_TOGGLED] =
    g_signal_new ("favorite-toggled",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-conversation-row.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationRow, avatar);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationRow, title_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationRow, subtitle_label);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationRow, sent_status_icon);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationRow, status_icon);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationRow, federation_icon);
  gtk_widget_class_bind_template_child (widget_class, GnalkConversationRow, unread_badge);
}

static void update_status_indicator (GnalkConversationRow *self);

/* Forward declarations for avatar re-fetch in on_data_updated */
typedef struct { GWeakRef weak_ref; } AvatarCallbackData;
static void on_avatar_loaded (GnalkAvatarCache *cache, GdkTexture *texture, gpointer user_data);

/* Helper to get current user ID from conversation data's session */
static const char *
get_current_user_id (GnalkConversationData *data)
{
  GnalkSession *session = gnalk_conversation_data_get_session (data);
  if (session)
    {
      GnalkAccount *account = gnalk_session_get_account (session);
      if (account)
        return gnalk_account_get_user_id (account);
    }
  return NULL;
}

/* Callback for in-place data updates */
static void
on_data_updated (GnalkConversationData *data,
                 GnalkConversationRow  *self)
{
  /* Get current user ID for subtitle formatting */
  const char *current_user_id = get_current_user_id (data);

  /* Re-read values from data and update UI */
  g_free (self->title);
  self->title = g_strdup (gnalk_conversation_data_get_title (data));
  gtk_label_set_text (GTK_LABEL (self->title_label), self->title ? self->title : "");

  g_free (self->subtitle);
  self->subtitle = gnalk_conversation_data_get_subtitle (data, current_user_id);
  if (self->subtitle && *self->subtitle)
    {
      g_autofree char *markup = gnalk_markdown_to_pango (self->subtitle, NULL);
      gtk_label_set_markup (GTK_LABEL (self->subtitle_label), markup);
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (self->subtitle_label), "");
    }

  g_free (self->user_status);
  self->user_status = g_strdup (gnalk_conversation_data_get_user_status (data));

  self->last_activity = gnalk_conversation_data_get_last_activity (data);
  self->unread_messages = gnalk_conversation_data_get_unread_messages (data);
  self->is_favorite = gnalk_conversation_data_get_is_favorite (data);
  self->last_message_id = gnalk_conversation_data_get_last_message_id (data);
  self->last_message_is_own = gnalk_conversation_data_get_last_message_is_own (data, current_user_id);

  /* Update sent status icon - show only if last message is from current user */
  gtk_widget_set_visible (self->sent_status_icon, self->last_message_is_own);

  /* Update unread badge */
  if (self->unread_messages > 0)
    {
      gtk_widget_add_css_class (GTK_WIDGET (self), "conversation-unread");
      g_autofree char *badge_text = g_strdup_printf ("%d", MIN (self->unread_messages, 99));
      gtk_label_set_text (GTK_LABEL (self->unread_badge), badge_text);
      gtk_widget_set_visible (self->unread_badge, TRUE);
    }
  else
    {
      gtk_widget_remove_css_class (GTK_WIDGET (self), "conversation-unread");
      gtk_widget_set_visible (self->unread_badge, FALSE);
    }

  /* Update status indicator */
  update_status_indicator (self);

  /* Update federation indicator */
  gtk_widget_set_visible (self->federation_icon,
                          gnalk_conversation_data_is_federated (data));

  /* Re-fetch room avatar (cache-hit is fast; actual fetch only after invalidation) */
  if (self->avatar_cache && self->token && self->room_type != 1)
    {
      AvatarCallbackData *cb_data = g_new0 (AvatarCallbackData, 1);
      g_weak_ref_init (&cb_data->weak_ref, self);
      gnalk_avatar_cache_get_room_avatar (self->avatar_cache, self->token,
                                             on_avatar_loaded, cb_data);
    }
}

static void
update_status_indicator (GnalkConversationRow *self)
{
  /* Only show status for 1:1 conversations (type 1) */
  if (self->room_type != 1 || !self->user_status || !*self->user_status)
    {
      gtk_widget_set_visible (self->status_icon, FALSE);
      return;
    }

  /* Remove previous status classes */
  gtk_widget_remove_css_class (self->status_icon, "status-online");
  gtk_widget_remove_css_class (self->status_icon, "status-away");
  gtk_widget_remove_css_class (self->status_icon, "status-dnd");

  if (g_strcmp0 (self->user_status, "online") == 0)
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (self->status_icon), "user-available-symbolic");
      gtk_widget_add_css_class (self->status_icon, "status-online");
      gtk_widget_set_visible (self->status_icon, TRUE);
    }
  else if (g_strcmp0 (self->user_status, "away") == 0)
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (self->status_icon), "user-away-symbolic");
      gtk_widget_add_css_class (self->status_icon, "status-away");
      gtk_widget_set_visible (self->status_icon, TRUE);
    }
  else if (g_strcmp0 (self->user_status, "dnd") == 0 ||
           g_strcmp0 (self->user_status, "busy") == 0)
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (self->status_icon), "user-busy-symbolic");
      gtk_widget_add_css_class (self->status_icon, "status-dnd");
      gtk_widget_set_visible (self->status_icon, TRUE);
    }
  else
    {
      /* offline or unknown - hide indicator */
      gtk_widget_set_visible (self->status_icon, FALSE);
    }
}

/* Context menu button callbacks */
static void
on_leave_clicked (GtkButton *button, GnalkConversationRow *self)
{
  gtk_popover_popdown (GTK_POPOVER (self->context_popover));
  if (self->token)
    g_signal_emit (self, signals[SIGNAL_LEAVE_REQUESTED], 0, self->token);
}

static void
on_delete_clicked (GtkButton *button, GnalkConversationRow *self)
{
  gtk_popover_popdown (GTK_POPOVER (self->context_popover));
  if (self->token)
    g_signal_emit (self, signals[SIGNAL_DELETE_REQUESTED], 0, self->token);
}

static void
on_favorite_clicked (GtkButton *button, GnalkConversationRow *self)
{
  gtk_popover_popdown (GTK_POPOVER (self->context_popover));
  if (self->token)
    {
      /* Toggle favorite state */
      gboolean new_favorite = !self->is_favorite;
      g_signal_emit (self, signals[SIGNAL_FAVORITE_TOGGLED], 0, self->token, new_favorite);
    }
}

static void
show_context_menu (GnalkConversationRow *self,
                   GtkGesture            *gesture,
                   double                 x,
                   double                 y)
{
  /* Claim the sequence */
  gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_CLAIMED);

  /* Update favorite button label based on current state */
  if (self->is_favorite)
    gtk_button_set_label (GTK_BUTTON (self->favorite_button), _("Remove from favorites"));
  else
    gtk_button_set_label (GTK_BUTTON (self->favorite_button), _("Add to favorites"));

  /* Enable/disable buttons based on permissions */
  gtk_widget_set_sensitive (self->leave_button, self->can_leave);
  gtk_widget_set_sensitive (self->delete_button, self->can_delete);

  /* Position and show the popover */
  GdkRectangle rect = { (int) x, (int) y, 1, 1 };
  gtk_popover_set_pointing_to (GTK_POPOVER (self->context_popover), &rect);

  g_message ("ConversationRow: showing popover, parent=%p, popover=%p",
             (void *) gtk_widget_get_parent (self->context_popover),
             (void *) self->context_popover);

  gtk_popover_popup (GTK_POPOVER (self->context_popover));
}

static void
on_right_click (GtkGestureClick *gesture,
                int              n_press,
                double           x,
                double           y,
                gpointer         user_data)
{
  (void) n_press;
  GnalkConversationRow *self = GNALK_CONVERSATION_ROW (user_data);

  g_message ("ConversationRow: right-click at (%.0f, %.0f) on %s", x, y,
             self->token ? self->token : "(no token)");

  /* Claim the gesture to prevent the list view from activating the row */
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  show_context_menu (self, GTK_GESTURE (gesture), x, y);
}

static void
on_long_press (GtkGestureLongPress *gesture,
               double               x,
               double               y,
               gpointer             user_data)
{
  GnalkConversationRow *self = GNALK_CONVERSATION_ROW (user_data);

  /* Claim the gesture to prevent the list view from activating the row */
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  show_context_menu (self, GTK_GESTURE (gesture), x, y);
}

static void
gnalk_conversation_row_init (GnalkConversationRow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  /* Create context menu popover */
  self->context_popover = gtk_popover_new ();
  gtk_widget_set_parent (self->context_popover, GTK_WIDGET (self));
  gtk_popover_set_has_arrow (GTK_POPOVER (self->context_popover), TRUE);

  GtkWidget *menu_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (menu_box, "context-menu");

  /* Favorite button */
  self->favorite_button = gtk_button_new_with_label (_("Add to favorites"));
  gtk_button_set_has_frame (GTK_BUTTON (self->favorite_button), FALSE);
  gtk_widget_add_css_class (self->favorite_button, "flat");
  g_signal_connect (self->favorite_button, "clicked", G_CALLBACK (on_favorite_clicked), self);
  gtk_box_append (GTK_BOX (menu_box), self->favorite_button);

  /* Leave button */
  self->leave_button = gtk_button_new_with_label (_("Leave conversation"));
  gtk_button_set_has_frame (GTK_BUTTON (self->leave_button), FALSE);
  gtk_widget_add_css_class (self->leave_button, "flat");
  g_signal_connect (self->leave_button, "clicked", G_CALLBACK (on_leave_clicked), self);
  gtk_box_append (GTK_BOX (menu_box), self->leave_button);

  /* Delete button */
  self->delete_button = gtk_button_new_with_label (_("Delete conversation"));
  gtk_button_set_has_frame (GTK_BUTTON (self->delete_button), FALSE);
  gtk_widget_add_css_class (self->delete_button, "flat");
  gtk_widget_add_css_class (self->delete_button, "error");
  g_signal_connect (self->delete_button, "clicked", G_CALLBACK (on_delete_clicked), self);
  gtk_box_append (GTK_BOX (menu_box), self->delete_button);

  gtk_popover_set_child (GTK_POPOVER (self->context_popover), menu_box);

  /* Add right-click gesture */
  GtkGesture *right_click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (right_click), GDK_BUTTON_SECONDARY);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (right_click), GTK_PHASE_CAPTURE);
  g_signal_connect (right_click, "pressed", G_CALLBACK (on_right_click), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (right_click));

  /* Add long press gesture for touch devices */
  GtkGesture *long_press = gtk_gesture_long_press_new ();
  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (long_press), TRUE);
  g_signal_connect (long_press, "pressed", G_CALLBACK (on_long_press), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (long_press));
}

GnalkConversationRow *
gnalk_conversation_row_new (void)
{
  return g_object_new (GNALK_TYPE_CONVERSATION_ROW, NULL);
}

static void
on_avatar_loaded (GnalkAvatarCache *cache,
                  GdkTexture        *texture,
                  gpointer           user_data)
{
  AvatarCallbackData *data = user_data;
  GnalkConversationRow *self = g_weak_ref_get (&data->weak_ref);

  if (self)
    {
      if (texture)
        gtk_image_set_from_paintable (GTK_IMAGE (self->avatar), GDK_PAINTABLE (texture));
      else
        gtk_image_set_from_icon_name (GTK_IMAGE (self->avatar), "avatar-default-symbolic");
      g_object_unref (self);
    }

  g_weak_ref_clear (&data->weak_ref);
  g_free (data);
}

void
gnalk_conversation_row_set_data (GnalkConversationRow *self,
                                   JsonObject            *room_data,
                                   GnalkAvatarCache     *avatar_cache)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_ROW (self));
  g_return_if_fail (room_data != NULL);

  g_free (self->token);
  g_free (self->title);
  g_free (self->name);
  g_free (self->subtitle);
  g_free (self->user_status);
  g_free (self->actor_id);

  self->token = g_strdup (json_object_get_string_member_with_default (room_data, "token", ""));
  self->title = g_strdup (json_object_get_string_member_with_default (room_data, "displayName", ""));
  self->name = g_strdup (json_object_get_string_member_with_default (room_data, "name", ""));
  self->room_type = (gint) json_object_get_int_member_with_default (room_data, "type", 0);

  /* Get user status for 1:1 conversations */
  self->user_status = g_strdup (json_object_get_string_member_with_default (room_data, "status", ""));

  /* Get actorId for user avatar loading (from search results) */
  self->actor_id = g_strdup (json_object_get_string_member_with_default (room_data, "actorId", ""));

  /* Note: This function doesn't have access to session, so we can't determine
   * if the last message is from the current user. Only used for legacy code paths. */
  self->last_message_is_own = FALSE;
  self->last_message_id = 0;

  JsonNode *last_message_node = json_object_get_member (room_data, "lastMessage");
  if (last_message_node && JSON_NODE_HOLDS_OBJECT (last_message_node))
    {
      JsonObject *last_message = json_node_get_object (last_message_node);
      const char *message_text = json_object_get_string_member_with_default (last_message, "message", "");
      const char *actor_name = json_object_get_string_member_with_default (last_message, "actorDisplayName", "");

      /* Extract message parameters if present */
      JsonObject *params = NULL;
      if (json_object_has_member (last_message, "messageParameters"))
        {
          JsonNode *params_node = json_object_get_member (last_message, "messageParameters");
          if (params_node && JSON_NODE_HOLDS_OBJECT (params_node))
            params = json_node_get_object (params_node);
        }

      g_autofree char *processed = gnalk_message_utils_replace_parameters (message_text, params, actor_name);

      /* Extract only the first line */
      const char *newline = strchr (processed, '\n');
      if (newline)
        self->subtitle = g_strndup (processed, newline - processed);
      else
        self->subtitle = g_strdup (processed);
    }
  else
    self->subtitle = g_strdup ("");

  self->last_activity = json_object_get_int_member_with_default (room_data, "lastActivity", 0);
  self->last_common_read_message = json_object_get_int_member_with_default (room_data, "lastCommonReadMessage", 0);
  self->unread_messages = (gint) json_object_get_int_member_with_default (room_data, "unreadMessages", 0);

  gtk_label_set_text (GTK_LABEL (self->title_label), self->title ? self->title : "");
  if (self->subtitle && *self->subtitle)
    {
      g_autofree char *markup = gnalk_markdown_to_pango (self->subtitle, NULL);
      gtk_label_set_markup (GTK_LABEL (self->subtitle_label), markup);
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (self->subtitle_label), "");
    }

  /* Hide sent status icon (no session to determine ownership) */
  gtk_widget_set_visible (self->sent_status_icon, FALSE);

  /* Apply/remove unread CSS class and update badge */
  if (self->unread_messages > 0)
    {
      gtk_widget_add_css_class (GTK_WIDGET (self), "conversation-unread");
      g_autofree char *badge_text = g_strdup_printf ("%d", self->unread_messages > 99 ? 99 : self->unread_messages);
      gtk_label_set_text (GTK_LABEL (self->unread_badge), badge_text);
      gtk_widget_set_visible (self->unread_badge, TRUE);
    }
  else
    {
      gtk_widget_remove_css_class (GTK_WIDGET (self), "conversation-unread");
      gtk_widget_set_visible (self->unread_badge, FALSE);
    }

  /* Update status indicator */
  update_status_indicator (self);

  if (avatar_cache)
    {
      AvatarCallbackData *cb_data = g_new0 (AvatarCallbackData, 1);
      g_weak_ref_init (&cb_data->weak_ref, self);
      gnalk_avatar_cache_get_room_avatar (avatar_cache, self->token, on_avatar_loaded, cb_data);
    }
}

const char *
gnalk_conversation_row_get_token (GnalkConversationRow *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_ROW (self), NULL);
  return self->token;
}

const char *
gnalk_conversation_row_get_title (GnalkConversationRow *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_ROW (self), NULL);
  return self->title;
}

const char *
gnalk_conversation_row_get_name (GnalkConversationRow *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_ROW (self), NULL);
  return self->name;
}

const char *
gnalk_conversation_row_get_user_status (GnalkConversationRow *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_ROW (self), NULL);
  return self->user_status;
}

gint
gnalk_conversation_row_get_room_type (GnalkConversationRow *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_ROW (self), 0);
  return self->room_type;
}

gint64
gnalk_conversation_row_get_last_activity (GnalkConversationRow *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_ROW (self), 0);
  return self->last_activity;
}

gint64
gnalk_conversation_row_get_last_common_read_message (GnalkConversationRow *self)
{
  g_return_val_if_fail (GNALK_IS_CONVERSATION_ROW (self), 0);
  return self->last_common_read_message;
}

void
gnalk_conversation_row_set_active (GnalkConversationRow *self,
                                     gboolean               active)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_ROW (self));

  if (self->is_active == active)
    return;

  self->is_active = active;

  if (active)
    gtk_widget_add_css_class (GTK_WIDGET (self), "conversation-active");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "conversation-active");
}

void
gnalk_conversation_row_set_from_data (GnalkConversationRow  *self,
                                        GnalkConversationData *data,
                                        GnalkAvatarCache      *avatar_cache)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_ROW (self));
  g_return_if_fail (GNALK_IS_CONVERSATION_DATA (data));

  /* Disconnect from previous bound data */
  if (self->bound_data && self->updated_handler_id > 0)
    {
      g_signal_handler_disconnect (self->bound_data, self->updated_handler_id);
      self->updated_handler_id = 0;
    }
  g_clear_object (&self->bound_data);

  /* Connect to updated signal for in-place updates */
  self->bound_data = g_object_ref (data);
  self->avatar_cache = avatar_cache;  /* unowned, for avatar re-fetch on data updates */
  self->updated_handler_id = g_signal_connect (data, "updated",
                                                G_CALLBACK (on_data_updated), self);

  /* Get current user ID for subtitle formatting */
  const char *current_user_id = get_current_user_id (data);

  g_free (self->token);
  g_free (self->title);
  g_free (self->name);
  g_free (self->subtitle);
  g_free (self->user_status);
  g_free (self->actor_id);

  self->token = g_strdup (gnalk_conversation_data_get_token (data));
  self->title = g_strdup (gnalk_conversation_data_get_title (data));
  self->name = g_strdup (gnalk_conversation_data_get_name (data));
  self->subtitle = gnalk_conversation_data_get_subtitle (data, current_user_id);
  self->user_status = g_strdup (gnalk_conversation_data_get_user_status (data));
  self->actor_id = g_strdup (gnalk_conversation_data_get_actor_id (data));
  self->room_type = gnalk_conversation_data_get_room_type (data);
  self->last_activity = gnalk_conversation_data_get_last_activity (data);
  self->last_common_read_message = gnalk_conversation_data_get_last_common_read_message (data);
  self->unread_messages = gnalk_conversation_data_get_unread_messages (data);
  self->is_favorite = gnalk_conversation_data_get_is_favorite (data);
  self->can_leave = gnalk_conversation_data_get_can_leave (data);
  self->can_delete = gnalk_conversation_data_get_can_delete (data);
  self->last_message_id = gnalk_conversation_data_get_last_message_id (data);
  self->last_message_is_own = gnalk_conversation_data_get_last_message_is_own (data, current_user_id);

  gtk_label_set_text (GTK_LABEL (self->title_label), self->title ? self->title : "");
  if (self->subtitle && *self->subtitle)
    {
      g_autofree char *markup = gnalk_markdown_to_pango (self->subtitle, NULL);
      gtk_label_set_markup (GTK_LABEL (self->subtitle_label), markup);
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (self->subtitle_label), "");
    }

  /* Update sent status icon - show only if last message is from current user */
  gtk_widget_set_visible (self->sent_status_icon, self->last_message_is_own);

  /* Apply/remove unread CSS class and update badge */
  if (self->unread_messages > 0)
    {
      gtk_widget_add_css_class (GTK_WIDGET (self), "conversation-unread");
      g_autofree char *badge_text = g_strdup_printf ("%d", self->unread_messages > 99 ? 99 : self->unread_messages);
      gtk_label_set_text (GTK_LABEL (self->unread_badge), badge_text);
      gtk_widget_set_visible (self->unread_badge, TRUE);
    }
  else
    {
      gtk_widget_remove_css_class (GTK_WIDGET (self), "conversation-unread");
      gtk_widget_set_visible (self->unread_badge, FALSE);
    }

  /* Update status indicator */
  update_status_indicator (self);

  /* Update federation indicator */
  gtk_widget_set_visible (self->federation_icon,
                          gnalk_conversation_data_is_federated (data));

  if (avatar_cache)
    {
      AvatarCallbackData *cb_data = g_new0 (AvatarCallbackData, 1);
      g_weak_ref_init (&cb_data->weak_ref, self);

      /* For search results (fake token starting with "user:"), use user avatar */
      if (self->token && g_str_has_prefix (self->token, "user:") && self->actor_id && *self->actor_id)
        gnalk_avatar_cache_get_user_avatar (avatar_cache, self->actor_id, 64, on_avatar_loaded, cb_data);
      else
        gnalk_avatar_cache_get_room_avatar (avatar_cache, self->token, on_avatar_loaded, cb_data);
    }
}

void
gnalk_conversation_row_reset (GnalkConversationRow *self)
{
  g_return_if_fail (GNALK_IS_CONVERSATION_ROW (self));

  /* Disconnect from bound data */
  if (self->bound_data && self->updated_handler_id > 0)
    {
      g_signal_handler_disconnect (self->bound_data, self->updated_handler_id);
      self->updated_handler_id = 0;
    }
  g_clear_object (&self->bound_data);

  g_clear_pointer (&self->token, g_free);
  g_clear_pointer (&self->title, g_free);
  g_clear_pointer (&self->name, g_free);
  g_clear_pointer (&self->subtitle, g_free);
  g_clear_pointer (&self->user_status, g_free);
  g_clear_pointer (&self->actor_id, g_free);

  self->last_activity = 0;
  self->last_common_read_message = 0;
  self->last_message_id = 0;
  self->unread_messages = 0;
  self->room_type = 0;
  self->is_active = FALSE;
  self->last_message_is_own = FALSE;

  gtk_label_set_text (GTK_LABEL (self->title_label), "");
  gtk_label_set_text (GTK_LABEL (self->subtitle_label), "");
  gtk_widget_set_visible (self->unread_badge, FALSE);
  gtk_widget_set_visible (self->sent_status_icon, FALSE);
  gtk_widget_set_visible (self->status_icon, FALSE);
  gtk_widget_set_visible (self->federation_icon, FALSE);
  gtk_widget_remove_css_class (GTK_WIDGET (self), "conversation-unread");
  gtk_widget_remove_css_class (GTK_WIDGET (self), "conversation-active");
  gtk_image_set_from_icon_name (GTK_IMAGE (self->avatar), "avatar-default-symbolic");
}
