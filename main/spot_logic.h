// main/spot_logic.h - pure logic for the Spotify "Now Playing" page.
//
// Response parsing (streamed), token handling, HTTP result classification,
// poll timing, progress extrapolation, text preparation for the built-in
// fonts, and page messages. No ESP-IDF, FreeRTOS, or LVGL, so all of it is
// covered by host tests (tests/test_spot_logic.c). Firmware glue lives in
// spot_net.c (requests) and home_ui.c (screen).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spot_json.h"

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

#define SPOT_CLIENT_ID_LEN 32     // Spotify client IDs are 32 hex digits
#define SPOT_REFRESH_MAX   256    // refresh tokens are ~130 URL-safe characters today
#define SPOT_ACCESS_MAX    512    // access tokens are ~200-300 characters today
#define SPOT_ID_MAX        40
#define SPOT_TITLE_MAX     128
#define SPOT_ARTIST_MAX    96
#define SPOT_ALBUM_MAX     96
#define SPOT_DEVICE_MAX    48
#define SPOT_COVER_URL_MAX 96     // https://i.scdn.co/image/<40 hex> is 64 characters
#define SPOT_COVER_IMAGES  4      // Spotify lists 3 sizes (640, 300, 64 px)
#define SPOT_COVER_PX      80     // largest cover drawn on the page, in pixels

// ---------------------------------------------------------------------------
// Playback state (GET /v1/me/player?additional_types=episode)
// ---------------------------------------------------------------------------

typedef enum {
    SPOT_ITEM_NONE = 0,
    SPOT_ITEM_TRACK,
    SPOT_ITEM_EPISODE,
    SPOT_ITEM_AD,
    SPOT_ITEM_OTHER,
} spot_item_type_t;

typedef struct {
    bool have_item;           // a track or episode is loaded
    bool is_playing;
    spot_item_type_t type;
    int32_t progress_ms;      // -1 when unknown
    int32_t duration_ms;      // -1 when unknown
    char id[SPOT_ID_MAX];
    char title[SPOT_TITLE_MAX];     // UTF-8
    char artist[SPOT_ARTIST_MAX];   // up to three artists, or the show name
    char album[SPOT_ALBUM_MAX];
    char device[SPOT_DEVICE_MAX];
    char cover_url[SPOT_COVER_URL_MAX];  // chosen album (or episode) image, "" if none
    int16_t cover_width;      // its width in pixels (0 when unknown)
    bool disallow_pause;      // from actions.disallows
    bool disallow_resume;
    bool disallow_next;
    bool disallow_prev;
} spot_now_t;

typedef struct {
    char url[SPOT_COVER_URL_MAX];
    int16_t width;
} spot_image_t;

typedef struct {
    spot_json_t json;
    spot_now_t *out;
    int artists;              // artist names joined so far
    bool saw_item_object;
    spot_image_t album[SPOT_COVER_IMAGES];    // item.album.images (tracks)
    spot_image_t episode[SPOT_COVER_IMAGES];  // item.images (episodes)
} spot_now_parser_t;

// Streams a playback-state body: begin, feed chunks as they arrive, end.
void spot_now_begin(spot_now_parser_t *p, spot_now_t *out);
bool spot_now_feed(spot_now_parser_t *p, const char *data, size_t len);
// Returns false for malformed JSON. *out is then incomplete and must not be shown.
bool spot_now_end(spot_now_parser_t *p);

// One-shot helper over a complete body (tests, small documents).
bool spot_parse_now(const char *json, spot_now_t *out);

// Playback position `now_us` after the reading taken at `fetched_us`, assuming
// playback continued; clamped to the duration. -1 when unknown.
int32_t spot_progress_at(const spot_now_t *now, int64_t fetched_us, int64_t now_us);

// "3:07", or "1:02:03" past an hour; "-:--" for a negative value.
void spot_fmt_time(char *out, size_t size, int32_t ms);

// Progress bar fill 0..1000, or 0 when the duration is unknown.
int spot_progress_permille(int32_t progress_ms, int32_t duration_ms);

