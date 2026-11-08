// Host tests for main/spot_json.c and main/spot_logic.c.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "spot_json.h"
#include "spot_logic.h"

// A trimmed but structurally faithful GET /v1/me/player response: nested
// album/artist objects, image arrays, a context, and fields we skip.
static const char PLAYER_JSON[] =
    "{\n"
    "  \"device\": {\"id\": \"abc\", \"is_active\": true, \"is_private_session\": false,\n"
    "    \"is_restricted\": false, \"name\": \"MY-PC\", \"type\": \"Computer\",\n"
    "    \"volume_percent\": 64, \"supports_volume\": true},\n"
    "  \"shuffle_state\": false, \"smart_shuffle\": false, \"repeat_state\": \"off\",\n"
    "  \"timestamp\": 1762588800000,\n"
    "  \"context\": {\"external_urls\": {\"spotify\": \"https://open.spotify.com/playlist/x\"},\n"
    "    \"href\": \"https://api.spotify.com/v1/playlists/x\", \"type\": \"playlist\", \"uri\": \"spotify:playlist:x\"},\n"
    "  \"progress_ms\": 61234,\n"
    "  \"item\": {\n"
    "    \"album\": {\"album_type\": \"album\", \"total_tracks\": 12,\n"
    "      \"artists\": [{\"name\": \"Album Artist\", \"type\": \"artist\"}],\n"
    "      \"images\": [{\"height\": 640, \"url\": \"https://i.scdn.co/image/a\", \"width\": 640},\n"
    "                 {\"height\": 300, \"url\": \"https://i.scdn.co/image/b\", \"width\": 300},\n"
    "                 {\"height\": 64, \"url\": \"https://i.scdn.co/image/c\", \"width\": 64}],\n"
    "      \"name\": \"Caf\\u00e9 Sessions\", \"release_date\": \"2024-05-01\"},\n"
    "    \"artists\": [{\"external_urls\": {\"spotify\": \"https://open.spotify.com/artist/1\"},\n"
    "                  \"id\": \"1\", \"name\": \"Beyonc\\u00e9\", \"type\": \"artist\"},\n"
    "                 {\"id\": \"2\", \"name\": \"\xE5\x91\xA8\xE6\x9D\xB0\xE4\xBC\xA6\", \"type\": \"artist\"},\n"
    "                 {\"id\": \"3\", \"name\": \"Third\"},\n"
    "                 {\"id\": \"4\", \"name\": \"Fourth is dropped\"}],\n"
    "    \"disc_number\": 1, \"duration_ms\": 215000, \"explicit\": false,\n"
    "    \"id\": \"4uLU6hMCjMI75M1A2tKUQC\", \"is_local\": false,\n"
    "    \"name\": \"Song \\\"Quoted\\\" \\ud83c\\udfb5 \\/ end\",\n"
    "    \"preview_url\": null, \"track_number\": 3, \"type\": \"track\"\n"
    "  },\n"
    "  \"currently_playing_type\": \"track\",\n"
    "  \"actions\": {\"disallows\": {\"resuming\": true, \"skipping_prev\": false}},\n"
    "  \"is_playing\": true\n"
    "}\n";

// ---------------------------------------------------------------------------
// Streaming scanner
// ---------------------------------------------------------------------------

typedef struct {
    char log[2048];
    int count;
} collect_t;

static void collect_cb(void *ctx, const char *path, spot_json_type_t type, const char *value, bool truncated) {
    collect_t *c = ctx;
    char line[320];
    snprintf(line, sizeof(line), "%s=%d:%s%s|", path, (int)type, value, truncated ? "~" : "");
    strncat(c->log, line, sizeof(c->log) - strlen(c->log) - 1);
    c->count++;
}

static bool scan(const char *json, size_t chunk, collect_t *out) {
    memset(out, 0, sizeof(*out));
    spot_json_t s;
    spot_json_init(&s, collect_cb, out);
    size_t len = strlen(json);
    bool ok = true;
    for (size_t i = 0; i < len && ok; i += chunk) {
        size_t n = len - i < chunk ? len - i : chunk;
        ok = spot_json_feed(&s, json + i, n);
    }
    return ok && spot_json_finish(&s);
}

