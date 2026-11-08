// main/spot_logic.c - see spot_logic.h.
#include "spot_logic.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "home_json.h"

// ---------------------------------------------------------------------------
// UTF-8 helpers
// ---------------------------------------------------------------------------

// Copies at most size-1 bytes of UTF-8, never splitting a character.
static void copy_utf8(char *dst, size_t size, const char *src) {
    if (size == 0) return;
    size_t len = strlen(src);
    if (len >= size) {
        len = size - 1;
        // Back up to the start of the character that would be cut.
        size_t i = len;
        while (i > 0 && ((unsigned char)src[i] & 0xC0) == 0x80) i--;
        len = i;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

// Appends `src` to the NUL-terminated `dst`, with the same rule as copy_utf8.
static void append_utf8(char *dst, size_t size, const char *src) {
    size_t used = strlen(dst);
    if (used + 1 >= size) return;
    copy_utf8(dst + used, size - used, src);
}

// ---------------------------------------------------------------------------
// Playback state
// ---------------------------------------------------------------------------

static bool path_is(const char *path, const char *want) {
    return strcmp(path, want) == 0;
}

// Matches "<prefix><n>.<field>" and returns n (0..SPOT_COVER_IMAGES-1), or -1.
static int image_index(const char *path, const char *prefix, const char *field) {
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0) return -1;
    const char *p = path + plen;
    if (*p < '0' || *p > '9') return -1;
    int n = 0;
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        if (n > 99) return -1;
        p++;
    }
    if (*p != '.' || strcmp(p + 1, field) != 0) return -1;
    return n < SPOT_COVER_IMAGES ? n : -1;
}

// Records item.album.images.N.{url,width} and item.images.N.{url,width}.
static bool image_field(spot_now_parser_t *p, const char *path, spot_json_type_t type, const char *v) {
    static const struct {
        const char *prefix;
        size_t offset;
    } groups[] = {
        { "item.album.images.", offsetof(spot_now_parser_t, album) },
        { "item.images.", offsetof(spot_now_parser_t, episode) },
    };
    for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
        spot_image_t *list = (spot_image_t *)((char *)p + groups[g].offset);
        int i = image_index(path, groups[g].prefix, "url");
        if (i >= 0) {
            // A URL that does not fit would be cut; never store a broken one.
            if (type == SPOT_JSON_STRING && strlen(v) < sizeof(list[i].url) && strncmp(v, "https://", 8) == 0) {
                memcpy(list[i].url, v, strlen(v) + 1);
            }
            return true;
        }
        i = image_index(path, groups[g].prefix, "width");
        if (i >= 0) {
            if (type == SPOT_JSON_NUMBER) {
                long w = strtol(v, NULL, 10);
                list[i].width = (int16_t)(w > 0 && w < 10000 ? w : 0);
            }
            return true;
        }
    }
    return false;
}

// Matches "item.artists.<n>.name".
static bool is_artist_name(const char *path) {
    static const char prefix[] = "item.artists.";
    if (strncmp(path, prefix, sizeof(prefix) - 1) != 0) return false;
    const char *p = path + sizeof(prefix) - 1;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') p++;
    return strcmp(p, ".name") == 0;
}

static int32_t to_ms(const char *v) {
    long long ms = strtoll(v, NULL, 10);
    if (ms < 0) return -1;
    if (ms > INT32_MAX) return INT32_MAX;
    return (int32_t)ms;
}

static spot_item_type_t item_type(const char *v) {
    if (strcmp(v, "track") == 0) return SPOT_ITEM_TRACK;
    if (strcmp(v, "episode") == 0) return SPOT_ITEM_EPISODE;
    if (strcmp(v, "ad") == 0) return SPOT_ITEM_AD;
    return SPOT_ITEM_OTHER;
}

