// main/home_ui.c - see home_ui.h.
#include "home_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "home_layout.h"
#include "home_net.h"
#include "home_store.h"
#include "lvgl.h"
#include "sdkconfig.h"

// Palette
#define C_BG       0x0E1116
#define C_TEXT     0xE8ECF1
#define C_MUTED    0x9AA4B2
#define C_ACCENT   0x4CC38A
#define C_WARN     0xF0A04B
#define C_CARD     0x171C24
#define C_HINT_BG  0x141921
#define C_DIVIDER  0x232A34
#define C_DIM      0x4A5260
#define C_SUN      0xF5C451
#define C_MOON     0xE8D9A8
#define C_CLOUD    0xD6DDE6
#define C_RAIN     0x5AA9E6
#define C_SNOW     0xF4F7FA

#define REFRESH_MS 250

typedef struct {
    lv_obj_t *sun;
    lv_obj_t *moon;
    lv_obj_t *moon_cut;      // card-coloured disc that turns the moon into a crescent
    lv_obj_t *cloud[3];      // base + two puffs
    lv_obj_t *drop[3];       // rain drops / snow flakes
    lv_obj_t *fog[2];
    lv_obj_t *bolt;          // LV_SYMBOL_CHARGE label
} weather_icon_t;

static lv_obj_t *s_screens[3];
static lv_timer_t *s_refresh_timer;

// Home screen widgets
static lv_obj_t *s_wifi;
static lv_obj_t *s_battery;
static lv_obj_t *s_date;
static lv_obj_t *s_clock;
static lv_obj_t *s_seconds;
static lv_obj_t *s_ampm;
static lv_obj_t *s_temp;
static lv_obj_t *s_condition;
static lv_obj_t *s_pin;
static lv_obj_t *s_location;
static lv_obj_t *s_sync;
static weather_icon_t s_icon;

static volatile int s_battery_soc = -1;

// Last drawn values, so labels are only invalidated when they change.
typedef struct {
    char date[24];
    char clock[8];
    char seconds[4];
    char ampm[4];
    char battery[24];
    char temp[12];
    char condition[24];
    char location[64];
    char sync[48];
    int wifi_state;      // -1 unknown, 0 offline, 1 online
    int battery_color;
    int sky;             // home_sky_t, -1 none
    int weather_fresh;
    bool sync_warning;
} ui_cache_t;

static ui_cache_t s_drawn;

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

