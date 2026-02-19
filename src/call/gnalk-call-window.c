#include "gnalk-call-window-private.h"

/* Property and signal arrays - declared in private header as extern */
GParamSpec *gnalk_call_window_properties[GNALK_CALL_WINDOW_N_PROPS];
guint gnalk_call_window_signals[GNALK_CALL_WINDOW_N_SIGNALS];

G_DEFINE_FINAL_TYPE (GnalkCallWindow, gnalk_call_window, ADW_TYPE_WINDOW)

/* Use shortened names for local references to the global arrays */
#define properties gnalk_call_window_properties
#define signals gnalk_call_window_signals
#define PROP_ROOM_NAME GNALK_CALL_WINDOW_PROP_ROOM_NAME
#define PROP_CALL_STATUS GNALK_CALL_WINDOW_PROP_CALL_STATUS
#define N_PROPS GNALK_CALL_WINDOW_N_PROPS
#define SIGNAL_CALL_ENDED GNALK_CALL_WINDOW_SIGNAL_CALL_ENDED
#define N_SIGNALS GNALK_CALL_WINDOW_N_SIGNALS

static void
call_participant_record_free (gpointer data)
{
  CallParticipantRecord *record = data;
  g_free (record->display_name);
  g_free (record->user_id);
  if (record->tile)
    g_object_unref (record->tile);
  if (record->screen_tile)
    g_object_unref (record->screen_tile);
  if (record->screen_subscriber)
    g_object_unref (record->screen_subscriber);
  g_free (record->screen_subscriber_sid);
  g_free (record);
}

CallParticipantRecord *
gnalk_call_ensure_participant_record (GnalkCallWindow *self,
                                        const char       *session_id)
{
  CallParticipantRecord *record = g_hash_table_lookup (self->participants, session_id);
  if (!record)
    {
      record = g_new0 (CallParticipantRecord, 1);
      g_hash_table_insert (self->participants, g_strdup (session_id), record);
    }
  return record;
}

static void
gnalk_call_window_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (object);

  switch (prop_id)
    {
    case PROP_ROOM_NAME:
      g_value_set_string (value, self->room_name);
      break;
    case PROP_CALL_STATUS:
      g_value_set_string (value, self->call_status);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gnalk_call_window_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (object);

  switch (prop_id)
    {
    case PROP_ROOM_NAME:
      g_free (self->room_name);
      self->room_name = g_value_dup_string (value);
      break;
    case PROP_CALL_STATUS:
      g_free (self->call_status);
      self->call_status = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* Forward declarations */
static void on_signaling_message (GnalkSignaling *signaling,
                                  JsonNode        *message,
                                  gpointer         user_data);

/* ============ Hand raise / Reaction signal handlers ============ */

static void
on_hand_raised (GnalkSignaling  *signaling G_GNUC_UNUSED,
                const char       *session_id,
                gboolean          state,
                gint64            timestamp G_GNUC_UNUSED,
                GnalkCallWindow *self)
{
  if (!session_id)
    return;

  CallParticipantRecord *record = g_hash_table_lookup (self->participants, session_id);
  if (record)
    {
      record->hand_raised = state;
      if (record->tile)
        gnalk_participant_tile_set_hand_raised (record->tile, state);
    }
}

typedef struct {
  GWeakRef window_ref;
  char *session_id;
} ReactionFadeData;

static void
reaction_fade_data_free (ReactionFadeData *data)
{
  g_weak_ref_clear (&data->window_ref);
  g_free (data->session_id);
  g_free (data);
}

static gboolean
on_reaction_fade_timeout (gpointer user_data)
{
  ReactionFadeData *data = user_data;

  g_autoptr(GObject) window_obj = g_weak_ref_get (&data->window_ref);
  if (window_obj)
    {
      GnalkCallWindow *self = GNALK_CALL_WINDOW (window_obj);
      self->reaction_fade_timeout_id = 0;

      CallParticipantRecord *record = g_hash_table_lookup (self->participants,
                                                            data->session_id);
      if (record && record->tile)
        gnalk_participant_tile_set_reaction (record->tile, NULL);
    }

  reaction_fade_data_free (data);
  return G_SOURCE_REMOVE;
}

static void
on_call_reaction (GnalkSignaling  *signaling G_GNUC_UNUSED,
                  const char       *session_id,
                  const char       *emoji,
                  GnalkCallWindow *self)
{
  if (!session_id || !emoji || !*emoji)
    return;

  CallParticipantRecord *record = g_hash_table_lookup (self->participants, session_id);
  if (!record || !record->tile)
    return;

  gnalk_participant_tile_set_reaction (record->tile, emoji);

  /* Cancel previous fade timeout if any */
  if (self->reaction_fade_timeout_id > 0)
    g_source_remove (self->reaction_fade_timeout_id);

  ReactionFadeData *data = g_new0 (ReactionFadeData, 1);
  g_weak_ref_init (&data->window_ref, self);
  data->session_id = g_strdup (session_id);

  self->reaction_fade_timeout_id = g_timeout_add_seconds (3, on_reaction_fade_timeout, data);
}

/* Drag gesture callbacks for preview overlay.
 * We translate coordinates to the parent (overlay) to avoid jitter caused
 * by the widget moving under the gesture as margins change. */
static void
on_preview_drag_begin (GtkGestureDrag   *gesture G_GNUC_UNUSED,
                       double            start_x,
                       double            start_y,
                       GnalkCallWindow *self)
{
  /* Store initial margins */
  self->preview_margin_start = gtk_widget_get_margin_start (self->local_video_box);
  self->preview_margin_top = gtk_widget_get_margin_top (self->local_video_box);

  /* Convert start point to parent coordinates for stable tracking */
  GtkWidget *parent = gtk_widget_get_parent (self->local_video_box);
  if (parent)
    {
      graphene_point_t widget_point = GRAPHENE_POINT_INIT ((float)start_x, (float)start_y);
      graphene_point_t parent_point;
      if (gtk_widget_compute_point (self->local_video_box, parent, &widget_point, &parent_point))
        {
          self->drag_start_x = parent_point.x;
          self->drag_start_y = parent_point.y;
        }
      else
        {
          self->drag_start_x = start_x;
          self->drag_start_y = start_y;
        }
    }
  else
    {
      self->drag_start_x = start_x;
      self->drag_start_y = start_y;
    }
}

static void
on_preview_drag_update (GtkGestureDrag   *gesture,
                        double            offset_x G_GNUC_UNUSED,
                        double            offset_y G_GNUC_UNUSED,
                        GnalkCallWindow *self)
{
  GtkWidget *parent = gtk_widget_get_parent (self->local_video_box);
  if (!parent)
    return;

  /* Get current point in widget coordinates */
  double widget_x, widget_y;
  if (!gtk_gesture_drag_get_offset (GTK_GESTURE_DRAG (gesture), &widget_x, &widget_y))
    return;

  /* Get the start point from the gesture */
  double start_x, start_y;
  gtk_gesture_drag_get_start_point (GTK_GESTURE_DRAG (gesture), &start_x, &start_y);

  /* Convert current point to parent coordinates */
  graphene_point_t widget_point = GRAPHENE_POINT_INIT ((float)(start_x + widget_x), (float)(start_y + widget_y));
  graphene_point_t parent_point;
  if (!gtk_widget_compute_point (self->local_video_box, parent, &widget_point, &parent_point))
    return;

  /* Calculate new margins based on movement in parent coordinate space */
  int new_margin_start = self->preview_margin_start + (int)(parent_point.x - self->drag_start_x);
  int new_margin_top = self->preview_margin_top + (int)(parent_point.y - self->drag_start_y);

  /* Clamp to reasonable bounds */
  int parent_width = gtk_widget_get_width (parent);
  int parent_height = gtk_widget_get_height (parent);
  int frame_width = gtk_widget_get_width (self->local_video_box);
  int frame_height = gtk_widget_get_height (self->local_video_box);

  /* Keep preview within bounds with some padding */
  new_margin_start = CLAMP (new_margin_start, 8, parent_width - frame_width - 8);
  new_margin_top = CLAMP (new_margin_top, 8, parent_height - frame_height - 60);

  gtk_widget_set_margin_start (self->local_video_box, new_margin_start);
  gtk_widget_set_margin_top (self->local_video_box, new_margin_top);
}

/* Idle callback to recalculate grid layout after resize */
static gboolean
on_resize_idle (gpointer user_data)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (user_data);

  self->resize_idle_id = 0;

  /* Only recalculate if in grid mode */
  if (self->layout_mode == GNALK_LAYOUT_GRID)
    gnalk_call_layout_recalculate (self);

  return G_SOURCE_REMOVE;
}

/* Handle window resize to recalculate grid layout */
static void
on_window_size_changed (GObject    *object,
                        GParamSpec *pspec G_GNUC_UNUSED,
                        gpointer    user_data G_GNUC_UNUSED)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (object);

  /* Defer recalculation to avoid recursion during layout phase */
  if (self->resize_idle_id == 0)
    self->resize_idle_id = g_idle_add (on_resize_idle, self);
}

