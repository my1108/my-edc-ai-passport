// main/setup_logic.c - see setup_logic.h.
#include "setup_logic.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void setup_sha256(const void *data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const uint8_t *p = data;
    size_t left = len;
    while (left >= 64) {
        sha256_block(h, p);
        p += 64;
        left -= 64;
    }
    uint8_t tail[128];
    memset(tail, 0, sizeof(tail));
    memcpy(tail, p, left);
    tail[left] = 0x80;
    size_t tail_len = left + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[tail_len - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha256_block(h, tail);
    if (tail_len == 128) sha256_block(h, tail + 64);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

// ---------------------------------------------------------------------------
// base64url and random text
// ---------------------------------------------------------------------------

static const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t setup_base64url(const uint8_t *data, size_t len, char *out, size_t out_size) {
    size_t need = (len * 4 + 2) / 3;
    if (need + 1 > out_size) return 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out[o++] = B64URL[(v >> 18) & 63];
        out[o++] = B64URL[(v >> 12) & 63];
        if (i + 1 < len) out[o++] = B64URL[(v >> 6) & 63];
        if (i + 2 < len) out[o++] = B64URL[v & 63];
    }
    out[o] = '\0';
    return o;
}

void setup_random_text(const uint8_t *random, size_t len, char *out) {
    for (size_t i = 0; i < len; i++) out[i] = B64URL[random[i] & 63];
    out[len] = '\0';
}

void setup_pkce_challenge(const char *verifier, char out[44]) {
    uint8_t digest[32];
    setup_sha256(verifier, strlen(verifier), digest);
    setup_base64url(digest, sizeof(digest), out, 44);
}

// ---------------------------------------------------------------------------
// URLs and forms
// ---------------------------------------------------------------------------

static bool unreserved(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.' || c == '~';
}

bool setup_url_encode(const char *in, char *out, size_t out_size) {
    static const char HEX[] = "0123456789ABCDEF";
    if (out_size == 0) return false;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (unreserved((char)*p)) {
            if (o + 1 >= out_size) goto full;
            out[o++] = (char)*p;
        } else {
            if (o + 3 >= out_size) goto full;
            out[o++] = '%';
            out[o++] = HEX[*p >> 4];
            out[o++] = HEX[*p & 15];
        }
    }
    out[o] = '\0';
    return true;
full:
    out[o] = '\0';
    return false;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool setup_url_decode(const char *in, size_t in_len, char *out, size_t out_size, bool plus_is_space) {
    if (out_size == 0) return false;
    size_t o = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = in[i];
        if (c == '%') {
            int hi = i + 1 < in_len ? hexval(in[i + 1]) : -1;
            int lo = i + 2 < in_len ? hexval(in[i + 2]) : -1;
            if (hi >= 0 && lo >= 0) {
                c = (char)(hi << 4 | lo);
                if (c == '\0') {
                    out[o] = '\0';
                    return false;
                }
                i += 2;
            }
        } else if (c == '+' && plus_is_space) {
            c = ' ';
        }
        if (o + 1 >= out_size) {
            out[o] = '\0';
            return false;
        }
        out[o++] = c;
    }
    out[o] = '\0';
    return true;
}

bool setup_form_value(const char *form, const char *key, char *out, size_t out_size) {
    if (out_size) out[0] = '\0';
    if (!form || !key) return false;
    const char *q = strchr(form, '?');
    const char *p = q ? q + 1 : form;
    const char *end = strchr(p, '#');
    if (!end) end = p + strlen(p);
    size_t key_len = strlen(key);
    while (p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *pair_end = amp ? amp : end;
        const char *eq = memchr(p, '=', (size_t)(pair_end - p));
        if (eq && (size_t)(eq - p) == key_len && memcmp(p, key, key_len) == 0) {
            return setup_url_decode(eq + 1, (size_t)(pair_end - eq - 1), out, out_size, true);
        }
        p = amp ? amp + 1 : end;
    }
    return false;
}

