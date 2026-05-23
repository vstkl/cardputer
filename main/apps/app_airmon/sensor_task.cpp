/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "airmon.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sgp30.hpp>
#include <nvs.h>
#include <esp_timer.h>
#include <mooncake_log.h>
#include <ctime>

static const char* TAG = "sensor";

// ─── SGP30 hardware config (PORT.A on Cardputer ADV) ──────────────────────────
static constexpr int SGP30_SDA_PIN = 2;
static constexpr int SGP30_SCL_PIN = 1;

// ─── NVS keys ─────────────────────────────────────────────────────────────────
static constexpr const char* NVS_NS    = "airmon";
static constexpr const char* NVS_ECO2  = "bl_eco2";
static constexpr const char* NVS_TVOC  = "bl_tvoc";
static constexpr const char* NVS_BTIME = "bl_time";  // Unix epoch of last save; 0 = unknown

// ─── Baseline timing ──────────────────────────────────────────────────────────
// The SGP30 algorithm needs ≥12 h of continuous operation before the baseline
// is fully representative, but saving at 1 h is a practical compromise for
// devices that may not run continuously.  Saving a sub-optimal baseline is
// better than losing it entirely on power-off.
static constexpr uint32_t WARMUP_TICKS        = 15;    //  15 s until data is valid
static constexpr uint32_t SAVE_AFTER_TICKS    = 3600;  //   1 h first save
static constexpr uint32_t SAVE_INTERVAL_TICKS = 3600;  //   1 h subsequent saves
static constexpr int64_t  BASELINE_MAX_AGE_S  = 7LL * 24 * 3600;  // 7 days

// ─── Reconnect policy ─────────────────────────────────────────────────────────
static constexpr uint32_t ERR_THRESHOLD  = 5;   // consecutive errors before reconnect
static constexpr uint32_t RETRY_INTERVAL = 10;  // ticks between reconnect attempts

// ─────────────────────────────────────────────────────────────────────────────
static void baseline_save(SGP30& sgp)
{
    SGP30::Baseline bl;
    if (sgp.getBaseline(bl) != ESP_OK) {
        mclog::tagWarn(TAG, "getBaseline failed — skipping save");
        return;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    int64_t ts = 0;
    time_t  now;
    time(&now);
    if (now > 1000000000LL) ts = static_cast<int64_t>(now);  // SNTP synced

    nvs_set_u16(h, NVS_ECO2,  bl.eco2);
    nvs_set_u16(h, NVS_TVOC,  bl.tvoc);
    nvs_set_i64(h, NVS_BTIME, ts);
    nvs_commit(h);
    nvs_close(h);
    mclog::tagInfo(TAG, "baseline saved  eco2=%u  tvoc=%u", bl.eco2, bl.tvoc);
}

static bool baseline_restore(SGP30& sgp)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    uint16_t eco2 = 0, tvoc = 0;
    int64_t  ts   = 0;
    bool ok = (nvs_get_u16(h, NVS_ECO2,  &eco2) == ESP_OK &&
               nvs_get_u16(h, NVS_TVOC,  &tvoc) == ESP_OK &&
               nvs_get_i64(h, NVS_BTIME, &ts)   == ESP_OK);
    nvs_close(h);

    if (!ok) return false;

    // Discard baseline older than 7 days when we have valid wall-clock time.
    time_t now;
    time(&now);
    if (ts > 0 && now > 1000000000LL) {
        int64_t age_s = static_cast<int64_t>(now) - ts;
        if (age_s > BASELINE_MAX_AGE_S) {
            mclog::tagWarn(TAG, "saved baseline is %lld days old — discarding", age_s / 86400);
            return false;
        }
    }

    SGP30::Baseline bl{eco2, tvoc};
    if (sgp.setBaseline(bl) != ESP_OK) return false;

    mclog::tagInfo(TAG, "baseline restored  eco2=%u  tvoc=%u", eco2, tvoc);
    return true;
}

