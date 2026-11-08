// main/setup_logic.h - pure logic for the phone setup page (Wi-Fi + Spotify).
//
// SHA-256 and base64url for PKCE, URL and form encoding, the pasted Spotify
// redirect, JSON string escaping, the Wi-Fi QR payload, setup-hotspot names,
// and the cards the Setup screen cycles through. No ESP-IDF, FreeRTOS, or
// LVGL, so all of it is covered by host tests (tests/test_setup_logic.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// SHA-256 and base64url (PKCE, RFC 7636)
// ---------------------------------------------------------------------------

void setup_sha256(const void *data, size_t len, uint8_t out[32]);

// Unpadded base64url. Returns the length written, or 0 when `out` is too small.
size_t setup_base64url(const uint8_t *data, size_t len, char *out, size_t out_size);

#define SETUP_VERIFIER_LEN 64     // RFC 7636 allows 43..128
#define SETUP_STATE_LEN    22

// Fills `out` (len + 1 bytes) with `len` unreserved characters derived from
// `random` (one byte per character).
void setup_random_text(const uint8_t *random, size_t len, char *out);

// S256 code challenge for `verifier`: base64url(SHA-256(verifier)), 43 characters.
void setup_pkce_challenge(const char *verifier, char out[44]);

// ---------------------------------------------------------------------------
// URLs and forms
// ---------------------------------------------------------------------------

#define SETUP_REDIRECT_URI "http://127.0.0.1:8888/callback"
#define SETUP_SPOTIFY_SCOPES "user-read-playback-state user-modify-playback-state user-read-currently-playing"

// Percent-encodes everything except unreserved characters. Returns false
// (with a truncated, terminated `out`) when it does not fit.
bool setup_url_encode(const char *in, char *out, size_t out_size);

// Decodes %XX and, when `plus_is_space`, '+' (form encoding). Invalid escapes
// are copied as-is. Returns false when it does not fit or decodes a NUL.
bool setup_url_decode(const char *in, size_t in_len, char *out, size_t out_size, bool plus_is_space);

// Finds `key` in an x-www-form-urlencoded string (or a URL's query, given the
// whole URL: everything up to '?' is skipped) and decodes its value. Returns
// false when absent or too long.
bool setup_form_value(const char *form, const char *key, char *out, size_t out_size);

// Spotify authorize URL for the PKCE flow with SETUP_REDIRECT_URI.
bool setup_authorize_url(char *out, size_t size, const char *client_id, const char *challenge,
                         const char *state);

// Token request body for the authorization_code grant.
bool setup_code_body(char *out, size_t size, const char *client_id, const char *code,
                     const char *verifier);

typedef enum {
    SETUP_PASTE_OK = 0,
    SETUP_PASTE_EMPTY,          // nothing usable was pasted
    SETUP_PASTE_DENIED,         // Spotify returned error=... (e.g. access_denied)
    SETUP_PASTE_STATE,          // the state does not match this login attempt
    SETUP_PASTE_NO_LOGIN,       // no login was started on this page
} setup_paste_t;

// Reads the address the browser was sent to after the Spotify login (the
// "can't connect" page at 127.0.0.1). Accepts the whole URL or just its query.
// `expected_state` NULL or "" means no login is pending.
setup_paste_t setup_parse_paste(const char *pasted, const char *expected_state, char *code, size_t code_size,
                                char *error, size_t error_size);

// ---------------------------------------------------------------------------
// JSON and QR text
// ---------------------------------------------------------------------------

// Appends `s` as a JSON string literal (with quotes). Returns false when full.
bool setup_json_str(char *out, size_t size, const char *s);

// "WIFI:T:WPA;S:<ssid>;P:<pass>;;" with \ ; , : " escaped (ZXing format).
bool setup_wifi_qr(char *out, size_t size, const char *ssid, const char *password);

// Checks Wi-Fi credentials from the setup page: SSID 1..32 bytes; password
// empty (open network), 8..63 characters, or 64 hex digits. Returns NULL when
// valid, else a message.
const char *setup_wifi_check(const char *ssid, const char *password);

// "AI-Passport-7DE4" from the last two bytes of the MAC.
void setup_ap_ssid(const uint8_t mac[6], char out[20]);

// 10-character hotspot password from 10 random bytes, using letters and
// digits that are easy to read (no 0/O, 1/l/I).
void setup_ap_password(const uint8_t random[10], char out[11]);

// ---------------------------------------------------------------------------
// Setup screen cards
// ---------------------------------------------------------------------------

typedef enum {
    SETUP_CARD_JOIN = 0,        // QR: join the setup hotspot
    SETUP_CARD_AP_PAGE,         // QR: http://192.168.4.1/
    SETUP_CARD_LAN_PAGE,        // QR: http://<home IP>/ (only when connected)
} setup_card_t;

typedef struct {
    bool sta_connected;
    char ap_ssid[20];
    char ap_password[11];
    char sta_ip[16];
} setup_info_t;

typedef struct {
    setup_card_t card;
    char qr[96];
    const char *heading;        // e.g. "1  Join setup Wi-Fi"
    char line1[48];
    char line2[48];
} setup_view_t;

#define SETUP_AP_IP "192.168.4.1"

// Number of cards for this state (2, or 3 when connected to home Wi-Fi).
int setup_card_count(const setup_info_t *info);

// The first card to show: the home-Wi-Fi page when connected, else "join".
int setup_first_card(const setup_info_t *info);

// Fills the view for card `index` (wrapped into range).
void setup_view(const setup_info_t *info, int index, setup_view_t *out);
