# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Target Hardware

**M5Stack Cardputer ADV** — ESP32-S3 MCU, 240×135 px display, physical QWERTY keyboard, speaker, mic, IMU, IR, LoRa868 cap, GPS cap, BLE/USB HID, SD card slot. Toolchain: **ESP-IDF v5.4.2**.

## Build Commands

```bash
# Fetch vendored component repos (run once or after repos.json changes)
python3 ./fetch_repos.py

# Build firmware
idf.py build

# Flash to device
idf.py flash

# Monitor serial output
idf.py monitor

# Build + flash + monitor in one step
idf.py flash monitor
```

No test suite exists — verification is done by flashing and observing on device.

## Dependency Management

- `repos.json` — list of git-cloned components fetched into `components/` by `fetch_repos.py`. Pinned by branch/tag.
- `managed_components/` — ESP-IDF component manager packages (declared in `main/idf_component.yml`).
- `sdkconfig` / `sdkconfig.defaults` — Kconfig build options. Do not edit `sdkconfig` by hand; use `idf.py menuconfig`.

Custom components (e.g., `sgp30`) live in `components/` alongside the vendored repos. The `my_components/` directory (tracked in the project git) is the source of truth for custom components and is copied into `components/` at build time.

## Architecture

The firmware follows a **HAL + Mooncake app framework** pattern:

```
main/
  main.cpp          — entry point: init HAL, install apps, run main loop
  hal/              — hardware abstraction layer (singleton via GetHAL())
    hal.h / hal.cpp — Hal class: display, audio, keyboard, WiFi, BLE, USB, IR, LoRa, SD, IMU
    hal_config.h    — GPIO pin definitions
    keyboard/       — TCA8418 I2C keyboard matrix driver
    cap_lora868/    — LoRa868 cap driver (RadioLib + TinyGPS++)
    utils/          — BLE HID, USB HID, IR NEC, settings (NVS)
  apps/
    apps.h          — single include that pulls in all app headers
    app_*/          — one directory per app
    utils/          — shared: audio, theme defines, common macros, raylib wrapper
```

**App lifecycle (Mooncake `AppAbility`):** each app implements `onOpen()`, `onRunning()` (called every tick), `onClose()`. Apps are installed in `main.cpp` and managed by `GetMooncake()`.

**Display pipeline:** `GetHAL().canvas` (main sprite) + `canvasSystemBar` + `canvasKeyboardBar`. After drawing, call `GetHAL().pushCanvas()` (or `pushCanvasSystemBar()` / `pushCanvasKeyboardBar()`). Canvas origin is offset by the keyboard bar width and system bar height.

**Theme & fonts:** defined in `main/apps/utils/theme.h`. Use `THEME_COLOR_BG`, `FONT_REPL`, etc. Screen is 240×135 px; `FONT_REPL` at `textSize(1)` gives ~8×16 px cells.

**Adding a new app:**
1. Create `main/apps/app_<name>/app_<name>.h` and `.cpp` inheriting `mooncake::AppAbility`.
2. Set `setAppInfo().name` in the constructor; optionally attach an `AppIcon_t` to `userData`.
3. Include the header in `main/apps/apps.h`.
4. Install with `GetMooncake().installApp(std::make_unique<AppName>())` in `main.cpp`.

`AppDummy` (`main/apps/app_dummy/`) is the minimal template for a new app.

## Code Style

`.clang-format` is present at the repo root — use it. C++17, ESP-IDF conventions, `mclog::tagInfo/Warn/Error` for logging.
