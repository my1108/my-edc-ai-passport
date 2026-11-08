// main/home_store.h - NVS persistence for the home screen.
//
// Namespace "home":
//   cache  blob  last weather + location (home_cache_blob_t), survives power loss
//   ssid   str   Wi-Fi SSID      (set from the serial console, never compiled in)
//   pass   str   Wi-Fi password  (stored unencrypted in NVS; never logged)
//   tz     str   POSIX TZ override (default CONFIG_HOME_TZ_DEFAULT)
//   clk12  u8    12-hour clock override (default CONFIG_HOME_CLOCK_12H_DEFAULT)
//   sp_cid str   Spotify app client ID (public, but set per user, never compiled in)
//   sp_rt  str   Spotify refresh token (secret: unencrypted in NVS; never logged)
//
// All functions may block on flash and must not run in button callbacks or the
// LVGL task.
#pragma once

#include <stdbool.h>
#include <stddef.h>

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

// Spotify link. Either output may be empty when not set. Sizes:
// client_id >= SPOT_CLIENT_ID_LEN + 1, refresh >= SPOT_REFRESH_MAX + 1.
void home_store_load_spotify(char *client_id, size_t client_id_size, char *refresh, size_t refresh_size);
esp_err_t home_store_save_spotify_client(const char *client_id);
// Spotify may issue a new refresh token on every refresh; it is saved at once.
esp_err_t home_store_save_spotify_token(const char *refresh);
// Erases both the client ID and the refresh token.
esp_err_t home_store_clear_spotify(void);
// Erases only the refresh token (after Spotify revoked it).
esp_err_t home_store_clear_spotify_token(void);

// Applies a POSIX TZ string to this process (setenv + tzset).
void home_apply_tz(const char *tz);

// Current clock mode, readable from any task (updated by home_store_save_clock
// and home_store_load_settings).
bool home_twelve_hour(void);
