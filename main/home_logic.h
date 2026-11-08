// main/home_logic.h - pure home-screen logic: text formatting, weather mapping,
// sync scheduling, navigation, cache encoding, and serial-command parsing.
//
// Nothing here touches ESP-IDF, FreeRTOS, or LVGL so it can be covered by host
// tests (tests/test_home_logic.c). Firmware glue lives in home_net.c,
// home_store.c, home_console.c, and home_ui.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "spot_logic.h"

// ---------------------------------------------------------------------------
// Clock and date
// ---------------------------------------------------------------------------

// Bullet separator. The built-in Montserrat fonts include U+2022 but not the
// middle dot U+00B7, so the UI uses this instead of "·".
#define HOME_SEP "\xE2\x80\xA2"

// Sanity floor for a synced wall clock (2024-01-01 UTC). Not proof of a sync on
// its own: the board's clock starts at 2026-01-01; see home_net_time_valid().
#define HOME_TIME_VALID_EPOCH 1704067200LL

static inline bool home_time_is_valid(int64_t epoch) {
    return epoch >= HOME_TIME_VALID_EPOCH;
}

// "SUN • 8 NOV"
void home_fmt_date(char *out, size_t size, const struct tm *local);

// Fills "HH:MM" (or "h:MM" in 12-hour mode), "SS", and "AM"/"PM" (empty in
// 24-hour mode). Any output pointer may be NULL.
void home_fmt_clock(const struct tm *local, bool twelve_hour,
                    char *hm, size_t hm_size,
                    char *sec, size_t sec_size,
                    char *ampm, size_t ampm_size);

// "14:05" or "2:05 PM".
void home_fmt_hm(char *out, size_t size, const struct tm *local, bool twelve_hour);

// ---------------------------------------------------------------------------
// Sync status line
// ---------------------------------------------------------------------------

typedef enum {
    HOME_SYNC_IDLE = 0,   // not attempted yet this boot
    HOME_SYNC_RUNNING,
    HOME_SYNC_OK,
    HOME_SYNC_FAILED,
} home_sync_state_t;

typedef struct {
    bool wifi_configured;
    bool wifi_connected;
    home_sync_state_t state;
    bool have_last_sync;          // last_sync is valid
    struct tm last_sync;          // local time of the last successful sync
    struct tm now;                // current local time (valid when have_last_sync)
    int32_t retry_in_s;           // seconds until the next attempt after a failure
    bool twelve_hour;
} home_sync_view_t;

// Writes the sync line. Returns true when it should be drawn as a warning
// (orange), i.e. after a failed sync.
bool home_fmt_sync(char *out, size_t size, const home_sync_view_t *view);

// ---------------------------------------------------------------------------
// Scheduling (all values are delays in seconds, so they can be applied to a
// monotonic clock even when the wall clock is not yet valid)
// ---------------------------------------------------------------------------

#define HOME_SYNC_RETRY_MIN_S 60          // first retry after a failed sync
#define HOME_SYNC_RETRY_S     (60 * 60)    // longest gap between retries

// Delay from `local_sec_of_day` (0..86399) until the next `hour`:00:00 local,
// strictly in the future (exactly at the hour yields 24 h).
int32_t home_delay_until_hour(int32_t local_sec_of_day, int hour);

// Delay after a time-sync attempt: the next daily `hour`:00 on success,
// a retry on failure: HOME_SYNC_RETRY_MIN_S after the first failure, doubling
// with each further one (`failures` counts consecutive failures, including
// this one) up to HOME_SYNC_RETRY_S. A single dropped packet at boot then
// costs a minute of "--:--", not an hour.
int32_t home_time_sync_delay(bool success, int32_t local_sec_of_day, int hour, int failures);

// Delay after a weather fetch: `refresh_min` minutes on success, a shorter
// retry (10 min, never longer than refresh_min) on failure.
int32_t home_weather_delay(bool success, int refresh_min);

// Whether a weather reading of `age_s` seconds should still be drawn in colour.
// Readings older than two refresh periods, or shown while offline, are greyed.
bool home_weather_is_fresh(bool online, bool fetched_this_boot, int64_t age_s, int refresh_min);

// ---------------------------------------------------------------------------
// Weather
// ---------------------------------------------------------------------------

