// Host tests for main/home_logic.c.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "home_layout.h"  // its static asserts check the screen rows on the host
#include "home_logic.h"

#define SEP HOME_SEP
#define DEG "\xC2\xB0"

static struct tm tm_of(int year, int mon, int mday, int wday, int yday, int h, int m, int s) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon = mon - 1;
    t.tm_mday = mday;
    t.tm_wday = wday;
    t.tm_yday = yday;
    t.tm_hour = h;
    t.tm_min = m;
    t.tm_sec = s;
    return t;
}

static void test_date_and_clock(void) {
    char buf[32], hm[8], sec[4], ampm[4];
    struct tm t = tm_of(2026, 11, 8, 0, 311, 14, 5, 9);
    home_fmt_date(buf, sizeof(buf), &t);
    assert(strcmp(buf, "SUN " SEP " 8 NOV") == 0);

    home_fmt_clock(&t, false, hm, sizeof(hm), sec, sizeof(sec), ampm, sizeof(ampm));
    assert(strcmp(hm, "14:05") == 0 && strcmp(sec, "09") == 0 && ampm[0] == '\0');
    home_fmt_clock(&t, true, hm, sizeof(hm), sec, sizeof(sec), ampm, sizeof(ampm));
    assert(strcmp(hm, "2:05") == 0 && strcmp(ampm, "PM") == 0);

    struct tm midnight = tm_of(2026, 11, 8, 0, 311, 0, 7, 0);
    home_fmt_clock(&midnight, true, hm, sizeof(hm), NULL, 0, ampm, sizeof(ampm));
    assert(strcmp(hm, "12:07") == 0 && strcmp(ampm, "AM") == 0);
    home_fmt_clock(&midnight, false, hm, sizeof(hm), NULL, 0, NULL, 0);
    assert(strcmp(hm, "00:07") == 0);
    struct tm noon = tm_of(2026, 11, 8, 0, 311, 12, 0, 0);
    home_fmt_hm(buf, sizeof(buf), &noon, true);
    assert(strcmp(buf, "12:00 PM") == 0);

    assert(!home_time_is_valid(0));
    assert(home_time_is_valid(1794100000LL));
}

static void test_sync_line(void) {
    char buf[48];
    home_sync_view_t v;
    memset(&v, 0, sizeof(v));
    v.wifi_configured = true;
    v.wifi_connected = true;

    v.state = HOME_SYNC_RUNNING;
    assert(!home_fmt_sync(buf, sizeof(buf), &v));
    assert(strcmp(buf, "Syncing time...") == 0);

    v.state = HOME_SYNC_FAILED;
    v.retry_in_s = 3600;
    assert(home_fmt_sync(buf, sizeof(buf), &v));
    assert(strcmp(buf, "Sync failed " SEP " retrying in 1 h") == 0);
    v.retry_in_s = 1790;
    assert(home_fmt_sync(buf, sizeof(buf), &v));
    assert(strcmp(buf, "Sync failed " SEP " retrying in 30 min") == 0);
    v.retry_in_s = 0;
    assert(home_fmt_sync(buf, sizeof(buf), &v));
    assert(strcmp(buf, "Sync failed " SEP " retrying soon") == 0);

    v.state = HOME_SYNC_OK;
    v.have_last_sync = true;
    v.last_sync = tm_of(2026, 11, 8, 0, 311, 3, 0, 4);
    v.now = tm_of(2026, 11, 8, 0, 311, 14, 5, 0);
    assert(!home_fmt_sync(buf, sizeof(buf), &v));
    assert(strcmp(buf, "Time synced today " SEP " 03:00") == 0);
    v.twelve_hour = true;
    home_fmt_sync(buf, sizeof(buf), &v);
    assert(strcmp(buf, "Time synced today " SEP " 3:00 AM") == 0);
    v.twelve_hour = false;
    v.now = tm_of(2026, 11, 9, 1, 312, 1, 0, 0);
    home_fmt_sync(buf, sizeof(buf), &v);
    assert(strcmp(buf, "Time synced 8 NOV " SEP " 03:00") == 0);

    memset(&v, 0, sizeof(v));
    home_fmt_sync(buf, sizeof(buf), &v);
    assert(strcmp(buf, "Wi-Fi not set " SEP " see serial") == 0);
    v.wifi_configured = true;
    home_fmt_sync(buf, sizeof(buf), &v);
    assert(strcmp(buf, "Offline " SEP " waiting for Wi-Fi") == 0);
    v.wifi_connected = true;
    home_fmt_sync(buf, sizeof(buf), &v);
    assert(strcmp(buf, "Waiting to sync...") == 0);
}

