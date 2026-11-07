// main/home_json.h - allocation-free lookups in small JSON documents.
//
// The home screen only reads a handful of fields from two short responses
// (IP geolocation and Open-Meteo, both well under 1 KB). Instead of building a
// cJSON tree on the heap, these helpers scan the text in place.
//
// A path is a dot-separated list of object keys and array indexes, for example
// "current.temperature_2m" or "daily.temperature_2m_max.0". Keys that contain
// dots are not supported. Pure C, no ESP-IDF dependency, covered by host tests.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Reads a JSON number at `path`. Returns false when the path is missing, the
// value is not a number, or the document is malformed before the value.
bool home_json_number(const char *json, const char *path, double *out);

// Reads a JSON boolean at `path`.
bool home_json_bool(const char *json, const char *path, bool *out);

// Reads a JSON string at `path`, decoding escapes (including \uXXXX) to UTF-8.
// The result is always NUL-terminated; it is truncated on a UTF-8 boundary when
// `out_size` is too small. Returns false when missing or not a string.
bool home_json_string(const char *json, const char *path, char *out, size_t out_size);
