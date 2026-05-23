/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <M5Unified.hpp>
#include <mooncake_log.h>
#include <hal.h>
#include "apps/app_airmon/airmon.h"

extern "C" void app_main(void)
{
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    GetHAL().init();

    airmon_start();

    // All work is done by RTOS tasks; this task is no longer needed.
    vTaskDelete(nullptr);
}
