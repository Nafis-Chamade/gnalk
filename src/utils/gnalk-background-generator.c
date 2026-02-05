#include "gnalk-background-generator.h"
#include <cairo.h>

/* Convert a GdkTexture to a Cairo surface */
static cairo_surface_t *
texture_to_cairo_surface (GdkTexture *texture)
{
  int width = gdk_texture_get_width (texture);
  int height = gdk_texture_get_height (texture);

  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
  guchar *data = cairo_image_surface_get_data (surface);
  int stride = cairo_image_surface_get_stride (surface);

  /* Download texture data */
  gdk_texture_download (texture, data, stride);
  cairo_surface_mark_dirty (surface);

  return surface;
}

/* Convert surface to light grayscale in-place */
static void
surface_to_light_grayscale (cairo_surface_t *surface, double opacity)
{
  int width = cairo_image_surface_get_width (surface);
  int height = cairo_image_surface_get_height (surface);
  int stride = cairo_image_surface_get_stride (surface);
  guchar *data = cairo_image_surface_get_data (surface);

  cairo_surface_flush (surface);

  for (int y = 0; y < height; y++)
    {
      guchar *row = data + y * stride;
      for (int x = 0; x < width; x++)
        {
          guchar *pixel = row + x * 4;

          /* Cairo uses BGRA format (premultiplied alpha) */
          guchar b = pixel[0];
          guchar g = pixel[1];
          guchar r = pixel[2];
          guchar a = pixel[3];

          if (a == 0)
            continue;

          /* Unpremultiply */
          if (a < 255)
            {
              r = (r * 255) / a;
              g = (g * 255) / a;
              b = (b * 255) / a;
            }

          /* Convert to grayscale using luminance formula */
          guchar gray = (guchar)(0.299 * r + 0.587 * g + 0.114 * b);

          /* Lighten: blend with white (50% gray, 50% white) */
          guchar light_gray = (guchar)(gray * 0.5 + 255 * 0.5);

          /* Apply opacity to alpha */
          guchar new_alpha = (guchar)(a * opacity);

          /* Premultiply again */
          guchar premult_gray = (light_gray * new_alpha) / 255;

          pixel[0] = premult_gray; /* B */
          pixel[1] = premult_gray; /* G */
          pixel[2] = premult_gray; /* R */
          pixel[3] = new_alpha;    /* A */
        }
    }

  cairo_surface_mark_dirty (surface);
}

/* Draw a single sub-tile at the given offset */
static void
draw_subtile (cairo_t   *cr,
              GPtrArray *textures,
              int        subtile_size,
              int        avatar_size,
              double     opacity,
              int        offset_x,
              int        offset_y,
              guint      subtile_seed)
{
  int cols = subtile_size / avatar_size;
  int rows = subtile_size / avatar_size;
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;

  double cell_width = (double)subtile_size / cols;
  double cell_height = (double)subtile_size / rows;

  /* Starting texture index varies per sub-tile for different avatar order */
  int texture_index = subtile_seed % textures->len;

  for (int row = 0; row < rows; row++)
    {
      for (int col = 0; col < cols; col++)
        {
          GdkTexture *avatar_texture = g_ptr_array_index (textures, texture_index % textures->len);
          texture_index++;

          cairo_surface_t *avatar_surface = texture_to_cairo_surface (avatar_texture);
          surface_to_light_grayscale (avatar_surface, opacity);

          /* Pseudo-random variations - seed includes subtile_seed for variety */
          guint seed = (row * 7 + col * 13 + texture_index * 17 + subtile_seed * 31) % 100;
          double size_factor = 0.8 + (seed % 41) / 100.0;  /* 0.8 to 1.2 */
          double rotation = ((seed % 31) - 15) * G_PI / 180.0;  /* -15° to +15° */

          double center_x = offset_x + col * cell_width + cell_width / 2.0;
          double center_y = offset_y + row * cell_height + cell_height / 2.0;

          int avatar_width = cairo_image_surface_get_width (avatar_surface);
          int avatar_height = cairo_image_surface_get_height (avatar_surface);
          double base_scale = (double)avatar_size / avatar_width;
          double final_scale = base_scale * size_factor;

          cairo_save (cr);
          cairo_translate (cr, center_x, center_y);
          cairo_rotate (cr, rotation);
          cairo_scale (cr, final_scale, final_scale);

          cairo_arc (cr, 0, 0, avatar_width / 2.0, 0, 2 * G_PI);
          cairo_clip (cr);

          cairo_set_source_surface (cr, avatar_surface, -avatar_width / 2.0, -avatar_height / 2.0);
          cairo_paint (cr);
          cairo_restore (cr);

          cairo_surface_destroy (avatar_surface);
        }
    }
}

GdkTexture *
gnalk_background_generator_create_avatar_mosaic (GPtrArray *textures,
                                                  int        subtile_size,
                                                  int        avatar_size,
                                                  double     opacity)
{
  if (!textures || textures->len == 0)
    return NULL;

  /* Create a 3x3 grid of sub-tiles for more variety before repeating */
  const int grid = 3;
  int total_size = subtile_size * grid;

  cairo_surface_t *tile = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                       total_size, total_size);
  cairo_t *cr = cairo_create (tile);

  /* Clear with transparent */
  cairo_set_source_rgba (cr, 0, 0, 0, 0);
  cairo_paint (cr);

  /* Draw 3x3 sub-tiles, each with different seed for variety */
  guint seeds[9] = { 0, 17, 31, 47, 61, 79, 89, 97, 107 };

  for (int sy = 0; sy < grid; sy++)
    {
      for (int sx = 0; sx < grid; sx++)
        {
          int idx = sy * grid + sx;
          draw_subtile (cr, textures, subtile_size, avatar_size, opacity,
                        sx * subtile_size, sy * subtile_size, seeds[idx]);
        }
    }

  cairo_destroy (cr);

  /* Convert Cairo surface to GdkTexture using GBytes */
  cairo_surface_flush (tile);
  guchar *data = cairo_image_surface_get_data (tile);
  int stride = cairo_image_surface_get_stride (tile);

  GBytes *bytes = g_bytes_new (data, stride * total_size);
  GdkTexture *result = gdk_memory_texture_new (total_size, total_size,
                                                GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                                bytes, stride);
  g_bytes_unref (bytes);
  cairo_surface_destroy (tile);

  return result;
}

GtkCssProvider *
gnalk_background_generator_create_css_provider (GdkTexture *texture)
{
  if (!texture)
    return NULL;

  int width = gdk_texture_get_width (texture);
  int height = gdk_texture_get_height (texture);

  /* Save texture to a temporary PNG file */
  g_autofree char *cache_dir = g_build_filename (g_get_user_cache_dir (), "gnalk", NULL);
  g_mkdir_with_parents (cache_dir, 0755);

  g_autofree char *png_path = g_build_filename (cache_dir, "mosaic-bg.png", NULL);

  /* Save texture as PNG */
  gdk_texture_save_to_png (texture, png_path);

  /* Create CSS that uses the PNG */
  g_autofree char *css = g_strdup_printf (
    ".message-list.bg-mosaic {\n"
    "  background-image: url('file://%s');\n"
    "  background-repeat: repeat;\n"
    "  background-size: %dpx %dpx;\n"
    "}\n",
    png_path, width, height);

  GtkCssProvider *provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (provider, css);

  return provider;
}
