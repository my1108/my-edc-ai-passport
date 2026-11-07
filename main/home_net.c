// main/home_net.c - see home_net.h.
//
// Schedule (all on the monotonic clock, so it works before the wall clock is set):
//   time sync   at every boot once Wi-Fi is up, then daily at CONFIG_HOME_SYNC_HOUR
//               local time; after a failure, retry every hour.
//   location    at boot and on "sync now"; daily after success, 30 min after failure.
//   weather     every CONFIG_HOME_WEATHER_REFRESH_MIN; 10 min after a failure.
//   DOWN key / "sync" command: all three immediately.
//
// Memory: responses are read into one static 1.5 KB buffer (both providers
// answer in well under 1 KB). TLS uses the ESP-IDF certificate bundle with
// mbedTLS dynamic buffers (see sdkconfig.defaults); heap is logged around each
// request so the TLS budget can be checked on the device.
#include "home_net.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "demo_radio.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "home_store.h"
#include "sdkconfig.h"

static const char *TAG = "home_net";

#define EV_STA_START     BIT0
#define EV_GOT_IP        BIT1
#define EV_DISCONNECTED  BIT2
#define EV_SYNC_NOW      BIT3
#define EV_WIFI_CHANGED  BIT4
#define EV_ALL (EV_STA_START | EV_GOT_IP | EV_DISCONNECTED | EV_SYNC_NOW | EV_WIFI_CHANGED)

#define NET_TASK_STACK     8192   // TLS handshake + HTTP client
#define HTTP_BODY_MAX      1536
#define HTTP_TIMEOUT_MS    10000
#define SNTP_WAIT_MS       15000
#define CONNECT_BACKOFF_MIN_S 2
#define CONNECT_BACKOFF_MAX_S 60
#define CONNECT_WATCHDOG_S 30     // re-issue connect if no event arrives
#define GEO_OK_DELAY_S     (24 * 3600)
#define GEO_FAIL_DELAY_S   (30 * 60)
#define US_PER_S           1000000LL

static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;
static home_status_t s_status;
static home_cache_t s_cache;          // net task only
static char s_body[HTTP_BODY_MAX];    // net task only
static bool s_wifi_inited;
static bool s_wifi_started;

// Survives a soft reset but not power loss; random at power-on, so a magic
// value (not a bool) marks "SNTP has set the clock".
#define RTC_TIME_MAGIC 0x54494D45u  // "TIME"
static RTC_NOINIT_ATTR uint32_t s_rtc_time_magic;

bool home_net_time_valid(void) {
    return s_rtc_time_magic == RTC_TIME_MAGIC && home_time_is_valid((int64_t)time(NULL));
}

static int64_t now_us(void) {
    return esp_timer_get_time();
}

#define STATUS_UPDATE(...) do {                     \
        xSemaphoreTake(s_lock, portMAX_DELAY);      \
        __VA_ARGS__;                                \
        xSemaphoreGive(s_lock);                     \
    } while (0)

static void fmt_e4(char *out, size_t size, int32_t e4) {
    int32_t a = e4 < 0 ? -e4 : e4;
    snprintf(out, size, "%s%" PRId32 ".%04" PRId32, e4 < 0 ? "-" : "", a / 10000, a % 10000);
}

void home_net_get_status(home_status_t *out) {
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_status;
    xSemaphoreGive(s_lock);
}

void home_net_sync_now(void) {
    if (s_events) xEventGroupSetBits(s_events, EV_SYNC_NOW);
}

void home_net_wifi_changed(void) {
    if (s_events) xEventGroupSetBits(s_events, EV_WIFI_CHANGED);
}

static void log_heap(const char *when) {
    ESP_LOGI(TAG, "heap %s: free=%u largest=%u min_ever=%u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_events, EV_STA_START);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        STATUS_UPDATE(s_status.wifi_connected = false);
        xEventGroupSetBits(s_events, EV_DISCONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        STATUS_UPDATE(s_status.wifi_connected = true);
        xEventGroupSetBits(s_events, EV_GOT_IP);
    }
}

static esp_err_t wifi_init_once(void) {
    if (s_wifi_inited) return ESP_OK;
    esp_err_t err = demo_radio_network_prepare();
    if (err != ESP_OK) return err;
    if (!esp_netif_create_default_wifi_sta()) return ESP_FAIL;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;
    // Credentials live in our own NVS keys; keep the driver from copying them.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL);
    }
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    s_wifi_inited = true;
    return ESP_OK;
}

