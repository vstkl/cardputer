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

static constexpr int SCREEN_W = 240;

static constexpr int Y_TITLE     = 2;
static constexpr int Y_SEP_TOP   = 20;
static constexpr int Y_ROW_START = 26;
static constexpr int ROW_STRIDE  = 20;
static constexpr int Y_SEP_BOT   = 106;
static constexpr int Y_HINT      = 110;

static constexpr int X_LABEL  = 6;
static constexpr int X_VALUE  = 160;
static constexpr int X_BADGE  = 190;

static constexpr uint32_t COL_TITLE = 0xFFFFFF;
static constexpr uint32_t COL_SEP   = 0x404040;
static constexpr uint32_t COL_LABEL = 0x8FC8AA;
static constexpr uint32_t COL_VALUE = 0x88AED9;
static constexpr uint32_t COL_OK    = 0x44CC44;
static constexpr uint32_t COL_WARN  = 0xFFAA00;
static constexpr uint32_t COL_CRIT  = 0xFF3333;
static constexpr uint32_t COL_DIM   = 0x606060;
static constexpr uint32_t COL_HINT  = 0x404040;

const char* AppLiveData::_labels[4] = {
    "eCO2  ppm",
    "TVOC  ppb",
    "Raw H2",
    "Raw EtOH",
};

AppLiveData::AppLiveData()
{
    setAppInfo().name = "Air Quality";
}

void AppLiveData::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    GetHAL().canvas.setBaseColor(THEME_COLOR_BG);
    GetHAL().canvas.setFont(FONT_REPL);
    GetHAL().canvas.setTextSize(1);

    _update_values();
    _render();
    _time_count = GetHAL().millis();
}

void AppLiveData::onRunning()
{
    uint32_t now = GetHAL().millis();

    if (now - _time_count >= 1000) {
        _update_values();
        _render();
        _time_count = now;
    }

    // Alarm beeps — triggers immediately on first entry (_alarm_beep_time=0),
    // then at the level's interval
    if (_alarm_level != ALARM_OK) {
        uint32_t interval = (_alarm_level == ALARM_CRITICAL) ? CRIT_BEEP_MS : WARN_BEEP_MS;
        if (now - _alarm_beep_time >= interval) {
            if (_alarm_level == ALARM_WARN) {
                // Gentle rising two-note warning: G4 → C5
                audio::play_melody({67, 72}, 0.15);
            } else {
                // Urgent triple staccato: C6 · · C6 · · C6
                audio::play_melody({84, -1, 84, -1, 84}, 0.08);
            }
            _alarm_beep_time = now;
        }
    }

    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
    }
}

void AppLiveData::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
        _handle_key_event_slot_id = -1;
    }
}

void AppLiveData::_update_values()
{
    if (!GetHAL().sgp30.isReady()) return;

    GetHAL().sgp30.update();
    auto d      = GetHAL().sgp30.getData();
    _values[0]  = d.eco2;
    _values[1]  = d.tvoc;
    _values[2]  = d.raw_h2;
    _values[3]  = d.raw_ethanol;
    _data_valid = d.valid;

    // Don't alarm on placeholder warm-up values (first 15 s)
    if (!d.valid) {
        _alarm_level = ALARM_OK;
        return;
    }

    if (d.eco2 >= ECO2_CRITICAL || d.tvoc >= TVOC_CRITICAL) {
        _alarm_level = ALARM_CRITICAL;
    } else if (d.eco2 >= ECO2_WARN || d.tvoc >= TVOC_WARN) {
        _alarm_level = ALARM_WARN;
    } else {
        _alarm_level = ALARM_OK;
    }
}

void AppLiveData::_render()
{
    GetHAL().canvas.fillScreen(THEME_COLOR_BG);

    // Title
    GetHAL().canvas.setTextColor(COL_TITLE);
    GetHAL().canvas.drawString("Air Quality", X_LABEL, Y_TITLE);

    // Status badge — top right
    const char* badge;
    uint32_t badge_color;
    if (!GetHAL().sgp30.isReady()) {
        badge = "NO SNS";
        badge_color = COL_DIM;
    } else if (!_data_valid) {
        badge = "INIT";
        badge_color = COL_DIM;
    } else {
        switch (_alarm_level) {
            case ALARM_WARN:
                badge = "WARN";
                badge_color = COL_WARN;
                break;
            case ALARM_CRITICAL:
                badge = "ALERT";
                badge_color = COL_CRIT;
                break;
            default:
                badge = "OK";
                badge_color = COL_OK;
                break;
        }
    }
    GetHAL().canvas.setTextColor(badge_color);
    GetHAL().canvas.drawString(badge, X_BADGE, Y_TITLE);

    // Separator lines
    GetHAL().canvas.fillRect(0, Y_SEP_TOP, SCREEN_W, 1, COL_SEP);
    GetHAL().canvas.fillRect(0, Y_SEP_BOT, SCREEN_W, 1, COL_SEP);

    if (!_data_valid) {
        GetHAL().canvas.setTextColor(COL_DIM);
        GetHAL().canvas.drawString("Warming up...", X_LABEL, Y_ROW_START + ROW_STRIDE);
    } else {
        for (int i = 0; i < 4; i++) {
            int y = Y_ROW_START + i * ROW_STRIDE;

            GetHAL().canvas.setTextColor(COL_LABEL);
            GetHAL().canvas.drawString(_labels[i], X_LABEL, y);

            // eCO2 and TVOC coloured by their individual threshold contribution
            uint32_t val_color = COL_VALUE;
            if (i == 0) {
                if (_values[0] >= ECO2_CRITICAL)      val_color = COL_CRIT;
                else if (_values[0] >= ECO2_WARN)     val_color = COL_WARN;
            } else if (i == 1) {
                if (_values[1] >= TVOC_CRITICAL)      val_color = COL_CRIT;
                else if (_values[1] >= TVOC_WARN)     val_color = COL_WARN;
            }

            GetHAL().canvas.setTextColor(val_color);
            _str_buffer = fmt::format("{:>5}", _values[i]);
            GetHAL().canvas.drawString(_str_buffer.c_str(), X_VALUE, y);
        }
    }

    GetHAL().canvas.setTextColor(COL_HINT);
    GetHAL().canvas.drawString("HOME  exit", X_LABEL, Y_HINT);

    GetHAL().pushCanvas();
}