static void test_schedule(void) {
    // 14:05:00 -> next 03:00 is tomorrow: 12 h 55 min.
    assert(home_delay_until_hour(14 * 3600 + 5 * 60, 3) == 12 * 3600 + 55 * 60);
    // 01:00 -> 03:00 today.
    assert(home_delay_until_hour(3600, 3) == 2 * 3600);
    // Exactly 03:00 -> tomorrow, never zero (would loop).
    assert(home_delay_until_hour(3 * 3600, 3) == 86400);
    assert(home_delay_until_hour(3 * 3600 - 1, 3) == 1);
    assert(home_time_sync_delay(true, 3600, 3) == 7200);
    assert(home_time_sync_delay(false, 3600, 3) == 3600);
    assert(home_weather_delay(true, 30) == 1800);
    assert(home_weather_delay(false, 30) == 600);
    assert(home_weather_delay(false, 5) == 300);
    assert(home_weather_delay(true, 0) == 60);

    assert(home_weather_is_fresh(true, true, 1800, 30));
    assert(home_weather_is_fresh(true, true, 3600, 30));
    assert(!home_weather_is_fresh(true, true, 3601, 30));
    assert(!home_weather_is_fresh(false, true, 10, 30));
    assert(!home_weather_is_fresh(true, false, 10, 30));  // cached from flash only
}

static void test_weather(void) {
    assert(home_sky_from_wmo(0, true) == HOME_SKY_CLEAR);
    assert(home_sky_from_wmo(1, false) == HOME_SKY_CLEAR_NIGHT);
    assert(home_sky_from_wmo(2, true) == HOME_SKY_PARTLY);
    assert(home_sky_from_wmo(3, true) == HOME_SKY_CLOUDY);
    assert(home_sky_from_wmo(45, true) == HOME_SKY_FOG);
    assert(home_sky_from_wmo(61, true) == HOME_SKY_RAIN);
    assert(home_sky_from_wmo(81, true) == HOME_SKY_RAIN);
    assert(home_sky_from_wmo(73, true) == HOME_SKY_SNOW);
    assert(home_sky_from_wmo(95, true) == HOME_SKY_STORM);
    assert(home_sky_from_wmo(42, true) == HOME_SKY_UNKNOWN);
    assert(strcmp(home_condition_text(2), "Partly cloudy") == 0);
    assert(strcmp(home_condition_text(63), "Rain") == 0);
    assert(strcmp(home_condition_text(-1), "Unknown") == 0);

    const char *json =
        "{\"current\":{\"time\":\"2026-11-08T00:45\",\"temperature_2m\":25.4,"
        "\"weather_code\":2,\"is_day\":0},\"daily\":{\"time\":[\"2026-11-08\"],"
        "\"temperature_2m_max\":[31.4],\"temperature_2m_min\":[23.9]}}";
    home_weather_t w;
    assert(home_parse_weather(json, &w));
    assert(w.temp_c10 == 254 && w.high_c10 == 314 && w.low_c10 == 239);
    assert(w.wmo == 2 && !w.is_day);
    assert(home_round_c10(w.temp_c10) == 25);
    assert(home_round_c10(-15) == -2 && home_round_c10(-14) == -1 && home_round_c10(5) == 1);

    assert(!home_parse_weather("{\"current\":{\"temperature_2m\":25}}", &w));
    assert(!home_parse_weather("{\"error\":true,\"reason\":\"bad\"}", &w));
    const char *hot =
        "{\"current\":{\"temperature_2m\":999,\"weather_code\":0},"
        "\"daily\":{\"temperature_2m_max\":[1],\"temperature_2m_min\":[0]}}";
    assert(!home_parse_weather(hot, &w));

    char line[48];
    home_weather_t lw = { .temp_c10 = 254, .high_c10 = 314, .low_c10 = 239, .wmo = 2 };
    home_fmt_location_line(line, sizeof(line), "Petaling", true, &lw);
    assert(strcmp(line, "Petaling " SEP " H 31" DEG " L 24" DEG) == 0);
    home_fmt_location_line(line, sizeof(line), "", true, &lw);
    assert(strcmp(line, "H 31" DEG " L 24" DEG) == 0);
    home_fmt_location_line(line, sizeof(line), "", false, NULL);
    assert(strcmp(line, "Locating...") == 0);
}

