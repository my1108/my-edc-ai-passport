// main/home_ui.c - see home_ui.h.
#include "home_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "bsp_display.h"
#include "home_layout.h"
#include "home_net.h"
#include "home_store.h"
#include "setup_portal.h"
#include "spot_net.h"
#include "lvgl.h"
// lv_image_cache_drop(): the cover reuses one pixel buffer for every album, so
// LVGL's cached copy must be dropped when it changes (not in lvgl.h in 9.5).
#include "src/misc/cache/instance/lv_image_cache.h"
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
#define C_SPOT     0x1ED760   // Spotify green, used only on the Now Playing page

#define TOAST_US   (3LL * 1000 * 1000)

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

static lv_obj_t *s_screens[HOME_PAGE_COUNT];
static lv_timer_t *s_refresh_timer;
static home_page_t s_shown = HOME_PAGE_HOME;
static lv_obj_t *s_app_screen;          // the open app's screen, NULL when none
static const home_app_t *s_app;         // the open app

// Apps tray
static lv_obj_t *s_tray_row[HOME_APPS_MAX];
static lv_obj_t *s_tray_battery;
static char s_tray_battery_text[24];
static int s_tray_battery_color = -1;
static int s_tray_sel;
static uint32_t s_tick;

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

// Spotify page widgets
typedef struct {
    lv_obj_t *battery;
    lv_obj_t *state;
    lv_obj_t *eq[SPOT_EQ_BARS];
    lv_obj_t *cover;          // album art (lv_image over s_cover_dsc)
    lv_obj_t *title;
    lv_obj_t *artist;
    lv_obj_t *bar_bg;
    lv_obj_t *bar_fill;
    lv_obj_t *pos;
    lv_obj_t *len;
    lv_obj_t *msg_title;
    lv_obj_t *msg_detail;
    lv_obj_t *foot;
    lv_obj_t *ok_action;      // hint text under OK: "Pause" / "Play"
} spot_ui_t;

static spot_ui_t s_sp;
static lv_image_dsc_t s_cover_dsc;  // points at spot_cover_pixels()

// Setup page widgets and state
typedef struct {
    lv_obj_t *battery;
    lv_obj_t *heading;
    lv_obj_t *qr_box;
    lv_obj_t *qr;
    lv_obj_t *line1;
    lv_obj_t *line2;
    lv_obj_t *status;
    setup_info_t info;      // hotspot name/password (from setup_portal_start)
    int card;
    int card_count;
    char qr_text[96];
    char heading_text[40];
    char line1_text[48];
    char line2_text[48];
    char status_text[64];
    char battery_text[24];
    int battery_color;
    int status_color;
} setup_ui_t;

static setup_ui_t s_su;

// Montserrat for Latin text with the built-in Source Han Sans SC 16 subset as
// fallback for CJK (about 1,000 common Chinese characters plus kana). Writable
// copies, because the fallback pointer is part of the font descriptor.
static lv_font_t s_font_title;
static lv_font_t s_font_artist;

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
    // Spotify page
    char sp_battery[24];
    int sp_battery_color;
    char sp_raw_title[SPOT_TITLE_MAX];
    char sp_raw_artist[SPOT_ARTIST_MAX];
    char sp_state[80];
    char sp_pos[12];
    char sp_len[12];
    char sp_msg_title[32];
    char sp_msg_detail[160];
    char sp_foot[40];
    char sp_ok[8];
    int sp_mode;          // spot_show_t, -1 none
    int sp_state_color;
    int sp_foot_color;
    int sp_msg_color;
    int sp_fill_w;
    int sp_eq_live;       // bars animating
    int sp_cover_shown;   // -1 unknown, 0 equalizer, 1 cover
    uint32_t sp_cover_seq;
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
static void build_hints(lv_obj_t *scr, const char *const keys[3], const char *const actions[3],
                        lv_obj_t *action_labels[3]) {
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
        if (action_labels) action_labels[i] = act;
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
    static const char *const actions[3] = { "Apps", "Setup", "Sync now" };
    build_hints(scr, keys, actions, NULL);
}

