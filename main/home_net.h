// main/home_net.h - Wi-Fi station, SNTP time sync, IP geolocation, and weather.
//
// One worker task owns all networking. Other tasks only read a status snapshot
// or post a request; nothing here blocks the caller.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "home_logic.h"

typedef struct {
    bool wifi_configured;
    bool wifi_connected;

    home_sync_state_t sync_state;
    int64_t last_sync_epoch;     // wall clock of the last successful sync, 0 if none
    int64_t retry_at_us;         // monotonic time of the next attempt after a failure

    bool have_weather;           // from this boot or from the flash cache
    bool weather_this_boot;      // fetched since power-on (otherwise cached)
    int64_t weather_at_us;       // monotonic time of the fetch (valid if this boot)
    home_weather_t weather;

    bool have_location;
    char city[32];               // UTF-8 from the provider
} home_status_t;

// Loads the cache and starts the worker. Call once after NVS is ready.
esp_err_t home_net_start(void);

// True once SNTP has set the wall clock since the last power-on. A soft reset
// keeps it (the RTC keeps counting); a power loss clears it. Do not judge the
// clock by its year: this board starts counting from 2026-01-01, not 1970.
bool home_net_time_valid(void);

// Copies the current status. Safe from any task, including the LVGL task.
void home_net_get_status(home_status_t *out);

// Requests an immediate time sync, geolocation, and weather refresh.
void home_net_sync_now(void);

// Tells the worker that the stored Wi-Fi credentials changed.
void home_net_wifi_changed(void);
