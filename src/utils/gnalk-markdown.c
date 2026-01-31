#include "gnalk-markdown.h"
#include <string.h>

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

/* Process inline code - protect from further parsing by using <tt> tags */
static char *
process_inline_code (const char *text)
{
  return replace_pattern (text, "`([^`]+)`", "<tt>", "</tt>");
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

  /* Step 1: Escape special characters for Pango markup */
  g_autofree char *escaped = g_markup_escape_text (text, -1);

  /* Step 2: Process inline code first (protect from further parsing) */
  g_autofree char *with_code = process_inline_code (escaped);

  /* Step 3: Process bold (before italic to avoid conflicts) */
  g_autofree char *with_bold = process_bold (with_code);

  /* Step 4: Process italic */
  g_autofree char *with_italic = process_italic (with_bold);

  /* Step 5: Process strikethrough */
  g_autofree char *with_strike = process_strikethrough (with_italic);

  /* Step 6: Process headings */
  g_autofree char *with_headings = process_headings (with_strike);

  /* Step 7: Replace mention placeholders */
  g_autofree char *with_mentions = process_mentions (with_headings, message_parameters);

  /* Step 8: Convert URLs to clickable links */
  char *final = process_links (with_mentions);

  return final;
}
