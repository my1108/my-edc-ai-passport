// main/home_ui.h - home, Spotify "Now Playing", and Setup screens.
//
// All three screens are built once and switched with lv_screen_load, so no
// screen is ever deleted while a timer can still reach it. A single LVGL timer
// (running inside the LVGL task, so it needs no extra lock) refreshes the home
// screen from the clock, home_net status, and the cached battery reading.
#pragma once

#include "home_logic.h"
#include "setup_logic.h"

// Build all screens. Caller must hold bsp_lvgl_lock().
void home_ui_create(void);

// Show a page. Caller must hold bsp_lvgl_lock().
void home_ui_show(home_page_t page);

// Pause the refresh timer while the screen sleeps; on wake, resume it and
// redraw at once so the first lit frame is current. Caller must hold
// bsp_lvgl_lock().
void home_ui_set_awake(bool awake);

// Store the latest battery state of charge (0..100, or -1 when unavailable).
// Safe from any task; the UI picks it up on its next refresh.
void home_ui_set_battery(int soc);

// Setup page: the hotspot name/password to show (from setup_portal_start).
// Call before showing HOME_PAGE_SETUP. Caller must hold bsp_lvgl_lock().
void home_ui_setup_open(const setup_info_t *info);

// Move to the previous (-1) or next (+1) QR card. Caller must hold bsp_lvgl_lock().
void home_ui_setup_step(int delta);
