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
