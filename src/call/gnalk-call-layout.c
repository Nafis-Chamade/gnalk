/* gnalk-call-layout.c
 *
 * Layout management for the call window.
 * Handles grid and speaker focus layouts for participant tiles.
 */

#include "gnalk-call-window-private.h"

/* Internal functions for layout recalculation */
static void update_grid_layout (GnalkCallWindow *self);
static void update_speaker_layout (GnalkCallWindow *self);

/* Idle callback to recalculate layout with weak reference handling */
static gboolean
recalculate_layout_idle (gpointer user_data)
{
  GnalkCallWindow *self = g_weak_ref_get (user_data);
  if (self)
    {
      self->layout_idle_id = 0;
      gnalk_call_layout_recalculate (self);
      g_object_unref (self);
    }
  g_weak_ref_clear (user_data);
  g_free (user_data);
  return G_SOURCE_REMOVE;
}

/* Schedule layout recalculation in next idle iteration */
void
gnalk_call_layout_schedule_recalculation (GnalkCallWindow *self)
{
  /* If already scheduled, don't add another */
  if (self->layout_idle_id > 0)
    return;

  GWeakRef *weak_ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (weak_ref, self);
  self->layout_idle_id = g_idle_add (recalculate_layout_idle, weak_ref);
}

/* Update grid layout based on participant count */
static void
update_grid_layout (GnalkCallWindow *self)
{
  guint count = 0;
  guint screen_count = 0;
  {
    GHashTableIter count_iter;
    gpointer k, v;
    g_hash_table_iter_init (&count_iter, self->participants);
    while (g_hash_table_iter_next (&count_iter, &k, &v))
      {
        CallParticipantRecord *rec = v;
        if (rec->tile)
          count++;
        if (rec->screen_tile)
          screen_count++;
      }
  }
  if (self->local_tile)
    count++;
  count += screen_count;

  /* Get window dimensions to determine optimal layout */
  int window_width = gtk_widget_get_width (GTK_WIDGET (self));
  int window_height = gtk_widget_get_height (GTK_WIDGET (self));
  gboolean is_landscape = window_width > window_height;

  /* Calculate grid dimensions */
  gint cols;
  if (count <= 1)
    {
      cols = 1;
    }
  else if (count <= 2)
    {
      /* Split based on window orientation */
      cols = is_landscape ? 2 : 1;
    }
  else if (count <= 4)
    {
      cols = 2;
    }
  else
    {
      cols = 3;
    }

  /* Remove all children from grid first */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->video_grid)) != NULL)
    gtk_grid_remove (GTK_GRID (self->video_grid), child);

  /* Add tiles to grid */
  gint col = 0, row = 0;

  /* Add screen share tiles first (most important) */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, self->participants);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      CallParticipantRecord *record = value;
      if (!record->screen_tile)
        continue;
      gtk_grid_attach (GTK_GRID (self->video_grid),
                       GTK_WIDGET (record->screen_tile),
                       col, row, 1, 1);
      col++;
      if (col >= cols)
        {
          col = 0;
          row++;
        }
    }

  /* Add local tile */
  if (self->local_tile)
    {
      gtk_grid_attach (GTK_GRID (self->video_grid),
                       GTK_WIDGET (self->local_tile),
                       col, row, 1, 1);
      col++;
      if (col >= cols)
        {
          col = 0;
          row++;
        }
    }

  /* Add remote participant tiles */
  g_hash_table_iter_init (&iter, self->participants);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      CallParticipantRecord *record = value;
      if (!record->tile)
        continue;
      gtk_grid_attach (GTK_GRID (self->video_grid),
                       GTK_WIDGET (record->tile),
                       col, row, 1, 1);
      col++;
      if (col >= cols)
        {
          col = 0;
          row++;
        }
    }
}