static void test_json_paths(void) {
    collect_t c;
    assert(scan("{\"a\":1,\"b\":{\"c\":[true,false,null,\"x\"],\"d\":-2.5e3},\"e\":[]}", 1000, &c));
    assert(strcmp(c.log, "a=1:1|b.c.0=2:true|b.c.1=3:false|b.c.2=4:null|b.c.3=0:x|b.d=1:-2.5e3|") == 0);

    // Arrays of objects, nested arrays, and empty containers.
    assert(scan("[{\"k\":[[1],[2,3]]},{},{\"z\":\"\"}]", 1000, &c));
    assert(strcmp(c.log, "0.k.0.0=1:1|0.k.1.0=1:2|0.k.1.1=1:3|2.z=0:|") == 0);

    // A scalar at the top level.
    assert(scan("  42  ", 1000, &c) && strcmp(c.log, "=1:42|") == 0);
    assert(scan("\"hi\"", 1000, &c) && strcmp(c.log, "=0:hi|") == 0);
}

static void test_json_escapes(void) {
    collect_t c;
    assert(scan("{\"s\":\"a\\\"b\\\\c\\/d\\n\\u00e9\\u4e2d\"}", 1000, &c));
    assert(strcmp(c.log, "s=0:a\"b\\c/d\n\xC3\xA9\xE4\xB8\xAD|") == 0);
    // Surrogate pair -> one 4-byte character; lone halves -> '?'.
    assert(scan("{\"s\":\"\\ud83c\\udfb5|\\ud83c|\\udfb5|\\ud83cx\"}", 1000, &c));
    assert(strcmp(c.log, "s=0:\xF0\x9F\x8E\xB5|?|?|?x|") == 0);
    // Raw UTF-8 passes through.
    assert(scan("{\"s\":\"\xE5\x91\xA8\"}", 1000, &c) && strcmp(c.log, "s=0:\xE5\x91\xA8|") == 0);
}

static void test_json_errors(void) {
    collect_t c;
    assert(!scan("", 1000, &c));                      // nothing
    assert(!scan("{", 1000, &c));                     // unfinished
    assert(!scan("{\"a\":1,}", 1000, &c));            // trailing comma
    assert(!scan("{\"a\" 1}", 1000, &c));             // missing colon
    assert(!scan("{\"a\":1]", 1000, &c));             // mismatched bracket
    assert(!scan("[1 2]", 1000, &c));                 // missing comma
    assert(!scan("{\"a\":tru}", 1000, &c));           // bad literal
    assert(!scan("{\"a\":01}", 1000, &c));            // leading zero
    assert(!scan("{\"a\":\"x\ny\"}", 1000, &c));     // raw control character
    assert(!scan("{\"a\":\"\\q\"}", 1000, &c));       // bad escape
    assert(!scan("{\"a\":\"\\u12g4\"}", 1000, &c));   // bad hex
    assert(!scan("{} {}", 1000, &c));                 // two documents
    assert(!scan("{a:1}", 1000, &c));                 // unquoted key

    // Too deep: SPOT_JSON_DEPTH_MAX levels are fine, one more is not.
    char deep[64] = "";
    for (int i = 0; i < SPOT_JSON_DEPTH_MAX; i++) strcat(deep, "[");
    for (int i = 0; i < SPOT_JSON_DEPTH_MAX; i++) strcat(deep, "]");
    assert(scan(deep, 1000, &c));
    char deeper[64] = "[";
    strcat(deeper, deep);
    strcat(deeper, "]");
    assert(!scan(deeper, 1000, &c));

    // Feeding after an error keeps failing.
    spot_json_t s;
    spot_json_init(&s, NULL, NULL);
    assert(!spot_json_feed(&s, "]", 1));
    assert(!spot_json_feed(&s, "{}", 2));
    assert(!spot_json_finish(&s));
}

