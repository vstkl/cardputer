/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <sgp30.hpp>
#include <string>

class AppLiveData : public mooncake::AppAbility {
public:
    AppLiveData();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    uint32_t _time_count          = 0;
    int _handle_key_event_slot_id = -1;
    std::string _str_buffer;

    SGP30 _sgp30;
    bool _sgp30_ok    = false;
    SGP30::Data _data = {};

    static const char* _labels[4];

    void _update_values();
    void _render();
};