/* Update speaker focus layout */
static void
update_speaker_layout (GnalkCallWindow *self)
{
  g_message ("update_speaker_layout: participants=%u, local_tile=%p, active_speaker=%s",
             g_hash_table_size (self->participants),
             (void *)self->local_tile,
             self->active_speaker_id ? self->active_speaker_id : "none");

  /* Remove all children from speaker containers */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->speaker_main)) != NULL)
    gtk_box_remove (GTK_BOX (self->speaker_main), child);
  while ((child = gtk_widget_get_first_child (self->speaker_strip)) != NULL)
    gtk_box_remove (GTK_BOX (self->speaker_strip), child);

  /* Find the main tile - prioritize screen share over speaker */
  GnalkParticipantTile *main_tile = NULL;

  /* If there's a screen share, show it as main */
  {
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init (&iter, self->participants);
    while (g_hash_table_iter_next (&iter, &key, &value))
      {
        CallParticipantRecord *record = value;
        if (record->screen_tile)
          {
            main_tile = record->screen_tile;
            break;
          }
      }
  }

  /* Otherwise use active speaker */
  if (!main_tile && self->active_speaker_id)
    {
      CallParticipantRecord *speaker_record = g_hash_table_lookup (self->participants, self->active_speaker_id);
      if (speaker_record && speaker_record->tile)
        main_tile = speaker_record->tile;
    }

  /* Fall back to first remote participant if no active speaker */
  if (!main_tile)
    {
      GHashTableIter iter;
      gpointer key, value;
      g_hash_table_iter_init (&iter, self->participants);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          CallParticipantRecord *record = value;
          if (record->tile)
            {
              main_tile = record->tile;
              break;
            }
        }
    }

  /* If no remote participants, show local in main */
  if (!main_tile && self->local_tile)
    main_tile = self->local_tile;

  g_message ("update_speaker_layout: main_tile=%p", (void *)main_tile);

  if (main_tile)
    {
      gtk_widget_add_css_class (GTK_WIDGET (main_tile), "speaker-main-tile");
      gtk_box_append (GTK_BOX (self->speaker_main), GTK_WIDGET (main_tile));
    }

  /* Add remaining tiles to strip */
  if (self->local_tile && self->local_tile != main_tile)
    {
      gtk_widget_remove_css_class (GTK_WIDGET (self->local_tile), "speaker-main-tile");
      gtk_widget_add_css_class (GTK_WIDGET (self->local_tile), "speaker-strip-tile");
      gtk_box_append (GTK_BOX (self->speaker_strip), GTK_WIDGET (self->local_tile));
    }

  GHashTableIter iter;
  gpointer key, value;

  /* Add screen tiles and participant tiles to strip (except main) */
  g_hash_table_iter_init (&iter, self->participants);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      CallParticipantRecord *record = value;

      /* Screen tile to strip if present and not main */
      if (record->screen_tile && record->screen_tile != main_tile)
        {
          gtk_widget_remove_css_class (GTK_WIDGET (record->screen_tile), "speaker-main-tile");
          gtk_widget_add_css_class (GTK_WIDGET (record->screen_tile), "speaker-strip-tile");
          gtk_box_append (GTK_BOX (self->speaker_strip), GTK_WIDGET (record->screen_tile));
        }

      /* Participant tile to strip if present and not main */
      if (record->tile && record->tile != main_tile)
        {
          gtk_widget_remove_css_class (GTK_WIDGET (record->tile), "speaker-main-tile");
          gtk_widget_add_css_class (GTK_WIDGET (record->tile), "speaker-strip-tile");
          gtk_box_append (GTK_BOX (self->speaker_strip), GTK_WIDGET (record->tile));
        }
    }
}

/* Remove a tile from its current parent container */
void
gnalk_call_layout_remove_tile_from_parent (GnalkCallWindow *self, GtkWidget *tile)
{
  GtkWidget *parent = gtk_widget_get_parent (tile);
  if (!parent)
    return;

  if (parent == self->video_grid)
    gtk_grid_remove (GTK_GRID (self->video_grid), tile);
  else if (parent == self->speaker_main)
    gtk_box_remove (GTK_BOX (self->speaker_main), tile);
  else if (parent == self->speaker_strip)
    gtk_box_remove (GTK_BOX (self->speaker_strip), tile);
}

/* Recalculate the layout based on current mode */
void
gnalk_call_layout_recalculate (GnalkCallWindow *self)
{
  /* Safety check - don't layout if widgets aren't ready */
  if (!self->layout_stack || !self->video_grid || !self->speaker_main || !self->speaker_strip)
    return;

  /* Re-entrancy guard - if we're already in layout, mark that we need another pass */
  if (self->layout_in_progress)
    {
      self->layout_needs_recalc = TRUE;
      return;
    }

  self->layout_in_progress = TRUE;
  self->layout_needs_recalc = FALSE;

  /* Remove all tiles from their current parents first */
  if (self->local_tile)
    gnalk_call_layout_remove_tile_from_parent (self, GTK_WIDGET (self->local_tile));

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, self->participants);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      CallParticipantRecord *record = value;
      if (record->tile)
        gnalk_call_layout_remove_tile_from_parent (self, GTK_WIDGET (record->tile));
      if (record->screen_tile)
        gnalk_call_layout_remove_tile_from_parent (self, GTK_WIDGET (record->screen_tile));
    }

  if (self->layout_mode == GNALK_LAYOUT_GRID)
    {
      update_grid_layout (self);
      gtk_stack_set_visible_child_name (GTK_STACK (self->layout_stack), "grid");
    }
  else
    {
      update_speaker_layout (self);
      gtk_stack_set_visible_child_name (GTK_STACK (self->layout_stack), "speaker");
    }

  self->layout_in_progress = FALSE;

  /* If layout was requested while we were busy, do another pass */
  if (self->layout_needs_recalc)
    gnalk_call_layout_recalculate (self);
}

/* Layout toggle button clicked handler */
void
gnalk_call_layout_on_toggle_clicked (GtkButton        *button,
                                       GnalkCallWindow *self)
{
  if (self->layout_mode == GNALK_LAYOUT_GRID)
    {
      self->layout_mode = GNALK_LAYOUT_SPEAKER_FOCUS;
      gtk_button_set_icon_name (button, "view-pin-symbolic");
    }
  else
    {
      self->layout_mode = GNALK_LAYOUT_GRID;
      gtk_button_set_icon_name (button, "view-grid-symbolic");
    }

  gnalk_call_layout_recalculate (self);
}
