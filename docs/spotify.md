<p align="right">
  <a href="spotify.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Spotify Now Playing

The Spotify page of the [home screen](home-screen.md) shows the track playing
on your Spotify account and lets the three buttons control playback. Press
`UP` on the home screen to open the Apps tray, then `OK` on **Spotify**.
Spotify is polled only while this page is open and the screen is on; leaving
the app stops polling and frees its screen and cover memory, while playback
carries on in your Spotify app. Reopening it shows `Loading` for a moment if
the last reading is more than 30 seconds old.

## Screen and keys

| Area | Content |
| --- | --- |
| Status bar | `NOW PLAYING` on the left, battery on the right |
| State line | `PLAYING • <device>` or `PAUSED • <device>` in green; `RECONNECTING`, `OFFLINE`, or `SPOTIFY BUSY` in orange |
| Album cover | The album (or podcast) art, up to 80 x 80 px, centred under the state line |
| Equalizer | Shown instead of the cover while it loads or when there is none: green bars that move while music plays, flat and grey when paused or idle |
| Title | Up to two lines, cut with `...` |
| Artist | Up to three artists (or the podcast name), one line |
| Progress | Bar plus elapsed and total time (`1:23` / `3:45`), advanced locally between polls |
| Footer | `Hold OK for apps`; `Sending...` while a command is in flight; a command error such as `No active device` for 3 s |
| Key hints | `UP` Prev, `OK` Play or Pause, `DOWN` Next |

| Key | Action |
| --- | --- |
| `UP` click | Previous track (`POST /v1/me/player/previous`) |
| `DOWN` click | Next track (`POST /v1/me/player/next`) |
| `OK` click | Pause when playing, otherwise resume (`PUT /v1/me/player/pause` or `/play`) |
| `OK` long press | Back to the Apps tray |

A quick double press counts as one press, as elsewhere on the home screen. The
screen still sleeps after 30 seconds without a key; the key that wakes it does
nothing else.

When there is no track to show, the page shows a message instead: `Link
Spotify` (no account linked), `Link expired`, `No Wi-Fi`, `Offline`,
`Loading`, `Nothing playing`, `Access denied`, `Premium needed`, `Spotify is
busy`, or `Can't reach Spotify`.

## Requirements

- **Spotify Premium.** Since February 2026 every Development Mode app needs
  its owner to have Premium, and the playback-control endpoints need Premium
  too. Without it the page can show `Premium needed`, and buttons report
  `Needs Spotify Premium`.
- **An active Spotify device.** The board is a remote control, not a player.
  Start Spotify on a phone, computer, or speaker first; with nothing active,
  the page shows `Nothing playing` and buttons report `No active device`.
- **Development Mode limits.** A personal app stays in Development Mode: up to
  5 Spotify accounts, each added by hand under **User Management**; the
  request quota is shared by all of the developer's Development Mode apps.