/* Handle window close request */
static gboolean
on_close_request (GtkWindow *window,
                  gpointer   user_data)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (window);

  gnalk_call_window_end_call (self);
  g_signal_emit (self, signals[SIGNAL_CALL_ENDED], 0);

  return FALSE;  /* Allow window to close */
}

static void
gnalk_call_window_finalize (GObject *object)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (object);

  gnalk_call_window_end_call (self);

#ifdef HAVE_GSTREAMER
  gnalk_call_preview_stop_local_preview (self);
#endif

  /* Cancel pending resize recalculation */
  if (self->resize_idle_id > 0)
    {
      g_source_remove (self->resize_idle_id);
      self->resize_idle_id = 0;
    }

  /* Cancel pending layout recalculation */
  if (self->layout_idle_id > 0)
    {
      g_source_remove (self->layout_idle_id);
      self->layout_idle_id = 0;
    }

  g_free (self->room_token);
  g_free (self->room_name);
  g_free (self->call_status);
  g_free (self->session_id);
  g_free (self->signaling_session_id);
  g_free (self->publisher_sid);
  g_free (self->active_speaker_id);
  g_clear_pointer (&self->peer_session_ids, g_ptr_array_unref);
  g_clear_pointer (&self->participants, g_hash_table_unref);
  g_clear_pointer (&self->user_display_names, g_hash_table_unref);

  /* Disconnect signal handlers before clearing state */
  if (self->signaling)
    {
      if (self->screen_share_handler_id > 0)
        {
          g_signal_handler_disconnect (self->signaling, self->screen_share_handler_id);
          self->screen_share_handler_id = 0;
        }
      if (self->hand_raised_handler_id > 0)
        {
          g_signal_handler_disconnect (self->signaling, self->hand_raised_handler_id);
          self->hand_raised_handler_id = 0;
        }
      if (self->call_reaction_handler_id > 0)
        {
          g_signal_handler_disconnect (self->signaling, self->call_reaction_handler_id);
          self->call_reaction_handler_id = 0;
        }
    }

  if (self->reaction_fade_timeout_id > 0)
    {
      g_source_remove (self->reaction_fade_timeout_id);
      self->reaction_fade_timeout_id = 0;
    }

  /* Screen sharing state is cleaned up via participants hash table */
  g_clear_object (&self->screen_capture);
  g_clear_object (&self->screen_publisher);
  g_free (self->screen_publisher_sid);

  g_clear_object (&self->avatar_cache);
  g_clear_object (&self->http_client);

  /* Clear video callbacks before releasing webrtc to prevent use-after-free
   * from pending idle callbacks */
  if (self->webrtc)
    gnalk_webrtc_set_local_video_callback (self->webrtc, NULL, NULL);

  g_clear_object (&self->webrtc);
  g_clear_object (&self->signaling);

  G_OBJECT_CLASS (gnalk_call_window_parent_class)->finalize (object);
}

static void
gnalk_call_window_class_init (GnalkCallWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = gnalk_call_window_get_property;
  object_class->set_property = gnalk_call_window_set_property;
  object_class->finalize = gnalk_call_window_finalize;

  properties[PROP_ROOM_NAME] =
    g_param_spec_string ("room-name", NULL, NULL, "", G_PARAM_READWRITE);

  properties[PROP_CALL_STATUS] =
    g_param_spec_string ("call-status", NULL, NULL, _("Connecting..."), G_PARAM_READWRITE);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  signals[SIGNAL_CALL_ENDED] =
    g_signal_new ("call-ended",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/net/skilpa/gnalk/gnalk-call-window.ui");
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, header_bar);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, video_overlay);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, video_stack);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, local_video_fullscreen);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, local_video_box);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, local_video_frame);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, switch_camera_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, mute_audio_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, mute_video_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, screen_share_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, raise_hand_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, reaction_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, hangup_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, layout_toggle_button);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, layout_stack);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, video_grid);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, speaker_view);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, speaker_main);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, speaker_strip);
  gtk_widget_class_bind_template_child (widget_class, GnalkCallWindow, connecting_spinner);
}

/* Detect if we're running on mobile based on display size */
static gboolean
is_mobile_display (void)
{
  GdkDisplay *display = gdk_display_get_default ();
  if (!display)
    return FALSE;

  GListModel *monitors = gdk_display_get_monitors (display);
  if (!monitors || g_list_model_get_n_items (monitors) == 0)
    return FALSE;

  GdkMonitor *monitor = g_list_model_get_item (monitors, 0);
  if (!monitor)
    return FALSE;

  GdkRectangle geometry;
  gdk_monitor_get_geometry (monitor, &geometry);
  g_object_unref (monitor);

  /* Consider it mobile if width is less than threshold */
  return geometry.width < MOBILE_WIDTH_THRESHOLD;
}

