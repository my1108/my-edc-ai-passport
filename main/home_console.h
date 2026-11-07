// main/home_console.h - line-based settings console on the USB-Serial-JTAG port.
//
// Lets the user provision Wi-Fi and change the timezone / clock format from
// `idf.py monitor` (or any serial terminal), so credentials are never compiled
// into the firmware. Commands (see `help`):
//   wifi "<ssid>" "<password>"   wifi clear
//   tz <POSIX TZ>                tz default
//   clock 12 | clock 24          sync          status
#pragma once

#include "esp_err.h"

esp_err_t home_console_start(void);
