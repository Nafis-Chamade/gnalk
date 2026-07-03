#include "gnalk-markdown.h"
#include <string.h>

/* Fenced code block storage for extract-process-restore pattern */
typedef struct {
  char *language;    /* Optional language identifier (python, bash, etc.) */
  char *content;     /* Raw code content */
  char *placeholder; /* Unique token for restoration */
} CodeBlock;

static void
code_block_free (CodeBlock *block)
{
  if (block)
    {
      g_free (block->language);
      g_free (block->content);
      g_free (block->placeholder);
      g_free (block);
    }
}

/* Extract fenced code blocks and replace with placeholders.
 * Returns the text with placeholders, stores blocks in out_blocks.
 * Must be called BEFORE Pango escaping. */
static char *
extract_fenced_code_blocks (const char *text, GPtrArray **out_blocks)
{
  GRegex *regex;
  GMatchInfo *match_info;
  GString *result;
  gint last_end = 0;
  guint block_index = 0;
  g_autoptr(GError) error = NULL;

  /* Per-call random nonce so message text can't spoof the placeholder token
   * (restore matches on the stored block->placeholder, so this is transparent). */
  guint32 nonce = g_random_int ();

  *out_blocks = g_ptr_array_new_with_free_func ((GDestroyNotify) code_block_free);

  /* Match ```language\ncontent``` - language is optional, content can be empty.
   * Use DOTALL so . matches newlines, making .*? match multiline content. */
  regex = g_regex_new ("```([a-zA-Z0-9_-]*)\\r?\\n(.*?)```",
                       G_REGEX_DOTALL, 0, &error);
  if (!regex)
    return g_strdup (text);

  result = g_string_new (NULL);

  if (g_regex_match (regex, text, 0, &match_info))
    {
      while (g_match_info_matches (match_info))
        {
          gint start, end;
          g_match_info_fetch_pos (match_info, 0, &start, &end);

          /* Append text before match */
          g_string_append_len (result, text + last_end, start - last_end);

          /* Extract language and content */
          g_autofree char *language = g_match_info_fetch (match_info, 1);
          g_autofree char *content = g_match_info_fetch (match_info, 2);

          /* Create code block with unique placeholder */
          CodeBlock *block = g_new0 (CodeBlock, 1);
          block->language = g_strdup (language ? language : "");
          block->content = g_strdup (content ? content : "");
          block->placeholder = g_strdup_printf ("<<<GNALKFENCED%08x_%u>>>", nonce, block_index);

          g_ptr_array_add (*out_blocks, block);

          /* Insert placeholder */
          g_string_append (result, block->placeholder);

          last_end = end;
          block_index++;
          g_match_info_next (match_info, NULL);
        }
    }

  /* Append remaining text */
  g_string_append (result, text + last_end);

  g_match_info_free (match_info);
  g_regex_unref (regex);

  return g_string_free (result, FALSE);
}

/* Restore fenced code blocks from placeholders.
 * Must be called AFTER all other markdown processing. */
static char *
restore_fenced_code_blocks (const char *text, GPtrArray *blocks)
{
  GString *result = g_string_new (text);

  for (guint i = 0; i < blocks->len; i++)
    {
      CodeBlock *block = g_ptr_array_index (blocks, i);

      /* Escape code content for Pango */
      g_autofree char *escaped_content = g_markup_escape_text (block->content, -1);

      /* Build the Pango markup for the code block */
      g_autofree char *replacement = g_strdup_printf (
          "<span font_family=\"monospace\" background=\"#e8e8e8\">%s</span>",
          escaped_content);

      /* Find and replace placeholder (escaped version after g_markup_escape_text) */
      g_autofree char *escaped_placeholder = g_markup_escape_text (block->placeholder, -1);
      char *pos = strstr (result->str, escaped_placeholder);
      if (pos)
        {
          gsize offset = pos - result->str;
          gsize placeholder_len = strlen (escaped_placeholder);
          g_string_erase (result, offset, placeholder_len);
          g_string_insert (result, offset, replacement);
        }
    }

  return g_string_free (result, FALSE);
}

/* Inline code storage for extract-process-restore pattern */
typedef struct {
  char *content;     /* Raw code content */
  char *placeholder; /* Unique token for restoration */
} InlineCode;

