/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "airmon.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <M5Unified.hpp>
#include <hal.h>
#include <apps/utils/theme.h>
#include <esp_timer.h>
#include <mooncake_log.h>
#include <cstdio>
#include <algorithm>

static const char* TAG = "display";

// ─── Canvas geometry (set by Hal::display_init) ────────────────────────────────
static constexpr int CW = 204;
static constexpr int CH = 109;

// ─── Layout constants ──────────────────────────────────────────────────────────
static constexpr int TITLE_H  = 16;
static constexpr int SEP_H    = 1;
static constexpr int VALUES_H = 21;  // 2 px pad + 16 px font + 3 px pad
static constexpr int GRAPH_Y  = TITLE_H + SEP_H;
static constexpr int GRAPH_H  = CH - TITLE_H - SEP_H - SEP_H - VALUES_H;  // 70
static constexpr int VALUES_Y = CH - VALUES_H;

// ─── Title bar x positions (8 px / char, textSize 1) ─────────────────────────
// Layout (left → right):
//   x=4  : "AirMon" (6 chars, ends at 52)
//   x=60 : battery "xx%" (3–4 chars, up to 88)
//   x=124: alert pill " W" / "!!" / "  " (2 chars, ends at 140)
//   x=140: sensor pill "SEN?" / "W/UP" / "SEN " (4 chars, ends at 172)
//   x=172: SD pill " SD" / " !S" (3 chars, ends at 196)
static constexpr int TITLE_X_BAT    = 60;
static constexpr int TITLE_X_ALERT  = CW - 80;   // 124
static constexpr int TITLE_X_SENSOR = CW - 64;   // 140
static constexpr int TITLE_X_SD     = CW - 32;   // 172

// ─── Colour palette ────────────────────────────────────────────────────────────
static constexpr uint32_t COL_BG    = 0x1A1A2E;
static constexpr uint32_t COL_PANEL = 0x16213E;
static constexpr uint32_t COL_ECO2  = 0x00E676;  // material green
static constexpr uint32_t COL_TVOC  = 0xFF6D00;  // deep orange
static constexpr uint32_t COL_TEXT  = 0xE0E0E0;
static constexpr uint32_t COL_DIM   = 0x555566;
static constexpr uint32_t COL_SEP   = 0x2D2D44;
static constexpr uint32_t COL_OK    = 0x00C853;
static constexpr uint32_t COL_ERR   = 0xFF1744;
static constexpr uint32_t COL_WARN  = 0xFFD600;
static constexpr uint32_t COL_GRID  = 0x252540;

// ─── Fixed scale ranges ────────────────────────────────────────────────────────
static constexpr uint16_t ECO2_MIN = 400;
static constexpr uint16_t ECO2_MAX = 3000;
static constexpr uint16_t TVOC_MAX = 1000;

// ─── Scrolling graph ring buffer ───────────────────────────────────────────────
struct GraphBuf {
    uint16_t eco2[GRAPH_DEPTH] = {};
    uint16_t tvoc[GRAPH_DEPTH] = {};
    size_t   head              = 0;
    size_t   count             = 0;

    void push(uint16_t e, uint16_t t)
    {
        eco2[head] = e;
        tvoc[head] = t;
        head       = (head + 1) % GRAPH_DEPTH;
        if (count < GRAPH_DEPTH) count++;
    }
};

// ─── Y coordinate helpers ──────────────────────────────────────────────────────
static inline int eco2_to_y(uint16_t v)
{
    int c = std::clamp(static_cast<int>(v), static_cast<int>(ECO2_MIN), static_cast<int>(ECO2_MAX));
    return (GRAPH_H - 1) - (c - ECO2_MIN) * (GRAPH_H - 1) / (ECO2_MAX - ECO2_MIN);
}

static inline int tvoc_to_y(uint16_t v)
{
    int c = std::clamp(static_cast<int>(v), 0, static_cast<int>(TVOC_MAX));
    return (GRAPH_H - 1) - c * (GRAPH_H - 1) / TVOC_MAX;
}

// ─── Per-pollutant value colour (ok → warn → crit) ────────────────────────────
static inline uint32_t value_color(uint16_t val, uint16_t warn, uint16_t crit, bool valid)
{
    if (!valid)      return COL_DIM;
    if (val >= crit) return COL_ERR;
    if (val >= warn) return COL_WARN;
    return COL_TEXT;
}