static void test_json_limits(void) {
    collect_t c;
    // A value longer than the buffer is truncated on a UTF-8 boundary and flagged.
    char json[600] = "{\"s\":\"";
    for (int i = 0; i < 100; i++) strcat(json, "\xE4\xB8\xAD");  // 300 bytes of 3-byte characters
    strcat(json, "\",\"n\":1}");
    assert(scan(json, 7, &c));
    const char *v = strstr(c.log, "s=0:") + 4;
    const char *end = strstr(v, "~|");
    assert(end);
    size_t len = (size_t)(end - v);
    assert(len <= SPOT_JSON_VALUE_MAX - 1 && len % 3 == 0 && len >= SPOT_JSON_VALUE_MAX - 3);
    assert(strstr(c.log, "|n=1:1|"));

    // A key longer than the key buffer never matches; its siblings still do.
    char longkey[200] = "{\"";
    for (int i = 0; i < 40; i++) strcat(longkey, "k");
    strcat(longkey, "\":{\"name\":\"x\"},\"name\":\"y\"}");
    assert(scan(longkey, 3, &c));
    assert(strcmp(c.log, "?.name=0:x|name=0:y|") == 0);

    // A path deeper than the path buffer ends in '?'.
    char longpath[400] = "";
    int levels = 10;
    for (int i = 0; i < levels; i++) strcat(longpath, "{\"abcdefghijklmn\":");
    strcat(longpath, "1");
    for (int i = 0; i < levels; i++) strcat(longpath, "}");
    assert(scan(longpath, 5, &c));
    assert(c.count == 1 && strstr(c.log, "?=1:1|"));
    assert(strlen(c.log) < SPOT_JSON_PATH_MAX + 8);
}

static void test_json_chunking(void) {
    collect_t whole, part;
    assert(scan(PLAYER_JSON, sizeof(PLAYER_JSON), &whole));
    assert(whole.count > 40);
    for (size_t chunk = 1; chunk < 80; chunk++) {
        assert(scan(PLAYER_JSON, chunk, &part));
        assert(strcmp(whole.log, part.log) == 0);
    }
}

// ---------------------------------------------------------------------------
// Playback state
// ---------------------------------------------------------------------------

static void test_parse_now(void) {
    spot_now_t n;
    assert(spot_parse_now(PLAYER_JSON, &n));
    assert(n.have_item && n.is_playing && n.type == SPOT_ITEM_TRACK);
    assert(n.progress_ms == 61234 && n.duration_ms == 215000);
    assert(strcmp(n.id, "4uLU6hMCjMI75M1A2tKUQC") == 0);
    assert(strcmp(n.title, "Song \"Quoted\" \xF0\x9F\x8E\xB5 / end") == 0);
    // Only the track's own artists, first three, album artists excluded.
    assert(strcmp(n.artist, "Beyonc\xC3\xA9, \xE5\x91\xA8\xE6\x9D\xB0\xE4\xBC\xA6, Third") == 0);
    assert(strcmp(n.album, "Caf\xC3\xA9 Sessions") == 0);
    assert(strcmp(n.device, "MY-PC") == 0);
    assert(n.disallow_resume && !n.disallow_prev && !n.disallow_pause && !n.disallow_next);
    // 300 px at 1/4 (75 px) is about as sharp as 640 px at 1/8 (80 px) and a smaller download.
    assert(strcmp(n.cover_url, "https://i.scdn.co/image/b") == 0 && n.cover_width == 300);

    // Streaming in odd chunks gives the same result.
    spot_now_parser_t p;
    spot_now_t m;
    spot_now_begin(&p, &m);
    for (size_t i = 0; i < sizeof(PLAYER_JSON) - 1; i += 13) {
        size_t len = sizeof(PLAYER_JSON) - 1 - i < 13 ? sizeof(PLAYER_JSON) - 1 - i : 13;
        assert(spot_now_feed(&p, PLAYER_JSON + i, len));
    }
    assert(spot_now_end(&p));
    assert(memcmp(&m, &n, sizeof(n)) == 0);
}

static void test_parse_now_variants(void) {
    spot_now_t n;
    // Paused episode: the show name stands in for the artist.
    assert(spot_parse_now("{\"is_playing\":false,\"progress_ms\":5000,\"currently_playing_type\":\"episode\","
                          "\"item\":{\"type\":\"episode\",\"name\":\"Ep 12\",\"duration_ms\":3600000,"
                          "\"show\":{\"name\":\"The Show\",\"publisher\":\"P\"}}}", &n));
    assert(n.have_item && !n.is_playing && n.type == SPOT_ITEM_EPISODE);
    assert(strcmp(n.title, "Ep 12") == 0 && strcmp(n.artist, "The Show") == 0);

    // Ad break: no item, but something is playing.
    assert(spot_parse_now("{\"is_playing\":true,\"progress_ms\":1000,\"item\":null,"
                          "\"currently_playing_type\":\"ad\"}", &n));
    assert(!n.have_item && n.type == SPOT_ITEM_AD && n.is_playing);

    // Unknown type with no item: nothing to show.
    assert(spot_parse_now("{\"is_playing\":true,\"item\":null,\"currently_playing_type\":\"unknown\"}", &n));
    assert(!n.have_item && n.type == SPOT_ITEM_NONE && !n.is_playing);

    // Missing numbers stay unknown.
    assert(spot_parse_now("{\"item\":{\"name\":\"T\"},\"progress_ms\":null}", &n));
    assert(n.have_item && n.progress_ms == -1 && n.duration_ms == -1 && n.type == SPOT_ITEM_TRACK);

    // Long titles are cut on a character boundary.
    char json[700] = "{\"item\":{\"name\":\"";
    for (int i = 0; i < 60; i++) strcat(json, "\xE4\xB8\xAD");
    strcat(json, "\"}}");
    assert(spot_parse_now(json, &n));
    size_t len = strlen(n.title);
    assert(len < SPOT_TITLE_MAX && len % 3 == 0 && len >= SPOT_TITLE_MAX - 3);

    // Malformed JSON fails.
    assert(!spot_parse_now("{\"item\":{\"name\":\"T\"}", &n));
}

