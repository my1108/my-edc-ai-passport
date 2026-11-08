// main/main.c - AI Passport home screen: startup and key dispatch.
//
// Keys (see home_nav_handle in home_logic.c):
//   Home        UP = Apps tray, OK = Setup, DOWN = sync time and weather now
//   Apps tray   UP / DOWN = move (clamped), OK = open, hold OK = back to Home
//   An app      its own keys (home_apps.c); hold OK = back to the tray.
//               Spotify: UP = previous, DOWN = next, OK = play/pause.
//               An app's screen exists only while it is open (home_apps.h).
//   Setup       UP / DOWN = previous / next QR card, OK = back to Home.
//               Opening Setup starts the setup hotspot and web page
//               (setup_portal.c); leaving it stops them. Setup opens at boot
//               when no Wi-Fi is saved, keeps the screen awake, and closes
//               itself after SETUP_IDLE_US without a key or page activity.
//   The screen sleeps after CONFIG_HOME_SCREEN_SLEEP_S without a key; the key
//   that wakes it does nothing else (home_idle_key in home_logic.c).
//   After CONFIG_HOME_DEEP_SLEEP_MIN without a key the board deep-sleeps
//   ("off"): any function key wakes it and the app restarts; key_guard drops
//   that key until it is released. See enter_deep_sleep().
//
// Button callbacks only enqueue. The input task maps keys to actions, switches
// screens under the LVGL lock, and polls the battery gauge every 15 s. Network
// work runs in home_net; serial provisioning in home_console.
#include <string.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "home_apps.h"
#include "home_console.h"
#include "home_logic.h"
#include "home_net.h"
#include "home_store.h"
#include "home_ui.h"
#include "key_guard.h"
#include "lvgl.h"
#include "setup_portal.h"
#include "spot_net.h"
#include "sdkconfig.h"

static const char *TAG = "main";

#define INPUT_QUEUE_DEPTH  8
#define BATTERY_POLL_US    (15LL * 1000 * 1000)
#define SCREEN_SLEEP_US    ((int64_t)CONFIG_HOME_SCREEN_SLEEP_S * 1000 * 1000)
#define BACKLIGHT_ON       80
#define SETUP_IDLE_US      (15LL * 60 * 1000 * 1000)
#define DEEP_SLEEP_US      ((int64_t)CONFIG_HOME_DEEP_SLEEP_MIN * 60 * 1000 * 1000)

// A disabled Kconfig bool leaves its macro undefined.
#ifdef CONFIG_HOME_DEEP_SLEEP_STAY_ON_USB
#define DEEP_SLEEP_STAY_ON_USB true
#else
#define DEEP_SLEEP_STAY_ON_USB false
#endif

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
static bool s_battery_ok;
static home_page_t s_page = HOME_PAGE_HOME;   // input task only
static home_idle_t s_idle;                    // input task only
static bool s_awake = true;                   // input task only
static int64_t s_setup_activity_us;           // input task only
static int s_tray_sel;                        // input task only: tray selection
static int s_app = -1;                        // input task only: open app, -1 none

static const home_app_t *open_app(void) {
    return s_app >= 0 && s_app < HOME_APP_COUNT ? &HOME_APPS[s_app] : NULL;
}

// An app runs its services (e.g. Spotify polling) only while its screen is lit.
static void app_set_active(bool active) {
    const home_app_t *app = open_app();
    if (app && app->set_active) app->set_active(active);
}

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

// Screen sleep turns the backlight off and pauses the UI refresh; time, Wi-Fi,
// and weather keep running so the screen is current when it wakes.
static void screen_set_awake(bool awake) {
    s_awake = awake;
    app_set_active(awake && s_page == HOME_PAGE_APP);
    if (bsp_lvgl_lock(500)) {
        home_ui_set_awake(awake);
        bsp_lvgl_unlock();
    }
    bsp_display_backlight(awake ? BACKLIGHT_ON : 0);
    ESP_LOGI(TAG, "screen %s", awake ? "on" : "asleep");
}

// Switches pages, starting and stopping the setup portal with its page.
// Input task only (or app_main before the input task starts).
static void go_to_page(home_page_t page) {
    if (page == s_page) return;
    if (page == HOME_PAGE_APP && (s_tray_sel < 0 || s_tray_sel >= HOME_APP_COUNT)) return;
    setup_info_t info;
    memset(&info, 0, sizeof(info));
    // Leaving an app: stop its services before its screen is deleted.
    bool leaving_app = s_page == HOME_PAGE_APP;
    if (leaving_app) app_set_active(false);
    if (s_page == HOME_PAGE_SETUP) setup_portal_stop();
    if (page == HOME_PAGE_SETUP) {
        esp_err_t err = setup_portal_start(&info);
        if (err != ESP_OK) ESP_LOGE(TAG, "setup portal failed: %s", esp_err_to_name(err));
        s_setup_activity_us = esp_timer_get_time();
    }
    if (bsp_lvgl_lock(2000)) {
        if (page == HOME_PAGE_SETUP) home_ui_setup_open(&info);
        if (leaving_app) {
            home_ui_app_close(page);
            s_app = -1;
        }
        if (page == HOME_PAGE_APP) {
            s_app = s_tray_sel;
            home_ui_app_open(&HOME_APPS[s_app]);
        } else if (!leaving_app) {
            home_ui_show(page);
        }
        s_page = page;
        bsp_lvgl_unlock();
        if (page == HOME_PAGE_APP) {
            ESP_LOGI(TAG, "opened %s", HOME_APPS[s_app].name);
            app_set_active(s_awake);
        }
    } else {
        // Without the lock nothing was built or deleted; stay where we were.
        ESP_LOGE(TAG, "LVGL busy; page change dropped");
        if (leaving_app) app_set_active(s_awake);
    }
    memset(&info, 0, sizeof(info));
}