static esp_err_t wifi_apply(const home_settings_t *settings) {
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    // ssid/password fields are fixed arrays and need not be NUL-terminated.
    memcpy(wc.sta.ssid, settings->ssid, strnlen(settings->ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, settings->password,
           strnlen(settings->password, sizeof(wc.sta.password)));
    wc.sta.threshold.authmode = settings->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof(wc));
    return err;
}

// Starts, reconfigures, or stops the station from the stored credentials.
// Returns whether Wi-Fi is configured.
static bool wifi_reload(void) {
    home_settings_t settings;
    home_store_load_settings(&settings);
    bool configured = settings.ssid[0] != '\0';
    STATUS_UPDATE({ s_status.wifi_configured = configured; s_status.wifi_connected = false; });

    if (!configured) {
        if (s_wifi_started) {
            esp_wifi_disconnect();
            esp_wifi_stop();
            s_wifi_started = false;
        }
        ESP_LOGW(TAG, "Wi-Fi not configured; use the serial command: wifi \"<ssid>\" \"<password>\"");
    } else {
        esp_err_t err = wifi_init_once();
        if (err == ESP_OK && s_wifi_started) {
            esp_wifi_disconnect();
        }
        if (err == ESP_OK) err = wifi_apply(&settings);
        if (err == ESP_OK && !s_wifi_started) {
            err = esp_wifi_start();
            if (err == ESP_OK) s_wifi_started = true;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Wi-Fi station configured for \"%s\"", settings.ssid);
        }
    }
    memset(&settings, 0, sizeof(settings));
    return configured && s_wifi_started;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

// GETs `url` into s_body. Returns ESP_OK only for HTTP 200 with a complete body.
static esp_err_t http_get(const char *url) {
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 768,
        .user_agent = "ai-passport-home/1.0",
        .keep_alive_enable = false,
    };
    log_heap("before request");
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_open(client, 0);
    int total = 0;
    if (err == ESP_OK) {
        log_heap("after connect");
        int64_t length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (length < 0) {
            err = ESP_FAIL;
        } else if (status != 200) {
            ESP_LOGW(TAG, "HTTP status %d", status);
            err = ESP_FAIL;
        } else if (length >= HTTP_BODY_MAX) {
            ESP_LOGW(TAG, "response too large (%" PRId64 " bytes)", length);
            err = ESP_ERR_INVALID_SIZE;
        }
        while (err == ESP_OK && total < HTTP_BODY_MAX - 1) {
            int n = esp_http_client_read(client, s_body + total, HTTP_BODY_MAX - 1 - total);
            if (n < 0) {
                err = ESP_FAIL;
            } else if (n == 0) {
                break;
            } else {
                total += n;
            }
        }
        if (err == ESP_OK && !esp_http_client_is_complete_data_received(client)) {
            ESP_LOGW(TAG, "response truncated at %d bytes", total);
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    s_body[total < HTTP_BODY_MAX ? total : HTTP_BODY_MAX - 1] = '\0';
    if (err != ESP_OK) ESP_LOGW(TAG, "GET failed: %s", esp_err_to_name(err));
    return err;
}

// ---------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------

static int32_t local_sec_of_day(time_t t) {
    struct tm local;
    localtime_r(&t, &local);
    return local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
}

// Returns the delay in seconds until the next attempt.
static int32_t job_time_sync(void) {
    STATUS_UPDATE(s_status.sync_state = HOME_SYNC_RUNNING);
    ESP_LOGI(TAG, "syncing time from %s", CONFIG_HOME_NTP_SERVER);

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_HOME_NTP_SERVER);
    bool ok = esp_netif_sntp_init(&cfg) == ESP_OK &&
              esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_WAIT_MS)) == ESP_OK;
    esp_netif_sntp_deinit();

    time_t now = time(NULL);
    ok = ok && home_time_is_valid((int64_t)now);
    if (ok) s_rtc_time_magic = RTC_TIME_MAGIC;
    int32_t delay = home_time_sync_delay(ok, ok ? local_sec_of_day(now) : 0, CONFIG_HOME_SYNC_HOUR);
    int64_t retry_at = now_us() + (int64_t)delay * US_PER_S;
    STATUS_UPDATE({
        s_status.sync_state = ok ? HOME_SYNC_OK : HOME_SYNC_FAILED;
        if (ok) s_status.last_sync_epoch = (int64_t)now;
        s_status.retry_at_us = retry_at;
    });
    if (ok) {
        ESP_LOGI(TAG, "time synced; next sync in %" PRId32 " s", delay);
    } else {
        ESP_LOGW(TAG, "time sync failed; retrying in %" PRId32 " s", delay);
    }
    return delay;
}

