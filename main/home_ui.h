// main/home_ui.h - home, Apps, and Focus screens.
//
// All three screens are built once and switched with lv_screen_load, so no
// screen is ever deleted while a timer can still reach it. A single LVGL timer
// (running inside the LVGL task, so it needs no extra lock) refreshes the home
// screen from the clock, home_net status, and the cached battery reading.
#pragma once

#include "home_logic.h"

// Build all screens. Caller must hold bsp_lvgl_lock().
void home_ui_create(void);

// Show a page. Caller must hold bsp_lvgl_lock().
void home_ui_show(home_page_t page);

// Store the latest battery state of charge (0..100, or -1 when unavailable).
// Safe from any task; the UI picks it up on its next refresh.
void home_ui_set_battery(int soc);
