// main/home_layout.h - home-screen geometry for the 240 x 320 portrait panel.
//
// Rows from top to bottom: status bar, date, clock, weather card, sync line,
// button hints. The static asserts keep rows from overlapping when one is
// resized; tests/test_home_logic.c includes this header so they also run on
// the host.
#pragma once

#define HOME_SCREEN_W     240
#define HOME_SCREEN_H     320
// The BSP masks the frame to a 30 px corner radius; keep corner content inset.
#define HOME_GUTTER       20

#define HOME_STATUS_H     28
#define HOME_STATUS_TEXT_Y 7

#define HOME_DATE_Y       34
#define HOME_DATE_H       16    // Montserrat 14 line height

#define HOME_CLOCK_Y      52
#define HOME_CLOCK_H      50    // Montserrat 48 line height

#define HOME_CARD_X       12
#define HOME_CARD_Y       112
#define HOME_CARD_W       (HOME_SCREEN_W - 2 * HOME_CARD_X)
#define HOME_CARD_H       100
#define HOME_ICON_BOX     56    // weather icon drawing area inside the card

#define HOME_SYNC_Y       226
#define HOME_SYNC_H       16

#define HOME_HINTS_H      44
#define HOME_HINTS_Y      (HOME_SCREEN_H - HOME_HINTS_H)
#define HOME_HINT_COLS    3
#define HOME_HINT_COL_W   (HOME_SCREEN_W / HOME_HINT_COLS)

_Static_assert(HOME_DATE_Y >= HOME_STATUS_H, "date overlaps status bar");
_Static_assert(HOME_CLOCK_Y >= HOME_DATE_Y + HOME_DATE_H, "clock overlaps date");
_Static_assert(HOME_CARD_Y >= HOME_CLOCK_Y + HOME_CLOCK_H, "card overlaps clock");
_Static_assert(HOME_SYNC_Y >= HOME_CARD_Y + HOME_CARD_H + 8, "sync line overlaps card");
_Static_assert(HOME_SYNC_Y + HOME_SYNC_H + 8 <= HOME_HINTS_Y, "sync line overlaps hints");
_Static_assert(HOME_ICON_BOX + 24 < HOME_CARD_H, "icon leaves no room for location row");
_Static_assert(HOME_HINT_COL_W * HOME_HINT_COLS == HOME_SCREEN_W, "hint columns must tile the width");

// ---------------------------------------------------------------------------
// Spotify "Now Playing" page. Rows: status bar, playback line, equalizer,
// title (two lines), artist, progress bar, times, footer line, button hints.
// ---------------------------------------------------------------------------

#define SPOT_STATE_Y      36    // "PLAYING • device" or a toast
#define SPOT_STATE_H      16

#define SPOT_EQ_BARS      9
#define SPOT_EQ_BAR_W     8
#define SPOT_EQ_GAP       6
#define SPOT_EQ_W         (SPOT_EQ_BARS * SPOT_EQ_BAR_W + (SPOT_EQ_BARS - 1) * SPOT_EQ_GAP)
#define SPOT_EQ_X         ((HOME_SCREEN_W - SPOT_EQ_W) / 2)
#define SPOT_EQ_Y         62
#define SPOT_EQ_H         70    // tallest bar; bars grow up from the bottom
#define SPOT_EQ_MIN_H     4

// Album cover: replaces the equalizer while one is loaded.
#define SPOT_COVER_BOX    80    // SPOT_COVER_PX in spot_logic.h
#define SPOT_COVER_X      ((HOME_SCREEN_W - SPOT_COVER_BOX) / 2)
#define SPOT_COVER_Y      56

#define SPOT_TEXT_X       HOME_GUTTER
#define SPOT_TEXT_W       (HOME_SCREEN_W - 2 * HOME_GUTTER)
#define SPOT_TITLE_Y      144
#define SPOT_TITLE_H      48    // two lines of Montserrat 20 / Source Han Sans 16
#define SPOT_ARTIST_Y     194
#define SPOT_ARTIST_H     20    // one line; the CJK fallback is 20 px tall

#define SPOT_BAR_Y        222
#define SPOT_BAR_H        4
#define SPOT_TIME_Y       232
#define SPOT_TIME_H       16

#define SPOT_FOOT_Y       256   // "Hold OK for home", "Sending...", or a toast
#define SPOT_FOOT_H       16

