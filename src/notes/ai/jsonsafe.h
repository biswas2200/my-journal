/* jsonsafe: reading JSON that comes from outside (Google, the helpers)
 * without trusting it.
 *
 * Only one object is read, and only after a check that it is exactly one
 * {...} with proper escapes: json-glib 1.8 (Ubuntu 24.04) reads on past
 * the first value and leaks, leaks on a broken \u escape and passes a
 * lone surrogate on as broken UTF-8; 1.10 aborts the program on some
 * broken input with its immutable parser (all found by fuzzing). Members
 * of the wrong type, and strings that are not valid UTF-8, read as
 * missing (NULL / FALSE), so callers never have to check types. */
#pragma once

#include <json-glib/json-glib.h>

/* The top-level object, or NULL (*parser is always set: unref it). */
JsonObject *jn_json_object    (const char *json, gssize len, JsonParser **parser);
/* A string member as valid UTF-8, or NULL. */
const char *jn_json_str       (JsonObject *o, const char *name);
/* A string member as it is (possibly not valid UTF-8), or NULL. */
const char *jn_json_raw_str   (JsonObject *o, const char *name);
JsonObject *jn_json_obj       (JsonObject *o, const char *name);
JsonArray  *jn_json_arr       (JsonObject *o, const char *name);
/* An integer member into *out; FALSE (and *out untouched) otherwise. */
gboolean    jn_json_int       (JsonObject *o, const char *name, gint64 *out);
/* TRUE only for a member that is the boolean true. */
gboolean    jn_json_bool      (JsonObject *o, const char *name);
/* The builder's object as text; frees the builder. */
char       *jn_json_to_string (JsonBuilder *b);