static void test_cover(void) {
    assert(spot_cover_scale(64, 80) == 0);
    assert(spot_cover_scale(80, 80) == 0);
    assert(spot_cover_scale(160, 80) == 1);
    assert(spot_cover_scale(300, 80) == 2 && spot_cover_scaled(300, 2) == 75);
    assert(spot_cover_scale(640, 80) == 3 && spot_cover_scaled(640, 3) == 80);
    assert(spot_cover_scale(641, 80) == -1);   // 1/8 would give 81 px
    assert(spot_cover_scale(0, 80) == -1);
    assert(spot_cover_scaled(301, 2) == 76);   // rounds up like TJpgDec

    spot_image_t imgs[SPOT_COVER_IMAGES];
    memset(imgs, 0, sizeof(imgs));
    strcpy(imgs[0].url, "https://x/640"); imgs[0].width = 640;
    strcpy(imgs[1].url, "https://x/300"); imgs[1].width = 300;
    strcpy(imgs[2].url, "https://x/64");  imgs[2].width = 64;
    assert(spot_cover_pick(imgs, 3, 80) == 1);
    assert(spot_cover_pick(imgs, 1, 80) == 0);              // only 640: 1/8
    imgs[1].width = 0;                                      // unknown width ignored when others exist
    assert(spot_cover_pick(imgs, 3, 80) == 0);              // 80 px clearly beats 64 px
    memset(imgs, 0, sizeof(imgs));
    strcpy(imgs[0].url, "https://x/u");
    assert(spot_cover_pick(imgs, 1, 80) == 0);              // unknown only: still try it
    imgs[0].width = 2000;
    assert(spot_cover_pick(imgs, 1, 80) == -1);             // too big to decode
    assert(spot_cover_pick(imgs, 0, 80) == -1);

    // Episode images, and none at all.
    spot_now_t n;
    assert(spot_parse_now("{\"item\":{\"type\":\"episode\",\"name\":\"E\",\"images\":["
                          "{\"url\":\"https://i.scdn.co/image/e300\",\"width\":300}]}}", &n));
    assert(strcmp(n.cover_url, "https://i.scdn.co/image/e300") == 0);
    assert(spot_parse_now("{\"item\":{\"name\":\"T\",\"album\":{\"images\":[]}}}", &n));
    assert(n.cover_url[0] == '\0');
    // A non-https or overlong URL is never stored.
    assert(spot_parse_now("{\"item\":{\"name\":\"T\",\"album\":{\"images\":["
                          "{\"url\":\"http://plain/x\",\"width\":300}]}}}", &n));
    assert(n.cover_url[0] == '\0');
    // No item: no cover.
    assert(spot_parse_now("{\"item\":null,\"currently_playing_type\":\"ad\"}", &n));
    assert(n.cover_url[0] == '\0');

    const int64_t S = 1000000;
    assert(spot_cover_should_fetch("u1", "", "", 0, 0, 60 * S));
    assert(!spot_cover_should_fetch("u1", "u1", "", 0, 0, 60 * S));     // already shown
    assert(!spot_cover_should_fetch("", "u1", "", 0, 0, 60 * S));       // nothing to fetch
    assert(!spot_cover_should_fetch("u2", "u1", "u2", 10 * S, 20 * S, 60 * S));  // failed recently
    assert(spot_cover_should_fetch("u2", "u1", "u2", 10 * S, 71 * S, 60 * S));   // retry later
    assert(spot_cover_should_fetch("u3", "u1", "u2", 10 * S, 20 * S, 60 * S));   // a different album
}

