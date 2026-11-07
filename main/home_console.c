// main/home_console.c - see home_console.h.
#include "home_console.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "home_logic.h"
#include "home_net.h"
#include "home_store.h"
#include "sdkconfig.h"

static const char *TAG = "home_console";

#define LINE_MAX_LEN 200

static const char *sync_state_name(home_sync_state_t s) {
    switch (s) {
    case HOME_SYNC_RUNNING: return "syncing";
    case HOME_SYNC_OK: return "ok";
    case HOME_SYNC_FAILED: return "failed";
    default: return "not yet";
    }
}

static void print_help(void) {
    printf("\nHome screen console commands:\n"
           "  wifi \"<ssid>\" \"<password>\"  save Wi-Fi (quotes needed for spaces)\n"
           "  wifi \"<ssid>\"               save an open network\n"
           "  wifi clear                  forget Wi-Fi\n"
           "  tz <POSIX TZ>               e.g. tz <+08>-8   (UTC+8, Kuala Lumpur)\n"
           "  tz default                  back to %s\n"
           "  clock 12 | clock 24         clock format\n"
           "  sync                        sync time and weather now\n"
           "  status                      show settings and state\n\n",
           CONFIG_HOME_TZ_DEFAULT);
}

static void print_status(void) {
    home_settings_t settings;
    home_store_load_settings(&settings);
    home_status_t st;
    home_net_get_status(&st);

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char when[32] = "not set";
    if (home_net_time_valid()) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);

    printf("\nWi-Fi    : %s%s%s (password %s)\n",
           settings.ssid[0] ? "\"" : "", settings.ssid[0] ? settings.ssid : "not configured",
           settings.ssid[0] ? "\"" : "", settings.password[0] ? "set, hidden" : "none");
    printf("Network  : %s\n", st.wifi_connected ? "connected" : "offline");
    printf("Timezone : %s\n", settings.tz);
    printf("Clock    : %s-hour\n", settings.twelve_hour ? "12" : "24");
    printf("Time     : %s (sync %s)\n", when, sync_state_name(st.sync_state));
    if (st.have_weather) {
        printf("Weather  : %d C, code %d, %s%s\n", home_round_c10(st.weather.temp_c10), st.weather.wmo,
               st.city[0] ? st.city : "unknown city", st.weather_this_boot ? "" : " (cached)");
    } else {
        printf("Weather  : none yet\n");
    }
    printf("Heap     : free %u, largest block %u, minimum ever %u\n\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    memset(&settings, 0, sizeof(settings));
}

static void handle_line(const char *line) {
    home_cmd_t cmd;
    home_cmd_parse(line, &cmd);
    esp_err_t err = ESP_OK;

    switch (cmd.type) {
    case HOME_CMD_NONE:
        break;
    case HOME_CMD_HELP:
        print_help();
        break;
    case HOME_CMD_STATUS:
        print_status();
        break;
    case HOME_CMD_SYNC:
        home_net_sync_now();
        printf("Sync requested.\n");
        break;
    case HOME_CMD_WIFI_SET:
        err = home_store_save_wifi(cmd.ssid, cmd.password);
        if (err == ESP_OK) {
            printf("Saved Wi-Fi \"%s\" (password hidden). Connecting...\n", cmd.ssid);
            home_net_wifi_changed();
        }
        break;
    case HOME_CMD_WIFI_CLEAR:
        err = home_store_save_wifi("", "");
        if (err == ESP_OK) {
            printf("Wi-Fi credentials erased.\n");
            home_net_wifi_changed();
        }
        break;
    case HOME_CMD_TZ:
        err = home_store_save_tz(cmd.tz);
        if (err == ESP_OK) {
            home_apply_tz(cmd.tz);
            printf("Timezone set to %s.\n", cmd.tz);
        }
        break;
    case HOME_CMD_TZ_RESET:
        err = home_store_save_tz(NULL);
        if (err == ESP_OK) {
            home_apply_tz(CONFIG_HOME_TZ_DEFAULT);
            printf("Timezone reset to %s.\n", CONFIG_HOME_TZ_DEFAULT);
        }
        break;
    case HOME_CMD_CLOCK:
        err = home_store_save_clock(cmd.twelve_hour);
        if (err == ESP_OK) printf("Clock set to %s-hour.\n", cmd.twelve_hour ? "12" : "24");
        break;
    case HOME_CMD_ERROR:
    default:
        printf("Error: %s\n", cmd.error ? cmd.error : "invalid command");
        break;
    }
    if (err != ESP_OK) printf("Error: could not save setting (%s)\n", esp_err_to_name(err));
    memset(&cmd, 0, sizeof(cmd));  // drop the password copy
}

static void console_task(void *arg) {
    (void)arg;
    char line[LINE_MAX_LEN + 1];
    size_t len = 0;
    bool overflow = false;
    uint8_t buf[32];

    printf("\nHome screen ready. Type 'help' and press Enter for settings commands.\n");
    for (;;) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r' || c == '\n') {
                if (overflow) {
                    printf("Error: line too long\n");
                } else if (len > 0) {
                    line[len] = '\0';
                    handle_line(line);
                }
                memset(line, 0, sizeof(line));
                len = 0;
                overflow = false;
            } else if (c == '\b' || c == 0x7F) {
                if (len > 0) len--;
            } else if (len < LINE_MAX_LEN) {
                line[len++] = c;
            } else {
                overflow = true;
            }
        }
        memset(buf, 0, sizeof(buf));
    }
}

esp_err_t home_console_start(void) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB-Serial-JTAG driver install failed: %s", esp_err_to_name(err));
        return err;
    }
    // Route console output through the driver too, so logs and command replies
    // do not race the driver for the hardware FIFO.
    usb_serial_jtag_vfs_use_driver();
    if (xTaskCreate(console_task, "home_console", 4096, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
