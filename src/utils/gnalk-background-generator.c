#include "gnalk-background-generator.h"
#include <cairo.h>
#include <glib/gstdio.h>

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

/* Simple pseudo-random number generator (deterministic for reproducibility) */
static guint32
xorshift32 (guint32 *state)
{
  guint32 x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

/* Draw a single avatar at the given position */
static void
draw_avatar (cairo_t         *cr,
             cairo_surface_t *avatar_surface,
             double           center_x,
             double           center_y,
             double           size,
             double           rotation)
{
  int avatar_width = cairo_image_surface_get_width (avatar_surface);
  int avatar_height = cairo_image_surface_get_height (avatar_surface);
  double scale = size / avatar_width;

  cairo_save (cr);
  cairo_translate (cr, center_x, center_y);
  cairo_rotate (cr, rotation);
  cairo_scale (cr, scale, scale);

  /* Clip to circle */
  cairo_arc (cr, 0, 0, avatar_width / 2.0, 0, 2 * G_PI);
  cairo_clip (cr);

  cairo_set_source_surface (cr, avatar_surface, -avatar_width / 2.0, -avatar_height / 2.0);
  cairo_paint (cr);
  cairo_restore (cr);
}

/* Check if a block of cells is free in the occupancy grid */
static gboolean
cells_free (gboolean *grid, int grid_cols, int grid_rows, int col, int row, int block_size)
{
  if (col + block_size > grid_cols || row + block_size > grid_rows)
    return FALSE;

  for (int dy = 0; dy < block_size; dy++)
    for (int dx = 0; dx < block_size; dx++)
      if (grid[(row + dy) * grid_cols + (col + dx)])
        return FALSE;

  return TRUE;
}

/* Mark a block of cells as occupied */
static void
mark_cells (gboolean *grid, int grid_cols, int col, int row, int block_size)
{
  for (int dy = 0; dy < block_size; dy++)
    for (int dx = 0; dx < block_size; dx++)
      grid[(row + dy) * grid_cols + (col + dx)] = TRUE;
}

GdkTexture *
gnalk_background_generator_create_avatar_mosaic (GPtrArray *textures,
                                                  int        canvas_width,
                                                  int        canvas_height,
                                                  double     opacity)
{
  if (!textures || textures->len == 0)
    return NULL;

  const int cell_size = 40;  /* Base cell size in pixels */
  int grid_cols = canvas_width / cell_size;
  int grid_rows = canvas_height / cell_size;

  if (grid_cols < 1) grid_cols = 1;
  if (grid_rows < 1) grid_rows = 1;

  /* Occupancy grid */
  gboolean *grid = g_new0 (gboolean, grid_cols * grid_rows);

  /* Create the canvas */
  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                          canvas_width, canvas_height);
  cairo_t *cr = cairo_create (surface);

  /* Clear with transparent */
  cairo_set_source_rgba (cr, 0, 0, 0, 0);
  cairo_paint (cr);

  /* Pre-convert all textures to grayscale surfaces */
  GPtrArray *surfaces = g_ptr_array_new ();
  for (guint i = 0; i < textures->len; i++)
    {
      cairo_surface_t *s = texture_to_cairo_surface (g_ptr_array_index (textures, i));
      surface_to_light_grayscale (s, opacity);
      g_ptr_array_add (surfaces, s);
    }

  guint32 rng_state = 42;  /* Seed for reproducible randomness */
  int texture_idx = 0;

  /* Available block sizes: 3x3, 2x2, 1x1 (in cells) */
  int block_sizes[] = { 3, 2, 1 };
  int num_sizes = 3;

  /* First pass: randomly place larger blocks */
  for (int row = 0; row < grid_rows; row++)
    {
      for (int col = 0; col < grid_cols; col++)
        {
          if (grid[row * grid_cols + col])
            continue;  /* Already occupied */

          /* Try to place a random-sized block */
          int size_idx = xorshift32 (&rng_state) % (num_sizes + 2);  /* Bias toward smaller */
          if (size_idx >= num_sizes)
            size_idx = num_sizes - 1;  /* Clamp to 1x1 */

          int block_size = block_sizes[size_idx];

          /* Find largest block that fits */
          while (block_size > 1 && !cells_free (grid, grid_cols, grid_rows, col, row, block_size))
            block_size--;

          if (!cells_free (grid, grid_cols, grid_rows, col, row, block_size))
            continue;  /* Can't place anything here */

          /* Mark cells as occupied */
          mark_cells (grid, grid_cols, col, row, block_size);

          /* Draw avatar */
          cairo_surface_t *avatar_surface = g_ptr_array_index (surfaces, texture_idx % surfaces->len);
          texture_idx++;

          double avatar_size = block_size * cell_size * 0.9;  /* 90% of cell block */
          double center_x = (col + block_size / 2.0) * cell_size;
          double center_y = (row + block_size / 2.0) * cell_size;

          /* Random rotation ±45° with finer granularity */
          double rotation = ((int)(xorshift32 (&rng_state) % 901) - 450) / 10.0 * G_PI / 180.0;

          draw_avatar (cr, avatar_surface, center_x, center_y, avatar_size, rotation);
        }
    }

  /* Free surfaces */
  for (guint i = 0; i < surfaces->len; i++)
    cairo_surface_destroy (g_ptr_array_index (surfaces, i));
  g_ptr_array_unref (surfaces);
  g_free (grid);

  cairo_destroy (cr);

  /* Convert to GdkTexture */
  cairo_surface_flush (surface);
  guchar *data = cairo_image_surface_get_data (surface);
  int stride = cairo_image_surface_get_stride (surface);

  GBytes *bytes = g_bytes_new (data, stride * canvas_height);
  GdkTexture *result = gdk_memory_texture_new (canvas_width, canvas_height,
                                                GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                                bytes, stride);
  g_bytes_unref (bytes);
  cairo_surface_destroy (surface);

  return result;
}