static void
inline_code_free (InlineCode *code)
{
  if (code)
    {
      g_free (code->content);
      g_free (code->placeholder);
      g_free (code);
    }
}

/* Extract inline code (`code`) and replace with placeholders.
 * Must be called BEFORE Pango escaping. */
static char *
extract_inline_code (const char *text, GPtrArray **out_codes)
{
  GRegex *regex;
  GMatchInfo *match_info;
  GString *result;
  gint last_end = 0;
  guint code_index = 0;
  g_autoptr(GError) error = NULL;

  *out_codes = g_ptr_array_new_with_free_func ((GDestroyNotify) inline_code_free);

  /* Match `content` - content cannot contain backticks */
  regex = g_regex_new ("`([^`]+)`", 0, 0, &error);
  if (!regex)
    return g_strdup (text);

  result = g_string_new (NULL);

  if (g_regex_match (regex, text, 0, &match_info))
    {
      while (g_match_info_matches (match_info))
        {
          gint start, end;
          g_match_info_fetch_pos (match_info, 0, &start, &end);

          /* Append text before match */
          g_string_append_len (result, text + last_end, start - last_end);

          /* Extract content */
          g_autofree char *content = g_match_info_fetch (match_info, 1);

          /* Create inline code with unique placeholder */
          InlineCode *code = g_new0 (InlineCode, 1);
          code->content = g_strdup (content ? content : "");
          code->placeholder = g_strdup_printf ("<<<GNALINLINE%u>>>", code_index);

          g_ptr_array_add (*out_codes, code);

          /* Insert placeholder */
          g_string_append (result, code->placeholder);

          last_end = end;
          code_index++;
          g_match_info_next (match_info, NULL);
        }
    }

  /* Append remaining text */
  g_string_append (result, text + last_end);

  g_match_info_free (match_info);
  g_regex_unref (regex);

  return g_string_free (result, FALSE);
}

/* Restore inline code from placeholders.
 * Must be called AFTER all other markdown processing. */
static char *
restore_inline_code (const char *text, GPtrArray *codes)
{
  GString *result = g_string_new (text);

  for (guint i = 0; i < codes->len; i++)
    {
      InlineCode *code = g_ptr_array_index (codes, i);

      /* Escape code content for Pango */
      g_autofree char *escaped_content = g_markup_escape_text (code->content, -1);

      /* Build the Pango markup for inline code */
      g_autofree char *replacement = g_strdup_printf (
          "<span font_family=\"monospace\" background=\"#e8e8e8\">%s</span>",
          escaped_content);

      /* Find and replace placeholder (escaped version after g_markup_escape_text) */
      g_autofree char *escaped_placeholder = g_markup_escape_text (code->placeholder, -1);
      char *pos = strstr (result->str, escaped_placeholder);
      if (pos)
        {
          gsize offset = pos - result->str;
          gsize placeholder_len = strlen (escaped_placeholder);
          g_string_erase (result, offset, placeholder_len);
          g_string_insert (result, offset, replacement);
        }
    }

  return g_string_free (result, FALSE);
}

/* Helper to replace a pattern with Pango tags */
static char *
replace_pattern (const char *text,
                 const char *pattern,
                 const char *open_tag,
                 const char *close_tag)
{
  GRegex *regex;
  GMatchInfo *match_info;
  GString *result;
  gint last_end = 0;
  g_autoptr(GError) error = NULL;

  regex = g_regex_new (pattern, G_REGEX_MULTILINE, 0, &error);
  if (!regex)
    return g_strdup (text);

  result = g_string_new (NULL);

  if (g_regex_match (regex, text, 0, &match_info))
    {
      while (g_match_info_matches (match_info))
        {
          gint start, end;
          g_match_info_fetch_pos (match_info, 0, &start, &end);

          /* Append text before match */
          g_string_append_len (result, text + last_end, start - last_end);

          /* Get captured group (the content between markers) */
          g_autofree char *content = g_match_info_fetch (match_info, 1);
          if (content)
            {
              g_string_append (result, open_tag);
              g_string_append (result, content);
              g_string_append (result, close_tag);
            }

          last_end = end;
          g_match_info_next (match_info, NULL);
        }
    }

  /* Append remaining text */
  g_string_append (result, text + last_end);

  g_match_info_free (match_info);
  g_regex_unref (regex);

  return g_string_free (result, FALSE);
}