static int32_t job_geolocate(void) {
    ESP_LOGI(TAG, "geolocating via %s", CONFIG_HOME_GEO_URL);
    home_location_t loc;
    if (http_get(CONFIG_HOME_GEO_URL) != ESP_OK || !home_parse_location(s_body, &loc)) {
        ESP_LOGW(TAG, "geolocation failed%s", s_cache.have_location ? "; keeping cached location" : "");
        return GEO_FAIL_DELAY_S;
    }
    s_cache.location = loc;
    s_cache.have_location = true;
    STATUS_UPDATE({
        s_status.have_location = true;
        memcpy(s_status.city, loc.city, sizeof(s_status.city));
    });
    char lat[16], lon[16];
    fmt_e4(lat, sizeof(lat), loc.lat_e4);
    fmt_e4(lon, sizeof(lon), loc.lon_e4);
    ESP_LOGI(TAG, "location: %s (%s, %s)", loc.city, lat, lon);
    (void)home_store_save_cache(&s_cache);
    return GEO_OK_DELAY_S;
}

static int32_t job_weather(void) {
    if (!s_cache.have_location) return home_weather_delay(false, CONFIG_HOME_WEATHER_REFRESH_MIN);

    char lat[16], lon[16], url[320];
    fmt_e4(lat, sizeof(lat), s_cache.location.lat_e4);
    fmt_e4(lon, sizeof(lon), s_cache.location.lon_e4);
    snprintf(url, sizeof(url),
             "%s/v1/forecast?latitude=%s&longitude=%s"
             "&current=temperature_2m,weather_code,is_day"
             "&daily=temperature_2m_max,temperature_2m_min"
             "&timezone=auto&forecast_days=1",
             CONFIG_HOME_WEATHER_BASE_URL, lat, lon);

    home_weather_t w;
    bool ok = http_get(url) == ESP_OK && home_parse_weather(s_body, &w);
    if (ok) {
        time_t now = time(NULL);
        s_cache.weather = w;
        s_cache.have_weather = true;
        s_cache.weather_epoch = home_net_time_valid() ? (int64_t)now : 0;
        int64_t at = now_us();
        STATUS_UPDATE({
            s_status.weather = w;
            s_status.have_weather = true;
            s_status.weather_this_boot = true;
            s_status.weather_at_us = at;
        });
        ESP_LOGI(TAG, "weather: %d.%d C code %d, H %d L %d", w.temp_c10 / 10,
                 (w.temp_c10 < 0 ? -w.temp_c10 : w.temp_c10) % 10, w.wmo,
                 home_round_c10(w.high_c10), home_round_c10(w.low_c10));
        (void)home_store_save_cache(&s_cache);
    }
    return home_weather_delay(ok, CONFIG_HOME_WEATHER_REFRESH_MIN);
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

static TickType_t ticks_until(int64_t due_us, int64_t now) {
    int64_t ms = (due_us - now) / 1000;
    if (ms < 0) ms = 0;
    if (ms > 60000) ms = 60000;  // wake at least once a minute
    return pdMS_TO_TICKS(ms);
}

static void net_task(void *arg) {
    (void)arg;
    bool wifi_on = wifi_reload();
    bool connected = false;
    int backoff_s = CONNECT_BACKOFF_MIN_S;
    int64_t connect_due = INT64_MAX;  // set on STA_START
    int64_t time_due = 0, geo_due = 0, weather_due = 0;

    for (;;) {
        int64_t now = now_us();
        int64_t next = INT64_MAX;
        if (wifi_on && !connected) next = connect_due;
        if (connected) {
            next = time_due;
            if (geo_due < next) next = geo_due;
            if (weather_due < next) next = weather_due;
        }
        EventBits_t bits = xEventGroupWaitBits(s_events, EV_ALL, pdTRUE, pdFALSE,
                                               ticks_until(next, now));
        now = now_us();

        if (bits & EV_WIFI_CHANGED) {
            bool was_started = s_wifi_started;
            connected = false;
            wifi_on = wifi_reload();
            backoff_s = CONNECT_BACKOFF_MIN_S;
            // A freshly started station connects on its STA_START event; an
            // already running one was just disconnected and reconnects now.
            connect_due = was_started ? now : INT64_MAX;
            time_due = geo_due = weather_due = 0;
            // A disconnect caused by the reload must not push the reconnect out.
            bits &= ~EV_DISCONNECTED;
        }
        if (bits & EV_STA_START) connect_due = now;
        if (bits & EV_DISCONNECTED) {
            if (connected) ESP_LOGW(TAG, "Wi-Fi disconnected");
            connected = false;
            connect_due = now + (int64_t)backoff_s * US_PER_S;
            backoff_s = backoff_s * 2 > CONNECT_BACKOFF_MAX_S ? CONNECT_BACKOFF_MAX_S : backoff_s * 2;
        }
        if (bits & EV_GOT_IP) {
            ESP_LOGI(TAG, "Wi-Fi connected");
            connected = true;
            backoff_s = CONNECT_BACKOFF_MIN_S;
            // Back online: refresh stale weather right away.
            home_status_t st;
            home_net_get_status(&st);
            if (!st.weather_this_boot ||
                now - st.weather_at_us > (int64_t)CONFIG_HOME_WEATHER_REFRESH_MIN * 60 * US_PER_S) {
                weather_due = now;
            }
        }
        if (bits & EV_SYNC_NOW) {
            ESP_LOGI(TAG, "sync requested");
            time_due = geo_due = weather_due = now;
            if (wifi_on && !connected) {
                backoff_s = CONNECT_BACKOFF_MIN_S;
                connect_due = now;
            }
        }

        if (wifi_on && !connected && now >= connect_due) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) ESP_LOGD(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
            connect_due = now + (int64_t)CONNECT_WATCHDOG_S * US_PER_S;
        }
        if (!connected) continue;

        if (now_us() >= time_due) {
            time_due = now_us() + (int64_t)job_time_sync() * US_PER_S;
        }
        if (now_us() >= geo_due) {
            geo_due = now_us() + (int64_t)job_geolocate() * US_PER_S;
        }
        if (now_us() >= weather_due) {
            weather_due = now_us() + (int64_t)job_weather() * US_PER_S;
            log_heap("after weather");
        }
    }
}

esp_err_t home_net_start(void) {
    if (s_events) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (!s_lock || !s_events) return ESP_ERR_NO_MEM;

    if (home_store_load_cache(&s_cache)) {
        s_status.have_weather = s_cache.have_weather;
        s_status.weather = s_cache.weather;
        s_status.have_location = s_cache.have_location;
        memcpy(s_status.city, s_cache.location.city, sizeof(s_status.city));
        ESP_LOGI(TAG, "loaded cached weather/location (%s)", s_cache.location.city);
    }
    // The RTC keeps time across a soft reset (not across power loss).
    if (home_net_time_valid()) {
        ESP_LOGI(TAG, "wall clock already valid after reset; still syncing at boot");
    }

    if (xTaskCreate(net_task, "home_net", NET_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
