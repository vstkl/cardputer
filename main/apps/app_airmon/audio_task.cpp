/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "airmon.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <mooncake_log.h>

static const char* TAG = "audio";

// ─── Alert cooldown periods ────────────────────────────────────────────────────
// These prevent the speaker from beeping continuously in a sustained alert.
// Critical alerts repeat faster because they warrant more urgent attention.
static constexpr uint32_t COOLDOWN_CRIT_MS = 15000;  // 15 s between critical beep sets
static constexpr uint32_t COOLDOWN_WARN_MS = 60000;  // 60 s between warning beep sets

// How often this task polls the event group.  Long enough to yield the CPU
// between checks; the 2 s window means alert onset latency ≤ 2 s.
static constexpr uint32_t POLL_MS = 2000;

// ─────────────────────────────────────────────────────────────────────────────
void audio_task(void*)
{
    uint32_t last_crit_ms = 0;
    uint32_t last_warn_ms = 0;

    for (;;) {
        // Block until the sensor has valid data; the task consumes no CPU
        // during warm-up and the CPU can light-sleep freely here.
        xEventGroupWaitBits(g_events, EV_SENSOR_READY, pdFALSE, pdTRUE, portMAX_DELAY);

        // Sleep before next evaluation; this is the main idle block where
        // light sleep fires on core 1.
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
        EventBits_t ev  = xEventGroupGetBits(g_events);

        bool is_crit = (ev & (EV_CRIT_CO2 | EV_CRIT_TVOC)) != 0;
        bool is_warn = (ev & (EV_WARN_CO2 | EV_WARN_TVOC))  != 0;

        AudioCmd cmd   = {};
        bool do_alert  = false;

        if (is_crit && (now_ms - last_crit_ms) >= COOLDOWN_CRIT_MS) {
            cmd.level    = 2;
            do_alert     = true;
            last_crit_ms = now_ms;
            last_warn_ms = now_ms;  // reset warn cooldown too
            mclog::tagWarn(TAG, "critical IAQ — queuing alert");
        } else if (is_warn && !is_crit && (now_ms - last_warn_ms) >= COOLDOWN_WARN_MS) {
            cmd.level    = 1;
            do_alert     = true;
            last_warn_ms = now_ms;
            mclog::tagWarn(TAG, "elevated IAQ — queuing warning");
        }

        if (do_alert) {
            // Non-blocking send: if display_task's audio queue is full the alert
            // is silently dropped.  This is intentional — the display is already
            // playing a tone; stacking duplicates would be worse than dropping.
            xQueueSend(g_audio_queue, &cmd, 0);
        }
    }
}
