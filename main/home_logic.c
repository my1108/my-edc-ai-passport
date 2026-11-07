// main/home_logic.c - see home_logic.h.
#include "home_logic.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "home_json.h"

#define DEGREE "\xC2\xB0"

static const char *const WEEKDAYS[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char *const MONTHS[12] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
};

static void copy_text(char *out, size_t size, const char *text) {
    if (!out || size == 0) return;
    snprintf(out, size, "%s", text);
}

// ---------------------------------------------------------------------------
// Clock and date
// ---------------------------------------------------------------------------

void home_fmt_date(char *out, size_t size, const struct tm *local) {
    int wday = (local->tm_wday >= 0 && local->tm_wday < 7) ? local->tm_wday : 0;
    int mon = (local->tm_mon >= 0 && local->tm_mon < 12) ? local->tm_mon : 0;
    snprintf(out, size, "%s " HOME_SEP " %d %s", WEEKDAYS[wday], local->tm_mday, MONTHS[mon]);
}

static int hour_12(int hour) {
    int h = hour % 12;
    return h == 0 ? 12 : h;
}

void home_fmt_clock(const struct tm *local, bool twelve_hour,
                    char *hm, size_t hm_size,
                    char *sec, size_t sec_size,
                    char *ampm, size_t ampm_size) {
    if (hm && hm_size) {
        if (twelve_hour) {
            snprintf(hm, hm_size, "%d:%02d", hour_12(local->tm_hour), local->tm_min);
        } else {
            snprintf(hm, hm_size, "%02d:%02d", local->tm_hour, local->tm_min);
        }
    }
    if (sec && sec_size) snprintf(sec, sec_size, "%02d", local->tm_sec);
    if (ampm && ampm_size) copy_text(ampm, ampm_size, twelve_hour ? (local->tm_hour < 12 ? "AM" : "PM") : "");
}

void home_fmt_hm(char *out, size_t size, const struct tm *local, bool twelve_hour) {
    if (twelve_hour) {
        snprintf(out, size, "%d:%02d %s", hour_12(local->tm_hour), local->tm_min,
                 local->tm_hour < 12 ? "AM" : "PM");
    } else {
        snprintf(out, size, "%02d:%02d", local->tm_hour, local->tm_min);
    }
}

// ---------------------------------------------------------------------------
// Sync status line
// ---------------------------------------------------------------------------

bool home_fmt_sync(char *out, size_t size, const home_sync_view_t *view) {
    if (view->state == HOME_SYNC_RUNNING) {
        copy_text(out, size, "Syncing time...");
        return false;
    }
    if (view->state == HOME_SYNC_FAILED) {
        if (view->retry_in_s <= 0) {
            copy_text(out, size, "Sync failed " HOME_SEP " retrying soon");
        } else {
            int minutes = (view->retry_in_s + 59) / 60;
            if (minutes >= 60) {
                snprintf(out, size, "Sync failed " HOME_SEP " retrying in %d h", (minutes + 59) / 60);
            } else {
                snprintf(out, size, "Sync failed " HOME_SEP " retrying in %d min", minutes);
            }
        }
        return true;
    }
    if (view->have_last_sync) {
        char hm[16];
        home_fmt_hm(hm, sizeof(hm), &view->last_sync, view->twelve_hour);
        bool same_day = view->last_sync.tm_year == view->now.tm_year &&
                        view->last_sync.tm_yday == view->now.tm_yday;
        if (same_day) {
            snprintf(out, size, "Time synced today " HOME_SEP " %s", hm);
        } else {
            int mon = (view->last_sync.tm_mon >= 0 && view->last_sync.tm_mon < 12)
                    ? view->last_sync.tm_mon : 0;
            snprintf(out, size, "Time synced %d %s " HOME_SEP " %s",
                     view->last_sync.tm_mday, MONTHS[mon], hm);
        }
        return false;
    }
    if (!view->wifi_configured) {
        copy_text(out, size, "Wi-Fi not set " HOME_SEP " see serial");
    } else if (!view->wifi_connected) {
        copy_text(out, size, "Offline " HOME_SEP " waiting for Wi-Fi");
    } else {
        copy_text(out, size, "Waiting to sync...");
    }
    return false;
}

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------

