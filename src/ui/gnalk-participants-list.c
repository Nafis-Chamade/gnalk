#include "gnalk-participants-list.h"

struct _GnalkParticipantsList
{
  GtkBox parent_instance;

  GtkWidget *revealer;
  GtkWidget *participants_box;

  /* Context menu */
  GtkWidget *context_popover;
  GtkWidget *chat_button;
  GtkWidget *promote_button;
  GtkWidget *demote_button;
  GtkWidget *remove_button;

  /* Context for current action */
  char   *context_actor_id;
  gint64  context_attendee_id;
  gint    context_participant_type;

  /* Caller's permissions */
  char   *my_user_id;
  gint    my_participant_type;
  gint    room_type;
  gboolean is_online;
};

G_DEFINE_FINAL_TYPE (GnalkParticipantsList, gnalk_participants_list, GTK_TYPE_BOX)

enum
{
  SIGNAL_PARTICIPANT_CLICKED,
  SIGNAL_CHAT_REQUESTED,
  SIGNAL_PROMOTE_REQUESTED,
  SIGNAL_DEMOTE_REQUESTED,
  SIGNAL_REMOVE_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void update_participant_status_icon (GtkWidget *item, const char *status);

/* Update call icon for a participant item */
static void
update_participant_call_icon (GtkWidget *item, gint64 in_call)
{
  GtkWidget *call_icon = g_object_get_data (G_OBJECT (item), "call-icon");
  if (!call_icon)
    return;

  /* Remove previous call classes */
  gtk_widget_remove_css_class (call_icon, "call-audio");
  gtk_widget_remove_css_class (call_icon, "call-video");
  gtk_widget_remove_css_class (call_icon, "call-screen");

  if (in_call > 0)
    {
      /* inCall flags: 1=audio, 2=video, 4=phone, 8=screen */
      if (in_call & 8)
        {
          gtk_image_set_from_icon_name (GTK_IMAGE (call_icon), "screen-shared-symbolic");
          gtk_widget_add_css_class (call_icon, "call-screen");
        }
      else if (in_call & 2)
        {
          gtk_image_set_from_icon_name (GTK_IMAGE (call_icon), "camera-video-symbolic");
          gtk_widget_add_css_class (call_icon, "call-video");
        }
      else
        {
          gtk_image_set_from_icon_name (GTK_IMAGE (call_icon), "audio-headphones-symbolic");
          gtk_widget_add_css_class (call_icon, "call-audio");
        }
      gtk_widget_set_visible (call_icon, TRUE);
    }
  else
    {
      gtk_widget_set_visible (call_icon, FALSE);
    }
}

/* Wrapper struct for avatar load callback to handle widget destruction */
typedef struct
{
  GWeakRef avatar_ref;
} AvatarCallbackData;

static void
avatar_callback_data_free (gpointer data)
{
  AvatarCallbackData *cb_data = data;
  g_weak_ref_clear (&cb_data->avatar_ref);
  g_free (cb_data);
}

static void
on_avatar_loaded (GnalkAvatarCache *cache,
                  GdkTexture        *texture,
                  gpointer           user_data)
{
  (void) cache;
  AvatarCallbackData *cb_data = user_data;

  /* Get strong reference to avatar widget - may have been destroyed */
  GtkWidget *avatar = g_weak_ref_get (&cb_data->avatar_ref);
  if (!avatar)
    {
      avatar_callback_data_free (cb_data);
      return;
    }

  if (texture)
    gtk_image_set_from_paintable (GTK_IMAGE (avatar), GDK_PAINTABLE (texture));

  g_object_unref (avatar);
  avatar_callback_data_free (cb_data);
}

static GtkWidget *
create_participant_item (GnalkParticipantsList *self,
                         const char             *user_id,
                         const char             *display_name,
                         const char             *status,
                         GnalkAvatarCache      *cache)
{
  (void) self;

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class (box, "participant-item");

  /* Store user_id as widget data for later updates */
  if (user_id)
    g_object_set_data_full (G_OBJECT (box), "user-id", g_strdup (user_id), g_free);

  /* Overlay for avatar and status */
  GtkWidget *overlay = gtk_overlay_new ();

  /* Avatar wrapped in frame for circular clipping */
  GtkWidget *avatar_frame = gtk_frame_new (NULL);
  gtk_widget_set_overflow (avatar_frame, GTK_OVERFLOW_HIDDEN);
  gtk_widget_add_css_class (avatar_frame, "participant-avatar");

  GtkWidget *avatar = gtk_image_new_from_icon_name ("avatar-default-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (avatar), 32);
  gtk_frame_set_child (GTK_FRAME (avatar_frame), avatar);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), avatar_frame);