// ─── Rendering ─────────────────────────────────────────────────────────────────
static void draw_frame(LGFX_Sprite& c, const GraphBuf& g,
                       const SensorSnapshot& snap, bool sd_ok, uint8_t bat_pct)
{
    c.fillScreen(COL_BG);
    c.setFont(FONT_REPL);
    c.setTextSize(1);

    // ── Title ──────────────────────────────────────────────────────────────
    c.setTextColor(COL_TEXT);
    c.drawString("AirMon", 4, 0);

    // ── Battery percentage ─────────────────────────────────────────────────
    {
        char buf[5];
        snprintf(buf, sizeof(buf), "%u%%", static_cast<unsigned>(bat_pct));
        c.setTextColor(bat_pct > 20 ? COL_DIM : COL_WARN);
        c.drawString(buf, TITLE_X_BAT, 0);
    }

    // ── Alert pill — "  " / "W!" / "!!" ───────────────────────────────────
    {
        const char* label = "  ";
        uint32_t    col   = COL_DIM;
        if (snap.alert_level == 2)      { label = "!!"; col = COL_ERR;  }
        else if (snap.alert_level == 1) { label = "W!"; col = COL_WARN; }
        c.setTextColor(col);
        c.drawString(label, TITLE_X_ALERT, 0);
    }

    // ── Sensor status pill — "SEN?" / "W/UP" / "SEN " ─────────────────────
    {
        const char* label = "SEN?";
        uint32_t    col   = COL_WARN;
        if (snap.seq > 0 && snap.data.valid) { label = "SEN "; col = COL_OK;  }
        else if (snap.seq > 0)               { label = "W/UP"; col = COL_WARN; }
        c.setTextColor(col);
        c.drawString(label, TITLE_X_SENSOR, 0);
    }

    // ── SD pill — " SD" / " !S" ────────────────────────────────────────────
    c.setTextColor(sd_ok ? COL_OK : COL_ERR);
    c.drawString(sd_ok ? " SD" : " !S", TITLE_X_SD, 0);

    // ── Top separator ──────────────────────────────────────────────────────
    c.drawFastHLine(0, TITLE_H, CW, COL_SEP);

    // ── Graph area ─────────────────────────────────────────────────────────
    c.fillRect(0, GRAPH_Y, CW, GRAPH_H, COL_PANEL);

    for (int pct : {25, 50, 75}) {
        c.drawFastHLine(0, GRAPH_Y + GRAPH_H * pct / 100, CW, COL_GRID);
    }

    if (g.count >= 2) {
        size_t n      = std::min(g.count, GRAPH_DEPTH);
        int    x0     = static_cast<int>(CW - n);
        size_t oldest = (g.head + GRAPH_DEPTH - n) % GRAPH_DEPTH;

        for (size_t i = 0; i + 1 < n; i++) {
            size_t ia = (oldest + i)     % GRAPH_DEPTH;
            size_t ib = (oldest + i + 1) % GRAPH_DEPTH;
            int    xa = x0 + static_cast<int>(i);
            int    xb = xa + 1;

            c.drawLine(xa, GRAPH_Y + eco2_to_y(g.eco2[ia]),
                       xb, GRAPH_Y + eco2_to_y(g.eco2[ib]),
                       COL_ECO2);

            c.drawLine(xa, GRAPH_Y + tvoc_to_y(g.tvoc[ia]),
                       xb, GRAPH_Y + tvoc_to_y(g.tvoc[ib]),
                       COL_TVOC);
        }
    }

    // Scale legend (top-left of graph area)
    {
        char buf[8];
        c.setTextColor(COL_DIM);
        snprintf(buf, sizeof(buf), "%d", ECO2_MAX);
        c.drawString(buf, 2, GRAPH_Y + 1);
        snprintf(buf, sizeof(buf), "%d", ECO2_MIN);
        c.drawString(buf, 2, GRAPH_Y + GRAPH_H - 16);
    }

    // ── Bottom separator ───────────────────────────────────────────────────
    c.drawFastHLine(0, VALUES_Y - 1, CW, COL_SEP);

    // ── Values row ─────────────────────────────────────────────────────────
    // Colour each reading by its own threshold level so the user can tell at a
    // glance which pollutant is driving the alert without reading the pill.
    const int VY = VALUES_Y + 2;

    if (snap.seq == 0) {
        c.setTextColor(COL_WARN);
        c.drawString("Waiting for sensor...", 4, VY);
    } else {
        char buf[8];

        c.setTextColor(COL_ECO2);
        c.drawString("CO2", 2, VY);
        snprintf(buf, sizeof(buf), "%5u", snap.data.eco2);
        c.setTextColor(value_color(snap.data.eco2, ECO2_WARN, ECO2_CRIT, snap.data.valid));
        c.drawString(buf, 26, VY);
        c.setTextColor(COL_DIM);
        c.drawString("ppm", 66, VY);

        c.setTextColor(COL_TVOC);
        c.drawString("VOC", 104, VY);
        snprintf(buf, sizeof(buf), "%5u", snap.data.tvoc);
        c.setTextColor(value_color(snap.data.tvoc, TVOC_WARN, TVOC_CRIT, snap.data.valid));
        c.drawString(buf, 128, VY);
        c.setTextColor(COL_DIM);
        c.drawString("ppb", 168, VY);
    }
}

// ─── System bar (one-time static render) ───────────────────────────────────────
static void render_sysbar(Hal& hal)
{
    hal.canvasSystemBar.fillScreen(0x111122);
    hal.canvasSystemBar.setFont(FONT_REPL);
    hal.canvasSystemBar.setTextSize(1);
    hal.canvasSystemBar.setTextColor(0x8888AA);
    hal.canvasSystemBar.drawString("AirMon", 4, 5);
    hal.pushCanvasSystemBar();
}