static void
gnalk_call_window_init (GnalkCallWindow *self)
{
  self->call_status = g_strdup (_("Connecting..."));
  self->peer_session_ids = g_ptr_array_new_with_free_func (g_free);
  self->participants = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free, call_participant_record_free);
  self->user_display_names = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free, g_free);
  self->preview_margin_start = 12;
  self->preview_margin_top = 12;
  /* Read camera position from settings (0=Front, 1=Back, 2=Generic) */
  {
    g_autoptr(GSettings) settings = g_settings_new ("net.skilpa.gnalk");
    gint pos = g_settings_get_int (settings, "camera-position");
    if (pos == 1)
      self->camera_position = GNALK_CAMERA_BACK;
    else if (pos == 2)
      self->camera_position = GNALK_CAMERA_GENERIC;
    else
      self->camera_position = GNALK_CAMERA_FRONT;
  }
  self->layout_mode = GNALK_LAYOUT_GRID;       /* Default to grid layout */

  gtk_widget_init_template (GTK_WIDGET (self));

  /* Detect mobile vs desktop */
  self->is_mobile = is_mobile_display ();

  if (self->is_mobile)
    {
      /* Mobile: fullscreen window */
      gtk_window_fullscreen (GTK_WINDOW (self));
      g_message ("Call window: mobile mode (fullscreen)");
    }
  else
    {
      /* Desktop: regular floating window */
      g_message ("Call window: desktop mode (floating)");
    }

  /* Start with fullscreen preview visible, overlay hidden */
  gtk_stack_set_visible_child_name (GTK_STACK (self->video_stack), "preview");
  gtk_widget_set_visible (self->local_video_box, FALSE);

  /* Clip overlay and disable measure to prevent it from expanding */
  gtk_overlay_set_clip_overlay (GTK_OVERLAY (self->video_overlay), self->local_video_box, TRUE);
  gtk_overlay_set_measure_overlay (GTK_OVERLAY (self->video_overlay), self->local_video_box, FALSE);

  /* Explicitly constrain local preview size for GTK 4.20+ compatibility */
  gtk_widget_set_size_request (self->local_video_frame, 160, 120);
  gtk_widget_set_hexpand (self->local_video_frame, FALSE);
  gtk_widget_set_vexpand (self->local_video_frame, FALSE);

  g_signal_connect (self->hangup_button, "clicked", G_CALLBACK (gnalk_call_control_on_hangup_clicked), self);
  g_signal_connect (self->switch_camera_button, "clicked", G_CALLBACK (gnalk_call_control_on_switch_camera_clicked), self);
  g_signal_connect (self->mute_audio_button, "toggled", G_CALLBACK (gnalk_call_control_on_mute_audio_toggled), self);
  g_signal_connect (self->mute_video_button, "toggled", G_CALLBACK (gnalk_call_control_on_mute_video_toggled), self);
  if (self->layout_toggle_button)
    g_signal_connect (self->layout_toggle_button, "clicked", G_CALLBACK (gnalk_call_layout_on_toggle_clicked), self);
  if (self->screen_share_button)
    g_signal_connect (self->screen_share_button, "toggled", G_CALLBACK (gnalk_call_screen_share_on_toggled), self);
  if (self->raise_hand_button)
    g_signal_connect (self->raise_hand_button, "toggled", G_CALLBACK (gnalk_call_control_on_raise_hand_toggled), self);

  /* Set up reaction emoji popover */
  if (self->reaction_button)
    {
      static const char *emojis[] = { "\xF0\x9F\x91\x8D", "\xF0\x9F\x91\x8E",  /* thumbs up/down */
                                       "\xF0\x9F\x98\x82", "\xE2\x9D\xA4\xEF\xB8\x8F",  /* face joy, red heart */
                                       "\xF0\x9F\x8E\x89", "\xF0\x9F\x91\x8F",  /* party popper, clap */
                                       "\xF0\x9F\xA4\x94", "\xF0\x9F\x98\xAE", NULL }; /* thinking, surprised */
      GtkWidget *popover = gtk_popover_new ();
      GtkWidget *flow = gtk_flow_box_new ();
      gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 4);
      gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
      gtk_widget_set_margin_start (flow, 6);
      gtk_widget_set_margin_end (flow, 6);
      gtk_widget_set_margin_top (flow, 6);
      gtk_widget_set_margin_bottom (flow, 6);

      for (int i = 0; emojis[i]; i++)
        {
          GtkWidget *btn = gtk_button_new_with_label (emojis[i]);
          gtk_widget_add_css_class (btn, "flat");
          gtk_widget_set_size_request (btn, 40, 40);
          g_object_set_data_full (G_OBJECT (btn), "emoji", g_strdup (emojis[i]), g_free);
          g_object_set_data (G_OBJECT (btn), "popover", popover);
          g_signal_connect (btn, "clicked", G_CALLBACK (gnalk_call_control_on_reaction_clicked), self);
          gtk_flow_box_insert (GTK_FLOW_BOX (flow), btn, -1);
        }

      gtk_popover_set_child (GTK_POPOVER (popover), flow);
      gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->reaction_button), popover);
    }

  /* Handle window close */
  g_signal_connect (self, "close-request", G_CALLBACK (on_close_request), NULL);

  /* Handle window resize to recalculate grid layout for 2-participant case */
  g_signal_connect (self, "notify::default-width", G_CALLBACK (on_window_size_changed), NULL);
  g_signal_connect (self, "notify::default-height", G_CALLBACK (on_window_size_changed), NULL);

  /* Add drag gesture for preview overlay - works with both mouse and touch */
  GtkGesture *drag = gtk_gesture_drag_new ();
  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (drag), FALSE);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_preview_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_preview_drag_update), self);
  gtk_widget_add_controller (self->local_video_box, GTK_EVENT_CONTROLLER (drag));
}

GnalkCallWindow *
gnalk_call_window_new (GtkApplication *app)
{
  return g_object_new (GNALK_TYPE_CALL_WINDOW,
                       "application", app,
                       NULL);
}

/* Forward declaration */
static void on_http_join_call_response (GnalkHttpClient *client,
                                        JsonNode         *response,
                                        GError           *error,
                                        gpointer          user_data);