  /* Status indicator (bottom-right) */
  GtkWidget *status_icon = gtk_image_new ();
  gtk_image_set_pixel_size (GTK_IMAGE (status_icon), 10);
  gtk_widget_set_halign (status_icon, GTK_ALIGN_END);
  gtk_widget_set_valign (status_icon, GTK_ALIGN_END);
  gtk_widget_add_css_class (status_icon, "status-indicator");

  gtk_widget_set_visible (status_icon, FALSE);  /* will be set by update_participant_status_icon */
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), status_icon);

  /* Call state indicator (top-left) */
  GtkWidget *call_icon = gtk_image_new_from_icon_name ("camera-video-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (call_icon), 12);
  gtk_widget_set_size_request (call_icon, 14, 14);
  gtk_widget_set_halign (call_icon, GTK_ALIGN_START);
  gtk_widget_set_valign (call_icon, GTK_ALIGN_START);
  gtk_widget_add_css_class (call_icon, "call-indicator");
  g_object_set_data (G_OBJECT (box), "call-icon", call_icon);

  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), call_icon);
  gtk_overlay_set_measure_overlay (GTK_OVERLAY (overlay), call_icon, TRUE);

  /* Call icon starts hidden */
  gtk_widget_set_visible (call_icon, FALSE);

  gtk_box_append (GTK_BOX (box), overlay);

  /* Name label */
  GtkWidget *label = gtk_label_new (display_name);
  gtk_widget_add_css_class (label, "participant-name");
  gtk_label_set_max_width_chars (GTK_LABEL (label), 10);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (box), label);

  /* Store references for later updates */
  g_object_set_data (G_OBJECT (box), "status-icon", status_icon);
  g_object_set_data (G_OBJECT (box), "name-label", label);

  /* Set initial status now that status-icon is stored on the widget */
  update_participant_status_icon (box, status);

  /* Load avatar asynchronously with weak reference to handle widget destruction */
  if (cache && user_id && *user_id)
    {
      AvatarCallbackData *cb_data = g_new0 (AvatarCallbackData, 1);
      g_weak_ref_init (&cb_data->avatar_ref, avatar);
      gnalk_avatar_cache_get_user_avatar (cache, user_id, 64, on_avatar_loaded, cb_data);
    }

  return box;
}

/* Update the status icon on an existing participant item */
static void
update_participant_status_icon (GtkWidget *item, const char *status)
{
  GtkWidget *status_icon = g_object_get_data (G_OBJECT (item), "status-icon");
  if (!status_icon)
    return;

  /* Remove previous status classes */
  gtk_widget_remove_css_class (status_icon, "status-online");
  gtk_widget_remove_css_class (status_icon, "status-away");
  gtk_widget_remove_css_class (status_icon, "status-dnd");

  if (status && *status)
    {
      if (g_strcmp0 (status, "online") == 0)
        {
          gtk_image_set_from_icon_name (GTK_IMAGE (status_icon), "user-available-symbolic");
          gtk_widget_add_css_class (status_icon, "status-online");
          gtk_widget_set_visible (status_icon, TRUE);
        }
      else if (g_strcmp0 (status, "away") == 0)
        {
          gtk_image_set_from_icon_name (GTK_IMAGE (status_icon), "user-away-symbolic");
          gtk_widget_add_css_class (status_icon, "status-away");
          gtk_widget_set_visible (status_icon, TRUE);
        }
      else if (g_strcmp0 (status, "dnd") == 0 || g_strcmp0 (status, "busy") == 0)
        {
          gtk_image_set_from_icon_name (GTK_IMAGE (status_icon), "user-busy-symbolic");
          gtk_widget_add_css_class (status_icon, "status-dnd");
          gtk_widget_set_visible (status_icon, TRUE);
        }
      else
        {
          gtk_widget_set_visible (status_icon, FALSE);
        }
    }
  else
    {
      gtk_widget_set_visible (status_icon, FALSE);
    }
}

