/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "airmon.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_pm.h>
#include <mooncake_log.h>

static const char* TAG = "airmon";

// ─── Global sync primitives (definitions) ─────────────────────────────────────
SemaphoreHandle_t  g_data_mutex  = nullptr;
QueueHandle_t      g_log_queue   = nullptr;
QueueHandle_t      g_audio_queue = nullptr;
EventGroupHandle_t g_events      = nullptr;
SensorSnapshot     g_snapshot    = {};

// Forward-declare task entry points (each defined in its own TU).
void sensor_task(void*);
void display_task(void*);
void logger_task(void*);
void audio_task(void*);

// ─── Power management ─────────────────────────────────────────────────────────
// Scale from 240 → 160 MHz max and enable automatic light sleep.
// All three tasks spend the vast majority of their time blocked (sensor: ~999 ms/s,
// display: ~200 ms/frame, logger: waiting on queue).  The CPU idles into light
// sleep during every block, waking only when the nearest task timer fires.
//
// Note: automatic light sleep briefly suspends the USB-JTAG serial link during
// each sleep window.  "idf.py monitor" output may be interrupted during
// development; this is expected and does not affect firmware correctness.
static void pm_init()
{
    esp_pm_config_t cfg = {
        .max_freq_mhz       = 160,
        .min_freq_mhz       = 80,
        .light_sleep_enable = true,
    };
    esp_err_t err = esp_pm_configure(&cfg);
    if (err != ESP_OK) {
        mclog::tagWarn(TAG, "pm_configure: %s — running at full speed", esp_err_to_name(err));
    } else {
        mclog::tagInfo(TAG, "PM: 80–160 MHz, automatic light sleep enabled");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void airmon_start()
{
    pm_init();

    g_data_mutex  = xSemaphoreCreateMutex();
    g_log_queue   = xQueueCreate(32, sizeof(SensorSnapshot));
    g_audio_queue = xQueueCreate(4,  sizeof(AudioCmd));
    g_events      = xEventGroupCreate();

    configASSERT(g_data_mutex);
    configASSERT(g_log_queue);
    configASSERT(g_audio_queue);
    configASSERT(g_events);

    // sensor + logger + audio share APP_CPU (core 1) — all sleep most of the time.
    // display owns PRO_CPU (core 0) and is the sole caller of M5.update() / Speaker.
    xTaskCreatePinnedToCore(sensor_task,  "airmon_sen",  SENSOR_STACK,  nullptr, SENSOR_PRI,  nullptr, 1);
    xTaskCreatePinnedToCore(logger_task,  "airmon_log",  LOGGER_STACK,  nullptr, LOGGER_PRI,  nullptr, 1);
    xTaskCreatePinnedToCore(audio_task,   "airmon_aud",  AUDIO_STACK,   nullptr, AUDIO_PRI,   nullptr, 1);
    xTaskCreatePinnedToCore(display_task, "airmon_disp", DISPLAY_STACK, nullptr, DISPLAY_PRI, nullptr, 0);
}
