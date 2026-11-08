// main/setup_portal.h - phone setup page: hotspot + small web server.
//
// Runs only while the Setup screen is open on the board, so changing Wi-Fi or
// the Spotify login needs someone at the device. While running:
//   - a WPA2 hotspot "AI-Passport-XXXX" with a fresh random password (shown
//     on the screen as a QR code) serves the page at http://192.168.4.1/;
//   - the same page is reachable on the home network at the board's IP.
// The page saves Wi-Fi credentials and links Spotify (PKCE; the board keeps
// the verifier and trades the code itself). API calls need a per-session
// token that only same-origin scripts can read, which blocks other web pages
// from posting to the board.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "setup_logic.h"

// Starts the hotspot and web server. Fills the hotspot name and password.
// Call from a task that may block briefly (not the LVGL task).
esp_err_t setup_portal_start(setup_info_t *info);

// Stops the web server and hotspot and forgets any half-finished login.
void setup_portal_stop(void);

bool setup_portal_running(void);

// Latest event for the Setup screen ("Phone connected", "Wi-Fi saved...",
// "Spotify linked"), a static string or NULL, with its monotonic time.
const char *setup_portal_event(int64_t *at_us);
