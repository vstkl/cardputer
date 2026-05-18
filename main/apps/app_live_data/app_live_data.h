/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <string>

class AppLiveData : public mooncake::AppAbility {
public:
    AppLiveData();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum AlarmLevel { ALARM_OK, ALARM_WARN, ALARM_CRITICAL };

    // eCO2 (ppm) and TVOC (ppb) thresholds — either channel triggers the level
    static constexpr uint16_t ECO2_WARN     = 1500;
    static constexpr uint16_t ECO2_CRITICAL = 3000;
    static constexpr uint16_t TVOC_WARN     = 500;
    static constexpr uint16_t TVOC_CRITICAL = 2000;

    static constexpr uint32_t WARN_BEEP_MS = 10000;  // beep every 10 s at warning
    static constexpr uint32_t CRIT_BEEP_MS = 3000;   // beep every 3 s at critical

    uint32_t _time_count          = 0;
    uint32_t _alarm_beep_time     = 0;
    int _handle_key_event_slot_id = -1;
    int _values[4]                = {0};
    bool _data_valid              = false;
    AlarmLevel _alarm_level       = ALARM_OK;
    std::string _str_buffer;

    static const char* _labels[4];

    void _update_values();
    void _render();
};
