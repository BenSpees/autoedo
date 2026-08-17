/* Minimal JSON helpers — just enough for the flat config objects this app
   exchanges with its web UI. No allocation on the parse path. */

#ifndef AUTOEDO_JSON_H
#define AUTOEDO_JSON_H

#include <stdbool.h>
#include <stddef.h>

/* All getters return true and fill *out only if `key` exists at the top
   level of the JSON object `json` with a value of the right type. */
bool ae_json_get_number (const char *json, const char *key, double *out);
bool ae_json_get_bool   (const char *json, const char *key, bool *out);

/* Copies the (unescaped) string value into out (NUL-terminated, truncated
   to cap-1 bytes). */
bool ae_json_get_string (const char *json, const char *key, char *out, size_t cap);

/* Copies the RAW text of an object (or array) value, braces included --
   for keys whose value is structured (e.g. sampleGainDb's id->dB map).
   False if absent, not an object/array, or larger than cap. */
bool ae_json_get_object (const char *json, const char *key, char *out, size_t cap);

/* Reads an array of numbers/booleans as 0/1 flags into out (up to max
   entries). Returns the element count, or -1 if key is missing / not an
   array. */
int ae_json_get_flag_array (const char *json, const char *key, unsigned char *out, int max);

/* Reads an array of numbers into out (up to max entries; non-numeric
   elements read as 0). Returns the element count, or -1 if key is
   missing / not an array. */
int ae_json_get_num_array (const char *json, const char *key, double *out, int max);

/* Array of short string tokens (enum-ish values like "voice"/"synth").
   Returns the count written, or -1 when the key is absent or not an array;
   a non-string element yields an empty string in its slot. */
#define AE_JSON_STR_MAX 16
int ae_json_get_str_array (const char *json, const char *key,
                           char out[][AE_JSON_STR_MAX], int max);

/* Appends src to dst (a buffer of cap bytes, already NUL-terminated) with
   JSON string escaping applied. Returns dst. */
char *ae_json_escape_append (char *dst, size_t cap, const char *src);

#endif /* AUTOEDO_JSON_H */