/* Compare participants: online first, then alphabetically by name */
static gint
compare_participants_by_status_and_name (gconstpointer a, gconstpointer b)
{
  JsonObject *pa = *(JsonObject **) a;
  JsonObject *pb = *(JsonObject **) b;

  const char *status_a = json_object_get_string_member_with_default (pa, "status", "");
  const char *status_b = json_object_get_string_member_with_default (pb, "status", "");
  const char *name_a = json_object_get_string_member_with_default (pa, "displayName", "");
  const char *name_b = json_object_get_string_member_with_default (pb, "displayName", "");

  gboolean online_a = g_strcmp0 (status_a, "online") == 0;
  gboolean online_b = g_strcmp0 (status_b, "online") == 0;

  /* Online users come first */
  if (online_a && !online_b)
    return -1;
  if (!online_a && online_b)
    return 1;

  /* Within same online status, sort alphabetically by name */
  return g_utf8_collate (name_a, name_b);
}

static void
clear_participants_box (GnalkParticipantsList *self)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->participants_box)) != NULL)
    gtk_box_remove (GTK_BOX (self->participants_box), child);
}

static void
gnalk_participants_list_finalize (GObject *object)
{
  GnalkParticipantsList *self = GNALK_PARTICIPANTS_LIST (object);

  g_clear_pointer (&self->context_actor_id, g_free);
  g_clear_pointer (&self->my_user_id, g_free);

  G_OBJECT_CLASS (gnalk_participants_list_parent_class)->finalize (object);
}

static void
gnalk_participants_list_dispose (GObject *object)
{
  GnalkParticipantsList *self = GNALK_PARTICIPANTS_LIST (object);

  if (self->context_popover)
    {
      /* Popover may be parented to a participant item - unparent it first */
      GtkWidget *parent = gtk_widget_get_parent (self->context_popover);
      if (parent)
        gtk_widget_unparent (self->context_popover);
      /* Release our ownership reference */
      g_object_unref (self->context_popover);
      self->context_popover = NULL;
    }

  G_OBJECT_CLASS (gnalk_participants_list_parent_class)->dispose (object);
}