// Height of equalizer bar `bar` at animation step `tick`, in min_h..max_h.
// A fixed pseudo-random pattern: each bar moves on its own, deterministic for tests.
int spot_eq_height(int bar, uint32_t tick, int min_h, int max_h);

// ---------------------------------------------------------------------------
// Album cover
// ---------------------------------------------------------------------------

// JPEG decode scale (TJpgDec: 0 = 1/1, 1 = 1/2, 2 = 1/4, 3 = 1/8) that brings
// `width` down to at most `target` pixels, or -1 when even 1/8 is too big.
int spot_cover_scale(int width, int target);

// Size after decoding at `scale` (rounded up, as TJpgDec does).
int spot_cover_scaled(int size, int scale);

// Picks which listed image to download: the one whose decoded size comes
// closest to `target` (sizes within 8 px count as equal, and then the smaller
// download wins). Images with unknown width (0) are used only when nothing
// else is listed. Returns the index, or -1 when none is usable.
int spot_cover_pick(const spot_image_t *images, int count, int target);

// RGB888 -> RGB565 (LVGL's native 16-bit format).
static inline uint16_t spot_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Whether the worker should download `want` now. `have` is the cover on
// screen; `failed` the last URL that failed, at `failed_at_us`. A failed URL
// is retried after `retry_us`.
bool spot_cover_should_fetch(const char *want, const char *have, const char *failed, int64_t failed_at_us,
                             int64_t now_us, int64_t retry_us);

// ---------------------------------------------------------------------------
// Tokens (POST https://accounts.spotify.com/api/token)
// ---------------------------------------------------------------------------

typedef struct {
    char access[SPOT_ACCESS_MAX];
    char refresh[SPOT_REFRESH_MAX + 1];   // empty when the response kept the old one
    int32_t expires_in_s;
    char error[32];                       // OAuth error code, e.g. "invalid_grant"
} spot_token_t;

// Parses a token-endpoint body (success or OAuth error). Returns true when it
// contains an access token.
bool spot_parse_token(const char *json, spot_token_t *out);

// Builds the refresh request body. Returns false when it does not fit.
bool spot_refresh_body(char *out, size_t size, const char *client_id, const char *refresh_token);

typedef enum {
    SPOT_TOK_OK = 0,
    SPOT_TOK_REVOKED,         // the stored link is no longer valid; run the setup tool again
    SPOT_TOK_RETRY,           // network or server trouble; try again later
} spot_tok_result_t;

spot_tok_result_t spot_token_result(int http_status, const spot_token_t *tok, bool parsed);

// Seconds before expiry at which the access token is renewed.
#define SPOT_TOKEN_MARGIN_S 120

// ---------------------------------------------------------------------------
// Web API results
// ---------------------------------------------------------------------------

typedef enum {
    SPOT_RES_NONE = 0,        // no request made yet
    SPOT_RES_OK,              // 200 with a body, or a 2xx command reply
    SPOT_RES_IDLE,            // 204 on GET /me/player: nothing playing, no active device
    SPOT_RES_AUTH,            // 401: access token expired or invalid
    SPOT_RES_PREMIUM,         // 403 PREMIUM_REQUIRED
    SPOT_RES_FORBIDDEN,       // other 403, e.g. account not on the app's user list
    SPOT_RES_NO_DEVICE,       // 404 (NO_ACTIVE_DEVICE) on a player command
    SPOT_RES_RATE_LIMIT,      // 429
    SPOT_RES_SERVER,          // 5xx
    SPOT_RES_NETWORK,         // no response: DNS, TLS, timeout
    SPOT_RES_BAD,             // other status, or a body that did not parse
} spot_result_t;

// `reason` is the error.reason field of a Web API error body (may be NULL).
spot_result_t spot_classify(int http_status, const char *reason, bool is_command);

// Retry-After header value in seconds, clamped to 1..600; 30 when absent or invalid.
int32_t spot_retry_after_s(const char *header);

// ---------------------------------------------------------------------------
// Polling
// ---------------------------------------------------------------------------