static void test_progress(void) {
    spot_now_t n;
    memset(&n, 0, sizeof(n));
    n.progress_ms = 10000;
    n.duration_ms = 12000;
    n.is_playing = true;
    assert(spot_progress_at(&n, 1000000, 1000000) == 10000);
    assert(spot_progress_at(&n, 1000000, 2500000) == 11500);
    assert(spot_progress_at(&n, 1000000, 9000000) == 12000);   // clamped to the end
    n.is_playing = false;
    assert(spot_progress_at(&n, 1000000, 9000000) == 10000);   // paused: frozen
    n.progress_ms = -1;
    assert(spot_progress_at(&n, 0, 1) == -1);

    char buf[16];
    spot_fmt_time(buf, sizeof(buf), 0);
    assert(strcmp(buf, "0:00") == 0);
    spot_fmt_time(buf, sizeof(buf), 187999);
    assert(strcmp(buf, "3:07") == 0);
    spot_fmt_time(buf, sizeof(buf), 3723000);
    assert(strcmp(buf, "1:02:03") == 0);
    spot_fmt_time(buf, sizeof(buf), -1);
    assert(strcmp(buf, "-:--") == 0);

    assert(spot_progress_permille(0, 1000) == 0);
    assert(spot_progress_permille(500, 1000) == 500);
    assert(spot_progress_permille(2000, 1000) == 1000);
    assert(spot_progress_permille(500, -1) == 0);
    assert(spot_progress_permille(-1, 1000) == 0);

    for (uint32_t t = 0; t < 200; t++) {
        for (int b = 0; b < 9; b++) {
            int h = spot_eq_height(b, t, 10, 70);
            assert(h >= 10 && h <= 70);
        }
    }
    assert(spot_eq_height(3, 7, 10, 70) == spot_eq_height(3, 7, 10, 70));
    assert(spot_eq_height(0, 0, 5, 5) == 5);
}

// ---------------------------------------------------------------------------
// Tokens and results
// ---------------------------------------------------------------------------

static void test_tokens(void) {
    spot_token_t t;
    assert(spot_parse_token("{\"access_token\":\"BQDabc-123_x\",\"token_type\":\"Bearer\",\"expires_in\":3600,"
                            "\"refresh_token\":\"AQCnewRefreshToken_0123456789\",\"scope\":\"user-read-playback-state\"}",
                            &t));
    assert(strcmp(t.access, "BQDabc-123_x") == 0 && t.expires_in_s == 3600);
    assert(strcmp(t.refresh, "AQCnewRefreshToken_0123456789") == 0);
    assert(spot_token_result(200, &t, true) == SPOT_TOK_OK);

    // No rotation: keep using the stored refresh token.
    assert(spot_parse_token("{\"access_token\":\"BQDxyz\",\"expires_in\":3600}", &t));
    assert(t.refresh[0] == '\0');

    // A malformed refresh token in the reply is ignored, not saved.
    assert(spot_parse_token("{\"access_token\":\"BQDxyz\",\"refresh_token\":\"bad token\"}", &t));
    assert(t.refresh[0] == '\0' && t.expires_in_s == 3600);

    assert(!spot_parse_token("{\"error\":\"invalid_grant\",\"error_description\":\"Refresh token revoked\"}", &t));
    assert(strcmp(t.error, "invalid_grant") == 0);
    assert(spot_token_result(400, &t, false) == SPOT_TOK_REVOKED);
    assert(!spot_parse_token("{\"error\":\"invalid_client\"}", &t));
    assert(spot_token_result(400, &t, false) == SPOT_TOK_REVOKED);
    assert(!spot_parse_token("{\"error\":\"server_error\"}", &t));
    assert(spot_token_result(503, &t, false) == SPOT_TOK_RETRY);
    assert(!spot_parse_token("<html>", &t));
    assert(spot_token_result(502, &t, false) == SPOT_TOK_RETRY);
    assert(spot_token_result(-1, &t, false) == SPOT_TOK_RETRY);

    // An access token that fills the buffer is treated as cut off.
    char big[SPOT_ACCESS_MAX + 64] = "{\"access_token\":\"";
    for (int i = 0; i < SPOT_ACCESS_MAX + 10; i++) strcat(big, "a");
    strcat(big, "\"}");
    assert(!spot_parse_token(big, &t));

    char body[512];
    assert(spot_refresh_body(body, sizeof(body), "0123456789abcdef0123456789abcdef", "AQC_refresh-token.0123"));
    assert(strcmp(body, "grant_type=refresh_token&refresh_token=AQC_refresh-token.0123"
                        "&client_id=0123456789abcdef0123456789abcdef") == 0);
    assert(!spot_refresh_body(body, 40, "0123456789abcdef0123456789abcdef", "AQC_refresh-token.0123"));
    assert(!spot_refresh_body(body, sizeof(body), "nothex", "AQC_refresh-token.0123"));
    assert(!spot_refresh_body(body, sizeof(body), "0123456789abcdef0123456789abcdef", "has space token 123"));
}

