// main/home_store.h - NVS persistence for the home screen.
//
// Namespace "home":
//   cache  blob  last weather + location (home_cache_blob_t), survives power loss
//   ssid   str   Wi-Fi SSID      (set from the serial console, never compiled in)
//   pass   str   Wi-Fi password  (stored unencrypted in NVS; never logged)
//   tz     str   POSIX TZ override (default CONFIG_HOME_TZ_DEFAULT)
//   clk12  u8    12-hour clock override (default CONFIG_HOME_CLOCK_12H_DEFAULT)
//
// All functions may block on flash and must not run in button callbacks or the
// LVGL task.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "home_logic.h"

typedef struct {
    char ssid[HOME_SSID_MAX + 1];
    char password[HOME_PASS_MAX + 1];
    char tz[HOME_TZ_MAX + 1];
    bool twelve_hour;
} home_settings_t;

// Initializes NVS without erasing it on failure (see demo_radio_nvs_prepare).
esp_err_t home_store_init(void);

// Fills Kconfig defaults first, then applies stored overrides.
void home_store_load_settings(home_settings_t *out);

bool home_store_load_cache(home_cache_t *out);
esp_err_t home_store_save_cache(const home_cache_t *cache);

// Empty ssid erases the stored credentials.
esp_err_t home_store_save_wifi(const char *ssid, const char *password);
// NULL tz erases the override and returns to CONFIG_HOME_TZ_DEFAULT.
esp_err_t home_store_save_tz(const char *tz);
esp_err_t home_store_save_clock(bool twelve_hour);

// Applies a POSIX TZ string to this process (setenv + tzset).
void home_apply_tz(const char *tz);

// Current clock mode, readable from any task (updated by home_store_save_clock
// and home_store_load_settings).
bool home_twelve_hour(void);