int32_t home_delay_until_hour(int32_t local_sec_of_day, int hour) {
    if (local_sec_of_day < 0) local_sec_of_day = 0;
    if (local_sec_of_day > 86399) local_sec_of_day = 86399;
    if (hour < 0 || hour > 23) hour = 3;
    int32_t delay = (int32_t)hour * 3600 - local_sec_of_day;
    if (delay <= 0) delay += 86400;
    return delay;
}

int32_t home_time_sync_delay(bool success, int32_t local_sec_of_day, int hour) {
    return success ? home_delay_until_hour(local_sec_of_day, hour) : HOME_SYNC_RETRY_S;
}

int32_t home_weather_delay(bool success, int refresh_min) {
    if (refresh_min < 1) refresh_min = 1;
    if (success) return (int32_t)refresh_min * 60;
    return (int32_t)(refresh_min < 10 ? refresh_min : 10) * 60;
}

bool home_weather_is_fresh(bool online, bool fetched_this_boot, int64_t age_s, int refresh_min) {
    if (!online || !fetched_this_boot || age_s < 0) return false;
    if (refresh_min < 1) refresh_min = 1;
    return age_s <= (int64_t)refresh_min * 60 * 2;
}

// ---------------------------------------------------------------------------
// Weather
// ---------------------------------------------------------------------------

home_sky_t home_sky_from_wmo(int wmo, bool is_day) {
    switch (wmo) {
    case 0: case 1:
        return is_day ? HOME_SKY_CLEAR : HOME_SKY_CLEAR_NIGHT;
    case 2:
        return is_day ? HOME_SKY_PARTLY : HOME_SKY_PARTLY_NIGHT;
    case 3:
        return HOME_SKY_CLOUDY;
    case 45: case 48:
        return HOME_SKY_FOG;
    case 51: case 53: case 55: case 56: case 57:
    case 61: case 63: case 65: case 66: case 67:
    case 80: case 81: case 82:
        return HOME_SKY_RAIN;
    case 71: case 73: case 75: case 77: case 85: case 86:
        return HOME_SKY_SNOW;
    case 95: case 96: case 99:
        return HOME_SKY_STORM;
    default:
        return HOME_SKY_UNKNOWN;
    }
}

const char *home_condition_text(int wmo) {
    switch (wmo) {
    case 0: return "Clear sky";
    case 1: return "Mainly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: case 73: case 75: return "Snow";
    case 77: return "Snow grains";
    case 80: case 81: case 82: return "Rain showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Storm with hail";
    default: return "Unknown";
    }
}

static bool to_c10(double celsius, int16_t *out) {
    if (!(celsius > -100.0 && celsius < 70.0)) return false;  // also rejects NaN
    double scaled = celsius * 10.0;
    *out = (int16_t)(scaled >= 0 ? scaled + 0.5 : scaled - 0.5);
    return true;
}

bool home_parse_weather(const char *json, home_weather_t *out) {
    double temp, high, low, code, is_day = 1;
    if (!home_json_number(json, "current.temperature_2m", &temp) ||
        !home_json_number(json, "current.weather_code", &code) ||
        !home_json_number(json, "daily.temperature_2m_max.0", &high) ||
        !home_json_number(json, "daily.temperature_2m_min.0", &low)) {
        return false;
    }
    (void)home_json_number(json, "current.is_day", &is_day);
    home_weather_t w = { 0 };
    if (!to_c10(temp, &w.temp_c10) || !to_c10(high, &w.high_c10) || !to_c10(low, &w.low_c10)) {
        return false;
    }
    if (!(code >= 0 && code <= 99)) return false;
    w.wmo = (int16_t)code;
    w.is_day = is_day != 0;
    *out = w;
    return true;
}

