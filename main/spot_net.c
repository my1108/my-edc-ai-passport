// main/spot_net.c - see spot_net.h.
//
// Requests (all from the home_net worker task):
//   token    POST https://accounts.spotify.com/api/token (refresh_token grant, PKCE
//            public client: client_id in the body, no secret). Renewed shortly
//            before the hour-long access token expires, or after a 401.
//   poll     GET  https://api.spotify.com/v1/me/player?additional_types=episode
//            Every 5 s while playing (sooner near the end of a track), 10 s
//            paused, 15 s idle, with back-off on errors and Retry-After on 429.
//            Only while the Spotify page is visible and the screen is awake.
//   command  POST /v1/me/player/next | /previous, PUT /v1/me/player/play | /pause
//   cover    GET  https://i.scdn.co/image/... when the album changes (page open
//            only). The JPEG is streamed into the ESP32-C3's ROM TJpgDec and
//            decoded at 1/1..1/8 scale to at most 80 x 80 RGB565 (12.8 KB,
//            allocated when needed and freed when the page closes, so other
//            pages get that memory back; reopening the page downloads the cover
//            again). The previous album's cover is hidden while the next one
//            decodes; a failed download is retried after 2 min.
//
// Memory: the playback-state body (several KB) is never buffered whole. It is
// read in 512-byte chunks into the streaming scanner in spot_json.c, which keeps
// only the fields the page shows. Error and token bodies are small and go into
// one 1.5 KB buffer. The API connection is kept alive between polls to avoid a
// TLS handshake every few seconds, and closed before any other HTTPS request.
#include "spot_net.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp32c3/rom/tjpgd.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "home_json.h"
#include "home_net.h"
#include "home_store.h"
#include "setup_logic.h"

static const char *TAG = "spotify";

#define API_BASE        "https://api.spotify.com"
#define TOKEN_URL       "https://accounts.spotify.com/api/token"
#define PLAYER_PATH     "/v1/me/player?additional_types=episode"
#define HTTP_TIMEOUT_MS 10000
#define SMALL_BODY_MAX  1536
#define CHUNK_SIZE      512
#define STALE_US        (30LL * 1000 * 1000)   // older readings are reloaded on page open
#define TOKEN_RETRY_MIN_S 10
#define TOKEN_RETRY_MAX_S 300
#define US_PER_MS       1000LL
#define US_PER_S        1000000LL
#define COVER_RETRY_US  (120LL * US_PER_S)
#define COVER_MAX_BYTES (256 * 1024)   // Spotify's 300 px JPEGs are ~20-40 KB
#define JPEG_WORK_SIZE  3200           // TJpgDec needs ~3100 bytes

// --- shared state (s_lock) ---
static SemaphoreHandle_t s_lock;
static spot_status_t s_st;
static spot_cmd_t s_pending;
static bool s_link_dirty;
static bool s_poll_now;

// Login from the setup page (s_lock), finished by the worker.
static struct {
    bool pending;
    char client_id[SPOT_CLIENT_ID_LEN + 1];
    char code[512];
    char verifier[SETUP_VERIFIER_LEN + 1];
    const char *result;
} s_code_req;
static SemaphoreHandle_t s_code_done;

// --- worker task only ---
static char s_client_id[SPOT_CLIENT_ID_LEN + 1];
static char s_refresh[SPOT_REFRESH_MAX + 1];
static char s_access[SPOT_ACCESS_MAX];
static char s_auth[SPOT_ACCESS_MAX + 8];       // "Bearer <access>"
static int64_t s_access_exp_us;
static int64_t s_token_retry_us;
static int s_token_failures;
static int64_t s_poll_due_us;
static int s_poll_failures;
static esp_http_client_handle_t s_api;
static bool s_api_connected;
static spot_now_parser_t s_parser;
static spot_now_t s_parsed;
static spot_token_t s_tok;
static char s_chunk[CHUNK_SIZE];
static char s_small[SMALL_BODY_MAX];

typedef struct {
    char retry_after[16];
} req_ctx_t;
static req_ctx_t s_req;