/* Process bold (**text** and __text__) */
static char *
process_bold (const char *text)
{
  g_autofree char *pass1 = replace_pattern (text, "\\*\\*([^*]+)\\*\\*", "<b>", "</b>");
  return replace_pattern (pass1, "__([^_]+)__", "<b>", "</b>");
}

/* Process italic (*text* and _text_) - must run after bold */
static char *
process_italic (const char *text)
{
  /* Use negative lookbehind/lookahead to avoid matching ** or __ */
  g_autofree char *pass1 = replace_pattern (text, "(?<!\\*)\\*(?!\\*)([^*]+)(?<!\\*)\\*(?!\\*)", "<i>", "</i>");
  return replace_pattern (pass1, "(?<!_)_(?!_)([^_]+)(?<!_)_(?!_)", "<i>", "</i>");
}

/* Process strikethrough (~~text~~) */
static char *
process_strikethrough (const char *text)
{
  return replace_pattern (text, "~~([^~]+)~~", "<s>", "</s>");
}

/* Process headings (# Heading) */
static char *
process_headings (const char *text)
{
  GRegex *regex;
  GMatchInfo *match_info;
  GString *result;
  gint last_end = 0;
  g_autoptr(GError) error = NULL;

  /* Match lines starting with # (1-6 hashes) followed by space and content */
  regex = g_regex_new ("^(#{1,6})\\s+(.+)$", G_REGEX_MULTILINE, 0, &error);
  if (!regex)
    return g_strdup (text);

  result = g_string_new (NULL);

  if (g_regex_match (regex, text, 0, &match_info))
    {
      while (g_match_info_matches (match_info))
        {
          gint start, end;
          g_match_info_fetch_pos (match_info, 0, &start, &end);

          /* Append text before match */
          g_string_append_len (result, text + last_end, start - last_end);

          /* Get heading content */
          g_autofree char *content = g_match_info_fetch (match_info, 2);
          if (content)
            {
              g_string_append (result, "<span size=\"larger\"><b>");
              g_string_append (result, content);
              g_string_append (result, "</b></span>");
            }

          last_end = end;
          g_match_info_next (match_info, NULL);
        }
    }

  /* Append remaining text */
  g_string_append (result, text + last_end);

  g_match_info_free (match_info);
  g_regex_unref (regex);

  return g_string_free (result, FALSE);
}

/* Process URLs and convert to clickable links */
static char *
process_links (const char *text)
{
  GRegex *regex;
  GMatchInfo *match_info;
  GString *result;
  gint last_end = 0;
  g_autoptr(GError) error = NULL;

  /* Match URLs: http(s):// or www. followed by non-whitespace */
  regex = g_regex_new ("(https?://[^\\s<>\"]+|www\\.[^\\s<>\"]+)", 0, 0, &error);
  if (!regex)
    return g_strdup (text);

  result = g_string_new (NULL);

  if (g_regex_match (regex, text, 0, &match_info))
    {
      while (g_match_info_matches (match_info))
        {
          gint start, end;
          g_match_info_fetch_pos (match_info, 0, &start, &end);

          /* Append text before match */
          g_string_append_len (result, text + last_end, start - last_end);

          /* Get the URL */
          g_autofree char *url = g_match_info_fetch (match_info, 1);
          if (url)
            {
              /* For www. URLs, prepend https:// for the href */
              if (g_str_has_prefix (url, "www."))
                g_string_append_printf (result, "<a href=\"https://%s\">%s</a>", url, url);
              else
                g_string_append_printf (result, "<a href=\"%s\">%s</a>", url, url);
            }

          last_end = end;
          g_match_info_next (match_info, NULL);
        }
    }

  /* Append remaining text */
  g_string_append (result, text + last_end);

  g_match_info_free (match_info);
  g_regex_unref (regex);

  return g_string_free (result, FALSE);
}

