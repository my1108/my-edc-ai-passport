// Host tests for main/setup_logic.c.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "setup_logic.h"

static void hex(const uint8_t *d, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static void test_sha256(void) {
    uint8_t d[32];
    char h[65];
    setup_sha256("", 0, d);
    hex(d, 32, h);
    assert(strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    setup_sha256("abc", 3, d);
    hex(d, 32, h);
    assert(strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";  // 56 bytes: two blocks
    setup_sha256(two, strlen(two), d);
    hex(d, 32, h);
    assert(strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
    char million[1000];
    memset(million, 'a', sizeof(million));
    setup_sha256(million, 64, d);  // exactly one block of data
    hex(d, 32, h);
    assert(strcmp(h, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb") == 0);
}

static void test_pkce(void) {
    // RFC 7636 appendix B.
    char c[44];
    setup_pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", c);
    assert(strcmp(c, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM") == 0);

    char b[16];
    const uint8_t foo[] = { 'f', 'o', 'o', 'b', 'a', 'r' };
    assert(setup_base64url(foo, 1, b, sizeof(b)) == 2 && strcmp(b, "Zg") == 0);
    assert(setup_base64url(foo, 2, b, sizeof(b)) == 3 && strcmp(b, "Zm8") == 0);
    assert(setup_base64url(foo, 6, b, sizeof(b)) == 8 && strcmp(b, "Zm9vYmFy") == 0);
    const uint8_t ff[] = { 0xfb, 0xff };
    assert(setup_base64url(ff, 2, b, sizeof(b)) == 3 && strcmp(b, "-_8") == 0);
    assert(setup_base64url(foo, 6, b, 8) == 0);  // no room for the NUL

    uint8_t r[SETUP_VERIFIER_LEN];
    for (int i = 0; i < SETUP_VERIFIER_LEN; i++) r[i] = (uint8_t)(i * 37);
    char v[SETUP_VERIFIER_LEN + 1];
    setup_random_text(r, SETUP_VERIFIER_LEN, v);
    assert(strlen(v) == SETUP_VERIFIER_LEN);
    for (int i = 0; v[i]; i++) assert(strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", v[i]));
}

static void test_url(void) {
    char out[128];
    assert(setup_url_encode("a b&c=d/é~", out, sizeof(out)));
    assert(strcmp(out, "a%20b%26c%3Dd%2F%C3%A9~") == 0);
    assert(!setup_url_encode("abcdef", out, 4) && strcmp(out, "abc") == 0);

    assert(setup_url_decode("a%20b+c%2", 9, out, sizeof(out), true) && strcmp(out, "a b c%2") == 0);
    assert(setup_url_decode("a+b", 3, out, sizeof(out), false) && strcmp(out, "a+b") == 0);
    assert(setup_url_decode("%zz%41", 6, out, sizeof(out), true) && strcmp(out, "%zzA") == 0);
    assert(!setup_url_decode("a%00b", 5, out, sizeof(out), true));  // NUL is refused
    assert(!setup_url_decode("abcdef", 6, out, 4, true));

    assert(setup_form_value("ssid=My+Home%21&password=p%26ss%3D1", "ssid", out, sizeof(out)));
    assert(strcmp(out, "My Home!") == 0);
    assert(setup_form_value("ssid=My+Home%21&password=p%26ss%3D1", "password", out, sizeof(out)));
    assert(strcmp(out, "p&ss=1") == 0);
    assert(setup_form_value("a=1&password=&b=2", "password", out, sizeof(out)) && out[0] == '\0');
    assert(!setup_form_value("xssid=1", "ssid", out, sizeof(out)));
    assert(!setup_form_value("ssid", "ssid", out, sizeof(out)));
    assert(setup_form_value("http://127.0.0.1:8888/callback?code=abc&state=x#frag", "state", out, sizeof(out)));
    assert(strcmp(out, "x") == 0);

    char url[512];
    assert(setup_authorize_url(url, sizeof(url), "0123456789abcdef0123456789abcdef", "CHAL", "STATE"));
    assert(strstr(url, "https://accounts.spotify.com/authorize?client_id=0123456789abcdef0123456789abcdef&") == url);
    assert(strstr(url, "&redirect_uri=http%3A%2F%2F127.0.0.1%3A8888%2Fcallback&"));
    assert(strstr(url, "&code_challenge_method=S256&code_challenge=CHAL&"));
    assert(strstr(url, "&scope=user-read-playback-state%20user-modify-playback-state%20user-read-currently-playing&"));
    assert(strstr(url, "&state=STATE") && url[strlen(url) - 1] == 'E');
    assert(!setup_authorize_url(url, 50, "0123456789abcdef0123456789abcdef", "CHAL", "STATE"));

    char body[512];
    assert(setup_code_body(body, sizeof(body), "cid", "AQ-code_1", "verifier"));
    assert(strcmp(body, "grant_type=authorization_code&code=AQ-code_1&redirect_uri=http%3A%2F%2F127.0.0.1%3A8888"
                        "%2Fcallback&client_id=cid&code_verifier=verifier") == 0);
}

static void test_paste(void) {
    char code[256], err[32];
    const char *code_value = "AQBabcdefghijklmnopqrstuvwxyz-_0123456789";
    char url[256];
    snprintf(url, sizeof(url), "  http://127.0.0.1:8888/callback?code=%s&state=s123\n", code_value);
    assert(setup_parse_paste(url, "s123", code, sizeof(code), err, sizeof(err)) == SETUP_PASTE_OK);
    assert(strcmp(code, code_value) == 0);

    // Only the query part, or only the code.
    snprintf(url, sizeof(url), "code=%s&state=s123", code_value);
    assert(setup_parse_paste(url, "s123", code, sizeof(code), err, sizeof(err)) == SETUP_PASTE_OK);
    assert(setup_parse_paste(code_value, "s123", code, sizeof(code), err, sizeof(err)) == SETUP_PASTE_OK);
    assert(strcmp(code, code_value) == 0);

    // Wrong state, denied, nothing pending, junk.
    snprintf(url, sizeof(url), "http://127.0.0.1:8888/callback?code=%s&state=other", code_value);
    assert(setup_parse_paste(url, "s123", code, sizeof(code), err, sizeof(err)) == SETUP_PASTE_STATE);
    assert(code[0] == '\0');
    assert(setup_parse_paste("http://127.0.0.1:8888/callback?error=access_denied&state=s123", "s123", code,
                             sizeof(code), err, sizeof(err)) == SETUP_PASTE_DENIED);
    assert(strcmp(err, "access_denied") == 0);
    snprintf(url, sizeof(url), "code=%s&state=s123", code_value);
    assert(setup_parse_paste(url, "", code, sizeof(code), err, sizeof(err)) == SETUP_PASTE_NO_LOGIN);
    assert(setup_parse_paste(url, NULL, code, sizeof(code), err, sizeof(err)) == SETUP_PASTE_NO_LOGIN);
    assert(setup_parse_paste("   ", "s123", code, sizeof(code), err, sizeof(err)) == SETUP_PASTE_EMPTY);
    assert(setup_parse_paste("hello", "s123", code, sizeof(code), err, sizeof(err)) == SETUP_PASTE_EMPTY);
    assert(setup_parse_paste("http://127.0.0.1:8888/callback?state=s123", "s123", code, sizeof(code), err,
                             sizeof(err)) == SETUP_PASTE_EMPTY);
    assert(setup_parse_paste("code=bad code with spaces here&state=s123", "s123", code, sizeof(code), err,
                             sizeof(err)) == SETUP_PASTE_EMPTY);
    assert(setup_parse_paste(NULL, "s123", code, sizeof(code), err, sizeof(err)) == SETUP_PASTE_EMPTY);
}

static void test_json_and_qr(void) {
    char out[64] = "";
    assert(setup_json_str(out, sizeof(out), "a\"b\\c\n"));
    assert(strcmp(out, "\"a\\\"b\\\\c\\u000a\"") == 0);
    char small[6] = "";
    assert(!setup_json_str(small, sizeof(small), "abcdef"));
    char ok[8] = "x:";
    assert(setup_json_str(ok, sizeof(ok), "ab") && strcmp(ok, "x:\"ab\"") == 0);

    char qr[96];
    assert(setup_wifi_qr(qr, sizeof(qr), "AI-Passport-7DE4", "abc23"));
    assert(strcmp(qr, "WIFI:T:WPA;S:AI-Passport-7DE4;P:abc23;;") == 0);
    assert(setup_wifi_qr(qr, sizeof(qr), "My;Net", "p:a\\s,s\""));
    assert(strcmp(qr, "WIFI:T:WPA;S:My\\;Net;P:p\\:a\\\\s\\,s\\\";;") == 0);
    assert(setup_wifi_qr(qr, sizeof(qr), "Open", ""));
    assert(strcmp(qr, "WIFI:T:nopass;S:Open;;") == 0);
    assert(!setup_wifi_qr(qr, 10, "AI-Passport-7DE4", "abc"));

    const uint8_t mac[6] = { 0x4c, 0x11, 0xae, 0x2f, 0x7d, 0xe4 };
    char ssid[20];
    setup_ap_ssid(mac, ssid);
    assert(strcmp(ssid, "AI-Passport-7DE4") == 0);
    const uint8_t r[10] = { 0, 1, 2, 30, 31, 62, 200, 255, 9, 10 };
    char pw[11];
    setup_ap_password(r, pw);
    assert(strlen(pw) == 10);
    for (int i = 0; pw[i]; i++) assert(!strchr("01lIoO", pw[i]));
}

static void test_cards(void) {
    setup_info_t info;
    memset(&info, 0, sizeof(info));
    strcpy(info.ap_ssid, "AI-Passport-7DE4");
    strcpy(info.ap_password, "abcdefghjk");
    assert(setup_card_count(&info) == 2 && setup_first_card(&info) == SETUP_CARD_JOIN);

    setup_view_t v;
    setup_view(&info, 0, &v);
    assert(v.card == SETUP_CARD_JOIN && strcmp(v.qr, "WIFI:T:WPA;S:AI-Passport-7DE4;P:abcdefghjk;;") == 0);
    assert(strcmp(v.line2, "Password: abcdefghjk") == 0);
    setup_view(&info, 1, &v);
    assert(v.card == SETUP_CARD_AP_PAGE && strcmp(v.qr, "http://192.168.4.1/") == 0);
    setup_view(&info, 2, &v);  // wraps
    assert(v.card == SETUP_CARD_JOIN);
    setup_view(&info, -1, &v);
    assert(v.card == SETUP_CARD_AP_PAGE);

    info.sta_connected = true;
    assert(setup_card_count(&info) == 2);  // connected but no IP yet
    strcpy(info.sta_ip, "192.168.1.23");
    assert(setup_card_count(&info) == 3 && setup_first_card(&info) == SETUP_CARD_LAN_PAGE);
    setup_view(&info, 2, &v);
    assert(v.card == SETUP_CARD_LAN_PAGE && strcmp(v.qr, "http://192.168.1.23/") == 0);
    assert(strcmp(v.line2, "http://192.168.1.23") == 0);
}

int main(void) {
    test_sha256();
    test_pkce();
    test_url();
    test_paste();
    test_json_and_qr();
    test_cards();
    printf("setup logic tests: PASS\n");
    return 0;
}