// Album cover (worker task writes; the UI reads while status.cover_ready).
static uint16_t *s_cover_px;
static int s_cover_w, s_cover_h;
static char s_cover_have[SPOT_COVER_URL_MAX];
static char s_cover_failed[SPOT_COVER_URL_MAX];
static int64_t s_cover_failed_at;
static bool (*s_cover_detach)(void);
static int64_t s_cover_release_retry_us;

#define LOCKED(...) do {                            \
        xSemaphoreTake(s_lock, portMAX_DELAY);      \
        __VA_ARGS__;                                \
        xSemaphoreGive(s_lock);                     \
    } while (0)

static int64_t now_us(void) {
    return esp_timer_get_time();
}

// Free heap after each new HTTPS connection (not on reused ones, so polling
// stays quiet). min_ever only falls, so the first line where it drops names
// the request that set the low point.
static void log_heap(const char *what) {
    ESP_LOGI(TAG, "heap %s: free=%u largest=%u min_ever=%u", what,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

// ---------------------------------------------------------------------------
// Public, any task
// ---------------------------------------------------------------------------

void spot_get_status(spot_status_t *out) {
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_st;
    xSemaphoreGive(s_lock);
}

void spot_set_active(bool active) {
    if (!s_lock) return;
    bool kick = false;
    LOCKED({
        if (active && !s_st.active) {
            if (s_st.now_at_us == 0 || now_us() - s_st.now_at_us > STALE_US) s_st.fetched = false;
            s_poll_now = true;
            kick = true;
        }
        if (!active && s_st.active) kick = true;  // lets the worker drop the connection
        s_st.active = active;
    });
    if (kick) home_net_kick();
}

void spot_send(spot_cmd_t cmd) {
    if (!s_lock || cmd == SPOT_CMD_NONE) return;
    LOCKED({
        s_pending = cmd;
        s_st.busy = true;
        s_st.toast = NULL;
    });
    home_net_kick();
}

void spot_link_changed(void) {
    if (!s_lock) return;
    LOCKED(s_link_dirty = true);
    home_net_kick();
}

// ---------------------------------------------------------------------------
// Link
// ---------------------------------------------------------------------------

static void load_link(void) {
    home_store_load_spotify(s_client_id, sizeof(s_client_id), s_refresh, sizeof(s_refresh));
    bool ok = spot_valid_client_id(s_client_id) && spot_valid_refresh_token(s_refresh);
    wipe(s_access, sizeof(s_access));
    wipe(s_auth, sizeof(s_auth));
    s_access_exp_us = 0;
    s_token_retry_us = 0;
    s_token_failures = 0;
    s_poll_due_us = 0;
    s_poll_failures = 0;
    LOCKED({
        s_st.link = ok ? SPOT_LINK_OK : SPOT_LINK_NONE;
        s_st.fetched = false;
        s_st.last = SPOT_RES_NONE;
        memset(&s_st.now, 0, sizeof(s_st.now));
        s_st.now_at_us = 0;
    });
    ESP_LOGI(TAG, "link %s", ok ? "configured" : (s_client_id[0] ? "missing refresh token" : "not configured"));
}

void spot_net_init(void) {
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    s_code_done = xSemaphoreCreateBinary();
    if (!s_lock || !s_code_done) return;
    load_link();
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Retry-After") == 0) {
        req_ctx_t *ctx = evt->user_data;
        snprintf(ctx->retry_after, sizeof(ctx->retry_after), "%s", evt->header_value);
    }
    return ESP_OK;
}

void spot_net_release(void) {
    if (!s_api) return;
    esp_http_client_close(s_api);
    esp_http_client_cleanup(s_api);
    s_api = NULL;
    s_api_connected = false;
}

// Reads the rest of a response into s_small (truncated, NUL-terminated).
// Returns false on a read error.
static bool read_small(esp_http_client_handle_t client) {
    int total = 0;
    for (;;) {
        int room = SMALL_BODY_MAX - 1 - total;
        char *dst = room > 0 ? s_small + total : s_chunk;
        int n = esp_http_client_read(client, dst, room > 0 ? room : CHUNK_SIZE);
        if (n < 0) return false;
        if (n == 0) break;
        if (room > 0) total += n;  // past the buffer: keep draining, drop the bytes
    }
    s_small[total] = '\0';
    return true;
}

// Sends one API request on the kept-alive connection. For a GET that answers
// 200, the body is streamed into s_parser/s_parsed and *parsed tells whether
// it was valid JSON. Error bodies go to s_small and their error.reason to
// `reason`. Returns the HTTP status, or -1 when there was no response.
static int api_request(esp_http_client_method_t method, const char *path, bool *parsed,
                       char *reason, size_t reason_size) {
    char url[96];
    snprintf(url, sizeof(url), "%s%s", API_BASE, path);
    if (parsed) *parsed = false;
    reason[0] = '\0';
    memset(&s_req, 0, sizeof(s_req));

    if (!s_api) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = HTTP_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = 1024,
            .buffer_size_tx = 1024,   // the Authorization header alone is ~300 bytes
            .user_agent = "ai-passport-home/1.0",
            .keep_alive_enable = true,
            .event_handler = on_http_event,
            .user_data = &s_req,
        };
        s_api = esp_http_client_init(&cfg);
        s_api_connected = false;
        if (!s_api) return -1;
    } else if (esp_http_client_set_url(s_api, url) != ESP_OK) {
        spot_net_release();
        return -1;
    }
    esp_http_client_set_method(s_api, method);
    esp_http_client_set_header(s_api, "Authorization", s_auth);

    int64_t length = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        bool reused = s_api_connected;
        esp_err_t err = esp_http_client_open(s_api, 0);
        if (err == ESP_OK) length = esp_http_client_fetch_headers(s_api);
        if (err == ESP_OK && length >= 0) {
            if (!reused) log_heap("after api.spotify.com connect");
            s_api_connected = true;
            break;
        }
        esp_http_client_close(s_api);
        s_api_connected = false;
        if (!reused) {
            ESP_LOGW(TAG, "%s: no response (%s)", path, esp_err_to_name(err != ESP_OK ? err : ESP_FAIL));
            return -1;
        }
        // The server closed the idle connection; reconnect once.
    }
    if (!s_api_connected) return -1;

    int status = esp_http_client_get_status_code(s_api);
    bool read_ok = true;
    if (status == 200 && method == HTTP_METHOD_GET && parsed) {
        spot_now_begin(&s_parser, &s_parsed);
        bool json_ok = true;
        for (;;) {
            int n = esp_http_client_read(s_api, s_chunk, sizeof(s_chunk));
            if (n < 0) {
                read_ok = false;
                break;
            }
            if (n == 0) break;
            if (json_ok) json_ok = spot_now_feed(&s_parser, s_chunk, (size_t)n);
        }
        *parsed = read_ok && json_ok && spot_now_end(&s_parser);
    } else if (status != 204) {
        read_ok = read_small(s_api);
        if (read_ok && s_small[0]) (void)home_json_string(s_small, "error.reason", reason, reason_size);
    }
    if (!read_ok || !esp_http_client_is_complete_data_received(s_api) ||
        !esp_http_client_is_persistent_connection(s_api)) {
        esp_http_client_close(s_api);
        s_api_connected = false;
    }
    if (!read_ok) return -1;
    return status;
}

