/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>

class AppLiveData : public mooncake::AppAbility {
public:
    AppLiveData();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    uint32_t _time_count          = 0;
    int _handle_key_event_slot_id = -1;
    int _values[4]                = {0};

    static const char* _labels[4];

    void _updateValues();
    void _drawAll();
};