- Wi-Fi set up with the [Setup page](home-screen.md#setup-from-a-phone) or the serial console.

## Set up the Spotify app (once)

1. Open the [Spotify developer dashboard](https://developer.spotify.com/dashboard)
   and sign in with the account that will own the app (it needs Premium).
2. **Create app.** Any name and description, for example `AI Passport`.
3. **Redirect URIs:** add exactly `http://127.0.0.1:8888/callback`. Spotify
   rejects `localhost`; use the loopback IP. After saving, reload the page and
   check that it was not rewritten to `localhost`.
4. **Which API/SDKs are you planning to use:** tick **Web API**. Accept the
   terms and save.
5. Open the app's **Settings** and copy the **Client ID** (32 hex digits).
   The client secret is not used: the device and the setup tool use PKCE.
6. Under **User Management**, add the name and email of every Spotify account
   that will be linked, including your own if it is not already listed.

## Link your account

### From a phone (Setup page)

1. Make sure the board is online: press `OK` on the home screen to open
   Setup. If Wi-Fi is not set yet, scan card 1 to join the setup hotspot and
   connect the board to your Wi-Fi first (see
   [Setup from a phone](home-screen.md#setup-from-a-phone)).
2. With the phone back on your home Wi-Fi, scan the **Setup page on home
   Wi-Fi** card (the login needs internet, which the setup hotspot does not
   have).
3. Enter the **Client ID** and press **Log in with Spotify**, then **Open the
   Spotify login**. It opens in a new tab and asks for these scopes only:

| Scope | Used for |
| --- | --- |
| `user-read-playback-state` | `GET /v1/me/player`: track, progress, device |
| `user-read-currently-playing` | Same data; requested for compatibility |
| `user-modify-playback-state` | Previous, next, play, and pause |

4. Approve. The tab then tries to open `http://127.0.0.1:8888/callback?...`
   and shows a "can't connect" page. This is expected: Spotify accepts only
   loopback addresses for plain-HTTP redirects, and the board cannot have a
   public HTTPS address.
5. Copy that tab's whole address, switch back to the setup page, paste it,
   and press **Finish login**. The board checks that the address belongs to
   the login it started, trades the code for tokens itself (PKCE: the secret
   verifier never leaves the board), and saves the result. Codes work once and
   expire after a few minutes; if it fails, press **Log in with Spotify**
   again.

The same page shows the link state and has **Forget Spotify**.

### From a computer (serial console)

Run the setup tool on a computer with a browser (Python 3.8 or newer, no
extra packages):

```bash
python3 tools/spotify_auth.py --client-id <your client ID>
```

It asks for the same scopes and prints two lines. Paste them into the
device's serial console (for example `idf.py -p COM3 monitor`):

```text
spotify client <client ID>
spotify token <refresh token>
```

Then open Apps (`UP` on the home screen) and choose Spotify. Type `spotify` in the console to see the
link state, the last poll result, and the current track in full UTF-8.

| Command | Effect |
| --- | --- |
| `spotify client <id>` | Save the app's client ID |
| `spotify token <refresh>` | Save the refresh token from `spotify_auth.py` |
| `spotify clear` | Forget both |
| `spotify` / `spotify status` | Link state, last result, current track; the token is never printed |

The refresh token is a password for your Spotify account. It is stored in the
NVS namespace `home` (key `sp_rt`) without NVS encryption, so anyone with the
board and a USB cable can read it. It is never compiled into the firmware,
logged, or printed. To revoke it, remove the app under
[Spotify account → Manage apps](https://www.spotify.com/account/apps/) and run
`spotify clear`.

## How it works

### Authorization

The board cannot show a browser login, so the Authorization Code flow with
PKCE runs once in a phone or computer browser: from the Setup page (the board
creates the verifier and trades the code itself) or with
`tools/spotify_auth.py`. The device then keeps only the client ID and the
refresh token. It trades the refresh token for an
access token at `https://accounts.spotify.com/api/token` (PKCE public client:
`client_id` in the body, no secret). The access token lasts about an hour and
lives only in RAM; the device renews it two minutes before it expires, or at
once after a `401`. When Spotify returns a new refresh token, the device saves
it to NVS before using the new access token.

If Spotify answers `invalid_grant`, `invalid_client`, or
`unauthorized_client` (token revoked, app deleted, or the refresh token's
lifetime ran out), the device erases the stored refresh token and shows `Link
expired`. Log in again from Setup. Network or server errors while renewing
are retried after 10 s, doubling up to 5 minutes.

### Polling and rate limits

The device polls `GET /v1/me/player?additional_types=episode` only while the
Spotify page is open and the screen is awake:

| State | Next poll |
| --- | --- |
| Playing | 5 s, or 0.7 s after the current track should end (at least 1 s) |
| Paused | 10 s |
| Nothing playing (`204`), no active device | 15 s |
| After a successful button command | 0.7 s |
| Network or server error | 5 s, doubling up to 60 s |
| `429` Too Many Requests | the `Retry-After` header (1 to 600 s; 30 s if missing) |
| `401` after a renewal, `403`, other errors | 60 s |

Opening the page polls at once. A reading older than 30 s is not shown on
reopening; the page shows `Loading` until the first new reply. While the page
is hidden or the screen sleeps, no Spotify requests are made and the
kept-alive connection is closed. Playing continuously, that is about 12
requests a minute. Spotify does not publish a number for its limit (it counts
requests in a rolling 30-second window per app), so the device obeys
`Retry-After` whenever it gets a `429`.

Button presses are queued; a press made before the previous one was sent
replaces it. Play/pause flips the state on screen at once and the next poll
confirms it.

### Memory

The board has no PSRAM. A `/v1/me/player` reply is several kilobytes (album
images, artists, device, context). It is never held in memory whole: the
HTTP body is read in 512-byte chunks into a streaming JSON scanner
(`main/spot_json.c`) that tracks the current key path (for example
`item.artists.0.name`) in a 96-byte buffer and keeps only the fields the page
draws. The scanner, parser, and parsed result take under 2 KB of static RAM.
Error and token replies are small and use a 1.5 KB buffer. The Web API has no
`fields` filter for this endpoint, so streaming is the way to bound memory.

All HTTPS traffic, Spotify included, runs on the single `home_net` worker task.
The connection to `api.spotify.com` is kept alive between polls to avoid a TLS
handshake every few seconds, and is closed before the token request, before
any geolocation or weather request, when Wi-Fi drops, and when the page closes.
So at most one TLS session holds memory at a time. Free heap is logged
around weather requests (`home_net` tag) and after every new Spotify
connection, cover decode, and cover release (`spotify` tag, for example
`heap after cover connect: free=... largest=... min_ever=...`). `min_ever`
only falls, so the first line where it drops names the request that set the
low point. Polls on a kept-alive connection are not logged.

### Album cover

Spotify lists each album's art as JPEGs of about 640, 300, and 64 px at
`i.scdn.co`. A 300 px image decoded in full would need 180 KB as RGB565, far
more than this board can spare. Instead, `spot_cover_pick()` chooses the image
whose decoded size comes closest to 80 px, preferring the smaller download
when two are within 8 px. In practice that is the 300 px image, decoded at
1/4 scale to 75 x 75. The 640 px image at 1/8 would give 80 x 80 for a
download several times larger.

- **Decoder.** The JPEG goes straight from the HTTP body into the ESP32-C3's
  ROM TJpgDec, which needs a 3.2 KB work area during the decode. The whole
  JPEG is never held in memory.
- **Pixels.** They are written to one 12.8 KB RGB565 buffer, allocated when
  the page needs a cover and freed when the page closes. Other pages (the
  home screen's weather requests, Setup) get that memory back, at the cost of
  one cover download (about 25 KB) each time the page opens.
- **When it downloads.** Only while the page is open, only when the cover URL
  changes (a new album, not every track or poll), and only after closing the
  API connection, so still only one TLS session is open at a time.
- **Switching albums.** While the next album's cover downloads (under a
  second on a good link), the page shows the equalizer instead of the old
  cover, which would belong to a different album. Keeping the old picture
  until the new one is ready would need a second 12.8 KB buffer, and free
  heap already drops to about 60 KB during HTTPS.
- **Failures.** If a download or decode fails (offline, progressive JPEG, or
  too large), the equalizer stays and that URL is retried after 2 minutes.

### Text and fonts

Titles and artists are shown with Montserrat (20 px title, 14 px artist) and
LVGL's built-in Source Han Sans SC 16 subset as a fallback
(`CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_16_CJK`). That subset holds about 1,000
common Chinese characters (simplified and traditional), Japanese kana, and
ASCII. It does not cover all Chinese text, nor Korean.

Before drawing, `spot_text_for_font()` checks each character against the
label's actual font chain:

- characters the fonts can draw are kept;
- accented Latin letters lose their accents (`Beyoncé` → `Beyonce`);
- typographic quotes, dashes, ellipses, full-width ASCII, and CJK punctuation
  become ASCII;
- everything else becomes `?`.

The serial `spotify` command prints the original UTF-8, so a title shown with
`?` can still be read there. Full coverage of Chinese, Japanese, or Korean
titles needs a larger generated font subset (see
[LVGL Chinese fonts](development/engineering/lvgl-chinese-fonts.md#4-option-b-generate-a-reproducible-application-subset)),
which needs a licensed font file and `lv_font_conv`; it is not included yet.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| `Link Spotify` | Link from the Setup page, or paste both `spotify client` and `spotify token` lines |
| Setup page says the login needs internet | The phone is on the setup hotspot; switch it to your home Wi-Fi and use the home-Wi-Fi card |
| `That address is from an older login` | Use the tab from the newest **Log in with Spotify** press |
| `Spotify rejected the code` | The code was used already or expired; log in again and paste within a few minutes |
| `Access denied` | Add the account under **User Management**; the app owner needs Premium |
| `Premium needed` / `Needs Spotify Premium` | The account or the app owner has no Premium |
| `Nothing playing` / `No active device` | Start playback in a Spotify app first; the board cannot start a device by itself |
| `Link expired` | The refresh token was revoked or expired; log in again from Setup |
| `Spotify is busy` | Rate limited; it waits for `Retry-After`. Close other apps that use the same developer account's quota |
| `INVALID_CLIENT: Invalid redirect URI` in the browser | The dashboard redirect URI must be exactly `http://127.0.0.1:8888/callback` |
| Titles with `?` | Characters outside the built-in CJK subset; read them with the `spotify` command |

## Code map

| File | Role |
| --- | --- |
| `main/spot_net.c` | Token renewal, polling, player commands; runs in the `home_net` task |
| `main/spot_logic.c` | Pure logic: playback-state parsing, token replies, HTTP result classes, poll timing, page messages, text folding |
| `main/spot_json.c` | Streaming JSON scanner with fixed memory |
| `main/spot_net.c` (`do_cover`) | Album cover download and ROM TJpgDec decode |
| `main/home_ui.c` | The Now Playing screen (`build_spotify`, `refresh_spotify`) |
| `main/home_logic.c` | Key map (`home_nav_handle`) and `spotify` console commands |
| `main/home_store.c` | NVS keys `sp_cid` and `sp_rt` |
| `main/setup_portal.c`, `main/setup_logic.c` | Setup page: login start, pasted-address check, code exchange request |
| `tools/spotify_auth.py` | Alternative one-time PKCE login on a computer |
| `tests/test_spot_logic.c` | Host tests for the scanner and logic, run by `tools/validate.sh` |
