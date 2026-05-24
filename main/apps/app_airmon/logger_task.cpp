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
#include <esp_timer.h>
#include <ctime>
#include <cstdio>
#include <unistd.h>

static const char* TAG = "logger";

static constexpr const char* LOG_PATH       = "/sdcard/airmon.csv";
static constexpr uint32_t    SD_RETRY_MS    = 5000; // probe interval when SD absent

// ─────────────────────────────────────────────────────────────────────────────
static FILE* open_log()
{
    FILE* probe = fopen(LOG_PATH, "r");
    bool  is_new = (probe == nullptr);
    if (probe) fclose(probe);

    FILE* fp = fopen(LOG_PATH, "a");
    if (!fp) {
        mclog::tagError(TAG, "cannot open %s", LOG_PATH);
        return nullptr;
    }

    if (is_new) {
        fprintf(fp, "seq,timestamp,uptime_ms,eco2_ppm,tvoc_ppb,raw_h2,raw_ethanol,valid,alert_level\n");
        fflush(fp);
        mclog::tagInfo(TAG, "created log file with header");
    }

    return fp;
}

// ─────────────────────────────────────────────────────────────────────────────
void logger_task(void*)
{
    // ── SD card mount (lazy, via HAL) ─────────────────────────────────────────
    while (true) {
        auto probe = GetHAL().sdCardProbe();
        if (probe.is_mounted) {
            mclog::tagInfo(TAG, "SD mounted: %s %s %s", probe.type.c_str(),
                           probe.size.c_str(), probe.name.c_str());
            xEventGroupSetBits(g_events, EV_SD_MOUNTED);
            break;
        }
        mclog::tagWarn(TAG, "SD not present — retry in %u ms", SD_RETRY_MS);
        vTaskDelay(pdMS_TO_TICKS(SD_RETRY_MS));
    }

    FILE* fp = open_log();

    for (;;) {
        SensorSnapshot entry;

        if (xQueueReceive(g_log_queue, &entry, pdMS_TO_TICKS(SD_RETRY_MS)) != pdTRUE) {
            continue;
        }

        // ── Re-open log if it was closed after a write error ──────────────────
        if (!fp) {
            auto probe = GetHAL().sdCardProbe();
            if (!probe.is_mounted) {
                mclog::tagWarn(TAG, "SD removed — waiting for re-insert");
                xEventGroupClearBits(g_events, EV_SD_MOUNTED);
                // Drain the queue so it does not back up the sensor task.
                SensorSnapshot discard;
                while (xQueueReceive(g_log_queue, &discard, 0) == pdTRUE) {}
                vTaskDelay(pdMS_TO_TICKS(SD_RETRY_MS));

                probe = GetHAL().sdCardProbe();
                if (!probe.is_mounted) continue;
                xEventGroupSetBits(g_events, EV_SD_MOUNTED);
            }
            fp = open_log();
            if (!fp) continue;
        }

        // ── Format timestamp ──────────────────────────────────────────────────
        char ts_buf[24] = "NO_SYNC";
        time_t now;
        time(&now);
        if (now > 1000000000LL) {
            struct tm ti;
            localtime_r(&now, &ti);
            strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &ti);
        }

        // ── Write CSV row ─────────────────────────────────────────────────────
        int r = fprintf(fp, "%lu,%s,%lu,%u,%u,%u,%u,%d,%u\n",
                        static_cast<unsigned long>(entry.seq),
                        ts_buf,
                        static_cast<unsigned long>(entry.uptime_ms),
                        entry.data.eco2,
                        entry.data.tvoc,
                        entry.data.raw_h2,
                        entry.data.raw_ethanol,
                        entry.data.valid ? 1 : 0,
                        static_cast<unsigned>(entry.alert_level));

        if (r < 0) {
            mclog::tagError(TAG, "write error — closing log");
            fclose(fp);
            fp = nullptr;
            continue;
        }

        // Flush libc buffer then fsync to SD hardware every write (1 Hz),
        // so the file is immediately readable on any other computer.
        fflush(fp);
        fsync(fileno(fp));
    }
}