// ─── Non-blocking audio player ────────────────────────────────────────────────
// Runs entirely within display_task — the sole owner of M5.Speaker.
// Drains g_audio_queue and drives a small state machine that triggers
// Speaker.tone() at the right moments without blocking the render loop.
struct AudioPlayer {
    enum class State { IDLE, BEEP_ON, BEEP_OFF } state = State::IDLE;
    uint32_t freq_hz = 0;
    uint32_t dur_ms  = 0;
    uint32_t gap_ms  = 0;
    int32_t  left    = 0;   // beeps remaining after current one
    uint32_t next_ms = 0;   // absolute ms for next state transition
};

static void audio_player_tick(AudioPlayer& p, m5::Speaker_Class& spk, uint32_t now_ms)
{
    switch (p.state) {

    case AudioPlayer::State::IDLE: {
        AudioCmd cmd;
        if (xQueueReceive(g_audio_queue, &cmd, 0) != pdTRUE) return;
        if (cmd.level == 2) {
            // Critical: 3 × 400 ms sharp beep @ 1500 Hz, 200 ms gap
            p.freq_hz = 1500; p.dur_ms = 400; p.gap_ms = 200; p.left = 3;
        } else {
            // Warning: 2 × 200 ms gentle beep @ 880 Hz, 250 ms gap
            p.freq_hz = 880; p.dur_ms = 200; p.gap_ms = 250; p.left = 2;
        }
        p.next_ms = now_ms;
        p.state   = AudioPlayer::State::BEEP_ON;
        break;
    }

    case AudioPlayer::State::BEEP_ON:
        if (now_ms < p.next_ms) return;
        spk.tone(static_cast<float>(p.freq_hz), p.dur_ms);
        p.left--;
        p.next_ms = now_ms + p.dur_ms + p.gap_ms;
        p.state   = AudioPlayer::State::BEEP_OFF;
        break;

    case AudioPlayer::State::BEEP_OFF:
        if (now_ms < p.next_ms) return;
        if (p.left > 0) {
            p.next_ms = now_ms;
            p.state   = AudioPlayer::State::BEEP_ON;
        } else {
            p.state = AudioPlayer::State::IDLE;
        }
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void display_task(void*)
{
    Hal& hal = GetHAL();

    hal.canvasKeyboardBar.fillScreen(0x0D0D1A);
    hal.pushCanvasKeyboardBar();
    render_sysbar(hal);

    hal.display.setBrightness(BRIGHTNESS_ACTIVE);
    xEventGroupSetBits(g_events, EV_DISPLAY_AWAKE);

    static GraphBuf   graph;    // in BSS — not on task stack
    static AudioPlayer aplayer;

    uint32_t last_seq    = 0;
    uint32_t last_act_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    bool     asleep      = false;

    for (;;) {
        // 5 Hz frame rate (was 10 Hz).  The ~200 ms block is the primary window
        // where automatic light sleep fires on PRO_CPU.
        vTaskDelay(pdMS_TO_TICKS(RENDER_PERIOD_MS));

        // ── M5 device update ──────────────────────────────────────────────
        // ONLY task that calls M5.update() or M5.Speaker — never split this.
        M5.update();
        hal.keyboard.update();

        bool any_input = hal.homeButton.wasPressed();
        {
            const Keyboard::KeyEvent_t& kev = hal.keyboard.getLatestKeyEvent();
            if (kev.state && kev.keyCode != KEY_NONE) {
                any_input = true;
                hal.keyboard.clearKeyEvent();
            }
        }

        uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);

        if (any_input) {
            last_act_ms = now_ms;
            if (asleep) {
                hal.display.setBrightness(BRIGHTNESS_ACTIVE);
                xEventGroupSetBits(g_events, EV_DISPLAY_AWAKE);
                asleep = false;
                mclog::tagInfo(TAG, "display wake");
            }
        }

        // ── Screen sleep (30 s idle) ──────────────────────────────────────
        if (!asleep && (now_ms - last_act_ms) >= SLEEP_TIMEOUT_MS) {
            hal.display.setBrightness(0);
            xEventGroupClearBits(g_events, EV_DISPLAY_AWAKE);
            asleep = true;
            mclog::tagInfo(TAG, "display sleep — backlight off");
        }

        // ── Audio player — always ticks, even when backlight is off ───────
        audio_player_tick(aplayer, hal.speaker, now_ms);

        if (asleep) continue;

        // ── Fetch latest snapshot ─────────────────────────────────────────
        SensorSnapshot snap = {};
        if (xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            snap = g_snapshot;
            xSemaphoreGive(g_data_mutex);
        }

        if (snap.seq > last_seq && snap.seq > 0) {
            graph.push(snap.data.eco2, snap.data.tvoc);
            last_seq = snap.seq;
        }

        EventBits_t ev = xEventGroupGetBits(g_events);
        bool sd_ok     = (ev & EV_SD_MOUNTED) != 0;
        uint8_t bat    = hal.getBatLevel();

        draw_frame(hal.canvas, graph, snap, sd_ok, bat);
        hal.pushCanvas();
    }
}