static lv_obj_t *make_center_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                                   int32_t y, int32_t h) {
    lv_obj_t *l = make_label(parent, font, color);
    lv_obj_set_pos(l, SPOT_TEXT_X, y);
    lv_obj_set_size(l, SPOT_TEXT_W, h);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

static void spotify_cache_reset(void) {
    s_drawn.sp_battery[0] = '\0';
    s_drawn.sp_battery_color = -1;
    s_drawn.sp_mode = -1;
    s_drawn.sp_state[0] = '\0';
    s_drawn.sp_state_color = -1;
    s_drawn.sp_pos[0] = s_drawn.sp_len[0] = '\0';
    s_drawn.sp_msg_title[0] = s_drawn.sp_msg_detail[0] = '\0';
    s_drawn.sp_foot[0] = s_drawn.sp_ok[0] = '\0';
    s_drawn.sp_foot_color = -1;
    s_drawn.sp_msg_color = -1;
    s_drawn.sp_fill_w = -1;
    s_drawn.sp_eq_live = -1;
    s_drawn.sp_cover_shown = -1;
    s_drawn.sp_cover_seq = UINT32_MAX;
    s_drawn.sp_raw_title[0] = '\x01';
    s_drawn.sp_raw_artist[0] = '\x01';
}

void home_ui_spotify_build(lv_obj_t *scr) {
    memset(&s_sp, 0, sizeof(s_sp));
    spotify_cache_reset();

    s_font_title = lv_font_montserrat_20;
    s_font_title.fallback = &lv_font_source_han_sans_sc_16_cjk;
    s_font_artist = lv_font_montserrat_14;
    s_font_artist.fallback = &lv_font_source_han_sans_sc_16_cjk;

    // Status bar: page name left, battery right.
    lv_obj_t *head = make_label(scr, &lv_font_montserrat_14, C_SPOT);
    lv_label_set_text(head, LV_SYMBOL_AUDIO "  NOW PLAYING");
    lv_obj_set_pos(head, HOME_GUTTER + 4, HOME_STATUS_TEXT_Y);
    s_sp.battery = make_label(scr, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(s_sp.battery, LV_ALIGN_TOP_RIGHT, -(HOME_GUTTER + 4), HOME_STATUS_TEXT_Y);
    lv_obj_t *rule = make_box(scr, C_DIVIDER, 0);
    set_box(rule, HOME_GUTTER, HOME_STATUS_H, HOME_SCREEN_W - 2 * HOME_GUTTER, 1);

    s_sp.state = make_center_label(scr, &s_font_artist, C_MUTED, SPOT_STATE_Y, SPOT_STATE_H + 4);

    for (int i = 0; i < SPOT_EQ_BARS; i++) {
        s_sp.eq[i] = make_box(scr, C_DIM, 3);
        set_box(s_sp.eq[i], SPOT_EQ_X + i * (SPOT_EQ_BAR_W + SPOT_EQ_GAP),
                SPOT_EQ_Y + SPOT_EQ_H - SPOT_EQ_MIN_H, SPOT_EQ_BAR_W, SPOT_EQ_MIN_H);
    }

    s_sp.cover = lv_image_create(scr);
    lv_obj_set_pos(s_sp.cover, SPOT_COVER_X, SPOT_COVER_Y);
    lv_obj_set_size(s_sp.cover, SPOT_COVER_BOX, SPOT_COVER_BOX);
    lv_image_set_inner_align(s_sp.cover, LV_IMAGE_ALIGN_CENTER);
    lv_obj_add_flag(s_sp.cover, LV_OBJ_FLAG_HIDDEN);

    s_sp.title = make_center_label(scr, &s_font_title, C_TEXT, SPOT_TITLE_Y, SPOT_TITLE_H);
    s_sp.artist = make_center_label(scr, &s_font_artist, C_MUTED, SPOT_ARTIST_Y, SPOT_ARTIST_H);

    s_sp.bar_bg = make_box(scr, C_DIVIDER, 2);
    set_box(s_sp.bar_bg, SPOT_TEXT_X, SPOT_BAR_Y, SPOT_TEXT_W, SPOT_BAR_H);
    s_sp.bar_fill = make_box(scr, C_SPOT, 2);
    set_box(s_sp.bar_fill, SPOT_TEXT_X, SPOT_BAR_Y, 0, SPOT_BAR_H);
    s_sp.pos = make_label(scr, &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(s_sp.pos, SPOT_TEXT_X, SPOT_TIME_Y);
    s_sp.len = make_label(scr, &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_width(s_sp.len, SPOT_TEXT_W / 2);
    lv_obj_set_style_text_align(s_sp.len, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_sp.len, SPOT_TEXT_X + SPOT_TEXT_W / 2, SPOT_TIME_Y);

    s_sp.msg_title = make_center_label(scr, &lv_font_montserrat_20, C_TEXT, SPOT_MSG_TITLE_Y, 24);
    s_sp.msg_detail = make_center_label(scr, &lv_font_montserrat_14, C_MUTED, SPOT_MSG_DETAIL_Y,
                                        SPOT_MSG_LINES * 16);

    s_sp.foot = make_center_label(scr, &lv_font_montserrat_14, C_DIM, SPOT_FOOT_Y, SPOT_FOOT_H + 2);

    static const char *const keys[3] = { "UP", "OK", "DOWN" };
    static const char *const actions[3] = { "Prev", "Play", "Next" };
    lv_obj_t *acts[3] = { NULL, NULL, NULL };
    build_hints(scr, keys, actions, acts);
    s_sp.ok_action = acts[1];
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

static void draw_battery(lv_obj_t *label, char *cache, size_t cache_size, int *color_cache) {
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
    if (strncmp(text, cache, cache_size) != 0) {
        set_text(label, cache, cache_size, text);
        lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -(HOME_GUTTER + 4), HOME_STATUS_TEXT_Y);
    }
    if (color != *color_cache) {
        *color_cache = color;
        set_color(label, (uint32_t)color);
    }
}

static void refresh_status_bar(const home_status_t *st) {
    int wifi = st->wifi_connected ? 1 : 0;
    if (wifi != s_drawn.wifi_state) {
        s_drawn.wifi_state = wifi;
        set_color(s_wifi, wifi ? C_TEXT : C_DIM);
    }
    draw_battery(s_battery, s_drawn.battery, sizeof(s_drawn.battery), &s_drawn.battery_color);
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

// ---------------------------------------------------------------------------
// Spotify page
// ---------------------------------------------------------------------------

static bool font_has(uint32_t cp, void *ctx) {
    const lv_font_t *font = ctx;
    lv_font_glyph_dsc_t dsc;
    memset(&dsc, 0, sizeof(dsc));
    return lv_font_get_glyph_dsc(font, &dsc, cp, 0) && !dsc.is_placeholder;
}

// Sets a label from UTF-8 that may hold characters the font cannot draw.
static void set_font_text(lv_obj_t *label, const lv_font_t *font, char *raw_cache, size_t raw_size,
                          const char *raw) {
    if (strncmp(raw_cache, raw, raw_size) == 0) return;
    snprintf(raw_cache, raw_size, "%s", raw);
    char text[SPOT_TITLE_MAX + 32];
    spot_text_for_font(raw, text, sizeof(text), font_has, (void *)font);
    lv_label_set_text(label, text);
}

static void set_visible(lv_obj_t *o, bool visible) {
    show(o, visible);
}

static void set_color_cached(lv_obj_t *label, int *cache, uint32_t color) {
    if (*cache == (int)color) return;
    *cache = (int)color;
    set_color(label, color);
}

static void refresh_eq(bool live) {
    if (!live && s_drawn.sp_eq_live == 0) return;   // already flat
    for (int i = 0; i < SPOT_EQ_BARS; i++) {
        int h = live ? spot_eq_height(i, s_tick, SPOT_EQ_MIN_H + 6, SPOT_EQ_H) : SPOT_EQ_MIN_H;
        set_box(s_sp.eq[i], SPOT_EQ_X + i * (SPOT_EQ_BAR_W + SPOT_EQ_GAP), SPOT_EQ_Y + SPOT_EQ_H - h,
                SPOT_EQ_BAR_W, h);
        if (live != (s_drawn.sp_eq_live == 1)) {
            lv_obj_set_style_bg_color(s_sp.eq[i], lv_color_hex(live ? C_SPOT : C_DIM), 0);
        }
    }
    s_drawn.sp_eq_live = live ? 1 : 0;
}

// Called by the Spotify worker before it overwrites the cover pixels.
static bool cover_detach(void) {
    if (!bsp_lvgl_lock(500)) return false;
    if (s_sp.cover) lv_obj_add_flag(s_sp.cover, LV_OBJ_FLAG_HIDDEN);  // NULL: app closed
    lv_image_cache_drop(&s_cover_dsc);
    s_drawn.sp_cover_shown = -1;
    s_drawn.sp_cover_seq = UINT32_MAX;
    bsp_lvgl_unlock();
    return true;
}

// Shows the album cover when one is ready, else the equalizer.
static void refresh_cover(const spot_status_t *sp, bool track) {
    bool cover = track && sp->cover_ready;
    if (cover && sp->cover_seq != s_drawn.sp_cover_seq) {
        int w = 0, h = 0;
        const uint16_t *px = spot_cover_pixels(&w, &h);
        if (!px || w <= 0 || h <= 0) {
            cover = false;
        } else {
            lv_image_cache_drop(&s_cover_dsc);
            memset(&s_cover_dsc, 0, sizeof(s_cover_dsc));
            s_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            s_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            s_cover_dsc.header.w = (uint32_t)w;
            s_cover_dsc.header.h = (uint32_t)h;
            s_cover_dsc.header.stride = (uint32_t)w * 2;
            s_cover_dsc.data_size = (uint32_t)(w * h * 2);
            s_cover_dsc.data = (const uint8_t *)px;
            lv_image_set_src(s_sp.cover, &s_cover_dsc);
            s_drawn.sp_cover_seq = sp->cover_seq;
        }
    }
    if ((int)cover == s_drawn.sp_cover_shown) return;
    s_drawn.sp_cover_shown = cover;
    set_visible(s_sp.cover, cover);
    for (int i = 0; i < SPOT_EQ_BARS; i++) set_visible(s_sp.eq[i], !cover);
}

static void refresh_spotify(const home_status_t *st, int64_t now_us) {
    draw_battery(s_sp.battery, s_drawn.sp_battery, sizeof(s_drawn.sp_battery), &s_drawn.sp_battery_color);

    spot_status_t sp;
    spot_get_status(&sp);
    spot_page_in_t in = {
        .link = sp.link,
        .wifi_configured = st->wifi_configured,
        .wifi_connected = st->wifi_connected,
        .fetched = sp.fetched,
        .last = sp.last,
        .have_item = sp.now.have_item,
        .is_playing = sp.now.is_playing,
    };
    spot_page_t pg = spot_page_state(&in);
    bool track = pg.show == SPOT_SHOW_TRACK;

    if ((int)pg.show != s_drawn.sp_mode) {
        s_drawn.sp_mode = (int)pg.show;
        set_visible(s_sp.title, track);
        set_visible(s_sp.artist, track);
        set_visible(s_sp.bar_bg, track);
        set_visible(s_sp.bar_fill, track);
        set_visible(s_sp.pos, track);
        set_visible(s_sp.len, track);
        set_visible(s_sp.msg_title, !track);
        set_visible(s_sp.msg_detail, !track);
    }

    char state[80];
    if (track) {
        set_font_text(s_sp.title, &s_font_title, s_drawn.sp_raw_title, sizeof(s_drawn.sp_raw_title),
                      sp.now.title[0] ? sp.now.title : "Unknown title");
        const char *who = sp.now.artist[0] ? sp.now.artist
                        : sp.now.type == SPOT_ITEM_AD ? "Advertisement" : "";
        set_font_text(s_sp.artist, &s_font_artist, s_drawn.sp_raw_artist, sizeof(s_drawn.sp_raw_artist), who);

        int32_t at = spot_progress_at(&sp.now, sp.now_at_us, now_us);
        char pos[12], len[12];
        spot_fmt_time(pos, sizeof(pos), at);
        spot_fmt_time(len, sizeof(len), sp.now.duration_ms);
        set_text(s_sp.pos, s_drawn.sp_pos, sizeof(s_drawn.sp_pos), pos);
        set_text(s_sp.len, s_drawn.sp_len, sizeof(s_drawn.sp_len), len);
        int fill = spot_progress_permille(at, sp.now.duration_ms) * SPOT_TEXT_W / 1000;
        if (fill != s_drawn.sp_fill_w) {
            s_drawn.sp_fill_w = fill;
            lv_obj_set_width(s_sp.bar_fill, fill);
        }

        char device[SPOT_DEVICE_MAX];
        spot_text_for_font(sp.now.device, device, sizeof(device), font_has, (void *)&s_font_artist);
        if (device[0]) {
            snprintf(state, sizeof(state), "%s " HOME_SEP " %s", pg.status, device);
        } else {
            snprintf(state, sizeof(state), "%s", pg.status);
        }
    } else {
        set_text(s_sp.msg_title, s_drawn.sp_msg_title, sizeof(s_drawn.sp_msg_title), pg.title);
        set_text(s_sp.msg_detail, s_drawn.sp_msg_detail, sizeof(s_drawn.sp_msg_detail),
                 pg.detail ? pg.detail : "");
        state[0] = '\0';
    }
    set_text(s_sp.state, s_drawn.sp_state, sizeof(s_drawn.sp_state), state);
    set_color_cached(s_sp.state, &s_drawn.sp_state_color, pg.warning ? C_WARN : C_SPOT);
    set_color_cached(s_sp.msg_title, &s_drawn.sp_msg_color, pg.warning ? C_WARN : C_TEXT);

    bool playing = track && sp.now.is_playing;
    refresh_cover(&sp, track);
    refresh_eq(playing && st->wifi_connected && s_drawn.sp_cover_shown != 1);
    set_text(s_sp.ok_action, s_drawn.sp_ok, sizeof(s_drawn.sp_ok), playing ? "Pause" : "Play");

    // Footer: a recent command error, then "Sending...", else the way home.
    const char *foot = "Hold OK for apps";
    uint32_t foot_color = C_DIM;
    if (sp.toast && now_us - sp.toast_at_us < TOAST_US) {
        foot = sp.toast;
        foot_color = C_WARN;
    } else if (sp.busy) {
        foot = "Sending...";
        foot_color = C_MUTED;
    }
    set_text(s_sp.foot, s_drawn.sp_foot, sizeof(s_drawn.sp_foot), foot);
    set_color_cached(s_sp.foot, &s_drawn.sp_foot_color, foot_color);
}

void home_ui_spotify_refresh(int64_t now_us) {
    home_status_t st;
    home_net_get_status(&st);
    refresh_spotify(&st, now_us);
}

void home_ui_spotify_destroy(void) {
    // The screen (and every widget on it) is deleted right after this.
    lv_image_cache_drop(&s_cover_dsc);
    memset(&s_sp, 0, sizeof(s_sp));
}

// ---------------------------------------------------------------------------
// Apps tray
// ---------------------------------------------------------------------------

static void tray_style_row(lv_obj_t *row, bool selected) {
    lv_obj_set_style_bg_color(row, lv_color_hex(selected ? 0x1E2632 : C_CARD), 0);
    lv_obj_set_style_border_width(row, selected ? 2 : 0, 0);
}

static void build_tray(void) {
    lv_obj_t *scr = s_screens[HOME_PAGE_APPS] = make_screen();

    lv_obj_t *head = make_label(scr, &lv_font_montserrat_14, C_ACCENT);
    lv_label_set_text(head, LV_SYMBOL_LIST "  APPS");
    lv_obj_set_pos(head, HOME_GUTTER + 4, HOME_STATUS_TEXT_Y);
    s_tray_battery = make_label(scr, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(s_tray_battery, LV_ALIGN_TOP_RIGHT, -(HOME_GUTTER + 4), HOME_STATUS_TEXT_Y);
    lv_obj_t *rule = make_box(scr, C_DIVIDER, 0);
    set_box(rule, HOME_GUTTER, HOME_STATUS_H, HOME_SCREEN_W - 2 * HOME_GUTTER, 1);

    for (int i = 0; i < HOME_APP_COUNT && i < HOME_APPS_MAX; i++) {
        const home_app_t *app = &HOME_APPS[i];
        lv_obj_t *row = s_tray_row[i] = make_box(scr, C_CARD, 12);
        set_box(row, TRAY_ROW_X, TRAY_ROW_Y0 + i * TRAY_ROW_PITCH, TRAY_ROW_W, TRAY_ROW_H);
        lv_obj_set_style_border_color(row, lv_color_hex(C_ACCENT), 0);
        tray_style_row(row, i == 0);

        lv_obj_t *icon = make_label(row, &lv_font_montserrat_20, C_ACCENT);
        lv_label_set_text(icon, app->icon);
        lv_obj_set_width(icon, 32);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_t *name = make_label(row, &lv_font_montserrat_20, C_TEXT);
        lv_label_set_text(name, app->name);
        lv_obj_set_pos(name, 48, 7);
        lv_obj_t *sub = make_label(row, &lv_font_montserrat_14, C_MUTED);
        lv_label_set_text(sub, app->subtitle);
        lv_obj_set_pos(sub, 48, 32);
        lv_obj_set_width(sub, TRAY_ROW_W - 56);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    }

    lv_obj_t *foot = make_label(scr, &lv_font_montserrat_14, C_DIM);
    lv_label_set_text(foot, "Hold OK for home");
    lv_obj_set_width(foot, HOME_SCREEN_W - 2 * HOME_GUTTER);
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(foot, HOME_GUTTER, TRAY_FOOT_Y);

    static const char *const keys[3] = { "UP", "OK", "DOWN" };
    static const char *const actions[3] = { "Up", "Open", "Down" };
    build_hints(scr, keys, actions, NULL);
}

void home_ui_tray_select(int index) {
    if (index == s_tray_sel) return;
    if (s_tray_sel >= 0 && s_tray_sel < HOME_APPS_MAX && s_tray_row[s_tray_sel]) {
        tray_style_row(s_tray_row[s_tray_sel], false);
    }
    s_tray_sel = index;
    if (index >= 0 && index < HOME_APPS_MAX && s_tray_row[index]) tray_style_row(s_tray_row[index], true);
}

void home_ui_app_open(const home_app_t *app) {
    if (s_app_screen || !app || !app->build) return;
    s_app_screen = make_screen();
    s_app = app;
    app->build(s_app_screen);
    s_shown = HOME_PAGE_APP;
    lv_screen_load(s_app_screen);
    if (app->refresh) app->refresh(esp_timer_get_time());  // first frame is current
}

void home_ui_app_close(home_page_t next) {
    home_ui_show(next);  // the app's screen must not be active when deleted
    if (!s_app_screen) return;
    if (s_app && s_app->destroy) s_app->destroy();
    lv_obj_delete(s_app_screen);
    s_app_screen = NULL;
    s_app = NULL;
}

void home_ui_mem(uint32_t *total, uint32_t *used, uint32_t *max_used) {
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    if (total) *total = (uint32_t)mon.total_size;
    if (used) *used = (uint32_t)(mon.total_size - mon.free_size);
    if (max_used) *max_used = (uint32_t)mon.max_used;
}

// ---------------------------------------------------------------------------
// Setup page
// ---------------------------------------------------------------------------

#define SETUP_EVENT_US (10LL * 1000 * 1000)

static void build_setup(void) {
    lv_obj_t *scr = s_screens[HOME_PAGE_SETUP] = make_screen();

    lv_obj_t *head = make_label(scr, &lv_font_montserrat_14, C_ACCENT);
    lv_label_set_text(head, LV_SYMBOL_SETTINGS "  SETUP");
    lv_obj_set_pos(head, HOME_GUTTER + 4, HOME_STATUS_TEXT_Y);
    s_su.battery = make_label(scr, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(s_su.battery, LV_ALIGN_TOP_RIGHT, -(HOME_GUTTER + 4), HOME_STATUS_TEXT_Y);
    lv_obj_t *rule = make_box(scr, C_DIVIDER, 0);
    set_box(rule, HOME_GUTTER, HOME_STATUS_H, HOME_SCREEN_W - 2 * HOME_GUTTER, 1);

    s_su.heading = make_center_label(scr, &lv_font_montserrat_14, C_TEXT, SETUP_HEAD_Y, SETUP_HEAD_H);

    // QR codes need a light quiet zone around them to scan reliably.
    s_su.qr_box = make_box(scr, 0xFFFFFF, 8);
    set_box(s_su.qr_box, SETUP_QR_X, SETUP_QR_Y, SETUP_QR_BOX, SETUP_QR_BOX);
    s_su.qr = lv_qrcode_create(s_su.qr_box);
    lv_qrcode_set_size(s_su.qr, SETUP_QR_SIZE);
    lv_qrcode_set_dark_color(s_su.qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_su.qr, lv_color_hex(0xFFFFFF));
    lv_obj_center(s_su.qr);

    s_su.line1 = make_center_label(scr, &lv_font_montserrat_14, C_MUTED, SETUP_LINE1_Y, 18);
    s_su.line2 = make_center_label(scr, &lv_font_montserrat_14, C_TEXT, SETUP_LINE2_Y, 18);
    s_su.status = make_center_label(scr, &lv_font_montserrat_14, C_MUTED, SETUP_STATUS_Y, 18);

    static const char *const keys[3] = { "UP", "OK", "DOWN" };
    static const char *const actions[3] = { "Prev", "Done", "Next" };
    build_hints(scr, keys, actions, NULL);
}

static bool font14_has(uint32_t cp, void *ctx) {
    (void)ctx;
    return font_has(cp, (void *)&lv_font_montserrat_14);
}

static void refresh_setup(const home_status_t *st, int64_t now_us) {
    draw_battery(s_su.battery, s_su.battery_text, sizeof(s_su.battery_text), &s_su.battery_color);

    s_su.info.sta_connected = st->wifi_connected;
    snprintf(s_su.info.sta_ip, sizeof(s_su.info.sta_ip), "%s", st->ip);
    int count = setup_card_count(&s_su.info);
    if (count != s_su.card_count) {
        // The board just joined (or left) a network: show the most useful card.
        s_su.card_count = count;
        s_su.card = setup_first_card(&s_su.info);
    }
    setup_view_t v;
    setup_view(&s_su.info, s_su.card, &v);
    s_su.card = (int)v.card;

    if (strcmp(v.qr, s_su.qr_text) != 0) {
        snprintf(s_su.qr_text, sizeof(s_su.qr_text), "%s", v.qr);
        lv_qrcode_update(s_su.qr, v.qr, (uint32_t)strlen(v.qr));
    }
    set_text(s_su.heading, s_su.heading_text, sizeof(s_su.heading_text), v.heading);
    set_text(s_su.line1, s_su.line1_text, sizeof(s_su.line1_text), v.line1);
    set_text(s_su.line2, s_su.line2_text, sizeof(s_su.line2_text), v.line2);

    char status[64], ssid[40];
    uint32_t color = C_MUTED;
    int64_t at = 0;
    const char *event = setup_portal_event(&at);
    spot_text_for_font(st->ssid, ssid, sizeof(ssid), font14_has, NULL);
    if (event && now_us - at < SETUP_EVENT_US) {
        snprintf(status, sizeof(status), "%s", event);
        color = C_ACCENT;
    } else if (st->wifi_connected) {
        snprintf(status, sizeof(status), LV_SYMBOL_WIFI "  %s", ssid);
        color = C_ACCENT;
    } else if (st->wifi_configured) {
        snprintf(status, sizeof(status), "Connecting to %s...", ssid);
        color = C_WARN;
    } else {
        snprintf(status, sizeof(status), "No Wi-Fi saved yet");
        color = C_WARN;
    }
    set_text(s_su.status, s_su.status_text, sizeof(s_su.status_text), status);
    if ((int)color != s_su.status_color) {
        s_su.status_color = (int)color;
        set_color(s_su.status, color);
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
    s_tick++;

    if (s_shown == HOME_PAGE_APP) {
        if (s_app && s_app->refresh) s_app->refresh(now_us);
        return;
    }
    if (s_shown == HOME_PAGE_APPS) {
        draw_battery(s_tray_battery, s_tray_battery_text, sizeof(s_tray_battery_text), &s_tray_battery_color);
        return;
    }
    if (s_shown == HOME_PAGE_SETUP) {
        refresh_setup(&st, now_us);
        return;
    }
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
    spotify_cache_reset();

    s_su.battery_color = -1;
    s_su.status_color = -1;
    s_su.card_count = -1;

    build_home();
    build_tray();
    build_setup();
    spot_set_cover_detach(cover_detach);
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
    if ((unsigned)page < HOME_PAGE_COUNT && s_screens[page]) {
        s_shown = page;
        lv_screen_load(s_screens[page]);
        refresh_cb(NULL);  // draw the new page's current state before the first frame
    }
}

void home_ui_setup_open(const setup_info_t *info) {
    s_su.info = *info;
    s_su.card_count = -1;      // pick the first card on the next refresh
    s_su.qr_text[0] = '\0';
}

void home_ui_setup_step(int delta) {
    s_su.card += delta;
    refresh_cb(NULL);
}

void home_ui_set_battery(int soc) {
    s_battery_soc = soc > 100 ? 100 : soc;
}