/* Signaling message handler - dispatches incoming signaling messages */
static void
on_signaling_message (GnalkSignaling *signaling,
                      JsonNode        *message,
                      gpointer         user_data)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (user_data);

  if (!JSON_NODE_HOLDS_OBJECT (message))
    return;

  JsonObject *msg_obj = json_node_get_object (message);
  const char *type = json_object_get_string_member_with_default (msg_obj, "type", "");

  /* Handle signaling errors from HPB */
  if (g_strcmp0 (type, "error") == 0)
    {
      JsonObject *error_obj = json_object_get_object_member (msg_obj, "error");
      if (error_obj)
        {
          const char *error_code = json_object_get_string_member_with_default (error_obj, "code", "unknown");
          const char *error_msg = json_object_get_string_member_with_default (error_obj, "message", "Unknown error");
          g_warning ("Signaling error: code=%s, message=%s", error_code, error_msg);

          /* processing_failed means the MCU couldn't process our request.
           * This typically happens when requesting an offer from a peer that
           * isn't publishing to the MCU (e.g., peer doesn't support MCU mode). */
          if (g_strcmp0 (error_code, "processing_failed") == 0)
            {
              g_message ("MCU processing failed - peer may not support MCU mode");
              /* The participant tile will show avatar after connection timeout */
            }
        }
      return;
    }

  if (g_strcmp0 (type, "room") == 0)
    {
      /* Room messages are received when joining the signaling room.
       * The actual room join now happens in gnalk-window.c when selecting
       * a conversation, so we just log this for debugging. */
      JsonObject *room = json_object_get_object_member (msg_obj, "room");
      if (room)
        {
          /* Room message received - no action needed */
        }
    }
  else if (g_strcmp0 (type, "event") == 0)
    {
      JsonObject *event = json_object_get_object_member (msg_obj, "event");
      if (event)
        {
          const char *target = json_object_get_string_member_with_default (event, "target", "");
          const char *event_type = json_object_get_string_member_with_default (event, "type", "");

          if (g_strcmp0 (target, "room") == 0 && g_strcmp0 (event_type, "join") == 0)
            {
              JsonArray *join_arr = json_object_get_array_member (event, "join");
              if (join_arr)
                {
                  guint len = json_array_get_length (join_arr);
                  for (guint i = 0; i < len; i++)
                    {
                      JsonObject *participant = json_array_get_object_element (join_arr, i);
                      if (!participant)
                        continue;

                      const char *session_id = json_object_get_string_member_with_default (participant, "sessionid", NULL);
                      const char *room_session_id = json_object_get_string_member_with_default (participant, "roomsessionid", NULL);

                      /* Check if features array exists before accessing */
                      JsonNode *features_node = json_object_get_member (participant, "features");
                      if (features_node && JSON_NODE_HOLDS_ARRAY (features_node))
                        {
                          JsonArray *features = json_node_get_array (features_node);
                          guint feat_len = json_array_get_length (features);
                          for (guint f = 0; f < feat_len; f++)
                            {
                              const char *feat = json_array_get_string_element (features, f);
                              if (g_strcmp0 (feat, "mcu") == 0)
                                {
                                  if (!self->has_mcu)
                                  self->has_mcu = TRUE;
                                  break;
                                }
                            }
                        }

                      if (!session_id)
                        continue;

                      if (self->session_id && room_session_id &&
                          g_strcmp0 (room_session_id, self->session_id) == 0)
                        {
                          g_free (self->signaling_session_id);
                          self->signaling_session_id = g_strdup (session_id);
                          continue;
                        }

                      /* Skip if this is ourselves */
                      if (self->signaling_session_id &&
                          g_strcmp0 (session_id, self->signaling_session_id) == 0)
                        continue;

                      gboolean already_added = FALSE;
                      for (guint j = 0; j < self->peer_session_ids->len; j++)
                        {
                          if (g_strcmp0 (g_ptr_array_index (self->peer_session_ids, j), session_id) == 0)
                            {
                              already_added = TRUE;
                              break;
                            }
                        }

                      /* Extract display name and userid from user object (do this regardless of already_added) */
                      JsonObject *user_obj = json_object_get_object_member (participant, "user");
                      if (user_obj)
                        {
                          CallParticipantRecord *record = gnalk_call_ensure_participant_record (self, session_id);
                          const char *displayname = json_object_get_string_member_with_default (user_obj, "displayname", NULL);
                          if (displayname)
                            {
                              g_free (record->display_name);
                              record->display_name = g_strdup (displayname);
                              g_message ("Stored display name '%s' for session %s", displayname, session_id);
                            }
                          const char *userid = json_object_get_string_member_with_default (user_obj, "userid", NULL);
                          if (userid)
                            {
                              g_free (record->user_id);
                              record->user_id = g_strdup (userid);
                              g_message ("Stored userid '%s' for session %s (for avatar loading)", userid, session_id);
                            }
                        }

                      if (!already_added)
                        {
                          g_ptr_array_add (self->peer_session_ids, g_strdup (session_id));
                        }
                    }
                }
            }
          else if (g_strcmp0 (target, "room") == 0 && g_strcmp0 (event_type, "leave") == 0)
            {
              /* Handle room leave event - participant disconnected */
              JsonArray *leave_arr = json_object_get_array_member (event, "leave");
              if (leave_arr)
                {
                  guint len = json_array_get_length (leave_arr);
                  for (guint i = 0; i < len; i++)
                    {
                      const char *session_id = json_array_get_string_element (leave_arr, i);
                      if (!session_id)
                        continue;

                      /* Skip if this is ourselves */
                      if (self->signaling_session_id &&
                          g_strcmp0 (session_id, self->signaling_session_id) == 0)
                        continue;

                      g_message ("Room leave event for session: %s", session_id);

                      /* Remove from peer tracking */
                      for (guint j = 0; j < self->peer_session_ids->len; j++)
                        {
                          if (g_strcmp0 (g_ptr_array_index (self->peer_session_ids, j), session_id) == 0)
                            {
                              g_ptr_array_remove_index (self->peer_session_ids, j);
                              break;
                            }
                        }

                      /* Remove the participant tile */
                      gnalk_call_participant_remove_tile (self, session_id);
                    }
                }
            }
          else if (g_strcmp0 (target, "participants") == 0 && g_strcmp0 (event_type, "update") == 0)
            {
              JsonObject *update = json_object_get_object_member (event, "update");
              if (update)
                {
                  /* Check for "end call for all" broadcast */
                  if (json_object_has_member (update, "all") &&
                      json_object_get_boolean_member_with_default (update, "all", FALSE) &&
                      json_object_get_int_member_with_default (update, "incall", -1) == 0)
                    {
                      /* Call ended for all - close window */
                      gnalk_call_window_end_call (self);
                      g_signal_emit (self, signals[SIGNAL_CALL_ENDED], 0);
                      gtk_window_close (GTK_WINDOW (self));
                      return;
                    }

                  JsonArray *users = NULL;
                  if (json_object_has_member (update, "users"))
                    users = json_object_get_array_member (update, "users");
                  if (users)
                    {
                      guint len = json_array_get_length (users);
                      for (guint i = 0; i < len; i++)
                        {
                          JsonObject *user = json_array_get_object_element (users, i);
                          if (!user)
                            continue;

                          const char *session_id = json_object_get_string_member_with_default (user, "sessionId", NULL);
                          gint64 in_call = json_object_get_int_member_with_default (user, "inCall", 0);

                          if (!session_id)
                            continue;

                          /* Skip ourselves */
                          if (self->signaling_session_id &&
                              g_strcmp0 (session_id, self->signaling_session_id) == 0)
                            continue;

                          if (in_call == 0)
                            {
                              {
                              CallParticipantRecord *pr = g_hash_table_lookup (self->participants, session_id);
                              g_message ("Peer left call: session_id=%s, tile_exists=%d, peer_tracked=%d",
                                         session_id,
                                         pr && pr->tile ? 1 : 0,
                                         self->peer_session_ids->len > 0);
                              }

                              /* Peer left the call - remove from tracking */
                              for (guint j = 0; j < self->peer_session_ids->len; j++)
                                {
                                  if (g_strcmp0 (g_ptr_array_index (self->peer_session_ids, j), session_id) == 0)
                                    {
                                      g_ptr_array_remove_index (self->peer_session_ids, j);
                                      break;
                                    }
                                }

                              /* Remove the participant tile */
                              gnalk_call_participant_remove_tile (self, session_id);
                            }
                          else if (in_call > 0)
                            {
                              /* Track this peer */
                              gboolean already_added = FALSE;
                              for (guint j = 0; j < self->peer_session_ids->len; j++)
                                {
                                  if (g_strcmp0 (g_ptr_array_index (self->peer_session_ids, j), session_id) == 0)
                                    {
                                      already_added = TRUE;
                                      break;
                                    }
                                }

                              if (!already_added)
                                {
                                  g_ptr_array_add (self->peer_session_ids, g_strdup (session_id));

                                  /* Cache display name and user_id for this session if we have it from HTTP API */
                                  const char *user_id = json_object_get_string_member_with_default (user, "userId", NULL);
                                  if (user_id)
                                    {
                                      CallParticipantRecord *record = gnalk_call_ensure_participant_record (self, session_id);
                                      g_free (record->user_id);
                                      record->user_id = g_strdup (user_id);
                                      const char *name = g_hash_table_lookup (self->user_display_names, user_id);
                                      if (name)
                                        {
                                          g_free (record->display_name);
                                          record->display_name = g_strdup (name);
                                        }
                                    }
                                  g_message ("New peer in call: %s (user_id=%s)", session_id, user_id ? user_id : "NULL");
                                }

                              /* Start WebRTC if we haven't yet */
                              if (self->room_joined && !self->offer_sent && self->webrtc)
                                {
                                  GtkApplication *gtk_app = gtk_window_get_application (GTK_WINDOW (self));
                                  if (GNALK_IS_APPLICATION (gtk_app))
                                    {
                                      GnalkApplication *app = GNALK_APPLICATION (gtk_app);
                                      const char *stun = gnalk_application_get_stun_server (app);
                                      const char *turn = gnalk_application_get_turn_server (app);
                                      const char *turn_user = gnalk_application_get_turn_username (app);
                                      const char *turn_cred = gnalk_application_get_turn_credential (app);

                                      if (stun)
                                        gnalk_webrtc_set_stun_server (self->webrtc, stun);
                                      if (turn && turn_user && turn_cred)
                                        gnalk_webrtc_set_turn_server (self->webrtc, turn, turn_user, turn_cred);
                                    }

                                  gnalk_webrtc_set_signal_callback (self->webrtc, gnalk_call_signaling_on_webrtc_signal, self);
                                  gnalk_webrtc_set_connection_state_callback (self->webrtc, gnalk_call_signaling_on_publisher_connection_state, self);
                                  self->offer_sent = TRUE;

                                  /* Stop preview pipeline to release the camera for WebRTC
                                   * (v4l2src can only be opened by one pipeline at a time)
                                   * We need a delay to allow the camera device to be fully released
                                   */
#ifdef HAVE_GSTREAMER
                                  gnalk_call_preview_stop_local_preview (self);
#endif

                                  /* Show connecting spinner while WebRTC is being set up */
                                  if (self->video_stack)
                                    gtk_stack_set_visible_child_name (GTK_STACK (self->video_stack), "connecting");

                                  /*
                                   * Start WebRTC after a short delay to allow camera release.
                                   * In MCU mode: offer goes to ourselves, MCU picks it up.
                                   * In P2P mode: offer goes to peer directly.
                                   */
                                  if (self->start_webrtc_timeout_id == 0)
                                    {
                                      /* Delay to allow libcamera to fully release camera after preview stops */
                                      self->start_webrtc_timeout_id = g_timeout_add (500,
                                                                                     gnalk_call_preview_on_start_webrtc_after_preview_stop,
                                                                                     self);
                                    }
                                }

                              /* In MCU mode, add tile and request the peer's stream */
                              if (self->has_mcu)
                                {
                                  CallParticipantRecord *record = g_hash_table_lookup (self->participants, session_id);
                                  /* Only add if we don't already have a tile for this peer */
                                  if (!record || !record->tile)
                                    {
                                      if (self->publisher_connected)
                                        {
                                          /* Already connected, add tile and subscribe immediately */
                                          const char *display_name = record ? record->display_name : NULL;
                                          const char *peer_user_id = record ? record->user_id : NULL;
                                          /* Fall back to user_display_names lookup by userId */
                                          if (!display_name && peer_user_id)
                                            display_name = g_hash_table_lookup (self->user_display_names, peer_user_id);
                                          gnalk_call_participant_add_tile (self, session_id, display_name, peer_user_id);
                                          gnalk_call_signaling_request_offer_from_peer (self, session_id);
                                          /* Note: Don't speculatively request screen offers here.
                                           * Screen offers are requested in on_signaling_screen_share_changed()
                                           * when we receive a "shareScreen" message from a peer. */
                                        }
                                      /* If not connected yet, gnalk_call_signaling_on_publisher_connection_state will handle it */
                                    }
                                }

                              g_object_set (self, "call-status", _("In call"), NULL);
                            }
                        }
                    }
                }
            }
        }
    }
  else if (g_strcmp0 (type, "message") == 0)
    {
      JsonObject *msg_data = json_object_get_object_member (msg_obj, "message");
      if (msg_data)
        {
          /* Get sender session ID to route messages correctly */
          JsonObject *sender = json_object_get_object_member (msg_data, "sender");
          const char *sender_session_id = NULL;
          if (sender)
            sender_session_id = json_object_get_string_member_with_default (sender, "sessionid", NULL);

          JsonObject *data = json_object_get_object_member (msg_data, "data");
          if (data)
            {
              const char *data_type = json_object_get_string_member_with_default (data, "type", "");
              JsonObject *payload = json_object_get_object_member (data, "payload");

              if (g_strcmp0 (data_type, "offer") == 0 && payload)
                {
                  const char *sdp = json_object_get_string_member_with_default (payload, "sdp", "");
                  const char *offer_sid = json_object_get_string_member_with_default (data, "sid", NULL);
                  const char *room_type = json_object_get_string_member_with_default (data, "roomType", "video");

                  g_message ("Received offer: roomType=%s, sender=%.30s..., sid=%s",
                             room_type,
                             sender_session_id ? sender_session_id : "NULL",
                             offer_sid ? offer_sid : "NULL");

                  /* Check if this is a screen share offer */
                  if (g_strcmp0 (room_type, "screen") == 0 && sender_session_id)
                    {
                      /* Route screen share offer to screen subscriber - create on-the-fly if needed */
                      CallParticipantRecord *screen_record = gnalk_call_ensure_participant_record (self, sender_session_id);
                      GnalkWebrtc *screen_sub = screen_record->screen_subscriber;

                      if (!screen_sub)
                        {
                          g_message ("Creating screen subscriber on-the-fly for %.30s...", sender_session_id);

                          /* Create screen tile */
                          GnalkParticipantTile *screen_tile = gnalk_participant_tile_new ();
                          gnalk_participant_tile_set_is_screen_share (screen_tile, TRUE);
                          gnalk_participant_tile_set_video_enabled (screen_tile, TRUE);  /* Enable frozen check */
                          gnalk_participant_tile_set_session_id (screen_tile, sender_session_id);

                          if (screen_record->display_name)
                            {
                              g_autofree char *label = g_strdup_printf ("%s's screen", screen_record->display_name);
                              gnalk_participant_tile_set_display_name (screen_tile, label);
                            }
                          else
                            {
                              gnalk_participant_tile_set_display_name (screen_tile, "Screen Share");
                            }

                          /* Store screen tile on record - sink floating ref */
                          screen_record->screen_tile = g_object_ref_sink (screen_tile);

                          /* Create subscriber WebRTC */
                          screen_sub = gnalk_webrtc_new ();

                          /* Configure STUN/TURN */
                          GnalkApplication *app = GNALK_APPLICATION (g_application_get_default ());
                          if (app)
                            {
                              const char *stun = gnalk_application_get_stun_server (app);
                              const char *turn = gnalk_application_get_turn_server (app);
                              const char *turn_user = gnalk_application_get_turn_username (app);
                              const char *turn_cred = gnalk_application_get_turn_credential (app);

                              if (stun)
                                gnalk_webrtc_set_stun_server (screen_sub, stun);
                              if (turn && turn_user && turn_cred)
                                gnalk_webrtc_set_turn_server (screen_sub, turn, turn_user, turn_cred);
                            }

                          /* Set up callbacks */
                          TileVideoCallbackData *video_cb_data = g_new0 (TileVideoCallbackData, 1);
                          g_weak_ref_init (&video_cb_data->window_ref, self);
                          video_cb_data->session_id = g_strdup (sender_session_id);

                          TileVideoCallbackData *signal_cb_data = g_new0 (TileVideoCallbackData, 1);
                          g_weak_ref_init (&signal_cb_data->window_ref, self);
                          signal_cb_data->session_id = g_strdup (sender_session_id);

                          TileVideoCallbackData *conn_cb_data = g_new0 (TileVideoCallbackData, 1);
                          g_weak_ref_init (&conn_cb_data->window_ref, self);
                          conn_cb_data->session_id = g_strdup (sender_session_id);

                          gnalk_webrtc_set_remote_video_callback (screen_sub, gnalk_call_participant_on_screen_remote_video_frame, video_cb_data);
                          gnalk_webrtc_set_signal_callback (screen_sub, gnalk_call_signaling_on_screen_subscriber_signal, signal_cb_data);
                          gnalk_webrtc_set_connection_state_callback (screen_sub, gnalk_call_signaling_on_screen_subscriber_connection_state, conn_cb_data);

                          /* Enable continuous keyframe requests for screen share quality */
                          gnalk_webrtc_enable_continuous_pli (screen_sub, TRUE, 2000);

                          screen_record->screen_subscriber = screen_sub;

                          gnalk_call_layout_recalculate (self);
                        }

                      /* Check if we've already processed an offer for this subscriber.
                       * If the new offer has a DIFFERENT sid, we must accept it - the peer
                       * may have restarted their screen share or the server reassigned the session.
                       * Ignoring new sids causes signaling failures ("processing_failed"). */
                      const char *existing_sid = screen_record->screen_subscriber_sid;
                      if (existing_sid && existing_sid[0] != '\0' &&
                          offer_sid && g_strcmp0 (existing_sid, offer_sid) == 0)
                        {
                          /* Same sid - this is a true duplicate, ignore it */
                          g_message ("Ignoring duplicate screen share offer from %.30s... (same sid %s)",
                                     sender_session_id, existing_sid);
                        }
                      else
                        {
                          if (existing_sid && existing_sid[0] != '\0')
                            {
                              /* Different sid - peer restarted screen share or server reassigned.
                               * We must accept the new sid to match what the server expects. */
                              g_message ("Screen share sid changed for %.30s... (old=%s, new=%s) - accepting new offer",
                                         sender_session_id, existing_sid, offer_sid ? offer_sid : "NULL");
                            }
                          else
                            {
                              g_message ("Routing screen share offer to screen_subscriber for %.30s...", sender_session_id);
                            }
                          /* Store/update SID for sending answer back */
                          g_free (screen_record->screen_subscriber_sid);
                          screen_record->screen_subscriber_sid = g_strdup (offer_sid ? offer_sid : "");
                          gnalk_webrtc_handle_offer (screen_sub, sdp);
                        }
                    }
                  else
                    {
                      /* Check if this offer is for a tile subscription (from peer we requested) */
                      CallParticipantRecord *offer_record = sender_session_id ?
                        g_hash_table_lookup (self->participants, sender_session_id) : NULL;
                      GnalkParticipantTile *tile = offer_record ? offer_record->tile : NULL;

                      if (tile)
                        {
                          /* Store the SID from the offer to use in our answer */
                          gnalk_participant_tile_set_subscriber_sid (tile, offer_sid);
                          GnalkWebrtc *subscriber = gnalk_participant_tile_get_webrtc (tile);
                          g_message ("Routing offer to tile's subscriber=%p for session=%.30s...",
                                     (void *)subscriber, sender_session_id);
                          if (subscriber)
                            gnalk_webrtc_handle_offer (subscriber, sdp);
                        }
                      else if (self->webrtc)
                        {
                          /* Offer for publisher */
                          gnalk_webrtc_handle_offer (self->webrtc, sdp);
                        }
                    }
                  g_object_set (self, "call-status", _("In call"), NULL);
                }
              else if (g_strcmp0 (data_type, "answer") == 0 && payload)
                {
                  const char *sdp = json_object_get_string_member_with_default (payload, "sdp", "");
                  const char *room_type = json_object_get_string_member_with_default (data, "roomType", "video");

                  /* Route answer based on roomType */
                  if (g_strcmp0 (room_type, "screen") == 0 && self->screen_publisher && self->is_sharing_screen)
                    {
                      g_message ("Routing screen share answer to screen_publisher");
                      gnalk_webrtc_handle_answer (self->screen_publisher, sdp);

                      /* After MCU accepts our screen share, send sendoffer to all participants
                       * so they can subscribe to our screen share stream */
                      gnalk_call_screen_share_send_offers_to_all (self);
                    }
                  else if (self->webrtc)
                    {
                      gnalk_webrtc_handle_answer (self->webrtc, sdp);
                    }
                  g_object_set (self, "call-status", _("In call"), NULL);
                }
              else if (g_strcmp0 (data_type, "candidate") == 0 && payload)
                {
                  const char *candidate = NULL;
                  int mline_index = 0;
                  const char *candidate_sid = json_object_get_string_member_with_default (data, "sid", NULL);

                  /* Check if candidate is nested inside payload.candidate object */
                  JsonNode *candidate_node = json_object_get_member (payload, "candidate");
                  if (candidate_node && JSON_NODE_HOLDS_OBJECT (candidate_node))
                    {
                      JsonObject *candidate_obj = json_node_get_object (candidate_node);
                      candidate = json_object_get_string_member_with_default (candidate_obj, "candidate", "");
                      mline_index = json_object_get_int_member_with_default (candidate_obj, "sdpMLineIndex", 0);
                    }
                  else
                    {
                      /* Flat structure: candidate directly in payload */
                      candidate = json_object_get_string_member_with_default (payload, "candidate", "");
                      mline_index = json_object_get_int_member_with_default (payload, "sdpMLineIndex", 0);
                    }

                  if (candidate && *candidate)
                    {
                      /* Check roomType to route to correct WebRTC instance */
                      const char *room_type = json_object_get_string_member_with_default (data, "roomType", "video");

                      if (g_strcmp0 (room_type, "screen") == 0)
                        {
                          /* Screen share candidates - route based on direction */
                          if (self->screen_publisher && self->is_sharing_screen)
                            {
                              /* We're sharing - route to our publisher */
                              gnalk_webrtc_handle_ice_candidate (self->screen_publisher, candidate, mline_index, NULL);
                            }
                          else if (sender_session_id)
                            {
                              /* We're receiving - check if candidate's SID matches the offer we accepted. */
                              CallParticipantRecord *ice_record = g_hash_table_lookup (self->participants, sender_session_id);
                              const char *stored_sid = ice_record ? ice_record->screen_subscriber_sid : NULL;
                              if (stored_sid && candidate_sid && g_strcmp0 (stored_sid, candidate_sid) != 0)
                                {
                                  g_message ("Ignoring screen ICE candidate with mismatched sid (have %s, got %s)",
                                             stored_sid, candidate_sid);
                                }
                              else
                                {
                                  GnalkWebrtc *screen_sub = ice_record ? ice_record->screen_subscriber : NULL;
                                  if (screen_sub)
                                    {
                                      g_message ("Routing screen ICE candidate to screen_subscriber for %.30s...", sender_session_id);
                                      gnalk_webrtc_handle_ice_candidate (screen_sub, candidate, mline_index, NULL);
                                    }
                                }
                            }
                        }
                      else
                        {
                          /* Video/audio candidates - route to correct WebRTC instance based on sender */
                          CallParticipantRecord *ice_record = sender_session_id ?
                            g_hash_table_lookup (self->participants, sender_session_id) : NULL;
                          GnalkParticipantTile *tile = ice_record ? ice_record->tile : NULL;

                          if (tile)
                            {
                              GnalkWebrtc *subscriber = gnalk_participant_tile_get_webrtc (tile);
                              if (subscriber)
                                gnalk_webrtc_handle_ice_candidate (subscriber, candidate, mline_index, NULL);
                            }
                          else if (self->webrtc)
                            {
                              gnalk_webrtc_handle_ice_candidate (self->webrtc, candidate, mline_index, NULL);
                            }
                        }
                    }
                }
              else if (g_strcmp0 (data_type, "mute") == 0 && payload && sender_session_id)
                {
                  /* Remote participant muted their media */
                  const char *media_name = json_object_get_string_member_with_default (payload, "name", "");
                  CallParticipantRecord *mute_record = g_hash_table_lookup (self->participants, sender_session_id);
                  if (mute_record && mute_record->tile)
                    {
                      g_message ("Peer %s muted %s", sender_session_id, media_name);
                      if (g_strcmp0 (media_name, "video") == 0)
                        gnalk_participant_tile_set_video_enabled (mute_record->tile, FALSE);
                      else if (g_strcmp0 (media_name, "audio") == 0)
                        gnalk_participant_tile_set_audio_enabled (mute_record->tile, FALSE);
                    }
                }
              else if (g_strcmp0 (data_type, "unmute") == 0 && payload && sender_session_id)
                {
                  /* Remote participant unmuted their media */
                  const char *media_name = json_object_get_string_member_with_default (payload, "name", "");
                  CallParticipantRecord *unmute_record = g_hash_table_lookup (self->participants, sender_session_id);
                  if (unmute_record && unmute_record->tile)
                    {
                      g_message ("Peer %s unmuted %s", sender_session_id, media_name);
                      if (g_strcmp0 (media_name, "video") == 0)
                        {
                          gnalk_participant_tile_set_video_enabled (unmute_record->tile, TRUE);
                          GnalkWebrtc *subscriber = gnalk_participant_tile_get_webrtc (unmute_record->tile);
                          if (subscriber)
                            gnalk_webrtc_request_keyframe (subscriber);
                        }
                      else if (g_strcmp0 (media_name, "audio") == 0)
                        gnalk_participant_tile_set_audio_enabled (unmute_record->tile, TRUE);
                    }
                }
            }
        }
    }
}