static void test_location(void) {
    home_location_t loc;
    const char *ipwho = "{\n    \"success\": true,\n    \"city\": \"Petaling\",\n"
                        "    \"latitude\": 3.0833326,\n    \"longitude\": 101.5833333\n}";
    assert(home_parse_location(ipwho, &loc));
    assert(strcmp(loc.city, "Petaling") == 0);
    assert(loc.lat_e4 == 30833 && loc.lon_e4 == 1015833);

    const char *ipapi = "{\"status\":\"success\",\"city\":\"Kuala Lumpur\",\"lat\":3.1412,\"lon\":101.6865}";
    assert(home_parse_location(ipapi, &loc));
    assert(strcmp(loc.city, "Kuala Lumpur") == 0 && loc.lat_e4 == 31412);

    assert(!home_parse_location("{\"success\":false,\"message\":\"Reserved range\"}", &loc));
    assert(!home_parse_location("{\"status\":\"fail\",\"lat\":1,\"lon\":1}", &loc));
    assert(!home_parse_location("{\"success\":true,\"latitude\":91,\"longitude\":0}", &loc));

    char out[16];
    home_ascii_fold("Z\xC3\xBCrich", out, sizeof(out));
    assert(strcmp(out, "Zurich") == 0);
    home_ascii_fold("S\xC3\xA3o Paulo", out, sizeof(out));
    assert(strcmp(out, "Sao Paulo") == 0);
    home_ascii_fold("\xE5\x90\x89\xE9\x9A\x86\xE5\x9D\xA1", out, sizeof(out));  // CJK -> ???
    assert(strcmp(out, "???") == 0);
    home_ascii_fold("Long city name here", out, 8);
    assert(strcmp(out, "Long ci") == 0);
}

static void test_battery(void) {
    assert(home_battery_level(-1) == -1);
    assert(home_battery_level(0) == 0);
    assert(home_battery_level(12) == 0);
    assert(home_battery_level(13) == 1);
    assert(home_battery_level(50) == 2);
    assert(home_battery_level(70) == 3);
    assert(home_battery_level(100) == 4);
}

static void test_cache(void) {
    home_cache_t in;
    memset(&in, 0, sizeof(in));
    in.have_weather = true;
    in.have_location = true;
    in.weather = (home_weather_t){ .temp_c10 = -35, .high_c10 = 10, .low_c10 = -80, .wmo = 73, .is_day = true };
    in.location.lat_e4 = 31412;
    in.location.lon_e4 = -1015833;
    strcpy(in.location.city, "Kuala Lumpur");
    in.weather_epoch = 1794100000LL;

    home_cache_blob_t blob;
    home_cache_encode(&in, &blob);
    home_cache_t out;
    assert(home_cache_decode(&blob, &out));
    assert(out.have_weather && out.have_location);
    assert(out.weather.temp_c10 == -35 && out.weather.low_c10 == -80 && out.weather.wmo == 73);
    assert(out.weather.is_day);
    assert(out.location.lon_e4 == -1015833 && strcmp(out.location.city, "Kuala Lumpur") == 0);
    assert(out.weather_epoch == 1794100000LL);

    home_cache_blob_t bad = blob;
    bad.temp_c10++;
    assert(!home_cache_decode(&bad, &out) && !out.have_weather);
    bad = blob;
    bad.version++;
    assert(!home_cache_decode(&bad, &out));
    memset(&bad, 0xFF, sizeof(bad));  // erased flash
    assert(!home_cache_decode(&bad, &out));
    assert(home_crc32("123456789", 9) == 0xCBF43926u);
}

static void test_navigation(void) {
    home_nav_result_t r = home_nav_handle(HOME_PAGE_HOME, HOME_KEY_UP, HOME_KEY_EV_CLICK);
    assert(r.page == HOME_PAGE_APPS && r.page_changed && !r.sync_now);
    r = home_nav_handle(HOME_PAGE_HOME, HOME_KEY_OK, HOME_KEY_EV_CLICK);
    assert(r.page == HOME_PAGE_FOCUS && r.page_changed);
    r = home_nav_handle(HOME_PAGE_HOME, HOME_KEY_DOWN, HOME_KEY_EV_CLICK);
    assert(r.page == HOME_PAGE_HOME && !r.page_changed && r.sync_now);
    r = home_nav_handle(HOME_PAGE_HOME, HOME_KEY_DOWN, HOME_KEY_EV_LONG);
    assert(!r.sync_now && !r.page_changed);
    r = home_nav_handle(HOME_PAGE_HOME, HOME_KEY_OK, HOME_KEY_EV_OTHER);
    assert(!r.page_changed);

    r = home_nav_handle(HOME_PAGE_APPS, HOME_KEY_OK, HOME_KEY_EV_CLICK);
    assert(r.page == HOME_PAGE_HOME && r.page_changed);
    r = home_nav_handle(HOME_PAGE_FOCUS, HOME_KEY_OK, HOME_KEY_EV_LONG);
    assert(r.page == HOME_PAGE_HOME);
    r = home_nav_handle(HOME_PAGE_FOCUS, HOME_KEY_DOWN, HOME_KEY_EV_CLICK);
    assert(r.page == HOME_PAGE_FOCUS && !r.page_changed && !r.sync_now);
    r = home_nav_handle(HOME_PAGE_APPS, HOME_KEY_UP, HOME_KEY_EV_CLICK);
    assert(r.page == HOME_PAGE_APPS && !r.page_changed);
}

