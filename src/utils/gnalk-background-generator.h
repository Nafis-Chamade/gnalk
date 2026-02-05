#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/**
 * gnalk_background_generator_create_avatar_mosaic:
 * @textures: (element-type GdkTexture): Array of avatar textures
 * @tile_size: Size of the output tile (e.g., 128 for 128x128)
 * @avatar_size: Size of each avatar in the mosaic (e.g., 32)
 * @opacity: Opacity of the avatars (0.0 to 1.0, recommend 0.08-0.12)
 *
 * Creates a tileable mosaic pattern from participant avatars.
 * Avatars are converted to light grayscale for a subtle watermark effect.
 *
 * Returns: (transfer full) (nullable): A new GdkTexture containing the mosaic tile
 */
GdkTexture *gnalk_background_generator_create_avatar_mosaic (GPtrArray *textures,
                                                              int        tile_size,
                                                              int        avatar_size,
                                                              double     opacity);

/**
 * gnalk_background_generator_create_css_provider:
 * @texture: The mosaic texture to use as background
 *
 * Creates a CSS provider that applies the texture as a repeating background
 * for the .message-list.bg-mosaic class.
 *
 * Returns: (transfer full) (nullable): A new GtkCssProvider
 */
GtkCssProvider *gnalk_background_generator_create_css_provider (GdkTexture *texture);

G_END_DECLS
