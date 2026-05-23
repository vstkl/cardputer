/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <sgp30.hpp>
#include <cstdio>
#include <string>

class AppSgp30Log : public mooncake::AppAbility {
public:
    AppSgp30Log();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    uint32_t _tick      = 0;
    SGP30 _sgp30;
    bool _sgp30_ok      = false;
    bool _sd_ok         = false;
    uint32_t _log_count = 0;
    SGP30::Data _data   = {};
    FILE* _log_file     = nullptr;
    std::string _str_buf;

    static constexpr const char* LOG_PATH = "/sdcard/sgp30_log.csv";

    void _init_log();
    void _write_entry();
    void _render();
};