static void test_classify(void) {
    assert(spot_classify(200, NULL, false) == SPOT_RES_OK);
    assert(spot_classify(204, NULL, false) == SPOT_RES_IDLE);
    assert(spot_classify(204, NULL, true) == SPOT_RES_OK);
    assert(spot_classify(202, NULL, true) == SPOT_RES_OK);
    assert(spot_classify(401, "", false) == SPOT_RES_AUTH);
    assert(spot_classify(403, "PREMIUM_REQUIRED", true) == SPOT_RES_PREMIUM);
    assert(spot_classify(403, "", false) == SPOT_RES_FORBIDDEN);
    assert(spot_classify(403, NULL, true) == SPOT_RES_FORBIDDEN);
    assert(spot_classify(404, "NO_ACTIVE_DEVICE", true) == SPOT_RES_NO_DEVICE);
    assert(spot_classify(404, "", true) == SPOT_RES_NO_DEVICE);
    assert(spot_classify(404, "", false) == SPOT_RES_BAD);
    assert(spot_classify(429, NULL, false) == SPOT_RES_RATE_LIMIT);
    assert(spot_classify(500, NULL, false) == SPOT_RES_SERVER);
    assert(spot_classify(503, NULL, true) == SPOT_RES_SERVER);
    assert(spot_classify(400, NULL, false) == SPOT_RES_BAD);
    assert(spot_classify(-1, NULL, false) == SPOT_RES_NETWORK);

    assert(spot_retry_after_s("7") == 7);
    assert(spot_retry_after_s(" 120") == 120);
    assert(spot_retry_after_s("0") == 1);
    assert(spot_retry_after_s("99999999") == 600);
    assert(spot_retry_after_s(NULL) == 30);
    assert(spot_retry_after_s("Wed, 21 Oct 2026 07:28:00 GMT") == 30);
}

static void test_poll_delay(void) {
    spot_poll_in_t in;
    memset(&in, 0, sizeof(in));
    assert(spot_poll_delay_ms(&in) == 0);  // first poll right away

    in.result = SPOT_RES_OK;
    in.have_item = true;
    in.is_playing = true;
    in.progress_ms = 10000;
    in.duration_ms = 200000;
    assert(spot_poll_delay_ms(&in) == SPOT_POLL_PLAYING_MS);
    in.progress_ms = 197000;  // 3 s left: poll just after the track ends
    assert(spot_poll_delay_ms(&in) == 3700);
    in.progress_ms = 200000;  // at the end: not faster than once a second
    assert(spot_poll_delay_ms(&in) == 1000);
    in.duration_ms = -1;
    assert(spot_poll_delay_ms(&in) == SPOT_POLL_PLAYING_MS);

    in.is_playing = false;
    assert(spot_poll_delay_ms(&in) == SPOT_POLL_PAUSED_MS);
    in.have_item = false;
    assert(spot_poll_delay_ms(&in) == SPOT_POLL_IDLE_MS);
    in.result = SPOT_RES_IDLE;
    assert(spot_poll_delay_ms(&in) == SPOT_POLL_IDLE_MS);

    in.result = SPOT_RES_NETWORK;
    in.failures = 1;
    assert(spot_poll_delay_ms(&in) == 5000);
    in.failures = 2;
    assert(spot_poll_delay_ms(&in) == 10000);
    in.failures = 3;
    assert(spot_poll_delay_ms(&in) == 20000);
    in.failures = 20;
    assert(spot_poll_delay_ms(&in) == SPOT_POLL_ERROR_MAX_MS);

    in.result = SPOT_RES_RATE_LIMIT;
    in.retry_after_s = 12;
    assert(spot_poll_delay_ms(&in) == 12000);
    in.result = SPOT_RES_FORBIDDEN;
    assert(spot_poll_delay_ms(&in) == SPOT_POLL_BLOCKED_MS);

    in.after_command = true;
    assert(spot_poll_delay_ms(&in) == SPOT_POLL_AFTER_CMD_MS);
}