// ---------------------------------------------------------------------------
// Token
// ---------------------------------------------------------------------------

static void set_revoked(void) {
    ESP_LOGW(TAG, "Spotify rejected the refresh token (%s); run tools/spotify_auth.py again",
             s_tok.error[0] ? s_tok.error : "no reason");
    (void)home_store_clear_spotify_token();
    wipe(s_refresh, sizeof(s_refresh));
    LOCKED({
        s_st.link = SPOT_LINK_REVOKED;
        s_st.fetched = false;
    });
}

// POSTs a form body to the token endpoint; the reply goes to s_small.
// Returns the HTTP status, or -1 without a response.
static int token_post(const char *body) {
    spot_net_release();  // one TLS session at a time
    esp_http_client_config_t cfg = {
        .url = TOKEN_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 768,
        .user_agent = "ai-passport-home/1.0",
        .keep_alive_enable = false,
    };
    int status = -1;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    s_small[0] = '\0';
    if (client) {
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        int len = (int)strlen(body);
        if (esp_http_client_open(client, len) == ESP_OK &&
            esp_http_client_write(client, body, len) == len &&
            esp_http_client_fetch_headers(client) >= 0) {
            log_heap("after accounts.spotify.com connect");
            status = esp_http_client_get_status_code(client);
            if (!read_small(client)) status = -1;
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return status;
}

static spot_tok_result_t refresh_token(int64_t now) {
    char body[SPOT_REFRESH_MAX + SPOT_CLIENT_ID_LEN + 64];
    if (!spot_refresh_body(body, sizeof(body), s_client_id, s_refresh)) {
        set_revoked();
        return SPOT_TOK_REVOKED;
    }
    int status = token_post(body);
    wipe(body, sizeof(body));

    bool parsed = status > 0 && spot_parse_token(s_small, &s_tok);
    spot_tok_result_t r = status > 0 ? spot_token_result(status, &s_tok, parsed) : SPOT_TOK_RETRY;
    if (r == SPOT_TOK_OK) {
        snprintf(s_access, sizeof(s_access), "%s", s_tok.access);
        snprintf(s_auth, sizeof(s_auth), "Bearer %s", s_tok.access);
        int32_t life = s_tok.expires_in_s - SPOT_TOKEN_MARGIN_S;
        if (life < 60) life = 60;
        s_access_exp_us = now + (int64_t)life * US_PER_S;
        s_token_failures = 0;
        s_token_retry_us = 0;
        if (s_tok.refresh[0] && strcmp(s_tok.refresh, s_refresh) != 0) {
            // Spotify may rotate the refresh token; keep the newest one.
            if (home_store_save_spotify_token(s_tok.refresh) == ESP_OK) {
                snprintf(s_refresh, sizeof(s_refresh), "%s", s_tok.refresh);
                ESP_LOGI(TAG, "access token renewed; new refresh token saved");
            } else {
                ESP_LOGE(TAG, "could not save the new refresh token");
            }
        } else {
            ESP_LOGI(TAG, "access token renewed (valid %" PRId32 " s)", s_tok.expires_in_s);
        }
    } else if (r == SPOT_TOK_REVOKED) {
        set_revoked();
    } else {
        int32_t d = TOKEN_RETRY_MIN_S;
        for (int i = 0; i < s_token_failures && d < TOKEN_RETRY_MAX_S; i++) d *= 2;
        if (d > TOKEN_RETRY_MAX_S) d = TOKEN_RETRY_MAX_S;
        s_token_failures++;
        s_token_retry_us = now + (int64_t)d * US_PER_S;
        ESP_LOGW(TAG, "token refresh failed (HTTP %d%s%s); retrying in %" PRId32 " s", status,
                 s_tok.error[0] ? ", " : "", s_tok.error, d);
    }
    wipe(&s_tok, sizeof(s_tok));
    wipe(s_small, sizeof(s_small));
    return r;
}

// Makes sure a valid access token is loaded. SPOT_RES_OK when ready, else the
// result to report (SPOT_RES_NETWORK while retrying, SPOT_RES_AUTH if revoked).
static spot_result_t ensure_token(int64_t now, bool force) {
    if (!force && s_access[0] && now < s_access_exp_us) return SPOT_RES_OK;
    if (now < s_token_retry_us) return SPOT_RES_NETWORK;
    switch (refresh_token(now)) {
    case SPOT_TOK_OK: return SPOT_RES_OK;
    case SPOT_TOK_REVOKED: return SPOT_RES_AUTH;
    default: return SPOT_RES_NETWORK;
    }
}

// ---------------------------------------------------------------------------
// Poll and commands
// ---------------------------------------------------------------------------

static void schedule_poll(spot_result_t res, int32_t retry_after_s, bool after_command) {
    spot_poll_in_t in = {
        .result = res,
        .have_item = s_parsed.have_item,
        .is_playing = s_parsed.is_playing,
        .progress_ms = s_parsed.progress_ms,
        .duration_ms = s_parsed.duration_ms,
        .failures = s_poll_failures,
        .retry_after_s = retry_after_s,
        .after_command = after_command,
    };
    int32_t delay = spot_poll_delay_ms(&in);
    s_poll_due_us = now_us() + (int64_t)delay * US_PER_MS;
}

static void do_poll(int64_t now) {
    spot_result_t res = ensure_token(now, false);
    bool parsed = false;
    int status = -1;
    char reason[32];
    if (res == SPOT_RES_OK) {
        status = api_request(HTTP_METHOD_GET, PLAYER_PATH, &parsed, reason, sizeof(reason));
        res = spot_classify(status, reason, false);
        if (res == SPOT_RES_AUTH && ensure_token(now_us(), true) == SPOT_RES_OK) {
            status = api_request(HTTP_METHOD_GET, PLAYER_PATH, &parsed, reason, sizeof(reason));
            res = spot_classify(status, reason, false);
        }
        if (res == SPOT_RES_OK && !parsed) {
            ESP_LOGW(TAG, "playback state did not parse");
            res = SPOT_RES_BAD;
        }
    }
    if (res == SPOT_RES_IDLE) spot_now_begin(&s_parser, &s_parsed);  // clear the old track
    s_poll_failures = (res == SPOT_RES_NETWORK || res == SPOT_RES_SERVER) ? s_poll_failures + 1 : 0;
    int32_t retry_after = res == SPOT_RES_RATE_LIMIT ? spot_retry_after_s(s_req.retry_after) : 0;
    if (res == SPOT_RES_RATE_LIMIT) ESP_LOGW(TAG, "rate limited; next poll in %" PRId32 " s", retry_after);
    if (res != SPOT_RES_OK && res != SPOT_RES_IDLE && status > 0) {
        ESP_LOGW(TAG, "poll: HTTP %d %s", status, reason);
    }

    int64_t at = now_us();
    LOCKED({
        s_st.last = res;
        if (res == SPOT_RES_OK || res == SPOT_RES_IDLE) {
            s_st.now = s_parsed;
            s_st.now_at_us = at;
            s_st.fetched = true;
        }
    });
    schedule_poll(res, retry_after, false);
}

static void do_command(int64_t now) {
    spot_cmd_t cmd;
    bool playing;
    LOCKED({
        cmd = s_pending;
        s_pending = SPOT_CMD_NONE;
        playing = s_st.now.is_playing;
    });
    spot_cmd_req_t req;
    if (!spot_cmd_request(cmd, playing, &req)) return;
    esp_http_client_method_t method = strcmp(req.method, "PUT") == 0 ? HTTP_METHOD_PUT : HTTP_METHOD_POST;

    char reason[32] = "";
    int status = -1;
    spot_result_t res = ensure_token(now, false);
    if (res == SPOT_RES_OK) {
        status = api_request(method, req.path, NULL, reason, sizeof(reason));
        res = spot_classify(status, reason, true);
        if (res == SPOT_RES_AUTH && ensure_token(now_us(), true) == SPOT_RES_OK) {
            status = api_request(method, req.path, NULL, reason, sizeof(reason));
            res = spot_classify(status, reason, true);
        }
    }
    ESP_LOGI(TAG, "%s %s -> HTTP %d%s%s", req.method, req.path, status, reason[0] ? " " : "", reason);

    int64_t at = now_us();
    const char *toast = spot_cmd_toast(res);
    LOCKED({
        s_st.busy = s_pending != SPOT_CMD_NONE;
        s_st.toast = toast;
        s_st.toast_at_us = at;
        if (res == SPOT_RES_OK && cmd == SPOT_CMD_TOGGLE && s_st.fetched) {
            // Show the new state at once; the next poll confirms it.
            s_st.now.progress_ms = spot_progress_at(&s_st.now, s_st.now_at_us, at);
            s_st.now_at_us = at;
            s_st.now.is_playing = !playing;
        }
    });
    if (res == SPOT_RES_OK) {
        schedule_poll(SPOT_RES_OK, 0, true);
    } else if (res == SPOT_RES_RATE_LIMIT) {
        schedule_poll(res, spot_retry_after_s(s_req.retry_after), false);
    }
}

// ---------------------------------------------------------------------------
// Album cover
// ---------------------------------------------------------------------------

void spot_set_cover_detach(bool (*detach)(void)) {
    s_cover_detach = detach;
}

const uint16_t *spot_cover_pixels(int *w, int *h) {
    if (w) *w = s_cover_w;
    if (h) *h = s_cover_h;
    return s_cover_px;
}

typedef struct {
    esp_http_client_handle_t client;
    int total;
    bool failed;
    int out_w, out_h;
} jpeg_io_t;

// TJpgDec input: reads the next bytes straight from the HTTP body (buf NULL
// means skip them).
static UINT jpeg_in(JDEC *jd, BYTE *buf, UINT len) {
    jpeg_io_t *io = jd->device;
    UINT done = 0;
    char skip[64];
    while (done < len && !io->failed) {
        int want = (int)(len - done);
        char *dst = buf ? (char *)buf + done : skip;
        if (!buf && want > (int)sizeof(skip)) want = sizeof(skip);
        int n = esp_http_client_read(io->client, dst, want);
        if (n < 0) io->failed = true;
        if (n <= 0) break;
        done += (UINT)n;
        io->total += n;
        if (io->total > COVER_MAX_BYTES) io->failed = true;
    }
    return done;
}

// TJpgDec output: one decoded block of RGB888 pixels.
static UINT jpeg_out(JDEC *jd, void *bitmap, JRECT *r) {
    jpeg_io_t *io = jd->device;
    const BYTE *p = bitmap;
    for (int y = r->top; y <= r->bottom; y++) {
        for (int x = r->left; x <= r->right; x++, p += 3) {
            if (x < io->out_w && y < io->out_h) s_cover_px[y * io->out_w + x] = spot_rgb565(p[0], p[1], p[2]);
        }
    }
    return io->failed ? 0 : 1;
}

static void cover_failed(const char *url, const char *why) {
    snprintf(s_cover_failed, sizeof(s_cover_failed), "%s", url);
    s_cover_failed_at = now_us();
    ESP_LOGW(TAG, "album cover not shown: %s", why);
}

static void do_cover(const char *url) {
    if (!s_cover_px) {
        s_cover_px = heap_caps_malloc(SPOT_COVER_PX * SPOT_COVER_PX * sizeof(uint16_t), MALLOC_CAP_8BIT);
        if (!s_cover_px) {
            cover_failed(url, "no memory for the pixels");
            return;
        }
    }
    // Stop the screen drawing the old cover before its pixels are overwritten.
    LOCKED(s_st.cover_ready = false);
    if (!s_cover_detach || !s_cover_detach()) {
        cover_failed(url, "screen busy");
        return;
    }
    s_cover_have[0] = '\0';
    spot_net_release();  // one TLS session at a time

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .user_agent = "ai-passport-home/1.0",
        .keep_alive_enable = false,
    };
    jpeg_io_t io = { 0 };
    void *work = NULL;
    const char *why = NULL;
    io.client = esp_http_client_init(&cfg);
    if (!io.client) {
        why = "HTTP client";
    } else if (esp_http_client_open(io.client, 0) != ESP_OK || esp_http_client_fetch_headers(io.client) < 0) {
        why = "no response";
    } else if (log_heap("after cover connect"), esp_http_client_get_status_code(io.client) != 200) {
        why = "HTTP error";
    } else if (!(work = malloc(JPEG_WORK_SIZE))) {
        why = "no memory for the decoder";
    } else {
        JDEC jd;
        JRESULT r = jd_prepare(&jd, jpeg_in, work, JPEG_WORK_SIZE, &io);
        int scale = -1;
        if (r == JDR_OK) {
            int sw = spot_cover_scale((int)jd.width, SPOT_COVER_PX);
            int sh = spot_cover_scale((int)jd.height, SPOT_COVER_PX);
            scale = sw < 0 || sh < 0 ? -1 : (sw > sh ? sw : sh);
        }
        if (r != JDR_OK) {
            why = r == JDR_FMT3 ? "unsupported JPEG (progressive?)" : "not a JPEG";
        } else if (scale < 0) {
            why = "image too large";
        } else {
            io.out_w = spot_cover_scaled((int)jd.width, scale);
            io.out_h = spot_cover_scaled((int)jd.height, scale);
            memset(s_cover_px, 0, SPOT_COVER_PX * SPOT_COVER_PX * sizeof(uint16_t));
            r = jd_decomp(&jd, jpeg_out, (BYTE)scale);
            if (r != JDR_OK || io.failed) why = "decode failed";
            log_heap("after cover decode");
        }
    }
    free(work);
    if (io.client) {
        esp_http_client_close(io.client);
        esp_http_client_cleanup(io.client);
    }
    if (why) {
        cover_failed(url, why);
        return;
    }
    s_cover_w = io.out_w;
    s_cover_h = io.out_h;
    snprintf(s_cover_have, sizeof(s_cover_have), "%s", url);
    LOCKED({
        s_st.cover_ready = true;
        s_st.cover_seq++;
    });
    ESP_LOGI(TAG, "album cover %dx%d (%d bytes)", s_cover_w, s_cover_h, io.total);
}

// Frees the cover pixels after the page closed. If the screen is busy, tries
// again a second later.
static void cover_release(int64_t now) {
    if (!s_cover_px || now < s_cover_release_retry_us) return;
    LOCKED(s_st.cover_ready = false);
    if (!s_cover_detach || !s_cover_detach()) {
        s_cover_release_retry_us = now + US_PER_S;
        return;
    }
    free(s_cover_px);
    s_cover_px = NULL;
    s_cover_w = s_cover_h = 0;
    s_cover_have[0] = '\0';
    log_heap("after freeing the cover");
}

// The cover URL to download now, or false when none is due.
static bool cover_due(int64_t now, char *url, size_t size) {
    bool active, fetched;
    LOCKED({
        active = s_st.active;
        fetched = s_st.fetched && s_st.now.have_item;
        snprintf(url, size, "%s", s_st.now.cover_url);
    });
    return active && fetched &&
           spot_cover_should_fetch(url, s_cover_have, s_cover_failed, s_cover_failed_at, now, COVER_RETRY_US);
}

// ---------------------------------------------------------------------------
// Login from the setup page
// ---------------------------------------------------------------------------

const char *spot_link_code(const char *client_id, const char *code, const char *verifier, int timeout_ms) {
    if (!s_lock || !s_code_done) return "Spotify is not ready yet";
    if (!spot_valid_client_id(client_id)) return "The client ID must be 32 hex digits";
    bool busy = false;
    LOCKED({
        if (s_code_req.pending) {
            busy = true;
        } else {
            s_code_req.pending = true;
            s_code_req.result = NULL;
            snprintf(s_code_req.client_id, sizeof(s_code_req.client_id), "%s", client_id);
            snprintf(s_code_req.code, sizeof(s_code_req.code), "%s", code);
            snprintf(s_code_req.verifier, sizeof(s_code_req.verifier), "%s", verifier);
        }
    });
    if (busy) return "A login is already in progress";
    (void)xSemaphoreTake(s_code_done, 0);  // drop a stale completion
    home_net_kick();
    if (xSemaphoreTake(s_code_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        const char *r = NULL;
        LOCKED({
            if (s_code_req.pending) {
                // Not started yet (for example the board is offline): cancel it.
                s_code_req.pending = false;
                wipe(&s_code_req, sizeof(s_code_req));
                r = "The board is not online yet. Check its Wi-Fi and try again";
            } else {
                r = s_code_req.result;  // finished just after the timeout
            }
        });
        return r ? r : "Spotify did not answer in time; try again";
    }
    const char *r;
    LOCKED(r = s_code_req.result);
    return r;
}

static void finish_code(const char *result) {
    LOCKED({
        wipe(s_code_req.code, sizeof(s_code_req.code));
        wipe(s_code_req.verifier, sizeof(s_code_req.verifier));
        s_code_req.result = result;
        s_code_req.pending = false;
    });
    xSemaphoreGive(s_code_done);
}

static void do_code_login(void) {
    char client_id[SPOT_CLIENT_ID_LEN + 1];
    char body[900];
    bool ok;
    LOCKED({
        memcpy(client_id, s_code_req.client_id, sizeof(client_id));
        ok = setup_code_body(body, sizeof(body), s_code_req.client_id, s_code_req.code, s_code_req.verifier);
    });
    if (!ok) {
        finish_code("The pasted code is too long");
        return;
    }
    int status = token_post(body);
    wipe(body, sizeof(body));
    bool parsed = status > 0 && spot_parse_token(s_small, &s_tok);
    const char *result = NULL;
    if (status == 200 && parsed && s_tok.refresh[0]) {
        if (home_store_save_spotify_client(client_id) != ESP_OK ||
            home_store_save_spotify_token(s_tok.refresh) != ESP_OK) {
            result = "Could not save the login on the board";
        } else {
            ESP_LOGI(TAG, "linked from the setup page");
            LOCKED(s_link_dirty = true);
        }
    } else if (status < 0) {
        result = "Could not reach Spotify. Check the board's Wi-Fi";
    } else if (strcmp(s_tok.error, "invalid_grant") == 0) {
        result = "Spotify rejected the code. Codes work once and expire after a few minutes; log in again";
    } else if (strcmp(s_tok.error, "invalid_client") == 0) {
        result = "Spotify does not recognise this client ID";
    } else if (strcmp(s_tok.error, "invalid_request") == 0) {
        result = "Spotify refused the request. Check the redirect URI in the dashboard";
    } else {
        result = "Spotify returned an error; try again";
    }
    if (result) ESP_LOGW(TAG, "setup-page login failed: HTTP %d %s", status, s_tok.error);
    wipe(&s_tok, sizeof(s_tok));
    wipe(s_small, sizeof(s_small));
    finish_code(result);
}

// ---------------------------------------------------------------------------
// Worker hooks
// ---------------------------------------------------------------------------

int64_t spot_net_due_us(int64_t now) {
    if (!s_lock) return INT64_MAX;
    bool dirty, active, poll_now, pending, code;
    spot_link_t link;
    LOCKED({
        dirty = s_link_dirty;
        active = s_st.active;
        poll_now = s_poll_now;
        pending = s_pending != SPOT_CMD_NONE;
        code = s_code_req.pending;
        link = s_st.link;
    });
    if (dirty || pending || code) return now;   // a command without a link fails with a toast
    if (!active && s_cover_px) return now > s_cover_release_retry_us ? now : s_cover_release_retry_us;
    if (link != SPOT_LINK_OK) return INT64_MAX;
    if (!active && s_api) return now;   // release the connection
    if (!active) return INT64_MAX;
    if (poll_now) return now;
    char url[SPOT_COVER_URL_MAX];
    if (cover_due(now, url, sizeof(url))) return now;
    int64_t due = s_poll_due_us;
    if (s_token_retry_us > due && !(s_access[0] && now < s_access_exp_us)) due = s_token_retry_us;
    return due;
}

void spot_net_run(int64_t now) {
    if (!s_lock) return;
    bool code;
    LOCKED(code = s_code_req.pending);
    if (code) {
        do_code_login();
        return;  // the link reload (s_link_dirty) runs on the next pass
    }
    bool dirty, active, poll_now, pending;
    spot_link_t link;
    LOCKED({
        dirty = s_link_dirty;
        s_link_dirty = false;
        active = s_st.active;
        poll_now = s_poll_now;
        s_poll_now = false;
        pending = s_pending != SPOT_CMD_NONE;
        link = s_st.link;
    });
    if (!active) cover_release(now);
    if (dirty) {
        spot_net_release();
        load_link();
        LOCKED(link = s_st.link);
        poll_now = true;
    }
    if (link != SPOT_LINK_OK) {
        if (pending) {
            LOCKED({
                s_pending = SPOT_CMD_NONE;
                s_st.busy = false;
                s_st.toast = "Spotify not linked";
                s_st.toast_at_us = now;
            });
        }
        return;
    }
    if (!active && s_api) spot_net_release();
    if (poll_now) s_poll_due_us = 0;

    char url[SPOT_COVER_URL_MAX];
    if (pending) {
        do_command(now);
    } else if (active && now >= s_poll_due_us) {
        do_poll(now);
    } else if (cover_due(now, url, sizeof(url))) {
        do_cover(url);
    }
}

void spot_net_offline(void) {
    spot_net_release();
    if (!s_lock) return;
    bool code;
    LOCKED(code = s_code_req.pending);
    if (code) finish_code("The board is not online yet. Check its Wi-Fi and try again");
    LOCKED({
        if (s_pending != SPOT_CMD_NONE) {
            s_pending = SPOT_CMD_NONE;
            s_st.toast = "No connection";
            s_st.toast_at_us = now_us();
        }
        s_st.busy = false;
    });
}