static bool to_e4(double degrees, double limit, int32_t *out) {
    if (!(degrees >= -limit && degrees <= limit)) return false;
    double scaled = degrees * 10000.0;
    *out = (int32_t)(scaled >= 0 ? scaled + 0.5 : scaled - 0.5);
    return true;
}

bool home_parse_location(const char *json, home_location_t *out) {
    bool ok;
    char status[16];
    if (home_json_bool(json, "success", &ok)) {
        if (!ok) return false;
    } else if (home_json_string(json, "status", status, sizeof(status))) {
        if (strcmp(status, "success") != 0) return false;
    }

    double lat, lon;
    if (!(home_json_number(json, "latitude", &lat) || home_json_number(json, "lat", &lat)) ||
        !(home_json_number(json, "longitude", &lon) || home_json_number(json, "lon", &lon))) {
        return false;
    }
    home_location_t loc = { 0 };
    if (!to_e4(lat, 90.0, &loc.lat_e4) || !to_e4(lon, 180.0, &loc.lon_e4)) return false;
    (void)home_json_string(json, "city", loc.city, sizeof(loc.city));
    *out = loc;
    return true;
}

int home_round_c10(int c10) {
    return c10 >= 0 ? (c10 + 5) / 10 : -((-c10 + 5) / 10);
}

// Base letters for U+00C0..U+00FF.
static const char LATIN1_FOLD[65] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

void home_ascii_fold(const char *in, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    size_t len = 0;
    const unsigned char *p = (const unsigned char *)(in ? in : "");
    while (*p && len + 1 < out_size) {
        unsigned char c = *p;
        if (c < 0x80) {
            if (c >= 0x20 && c < 0x7F) out[len++] = (char)c;
            p++;
            continue;
        }
        size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        size_t avail = 1;
        while (avail < n && p[avail] && (p[avail] & 0xC0) == 0x80) avail++;
        if (n == 2 && avail == 2) {
            unsigned cp = ((unsigned)(c & 0x1F) << 6) | (p[1] & 0x3F);
            out[len++] = (cp >= 0xC0 && cp <= 0xFF) ? LATIN1_FOLD[cp - 0xC0] : '?';
        } else {
            out[len++] = '?';
        }
        p += avail;
    }
    out[len] = '\0';
}

void home_fmt_location_line(char *out, size_t size, const char *city_ascii,
                            bool have_weather, const home_weather_t *w) {
    bool have_city = city_ascii && city_ascii[0];
    if (!have_weather) {
        copy_text(out, size, have_city ? city_ascii : "Locating...");
        return;
    }
    int high = home_round_c10(w->high_c10);
    int low = home_round_c10(w->low_c10);
    if (have_city) {
        snprintf(out, size, "%s " HOME_SEP " H %d" DEGREE " L %d" DEGREE, city_ascii, high, low);
    } else {
        snprintf(out, size, "H %d" DEGREE " L %d" DEGREE, high, low);
    }
}

// ---------------------------------------------------------------------------
// Battery
// ---------------------------------------------------------------------------

int home_battery_level(int soc) {
    if (soc < 0) return -1;
    if (soc >= 88) return 4;
    if (soc >= 63) return 3;
    if (soc >= 38) return 2;
    if (soc >= 13) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Persistent cache
// ---------------------------------------------------------------------------

uint32_t home_crc32(const void *data, size_t len) {
    const uint8_t *bytes = data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
        }
    }
    return ~crc;
}