GtkCssProvider *
gnalk_background_generator_create_css_provider (GdkTexture *texture)
{
  if (!texture)
    return NULL;

  int width = gdk_texture_get_width (texture);
  int height = gdk_texture_get_height (texture);

  /* Save texture to a temporary PNG file with timestamp to avoid caching */
  g_autofree char *cache_dir = g_build_filename (g_get_user_cache_dir (), "gnalk", NULL);
  g_mkdir_with_parents (cache_dir, 0755);

  /* Each generation writes a fresh timestamped PNG; sweep away the previous
   * ones so the cache dir does not grow without bound. */
  {
    GDir *dir = g_dir_open (cache_dir, 0, NULL);
    if (dir)
      {
        const char *entry;
        while ((entry = g_dir_read_name (dir)))
          {
            if (g_str_has_prefix (entry, "mosaic-bg-") && g_str_has_suffix (entry, ".png"))
              {
                g_autofree char *stale = g_build_filename (cache_dir, entry, NULL);
                g_unlink (stale);
              }
          }
        g_dir_close (dir);
      }
  }

  gint64 timestamp = g_get_monotonic_time ();
  g_autofree char *filename = g_strdup_printf ("mosaic-bg-%" G_GINT64_FORMAT ".png", timestamp);
  g_autofree char *png_path = g_build_filename (cache_dir, filename, NULL);

  /* Save texture as PNG */
  gdk_texture_save_to_png (texture, png_path);

  (void) width;
  (void) height;

  /* Create CSS that uses the PNG - cover the whole area without repeating */
  g_autofree char *css = g_strdup_printf (
    ".message-list.bg-mosaic {\n"
    "  background-image: url('file://%s');\n"
    "  background-repeat: no-repeat;\n"
    "  background-size: cover;\n"
    "  background-position: center;\n"
    "}\n",
    png_path);

  GtkCssProvider *provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (provider, css);

  return provider;
}
