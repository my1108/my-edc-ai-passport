// main/setup_portal.c - see setup_portal.h.
#include "setup_portal.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "home_net.h"
#include "home_store.h"
#include "lwip/inet.h"
#include "spot_net.h"

static const char *TAG = "setup";

#define BODY_MAX       1100   // the pasted Spotify address is the longest input
#define LOGIN_WAIT_MS  20000
#define TOKEN_LEN      22

extern const char setup_page_start[] asm("_binary_setup_page_html_start");
extern const char setup_page_end[] asm("_binary_setup_page_html_end");

static httpd_handle_t s_server;
static char s_token[TOKEN_LEN + 1];
static volatile const char *s_event;
static volatile int64_t s_event_at;
static bool s_seen_phone;

// Half-finished Spotify login (httpd task only).
static struct {
    bool active;
    char client_id[SPOT_CLIENT_ID_LEN + 1];
    char verifier[SETUP_VERIFIER_LEN + 1];
    char state[SETUP_STATE_LEN + 1];
} s_login;

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static void set_event(const char *e) {
    s_event_at = esp_timer_get_time();
    s_event = e;
}

const char *setup_portal_event(int64_t *at_us) {
    if (at_us) *at_us = s_event_at;
    return (const char *)s_event;
}

bool setup_portal_running(void) {
    return s_server != NULL;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void random_text(char *out, size_t len) {
    uint8_t r[SETUP_VERIFIER_LEN];
    if (len > sizeof(r)) len = sizeof(r);
    esp_fill_random(r, len);
    setup_random_text(r, len, out);
    wipe(r, sizeof(r));
}

static esp_err_t send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

// {"ok":true|false,"message":"..."}
static esp_err_t send_result(httpd_req_t *req, bool ok, const char *message) {
    char json[256];
    snprintf(json, sizeof(json), "{\"ok\":%s,\"message\":", ok ? "true" : "false");
    setup_json_str(json, sizeof(json), message);
    strlcat(json, "}", sizeof(json));
    return send_json(req, json);
}

static bool token_ok(httpd_req_t *req) {
    char got[TOKEN_LEN + 2];
    if (httpd_req_get_hdr_value_str(req, "X-Setup-Token", got, sizeof(got)) != ESP_OK) return false;
    return s_token[0] && strcmp(got, s_token) == 0;
}

static esp_err_t forbid(httpd_req_t *req) {
    httpd_resp_set_status(req, "403 Forbidden");
    return send_result(req, false, "Reload the page and try again");
}

// Reads the request body (form encoded) into `buf`. Returns false if too big.
static bool read_body(httpd_req_t *req, char *buf, size_t size) {
    if (req->content_len >= size) return false;
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return false;
        got += (size_t)n;
    }
    buf[got] = '\0';
    return true;
}

// Whether the phone reached us through the setup hotspot (no internet there).
static bool via_hotspot(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) return false;
    char ip[48] = "";
    if (addr.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&addr)->sin_addr, ip, sizeof(ip));
    } else if (addr.ss_family == AF_INET6) {
        // lwIP may report IPv4-mapped addresses on a dual-stack socket.
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&addr)->sin6_addr, ip, sizeof(ip));
    }
    return strstr(ip, SETUP_AP_IP) != NULL;
}