#define SPOT_POLL_PLAYING_MS   5000
#define SPOT_POLL_PAUSED_MS    10000
#define SPOT_POLL_IDLE_MS      15000
#define SPOT_POLL_AFTER_CMD_MS 700
#define SPOT_POLL_ERROR_MIN_MS 5000
#define SPOT_POLL_ERROR_MAX_MS 60000
#define SPOT_POLL_BLOCKED_MS   60000

typedef struct {
    spot_result_t result;     // of the last poll
    bool have_item;
    bool is_playing;
    int32_t progress_ms;
    int32_t duration_ms;
    int failures;             // consecutive network/server failures, >= 1 after one
    int32_t retry_after_s;    // for SPOT_RES_RATE_LIMIT
    bool after_command;       // a player command just succeeded
} spot_poll_in_t;

// Milliseconds until the next poll while the page is visible. Polling stops
// entirely while the page is hidden or the screen sleeps (spot_net.c).
int32_t spot_poll_delay_ms(const spot_poll_in_t *in);

// ---------------------------------------------------------------------------
// Player commands
// ---------------------------------------------------------------------------

typedef enum {
    SPOT_CMD_NONE = 0,
    SPOT_CMD_PREV,
    SPOT_CMD_NEXT,
    SPOT_CMD_TOGGLE,          // pause when playing, otherwise resume
} spot_cmd_t;

typedef struct {
    const char *method;       // "POST" or "PUT"
    const char *path;         // e.g. "/v1/me/player/next"
} spot_cmd_req_t;

// Resolves a command against the last known state (TOGGLE -> play or pause).
// Returns false for SPOT_CMD_NONE.
bool spot_cmd_request(spot_cmd_t cmd, bool is_playing, spot_cmd_req_t *out);

// Short message for the result of a player command, or NULL on success.
const char *spot_cmd_toast(spot_result_t result);

// ---------------------------------------------------------------------------
// Page state
// ---------------------------------------------------------------------------

typedef enum {
    SPOT_LINK_NONE = 0,       // no client ID or refresh token stored
    SPOT_LINK_OK,
    SPOT_LINK_REVOKED,        // Spotify rejected the refresh token
} spot_link_t;

typedef struct {
    spot_link_t link;
    bool wifi_configured;
    bool wifi_connected;
    bool fetched;             // a playback state was received since the page opened
    spot_result_t last;       // result of the last poll
    bool have_item;
    bool is_playing;
} spot_page_in_t;

typedef enum {
    SPOT_SHOW_MESSAGE = 0,    // a centred message instead of a track
    SPOT_SHOW_TRACK,
} spot_show_t;

typedef struct {
    spot_show_t show;
    const char *title;        // message headline (SPOT_SHOW_MESSAGE)
    const char *detail;       // message body, may contain '\n'
    const char *status;       // small status line above the track (SPOT_SHOW_TRACK)
    bool warning;             // draw the status/message in the warning colour
} spot_page_t;

spot_page_t spot_page_state(const spot_page_in_t *in);

// ---------------------------------------------------------------------------
// Text for the built-in fonts
// ---------------------------------------------------------------------------

// Returns whether the label's font chain can draw `cp`.
typedef bool (*spot_has_glyph_t)(uint32_t cp, void *ctx);

// Copies UTF-8 `in` to `out`, keeping every code point the font can draw.
// Others are replaced: typographic quotes, dashes, and spaces by ASCII,
// full-width ASCII forms by their ASCII letters, accented Latin letters by the
// base letter, and anything left by '?'. Control characters become spaces.
// Invalid UTF-8 bytes become '?'. Output is truncated on a character boundary.
void spot_text_for_font(const char *in, char *out, size_t size, spot_has_glyph_t has, void *ctx);

// ---------------------------------------------------------------------------
// Serial console arguments
// ---------------------------------------------------------------------------

// 32 hex digits.
bool spot_valid_client_id(const char *s);
// 16..SPOT_REFRESH_MAX URL-safe characters (A-Z a-z 0-9 - _ . ~).
bool spot_valid_refresh_token(const char *s);