static void
gnalk_participants_list_class_init (GnalkParticipantsListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = gnalk_participants_list_finalize;
  object_class->dispose = gnalk_participants_list_dispose;

  gtk_widget_class_set_template_from_resource (widget_class,
                                                "/net/skilpa/gnalk/gnalk-participants-list.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkParticipantsList, revealer);
  gtk_widget_class_bind_template_child (widget_class, GnalkParticipantsList, participants_box);

  signals[SIGNAL_PARTICIPANT_CLICKED] =
    g_signal_new ("participant-clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_CHAT_REQUESTED] =
    g_signal_new ("chat-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_PROMOTE_REQUESTED] =
    g_signal_new ("promote-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 2, G_TYPE_INT64, G_TYPE_STRING);

  signals[SIGNAL_DEMOTE_REQUESTED] =
    g_signal_new ("demote-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 2, G_TYPE_INT64, G_TYPE_STRING);

  signals[SIGNAL_REMOVE_REQUESTED] =
    g_signal_new ("remove-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 2, G_TYPE_INT64, G_TYPE_STRING);
}

static void
on_chat_clicked (GtkButton *button G_GNUC_UNUSED,
                 gpointer   user_data)
{
  GnalkParticipantsList *self = GNALK_PARTICIPANTS_LIST (user_data);

  gtk_popover_popdown (GTK_POPOVER (self->context_popover));

  if (self->context_actor_id && *self->context_actor_id)
    g_signal_emit (self, signals[SIGNAL_CHAT_REQUESTED], 0, self->context_actor_id);
}

static void
on_promote_clicked (GtkButton *button G_GNUC_UNUSED,
                    gpointer   user_data)
{
  GnalkParticipantsList *self = GNALK_PARTICIPANTS_LIST (user_data);

  gtk_popover_popdown (GTK_POPOVER (self->context_popover));

  g_signal_emit (self, signals[SIGNAL_PROMOTE_REQUESTED], 0,
                 self->context_attendee_id, self->context_actor_id);
}

static void
on_demote_clicked (GtkButton *button G_GNUC_UNUSED,
                   gpointer   user_data)
{
  GnalkParticipantsList *self = GNALK_PARTICIPANTS_LIST (user_data);

  gtk_popover_popdown (GTK_POPOVER (self->context_popover));

  g_signal_emit (self, signals[SIGNAL_DEMOTE_REQUESTED], 0,
                 self->context_attendee_id, self->context_actor_id);
}

static void
on_remove_clicked (GtkButton *button G_GNUC_UNUSED,
                   gpointer   user_data)
{
  GnalkParticipantsList *self = GNALK_PARTICIPANTS_LIST (user_data);

  gtk_popover_popdown (GTK_POPOVER (self->context_popover));

  g_signal_emit (self, signals[SIGNAL_REMOVE_REQUESTED], 0,
                 self->context_attendee_id, self->context_actor_id);
}

static void
show_participant_context_menu (GnalkParticipantsList *self,
                                GtkWidget              *item,
                                GtkGesture             *gesture,
                                double                  x,
                                double                  y)
{
  /* Don't show menu if offline */
  if (!self->is_online)
    return;

  /* Claim the sequence */
  gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_CLAIMED);

  /* Get participant data from item */
  const char *actor_id = g_object_get_data (G_OBJECT (item), "user-id");
  gint64 attendee_id = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "attendee-id"));
  gint participant_type = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "participant-type"));

  /* Store context for action handlers */
  g_free (self->context_actor_id);
  self->context_actor_id = g_strdup (actor_id);
  self->context_attendee_id = attendee_id;
  self->context_participant_type = participant_type;

  /* Calculate visibility for each action */
  gboolean is_self = (g_strcmp0 (actor_id, self->my_user_id) == 0);
  gboolean caller_is_mod = (self->my_participant_type == 1 || self->my_participant_type == 2);
  gboolean target_is_mod = (participant_type == 1 || participant_type == 2 || participant_type == 6);
  gboolean target_is_owner = (participant_type == 1);
  gboolean is_one_to_one = (self->room_type == 1);

  /* Chat directly: not self, not 1-to-1, target is a user (has actorId) */
  gtk_widget_set_visible (self->chat_button, !is_self && !is_one_to_one && actor_id && *actor_id);

  /* Promote: caller is mod, not self, target is NOT already mod */
  gtk_widget_set_visible (self->promote_button, caller_is_mod && !is_self && !target_is_mod);

  /* Demote: caller is mod, not self, target IS mod but not owner */
  gtk_widget_set_visible (self->demote_button, caller_is_mod && !is_self && target_is_mod && !target_is_owner);

  /* Remove: caller is mod, not self, target not owner, not 1-to-1 */
  gtk_widget_set_visible (self->remove_button, caller_is_mod && !is_self && !target_is_owner && !is_one_to_one);

  /* Check if any buttons are visible */
  gboolean any_visible = gtk_widget_get_visible (self->chat_button) ||
                         gtk_widget_get_visible (self->promote_button) ||
                         gtk_widget_get_visible (self->demote_button) ||
                         gtk_widget_get_visible (self->remove_button);

  if (!any_visible)
    return;

  /* Reparent popover to this item (unparent from previous if needed) */
  GtkWidget *current_parent = gtk_widget_get_parent (self->context_popover);
  if (current_parent && current_parent != item)
    gtk_widget_unparent (self->context_popover);
  if (!gtk_widget_get_parent (self->context_popover))
    gtk_widget_set_parent (self->context_popover, item);

  /* Position and show the popover relative to the participant item */
  GdkRectangle rect = { (int) x, (int) y, 1, 1 };
  gtk_popover_set_pointing_to (GTK_POPOVER (self->context_popover), &rect);

  gtk_popover_popup (GTK_POPOVER (self->context_popover));
}

static void
on_participant_right_click (GtkGestureClick *gesture,
                             int              n_press G_GNUC_UNUSED,
                             double           x,
                             double           y,
                             gpointer         user_data)
{
  GtkWidget *item = GTK_WIDGET (user_data);
  GnalkParticipantsList *self = GNALK_PARTICIPANTS_LIST (
    g_object_get_data (G_OBJECT (item), "participants-list"));

  if (self)
    show_participant_context_menu (self, item, GTK_GESTURE (gesture), x, y);
}

static void
on_participant_long_press (GtkGestureLongPress *gesture,
                            double               x,
                            double               y,
                            gpointer             user_data)
{
  GtkWidget *item = GTK_WIDGET (user_data);
  GnalkParticipantsList *self = GNALK_PARTICIPANTS_LIST (
    g_object_get_data (G_OBJECT (item), "participants-list"));

  if (self)
    show_participant_context_menu (self, item, GTK_GESTURE (gesture), x, y);
}


static void
gnalk_participants_list_init (GnalkParticipantsList *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  /* Create context menu popover - we own it and manage its parent */
  self->context_popover = gtk_popover_new ();
  g_object_ref_sink (self->context_popover);  /* Sink floating ref, we own it */
  gtk_popover_set_has_arrow (GTK_POPOVER (self->context_popover), TRUE);

  GtkWidget *menu_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (menu_box, "context-menu");

  /* Chat directly button */
  self->chat_button = gtk_button_new_with_label ("Chat directly");
  gtk_button_set_has_frame (GTK_BUTTON (self->chat_button), FALSE);
  gtk_widget_add_css_class (self->chat_button, "flat");
  g_signal_connect (self->chat_button, "clicked", G_CALLBACK (on_chat_clicked), self);
  gtk_box_append (GTK_BOX (menu_box), self->chat_button);

  /* Promote to moderator button */
  self->promote_button = gtk_button_new_with_label ("Promote to moderator");
  gtk_button_set_has_frame (GTK_BUTTON (self->promote_button), FALSE);
  gtk_widget_add_css_class (self->promote_button, "flat");
  g_signal_connect (self->promote_button, "clicked", G_CALLBACK (on_promote_clicked), self);
  gtk_box_append (GTK_BOX (menu_box), self->promote_button);

  /* Demote from moderator button */
  self->demote_button = gtk_button_new_with_label ("Demote from moderator");
  gtk_button_set_has_frame (GTK_BUTTON (self->demote_button), FALSE);
  gtk_widget_add_css_class (self->demote_button, "flat");
  g_signal_connect (self->demote_button, "clicked", G_CALLBACK (on_demote_clicked), self);
  gtk_box_append (GTK_BOX (menu_box), self->demote_button);

  /* Remove from conversation button */
  self->remove_button = gtk_button_new_with_label ("Remove from conversation");
  gtk_button_set_has_frame (GTK_BUTTON (self->remove_button), FALSE);
  gtk_widget_add_css_class (self->remove_button, "flat");
  gtk_widget_add_css_class (self->remove_button, "error");
  g_signal_connect (self->remove_button, "clicked", G_CALLBACK (on_remove_clicked), self);
  gtk_box_append (GTK_BOX (menu_box), self->remove_button);

  gtk_popover_set_child (GTK_POPOVER (self->context_popover), menu_box);

  /* Initialize state */
  self->is_online = TRUE;
}

GnalkParticipantsList *
gnalk_participants_list_new (void)
{
  return g_object_new (GNALK_TYPE_PARTICIPANTS_LIST, NULL);
}

void
gnalk_participants_list_set_participants (GnalkParticipantsList *self,
                                            JsonArray              *participants,
                                            GnalkAvatarCache      *cache,
                                            const char             *my_user_id,
                                            gint                    my_participant_type,
                                            gint                    room_type)
{
  g_return_if_fail (GNALK_IS_PARTICIPANTS_LIST (self));

  if (!participants)
    return;

  /* Store caller's permissions context */
  g_free (self->my_user_id);
  self->my_user_id = g_strdup (my_user_id);
  self->my_participant_type = my_participant_type;
  self->room_type = room_type;

  clear_participants_box (self);

  guint n_participants = json_array_get_length (participants);

  /* Collect participants for sorting (skip guests) */
  GPtrArray *participant_list = g_ptr_array_new ();
  for (guint i = 0; i < n_participants; i++)
    {
      JsonObject *participant = json_array_get_object_element (participants, i);
      const char *actor_id = json_object_get_string_member_with_default (participant, "actorId", "");

      /* Skip guests (empty actorId) */
      if (!actor_id || !*actor_id)
        continue;

      g_ptr_array_add (participant_list, participant);
    }

  /* Sort: online first, then alphabetically by name */
  g_ptr_array_sort (participant_list, (GCompareFunc) compare_participants_by_status_and_name);

  /* Create participant items in sorted order */
  for (guint i = 0; i < participant_list->len; i++)
    {
      JsonObject *participant = g_ptr_array_index (participant_list, i);
      const char *actor_id = json_object_get_string_member_with_default (participant, "actorId", "");
      const char *display_name = json_object_get_string_member_with_default (participant, "displayName", "");
      const char *status = json_object_get_string_member_with_default (participant, "status", "");
      gint64 attendee_id = json_object_get_int_member_with_default (participant, "attendeeId", 0);
      gint participant_type = (gint) json_object_get_int_member_with_default (participant, "participantType", 3);
      gint64 in_call = json_object_get_int_member_with_default (participant, "inCall", 0);

      GtkWidget *item = create_participant_item (self, actor_id, display_name, status, cache);

      /* Set call icon from JSON data directly */
      update_participant_call_icon (item, in_call);

      /* Store additional data for context menu */
      g_object_set_data (G_OBJECT (item), "attendee-id", GINT_TO_POINTER ((gint) attendee_id));
      g_object_set_data (G_OBJECT (item), "participant-type", GINT_TO_POINTER (participant_type));
      g_object_set_data (G_OBJECT (item), "participants-list", self);

      /* Add right-click gesture */
      GtkGesture *right_click = gtk_gesture_click_new ();
      gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (right_click), GDK_BUTTON_SECONDARY);
      gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (right_click), GTK_PHASE_CAPTURE);
      g_signal_connect (right_click, "pressed", G_CALLBACK (on_participant_right_click), item);
      gtk_widget_add_controller (item, GTK_EVENT_CONTROLLER (right_click));

      /* Add long press gesture for touch devices */
      GtkGesture *long_press = gtk_gesture_long_press_new ();
      gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (long_press), TRUE);
      g_signal_connect (long_press, "pressed", G_CALLBACK (on_participant_long_press), item);
      gtk_widget_add_controller (item, GTK_EVENT_CONTROLLER (long_press));

      gtk_box_append (GTK_BOX (self->participants_box), item);
    }

  g_ptr_array_free (participant_list, TRUE);
}