typedef enum {
    HOME_SKY_CLEAR = 0,
    HOME_SKY_CLEAR_NIGHT,
    HOME_SKY_PARTLY,
    HOME_SKY_PARTLY_NIGHT,
    HOME_SKY_CLOUDY,
    HOME_SKY_FOG,
    HOME_SKY_RAIN,
    HOME_SKY_SNOW,
    HOME_SKY_STORM,
    HOME_SKY_UNKNOWN,
} home_sky_t;

// Maps a WMO weather code (Open-Meteo `weather_code`) to an icon family.
home_sky_t home_sky_from_wmo(int wmo, bool is_day);

// Short English condition text for a WMO code, e.g. "Partly cloudy".
const char *home_condition_text(int wmo);

typedef struct {
    int16_t temp_c10;     // current temperature, tenths of a degree C
    int16_t high_c10;
    int16_t low_c10;
    int16_t wmo;
    bool is_day;
} home_weather_t;

typedef struct {
    char city[32];        // UTF-8, as returned by the provider
    int32_t lat_e4;       // degrees * 1e4
    int32_t lon_e4;
} home_location_t;

// Parses an Open-Meteo forecast response requested with
// current=temperature_2m,weather_code,is_day and
// daily=temperature_2m_max,temperature_2m_min.
bool home_parse_weather(const char *json, home_weather_t *out);

// Parses an ipwho.is response (fields=success,city,latitude,longitude). Also
// accepts ip-api.com's {"status":"success","city","lat","lon"} shape.
bool home_parse_location(const char *json, home_location_t *out);

// Rounds tenths of a degree to whole degrees (half away from zero).
int home_round_c10(int c10);

// Copies UTF-8 `in` to printable ASCII for the built-in font: Latin-1 accented
// letters become their base letter (e.g. "Zürich" -> "Zurich"), anything else
// outside ASCII becomes '?'.
void home_ascii_fold(const char *in, char *out, size_t out_size);

// Writes "City • H 31° L 24°" (falls back to "Locating..." without a city).
void home_fmt_location_line(char *out, size_t size, const char *city_ascii,
                            bool have_weather, const home_weather_t *w);

// ---------------------------------------------------------------------------
// Battery
// ---------------------------------------------------------------------------

// Icon level 0..4 for a state of charge 0..100, or -1 when unknown (soc < 0).
int home_battery_level(int soc);

// ---------------------------------------------------------------------------
// Persistent cache (weather + location survive power loss)
// ---------------------------------------------------------------------------

#define HOME_CACHE_MAGIC   0x484F4D45u  // "HOME"
#define HOME_CACHE_VERSION 1

typedef struct {
    bool have_weather;
    bool have_location;
    home_weather_t weather;
    home_location_t location;
    int64_t weather_epoch;   // wall-clock time of the reading (0 if unknown)
} home_cache_t;

// Fixed-layout blob written to NVS. Little-endian, explicitly padded.
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint8_t have_weather;
    uint8_t have_location;
    uint8_t is_day;
    uint8_t reserved0;
    int16_t temp_c10;
    int16_t high_c10;
    int16_t low_c10;
    int16_t wmo;
    int32_t lat_e4;
    int32_t lon_e4;
    uint32_t reserved1;      // keeps weather_epoch 8-byte aligned without implicit padding
    int64_t weather_epoch;
    char city[32];
    uint32_t crc32;
} home_cache_blob_t;

uint32_t home_crc32(const void *data, size_t len);
void home_cache_encode(const home_cache_t *cache, home_cache_blob_t *blob);
// Returns false (and leaves *cache zeroed) for a wrong magic/version/size/CRC.
bool home_cache_decode(const home_cache_blob_t *blob, home_cache_t *cache);

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

typedef enum {
    HOME_PAGE_HOME = 0,
    HOME_PAGE_APPS,          // app tray: a list of the apps in home_apps.c
    HOME_PAGE_APP,           // the app opened from the tray (one at a time)
    HOME_PAGE_SETUP,         // phone setup for Wi-Fi and Spotify (replaced Focus)
    HOME_PAGE_COUNT,
} home_page_t;

typedef enum {
    HOME_KEY_UP = 0,
    HOME_KEY_DOWN,
    HOME_KEY_OK,
} home_key_t;

typedef enum {
    HOME_KEY_EV_CLICK = 0,   // includes double clicks: quick taps are still taps
    HOME_KEY_EV_LONG,
    HOME_KEY_EV_OTHER,       // press / release: not user intent
} home_key_ev_t;

