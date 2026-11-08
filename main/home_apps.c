// main/home_apps.c - see home_apps.h. To add an app, add one entry below.
#include "home_apps.h"

#include "home_ui.h"
#include "spot_net.h"

static void spotify_key(home_key_t key, home_key_ev_t ev) {
    spot_cmd_t cmd = home_spotify_key(key, ev);
    if (cmd != SPOT_CMD_NONE) spot_send(cmd);
}

const home_app_t HOME_APPS[] = {
    {
        .name = "Spotify",
        .icon = LV_SYMBOL_AUDIO,
        .subtitle = "Now playing + controls",
        .build = home_ui_spotify_build,
        .refresh = home_ui_spotify_refresh,
        .destroy = home_ui_spotify_destroy,
        // Polls Spotify only while its screen is open and lit (spot_net.c).
        .set_active = spot_set_active,
        .key = spotify_key,
    },
};

const int HOME_APP_COUNT = (int)(sizeof(HOME_APPS) / sizeof(HOME_APPS[0]));

_Static_assert(sizeof(HOME_APPS) / sizeof(HOME_APPS[0]) <= HOME_APPS_MAX,
               "more apps than the tray screen has rows");