static lv_obj_t *make_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *make_box(lv_obj_t *parent, uint32_t color, int32_t radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void set_box(lv_obj_t *o, int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
}

// Bottom bar with three columns: a key name (accent) over its action (text).
static void build_hints(lv_obj_t *scr, const char *const keys[3], const char *const actions[3]) {
    lv_obj_t *bar = make_box(scr, C_HINT_BG, 0);
    set_box(bar, 0, HOME_HINTS_Y, HOME_SCREEN_W, HOME_HINTS_H);
    lv_obj_set_style_border_color(bar, lv_color_hex(C_DIVIDER), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);

    for (int i = 0; i < HOME_HINT_COLS; i++) {
        if (!keys[i]) continue;
        lv_obj_t *key = make_label(bar, &lv_font_montserrat_14, C_ACCENT);
        lv_label_set_text(key, keys[i]);
        lv_obj_set_width(key, HOME_HINT_COL_W);
        lv_obj_set_style_text_align(key, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(key, i * HOME_HINT_COL_W, 4);

        lv_obj_t *act = make_label(bar, &lv_font_montserrat_14, C_TEXT);
        lv_label_set_text(act, actions[i]);
        lv_obj_set_width(act, HOME_HINT_COL_W);
        lv_obj_set_style_text_align(act, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(act, i * HOME_HINT_COL_W, 22);
    }
}

static void build_icon(lv_obj_t *card) {
    lv_obj_t *box = lv_obj_create(card);
    lv_obj_remove_style_all(box);
    set_box(box, 12, 10, HOME_ICON_BOX, HOME_ICON_BOX);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Creation order is z-order: sky bodies, then clouds over them, then precipitation.
    s_icon.sun = make_box(box, C_SUN, LV_RADIUS_CIRCLE);
    s_icon.moon = make_box(box, C_MOON, LV_RADIUS_CIRCLE);
    s_icon.moon_cut = make_box(box, C_CARD, LV_RADIUS_CIRCLE);
    s_icon.cloud[0] = make_box(box, C_CLOUD, 8);
    s_icon.cloud[1] = make_box(box, C_CLOUD, LV_RADIUS_CIRCLE);
    s_icon.cloud[2] = make_box(box, C_CLOUD, LV_RADIUS_CIRCLE);
    for (int i = 0; i < 3; i++) s_icon.drop[i] = make_box(box, C_RAIN, 2);
    for (int i = 0; i < 2; i++) s_icon.fog[i] = make_box(box, C_CLOUD, 2);
    s_icon.bolt = make_label(box, &lv_font_montserrat_20, C_SUN);
    lv_label_set_text(s_icon.bolt, LV_SYMBOL_CHARGE);
}

static void build_home(void) {
    lv_obj_t *scr = s_screens[HOME_PAGE_HOME] = make_screen();

    // Status bar
    s_wifi = make_label(scr, &lv_font_montserrat_14, C_DIM);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_pos(s_wifi, HOME_GUTTER + 4, HOME_STATUS_TEXT_Y);
    s_battery = make_label(scr, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -(HOME_GUTTER + 4), HOME_STATUS_TEXT_Y);
    lv_obj_t *rule = make_box(scr, C_DIVIDER, 0);
    set_box(rule, HOME_GUTTER, HOME_STATUS_H, HOME_SCREEN_W - 2 * HOME_GUTTER, 1);

    // Date and clock
    s_date = make_label(scr, &lv_font_montserrat_14, C_ACCENT);
    lv_obj_set_pos(s_date, HOME_GUTTER, HOME_DATE_Y);
    s_clock = make_label(scr, &lv_font_montserrat_48, C_TEXT);
    lv_obj_set_pos(s_clock, HOME_GUTTER - 2, HOME_CLOCK_Y);
    s_seconds = make_label(scr, &lv_font_montserrat_20, C_MUTED);
    s_ampm = make_label(scr, &lv_font_montserrat_14, C_MUTED);

    // Weather card
    lv_obj_t *card = make_box(scr, C_CARD, 16);
    set_box(card, HOME_CARD_X, HOME_CARD_Y, HOME_CARD_W, HOME_CARD_H);
    build_icon(card);
    s_temp = make_label(card, &lv_font_montserrat_28, C_TEXT);
    lv_obj_set_pos(s_temp, 84, 12);
    s_condition = make_label(card, &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_pos(s_condition, 84, 46);
    lv_obj_set_width(s_condition, HOME_CARD_W - 84 - 10);
    lv_label_set_long_mode(s_condition, LV_LABEL_LONG_DOT);
    s_pin = make_label(card, &lv_font_montserrat_14, C_MUTED);
    lv_label_set_text(s_pin, LV_SYMBOL_GPS);
    lv_obj_set_pos(s_pin, 14, HOME_CARD_H - 24);
    s_location = make_label(card, &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(s_location, 34, HOME_CARD_H - 24);
    lv_obj_set_width(s_location, HOME_CARD_W - 34 - 10);
    lv_label_set_long_mode(s_location, LV_LABEL_LONG_DOT);

    // Sync status
    s_sync = make_label(scr, &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(s_sync, HOME_GUTTER, HOME_SYNC_Y);
    lv_obj_set_width(s_sync, HOME_SCREEN_W - 2 * HOME_GUTTER);
    lv_label_set_long_mode(s_sync, LV_LABEL_LONG_DOT);

    static const char *const keys[3] = { "UP", "OK", "DOWN" };
    static const char *const actions[3] = { "Apps", "Focus", "Sync now" };
    build_hints(scr, keys, actions);
}

static void build_placeholder(home_page_t page, const char *title, const char *symbol,
                              const char *line1, const char *line2) {
    lv_obj_t *scr = s_screens[page] = make_screen();

    lv_obj_t *head = make_label(scr, &lv_font_montserrat_14, C_ACCENT);
    lv_label_set_text(head, title);
    lv_obj_set_pos(head, HOME_GUTTER + 4, HOME_STATUS_TEXT_Y);
    lv_obj_t *rule = make_box(scr, C_DIVIDER, 0);
    set_box(rule, HOME_GUTTER, HOME_STATUS_H, HOME_SCREEN_W - 2 * HOME_GUTTER, 1);

    lv_obj_t *badge = make_box(scr, C_CARD, LV_RADIUS_CIRCLE);
    set_box(badge, (HOME_SCREEN_W - 88) / 2, 70, 88, 88);
    lv_obj_t *glyph = make_label(badge, &lv_font_montserrat_28, C_ACCENT);
    lv_label_set_text(glyph, symbol);
    lv_obj_center(glyph);

    lv_obj_t *big = make_label(scr, &lv_font_montserrat_20, C_TEXT);
    lv_label_set_text(big, line1);
    lv_obj_align(big, LV_ALIGN_TOP_MID, 0, 176);
    lv_obj_t *small = make_label(scr, &lv_font_montserrat_14, C_MUTED);
    lv_label_set_text(small, line2);
    lv_obj_set_width(small, HOME_SCREEN_W - 2 * HOME_GUTTER);
    lv_obj_set_style_text_align(small, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(small, LV_ALIGN_TOP_MID, 0, 208);

    static const char *const keys[3] = { NULL, "OK", NULL };
    static const char *const actions[3] = { NULL, "Back", NULL };
    build_hints(scr, keys, actions);
}

// ---------------------------------------------------------------------------
// Weather icon
// ---------------------------------------------------------------------------

static void show(lv_obj_t *o, bool visible) {
    if (visible) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void tint(lv_obj_t *o, uint32_t color, bool fresh) {
    lv_obj_set_style_bg_color(o, lv_color_hex(fresh ? color : C_DIM), 0);
}

// Cloud whose bounding box starts at (x, y): about 44 x 28 px.
static void place_cloud(int32_t x, int32_t y, bool fresh) {
    set_box(s_icon.cloud[0], x, y + 12, 44, 16);
    set_box(s_icon.cloud[1], x + 6, y + 4, 18, 18);
    set_box(s_icon.cloud[2], x + 18, y, 24, 24);
    for (int i = 0; i < 3; i++) {
        tint(s_icon.cloud[i], C_CLOUD, fresh);
        show(s_icon.cloud[i], true);
    }
}

static void draw_icon(home_sky_t sky, bool fresh) {
    show(s_icon.sun, false);
    show(s_icon.moon, false);
    show(s_icon.moon_cut, false);
    for (int i = 0; i < 3; i++) show(s_icon.cloud[i], false);
    for (int i = 0; i < 3; i++) show(s_icon.drop[i], false);
    for (int i = 0; i < 2; i++) show(s_icon.fog[i], false);
    show(s_icon.bolt, false);

    switch (sky) {
    case HOME_SKY_CLEAR:
        set_box(s_icon.sun, 10, 10, 36, 36);
        tint(s_icon.sun, C_SUN, fresh);
        show(s_icon.sun, true);
        break;
    case HOME_SKY_CLEAR_NIGHT:
        set_box(s_icon.moon, 12, 12, 32, 32);
        set_box(s_icon.moon_cut, 22, 6, 28, 28);
        tint(s_icon.moon, C_MOON, fresh);
        show(s_icon.moon, true);
        show(s_icon.moon_cut, true);
        break;
    case HOME_SKY_PARTLY:
        set_box(s_icon.sun, 26, 4, 26, 26);
        tint(s_icon.sun, C_SUN, fresh);
        show(s_icon.sun, true);
        place_cloud(4, 22, fresh);
        break;
    case HOME_SKY_PARTLY_NIGHT:
        set_box(s_icon.moon, 26, 4, 24, 24);
        set_box(s_icon.moon_cut, 33, 0, 20, 20);
        tint(s_icon.moon, C_MOON, fresh);
        show(s_icon.moon, true);
        show(s_icon.moon_cut, true);
        place_cloud(4, 22, fresh);
        break;
    case HOME_SKY_CLOUDY:
    case HOME_SKY_UNKNOWN:
        place_cloud(6, 14, fresh);
        break;
    case HOME_SKY_FOG:
        place_cloud(6, 6, fresh);
        for (int i = 0; i < 2; i++) {
            set_box(s_icon.fog[i], 6 + i * 6, 40 + i * 7, 38, 3);
            tint(s_icon.fog[i], C_CLOUD, fresh);
            show(s_icon.fog[i], true);
        }
        break;
    case HOME_SKY_RAIN:
    case HOME_SKY_SNOW:
        place_cloud(6, 6, fresh);
        for (int i = 0; i < 3; i++) {
            bool snow = sky == HOME_SKY_SNOW;
            set_box(s_icon.drop[i], 12 + i * 12, 40 + (i % 2) * 4, snow ? 6 : 3, snow ? 6 : 9);
            lv_obj_set_style_radius(s_icon.drop[i], snow ? LV_RADIUS_CIRCLE : 2, 0);
            tint(s_icon.drop[i], snow ? C_SNOW : C_RAIN, fresh);
            show(s_icon.drop[i], true);
        }
        break;
    case HOME_SKY_STORM:
        place_cloud(6, 6, fresh);
        lv_obj_set_pos(s_icon.bolt, 22, 32);
        lv_obj_set_style_text_color(s_icon.bolt, lv_color_hex(fresh ? C_SUN : C_DIM), 0);
        show(s_icon.bolt, true);
        break;
    }
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

// Sets label text only when it differs from the cached copy.
static void set_text(lv_obj_t *label, char *cache, size_t cache_size, const char *text) {
    if (strncmp(cache, text, cache_size) == 0) return;
    snprintf(cache, cache_size, "%s", text);
    lv_label_set_text(label, text);
}

static void set_color(lv_obj_t *label, uint32_t color) {
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

static void refresh_clock(time_t now, const struct tm *local, bool valid, bool twelve) {
    (void)now;
    char date[24], hm[8], sec[4], ampm[4];
    if (valid) {
        home_fmt_date(date, sizeof(date), local);
        home_fmt_clock(local, twelve, hm, sizeof(hm), sec, sizeof(sec), ampm, sizeof(ampm));
    } else {
        snprintf(date, sizeof(date), "WAITING FOR TIME");
        snprintf(hm, sizeof(hm), "--:--");
        snprintf(sec, sizeof(sec), "--");
        ampm[0] = '\0';
    }
    bool moved = strcmp(hm, s_drawn.clock) != 0 || strcmp(ampm, s_drawn.ampm) != 0;
    set_text(s_date, s_drawn.date, sizeof(s_drawn.date), date);
    set_text(s_clock, s_drawn.clock, sizeof(s_drawn.clock), hm);
    set_text(s_seconds, s_drawn.seconds, sizeof(s_drawn.seconds), sec);
    set_text(s_ampm, s_drawn.ampm, sizeof(s_drawn.ampm), ampm);
    if (moved) {
        // Seconds sit on the clock's baseline; AM/PM above them. Re-anchor
        // whenever the clock width can change (12-hour "9:05" vs "12:05").
        lv_obj_update_layout(s_clock);
        lv_obj_align_to(s_seconds, s_clock, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -8);
        lv_obj_align_to(s_ampm, s_clock, LV_ALIGN_OUT_RIGHT_TOP, 6, 8);
    }
}

static void refresh_status_bar(const home_status_t *st) {
    int wifi = st->wifi_connected ? 1 : 0;
    if (wifi != s_drawn.wifi_state) {
        s_drawn.wifi_state = wifi;
        set_color(s_wifi, wifi ? C_TEXT : C_DIM);
    }

    int soc = s_battery_soc;
    static const char *const ICONS[5] = {
        LV_SYMBOL_BATTERY_EMPTY, LV_SYMBOL_BATTERY_1, LV_SYMBOL_BATTERY_2,
        LV_SYMBOL_BATTERY_3, LV_SYMBOL_BATTERY_FULL,
    };
    int level = home_battery_level(soc);
    char text[24];
    if (level < 0) {
        snprintf(text, sizeof(text), "%s", LV_SYMBOL_BATTERY_EMPTY);  // gauge unavailable
    } else {
        snprintf(text, sizeof(text), "%d%%  %s", soc, ICONS[level]);
    }
    int color = level < 0 ? C_DIM : (soc < 15 ? C_WARN : C_TEXT);
    if (strcmp(text, s_drawn.battery) != 0) {
        set_text(s_battery, s_drawn.battery, sizeof(s_drawn.battery), text);
        lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -(HOME_GUTTER + 4), HOME_STATUS_TEXT_Y);
    }
    if (color != s_drawn.battery_color) {
        s_drawn.battery_color = color;
        set_color(s_battery, (uint32_t)color);
    }
}

static void refresh_weather(const home_status_t *st, int64_t now_us) {
    int64_t age_s = st->weather_this_boot ? (now_us - st->weather_at_us) / 1000000 : -1;
    bool fresh = st->have_weather &&
                 home_weather_is_fresh(st->wifi_connected, st->weather_this_boot, age_s,
                                       CONFIG_HOME_WEATHER_REFRESH_MIN);
    int sky = st->have_weather ? (int)home_sky_from_wmo(st->weather.wmo, st->weather.is_day)
                               : (int)HOME_SKY_UNKNOWN;
    if (sky != s_drawn.sky || (int)fresh != s_drawn.weather_fresh) {
        draw_icon((home_sky_t)sky, fresh);
        s_drawn.sky = sky;
        s_drawn.weather_fresh = fresh;
        set_color(s_temp, fresh ? C_TEXT : C_MUTED);
        set_color(s_condition, fresh ? C_TEXT : C_MUTED);
    }

    char temp[12], condition[24], city[16], location[64];
    if (st->have_weather) {
        snprintf(temp, sizeof(temp), "%d\xC2\xB0" "C", home_round_c10(st->weather.temp_c10));
        snprintf(condition, sizeof(condition), "%s", home_condition_text(st->weather.wmo));
    } else {
        snprintf(temp, sizeof(temp), "--\xC2\xB0" "C");
        snprintf(condition, sizeof(condition), "No weather yet");
    }
    home_ascii_fold(st->have_location ? st->city : "", city, sizeof(city));
    home_fmt_location_line(location, sizeof(location), city, st->have_weather, &st->weather);
    set_text(s_temp, s_drawn.temp, sizeof(s_drawn.temp), temp);
    set_text(s_condition, s_drawn.condition, sizeof(s_drawn.condition), condition);
    set_text(s_location, s_drawn.location, sizeof(s_drawn.location), location);
}

static void refresh_sync(const home_status_t *st, time_t now, const struct tm *local,
                         int64_t now_us, bool twelve) {
    (void)now;
    home_sync_view_t view;
    memset(&view, 0, sizeof(view));
    view.wifi_configured = st->wifi_configured;
    view.wifi_connected = st->wifi_connected;
    view.state = st->sync_state;
    view.twelve_hour = twelve;
    view.retry_in_s = (int32_t)((st->retry_at_us - now_us) / 1000000);
    if (st->last_sync_epoch > 0) {
        time_t last = (time_t)st->last_sync_epoch;
        localtime_r(&last, &view.last_sync);
        view.now = *local;
        view.have_last_sync = true;
    }
    char text[48];
    bool warning = home_fmt_sync(text, sizeof(text), &view);
    set_text(s_sync, s_drawn.sync, sizeof(s_drawn.sync), text);
    if (warning != s_drawn.sync_warning) {
        s_drawn.sync_warning = warning;
        set_color(s_sync, warning ? C_WARN : C_MUTED);
    }
}

static void refresh_cb(lv_timer_t *timer) {
    (void)timer;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    bool valid = home_net_time_valid();
    bool twelve = home_twelve_hour();
    int64_t now_us = esp_timer_get_time();

    home_status_t st;
    home_net_get_status(&st);

    refresh_clock(now, &local, valid, twelve);
    refresh_status_bar(&st);
    refresh_weather(&st, now_us);
    refresh_sync(&st, now, &local, now_us, twelve);
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

void home_ui_create(void) {
    memset(&s_drawn, 0, sizeof(s_drawn));
    s_drawn.wifi_state = -1;
    s_drawn.battery_color = -1;
    s_drawn.sky = -1;
    s_drawn.weather_fresh = -1;
    s_drawn.clock[0] = '\x01';  // force the first layout pass

    build_home();
    build_placeholder(HOME_PAGE_APPS, "APPS", LV_SYMBOL_LIST, "Apps",
                      "Nothing installed yet.\nMore apps are coming.");
    build_placeholder(HOME_PAGE_FOCUS, "FOCUS", LV_SYMBOL_EYE_OPEN, "Focus",
                      "Focus sessions are\ncoming soon.");
    refresh_cb(NULL);
    s_refresh_timer = lv_timer_create(refresh_cb, REFRESH_MS, NULL);
}

void home_ui_set_awake(bool awake) {
    if (!s_refresh_timer) return;
    if (awake) {
        refresh_cb(NULL);
        lv_timer_resume(s_refresh_timer);
    } else {
        lv_timer_pause(s_refresh_timer);
    }
}

void home_ui_show(home_page_t page) {
    if ((unsigned)page < 3 && s_screens[page]) lv_screen_load(s_screens[page]);
}

void home_ui_set_battery(int soc) {
    s_battery_soc = soc > 100 ? 100 : soc;
}
