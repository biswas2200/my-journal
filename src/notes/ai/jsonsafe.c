/* jsonsafe: see jsonsafe.h. */
#include "jsonsafe.h"

#include <string.h>


/* A \uXXXX escape at p (p[0] == 'u'): its value, or -1. */
static int
hex4 (const char *p, gsize left)
{
  int v = 0;
  for (gsize k = 1; k <= 4; k++)
    {
      int x = k < left ? g_ascii_xdigit_value (p[k]) : -1;
      if (x < 0)
        return -1;
      v = v * 16 + x;
    }
  return v;
}

/* Exactly one {...}, nothing after it but white space, and in strings
 * only proper escapes, surrogates in pairs. json-glib 1.10 refuses the
 * rest too; json-glib 1.8 (Ubuntu 24.04) reads on after the first value
 * and leaks it ("{}{...}"), leaks on a broken \u escape, and passes a
 * lone surrogate on as broken UTF-8 (all found by fuzzing). */
static gboolean
one_object (const char *json, gsize len)
{
  gsize i = 0;
  while (i < len && g_ascii_isspace (json[i]))
    i++;
  if (i == len || json[i] != '{')
    return FALSE;
  int depth = 0;
  gboolean in_string = FALSE;
  for (; i < len; i++)
    {
      char c = json[i];
      if (in_string && c == '\\')
        {
          if (++i == len)
            return FALSE;
          if (json[i] != 'u')
            {
              if (strchr ("\"\\/bfnrt", json[i]) == NULL || json[i] == '\0')
                return FALSE;
              continue;
            }
          int v = hex4 (json + i, len - i);
          if (v >= 0xDC00 && v <= 0xDFFF)
            return FALSE; /* a low surrogate on its own */
          if (v >= 0xD800 && v <= 0xDBFF)
            {
              /* A high surrogate needs a low one right after it. */
              int low = i + 6 < len && json[i + 5] == '\\' && json[i + 6] == 'u' ? hex4 (json + i + 6, len - i - 6) : -1;
              if (low < 0xDC00 || low > 0xDFFF)
                return FALSE;
              i += 6;
            }
          else if (v < 0)
            return FALSE;
          i += 4;
        }
      else if (in_string)
        in_string = c != '"';
      else if (c == '"')
        in_string = TRUE;
      else if (c == '{' || c == '[')
        depth++;
      else if (c == '}' || c == ']')
        {
          depth--;
          if (depth == 0)
            break;
        }
    }
  for (i++; i < len; i++)
    if (!g_ascii_isspace (json[i]))
      return FALSE;
  return depth == 0;
}

JsonObject *
jn_json_object (const char *json, gssize len, JsonParser **parser)
{
  /* Not json_parser_new_immutable (): with it, json-glib 1.10 aborts
   * the program on some broken input ("null{"), found by fuzzing. */
  *parser = json_parser_new ();
  if (json == NULL || !one_object (json, len < 0 ? strlen (json) : (gsize) len) ||
      !json_parser_load_from_data (*parser, json, len, NULL))
    return NULL;
  JsonNode *root = json_parser_get_root (*parser);
  return root != NULL && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
}

/* A string member, as it is: possibly not valid UTF-8 (json-glib 1.8
 * passes a lone "\ud800" through). */
const char *
jn_json_raw_str (JsonObject *o, const char *name)
{
  JsonNode *n = o != NULL ? json_object_get_member (o, name) : NULL;
  return n != NULL && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING
           ? json_node_get_string (n) : NULL;
}

/* A string member that is valid text; anything else reads as missing. */
const char *
jn_json_str (JsonObject *o, const char *name)
{
  const char *s = jn_json_raw_str (o, name);
  return s != NULL && g_utf8_validate (s, -1, NULL) ? s : NULL;
}

JsonObject *
jn_json_obj (JsonObject *o, const char *name)
{
  JsonNode *n = o != NULL ? json_object_get_member (o, name) : NULL;
  return n != NULL && JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
}

JsonArray *
jn_json_arr (JsonObject *o, const char *name)
{
  JsonNode *n = o != NULL ? json_object_get_member (o, name) : NULL;
  return n != NULL && JSON_NODE_HOLDS_ARRAY (n) ? json_node_get_array (n) : NULL;
}

gboolean
jn_json_int (JsonObject *o, const char *name, gint64 *out)
{
  JsonNode *n = o != NULL ? json_object_get_member (o, name) : NULL;
  if (n == NULL || !JSON_NODE_HOLDS_VALUE (n) || json_node_get_value_type (n) != G_TYPE_INT64)
    return FALSE;
  *out = json_node_get_int (n);
  return TRUE;
}

char *
jn_json_to_string (JsonBuilder *b)
{
  JsonNode *root = json_builder_get_root (b);
  JsonGenerator *g = json_generator_new ();
  json_generator_set_root (g, root);
  char *s = json_generator_to_data (g, NULL);
  g_object_unref (g);
  json_node_unref (root);
  g_object_unref (b);
  return s;
}


gboolean
jn_json_bool (JsonObject *o, const char *name)
{
  JsonNode *n = o != NULL ? json_object_get_member (o, name) : NULL;
  return n != NULL && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_BOOLEAN &&
         json_node_get_boolean (n);
}