static void note_phone(void) {
    if (!s_seen_phone) {
        s_seen_phone = true;
        set_event("Phone connected");
    }
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static esp_err_t h_page(httpd_req_t *req) {
    note_phone();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // EMBED_TXTFILES adds a trailing NUL; do not send it.
    return httpd_resp_send(req, setup_page_start, setup_page_end - setup_page_start - 1);
}

static esp_err_t h_session(httpd_req_t *req) {
    // Readable only by the page itself: no CORS headers are ever sent.
    char json[64];
    snprintf(json, sizeof(json), "{\"token\":\"%s\"}", s_token);
    return send_json(req, json);
}

static esp_err_t h_status(httpd_req_t *req) {
    if (!token_ok(req)) return forbid(req);
    home_status_t st;
    home_net_get_status(&st);
    spot_status_t sp;
    spot_get_status(&sp);
    char cid[SPOT_CLIENT_ID_LEN + 1], refresh[SPOT_REFRESH_MAX + 1];
    home_store_load_spotify(cid, sizeof(cid), refresh, sizeof(refresh));
    wipe(refresh, sizeof(refresh));

    char json[400] = "{\"wifi\":{\"ssid\":";
    setup_json_str(json, sizeof(json), st.ssid);
    strlcat(json, ",\"connected\":", sizeof(json));
    strlcat(json, st.wifi_connected && st.ip[0] ? "true" : "false", sizeof(json));
    strlcat(json, ",\"ip\":", sizeof(json));
    setup_json_str(json, sizeof(json), st.ip);
    strlcat(json, "},\"via_ap\":", sizeof(json));
    strlcat(json, via_hotspot(req) ? "true" : "false", sizeof(json));
    strlcat(json, ",\"spotify\":{\"link\":", sizeof(json));
    setup_json_str(json, sizeof(json), sp.link == SPOT_LINK_OK ? "ok" : sp.link == SPOT_LINK_REVOKED ? "revoked" : "none");
    strlcat(json, ",\"client_id\":", sizeof(json));
    setup_json_str(json, sizeof(json), cid);  // not secret; prefills the form
    strlcat(json, "}}", sizeof(json));
    return send_json(req, json);
}

static esp_err_t h_scan(httpd_req_t *req) {
    if (!token_ok(req)) return forbid(req);
    static home_scan_ap_t aps[HOME_SCAN_MAX];
    int n = home_net_scan(aps, HOME_SCAN_MAX);
    if (n < 0) {
        return send_json(req, "{\"message\":\"The board is busy connecting; type the network name instead.\"}");
    }
    char json[HOME_SCAN_MAX * 80 + 32] = "{\"networks\":[";
    for (int i = 0; i < n; i++) {
        if (i) strlcat(json, ",", sizeof(json));
        strlcat(json, "{\"ssid\":", sizeof(json));
        setup_json_str(json, sizeof(json), aps[i].ssid);
        char tail[48];
        snprintf(tail, sizeof(tail), ",\"rssi\":%d,\"secure\":%s}", aps[i].rssi, aps[i].secure ? "true" : "false");
        strlcat(json, tail, sizeof(json));
    }
    strlcat(json, "]}", sizeof(json));
    return send_json(req, json);
}

static esp_err_t h_wifi(httpd_req_t *req) {
    if (!token_ok(req)) return forbid(req);
    char body[256], ssid[HOME_SSID_MAX + 1], pass[HOME_PASS_MAX + 1];
    if (!read_body(req, body, sizeof(body)) || !setup_form_value(body, "ssid", ssid, sizeof(ssid))) {
        wipe(body, sizeof(body));
        return send_result(req, false, "Enter a network name (up to 32 bytes)");
    }
    if (!setup_form_value(body, "password", pass, sizeof(pass))) pass[0] = '\0';
    wipe(body, sizeof(body));
    const char *err = setup_wifi_check(ssid, pass);
    esp_err_t r;
    if (err) {
        r = send_result(req, false, err);
    } else if (home_store_save_wifi(ssid, pass) != ESP_OK) {
        r = send_result(req, false, "Could not save the setting on the board");
    } else {
        ESP_LOGI(TAG, "Wi-Fi \"%s\" saved from the setup page", ssid);
        set_event("Wi-Fi saved. Connecting...");
        // Answer first: joining the new network can move the hotspot to the
        // router's channel, which may drop the phone for a moment.
        r = send_result(req, true, "Saved. The board is connecting; watch its screen.");
        home_net_wifi_changed();
    }
    wipe(pass, sizeof(pass));
    return r;
}

static esp_err_t h_spot_start(httpd_req_t *req) {
    if (!token_ok(req)) return forbid(req);
    char body[128], cid[SPOT_CLIENT_ID_LEN + 2];
    if (!read_body(req, body, sizeof(body)) || !setup_form_value(body, "client_id", cid, sizeof(cid)) ||
        !spot_valid_client_id(cid)) {
        return send_result(req, false, "The client ID must be the 32 hex digits from the dashboard");
    }
    wipe(&s_login, sizeof(s_login));
    memcpy(s_login.client_id, cid, sizeof(s_login.client_id));
    random_text(s_login.verifier, SETUP_VERIFIER_LEN);
    random_text(s_login.state, SETUP_STATE_LEN);
    s_login.active = true;

    char challenge[44];
    setup_pkce_challenge(s_login.verifier, challenge);
    char url[512];
    if (!setup_authorize_url(url, sizeof(url), s_login.client_id, challenge, s_login.state)) {
        return send_result(req, false, "Internal error building the login address");
    }
    set_event("Spotify login started");
    char json[640] = "{\"ok\":true,\"url\":";
    setup_json_str(json, sizeof(json), url);
    strlcat(json, "}", sizeof(json));
    return send_json(req, json);
}

static esp_err_t h_spot_finish(httpd_req_t *req) {
    if (!token_ok(req)) return forbid(req);
    static char body[BODY_MAX], pasted[BODY_MAX];
    if (!read_body(req, body, sizeof(body)) || !setup_form_value(body, "pasted", pasted, sizeof(pasted))) {
        return send_result(req, false, "Paste the whole address of the page that could not load");
    }
    char code[512], error[48];
    setup_paste_t p = setup_parse_paste(pasted, s_login.active ? s_login.state : NULL, code, sizeof(code),
                                        error, sizeof(error));
    wipe(body, sizeof(body));
    wipe(pasted, sizeof(pasted));
    switch (p) {
    case SETUP_PASTE_DENIED:
        wipe(&s_login, sizeof(s_login));
        return send_result(req, false, "Spotify login was cancelled or refused. Press Log in to try again");
    case SETUP_PASTE_STATE:
        return send_result(req, false, "That address is from an older login. Use the newest login tab");
    case SETUP_PASTE_NO_LOGIN:
        return send_result(req, false, "Press Log in with Spotify first");
    case SETUP_PASTE_EMPTY:
        return send_result(req, false, "No login code found. Copy the whole address, starting with http://127.0.0.1");
    case SETUP_PASTE_OK:
    default:
        break;
    }
    const char *err = spot_link_code(s_login.client_id, code, s_login.verifier, LOGIN_WAIT_MS);
    wipe(code, sizeof(code));
    if (err) return send_result(req, false, err);
    wipe(&s_login, sizeof(s_login));
    set_event("Spotify linked");
    return send_result(req, true, "Spotify is linked. On the board, open Apps (UP) and choose Spotify.");
}

static esp_err_t h_spot_clear(httpd_req_t *req) {
    if (!token_ok(req)) return forbid(req);
    if (home_store_clear_spotify() != ESP_OK) return send_result(req, false, "Could not clear the login");
    spot_link_changed();
    set_event("Spotify login removed");
    return send_result(req, true, "Spotify login removed from the board");
}

// ---------------------------------------------------------------------------
// Start / stop
// ---------------------------------------------------------------------------

static char s_ap_ssid[20];
static char s_ap_password[11];

esp_err_t setup_portal_start(setup_info_t *info) {
    if (s_server) {
        memcpy(info->ap_ssid, s_ap_ssid, sizeof(s_ap_ssid));
        memcpy(info->ap_password, s_ap_password, sizeof(s_ap_password));
        return ESP_OK;
    }
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    uint8_t r[10];
    esp_fill_random(r, sizeof(r));
    setup_ap_ssid(mac, s_ap_ssid);
    setup_ap_password(r, s_ap_password);  // new password every time Setup opens
    wipe(r, sizeof(r));
    memcpy(info->ap_ssid, s_ap_ssid, sizeof(s_ap_ssid));
    memcpy(info->ap_password, s_ap_password, sizeof(s_ap_password));

    random_text(s_token, TOKEN_LEN);
    wipe(&s_login, sizeof(s_login));
    s_seen_phone = false;
    s_event = NULL;
    home_net_set_setup_ap(true, info->ap_ssid, info->ap_password);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;          // scan list + JSON + waiting for Spotify
    cfg.max_uri_handlers = 10;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web server failed to start: %s", esp_err_to_name(err));
        s_server = NULL;
        home_net_set_setup_ap(false, NULL, NULL);
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_page },
        { .uri = "/api/session", .method = HTTP_GET, .handler = h_session },
        { .uri = "/api/status", .method = HTTP_GET, .handler = h_status },
        { .uri = "/api/scan", .method = HTTP_GET, .handler = h_scan },
        { .uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi },
        { .uri = "/api/spotify/start", .method = HTTP_POST, .handler = h_spot_start },
        { .uri = "/api/spotify/finish", .method = HTTP_POST, .handler = h_spot_finish },
        { .uri = "/api/spotify/clear", .method = HTTP_POST, .handler = h_spot_clear },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_server, &uris[i]);
    ESP_LOGI(TAG, "setup page on http://%s/ via hotspot %s", SETUP_AP_IP, info->ap_ssid);
    return ESP_OK;
}

void setup_portal_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    home_net_set_setup_ap(false, NULL, NULL);
    wipe(&s_login, sizeof(s_login));
    wipe(s_token, sizeof(s_token));
    ESP_LOGI(TAG, "setup closed");
}