bool setup_authorize_url(char *out, size_t size, const char *client_id, const char *challenge,
                         const char *state) {
    char redirect[64], scopes[128];
    if (!setup_url_encode(SETUP_REDIRECT_URI, redirect, sizeof(redirect)) ||
        !setup_url_encode(SETUP_SPOTIFY_SCOPES, scopes, sizeof(scopes))) {
        return false;
    }
    int n = snprintf(out, size,
                     "https://accounts.spotify.com/authorize?client_id=%s&response_type=code"
                     "&redirect_uri=%s&code_challenge_method=S256&code_challenge=%s&scope=%s&state=%s",
                     client_id, redirect, challenge, scopes, state);
    return n > 0 && (size_t)n < size;
}

bool setup_code_body(char *out, size_t size, const char *client_id, const char *code,
                     const char *verifier) {
    char code_enc[512], redirect[64];
    if (!setup_url_encode(code, code_enc, sizeof(code_enc)) ||
        !setup_url_encode(SETUP_REDIRECT_URI, redirect, sizeof(redirect))) {
        return false;
    }
    int n = snprintf(out, size,
                     "grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s&code_verifier=%s",
                     code_enc, redirect, client_id, verifier);
    return n > 0 && (size_t)n < size;
}

static bool looks_like_code(const char *s) {
    size_t len = strlen(s);
    if (len < 20) return false;
    for (; *s; s++) {
        if (!unreserved(*s)) return false;
    }
    return true;
}

setup_paste_t setup_parse_paste(const char *pasted, const char *expected_state, char *code, size_t code_size,
                                char *error, size_t error_size) {
    if (code_size) code[0] = '\0';
    if (error_size) error[0] = '\0';
    if (!pasted) return SETUP_PASTE_EMPTY;

    // Trim surrounding whitespace (copy-paste often adds a newline).
    char buf[1024];
    while (*pasted == ' ' || *pasted == '\t' || *pasted == '\r' || *pasted == '\n') pasted++;
    size_t len = strlen(pasted);
    while (len > 0 && (pasted[len - 1] == ' ' || pasted[len - 1] == '\t' || pasted[len - 1] == '\r' ||
                       pasted[len - 1] == '\n')) {
        len--;
    }
    if (len == 0 || len >= sizeof(buf)) return SETUP_PASTE_EMPTY;
    memcpy(buf, pasted, len);
    buf[len] = '\0';

    bool pending = expected_state && expected_state[0];
    if (!strchr(buf, '=')) {
        // Just the code itself.
        if (!looks_like_code(buf) || len >= code_size) return SETUP_PASTE_EMPTY;
        if (!pending) return SETUP_PASTE_NO_LOGIN;
        memcpy(code, buf, len + 1);
        return SETUP_PASTE_OK;
    }

    char state[64];
    if (setup_form_value(buf, "error", error, error_size) && error[0]) return SETUP_PASTE_DENIED;
    if (!pending) return SETUP_PASTE_NO_LOGIN;
    if (!setup_form_value(buf, "state", state, sizeof(state)) || strcmp(state, expected_state) != 0) {
        return SETUP_PASTE_STATE;
    }
    if (!setup_form_value(buf, "code", code, code_size) || !looks_like_code(code)) {
        if (code_size) code[0] = '\0';
        return SETUP_PASTE_EMPTY;
    }
    return SETUP_PASTE_OK;
}

// ---------------------------------------------------------------------------
// JSON and QR text
// ---------------------------------------------------------------------------

static bool put(char *out, size_t size, size_t *len, const char *s, size_t n) {
    if (*len + n + 1 > size) return false;
    memcpy(out + *len, s, n);
    *len += n;
    out[*len] = '\0';
    return true;
}

bool setup_json_str(char *out, size_t size, const char *s) {
    size_t len = strlen(out);
    if (!put(out, size, &len, "\"", 1)) return false;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        char esc[8];
        if (*p == '"' || *p == '\\') {
            esc[0] = '\\';
            esc[1] = (char)*p;
            if (!put(out, size, &len, esc, 2)) return false;
        } else if (*p < 0x20) {
            snprintf(esc, sizeof(esc), "\\u%04x", *p);
            if (!put(out, size, &len, esc, 6)) return false;
        } else {
            if (!put(out, size, &len, (const char *)p, 1)) return false;
        }
    }
    return put(out, size, &len, "\"", 1);
}

