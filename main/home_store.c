// main/home_store.c - see home_store.h.
#include "home_store.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "demo_radio.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#define NS "home"

// A disabled Kconfig bool leaves its macro undefined.
#ifdef CONFIG_HOME_CLOCK_12H_DEFAULT
#define CLOCK_12H_DEFAULT true
#else
#define CLOCK_12H_DEFAULT false
#endif

static const char *TAG = "home_store";
static volatile bool s_twelve_hour = CLOCK_12H_DEFAULT;

esp_err_t home_store_init(void) {
    return demo_radio_nvs_prepare();
}

static void load_str(nvs_handle_t h, const char *key, char *out, size_t size) {
    size_t len = size;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = '\0';
}

void home_store_load_settings(home_settings_t *out) {
    memset(out, 0, sizeof(*out));
    strlcpy(out->tz, CONFIG_HOME_TZ_DEFAULT, sizeof(out->tz));
    out->twelve_hour = CLOCK_12H_DEFAULT;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        load_str(h, "ssid", out->ssid, sizeof(out->ssid));
        load_str(h, "pass", out->password, sizeof(out->password));
        char tz[sizeof(out->tz)];
        load_str(h, "tz", tz, sizeof(tz));
        if (tz[0]) strlcpy(out->tz, tz, sizeof(out->tz));
        uint8_t clk12;
        if (nvs_get_u8(h, "clk12", &clk12) == ESP_OK) out->twelve_hour = clk12 != 0;
        nvs_close(h);
    }
    s_twelve_hour = out->twelve_hour;
}

bool home_store_load_cache(home_cache_t *out) {
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    home_cache_blob_t blob;
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, "cache", &blob, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(blob)) return false;
    if (!home_cache_decode(&blob, out)) {
        ESP_LOGW(TAG, "cached weather is invalid or from another version; ignoring it");
        return false;
    }
    return true;
}

static esp_err_t write_and_commit(const char *key, esp_err_t (*op)(nvs_handle_t, const char *, const void *),
                                  const void *arg) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = op(h, key, arg);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;  // erasing a missing key
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS write %s failed: %s", key, esp_err_to_name(err));
    return err;
}

static esp_err_t op_set_blob(nvs_handle_t h, const char *key, const void *arg) {
    return nvs_set_blob(h, key, arg, sizeof(home_cache_blob_t));
}

static esp_err_t op_set_str(nvs_handle_t h, const char *key, const void *arg) {
    return nvs_set_str(h, key, (const char *)arg);
}

static esp_err_t op_erase(nvs_handle_t h, const char *key, const void *arg) {
    (void)arg;
    return nvs_erase_key(h, key);
}

static esp_err_t op_set_u8(nvs_handle_t h, const char *key, const void *arg) {
    return nvs_set_u8(h, key, *(const uint8_t *)arg);
}

esp_err_t home_store_save_cache(const home_cache_t *cache) {
    home_cache_blob_t blob;
    home_cache_encode(cache, &blob);
    return write_and_commit("cache", op_set_blob, &blob);
}

esp_err_t home_store_save_wifi(const char *ssid, const char *password) {
    if (!ssid || !ssid[0]) {
        esp_err_t err = write_and_commit("ssid", op_erase, NULL);
        esp_err_t err2 = write_and_commit("pass", op_erase, NULL);
        return err != ESP_OK ? err : err2;
    }
    esp_err_t err = write_and_commit("ssid", op_set_str, ssid);
    if (err == ESP_OK) err = write_and_commit("pass", op_set_str, password ? password : "");
    return err;
}

esp_err_t home_store_save_tz(const char *tz) {
    if (!tz) return write_and_commit("tz", op_erase, NULL);
    return write_and_commit("tz", op_set_str, tz);
}

esp_err_t home_store_save_clock(bool twelve_hour) {
    uint8_t value = twelve_hour ? 1 : 0;
    esp_err_t err = write_and_commit("clk12", op_set_u8, &value);
    if (err == ESP_OK) s_twelve_hour = twelve_hour;
    return err;
}

void home_apply_tz(const char *tz) {
    setenv("TZ", tz, 1);
    tzset();
}

bool home_twelve_hour(void) {
    return s_twelve_hour;
}
