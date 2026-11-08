// main/spot_net.h - Spotify Web API client for the "Now Playing" page.
//
// Runs inside the home_net worker task, so all HTTPS traffic stays on one task
// and one TLS session at a time. Other tasks only read a status snapshot, mark
// the page active, or queue a player command; nothing here blocks the caller.
//
// Auth: a refresh token from the Authorization Code with PKCE flow (run once on
// a computer with tools/spotify_auth.py) and the app's client ID are set over
// the serial console and kept in NVS. Access tokens live only in RAM.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "spot_logic.h"

typedef struct {
    spot_link_t link;
    bool active;               // page visible and screen awake
    bool fetched;              // playback state received since the page opened
    spot_result_t last;        // result of the last poll
    spot_now_t now;            // last playback state
    int64_t now_at_us;         // monotonic time of that reading
    bool busy;                 // a player command is waiting or in flight
    const char *toast;         // static message for the last failed command, or NULL
    int64_t toast_at_us;       // monotonic time of that message
    bool cover_ready;          // album cover pixels are valid (spot_cover_pixels)
    uint32_t cover_seq;        // changes each time a new cover is decoded
} spot_status_t;

// Loads the stored link. Call once from home_net_start, before the worker runs.
void spot_net_init(void);

// Copies the current status. Safe from any task.
void spot_get_status(spot_status_t *out);

// The page became visible (and the screen is awake) or not. Polling runs only
// while active. Safe from any task.
void spot_set_active(bool active);

// Queues a player command; a newer command replaces one not yet sent. Safe
// from any task.
void spot_send(spot_cmd_t cmd);

// The stored client ID or refresh token changed (serial console).
void spot_link_changed(void);

// Finishes a login from the phone setup page: trades the authorization code
// for tokens (PKCE, with the verifier the page's login started with), then
// saves the client ID and refresh token. Blocks the caller for up to
// `timeout_ms` while the home_net worker talks to Spotify; never call it from
// the LVGL task or a button callback. Returns NULL on success, else a static
// message for the page.
const char *spot_link_code(const char *client_id, const char *code, const char *verifier, int timeout_ms);

// Album cover as RGB565 pixels (w x h, at most SPOT_COVER_PX square) while
// status.cover_ready is true; NULL otherwise. The buffer stays allocated, but
// the worker rewrites it for the next album after calling the detach hook.
const uint16_t *spot_cover_pixels(int *w, int *h);

// Registers the UI hook the worker calls (with no Spotify lock held) before it
// rewrites the cover pixels. The hook must make sure the screen no longer
// draws them (take the LVGL lock, hide the image) and return true, or return
// false to skip this download for now.
void spot_set_cover_detach(bool (*detach)(void));

// --- home_net worker task only ---

// Monotonic time of the next due action, INT64_MAX when nothing is due.
int64_t spot_net_due_us(int64_t now_us);

// Performs at most one due action (token refresh, command, or poll).
void spot_net_run(int64_t now_us);

// Wi-Fi is down: fail queued commands and drop the kept-alive connection.
void spot_net_offline(void);

// Closes the kept-alive API connection (before other HTTPS requests, so only
// one TLS session holds memory at a time).
void spot_net_release(void);
