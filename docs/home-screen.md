<p align="right">
  <a href="home-screen.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Home Screen Application

The home screen turns AI Passport into a desk clock: a network-synced clock,
the local weather, battery level, and Wi-Fi state on one portrait screen. It
replaces the baseline hardware-test menu at startup.

## Screen and keys

| Area | Content |
| --- | --- |
| Status bar | Wi-Fi icon (dim while offline) on the left; battery percentage and icon from the CW2017 gauge on the right |
| Date | Accent-coloured line such as `SUN • 8 NOV` |
| Clock | Large `HH:MM` with small seconds; 24-hour by default, optional 12-hour with AM/PM |
| Weather card | Icon (clear, partly cloudy, cloudy, fog, rain, snow, storm; moon variants at night), temperature in °C, condition, and `City • H xx° L xx°` |
| Sync line | `Time synced today • HH:MM`, `Syncing time...`, or `Sync failed • retrying in 1 min` in orange |
| Key hints | `UP` Apps, `OK` Setup, `DOWN` Sync now |

`UP` opens the **Apps** tray. Choose an app with `UP` and `DOWN` (the
selection stops at the first and last entry instead of wrapping) and open it
with `OK`. Holding `OK` always goes back one level: from an app to the tray,
and from the tray to the home screen. The tray lists only installed apps; today
that is [Spotify Now Playing](spotify.md). `OK` opens
[Setup](#setup-from-a-phone), where a phone sets up Wi-Fi and Spotify.

| Where | `UP` | `DOWN` | `OK` click | `OK` hold |
| --- | --- | --- | --- | --- |
| Home | Apps tray | Sync now | Setup | - |
| Apps tray | Previous entry | Next entry | Open the app | Home |
| Spotify | Previous track | Next track | Play / pause | Apps tray |
| Setup | Previous QR card | Next QR card | Home | Home |

An app's screen exists only while the app is open. Opening it builds the
screen; leaving it first stops the app's services (Spotify stops polling) and
then deletes the screen, so only one app uses LVGL memory at a time. Adding an
app is one entry in `HOME_APPS` (`main/home_apps.c`): name, icon, subtitle, and
the build, refresh, destroy, start/stop, and key hooks. The tray has room for
three apps (`HOME_APPS_MAX`).

After 30 seconds without a key press the screen sleeps (backlight off). Any
key wakes it; that press only wakes the screen and does not open a page or
start a sync. The clock, Wi-Fi, and weather keep running while it sleeps.

After 10 minutes without a key press the board goes further, into deep sleep,
which turns Wi-Fi off and draws far less power. A single press of `UP`, `OK`,
or `DOWN` wakes it. The app then restarts: the screen is back in about 2 to
3 seconds and Wi-Fi, weather, and Spotify reconnect a few seconds later. That
press does nothing else. The clock keeps counting during deep sleep. Deep
sleep never starts while Setup is open, nor while a computer is connected over
USB (so the serial console keeps working); a plain USB charger still lets it
sleep.

The separate power button is wired to the power circuit, not to the chip, so
the firmware cannot change it. Hold it about 0.5 seconds to turn the board on
and about 2 seconds to turn it off.

The built-in Montserrat fonts have no middle dot (U+00B7) or ellipsis (U+2026),
so the screen uses a bullet (U+2022) and three periods instead. City names are
shown in ASCII; accented Latin letters lose their accents and other scripts
show as `?`.

## Network behaviour

- **Time**: SNTP at every boot once Wi-Fi is up, then daily at 03:00 local
  time. After a failure it retries in 1 minute, doubling with each further
  failure up to once an hour. `DOWN` syncs at once.
- **Location**: IP geolocation (default `https://ipwho.is/`) at boot, on `DOWN`,
  and daily. The provider sees the device's public IP address.
- **Weather**: [Open-Meteo](https://open-meteo.com/) (no API key) every
  30 minutes, 10 minutes after a failure. Offline or old readings are drawn grey.
- **Cache**: the last weather and location are stored in NVS and shown (grey)
  after a power loss until a new reading arrives. The clock itself has no
  backup battery and shows `--:--` after a power loss until the first sync.

Both requests use HTTPS with the ESP-IDF certificate bundle and responses are
read into one 1.5 KB buffer. The firmware logs free heap and the largest free
block before and after each request (`home_net` log tag) so the TLS memory
budget can be confirmed on the device.

## Setup from a phone

Press `OK` on the home screen to open **Setup**. It also opens by itself at
start-up when no Wi-Fi is saved. While Setup is open:

- the board runs a setup hotspot named `AI-Passport-XXXX` (the last four hex
  digits of its MAC) with a new random password each time;
- a web page for Wi-Fi and Spotify is served at `http://192.168.4.1/` on the
  hotspot and, once the board is online, at its address on your home network;
- the screen stays on and shows QR codes. `UP` and `DOWN` switch between
  cards, and `OK` closes Setup.

| Card | QR code | Use |
| --- | --- | --- |
| 1 Join the setup Wi-Fi | Joins the hotspot (Wi-Fi QR with name and password) | First setup, or when the board is offline |
| 2 Open the setup page | `http://192.168.4.1/` | Page over the hotspot |
| Setup page on home Wi-Fi | `http://<board IP>/` | Shown first once the board is online; needed for the Spotify login |

On the page, **Scan for networks**, choose your 2.4 GHz network, enter the
password, and press **Connect**. The board saves it and connects; its screen
then switches to the home-Wi-Fi card. Joining your router can move the
hotspot to the router's channel, so the phone may drop off the hotspot for a
moment; rejoin it if needed. Linking Spotify is described in
[Spotify Now Playing](spotify.md#link-your-account).

Setup closes when you press `OK` or after 15 minutes without a key press or
page activity, which also stops the hotspot and the web page. Nothing can
change the board's settings over the network while Setup is closed. Each
page load gets a session token that other web sites cannot read, so they
cannot post to the board while Setup is open.

## Settings over the serial console

The serial console is an alternative to the phone setup page. Wi-Fi
credentials are never compiled into the firmware. Connect over USB, open
a serial monitor (for example `idf.py -p <PORT> monitor`), and type:

```text
wifi "My Network" "my password"
status
```

| Command | Effect |
| --- | --- |
| `wifi "<ssid>" "<password>"` | Save a 2.4 GHz network and connect (quotes are required for spaces) |
| `wifi "<ssid>"` | Save an open network |
| `wifi clear` | Forget the network |
| `tz <POSIX TZ>` | Set the timezone, e.g. `tz <+08>-8` for UTC+8 or `tz CET-1CEST,M3.5.0,M10.5.0/3` |
| `tz default` | Return to the build default (`CONFIG_HOME_TZ_DEFAULT`, UTC+8) |
| `clock 12` / `clock 24` | Clock format |
| `sync` | Same as `DOWN` |
| `status` | Settings, network, sync, weather, and heap; the password is never printed |
| `spotify ...` | Link a Spotify account; see [Spotify Now Playing](spotify.md#link-your-account) |

Settings are stored in the NVS namespace `home`. The password is stored
without NVS encryption, so anyone with the device and a USB cable can read
flash. Flashing the merged image may reset NVS and erase these settings; see
[flashing and stored data](development/engineering/firmware-layout.md#flashing-and-stored-data).

## Build-time options

`idf.py menuconfig` → **AI Passport Home Screen** sets the default timezone,
default 12-hour mode, sync hour, SNTP server, geolocation URL, Open-Meteo base
URL, screen sleep delay (`CONFIG_HOME_SCREEN_SLEEP_S`, 0 = never), deep sleep
delay (`CONFIG_HOME_DEEP_SLEEP_MIN`, 0 = never), whether to stay awake while a
computer is connected (`CONFIG_HOME_DEEP_SLEEP_STAY_ON_USB`), and weather
refresh interval.

## Code map

| File | Role |
| --- | --- |
| `main/main.c` | Startup, key queue, battery polling |
| `main/home_ui.c` | Home, Apps tray, Spotify, and Setup screens; 250 ms refresh timer |
| `main/home_apps.c`, `main/home_apps.h` | App registry for the tray and the app lifecycle hooks |
| `main/home_net.c` | Wi-Fi station, SNTP, geolocation, weather worker task |
| `main/home_store.c` | NVS settings and cache |
| `main/home_console.c` | USB serial command console |
| `main/home_logic.c`, `main/home_json.c` | Pure logic covered by `tests/test_home_logic.c` and `tests/test_home_json.c` |
| `main/home_layout.h` | Screen geometry with compile-time overlap checks |
| `main/spot_*.c` | Spotify page; see [Spotify Now Playing](spotify.md#code-map) |
| `main/setup_portal.c`, `main/setup_page.html` | Setup hotspot and web page |
| `main/setup_logic.c` | Pure setup logic (PKCE, URL/form parsing, QR text), covered by `tests/test_setup_logic.c` |