void home_cache_encode(const home_cache_t *cache, home_cache_blob_t *blob) {
    memset(blob, 0, sizeof(*blob));
    blob->magic = HOME_CACHE_MAGIC;
    blob->version = HOME_CACHE_VERSION;
    blob->size = (uint16_t)sizeof(*blob);
    blob->have_weather = cache->have_weather ? 1 : 0;
    blob->have_location = cache->have_location ? 1 : 0;
    blob->is_day = cache->weather.is_day ? 1 : 0;
    blob->temp_c10 = cache->weather.temp_c10;
    blob->high_c10 = cache->weather.high_c10;
    blob->low_c10 = cache->weather.low_c10;
    blob->wmo = cache->weather.wmo;
    blob->lat_e4 = cache->location.lat_e4;
    blob->lon_e4 = cache->location.lon_e4;
    blob->weather_epoch = cache->weather_epoch;
    memcpy(blob->city, cache->location.city, sizeof(blob->city));
    blob->city[sizeof(blob->city) - 1] = '\0';
    blob->crc32 = home_crc32(blob, offsetof(home_cache_blob_t, crc32));
}

bool home_cache_decode(const home_cache_blob_t *blob, home_cache_t *cache) {
    memset(cache, 0, sizeof(*cache));
    if (blob->magic != HOME_CACHE_MAGIC || blob->version != HOME_CACHE_VERSION ||
        blob->size != sizeof(*blob) ||
        blob->crc32 != home_crc32(blob, offsetof(home_cache_blob_t, crc32))) {
        return false;
    }
    cache->have_weather = blob->have_weather != 0;
    cache->have_location = blob->have_location != 0;
    cache->weather.is_day = blob->is_day != 0;
    cache->weather.temp_c10 = blob->temp_c10;
    cache->weather.high_c10 = blob->high_c10;
    cache->weather.low_c10 = blob->low_c10;
    cache->weather.wmo = blob->wmo;
    cache->location.lat_e4 = blob->lat_e4;
    cache->location.lon_e4 = blob->lon_e4;
    cache->weather_epoch = blob->weather_epoch;
    memcpy(cache->location.city, blob->city, sizeof(cache->location.city));
    cache->location.city[sizeof(cache->location.city) - 1] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

home_nav_result_t home_nav_handle(home_page_t page, home_key_t key, home_key_ev_t ev) {
    home_nav_result_t r = { .page = page, .page_changed = false, .sync_now = false };
    if (ev == HOME_KEY_EV_OTHER) return r;

    if (page == HOME_PAGE_HOME) {
        if (ev != HOME_KEY_EV_CLICK) return r;
        if (key == HOME_KEY_UP) {
            r.page = HOME_PAGE_APPS;
        } else if (key == HOME_KEY_OK) {
            r.page = HOME_PAGE_FOCUS;
        } else if (key == HOME_KEY_DOWN) {
            r.sync_now = true;
        }
    } else if (key == HOME_KEY_OK) {
        r.page = HOME_PAGE_HOME;
    }
    r.page_changed = r.page != page;
    return r;
}

// ---------------------------------------------------------------------------
// Serial provisioning commands
// ---------------------------------------------------------------------------

#define CMD_MAX_TOKENS 4
#define CMD_TOKEN_MAX  (HOME_PASS_MAX + 1)

typedef struct {
    int count;
    char tok[CMD_MAX_TOKENS][CMD_TOKEN_MAX + 1];
    const char *error;
} tokens_t;

static void tokenize(const char *line, tokens_t *t) {
    memset(t, 0, sizeof(*t));
    const char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) return;
        if (t->count == CMD_MAX_TOKENS) {
            t->error = "too many arguments; put names with spaces in quotes";
            return;
        }
        char *dst = t->tok[t->count];
        size_t len = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                char c = *p++;
                if (c == '\\' && (*p == '"' || *p == '\\')) c = *p++;
                if (len == CMD_TOKEN_MAX) {
                    t->error = "argument too long";
                    return;
                }
                dst[len++] = c;
            }
            if (*p != '"') {
                t->error = "missing closing quote";
                return;
            }
            p++;
        } else {
            while (*p && *p != ' ' && *p != '\t') {
                if (len == CMD_TOKEN_MAX) {
                    t->error = "argument too long";
                    return;
                }
                dst[len++] = *p++;
            }
        }
        dst[len] = '\0';
        t->count++;
    }
}

