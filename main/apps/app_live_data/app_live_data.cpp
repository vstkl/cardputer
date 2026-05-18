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
using namespace mooncake;

// ---------------------------------------------------------------------------
// Screen constants — Cardputer ADV internal display
// ---------------------------------------------------------------------------
static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;

// FONT_REPL at textSize 1 → ~8 × 16 px per character cell
static constexpr int LINE_H = 16;

// ---------------------------------------------------------------------------
// Layout — derived purely from SCREEN_H and LINE_H so nothing overflows
//
//   y=  2  "Live Data"  (title)
//   y= 20  ── separator ──────────────────────────────
//   y= 26  Chan A        [label]       1234  [value]
//   y= 46  Chan B                      5678
//   y= 66  Chan C                      9012
//   y= 86  Chan D                      3456
//   y=106  ── separator ──────────────────────────────
//   y=110  HOME  exit  (hint)
// ---------------------------------------------------------------------------
static constexpr int Y_TITLE     = 2;
static constexpr int Y_SEP_TOP   = 20;
static constexpr int Y_ROW_START = 26;
static constexpr int ROW_STRIDE  = 20;  // 4 rows × 20 = 80 px; 26+80=106 < 135 ✓
static constexpr int Y_SEP_BOT   = 106;
static constexpr int Y_HINT      = 110;

static constexpr int X_LABEL = 6;
static constexpr int X_VALUE = 160;  // value starts ~2/3 across the 240px width

// Colours matching the IMU app's palette (0xRRGGBB hex literals)
static constexpr uint32_t COL_TITLE = 0xFFFFFF;  // white
static constexpr uint32_t COL_SEP   = 0x404040;  // dark grey
static constexpr uint32_t COL_LABEL = 0x8FC8AA;  // muted green (same as IMU accel)
static constexpr uint32_t COL_VALUE = 0x88AED9;  // muted blue  (same as IMU gyro)
static constexpr uint32_t COL_HINT  = 0x404040;  // dark grey

// ---------------------------------------------------------------------------
// Label strings — SGP30 signal names with units embedded
// ---------------------------------------------------------------------------
const char* AppLiveData::_labels[4] = {
    "eCO2 ppm",   // Equivalent CO2, derived from H2/EtOH  (400-60000 ppm)
    "TVOC ppb",   // Total Volatile Organic Compounds       (0-60000 ppb)
    "H2 raw",     // Raw H2 signal     (~13000 in clean air)
    "EtOH raw",   // Raw Ethanol signal (~18000 in clean air)
};

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
AppLiveData::AppLiveData()
{
    setAppInfo().name = "Live Data";
    // setAppInfo().userData = new AppIcon_t(image_data_live_data_big, image_data_live_data_small);
}

// ---------------------------------------------------------------------------
// onOpen
// ---------------------------------------------------------------------------
void AppLiveData::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    GetHAL().canvas.setBaseColor(THEME_COLOR_BG);
    GetHAL().canvas.setFont(FONT_REPL);
    GetHAL().canvas.setTextSize(1);

    // SGP30 on PORT.A: SDA=GPIO2, SCL=GPIO1, uses I2C_NUM_1
    _sgp30_ok = (_sgp30.begin(2, 1) == ESP_OK);
    if (!_sgp30_ok)
        mclog::tagWarn(getAppInfo().name, "SGP30 not found on PORT.A");

    _update_values();
    _render();
    _time_count = GetHAL().millis();
}

// ---------------------------------------------------------------------------
// onRunning
// ---------------------------------------------------------------------------
void AppLiveData::onRunning()
{
    if (GetHAL().millis() - _time_count >= 1000) {
        _update_values();
        _render();
        _time_count = GetHAL().millis();
    }

    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
    }
}

// ---------------------------------------------------------------------------
// onClose
// ---------------------------------------------------------------------------
void AppLiveData::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _sgp30.end();

    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
        _handle_key_event_slot_id = -1;
    }
}

// ---------------------------------------------------------------------------
// _update_values — poll the SGP30 sensor (must be called at ~1 Hz)
// ---------------------------------------------------------------------------
void AppLiveData::_update_values()
{
    if (!_sgp30_ok) return;
    _sgp30.update();
    _data = _sgp30.getData();
}

// ---------------------------------------------------------------------------
// _render — fillScreen → drawString calls → pushCanvas
// ---------------------------------------------------------------------------
void AppLiveData::_render()
{
    GetHAL().canvas.fillScreen(THEME_COLOR_BG);

    // Title — appends state hint when sensor is absent or still warming up
    GetHAL().canvas.setTextColor(COL_TITLE);
    if (!_sgp30_ok)
        GetHAL().canvas.drawString("Live Data - no sensor", X_LABEL, Y_TITLE);
    else if (!_data.valid)
        GetHAL().canvas.drawString("Live Data (init)", X_LABEL, Y_TITLE);
    else
        GetHAL().canvas.drawString("Live Data", X_LABEL, Y_TITLE);

    // Separator lines
    GetHAL().canvas.fillRect(0, Y_SEP_TOP, SCREEN_W, 1, COL_SEP);
    GetHAL().canvas.fillRect(0, Y_SEP_BOT, SCREEN_W, 1, COL_SEP);

    const uint16_t vals[4] = {_data.eco2, _data.tvoc, _data.raw_h2, _data.raw_ethanol};

    for (int i = 0; i < 4; i++) {
        int y = Y_ROW_START + i * ROW_STRIDE;

        // Label (includes unit for eCO2/TVOC rows)
        GetHAL().canvas.setTextColor(COL_LABEL);
        GetHAL().canvas.drawString(_labels[i], X_LABEL, y);

        // Value — "---" when no sensor; dimmed during the 15 s warm-up for
        // eCO2 and TVOC (which return placeholder 400/0 until valid==true)
        if (!_sgp30_ok) {
            GetHAL().canvas.setTextColor(COL_SEP);
            GetHAL().canvas.drawString("  ---", X_VALUE, y);
        } else {
            uint32_t col = (!_data.valid && i < 2) ? (uint32_t)0x506070 : COL_VALUE;
            GetHAL().canvas.setTextColor(col);
            _str_buffer = fmt::format("{:>5}", vals[i]);
            GetHAL().canvas.drawString(_str_buffer.c_str(), X_VALUE, y);
        }
    }

    // Hint
    GetHAL().canvas.setTextColor(COL_HINT);
    GetHAL().canvas.drawString("HOME  exit", X_LABEL, Y_HINT);

    GetHAL().pushCanvas();
}
