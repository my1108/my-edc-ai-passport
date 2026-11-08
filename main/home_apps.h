// main/home_apps.h - the app tray's registry.
//
// Each app is one entry in HOME_APPS (home_apps.c). The tray lists them in
// order; opening one builds its screen, and leaving it deletes the screen, so
// only one app's widgets are in the LVGL pool at a time.
//
// Lifecycle (main.c drives it, the UI parts run under bsp_lvgl_lock()):
//   open:  build(screen) -> screen shown -> set_active(true while the screen is lit)
//   keys:  key() for every click (OK long press is taken: it returns to the tray)
//   sleep: set_active(false) when the screen sleeps, set_active(true) on wake
//   close: set_active(false) -> another screen shown -> destroy() -> screen deleted
// set_active(false) must stop every task, timer, and network poll that could
// touch the app's widgets; destroy() then drops the app's widget pointers.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "home_logic.h"
#include "lvgl.h"

#define HOME_APPS_MAX 3   // rows the tray screen can show (home_layout.h)

typedef struct {
    const char *name;        // tray title, e.g. "Spotify"
    const char *icon;        // LV_SYMBOL_* glyph (Montserrat 20)
    const char *subtitle;    // one short line under the name

    void (*build)(lv_obj_t *screen);           // LVGL lock held: create widgets
    void (*refresh)(int64_t now_us);           // LVGL task, every 250 ms while shown
    void (*destroy)(void);                     // LVGL lock held: forget widgets
    void (*set_active)(bool active);           // any task: start / stop services
    void (*key)(home_key_t key, home_key_ev_t ev);  // input task: clicks
} home_app_t;

extern const home_app_t HOME_APPS[];
extern const int HOME_APP_COUNT;