static void process_input(const input_event_t *input) {
    home_idle_result_t idle = home_idle_key(&s_idle, input->event == BSP_BTN_PRESS,
                                            input->event == BSP_BTN_RELEASE, esp_timer_get_time());
    if (idle.woke) screen_set_awake(true);
    if (!idle.deliver) return;   // the waking key only wakes the screen

    home_nav_result_t r = home_nav_handle(s_page, map_key(input->btn), map_event(input->event));
    if (s_page == HOME_PAGE_SETUP) s_setup_activity_us = esp_timer_get_time();
    if (r.sync_now) home_net_sync_now();
    if (r.app_key) {
        const home_app_t *app = open_app();
        if (app && app->key) app->key(map_key(input->btn), map_event(input->event));
    }
    if (r.tray_step) {
        s_tray_sel = home_tray_move(s_tray_sel, r.tray_step, HOME_APP_COUNT);
        if (bsp_lvgl_lock(500)) {
            home_ui_tray_select(s_tray_sel);
            bsp_lvgl_unlock();
        }
    }
    if (r.setup_step && bsp_lvgl_lock(500)) {
        home_ui_setup_step(r.setup_step);
        bsp_lvgl_unlock();
    }
    if (r.page_changed) go_to_page(r.page);
}

static void poll_battery(void) {
    home_ui_set_battery(s_battery_ok ? bsp_battery_soc() : -1);
}

static void deep_sleep_step(const char *step, esp_err_t err) {
    if (err != ESP_OK) ESP_LOGW(TAG, "deep sleep: %s failed (%s); continuing", step, esp_err_to_name(err));
}

// Deep sleep after a long idle time. Same order as the BSP reference
// (main/demo_low_power.c): hand the key pin back and arm it as the wake source,
// then CW2017 -> ES8311 -> I2S -> shared I2C -> LCD. The key and bus releases
// are terminal: if anything after them fails, restart rather than run on
// without buttons or a display. Wake restarts the app with normal init.
static void enter_deep_sleep(void) {
    // A held key would wake the board at once (low-level wake); try later.
    if (bsp_button_read_mv() < BSP_BTN_MV_RELEASED_MIN) {
        s_idle.last_activity_us = esp_timer_get_time();
        return;
    }
    ESP_LOGI(TAG, "idle for %d min: deep sleep (any key wakes)", CONFIG_HOME_DEEP_SLEEP_MIN);
    s_input_ready = false;
    app_set_active(false);
    spot_set_active(false);
    // Leave the network cleanly; RF is powered down in deep sleep anyway.
    esp_wifi_disconnect();
    esp_wifi_stop();

    int level = 0;
    if (bsp_button_prepare_deep_sleep(&level) != ESP_OK) {
        ESP_LOGE(TAG, "deep sleep: key hand-off failed; restarting");
        esp_restart();
    }
    if (level != 1) ESP_LOGW(TAG, "deep sleep: key pressed while sleeping; it will wake at once");
    deep_sleep_step("GPIO wake arm", esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW));
    // The gauge and the codec share I2C: suspend both before releasing the bus.
    if (s_battery_ok) deep_sleep_step("CW2017 suspend", bsp_battery_sleep());
    deep_sleep_step("ES8311 suspend", bsp_audio_sleep());
    deep_sleep_step("I2S pin release", bsp_audio_prepare_deep_sleep());
    deep_sleep_step("shared I2C pin release", bsp_i2c_prepare_deep_sleep());
    // Wait for any LVGL flush to finish and keep LVGL off the panel from now on.
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "deep sleep: LVGL busy; restarting");
        esp_restart();
    }
    deep_sleep_step("ST7789 suspend", bsp_display_prepare_deep_sleep());
    esp_deep_sleep_start();
    ESP_LOGE(TAG, "deep sleep returned; restarting");
    esp_restart();
}

static void input_task(void *arg) {
    (void)arg;
    int64_t next_battery = 0;
    input_event_t input;
    home_idle_init(&s_idle, esp_timer_get_time());
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, pdMS_TO_TICKS(1000)) == pdTRUE) {
            process_input(&input);
        }
        int64_t now = esp_timer_get_time();
        // Keep the QR code visible while Setup is open.
        int64_t sleep_us = s_page == HOME_PAGE_SETUP ? 0 : SCREEN_SLEEP_US;
        if (home_idle_tick(&s_idle, now, sleep_us)) screen_set_awake(false);
        if (home_deep_sleep_due(now - s_idle.last_activity_us, DEEP_SLEEP_US, s_page == HOME_PAGE_SETUP,
                                usb_serial_jtag_is_connected(), DEEP_SLEEP_STAY_ON_USB)) {
            enter_deep_sleep();
        }
        if (s_page == HOME_PAGE_SETUP) {
            int64_t event_at = 0;
            if (setup_portal_event(&event_at) && event_at > s_setup_activity_us) s_setup_activity_us = event_at;
            if (now - s_setup_activity_us > SETUP_IDLE_US) {
                ESP_LOGI(TAG, "setup idle; closing it");
                go_to_page(HOME_PAGE_HOME);
            }
        }
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
    bool need_setup = settings.ssid[0] == '\0';
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
    // First start (no Wi-Fi saved): go straight to the phone setup page.
    if (need_setup) go_to_page(HOME_PAGE_SETUP);
    bsp_display_backlight(BACKLIGHT_ON);

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
