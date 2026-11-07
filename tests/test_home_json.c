// Host tests for main/home_json.c.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "home_json.h"

// Real Open-Meteo response captured 2026-11-08 (Kuala Lumpur).
static const char *WEATHER =
    "{\"latitude\":3.1282952,\"longitude\":101.68548,\"generationtime_ms\":0.126,"
    "\"utc_offset_seconds\":28800,\"timezone\":\"Asia/Kuala_Lumpur\","
    "\"timezone_abbreviation\":\"GMT+8\",\"elevation\":55.0,"
    "\"current_units\":{\"time\":\"iso8601\",\"interval\":\"seconds\","
    "\"temperature_2m\":\"\xC2\xB0" "C\",\"weather_code\":\"wmo code\",\"is_day\":\"\"},"
    "\"current\":{\"time\":\"2026-11-08T00:45\",\"interval\":900,\"temperature_2m\":25.4,"
    "\"weather_code\":2,\"is_day\":0},"
    "\"daily_units\":{\"time\":\"iso8601\",\"temperature_2m_max\":\"\xC2\xB0" "C\","
    "\"temperature_2m_min\":\"\xC2\xB0" "C\"},"
    "\"daily\":{\"time\":[\"2026-11-08\"],\"temperature_2m_max\":[31.4],"
    "\"temperature_2m_min\":[23.9]}}";

static void test_nested_numbers(void) {
    double v;
    assert(home_json_number(WEATHER, "current.temperature_2m", &v) && fabs(v - 25.4) < 1e-9);
    assert(home_json_number(WEATHER, "current.weather_code", &v) && v == 2);
    assert(home_json_number(WEATHER, "current.is_day", &v) && v == 0);
    assert(home_json_number(WEATHER, "daily.temperature_2m_max.0", &v) && fabs(v - 31.4) < 1e-9);
    assert(home_json_number(WEATHER, "daily.temperature_2m_min.0", &v) && fabs(v - 23.9) < 1e-9);
    assert(home_json_number(WEATHER, "utc_offset_seconds", &v) && v == 28800);
    // The same key under current_units is a string, not a number.
    assert(!home_json_number(WEATHER, "current_units.temperature_2m", &v));
    assert(!home_json_number(WEATHER, "daily.temperature_2m_max.1", &v));
    assert(!home_json_number(WEATHER, "missing", &v));
    assert(!home_json_number(WEATHER, "current.missing", &v));
}

static void test_strings_and_bools(void) {
    char buf[32];
    assert(home_json_string(WEATHER, "timezone", buf, sizeof(buf)));
    assert(strcmp(buf, "Asia/Kuala_Lumpur") == 0);
    assert(home_json_string(WEATHER, "daily.time.0", buf, sizeof(buf)));
    assert(strcmp(buf, "2026-11-08") == 0);
    assert(!home_json_string(WEATHER, "elevation", buf, sizeof(buf)));

    const char *geo = "{\n    \"success\": true,\n    \"city\": \"Petaling\",\n"
                      "    \"latitude\": 3.0833326,\n    \"longitude\": 101.5833333\n}";
    bool b = false;
    assert(home_json_bool(geo, "success", &b) && b);
    assert(home_json_string(geo, "city", buf, sizeof(buf)) && strcmp(buf, "Petaling") == 0);
    assert(!home_json_bool(geo, "city", &b));
}

static void test_escapes_and_truncation(void) {
    char buf[16];
    const char *doc = "{\"a\":\"Z\\u00fcrich \\\"x\\\"\",\"b\":\"\\ud83d\\ude00\",\"c\":\"\xC3\xA9\xC3\xA9\xC3\xA9\"}";
    assert(home_json_string(doc, "a", buf, sizeof(buf)));
    assert(strcmp(buf, "Z\xC3\xBCrich \"x\"") == 0);
    assert(home_json_string(doc, "b", buf, sizeof(buf)));
    assert(strcmp(buf, "\xF0\x9F\x98\x80") == 0);

    // A 4-byte buffer holds one 2-byte character plus NUL, never half of the next.
    char tiny[4];
    assert(home_json_string(doc, "c", tiny, sizeof(tiny)));
    assert(strcmp(tiny, "\xC3\xA9") == 0);
}

static void test_malformed(void) {
    double v;
    char buf[8];
    assert(!home_json_number("", "a", &v));
    assert(!home_json_number("{\"a\":", "a", &v));
    assert(!home_json_number("{\"a\":-}", "a", &v));
    assert(!home_json_number("{\"x\":\"unterminated,\"a\":1}", "a", &v));
    assert(!home_json_string("{\"a\":\"no end", "a", buf, sizeof(buf)));
    assert(!home_json_string("{\"a\":\"bad \\q escape\"}", "a", buf, sizeof(buf)));
    assert(!home_json_number("[1,2]", "a", &v));
    assert(home_json_number("[1,2]", "1", &v) && v == 2);
    assert(home_json_number("{ \"a\" : { \"b\" : [ 1 , { \"c\" : -7.5e1 } ] } }", "a.b.1.c", &v));
    assert(v == -75.0);

    // Deep nesting is rejected instead of recursing without bound.
    char deep[128];
    memset(deep, '[', 40);
    strcpy(deep + 40, "1");
    memset(deep + 41, ']', 40);
    deep[81] = '\0';
    char doc[160];
    snprintf(doc, sizeof(doc), "{\"skip\":%s,\"a\":3}", deep);
    assert(!home_json_number(doc, "a", &v));
}

int main(void) {
    test_nested_numbers();
    test_strings_and_bools();
    test_escapes_and_truncation();
    test_malformed();
    printf("home JSON tests: PASS\n");
    return 0;
}
