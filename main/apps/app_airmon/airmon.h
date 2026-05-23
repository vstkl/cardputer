/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <sgp30.hpp>
#include <cstdint>

// ─── Task configuration ────────────────────────────────────────────────────────
static constexpr uint32_t    SENSOR_STACK  = 6144;
static constexpr uint32_t    DISPLAY_STACK = 12288;
static constexpr uint32_t    LOGGER_STACK  = 6144;
static constexpr uint32_t    AUDIO_STACK   = 4096;
static constexpr UBaseType_t SENSOR_PRI    = 5;   // must not miss 1 Hz SGP30 window
static constexpr UBaseType_t DISPLAY_PRI   = 3;
static constexpr UBaseType_t LOGGER_PRI    = 2;
static constexpr UBaseType_t AUDIO_PRI     = 1;   // comfort feature; lowest priority

// ─── Battery-conservation tuning ──────────────────────────────────────────────
// Display is the largest consumer; these three constants are the primary levers.
static constexpr uint8_t  BRIGHTNESS_ACTIVE = 64;    // ~25% backlight duty
static constexpr uint32_t SLEEP_TIMEOUT_MS  = 30000; // 30 s without input → backlight off
static constexpr uint32_t RENDER_PERIOD_MS  = 200;   // 5 Hz refresh (was 10 Hz)

// ─── Graph ring buffer depth ───────────────────────────────────────────────────
// Matches main canvas width (204 px) — each column is exactly one sample.
static constexpr size_t GRAPH_DEPTH = 204;

// ─── IAQ alert thresholds ─────────────────────────────────────────────────────
// Sensirion SGP30 IAQ grades + ASHRAE 62.1 ventilation reference levels.
static constexpr uint16_t ECO2_WARN = 1000; // ppm — increased ventilation required
static constexpr uint16_t ECO2_CRIT = 2000; // ppm — health impact range
static constexpr uint16_t TVOC_WARN = 300;  // ppb — Sensirion IAQ grade 3
static constexpr uint16_t TVOC_CRIT = 600;  // ppb — Sensirion IAQ grade 4

// ─── Shared sensor snapshot ────────────────────────────────────────────────────
// Protected by g_data_mutex.  seq == 0 → no data yet.
// alert_level: 0 = clean, 1 = warning (warn threshold exceeded), 2 = critical.
struct SensorSnapshot {
    SGP30::Data data        = {};
    uint32_t    uptime_ms   = 0;
    uint32_t    seq         = 0;
    uint8_t     alert_level = 0;
};

// ─── Audio command ─────────────────────────────────────────────────────────────
// Produced by audio_task; consumed by display_task (sole owner of M5.Speaker).
// level: 1 = warning pattern (2 gentle beeps), 2 = critical pattern (3 sharp beeps).
struct AudioCmd {
    uint8_t level;
};

// ─── Inter-task synchronisation ────────────────────────────────────────────────
extern SemaphoreHandle_t  g_data_mutex;   // guards g_snapshot
extern QueueHandle_t      g_log_queue;    // SensorSnapshot → logger_task
extern QueueHandle_t      g_audio_queue;  // AudioCmd       → display_task
extern EventGroupHandle_t g_events;
extern SensorSnapshot     g_snapshot;

#define EV_SENSOR_READY  (EventBits_t)(BIT0)  // sensor past 15-s warm-up
#define EV_SD_MOUNTED    (EventBits_t)(BIT1)  // SD card mounted OK
#define EV_DISPLAY_AWAKE (EventBits_t)(BIT2)  // backlight is on
#define EV_WARN_CO2      (EventBits_t)(BIT3)  // eCO2 ≥ ECO2_WARN
#define EV_CRIT_CO2      (EventBits_t)(BIT4)  // eCO2 ≥ ECO2_CRIT
#define EV_WARN_TVOC     (EventBits_t)(BIT5)  // TVOC ≥ TVOC_WARN
#define EV_CRIT_TVOC     (EventBits_t)(BIT6)  // TVOC ≥ TVOC_CRIT

// ─── Entry point ───────────────────────────────────────────────────────────────
void airmon_start();
