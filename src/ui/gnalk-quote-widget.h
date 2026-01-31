/* gnalk-quote-widget.h
 *
 * Widget displaying a quoted/replied-to message.
 * Shows avatar, username, preview, and message text.
 */

#pragma once

#include <gtk/gtk.h>
#include "gnalk-avatar-cache.h"

G_BEGIN_DECLS

#define GNALK_TYPE_QUOTE_WIDGET (gnalk_quote_widget_get_type ())
G_DECLARE_FINAL_TYPE (GnalkQuoteWidget, gnalk_quote_widget, GNALK, QUOTE_WIDGET, GtkBox)

GnalkQuoteWidget *gnalk_quote_widget_new (void);

/**
 * gnalk_quote_widget_set_quote:
 * @self: the quote widget
 * @parent_id: ID of the quoted message (for scroll-to-message signal)
 * @author_id: author's user ID (for avatar loading)
 * @author_name: display name of the quoted message author
 * @message: quoted message text
 * @avatar_cache: (nullable): avatar cache for loading author avatar
 *
 * Sets the quote content. Shows the widget if content is valid.
 */
void gnalk_quote_widget_set_quote (GnalkQuoteWidget *self,
                                    gint64             parent_id,
                                    const char        *author_id,
                                    const char        *author_name,
                                    const char        *message,
                                    GnalkAvatarCache *avatar_cache);

/**
 * gnalk_quote_widget_set_file_preview:
 * @self: the quote widget
 * @file_id: file ID for preview loading
 * @avatar_cache: avatar cache for loading file preview
 *
 * Sets the file preview thumbnail for quoted file messages.
 */
void gnalk_quote_widget_set_file_preview (GnalkQuoteWidget *self,
                                           const char        *file_id,
                                           GnalkAvatarCache *avatar_cache);

/**
 * gnalk_quote_widget_reset:
 * @self: the quote widget
 *
 * Clears the quote and hides the widget.
 */
void gnalk_quote_widget_reset (GnalkQuoteWidget *self);

G_END_DECLS
