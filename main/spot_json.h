// main/spot_json.h - streaming JSON scanner with a fixed memory footprint.
//
// Spotify's playback-state response is several kilobytes (album images, artist
// objects, device and context data), more than this no-PSRAM board should
// buffer just to read a few fields. This scanner takes the body in chunks of any
// size, as they arrive from the HTTP client, and reports every scalar value
// together with its dot-separated path, for example "item.artists.0.name".
//
// Memory is fixed by the struct: a path buffer, a key buffer, and one value
// buffer. Values longer than SPOT_JSON_VALUE_MAX - 1 bytes are truncated on a
// UTF-8 boundary and flagged; keys or paths that do not fit are replaced by
// "?" so they never match a wanted path. Pure C, covered by host tests.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SPOT_JSON_PATH_MAX  96
#define SPOT_JSON_KEY_MAX   32
#define SPOT_JSON_VALUE_MAX 192
#define SPOT_JSON_DEPTH_MAX 12

typedef enum {
    SPOT_JSON_STRING = 0,
    SPOT_JSON_NUMBER,
    SPOT_JSON_TRUE,
    SPOT_JSON_FALSE,
    SPOT_JSON_NULL,
} spot_json_type_t;

// Called once per scalar. `value` is NUL-terminated: decoded UTF-8 for strings,
// the raw literal text for numbers, "true"/"false"/"null" otherwise. The
// pointers are valid only during the call.
typedef void (*spot_json_cb_t)(void *ctx, const char *path, spot_json_type_t type,
                               const char *value, bool truncated);

typedef struct {
    spot_json_cb_t cb;
    void *ctx;

    uint8_t state;
    uint8_t depth;
    bool in_key;              // the current string is an object key
    bool value_truncated;
    bool error;
    bool done;                // the top-level value is complete
    uint8_t esc_hex_left;     // remaining hex digits of a \uXXXX escape
    uint32_t esc_cp;
    uint32_t high_surrogate;  // pending \uD800-\uDBFF waiting for its pair

    struct {
        uint8_t is_array;
        uint16_t path_len;    // path length of the container itself
        uint16_t index;       // next array index
    } stack[SPOT_JSON_DEPTH_MAX];

    char path[SPOT_JSON_PATH_MAX];
    uint16_t path_len;
    char key[SPOT_JSON_KEY_MAX];
    uint8_t key_len;
    bool key_overflow;
    char value[SPOT_JSON_VALUE_MAX];
    uint16_t value_len;
} spot_json_t;

void spot_json_init(spot_json_t *s, spot_json_cb_t cb, void *ctx);

// Feeds the next `len` bytes. Returns false once the input is malformed (and
// for every later call). Bytes after the end of the top-level value, other
// than whitespace, are an error too.
bool spot_json_feed(spot_json_t *s, const char *data, size_t len);

// True when exactly one complete JSON value was fed without errors.
bool spot_json_finish(spot_json_t *s);