static void
on_http_get_participants_response (GnalkHttpClient *client,
                                   JsonNode         *response,
                                   GError           *error,
                                   gpointer          user_data)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (user_data);

  (void)client;

  if (error)
    {
      g_warning ("Failed to get participants: %s", error->message);
      g_object_unref (self);
      return;
    }

  /* Parse participants response and extract userId -> displayName */
  if (!response || !JSON_NODE_HOLDS_OBJECT (response))
    {
      g_object_unref (self);
      return;
    }

  JsonObject *root = json_node_get_object (response);
  JsonObject *ocs = json_object_get_object_member (root, "ocs");
  if (!ocs)
    {
      g_object_unref (self);
      return;
    }

  JsonArray *data = json_object_get_array_member (ocs, "data");
  if (!data)
    {
      g_object_unref (self);
      return;
    }

  guint len = json_array_get_length (data);
  for (guint i = 0; i < len; i++)
    {
      JsonObject *participant = json_array_get_object_element (data, i);
      if (!participant)
        continue;

      const char *actor_id = json_object_get_string_member_with_default (participant, "actorId", NULL);
      const char *display_name = json_object_get_string_member_with_default (participant, "displayName", NULL);

      if (actor_id && display_name)
        {
          g_hash_table_insert (self->user_display_names, g_strdup (actor_id), g_strdup (display_name));
          g_message ("Cached display name: %s -> %s", actor_id, display_name);
        }
    }

  g_object_unref (self);
}