static void test_commands(void) {
    spot_cmd_req_t r;
    assert(spot_cmd_request(SPOT_CMD_NEXT, false, &r));
    assert(strcmp(r.method, "POST") == 0 && strcmp(r.path, "/v1/me/player/next") == 0);
    assert(spot_cmd_request(SPOT_CMD_PREV, true, &r));
    assert(strcmp(r.method, "POST") == 0 && strcmp(r.path, "/v1/me/player/previous") == 0);
    assert(spot_cmd_request(SPOT_CMD_TOGGLE, true, &r));
    assert(strcmp(r.method, "PUT") == 0 && strcmp(r.path, "/v1/me/player/pause") == 0);
    assert(spot_cmd_request(SPOT_CMD_TOGGLE, false, &r));
    assert(strcmp(r.method, "PUT") == 0 && strcmp(r.path, "/v1/me/player/play") == 0);
    assert(!spot_cmd_request(SPOT_CMD_NONE, false, &r));

    assert(spot_cmd_toast(SPOT_RES_OK) == NULL);
    assert(strcmp(spot_cmd_toast(SPOT_RES_NO_DEVICE), "No active device") == 0);
    assert(strcmp(spot_cmd_toast(SPOT_RES_PREMIUM), "Needs Spotify Premium") == 0);
    assert(spot_cmd_toast(SPOT_RES_BAD) != NULL);
}

static void test_page_state(void) {
    spot_page_in_t in;
    memset(&in, 0, sizeof(in));
    spot_page_t p = spot_page_state(&in);
    assert(p.show == SPOT_SHOW_MESSAGE && strcmp(p.title, "Link Spotify") == 0);

    in.link = SPOT_LINK_REVOKED;
    p = spot_page_state(&in);
    assert(strcmp(p.title, "Link expired") == 0 && p.warning);

    in.link = SPOT_LINK_OK;
    p = spot_page_state(&in);
    assert(strcmp(p.title, "No Wi-Fi") == 0);

    in.wifi_configured = true;
    p = spot_page_state(&in);
    assert(strcmp(p.title, "Offline") == 0);

    in.wifi_connected = true;
    p = spot_page_state(&in);
    assert(strcmp(p.title, "Loading") == 0 && !p.warning);

    in.last = SPOT_RES_FORBIDDEN;
    p = spot_page_state(&in);
    assert(strcmp(p.title, "Access denied") == 0 && p.warning);

    in.last = SPOT_RES_IDLE;
    in.fetched = true;
    p = spot_page_state(&in);
    assert(strcmp(p.title, "Nothing playing") == 0);

    in.last = SPOT_RES_OK;
    in.have_item = true;
    in.is_playing = true;
    p = spot_page_state(&in);
    assert(p.show == SPOT_SHOW_TRACK && strcmp(p.status, "PLAYING") == 0 && !p.warning);
    in.is_playing = false;
    p = spot_page_state(&in);
    assert(strcmp(p.status, "PAUSED") == 0);

    // A known track stays on screen through errors, with a warning line.
    in.last = SPOT_RES_NETWORK;
    p = spot_page_state(&in);
    assert(p.show == SPOT_SHOW_TRACK && strcmp(p.status, "RECONNECTING") == 0 && p.warning);
    in.last = SPOT_RES_RATE_LIMIT;
    p = spot_page_state(&in);
    assert(strcmp(p.status, "SPOTIFY BUSY") == 0);
    in.wifi_connected = false;
    p = spot_page_state(&in);
    assert(p.show == SPOT_SHOW_TRACK && strcmp(p.status, "OFFLINE") == 0);
}

// ---------------------------------------------------------------------------
// Text for the fonts
// ---------------------------------------------------------------------------

// Fake font chain: ASCII, degree, bullet, and a few CJK characters.
static bool fake_has(uint32_t cp, void *ctx) {
    (void)ctx;
    return (cp >= 0x20 && cp < 0x7F) || cp == 0xB0 || cp == 0x2022 || cp == 0x4E2D || cp == 0x6587 ||
           cp == 0x3042;
}

