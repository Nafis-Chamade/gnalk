#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/**
 * gnalk_background_generator_create_avatar_mosaic:
 * @textures: (element-type GdkTexture): Array of avatar textures
 * @canvas_width: Width of the output image
 * @canvas_height: Height of the output image
 * @opacity: Opacity of the avatars (0.0 to 1.0, recommend 0.20-0.30)
 *
 * Creates a mosaic pattern using bin-packing with varied avatar sizes.
 * Avatars are placed in a Tetris-like fashion with sizes 1x, 2x, and 3x.
 * Converted to light grayscale for a subtle watermark effect.
 *
 * Returns: (transfer full) (nullable): A new GdkTexture containing the mosaic
 */
GdkTexture *gnalk_background_generator_create_avatar_mosaic (GPtrArray *textures,
                                                              int        canvas_width,
                                                              int        canvas_height,
                                                              double     opacity);

/**
 * gnalk_background_generator_create_css_provider:
 * @texture: The mosaic texture to use as background
 *
 * Creates a CSS provider that applies the texture as a cover background
 * (no repeat) for the .message-list.bg-mosaic class.
 *
 * Returns: (transfer full) (nullable): A new GtkCssProvider
 */
GtkCssProvider *gnalk_background_generator_create_css_provider (GdkTexture *texture);

G_END_DECLS