static void
on_http_join_call_response (GnalkHttpClient *client,
                            JsonNode         *response,
                            GError           *error,
                            gpointer          user_data)
{
  GnalkCallWindow *self = GNALK_CALL_WINDOW (user_data);

  if (error)
    {
      g_warning ("Failed to join call via HTTP: %s", error->message);
      g_object_set (self, "call-status", _("Failed to join call"), NULL);
      g_object_unref (self);
      return;
    }

  g_message ("Successfully joined call via HTTP");
  g_object_set (self, "call-status", _("Waiting for peers..."), NULL);

  g_object_unref (self);
}

void
gnalk_call_window_start_call (GnalkCallWindow  *self,
                               const char        *room_token,
                               const char        *room_name,
                               const char        *session_id,
                               GnalkHttpClient  *http_client,
                               GnalkWebrtc      *webrtc,
                               GnalkSignaling   *signaling,
                               GnalkAvatarCache *avatar_cache,
                               gboolean           start_with_video,
                               gboolean           is_federated)
{
  g_return_if_fail (GNALK_IS_CALL_WINDOW (self));

  g_free (self->room_token);
  self->room_token = g_strdup (room_token);

  g_free (self->session_id);
  self->session_id = g_strdup (session_id);

  self->is_federated = is_federated;

  g_object_set (self, "room-name", room_name ? room_name : "", NULL);

  g_set_object (&self->http_client, http_client);
  g_set_object (&self->webrtc, webrtc);
  g_set_object (&self->signaling, signaling);
  g_set_object (&self->avatar_cache, avatar_cache);

  /* Set up video callbacks on publisher webrtc */
  if (webrtc)
    {
      /* Set camera position from window's default before any other setup */
      gnalk_webrtc_set_camera_position (webrtc, self->camera_position);
      /* Local preview from WebRTC tee - called when WebRTC pipeline starts streaming */
      gnalk_webrtc_set_local_video_callback (webrtc, gnalk_call_participant_on_local_video_frame, self);
    }

  if (!http_client)
    {
      g_object_set (self, "call-status", _("HTTP client not available"), NULL);
      g_warning ("Cannot start call: HTTP client is NULL");
      return;
    }

  if (!signaling || !gnalk_signaling_is_connected (signaling))
    {
      g_object_set (self, "call-status", _("Not connected to signaling server"), NULL);
      g_warning ("Cannot start call: WebSocket signaling not connected");
      return;
    }

  g_object_set (self, "call-status", _("Starting call..."), NULL);

  /* Store and apply initial video state */
  self->video_enabled = start_with_video;
  if (!start_with_video)
    {
      /* Start with video disabled - set toggle button to muted state */
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->mute_video_button), TRUE);
      gtk_button_set_icon_name (GTK_BUTTON (self->mute_video_button), "camera-disabled-symbolic");
    }