static void test_commands(void) {
    home_cmd_t c;
    home_cmd_parse("", &c);
    assert(c.type == HOME_CMD_NONE);
    home_cmd_parse("  HELP ", &c);
    assert(c.type == HOME_CMD_HELP);
    home_cmd_parse("status", &c);
    assert(c.type == HOME_CMD_STATUS);
    home_cmd_parse("sync", &c);
    assert(c.type == HOME_CMD_SYNC);

    home_cmd_parse("wifi MyNet hunter2pass", &c);
    assert(c.type == HOME_CMD_WIFI_SET);
    assert(strcmp(c.ssid, "MyNet") == 0 && strcmp(c.password, "hunter2pass") == 0);
    home_cmd_parse("wifi \"Home Wi-Fi 2.4\" \"pa ss \\\"q\\\" \\\\x\"", &c);
    assert(c.type == HOME_CMD_WIFI_SET);
    assert(strcmp(c.ssid, "Home Wi-Fi 2.4") == 0);
    assert(strcmp(c.password, "pa ss \"q\" \\x") == 0);
    home_cmd_parse("wifi OpenCafe", &c);
    assert(c.type == HOME_CMD_WIFI_SET && c.password[0] == '\0');
    home_cmd_parse("wifi clear", &c);
    assert(c.type == HOME_CMD_WIFI_CLEAR);
    home_cmd_parse("wifi Net short", &c);
    assert(c.type == HOME_CMD_ERROR && c.error);
    home_cmd_parse("wifi Home Wi-Fi password1", &c);  // unquoted spaces
    assert(c.type == HOME_CMD_ERROR);
    home_cmd_parse("wifi \"unterminated", &c);
    assert(c.type == HOME_CMD_ERROR);
    home_cmd_parse("wifi 123456789012345678901234567890123 password1", &c);
    assert(c.type == HOME_CMD_ERROR);
    home_cmd_parse("wifi Net 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", &c);
    assert(c.type == HOME_CMD_WIFI_SET && strlen(c.password) == 64);
    home_cmd_parse("wifi Net 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg", &c);
    assert(c.type == HOME_CMD_ERROR);

    home_cmd_parse("tz <+08>-8", &c);
    assert(c.type == HOME_CMD_TZ && strcmp(c.tz, "<+08>-8") == 0);
    home_cmd_parse("tz CET-1CEST,M3.5.0,M10.5.0/3", &c);
    assert(c.type == HOME_CMD_TZ);
    home_cmd_parse("tz default", &c);
    assert(c.type == HOME_CMD_TZ_RESET);
    home_cmd_parse("tz +8", &c);
    assert(c.type == HOME_CMD_ERROR);
    home_cmd_parse("tz", &c);
    assert(c.type == HOME_CMD_ERROR);

    home_cmd_parse("clock 12", &c);
    assert(c.type == HOME_CMD_CLOCK && c.twelve_hour);
    home_cmd_parse("clock 24", &c);
    assert(c.type == HOME_CMD_CLOCK && !c.twelve_hour);
    home_cmd_parse("clock 13", &c);
    assert(c.type == HOME_CMD_ERROR);
    home_cmd_parse("reboot", &c);
    assert(c.type == HOME_CMD_ERROR);
    home_cmd_parse("a b c d e", &c);
    assert(c.type == HOME_CMD_ERROR);
}

int main(void) {
    test_date_and_clock();
    test_sync_line();
    test_schedule();
    test_weather();
    test_location();
    test_battery();
    test_cache();
    test_navigation();
    test_commands();
    printf("home logic tests: PASS\n");
    return 0;
}