void
gnalk_participants_list_update_call_state (GnalkParticipantsList *self,
                                             const char             *actor_id,
                                             gint64                  in_call)
{
  g_return_if_fail (GNALK_IS_PARTICIPANTS_LIST (self));

  if (!actor_id)
    return;

  GtkWidget *child = gtk_widget_get_first_child (self->participants_box);
  while (child)
    {
      const char *item_user_id = g_object_get_data (G_OBJECT (child), "user-id");
      if (item_user_id && g_strcmp0 (item_user_id, actor_id) == 0)
        {
          update_participant_call_icon (child, in_call);
          break;
        }
      child = gtk_widget_get_next_sibling (child);
    }
}

void
gnalk_participants_list_update_participant_status (GnalkParticipantsList *self,
                                                     const char             *actor_id,
                                                     const char             *status)
{
  g_return_if_fail (GNALK_IS_PARTICIPANTS_LIST (self));

  if (!actor_id)
    return;

  GtkWidget *child = gtk_widget_get_first_child (self->participants_box);
  while (child)
    {
      const char *item_user_id = g_object_get_data (G_OBJECT (child), "user-id");
      if (item_user_id && g_strcmp0 (item_user_id, actor_id) == 0)
        {
          update_participant_status_icon (child, status);
          break;
        }
      child = gtk_widget_get_next_sibling (child);
    }
}

