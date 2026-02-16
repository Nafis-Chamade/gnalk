#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNALK_TYPE_MESSAGE_VIEW (gnalk_message_view_get_type ())
G_DECLARE_FINAL_TYPE (GnalkMessageView, gnalk_message_view, GNALK, MESSAGE_VIEW, GtkBox)

GnalkMessageView *gnalk_message_view_new (void);

void gnalk_message_view_set_model (GnalkMessageView  *self,
                                     GtkSelectionModel  *model,
                                     GtkListItemFactory *factory);

void gnalk_message_view_show_empty (GnalkMessageView *self);

void gnalk_message_view_show_messages (GnalkMessageView *self);

/* Show messages view already positioned at bottom (no visual jump) */
void gnalk_message_view_show_messages_at_bottom (GnalkMessageView *self);

/* Show messages view positioned at specific item (no visual jump) */
void gnalk_message_view_show_messages_at_item (GnalkMessageView *self,
                                                  guint              position);

void gnalk_message_view_scroll_to_bottom (GnalkMessageView *self);

void gnalk_message_view_scroll_to_item (GnalkMessageView *self,
                                          guint              position,
                                          GtkListScrollFlags flags);

GtkListView *gnalk_message_view_get_list_view (GnalkMessageView *self);

GtkScrolledWindow *gnalk_message_view_get_scrolled_window (GnalkMessageView *self);

void gnalk_message_view_set_loading_older (GnalkMessageView *self,
                                             gboolean           loading);

gboolean gnalk_message_view_get_loading_older (GnalkMessageView *self);

gboolean gnalk_message_view_is_at_bottom (GnalkMessageView *self);

/* Check if an item at the given position is currently visible in the viewport */
gboolean gnalk_message_view_is_item_visible (GnalkMessageView *self,
                                               guint              position);

/* Get viewport dimensions for message height measurement */
void gnalk_message_view_get_viewport_size (GnalkMessageView *self,
                                             int               *width,
                                             int               *height);

/* Get current scroll position (for anchor capture) */
gdouble gnalk_message_view_get_scroll_position (GnalkMessageView *self);

/* Set scroll position (for anchor restore) */
void gnalk_message_view_set_scroll_position (GnalkMessageView *self,
                                               gdouble            position);

/* Get position of first visible item (approximate) */
guint gnalk_message_view_get_first_visible_position (GnalkMessageView *self);

/**
 * gnalk_message_view_set_mosaic_avatars:
 * @self: The message view
 * @textures: (element-type GdkTexture) (nullable): Array of participant avatar textures
 * @room_avatar: (nullable): Room avatar texture to include in the mosaic
 *
 * Sets the participant avatars for the mosaic background pattern.
 * When the "Mosaic" background style is selected, these avatars will be
 * displayed as a light grayscale tiled pattern behind the messages.
 */
void gnalk_message_view_set_mosaic_avatars (GnalkMessageView *self,
                                             GPtrArray        *textures,
                                             GdkTexture       *room_avatar);

/* Context mode (jump-to-message): changes scroll-to-bottom button to "jump to present" */
void gnalk_message_view_set_context_mode (GnalkMessageView *self,
                                          gboolean          context_mode);

/* Thread mode: shows a banner with back button and "Thread" title */
void gnalk_message_view_set_thread_mode (GnalkMessageView *self,
                                         gboolean          thread_mode);

G_END_DECLS