static void fold(const char *in, char *out, size_t size) {
    spot_text_for_font(in, out, size, fake_has, NULL);
}

static void test_text(void) {
    char out[64];
    fold("Plain ASCII 123", out, sizeof(out));
    assert(strcmp(out, "Plain ASCII 123") == 0);
    // Supported CJK and kana pass through; unsupported ones become '?'.
    fold("\xE4\xB8\xAD\xE6\x96\x87 \xE3\x81\x82 \xE5\x91\xA8", out, sizeof(out));
    assert(strcmp(out, "\xE4\xB8\xAD\xE6\x96\x87 \xE3\x81\x82 ?") == 0);
    // Accents fold to the base letter.
    fold("Beyonc\xC3\xA9 \xC3\x85ngstr\xC3\xB6m \xC5\x81\xC3\xB3" "d\xC5\xBA \xC3\x9F", out, sizeof(out));
    assert(strcmp(out, "Beyonce Angstrom Lodz s") == 0);
    // Typography to ASCII.
    fold("Don\xE2\x80\x99t \xE2\x80\x9CStop\xE2\x80\x9D \xE2\x80\x94 Live\xE2\x80\xA6", out, sizeof(out));
    assert(strcmp(out, "Don't \"Stop\" - Live...") == 0);
    // Full-width ASCII, CJK punctuation, zero-width characters, emoji.
    fold("\xEF\xBC\xA1\xEF\xBC\xA2\xEF\xBC\x91\xE3\x80\x81\xE3\x80\x82\xE2\x80\x8B\xF0\x9F\x8E\xB5", out, sizeof(out));
    assert(strcmp(out, "AB1,.?") == 0);
    // Glyphs the font has are kept even outside ASCII.
    fold("31\xC2\xB0 \xE2\x80\xA2 x", out, sizeof(out));
    assert(strcmp(out, "31\xC2\xB0 \xE2\x80\xA2 x") == 0);
    // Control characters become spaces; invalid bytes become '?'.
    fold("a\tb\nc\xFF" "d\xC3", out, sizeof(out));
    assert(strcmp(out, "a b c?d?") == 0);
    // Overlong encodings and surrogates are invalid.
    fold("\xC0\xAF\xED\xA0\x80", out, sizeof(out));
    assert(strcmp(out, "?????") == 0);  // one '?' per rejected byte
    // Truncation never splits a character.
    fold("\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD", out, 8);
    assert(strcmp(out, "\xE4\xB8\xAD\xE4\xB8\xAD") == 0);
    fold("ab\xE2\x80\xA6", out, 5);  // "..." does not fit after "ab"
    assert(strcmp(out, "ab") == 0);
    // No glyph callback: everything outside ASCII is replaced.
    spot_text_for_font("\xE4\xB8\xAD" "a", out, sizeof(out), NULL, NULL);
    assert(strcmp(out, "?a") == 0);
    spot_text_for_font(NULL, out, sizeof(out), fake_has, NULL);
    assert(out[0] == '\0');
}

static void test_validation(void) {
    assert(spot_valid_client_id("0123456789abcdef0123456789ABCDEF"));
    assert(!spot_valid_client_id("0123456789abcdef0123456789abcde"));
    assert(!spot_valid_client_id("0123456789abcdef0123456789abcdeg"));
    assert(!spot_valid_client_id(NULL));
    assert(spot_valid_refresh_token("AQD-_.~0123456789abcdef"));
    assert(!spot_valid_refresh_token("short"));
    assert(!spot_valid_refresh_token("contains space 0123456789"));
    assert(!spot_valid_refresh_token("plus+sign0123456789"));
    char t[SPOT_REFRESH_MAX + 2];
    memset(t, 'a', SPOT_REFRESH_MAX);
    t[SPOT_REFRESH_MAX] = '\0';
    assert(spot_valid_refresh_token(t));
    t[SPOT_REFRESH_MAX] = 'a';
    t[SPOT_REFRESH_MAX + 1] = '\0';
    assert(!spot_valid_refresh_token(t));
}

int main(void) {
    test_json_paths();
    test_json_escapes();
    test_json_errors();
    test_json_limits();
    test_json_chunking();
    test_parse_now();
    test_parse_now_variants();
    test_progress();
    test_cover();
    test_tokens();
    test_classify();
    test_poll_delay();
    test_commands();
    test_page_state();
    test_text();
    test_validation();
    printf("spotify logic tests: PASS\n");
    return 0;
}