static void now_cb(void *ctx, const char *path, spot_json_type_t type, const char *v, bool truncated) {
    (void)truncated;  // long strings are shown cut; copy_utf8 keeps them valid
    spot_now_parser_t *p = ctx;
    spot_now_t *o = p->out;
    bool is_str = type == SPOT_JSON_STRING;
    bool is_true = type == SPOT_JSON_TRUE;

    if (path_is(path, "is_playing")) {
        o->is_playing = is_true;
    } else if (path_is(path, "progress_ms")) {
        if (type == SPOT_JSON_NUMBER) o->progress_ms = to_ms(v);
    } else if (path_is(path, "currently_playing_type")) {
        if (is_str && !o->have_item) o->type = item_type(v);
    } else if (path_is(path, "device.name")) {
        if (is_str) copy_utf8(o->device, sizeof(o->device), v);
    } else if (strncmp(path, "item.", 5) == 0) {
        p->saw_item_object = true;
        if (image_field(p, path, type, v)) return;
        const char *k = path + 5;
        if (path_is(k, "id")) {
            if (is_str) copy_utf8(o->id, sizeof(o->id), v);
        } else if (path_is(k, "name")) {
            if (is_str) copy_utf8(o->title, sizeof(o->title), v);
        } else if (path_is(k, "duration_ms")) {
            if (type == SPOT_JSON_NUMBER) o->duration_ms = to_ms(v);
        } else if (path_is(k, "type")) {
            if (is_str) o->type = item_type(v);
        } else if (path_is(k, "album.name")) {
            if (is_str) copy_utf8(o->album, sizeof(o->album), v);
        } else if (path_is(k, "show.name")) {
            // Episodes have no artists; the show stands in for them.
            if (is_str) copy_utf8(o->artist, sizeof(o->artist), v);
        } else if (is_artist_name(path) && is_str && p->artists < 3) {
            if (p->artists > 0) append_utf8(o->artist, sizeof(o->artist), ", ");
            append_utf8(o->artist, sizeof(o->artist), v);
            p->artists++;
        }
    } else if (strncmp(path, "actions.disallows.", 18) == 0) {
        const char *k = path + 18;
        if (path_is(k, "pausing")) o->disallow_pause = is_true;
        else if (path_is(k, "resuming")) o->disallow_resume = is_true;
        else if (path_is(k, "skipping_next")) o->disallow_next = is_true;
        else if (path_is(k, "skipping_prev")) o->disallow_prev = is_true;
    }
}

void spot_now_begin(spot_now_parser_t *p, spot_now_t *out) {
    memset(out, 0, sizeof(*out));
    out->progress_ms = -1;
    out->duration_ms = -1;
    p->out = out;
    p->artists = 0;
    p->saw_item_object = false;
    memset(p->album, 0, sizeof(p->album));
    memset(p->episode, 0, sizeof(p->episode));
    spot_json_init(&p->json, now_cb, p);
}

bool spot_now_feed(spot_now_parser_t *p, const char *data, size_t len) {
    return spot_json_feed(&p->json, data, len);
}

bool spot_now_end(spot_now_parser_t *p) {
    if (!spot_json_finish(&p->json)) return false;
    spot_now_t *o = p->out;
    o->have_item = p->saw_item_object && (o->title[0] || o->id[0]);
    if (o->have_item) {
        // Tracks carry album art; episodes carry their own images.
        const spot_image_t *list = p->album[0].url[0] ? p->album : p->episode;
        int pick = spot_cover_pick(list, SPOT_COVER_IMAGES, SPOT_COVER_PX);
        if (pick >= 0) {
            memcpy(o->cover_url, list[pick].url, sizeof(o->cover_url));
            o->cover_width = list[pick].width;
        }
    }
    if (!o->have_item) {
        o->is_playing = o->is_playing && o->type == SPOT_ITEM_AD;
        if (o->type != SPOT_ITEM_AD) o->type = SPOT_ITEM_NONE;
    } else if (o->type == SPOT_ITEM_NONE) {
        o->type = SPOT_ITEM_TRACK;
    }
    return true;
}

bool spot_parse_now(const char *json, spot_now_t *out) {
    spot_now_parser_t p;
    spot_now_begin(&p, out);
    spot_now_feed(&p, json, strlen(json));
    return spot_now_end(&p);
}

int32_t spot_progress_at(const spot_now_t *now, int64_t fetched_us, int64_t now_us) {
    if (now->progress_ms < 0) return -1;
    int64_t p = now->progress_ms;
    if (now->is_playing && now_us > fetched_us) p += (now_us - fetched_us) / 1000;
    if (now->duration_ms > 0 && p > now->duration_ms) p = now->duration_ms;
    return (int32_t)p;
}

void spot_fmt_time(char *out, size_t size, int32_t ms) {
    if (ms < 0) {
        snprintf(out, size, "-:--");
        return;
    }
    int32_t s = ms / 1000;
    int32_t h = s / 3600;
    if (h > 0) {
        snprintf(out, size, "%d:%02d:%02d", (int)h, (int)(s / 60 % 60), (int)(s % 60));
    } else {
        snprintf(out, size, "%d:%02d", (int)(s / 60), (int)(s % 60));
    }
}