#ifdef HAVE_GSTREAMER
  /* Start local camera preview immediately (only if video is enabled).
   * This simple pipeline (camera → appsink) shows the preview while waiting for peers.
   * When WebRTC call actually starts, we stop this preview and switch to the WebRTC
   * pipeline which uses a tee to provide both streaming and local preview. */
  if (start_with_video)
    gnalk_call_preview_start_local_preview (self);
#endif

  /* The signaling room is already joined when the conversation was selected
   * (in gnalk-window.c). This established the session ID mapping which is
   * required for the signaling server to process inCall updates correctly.
   *
   * Now we just need to:
   * 1. Get MCU capability and signaling session ID from signaling server
   * 2. Set up the message callback for call signaling
   * 3. Call HTTP API to set our inCall status */
  self->room_joined = TRUE;  /* Already joined from window */
  self->has_mcu = gnalk_signaling_has_mcu (signaling);

  /* Get our signaling session ID (from the hello response) */
  const char *sig_session_id = gnalk_signaling_get_session_id (signaling);
  if (sig_session_id)
    {
      g_free (self->signaling_session_id);
      self->signaling_session_id = g_strdup (sig_session_id);
    }

  gnalk_signaling_set_message_callback (signaling, on_signaling_message, self);

  /* Connect to screen share signal from signaling */
  self->screen_share_handler_id = g_signal_connect (signaling, "screen-share-changed",
                                                     G_CALLBACK (gnalk_call_screen_share_on_signaling_changed), self);

  /* Connect to hand raise and reaction signals */
  self->hand_raised_handler_id = g_signal_connect (signaling, "hand-raised",
                                                    G_CALLBACK (on_hand_raised), self);
  self->call_reaction_handler_id = g_signal_connect (signaling, "call-reaction",
                                                      G_CALLBACK (on_call_reaction), self);

  /* Fetch participants list to get display names */
  gnalk_http_client_get_participants (http_client, room_token, on_http_get_participants_response, g_object_ref (self));

  /* Call HTTP API to set inCall status - this notifies Nextcloud we're in the call.
   * The session was already established when joining the room in gnalk-window.c,
   * and the signaling server has mapped it to our signaling session ID.
   * Do NOT refresh the session here as that would create a new HTTP session
   * that doesn't match the signaling session, causing "not allowed" errors.
   * Always use flags=7 (audio+video+screenshare) for full capabilities.
   * Video on/off is communicated via mute/unmute peer messages. */
  /* Always use the regular call endpoint — for federated rooms, the local
   * Nextcloud server proxies the request to the remote server automatically. */
  gnalk_http_client_start_call (http_client, room_token, 7, on_http_join_call_response, g_object_ref (self));
}