// ─── Alert threshold evaluation ───────────────────────────────────────────────
// Returns alert_level (0/1/2) and populates to_set / to_clear event bit masks.
static uint8_t evaluate_thresholds(const SGP30::Data& d,
                                   EventBits_t& to_set,
                                   EventBits_t& to_clear)
{
    to_set   = 0;
    to_clear = 0;

    if (!d.valid) {
        to_clear = EV_WARN_CO2 | EV_CRIT_CO2 | EV_WARN_TVOC | EV_CRIT_TVOC;
        return 0;
    }

    uint8_t level = 0;

    // ── eCO2 ──────────────────────────────────────────────────────────────────
    if (d.eco2 >= ECO2_CRIT) {
        to_set |= EV_CRIT_CO2 | EV_WARN_CO2;
        level   = 2;
    } else if (d.eco2 >= ECO2_WARN) {
        to_set   |= EV_WARN_CO2;
        to_clear |= EV_CRIT_CO2;
        if (level < 1) level = 1;
    } else {
        to_clear |= EV_WARN_CO2 | EV_CRIT_CO2;
    }

    // ── TVOC ──────────────────────────────────────────────────────────────────
    if (d.tvoc >= TVOC_CRIT) {
        to_set |= EV_CRIT_TVOC | EV_WARN_TVOC;
        level   = 2;
    } else if (d.tvoc >= TVOC_WARN) {
        to_set   |= EV_WARN_TVOC;
        to_clear |= EV_CRIT_TVOC;
        if (level < 1) level = 1;
    } else {
        to_clear |= EV_WARN_TVOC | EV_CRIT_TVOC;
    }

    return level;
}

// ─────────────────────────────────────────────────────────────────────────────
void sensor_task(void*)
{
    SGP30    sgp;
    bool     sensor_ok  = false;
    uint32_t tick_count = 0;
    uint32_t next_save  = SAVE_AFTER_TICKS;
    uint32_t err_streak = 0;

    auto connect = [&]() -> bool {
        sgp.end();
        if (sgp.begin(SGP30_SDA_PIN, SGP30_SCL_PIN) != ESP_OK) {
            mclog::tagError(TAG, "SGP30 not found (SDA=%d SCL=%d)", SGP30_SDA_PIN, SGP30_SCL_PIN);
            return false;
        }
        baseline_restore(sgp);
        err_streak = 0;
        mclog::tagInfo(TAG, "SGP30 ready");
        return true;
    };

    sensor_ok = connect();

    // Anchor the first wake-up tick so vTaskDelayUntil drifts from a known point.
    // With light sleep enabled the tick counter still advances correctly (driven by
    // the hardware timer), so the 1 Hz constraint is maintained across sleep windows.
    TickType_t wake_tick = xTaskGetTickCount();

    for (;;) {
        // Drift-free 1 Hz: the SGP30 baseline algorithm is calibrated for exactly 1 Hz.
        // During the ~999 ms block the CPU enters light sleep (see pm_init).
        vTaskDelayUntil(&wake_tick, pdMS_TO_TICKS(1000));
        tick_count++;

        // ── Reconnect if sensor is missing ────────────────────────────────────
        if (!sensor_ok) {
            if (tick_count % RETRY_INTERVAL == 0) {
                sensor_ok = connect();
            }
            continue;
        }

        // ── Poll sensor ───────────────────────────────────────────────────────
        bool read_ok = sgp.update();
        if (!read_ok) {
            if (++err_streak >= ERR_THRESHOLD) {
                mclog::tagError(TAG, "%u consecutive I2C errors — reconnecting", ERR_THRESHOLD);
                sensor_ok  = false;
                err_streak = 0;
            }
            continue;
        }
        err_streak = 0;

        // ── Evaluate thresholds ───────────────────────────────────────────────
        EventBits_t to_set, to_clear;
        uint8_t alert = evaluate_thresholds(sgp.getData(), to_set, to_clear);

        // ── Build snapshot ────────────────────────────────────────────────────
        SensorSnapshot snap;
        snap.data        = sgp.getData();
        snap.uptime_ms   = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
        snap.seq         = tick_count;
        snap.alert_level = alert;

        // ── Update shared snapshot under mutex ────────────────────────────────
        if (xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            g_snapshot = snap;
            xSemaphoreGive(g_data_mutex);
        } else {
            mclog::tagWarn(TAG, "mutex timeout — snapshot skipped");
        }

        // ── Publish alert event bits ──────────────────────────────────────────
        if (to_set)   xEventGroupSetBits(g_events, to_set);
        if (to_clear) xEventGroupClearBits(g_events, to_clear);

        // ── Track warm-up state ───────────────────────────────────────────────
        if (snap.data.valid) {
            xEventGroupSetBits(g_events, EV_SENSOR_READY);
        } else {
            xEventGroupClearBits(g_events, EV_SENSOR_READY);
        }
        if (tick_count == WARMUP_TICKS) {
            mclog::tagInfo(TAG, "warm-up complete");
        }

        // ── Forward to logger (non-blocking: drop rather than stall sensor) ───
        xQueueSend(g_log_queue, &snap, 0);

        // ── Periodic baseline save ────────────────────────────────────────────
        if (tick_count >= next_save) {
            baseline_save(sgp);
            next_save = tick_count + SAVE_INTERVAL_TICKS;
        }
    }
}