int spot_progress_permille(int32_t progress_ms, int32_t duration_ms) {
    if (duration_ms <= 0 || progress_ms <= 0) return 0;
    if (progress_ms >= duration_ms) return 1000;
    return (int)((int64_t)progress_ms * 1000 / duration_ms);
}

int spot_eq_height(int bar, uint32_t tick, int min_h, int max_h) {
    if (max_h <= min_h) return min_h;
    // xorshift of (bar, tick): cheap, and different bars rarely move together.
    uint32_t x = (uint32_t)bar * 0x9E3779B1u ^ (tick + 1u) * 0x85EBCA77u;
    x ^= x >> 13;
    x *= 0xC2B2AE3Du;
    x ^= x >> 16;
    // Bias toward the middle so the motion looks like music, not noise.
    uint32_t span = (uint32_t)(max_h - min_h);
    uint32_t a = x % (span + 1);
    uint32_t b = (x >> 8) % (span + 1);
    return min_h + (int)((a + b) / 2);
}

// ---------------------------------------------------------------------------
// Album cover
// ---------------------------------------------------------------------------

int spot_cover_scaled(int size, int scale) {
    if (size <= 0 || scale < 0) return 0;
    return (size + (1 << scale) - 1) >> scale;
}

int spot_cover_scale(int width, int target) {
    if (width <= 0 || target <= 0) return -1;
    for (int s = 0; s <= 3; s++) {
        if (spot_cover_scaled(width, s) <= target) return s;
    }
    return -1;
}

int spot_cover_pick(const spot_image_t *images, int count, int target) {
    int best = -1, best_out = -1, best_width = 0;
    int unknown = -1;
    for (int i = 0; i < count; i++) {
        if (!images[i].url[0]) continue;
        int w = images[i].width;
        if (w <= 0) {
            if (unknown < 0) unknown = i;
            continue;
        }
        int s = spot_cover_scale(w, target);
        if (s < 0) continue;
        int out = spot_cover_scaled(w, s);
        bool better;
        if (best < 0) {
            better = true;
        } else if (out > best_out + 8) {
            better = true;                      // clearly sharper
        } else if (out + 8 >= best_out) {
            better = w < best_width;            // about the same: smaller download
        } else {
            better = false;
        }
        if (better) {
            best = i;
            best_out = out;
            best_width = w;
        }
    }
    return best >= 0 ? best : unknown;
}

