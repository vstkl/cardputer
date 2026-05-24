/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "airmon.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal.h>
#include <mooncake_log.h>
#include <esp_sntp.h>
#include <cstdio>
#include <cstring>
#include <ctime>

static const char* TAG = "ntp";

// wifi.txt on the SD card — three lines (third is optional):
//   line 1: SSID
//   line 2: password
//   line 3: POSIX TZ string (e.g. "CET-1CEST,M3.5.0,M10.5.0/3"), default "UTC0"
static constexpr const char* CRED_PATH      = "/sdcard/wifi.txt";
static constexpr uint32_t    SD_WAIT_MS     = 30000;
static constexpr uint32_t    NTP_TIMEOUT_MS = 20000;

static bool read_creds(char* ssid, size_t ss, char* pass, size_t ps, char* tz, size_t ts)
{
    FILE* f = fopen(CRED_PATH, "r");
    if (!f) {
        mclog::tagInfo(TAG, "no %s — skipping NTP sync", CRED_PATH);
        return false;
    }

    bool ok     = fgets(ssid, ss, f) && fgets(pass, ps, f);
    bool has_tz = ok && fgets(tz, ts, f);
    fclose(f);

    auto strip = [](char* s) { s[strcspn(s, "\r\n")] = '\0'; };
    strip(ssid);
    strip(pass);
    if (has_tz) {
        strip(tz);
        if (tz[0] == '\0') strncpy(tz, "UTC0", ts);
    } else {
        strncpy(tz, "UTC0", ts);
    }

    if (!ok || ssid[0] == '\0') {
        mclog::tagWarn(TAG, "wifi.txt: expected SSID on line 1, password on line 2");
        return false;
    }

    mclog::tagInfo(TAG, "wifi.txt: ssid='%s' tz='%s'", ssid, tz);
    return true;
}

void ntp_task(void*)
{
    // SD card must be mounted before we can read wifi.txt.
    EventBits_t bits = xEventGroupWaitBits(
        g_events, EV_SD_MOUNTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(SD_WAIT_MS));

    if (!(bits & EV_SD_MOUNTED)) {
        mclog::tagWarn(TAG, "SD not ready after %u ms — skipping NTP sync", SD_WAIT_MS);
        vTaskDelete(nullptr);
        return;
    }

    char ssid[64], pass[64], tz_str[64];
    if (!read_creds(ssid, sizeof(ssid), pass, sizeof(pass), tz_str, sizeof(tz_str))) {
        vTaskDelete(nullptr);
        return;
    }

    Hal& hal = GetHAL();
    hal.wifiInit();

    // wifiConnect also calls start_sntp() which sets timezone to UTC0 (changed from CST-8).
    bool connected = hal.wifiConnect(ssid, pass);
    if (!connected) {
        mclog::tagError(TAG, "WiFi connect failed — skipping NTP sync");
        hal.wifiDeinit();
        vTaskDelete(nullptr);
        return;
    }

    // Apply user timezone (overrides the default set in start_sntp).
    setenv("TZ", tz_str, 1);
    tzset();

    // Wait for SNTP to set the system clock.
    mclog::tagInfo(TAG, "waiting for NTP sync (TZ=%s)...", tz_str);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(NTP_TIMEOUT_MS);
    bool synced = false;

    while (xTaskGetTickCount() < deadline) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            synced = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (synced) {
        time_t now;
        time(&now);
        struct tm ti;
        localtime_r(&now, &ti);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S %Z", &ti);
        mclog::tagInfo(TAG, "NTP synced: %s", ts);
        xEventGroupSetBits(g_events, EV_TIME_SYNCED);
    } else {
        mclog::tagWarn(TAG, "NTP sync timed out after %u ms", NTP_TIMEOUT_MS);
    }

    // Power down WiFi — the system RTC keeps running after SNTP stops.
    hal.wifiDisconnect();
    hal.wifiDeinit();

    vTaskDelete(nullptr);
}