typedef struct {
    home_page_t page;        // page to show after this key
    bool page_changed;
    bool sync_now;           // start an immediate time/weather sync
    int setup_step;          // -1 / +1: previous / next card (Setup page)
    int tray_step;           // -1 / +1: move the tray selection (Apps page)
    bool open_app;           // open the selected tray entry (page becomes APP)
    bool app_key;            // pass this key to the open app's key handler
} home_nav_result_t;

// Holding OK always goes back one level.
// Home:  UP -> Apps tray, OK -> Setup, DOWN -> sync now.
// Apps:  UP / DOWN -> move the selection (clamped, no wrap), OK -> open the
//        selected app, OK long press -> Home.
// App:   OK long press -> Apps tray; every other click goes to the app.
// Setup: UP / DOWN -> previous / next QR card, OK (click or long) -> Home.
home_nav_result_t home_nav_handle(home_page_t page, home_key_t key, home_key_ev_t ev);

// Moves a list selection by `delta`, clamped to 0..count-1 (no wrap: at the
// top, UP stays put). Returns 0 for an empty list.
int home_tray_move(int selected, int delta, int count);

// Spotify's keys inside the app: UP previous track, DOWN next track, OK
// play/pause (clicks only; OK long press is handled by navigation).
spot_cmd_t home_spotify_key(home_key_t key, home_key_ev_t ev);

// ---------------------------------------------------------------------------
// Screen sleep
// ---------------------------------------------------------------------------

// The screen sleeps after a period without key activity. The key press that
// wakes it is swallowed (through its release), so waking never also opens a
// page or starts a sync.
typedef struct {
    bool asleep;
    bool swallow;            // drop events until the waking key is released
    int64_t last_activity_us;
} home_idle_t;

typedef struct {
    bool deliver;            // pass the event on to navigation
    bool woke;               // the screen must be turned back on
} home_idle_result_t;

void home_idle_init(home_idle_t *idle, int64_t now_us);

// Call for every raw key event (press, release, click, long, ...).
home_idle_result_t home_idle_key(home_idle_t *idle, bool is_press, bool is_release, int64_t now_us);

// Call periodically. Returns true exactly when the screen should go to sleep.
// timeout_us <= 0 disables sleeping.
bool home_idle_tick(home_idle_t *idle, int64_t now_us, int64_t timeout_us);

// Whether to enter deep sleep (the "off" state; any key wakes and restarts the
// app) after `idle_us` without a key. Never while Setup is open, and not while
// a computer is connected over USB when `stay_on_usb` is set. timeout_us <= 0
// disables deep sleep.
bool home_deep_sleep_due(int64_t idle_us, int64_t timeout_us, bool setup_open, bool usb_host,
                         bool stay_on_usb);

// ---------------------------------------------------------------------------
// Serial provisioning commands
// ---------------------------------------------------------------------------

#define HOME_SSID_MAX 32
#define HOME_PASS_MAX 64
#define HOME_TZ_MAX   48

typedef enum {
    HOME_CMD_NONE = 0,       // empty line
    HOME_CMD_HELP,
    HOME_CMD_STATUS,
    HOME_CMD_WIFI_SET,       // wifi "<ssid>" ["<password>"]
    HOME_CMD_WIFI_CLEAR,     // wifi clear
    HOME_CMD_TZ,             // tz <POSIX TZ string>
    HOME_CMD_TZ_RESET,       // tz default
    HOME_CMD_CLOCK,          // clock 12|24
    HOME_CMD_SYNC,           // sync
    HOME_CMD_SPOT_CLIENT,    // spotify client <32-hex client ID>
    HOME_CMD_SPOT_TOKEN,     // spotify token <refresh token>
    HOME_CMD_SPOT_CLEAR,     // spotify clear
    HOME_CMD_SPOT_STATUS,    // spotify  |  spotify status
    HOME_CMD_ERROR,
} home_cmd_type_t;

typedef struct {
    home_cmd_type_t type;
    char ssid[HOME_SSID_MAX + 1];
    char password[HOME_PASS_MAX + 1];
    char tz[HOME_TZ_MAX + 1];
    bool twelve_hour;
    char spot_arg[SPOT_REFRESH_MAX + 1];   // client ID or refresh token (secret; wipe after use)
    const char *error;       // static message when type == HOME_CMD_ERROR
} home_cmd_t;

// Parses one console line (without the line terminator). Arguments may be
// double-quoted; inside quotes, \" and \\ are escapes.
void home_cmd_parse(const char *line, home_cmd_t *out);