bool spot_cover_should_fetch(const char *want, const char *have, const char *failed, int64_t failed_at_us,
                             int64_t now_us, int64_t retry_us) {
    if (!want || !want[0]) return false;
    if (have && strcmp(want, have) == 0) return false;
    if (failed && strcmp(want, failed) == 0 && now_us - failed_at_us < retry_us) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------

bool spot_parse_token(const char *json, spot_token_t *out) {
    memset(out, 0, sizeof(*out));
    if (!json) return false;
    (void)home_json_string(json, "error", out->error, sizeof(out->error));
    double expires = 0;
    if (home_json_number(json, "expires_in", &expires) && expires > 0 && expires < 86400 * 30) {
        out->expires_in_s = (int32_t)expires;
    }
    bool ok = home_json_string(json, "access_token", out->access, sizeof(out->access));
    // A token that filled the buffer was probably cut off; refuse it.
    if (ok && strlen(out->access) >= sizeof(out->access) - 1) ok = false;
    if (!home_json_string(json, "refresh_token", out->refresh, sizeof(out->refresh)) ||
        !spot_valid_refresh_token(out->refresh)) {
        out->refresh[0] = '\0';
    }
    if (!ok) out->access[0] = '\0';
    if (ok && out->expires_in_s == 0) out->expires_in_s = 3600;
    return ok && out->access[0] != '\0';
}

bool spot_refresh_body(char *out, size_t size, const char *client_id, const char *refresh_token) {
    // Both values are validated to URL-safe characters, so no escaping is needed.
    if (!spot_valid_client_id(client_id) || !spot_valid_refresh_token(refresh_token)) return false;
    int n = snprintf(out, size, "grant_type=refresh_token&refresh_token=%s&client_id=%s",
                     refresh_token, client_id);
    return n > 0 && (size_t)n < size;
}

spot_tok_result_t spot_token_result(int http_status, const spot_token_t *tok, bool parsed) {
    if (http_status == 200 && parsed && tok->access[0]) return SPOT_TOK_OK;
    if (http_status == 400 || http_status == 401) {
        // OAuth 2.0 errors that mean the stored link itself is unusable.
        if (strcmp(tok->error, "invalid_grant") == 0 || strcmp(tok->error, "invalid_client") == 0 ||
            strcmp(tok->error, "unauthorized_client") == 0) {
            return SPOT_TOK_REVOKED;
        }
    }
    return SPOT_TOK_RETRY;
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

spot_result_t spot_classify(int http_status, const char *reason, bool is_command) {
    if (http_status <= 0) return SPOT_RES_NETWORK;
    if (http_status >= 200 && http_status < 300) {
        return (!is_command && http_status == 204) ? SPOT_RES_IDLE : SPOT_RES_OK;
    }
    bool has_reason = reason && reason[0];
    switch (http_status) {
    case 401:
        return SPOT_RES_AUTH;
    case 403:
        return has_reason && strcmp(reason, "PREMIUM_REQUIRED") == 0 ? SPOT_RES_PREMIUM
                                                                     : SPOT_RES_FORBIDDEN;
    case 404:
        if (is_command || (has_reason && strcmp(reason, "NO_ACTIVE_DEVICE") == 0)) {
            return SPOT_RES_NO_DEVICE;
        }
        return SPOT_RES_BAD;
    case 429:
        return SPOT_RES_RATE_LIMIT;
    default:
        return http_status >= 500 ? SPOT_RES_SERVER : SPOT_RES_BAD;
    }
}

int32_t spot_retry_after_s(const char *header) {
    if (!header) return 30;
    while (*header == ' ') header++;
    if (*header < '0' || *header > '9') return 30;
    long v = 0;
    while (*header >= '0' && *header <= '9') {
        v = v * 10 + (*header - '0');
        if (v > 100000) break;
        header++;
    }
    if (v < 1) return 1;
    if (v > 600) return 600;
    return (int32_t)v;
}

// ---------------------------------------------------------------------------
// Polling
// ---------------------------------------------------------------------------

int32_t spot_poll_delay_ms(const spot_poll_in_t *in) {
    if (in->after_command) return SPOT_POLL_AFTER_CMD_MS;
    switch (in->result) {
    case SPOT_RES_NONE:
        return 0;
    case SPOT_RES_OK: {
        if (!in->have_item) return SPOT_POLL_IDLE_MS;
        if (!in->is_playing) return SPOT_POLL_PAUSED_MS;
        int32_t d = SPOT_POLL_PLAYING_MS;
        // Catch the change to the next track soon after this one ends.
        if (in->progress_ms >= 0 && in->duration_ms > 0) {
            int32_t remaining = in->duration_ms - in->progress_ms;
            if (remaining < 0) remaining = 0;
            if (remaining + 700 < d) d = remaining + 700;
            if (d < 1000) d = 1000;
        }
        return d;
    }
    case SPOT_RES_IDLE:
    case SPOT_RES_NO_DEVICE:
        return SPOT_POLL_IDLE_MS;
    case SPOT_RES_RATE_LIMIT:
        return (in->retry_after_s > 0 ? in->retry_after_s : 30) * 1000;
    case SPOT_RES_NETWORK:
    case SPOT_RES_SERVER: {
        int32_t d = SPOT_POLL_ERROR_MIN_MS;
        for (int i = 1; i < in->failures && d < SPOT_POLL_ERROR_MAX_MS; i++) d *= 2;
        return d > SPOT_POLL_ERROR_MAX_MS ? SPOT_POLL_ERROR_MAX_MS : d;
    }
    case SPOT_RES_AUTH:
    case SPOT_RES_PREMIUM:
    case SPOT_RES_FORBIDDEN:
    case SPOT_RES_BAD:
    default:
        return SPOT_POLL_BLOCKED_MS;
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

bool spot_cmd_request(spot_cmd_t cmd, bool is_playing, spot_cmd_req_t *out) {
    switch (cmd) {
    case SPOT_CMD_PREV:
        out->method = "POST";
        out->path = "/v1/me/player/previous";
        return true;
    case SPOT_CMD_NEXT:
        out->method = "POST";
        out->path = "/v1/me/player/next";
        return true;
    case SPOT_CMD_TOGGLE:
        out->method = "PUT";
        out->path = is_playing ? "/v1/me/player/pause" : "/v1/me/player/play";
        return true;
    default:
        return false;
    }
}

const char *spot_cmd_toast(spot_result_t result) {
    switch (result) {
    case SPOT_RES_OK:
    case SPOT_RES_IDLE:
        return NULL;
    case SPOT_RES_NO_DEVICE:
        return "No active device";
    case SPOT_RES_PREMIUM:
        return "Needs Spotify Premium";
    case SPOT_RES_FORBIDDEN:
        return "Not allowed right now";
    case SPOT_RES_RATE_LIMIT:
        return "Too many requests";
    case SPOT_RES_NETWORK:
        return "No connection";
    case SPOT_RES_SERVER:
        return "Spotify error, try again";
    case SPOT_RES_AUTH:
        return "Signing in, try again";
    default:
        return "Command failed";
    }
}

// ---------------------------------------------------------------------------
// Page state
// ---------------------------------------------------------------------------

static spot_page_t message(const char *title, const char *detail, bool warning) {
    spot_page_t p = { .show = SPOT_SHOW_MESSAGE, .title = title, .detail = detail,
                      .status = NULL, .warning = warning };
    return p;
}

spot_page_t spot_page_state(const spot_page_in_t *in) {
    if (in->link == SPOT_LINK_NONE) {
        return message("Link Spotify",
                       "Press OK on the home\nscreen to open Setup,\nthen follow the steps\non your phone.", false);
    }
    if (in->link == SPOT_LINK_REVOKED) {
        return message("Link expired",
                       "Spotify rejected the saved\nlogin. Open Setup (OK on\nthe home screen) and log\nin again.", true);
    }
    if (!in->wifi_configured) {
        return message("No Wi-Fi", "Press OK on the home\nscreen to open Setup.", false);
    }

    if (in->fetched && in->have_item) {
        spot_page_t p = { .show = SPOT_SHOW_TRACK, .title = NULL, .detail = NULL,
                          .status = in->is_playing ? "PLAYING" : "PAUSED", .warning = false };
        if (!in->wifi_connected) {
            p.status = "OFFLINE";
            p.warning = true;
        } else if (in->last == SPOT_RES_NETWORK || in->last == SPOT_RES_SERVER) {
            p.status = "RECONNECTING";
            p.warning = true;
        } else if (in->last == SPOT_RES_RATE_LIMIT) {
            p.status = "SPOTIFY BUSY";
            p.warning = true;
        }
        return p;
    }

    if (!in->wifi_connected) return message("Offline", "Waiting for Wi-Fi...", true);

    switch (in->last) {
    case SPOT_RES_NONE:
    case SPOT_RES_AUTH:
        return message("Loading", "Asking Spotify what is\nplaying...", false);
    case SPOT_RES_NETWORK:
    case SPOT_RES_SERVER:
        return message("Can't reach Spotify", "Retrying shortly.", true);
    case SPOT_RES_RATE_LIMIT:
        return message("Spotify is busy", "Too many requests.\nWaiting before retrying.", true);
    case SPOT_RES_PREMIUM:
        return message("Premium needed", "This Spotify app needs\nthe owner to have\nSpotify Premium.", true);
    case SPOT_RES_FORBIDDEN:
        return message("Access denied",
                       "Add your Spotify account\nunder User Management in\nthe developer dashboard.", true);
    case SPOT_RES_BAD:
        return message("Unexpected reply", "Retrying in a minute.", true);
    case SPOT_RES_OK:
    case SPOT_RES_IDLE:
    case SPOT_RES_NO_DEVICE:
    default:
        return message("Nothing playing", "Start Spotify on your\nphone or computer.", false);
    }
}

// ---------------------------------------------------------------------------
// Text for the built-in fonts
// ---------------------------------------------------------------------------

// Base letters for U+00C0..U+00FF.
static const char LATIN1_FOLD[] =
    "AAAAAAACEEEEIIII"   // C0-CF
    "DNOOOOOxOUUUUYPs"   // D0-DF
    "aaaaaaaceeeeiiii"   // E0-EF
    "dnooooo/ouuuuypy";  // F0-FF

// Base letters for U+0100..U+017F (Latin Extended-A).
static const char LATIN_EXT_A_FOLD[] =
    "AaAaAa" "CcCcCcCc" "DdDd" "EeEeEeEeEe" "GgGgGgGg" "HhHh" "IiIiIiIiIi" "Ii" "Jj" "Kkk"
    "LlLlLlLlLl" "NnNnNnnNn" "OoOoOoOo" "RrRrRr" "SsSsSsSs" "TtTtTt" "UuUuUuUuUuUu" "Ww"
    "YyY" "ZzZzZzs";

_Static_assert(sizeof(LATIN1_FOLD) - 1 == 64, "Latin-1 fold table covers U+00C0..U+00FF");
_Static_assert(sizeof(LATIN_EXT_A_FOLD) - 1 == 128, "Latin Extended-A table covers U+0100..U+017F");

// Decodes one UTF-8 character. Returns its length, or 0 for an invalid byte.
static size_t utf8_decode(const unsigned char *p, uint32_t *cp) {
    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    size_t n;
    uint32_t v;
    if ((p[0] & 0xE0) == 0xC0) {
        n = 2;
        v = p[0] & 0x1F;
    } else if ((p[0] & 0xF0) == 0xE0) {
        n = 3;
        v = p[0] & 0x0F;
    } else if ((p[0] & 0xF8) == 0xF0) {
        n = 4;
        v = p[0] & 0x07;
    } else {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (p[i] & 0x3F);
    }
    // Reject overlong forms and surrogates.
    if ((n == 2 && v < 0x80) || (n == 3 && v < 0x800) || (n == 4 && (v < 0x10000 || v > 0x10FFFF)) ||
        (v >= 0xD800 && v <= 0xDFFF)) {
        return 0;
    }
    *cp = v;
    return n;
}

// ASCII replacement for a code point the font cannot draw. Returns "" to drop it.
static const char *replacement(uint32_t cp, char buf[2]) {
    buf[1] = '\0';
    if (cp < 0x20 || cp == 0x7F) return " ";
    if (cp >= 0xC0 && cp <= 0xFF) {
        buf[0] = LATIN1_FOLD[cp - 0xC0];
        return buf;
    }
    if (cp >= 0x100 && cp <= 0x17F) {
        buf[0] = LATIN_EXT_A_FOLD[cp - 0x100];
        return buf;
    }
    if (cp >= 0xFF01 && cp <= 0xFF5E) {  // full-width ASCII forms
        buf[0] = (char)(cp - 0xFEE0);
        return buf;
    }
    switch (cp) {
    case 0xA0: case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004: case 0x2005:
    case 0x2006: case 0x2007: case 0x2008: case 0x2009: case 0x200A: case 0x202F: case 0x205F:
    case 0x3000:
        return " ";
    case 0x200B: case 0x200C: case 0x200D: case 0x2060: case 0xFEFF: case 0xFE0F:
        return "";
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0xB4:
        return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: case 0xAB: case 0xBB:
    case 0x300A: case 0x300B: case 0x300C: case 0x300D: case 0x300E: case 0x300F:
        return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
    case 0x30FB:
        return "-";
    case 0x2026:
        return "...";
    case 0x3001:
        return ",";
    case 0x3002:
        return ".";
    case 0x3010:
        return "[";
    case 0x3011:
        return "]";
    case 0xA1:
        return "!";
    case 0xBF:
        return "?";
    case 0xD7:
        return "x";
    default:
        return "?";
    }
}

void spot_text_for_font(const char *in, char *out, size_t size, spot_has_glyph_t has, void *ctx) {
    if (!out || size == 0) return;
    size_t len = 0;
    const unsigned char *p = (const unsigned char *)(in ? in : "");
    while (*p) {
        uint32_t cp;
        size_t n = utf8_decode(p, &cp);
        const char *piece;
        size_t piece_len;
        char buf[2];
        if (n == 0) {
            piece = "?";
            piece_len = 1;
            n = 1;
        } else if ((cp >= 0x20 && cp < 0x7F) || (cp > 0x7F && has && has(cp, ctx))) {
            // Printable ASCII is always drawable: every replacement is ASCII too.
            piece = (const char *)p;
            piece_len = n;
        } else {
            piece = replacement(cp, buf);
            piece_len = strlen(piece);
        }
        if (len + piece_len >= size) break;
        memcpy(out + len, piece, piece_len);
        len += piece_len;
        p += n;
    }
    out[len] = '\0';
}

// ---------------------------------------------------------------------------
// Console arguments
// ---------------------------------------------------------------------------

bool spot_valid_client_id(const char *s) {
    if (!s || strlen(s) != SPOT_CLIENT_ID_LEN) return false;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

bool spot_valid_refresh_token(const char *s) {
    if (!s) return false;
    size_t len = strlen(s);
    if (len < 16 || len > SPOT_REFRESH_MAX) return false;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' ||
              c == '_' || c == '.' || c == '~')) {
            return false;
        }
    }
    return true;
}
