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
| Sync line | `Time synced today • HH:MM`, `Syncing time...`, or `Sync failed • retrying in 1 h` in orange |
| Key hints | `UP` Apps, `OK` Focus, `DOWN` Sync now |

On the Apps and Focus placeholder screens, `OK` returns home.

After 30 seconds without a key press the screen sleeps (backlight off). Any
key wakes it; that press only wakes the screen and does not open a page or
start a sync. The clock, Wi-Fi, and weather keep running while it sleeps.

The built-in Montserrat fonts have no middle dot (U+00B7) or ellipsis (U+2026),
so the screen uses a bullet (U+2022) and three periods instead. City names are
shown in ASCII; accented Latin letters lose their accents and other scripts
show as `?`.

## Network behaviour

- **Time**: SNTP at every boot once Wi-Fi is up, then daily at 03:00 local
  time; after a failure, retry every hour. `DOWN` syncs at once.
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

## Settings over the serial console

Wi-Fi credentials are never compiled into the firmware. Connect over USB, open
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

Settings are stored in the NVS namespace `home`. The password is stored
without NVS encryption, so anyone with the device and a USB cable can read
flash. Flashing the merged image may reset NVS and erase these settings; see
[flashing and stored data](development/engineering/firmware-layout.md#flashing-and-stored-data).

## Build-time options

`idf.py menuconfig` → **AI Passport Home Screen** sets the default timezone,
default 12-hour mode, sync hour, SNTP server, geolocation URL, Open-Meteo base
URL, screen sleep delay (`CONFIG_HOME_SCREEN_SLEEP_S`, 0 = never), and weather
refresh interval.

## Code map

| File | Role |
| --- | --- |
| `main/main.c` | Startup, key queue, battery polling |
| `main/home_ui.c` | Home, Apps, and Focus screens; 250 ms refresh timer |
| `main/home_net.c` | Wi-Fi station, SNTP, geolocation, weather worker task |
| `main/home_store.c` | NVS settings and cache |
| `main/home_console.c` | USB serial command console |
| `main/home_logic.c`, `main/home_json.c` | Pure logic covered by `tests/test_home_logic.c` and `tests/test_home_json.c` |
| `main/home_layout.h` | Screen geometry with compile-time overlap checks |