void
gnalk_call_window_end_call (GnalkCallWindow *self)
{
  g_return_if_fail (GNALK_IS_CALL_WINDOW (self));

  /* Guard against multiple calls */
  if (self->call_ending)
    {
      g_message ("end_call: already ending, skipping duplicate call");
      return;
    }
  self->call_ending = TRUE;

  g_message ("end_call: starting cleanup");

  /* Clear callbacks first to prevent use-after-free from pending idle sources */
  g_message ("end_call: clearing signaling callback");
  if (self->signaling)
    gnalk_signaling_set_message_callback (self->signaling, NULL, NULL);

  g_message ("end_call: clearing webrtc local video callback");
  if (self->webrtc)
    gnalk_webrtc_set_local_video_callback (self->webrtc, NULL, NULL);

  /* Cancel pending layout recalculation */
  g_message ("end_call: cancelling layout idle (id=%u)", self->layout_idle_id);
  if (self->layout_idle_id > 0)
    {
      g_source_remove (self->layout_idle_id);
      self->layout_idle_id = 0;
    }

  /* Stop the unmute timer */
  g_message ("end_call: stopping unmute timer (id=%u)", self->unmute_timer_id);
  if (self->unmute_timer_id > 0)
    {
      g_source_remove (self->unmute_timer_id);
      self->unmute_timer_id = 0;
    }

  /* Cancel pending WebRTC start */
  g_message ("end_call: cancelling webrtc start timeout (id=%u)", self->start_webrtc_timeout_id);
  if (self->start_webrtc_timeout_id > 0)
    {
      g_source_remove (self->start_webrtc_timeout_id);
      self->start_webrtc_timeout_id = 0;
    }

  g_message ("end_call: ending main webrtc connection");
  if (self->webrtc)
    gnalk_webrtc_end_call (self->webrtc);

  /* End all subscriber WebRTC connections, clear tiles and screen shares */
  if (self->participants)
    {
      g_message ("end_call: clearing %u participant records", g_hash_table_size (self->participants));
      GHashTableIter iter;
      gpointer key, value;
      g_hash_table_iter_init (&iter, self->participants);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          CallParticipantRecord *record = value;
          g_message ("end_call: cleaning up participant session=%s", (char *)key);

          /* Clean up main tile */
          if (record->tile)
            {
              GnalkWebrtc *subscriber = gnalk_participant_tile_get_webrtc (record->tile);
              if (subscriber)
                gnalk_webrtc_end_call (subscriber);
              gnalk_call_layout_remove_tile_from_parent (self, GTK_WIDGET (record->tile));
            }

          /* Clean up screen tile and subscriber */
          if (record->screen_subscriber)
            gnalk_webrtc_end_call (record->screen_subscriber);
          if (record->screen_tile)
            gnalk_call_layout_remove_tile_from_parent (self, GTK_WIDGET (record->screen_tile));
        }
      g_hash_table_remove_all (self->participants);
      g_message ("end_call: all participants cleared");
    }

  /* Unparent local tile as well */
  if (self->local_tile)
    gnalk_call_layout_remove_tile_from_parent (self, GTK_WIDGET (self->local_tile));
  self->local_tile = NULL;
  g_clear_pointer (&self->active_speaker_id, g_free);

  /* Stop outgoing screen sharing */
  if (self->is_sharing_screen && self->screen_capture)
    {
      g_message ("end_call: stopping screen sharing");
      gnalk_screen_capture_stop (self->screen_capture);
    }

  /* End screen publisher WebRTC connection */
  if (self->screen_publisher)
    {
      g_message ("end_call: ending screen publisher");
      gnalk_webrtc_end_call (self->screen_publisher);
      g_clear_object (&self->screen_publisher);
    }
  g_clear_pointer (&self->screen_publisher_sid, g_free);
  self->is_sharing_screen = FALSE;

  /* Reset screen share button state */
  if (self->screen_share_button)
    {
      g_signal_handlers_block_by_func (self->screen_share_button,
                                       gnalk_call_screen_share_on_toggled, self);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->screen_share_button), FALSE);
      gtk_button_set_icon_name (GTK_BUTTON (self->screen_share_button),
                                "preferences-desktop-display-symbolic");
      g_signal_handlers_unblock_by_func (self->screen_share_button,
                                         gnalk_call_screen_share_on_toggled, self);
    }

  /* Don't leave the signaling room - it's shared with the conversation.
   * We only leave the call via HTTP API (which updates inCall status). */

  if (self->http_client && self->room_token)
    {
      /* Always use the regular call endpoint — for federated rooms, the local
       * Nextcloud server proxies the leave request to the remote server. */
      gnalk_http_client_leave_call (self->http_client, self->room_token, NULL, NULL);
    }

  /* Reset call state so we can start a new call */
  self->offer_sent = FALSE;
  self->publisher_connected = FALSE;
  g_ptr_array_set_size (self->peer_session_ids, 0);
  g_clear_pointer (&self->publisher_sid, g_free);
}
