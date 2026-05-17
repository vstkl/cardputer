/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_live_data.h"
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <assets.h>
#include <hal.h>
#include <cstdlib>
using namespace mooncake;

// ---------------------------------------------------------------------------
// Label strings for the four data channels
// ---------------------------------------------------------------------------
const char* AppLiveData::_labels[4] = {
    "Channel A",
    "Channel B",
    "Channel C",
    "Channel D",
};

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
AppLiveData::AppLiveData()
{
    setAppInfo().name = "Live Data";
    // setAppInfo().userData = new AppIcon_t(icon_big..., icon_small...);
}

// ---------------------------------------------------------------------------
// onOpen — called once when the user launches the app
// ---------------------------------------------------------------------------
void AppLiveData::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    // Canvas setup — fixed layout, no scroll
    GetHAL().canvas.setBaseColor(THEME_COLOR_BG);
    GetHAL().canvas.setTextScroll(false);
    GetHAL().canvas.setFont(FONT_REPL);
    GetHAL().canvas.setTextSize(1);

    // Seed and populate initial values so the first frame is not all zeros
    _updateValues();
    _drawAll();

    // Reset the 1-second ticker
    _time_count = GetHAL().millis();

    // Key handler — any keypress closes the app (same feel as other apps)
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& keyEvent) {
        if (keyEvent.isModifier || keyEvent.state == false) {
            return;
        }
        // ESC / backtick on Cardputer layout = close
        if (keyEvent.keyCode == KEY_ESC || keyEvent.keyCode == '`') {
            audio::play_random_tone();
            close();
        }
    });
}

// ---------------------------------------------------------------------------
// onRunning — hot loop while the app is visible
// ---------------------------------------------------------------------------
void AppLiveData::onRunning()
{
    // Refresh data every 1000 ms
    if (GetHAL().millis() - _time_count >= 1000) {
        _updateValues();
        _drawAll();
        _time_count = GetHAL().millis();
    }

    // Home button closes the app
    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
    }
}

// ---------------------------------------------------------------------------
// onClose — called when the app exits
// ---------------------------------------------------------------------------
void AppLiveData::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
        _handle_key_event_slot_id = -1;
    }
}

// ---------------------------------------------------------------------------
// _updateValues — fill _values[] with fresh random numbers (0–9999)
// ---------------------------------------------------------------------------
void AppLiveData::_updateValues()
{
    for (int i = 0; i < 4; i++) {
        _values[i] = rand() % 10000;
    }
}

// ---------------------------------------------------------------------------
// _drawAll — clear the canvas and paint the full data panel
//
// Screen layout (320 × 240, FONT_REPL size 1 → ~8×16 px per char):
//
//   y=  4  ┌─ title bar ─────────────────────────────┐
//   y= 28  │  Channel A           1234                │
//   y= 68  │  Channel B           5678                │
//   y=108  │  Channel C           9012                │
//   y=148  │  Channel D           3456                │
//   y=200  │  [HOME / ESC to exit]                    │
//          └──────────────────────────────────────────┘
// ---------------------------------------------------------------------------
void AppLiveData::_drawAll()
{
    auto& cv = GetHAL().canvas;

    // --- Background ---
    cv.fillScreen(THEME_COLOR_BG);

    // --- Title bar ---
    cv.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    cv.setTextSize(2);
    cv.setCursor(8, 4);
    cv.print("Live Data");

    // Divider line under title
    cv.drawFastHLine(0, 26, 320, TFT_DARKGREY);

    // --- Four data rows ---
    cv.setTextSize(1);

    const int row_height  = 40;
    const int first_row_y = 36;
    const int label_x     = 8;
    const int value_x     = 220;  // right-aligned area start

    for (int i = 0; i < 4; i++) {
        int y = first_row_y + i * row_height;

        // Row separator (skip the first one — title line already there)
        if (i > 0) {
            cv.drawFastHLine(0, y - 4, 320, TFT_DARKGREY);
        }

        // Label in grey
        cv.setTextColor(TFT_LIGHTGREY, THEME_COLOR_BG);
        cv.setCursor(label_x, y + 4);
        cv.print(_labels[i]);

        // Value in bright green, right-aligned at value_x
        cv.setTextColor(TFT_GREEN, THEME_COLOR_BG);
        cv.setCursor(value_x, y + 4);
        cv.printf("%5d", _values[i]);
    }

    // --- Footer hint ---
    cv.drawFastHLine(0, 196, 320, TFT_DARKGREY);
    cv.setTextColor(TFT_DARKGREY, THEME_COLOR_BG);
    cv.setCursor(8, 200);
    cv.print("HOME / ESC  exit");

    GetHAL().pushCanvas();
}