void
gnalk_participants_list_clear_all_call_states (GnalkParticipantsList *self)
{
  g_return_if_fail (GNALK_IS_PARTICIPANTS_LIST (self));

  GtkWidget *child = gtk_widget_get_first_child (self->participants_box);
  while (child)
    {
      update_participant_call_icon (child, 0);
      child = gtk_widget_get_next_sibling (child);
    }
}

void
gnalk_participants_list_clear (GnalkParticipantsList *self)
{
  g_return_if_fail (GNALK_IS_PARTICIPANTS_LIST (self));

  clear_participants_box (self);
  gtk_revealer_set_reveal_child (GTK_REVEALER (self->revealer), FALSE);
}

void
gnalk_participants_list_hide (GnalkParticipantsList *self)
{
  g_return_if_fail (GNALK_IS_PARTICIPANTS_LIST (self));

  gtk_revealer_set_reveal_child (GTK_REVEALER (self->revealer), FALSE);
}

void
gnalk_participants_list_show (GnalkParticipantsList *self)
{
  g_return_if_fail (GNALK_IS_PARTICIPANTS_LIST (self));

  gtk_revealer_set_reveal_child (GTK_REVEALER (self->revealer), TRUE);
}

void
gnalk_participants_list_set_visible (GnalkParticipantsList *self,
                                       gboolean                visible)
{
  g_return_if_fail (GNALK_IS_PARTICIPANTS_LIST (self));

  gtk_revealer_set_reveal_child (GTK_REVEALER (self->revealer), visible);
}

void
gnalk_participants_list_set_online (GnalkParticipantsList *self,
                                      gboolean                is_online)
{
  g_return_if_fail (GNALK_IS_PARTICIPANTS_LIST (self));

  self->is_online = is_online;
}