/* Replace mention placeholders from messageParameters */
static char *
process_mentions (const char *text, JsonObject *message_parameters)
{
  GString *result;
  GRegex *regex;
  GMatchInfo *match_info;
  gint last_end = 0;
  g_autoptr(GError) error = NULL;

  if (!message_parameters)
    return g_strdup (text);

  /* Match {placeholder-name} patterns */
  regex = g_regex_new ("\\{([a-zA-Z0-9-]+)\\}", 0, 0, &error);
  if (!regex)
    return g_strdup (text);

  result = g_string_new (NULL);

  if (g_regex_match (regex, text, 0, &match_info))
    {
      while (g_match_info_matches (match_info))
        {
          gint start, end;
          g_match_info_fetch_pos (match_info, 0, &start, &end);

          /* Append text before match */
          g_string_append_len (result, text + last_end, start - last_end);

          /* Get placeholder name */
          g_autofree char *placeholder = g_match_info_fetch (match_info, 1);
          if (placeholder && json_object_has_member (message_parameters, placeholder))
            {
              JsonObject *param = json_object_get_object_member (message_parameters, placeholder);
              if (param)
                {
                  const char *type = json_object_get_string_member_with_default (param, "type", "");
                  const char *name = json_object_get_string_member_with_default (param, "name", placeholder);

                  if (g_strcmp0 (type, "user") == 0 ||
                      g_strcmp0 (type, "call") == 0 ||
                      g_strcmp0 (type, "guest") == 0)
                    {
                      /* Style mentions with Nextcloud blue color */
                      g_autofree char *escaped_name = g_markup_escape_text (name, -1);
                      g_string_append_printf (result,
                                              "<span color=\"#0082c9\"><b>@%s</b></span>",
                                              escaped_name);
                    }
                  else
                    {
                      /* Other parameter types - just show the name */
                      g_autofree char *escaped_name = g_markup_escape_text (name, -1);
                      g_string_append (result, escaped_name);
                    }
                }
              else
                {
                  /* Invalid param object - keep original */
                  g_string_append_len (result, text + start, end - start);
                }
            }
          else
            {
              /* Unknown placeholder - keep original */
              g_string_append_len (result, text + start, end - start);
            }

          last_end = end;
          g_match_info_next (match_info, NULL);
        }
    }

  /* Append remaining text */
  g_string_append (result, text + last_end);

  g_match_info_free (match_info);
  g_regex_unref (regex);

  return g_string_free (result, FALSE);
}

char *
gnalk_markdown_escape_pango (const char *text)
{
  if (!text || !*text)
    return g_strdup ("");

  return g_markup_escape_text (text, -1);
}

char *
gnalk_markdown_to_pango (const char *text, JsonObject *message_parameters)
{
  if (!text || !*text)
    return g_strdup ("");

  /* Step 1: Extract fenced code blocks BEFORE escaping (protect from all processing) */
  g_autoptr(GPtrArray) code_blocks = NULL;
  g_autofree char *without_code_blocks = extract_fenced_code_blocks (text, &code_blocks);

  /* Step 2: Extract inline code BEFORE escaping (protect from bold/italic/etc) */
  g_autoptr(GPtrArray) inline_codes = NULL;
  g_autofree char *without_inline_code = extract_inline_code (without_code_blocks, &inline_codes);

  /* Step 3: Escape special characters for Pango markup */
  g_autofree char *escaped = g_markup_escape_text (without_inline_code, -1);


  /* Step 4: Process bold (before italic to avoid conflicts) */
  g_autofree char *with_bold = process_bold (escaped);

  /* Step 5: Process italic */
  g_autofree char *with_italic = process_italic (with_bold);

  /* Step 6: Process strikethrough */
  g_autofree char *with_strike = process_strikethrough (with_italic);

  /* Step 7: Process headings */
  g_autofree char *with_headings = process_headings (with_strike);

  /* Step 8: Replace mention placeholders */
  g_autofree char *with_mentions = process_mentions (with_headings, message_parameters);

  /* Step 9: Convert URLs to clickable links */
  g_autofree char *with_links = process_links (with_mentions);

  /* Step 10: Restore inline code */
  g_autofree char *with_inline_restored = restore_inline_code (with_links, inline_codes);


  /* Step 11: Restore fenced code blocks at the end */
  char *final = restore_fenced_code_blocks (with_inline_restored, code_blocks);


  return final;
}
