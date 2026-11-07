// main/main.c - AI Passport home screen: startup and key dispatch.
//
// Keys (see home_nav_handle in home_logic.c):
//   Home        UP = Apps, OK = Focus, DOWN = sync time and weather now
//   Apps/Focus  OK = back to Home
//
// Button callbacks only enqueue. The input task maps keys to actions, switches
// screens under the LVGL lock, and polls the battery gauge every 15 s. Network
// work runs in home_net; serial provisioning in home_console.
#include <string.h>

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "home_console.h"
#include "home_logic.h"
#include "home_net.h"
#include "home_store.h"
#include "home_ui.h"
#include "key_guard.h"
#include "lvgl.h"

static const char *TAG = "main";

#define INPUT_QUEUE_DEPTH  8
#define BATTERY_POLL_US    (15LL * 1000 * 1000)

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
static bool s_battery_ok;
static home_page_t s_page = HOME_PAGE_HOME;   // input task only

// A deep-sleep key wake reboots with the key still held; drop key actions until
// it is released (key_guard.h). This firmware does not deep sleep yet, but the
// guard keeps a held key at power-on from firing an action as well.
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

static home_key_ev_t map_event(bsp_btn_ev_t ev) {
    switch (ev) {
    case BSP_BTN_CLICK:
    case BSP_BTN_DOUBLE:   // quick taps are merged into a double click; still a tap
        return HOME_KEY_EV_CLICK;
    case BSP_BTN_LONG:
        return HOME_KEY_EV_LONG;
    default:
        return HOME_KEY_EV_OTHER;
    }
}

static home_key_t map_key(bsp_btn_t btn) {
    if (btn == BSP_BTN_UP) return HOME_KEY_UP;
    if (btn == BSP_BTN_DOWN) return HOME_KEY_DOWN;
    return HOME_KEY_OK;
}

static void process_input(const input_event_t *input) {
    home_nav_result_t r = home_nav_handle(s_page, map_key(input->btn), map_event(input->event));
    if (r.sync_now) home_net_sync_now();
    if (r.page_changed && bsp_lvgl_lock(500)) {
        home_ui_show(r.page);
        bsp_lvgl_unlock();
        s_page = r.page;
    }
}

static void poll_battery(void) {
    home_ui_set_battery(s_battery_ok ? bsp_battery_soc() : -1);
}

static void input_task(void *arg) {
    (void)arg;
    int64_t next_battery = 0;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, pdMS_TO_TICKS(1000)) == pdTRUE) {
            process_input(&input);
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_battery) {
            poll_battery();
            next_battery = now + BATTERY_POLL_US;
        }
    }
}

// Button callbacks run on the shared esp_timer task; enqueue only.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    // Consume the guard before the readiness check so an early release still
    // disarms it instead of leaving it armed for the next real press.
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) ESP_LOGI(TAG, "wake key released; keys active");
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static void log_memory(const char *when) {
    lv_mem_monitor_t mon;
    if (bsp_lvgl_lock(500)) {
        lv_mem_monitor(&mon);
        bsp_lvgl_unlock();
        ESP_LOGI(TAG, "LVGL pool %s: total=%u used=%u%% max_used=%u largest_free=%u", when,
                 (unsigned)mon.total_size, (unsigned)mon.used_pct, (unsigned)mon.max_used,
                 (unsigned)mon.free_biggest_size);
    }
    ESP_LOGI(TAG, "heap %s: free=%u largest=%u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void app_main(void) {
    ESP_LOGI(TAG, "AI Passport home screen starting");
    if (esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    }

    bsp_i2c_init();

    esp_err_t err = home_store_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS unavailable (%s); settings and cache disabled", esp_err_to_name(err));
    home_settings_t settings;
    home_store_load_settings(&settings);
    home_apply_tz(settings.tz);
    ESP_LOGI(TAG, "timezone %s, %s-hour clock, Wi-Fi %s", settings.tz,
             settings.twelve_hour ? "12" : "24", settings.ssid[0] ? "configured" : "not configured");
    memset(&settings, 0, sizeof(settings));

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }

    s_battery_ok = bsp_battery_init() == ESP_OK;
    if (!s_battery_ok) ESP_LOGW(TAG, "CW2017 gauge not found; battery shown as unavailable");

    // Start networking before the UI so the cached weather is ready for the first frame.
    err = home_net_start();
    if (err != ESP_OK) ESP_LOGE(TAG, "network worker failed to start: %s", esp_err_to_name(err));

    if (bsp_lvgl_lock(1000)) {
        home_ui_create();
        home_ui_show(HOME_PAGE_HOME);
        bsp_lvgl_unlock();
    }
    bsp_display_backlight(80);

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue ||
        xTaskCreate(input_task, "home_input", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "input task creation failed; keys disabled");
    } else if ((err = bsp_button_init(on_key, NULL)) != ESP_OK) {
        ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(err));
    } else {
        if (s_key_guard.armed) {
            // If the wake key was released before the driver started, no release
            // event will come; sync with the real level once.
            key_guard_sync_released(&s_key_guard, bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN);
        }
        s_input_ready = true;
    }

    err = home_console_start();
    if (err != ESP_OK) ESP_LOGW(TAG, "serial console unavailable: %s", esp_err_to_name(err));

    log_memory("after boot");
}
