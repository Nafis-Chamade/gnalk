#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * gnalk_markdown_to_pango:
 * @text: The markdown text to convert
 * @message_parameters: (nullable): JSON object containing mention parameters
 *
 * Converts markdown text to Pango markup.
 * Supports: **bold**, *italic*, `code`, ~~strike~~, headings, and mentions.
 *
 * Returns: (transfer full): A newly allocated string with Pango markup
 */
char *gnalk_markdown_to_pango (const char *text, JsonObject *message_parameters);

/**
 * gnalk_markdown_escape_pango:
 * @text: The text to escape
 *
 * Escapes text for safe use in Pango markup (escapes <, >, &).
 *
 * Returns: (transfer full): A newly allocated escaped string
 */
char *gnalk_markdown_escape_pango (const char *text);

G_END_DECLS
