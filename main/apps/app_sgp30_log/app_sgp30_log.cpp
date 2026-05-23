/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_sgp30_log.h"
#include <apps/utils/audio/audio.h>
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <hal.h>
#include <ctime>

using namespace mooncake;

// ---------------------------------------------------------------------------
// Layout constants — matching the live_data app's grid
// ---------------------------------------------------------------------------
static constexpr int SCREEN_W  = 240;
static constexpr int Y_TITLE   = 2;
static constexpr int Y_SEP_TOP = 20;
static constexpr int Y_ROW0    = 26;
static constexpr int ROW_H     = 20;
static constexpr int Y_SEP_BOT = 106;
static constexpr int Y_STATUS  = 110;
static constexpr int X_LABEL   = 6;
static constexpr int X_VALUE   = 155;

static constexpr uint32_t COL_TITLE = 0xFFFFFF;
static constexpr uint32_t COL_SEP   = 0x404040;
static constexpr uint32_t COL_LABEL = 0x8FC8AA;
static constexpr uint32_t COL_VALUE = 0x88AED9;
static constexpr uint32_t COL_OK    = 0x00CC44;
static constexpr uint32_t COL_ERR   = 0xCC4444;
static constexpr uint32_t COL_HINT  = 0x404040;

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
AppSgp30Log::AppSgp30Log()
{
    setAppInfo().name = "SGP30 Log";
}

// ---------------------------------------------------------------------------
// onOpen
// ---------------------------------------------------------------------------
void AppSgp30Log::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    GetHAL().canvas.setBaseColor(THEME_COLOR_BG);
    GetHAL().canvas.setFont(FONT_REPL);
    GetHAL().canvas.setTextSize(1);

    _sgp30.end();
    _sgp30_ok = (_sgp30.begin(2, 1) == ESP_OK);
    if (!_sgp30_ok)
        mclog::tagWarn(getAppInfo().name, "SGP30 not found on PORT.A");

    _init_log();

    // Initial sensor read so the first render shows live values
    if (_sgp30_ok) {
        _sgp30.update();
        _data = _sgp30.getData();
    }

    _render();
    _tick = GetHAL().millis();
}

// ---------------------------------------------------------------------------
// onRunning — poll at exactly 1 Hz (SGP30 baseline algorithm requires it)
// ---------------------------------------------------------------------------
void AppSgp30Log::onRunning()
{
    if (GetHAL().millis() - _tick >= 1000) {
        if (_sgp30_ok) {
            _sgp30.update();
            _data = _sgp30.getData();
        }
        _write_entry();
        _render();
        _tick = GetHAL().millis();
    }

    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
    }
}

// ---------------------------------------------------------------------------
// onClose
// ---------------------------------------------------------------------------
void AppSgp30Log::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    if (_log_file) {
        fclose(_log_file);
        _log_file = nullptr;
    }
    _sgp30.end();
}

// ---------------------------------------------------------------------------
// _init_log — mount SD card via HAL probe, then open the CSV log file
// ---------------------------------------------------------------------------
void AppSgp30Log::_init_log()
{
    auto probe = GetHAL().sdCardProbe();
    if (!probe.is_mounted) {
        mclog::tagWarn(getAppInfo().name, "SD card not available");
        _sd_ok = false;
        return;
    }

    // Write CSV header only when creating a new file
    bool is_new   = false;
    FILE* test_fp = fopen(LOG_PATH, "r");
    if (test_fp) {
        fclose(test_fp);
    } else {
        is_new = true;
    }

    _log_file = fopen(LOG_PATH, "a");
    if (!_log_file) {
        mclog::tagError(getAppInfo().name, "cannot open log file for writing");
        _sd_ok = false;
        return;
    }

    if (is_new) {
        fprintf(_log_file, "timestamp,uptime_ms,eco2_ppm,tvoc_ppb,raw_h2,raw_ethanol,valid\n");
        fflush(_log_file);
    }

    _sd_ok = true;
    mclog::tagInfo(getAppInfo().name, "logging to %s", LOG_PATH);
}

// ---------------------------------------------------------------------------
// _write_entry — append one CSV row; uses ISO-8601 if SNTP has synced,
//                otherwise "NO_SYNC"
// ---------------------------------------------------------------------------
void AppSgp30Log::_write_entry()
{
    if (!_log_file || !_sgp30_ok) return;

    char ts[24] = "NO_SYNC";
    time_t now;
    time(&now);
    if (now > 1000000000L) {  // sanity check: after year 2001
        struct tm ti;
        localtime_r(&now, &ti);
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &ti);
    }

    fprintf(_log_file, "%s,%lu,%u,%u,%u,%u,%d\n",
            ts,
            (unsigned long)GetHAL().millis(),
            _data.eco2,
            _data.tvoc,
            _data.raw_h2,
            _data.raw_ethanol,
            (int)_data.valid);
    fflush(_log_file);

    ++_log_count;
}

// ---------------------------------------------------------------------------
// _render — draw sensor values and logging status
// ---------------------------------------------------------------------------
void AppSgp30Log::_render()
{
    auto& cv = GetHAL().canvas;
    cv.fillScreen(THEME_COLOR_BG);

    // Title — annotate when sensor is missing or warming up
    cv.setTextColor(COL_TITLE);
    if (!_sgp30_ok)
        cv.drawString("SGP30 Log (no sensor)", X_LABEL, Y_TITLE);
    else if (!_data.valid)
        cv.drawString("SGP30 Log (init...)", X_LABEL, Y_TITLE);
    else
        cv.drawString("SGP30 Log", X_LABEL, Y_TITLE);

    cv.fillRect(0, Y_SEP_TOP, SCREEN_W, 1, COL_SEP);
    cv.fillRect(0, Y_SEP_BOT, SCREEN_W, 1, COL_SEP);

    struct Row {
        const char* label;
        uint16_t    value;
        bool        dim_if_invalid;
    };
    const Row rows[4] = {
        {"eCO2 ppm", _data.eco2,        true},
        {"TVOC ppb", _data.tvoc,        true},
        {"H2 raw",   _data.raw_h2,      false},
        {"EtOH raw", _data.raw_ethanol, false},
    };

    for (int i = 0; i < 4; i++) {
        int y = Y_ROW0 + i * ROW_H;
        cv.setTextColor(COL_LABEL);
        cv.drawString(rows[i].label, X_LABEL, y);

        if (!_sgp30_ok) {
            cv.setTextColor(COL_SEP);
            cv.drawString("  ---", X_VALUE, y);
        } else {
            uint32_t col = (rows[i].dim_if_invalid && !_data.valid) ? (uint32_t)0x506070 : COL_VALUE;
            cv.setTextColor(col);
            _str_buf = fmt::format("{:>5}", rows[i].value);
            cv.drawString(_str_buf.c_str(), X_VALUE, y);
        }
    }

    // Status bar: SD state + row counter on the left, HOME hint on the right
    if (_sd_ok) {
        cv.setTextColor(COL_OK);
        _str_buf = fmt::format("SD:OK #{}", _log_count);
    } else {
        cv.setTextColor(COL_ERR);
        _str_buf = "SD:ERR";
    }
    cv.drawString(_str_buf.c_str(), X_LABEL, Y_STATUS);

    cv.setTextColor(COL_HINT);
    cv.drawString("HOME exit", 163, Y_STATUS);

    GetHAL().pushCanvas();
}