static bool put_qr_escaped(char *out, size_t size, size_t *len, const char *s) {
    for (; *s; s++) {
        if (strchr("\\;,:\"", *s)) {
            if (!put(out, size, len, "\\", 1)) return false;
        }
        if (!put(out, size, len, s, 1)) return false;
    }
    return true;
}

bool setup_wifi_qr(char *out, size_t size, const char *ssid, const char *password) {
    if (size == 0) return false;
    out[0] = '\0';
    size_t len = 0;
    bool open = !password || !password[0];
    return put(out, size, &len, open ? "WIFI:T:nopass;S:" : "WIFI:T:WPA;S:", open ? 16 : 13) &&
           put_qr_escaped(out, size, &len, ssid) &&
           (open || (put(out, size, &len, ";P:", 3) && put_qr_escaped(out, size, &len, password))) &&
           put(out, size, &len, ";;", 2);
}

const char *setup_wifi_check(const char *ssid, const char *password) {
    size_t ssid_len = ssid ? strlen(ssid) : 0;
    if (ssid_len == 0 || ssid_len > 32) return "The network name must be 1 to 32 bytes";
    size_t pass_len = password ? strlen(password) : 0;
    if (pass_len == 0 || (pass_len >= 8 && pass_len <= 63)) return NULL;
    if (pass_len == 64) {
        for (size_t i = 0; i < 64; i++) {
            char c = password[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                return "A 64-character password must be hex digits";
            }
        }
        return NULL;
    }
    return "The password must be 8 to 63 characters, or empty for an open network";
}

void setup_ap_ssid(const uint8_t mac[6], char out[20]) {
    snprintf(out, 20, "AI-Passport-%02X%02X", mac[4], mac[5]);
}

void setup_ap_password(const uint8_t random[10], char out[11]) {
    static const char ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";  // 31 easy-to-read characters
    for (int i = 0; i < 10; i++) out[i] = ALPHABET[random[i] % (sizeof(ALPHABET) - 1)];
    out[10] = '\0';
}

// ---------------------------------------------------------------------------
// Cards
// ---------------------------------------------------------------------------

int setup_card_count(const setup_info_t *info) {
    return info->sta_connected && info->sta_ip[0] ? 3 : 2;
}

int setup_first_card(const setup_info_t *info) {
    return setup_card_count(info) == 3 ? SETUP_CARD_LAN_PAGE : SETUP_CARD_JOIN;
}

void setup_view(const setup_info_t *info, int index, setup_view_t *out) {
    memset(out, 0, sizeof(*out));
    int count = setup_card_count(info);
    index %= count;
    if (index < 0) index += count;
    out->card = (setup_card_t)index;
    switch (out->card) {
    case SETUP_CARD_JOIN:
        out->heading = "1  Join the setup Wi-Fi";
        setup_wifi_qr(out->qr, sizeof(out->qr), info->ap_ssid, info->ap_password);
        snprintf(out->line1, sizeof(out->line1), "Wi-Fi: %s", info->ap_ssid);
        snprintf(out->line2, sizeof(out->line2), "Password: %s", info->ap_password);
        break;
    case SETUP_CARD_AP_PAGE:
        out->heading = "2  Open the setup page";
        snprintf(out->qr, sizeof(out->qr), "http://" SETUP_AP_IP "/");
        snprintf(out->line1, sizeof(out->line1), "On the setup Wi-Fi, open");
        snprintf(out->line2, sizeof(out->line2), "http://" SETUP_AP_IP);
        break;
    case SETUP_CARD_LAN_PAGE:
    default:
        out->heading = "Setup page on home Wi-Fi";
        snprintf(out->qr, sizeof(out->qr), "http://%s/", info->sta_ip);
        snprintf(out->line1, sizeof(out->line1), "For Spotify login, open");
        snprintf(out->line2, sizeof(out->line2), "http://%s", info->sta_ip);
        break;
    }
}