static bool word_is(const char *word, const char *expected) {
    while (*word && *expected) {
        if (tolower((unsigned char)*word) != *expected) return false;
        word++;
        expected++;
    }
    return *word == '\0' && *expected == '\0';
}

static bool is_hex64(const char *s) {
    if (strlen(s) != 64) return false;
    for (; *s; s++) {
        if (!isxdigit((unsigned char)*s)) return false;
    }
    return true;
}

static void set_error(home_cmd_t *out, const char *message) {
    out->type = HOME_CMD_ERROR;
    out->error = message;
}

void home_cmd_parse(const char *line, home_cmd_t *out) {
    memset(out, 0, sizeof(*out));
    tokens_t t;
    tokenize(line ? line : "", &t);
    if (t.error) {
        set_error(out, t.error);
        return;
    }
    if (t.count == 0) {
        out->type = HOME_CMD_NONE;
        return;
    }

    const char *cmd = t.tok[0];
    if (word_is(cmd, "help") || strcmp(cmd, "?") == 0) {
        out->type = t.count == 1 ? HOME_CMD_HELP : HOME_CMD_ERROR;
        if (t.count != 1) out->error = "help takes no arguments";
    } else if (word_is(cmd, "status")) {
        out->type = t.count == 1 ? HOME_CMD_STATUS : HOME_CMD_ERROR;
        if (t.count != 1) out->error = "status takes no arguments";
    } else if (word_is(cmd, "sync")) {
        out->type = t.count == 1 ? HOME_CMD_SYNC : HOME_CMD_ERROR;
        if (t.count != 1) out->error = "sync takes no arguments";
    } else if (word_is(cmd, "wifi")) {
        if (t.count == 2 && word_is(t.tok[1], "clear")) {
            out->type = HOME_CMD_WIFI_CLEAR;
            return;
        }
        if (t.count < 2 || t.count > 3) {
            set_error(out, "usage: wifi \"<ssid>\" \"<password>\"  or  wifi clear");
            return;
        }
        size_t ssid_len = strlen(t.tok[1]);
        if (ssid_len == 0 || ssid_len > HOME_SSID_MAX) {
            set_error(out, "SSID must be 1 to 32 bytes");
            return;
        }
        const char *pass = t.count == 3 ? t.tok[2] : "";
        size_t pass_len = strlen(pass);
        if (pass_len != 0 && (pass_len < 8 || pass_len > 63) && !is_hex64(pass)) {
            set_error(out, "password must be 8 to 63 characters (or empty for an open network)");
            return;
        }
        out->type = HOME_CMD_WIFI_SET;
        memcpy(out->ssid, t.tok[1], ssid_len + 1);
        memcpy(out->password, pass, pass_len + 1);
    } else if (word_is(cmd, "tz")) {
        if (t.count != 2) {
            set_error(out, "usage: tz <POSIX TZ, e.g. <+08>-8>  or  tz default");
            return;
        }
        if (word_is(t.tok[1], "default")) {
            out->type = HOME_CMD_TZ_RESET;
            return;
        }
        size_t len = strlen(t.tok[1]);
        char first = t.tok[1][0];
        if (len > HOME_TZ_MAX || !(isalpha((unsigned char)first) || first == '<')) {
            set_error(out, "invalid TZ; use POSIX form such as <+08>-8 or CET-1CEST,M3.5.0,M10.5.0/3");
            return;
        }
        out->type = HOME_CMD_TZ;
        memcpy(out->tz, t.tok[1], len + 1);
    } else if (word_is(cmd, "clock")) {
        if (t.count == 2 && strcmp(t.tok[1], "12") == 0) {
            out->type = HOME_CMD_CLOCK;
            out->twelve_hour = true;
        } else if (t.count == 2 && strcmp(t.tok[1], "24") == 0) {
            out->type = HOME_CMD_CLOCK;
            out->twelve_hour = false;
        } else {
            set_error(out, "usage: clock 12  or  clock 24");
        }
    } else {
        set_error(out, "unknown command; type help");
    }
}
