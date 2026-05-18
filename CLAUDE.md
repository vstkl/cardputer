# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32-S3 firmware for the M5Stack Cardputer ADV, built with **ESP-IDF v5.4.2**. Target: `esp32s3`.

## Commands

### First-time setup — fetch vendored components

```bash
python3 ./fetch_repos.py
```

This clones the repos listed in `repos.json` into `components/` (M5GFX, M5Unified, mooncake, mooncake_log, smooth_ui_toolkit).

### Build / Flash / Monitor

```bash
idf.py build
idf.py flash
idf.py monitor
idf.py flash monitor   # flash then watch serial output
```

There is no unit-test suite; validation is done by flashing and observing runtime behaviour via `idf.py monitor`.

### SDK configuration

```bash
idf.py menuconfig      # interactive Kconfig editor
idf.py save-defconfig  # write minimised diff to sdkconfig.defaults
```

## Architecture

### Entry point and main loop (`main/main.cpp`)

`app_main()` runs on the main FreeRTOS task. It:
1. Initialises the HAL singleton (`GetHAL().init()`).
2. Wires the `smooth_ui_toolkit` timing callbacks to HAL helpers.
3. Installs every app into the Mooncake app manager.
4. Spins in `while(1)` calling `GetHAL().update()` and `GetMooncake().update()`.

### HAL (`main/hal/`)

`hal.h` defines the `Hal` class, accessed via the `GetHAL()` singleton. It owns all hardware state:

- **Display**: `M5GFX` display + three `LGFX_Sprite` layers (keyboard bar, system bar, main canvas). Push with `pushCanvasKeyboardBar()`, `pushCanvasSystemBar()`, `pushCanvas()`.
- **Input**: `Keyboard` (I2C, interrupt on GPIO 11) and `M5.BtnA` home button.
- **Audio**: `M5.Speaker` / `M5.Mic`.
- **Connectivity**: WiFi, ESP-NOW, BLE HID keyboard (`NimBLE`), USB HID keyboard (`TinyUSB`), IR TX (GPIO 44).
- **Peripherals**: IMU (`M5.Imu`), SD card (SPI: MISO 39, MOSI 14, SCLK 40, CS 12), LoRa 868 MHz cap (`CapLoRa868`), GPS (UART: TX 13, RX 15), SGP30 gas sensor (`sgp30` member).
- **Settings**: persisted via `Settings` (NVS-backed).

Pin assignments are all in `main/hal/hal_config.h`. PORT.A (SDA=GPIO1, SCL=GPIO2) is used for external I2C accessories.

### App framework (Mooncake)

Every app is a subclass of `mooncake::AppAbility` with three lifecycle methods:

```cpp
void onOpen() override;    // allocate resources, subscribe to events
void onRunning() override; // called every frame from the main loop
void onClose() override;   // release resources, unsubscribe
```

Apps live under `main/apps/app_<name>/`. To add a new app:
1. Create `main/apps/app_<name>/app_<name>.{h,cpp}` with the `AppAbility` subclass.
2. Include the header in `main/apps/apps.h`.
3. Register it in `main/main.cpp` with `GetMooncake().installApp(std::make_unique<AppFoo>())`.

The app list in `main.cpp` is the install order (Launcher must be first).

### SGP30 driver (`components/sgp30`)

Vendored via `repos.json` from `github.com/vstkl/esp-idf-sgp30`. Edit the driver there, bump the `branch` pin in `repos.json`, and re-run `fetch_repos.py`.

Uses the **new ESP-IDF 5.x I2C master API** (`driver/i2c_master.h`), not the legacy `driver/i2c.h`. The `Hal` class holds an `SGP30 sgp30` member; call `sgp30.begin(SDA, SCL, I2C_NUM_1)` then `sgp30.update()` at 1 Hz. The sensor needs ~15 s warm-up before `getData().valid` is true.

### Vendored components (`components/`)

Managed by `repos.json` + `fetch_repos.py` (plain `git clone`). Do not edit files under `components/` — track changes via `repos.json` branch pins instead.

### Assets (`main/assets/`)

Binary files placed here are embedded into the firmware via `EMBED_FILES` in `main/CMakeLists.txt` and accessible as `extern const uint8_t` symbols.