// Message mode (not linked, offline, nothing playing, ...) reuses the text area.
#define SPOT_MSG_TITLE_Y  146
#define SPOT_MSG_DETAIL_Y 174
#define SPOT_MSG_LINES    5

_Static_assert(SPOT_STATE_Y >= HOME_STATUS_H, "Spotify state line overlaps status bar");
_Static_assert(SPOT_EQ_Y >= SPOT_STATE_Y + SPOT_STATE_H + 4, "equalizer overlaps state line");
_Static_assert(SPOT_TITLE_Y >= SPOT_EQ_Y + SPOT_EQ_H + 8, "title overlaps equalizer");
_Static_assert(SPOT_ARTIST_Y >= SPOT_TITLE_Y + SPOT_TITLE_H, "artist overlaps title");
_Static_assert(SPOT_BAR_Y >= SPOT_ARTIST_Y + SPOT_ARTIST_H + 4, "progress bar overlaps artist");
_Static_assert(SPOT_TIME_Y >= SPOT_BAR_Y + SPOT_BAR_H + 4, "times overlap progress bar");
_Static_assert(SPOT_FOOT_Y >= SPOT_TIME_Y + SPOT_TIME_H + 4, "footer overlaps times");
_Static_assert(SPOT_FOOT_Y + SPOT_FOOT_H <= HOME_HINTS_Y, "footer overlaps hints");
_Static_assert(SPOT_MSG_DETAIL_Y + SPOT_MSG_LINES * 16 <= SPOT_FOOT_Y, "message overlaps footer");
_Static_assert(SPOT_EQ_X >= HOME_GUTTER, "equalizer too wide for the rounded frame");
_Static_assert(SPOT_COVER_Y >= SPOT_STATE_Y + SPOT_STATE_H + 4, "cover overlaps state line");
_Static_assert(SPOT_TITLE_Y >= SPOT_COVER_Y + SPOT_COVER_BOX + 8, "title overlaps cover");

// ---------------------------------------------------------------------------
// Apps tray: one row per app (icon, name, subtitle), then a footer hint.
// ---------------------------------------------------------------------------

#define TRAY_ROW_X        12
#define TRAY_ROW_W        (HOME_SCREEN_W - 2 * TRAY_ROW_X)
#define TRAY_ROW_Y0       40
#define TRAY_ROW_H        56
#define TRAY_ROW_PITCH    64
#define TRAY_ROWS         3     // HOME_APPS_MAX in home_apps.h
#define TRAY_FOOT_Y       256

_Static_assert(TRAY_ROW_Y0 >= HOME_STATUS_H + 8, "tray rows overlap status bar");
_Static_assert(TRAY_ROW_Y0 + (TRAY_ROWS - 1) * TRAY_ROW_PITCH + TRAY_ROW_H + 8 <= TRAY_FOOT_Y,
               "tray rows overlap footer");
_Static_assert(TRAY_FOOT_Y + 18 <= HOME_HINTS_Y, "tray footer overlaps hints");

// ---------------------------------------------------------------------------
// Setup page: heading, QR code in a white quiet-zone box, two text lines,
// a status line, and the button hints.
// ---------------------------------------------------------------------------

#define SETUP_HEAD_Y      34
#define SETUP_HEAD_H      18
#define SETUP_QR_BOX      160
#define SETUP_QR_SIZE     144   // the box leaves an 8 px quiet zone on each side
#define SETUP_QR_X        ((HOME_SCREEN_W - SETUP_QR_BOX) / 2)
#define SETUP_QR_Y        54
#define SETUP_LINE1_Y     218
#define SETUP_LINE2_Y     236
#define SETUP_STATUS_Y    256

_Static_assert(SETUP_HEAD_Y >= HOME_STATUS_H, "setup heading overlaps status bar");
_Static_assert(SETUP_QR_Y >= SETUP_HEAD_Y + SETUP_HEAD_H, "QR overlaps heading");
_Static_assert(SETUP_QR_BOX - SETUP_QR_SIZE >= 16, "QR needs a quiet zone");
_Static_assert(SETUP_LINE1_Y >= SETUP_QR_Y + SETUP_QR_BOX + 4, "text overlaps QR");
_Static_assert(SETUP_LINE2_Y >= SETUP_LINE1_Y + 18, "setup lines overlap");
_Static_assert(SETUP_STATUS_Y >= SETUP_LINE2_Y + 18, "status overlaps text");
_Static_assert(SETUP_STATUS_Y + 18 <= HOME_HINTS_Y, "status overlaps hints");
