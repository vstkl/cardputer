/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file sgp30.hpp
 * @brief Driver for the Sensirion SGP30 multi-pixel gas sensor.
 *
 * ─── Overview ────────────────────────────────────────────────────────────────
 *
 * The SGP30 is an I²C gas sensor that exposes four signals:
 *
 *   Signal         Range               Notes
 *   ─────────────  ──────────────────  ──────────────────────────────────────
 *   eCO2           400 – 60 000 ppm    Equivalent CO₂, derived from H₂/EtOH
 *   TVOC           0   – 60 000 ppb    Total Volatile Organic Compounds
 *   Raw H₂         dimensionless       ~13 000 in clean air, lower = more H₂
 *   Raw Ethanol    dimensionless       ~18 000 in clean air, lower = more EtOH
 *
 * eCO2 is *not* a true CO₂ measurement. It is computed from the H₂ and
 * Ethanol signals via an on-chip algorithm and cannot fully replace a
 * dedicated CO₂ sensor.
 *
 * ─── Timing constraints ──────────────────────────────────────────────────────
 *
 * The on-chip dynamic baseline compensation algorithm is calibrated for
 * exactly 1 Hz polling. Call update() once per second after begin().
 * Polling faster or slower degrades long-term accuracy.
 *
 * After begin(), the chip returns placeholder values (eCO2 = 400 ppm,
 * TVOC = 0 ppb) for the first 15 seconds. getData().valid is false
 * during this period.
 *
 * ─── Baseline persistence ────────────────────────────────────────────────────
 *
 * After ≥12 h of continuous operation, call getBaseline() and persist
 * the result to NVS. On the next boot, call setBaseline() immediately
 * after begin() (and before the first update()) to skip the warm-up
 * phase. Discard a saved baseline if it is older than 7 days.
 *
 * ─── Humidity compensation ───────────────────────────────────────────────────
 *
 * The chip assumes 50 %RH at 25 °C by default. If you have a humidity
 * sensor on the bus, call setHumidity() whenever your reading updates
 * (every 30–60 s is sufficient). This measurably improves eCO2/TVOC
 * accuracy. Pass (0, 0) to disable compensation and revert to default.
 *
 * ─── I²C wiring ──────────────────────────────────────────────────────────────
 *
 *  M5Stack TVOC unit (HY2.0-4P / Grove)   Cardputer ADV PORT.A
 *  ───────────────────────────────────     ────────────────────
 *  Black  GND                              GND
 *  Red    5 V  (unit has on-board LDO)     5 V
 *  Yellow SDA                              check hal_cardputer.cpp for GPIO
 *  White  SCL                              check hal_cardputer.cpp for GPIO
 *
 * The sensor is fixed at I²C address 0x58 (hardware, cannot be changed).
 * Maximum SCL frequency: 400 kHz. This driver uses 100 kHz for safety.
 *
 * ─── Usage example ───────────────────────────────────────────────────────────
 *
 *  SGP30 sgp;
 *
 *  // In HAL init or app onOpen():
 *  if (sgp.begin(SDA_PIN, SCL_PIN, I2C_NUM_1) != ESP_OK) { ... }
 *
 *  // Optionally restore saved baseline to skip warm-up:
 *  SGP30::Baseline bl = load_from_nvs();
 *  sgp.setBaseline(bl);
 *
 *  // In a 1 Hz task or app onRunning():
 *  if (sgp.update()) {
 *      SGP30::Data d = sgp.getData();
 *      if (d.valid) printf("eCO2=%d  TVOC=%d\n", d.eco2, d.tvoc);
 *  }
 *
 *  // Every ~1 h, save baseline:
 *  SGP30::Baseline bl;
 *  if (sgp.getBaseline(bl) == ESP_OK) save_to_nvs(bl);
 *
 *  // In HAL deinit or app onClose():
 *  sgp.end();
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <cstdint>

class SGP30 {
public:
    // =========================================================================
    // Data types
    // =========================================================================

    /**
     * @brief A single complete sensor reading.
     *
     * All four values are captured in the same update() call.
     * Check valid before trusting eCO2/TVOC.
     */
    struct Data {
        uint16_t eco2;         ///< eCO2 concentration in ppm  (400–60 000)
        uint16_t tvoc;         ///< TVOC concentration in ppb  (0–60 000)
        uint16_t raw_h2;       ///< Raw H₂ signal (dimensionless; ~13 000 clean)
        uint16_t raw_ethanol;  ///< Raw Ethanol signal (dimensionless; ~18 000 clean)
        bool valid;            ///< false for the first 15 s after begin()
    };

    /**
     * @brief Opaque baseline checkpoint for long-term accuracy.
     *
     * Persist to NVS after ≥12 h of operation; restore at next boot.
     * Discard if older than 7 days.
     *
     * @note The internal byte order differs from the order returned by
     *       getBaseline() because Set_baseline (datasheet §6.3) requires
     *       the parameters in the reverse order of Get_baseline. The
     *       driver handles this transparently – do not manipulate the
     *       words directly.
     */
    struct Baseline {
        uint16_t eco2;  ///< eCO2 baseline word (from getBaseline)
        uint16_t tvoc;  ///< TVOC baseline word  (from getBaseline)
    };

    /**
     * @brief 48-bit unique sensor serial number (3 × 16-bit words, MSW first).
     */
    struct SerialId {
        uint16_t word[3];
    };

    // =========================================================================
    // Lifecycle
    // =========================================================================

    SGP30() = default;
    ~SGP30()
    {
        end();
    }

    // Non-copyable; I²C handles are not sharable this way.
    SGP30(const SGP30&)            = delete;
    SGP30& operator=(const SGP30&) = delete;

    /**
     * @brief Initialise the I²C bus and the sensor.
     *
     * Creates a new I²C master bus on the given port and GPIO numbers,
     * adds the SGP30 device, and issues the mandatory Init_air_quality
     * command. Safe to call again after end().
     *
     * @param sda_pin   GPIO number for SDA.
     * @param scl_pin   GPIO number for SCL.
     * @param i2c_port  I²C peripheral index (I2C_NUM_0 or I2C_NUM_1).
     *                  Must not already be in use by another driver.
     *                  Default: I2C_NUM_1 (assumes NUM_0 is used by
     *                  the HAL for internal sensors).
     *
     * @return ESP_OK on success.
     *         ESP_ERR_INVALID_STATE if already initialised.
     *         Any esp_err_t from the underlying I²C driver.
     */
    esp_err_t begin(int sda_pin, int scl_pin, int i2c_port = I2C_NUM_1);

    /**
     * @brief Initialise using an existing I²C bus handle (e.g. from M5.In_I2C).
     *
     * Use this when another driver already owns the bus. The SGP30 device is
     * added to that bus; it will NOT be deleted on end().
     */
    esp_err_t begin(i2c_master_bus_handle_t existing_bus);

    /**
     * @brief Release I²C resources and put the sensor back to sleep.
     *
     * Safe to call even if begin() was never called or failed.
     * After end(), begin() may be called again.
     */
    void end();

    /**
     * @brief Returns true if begin() has been called successfully.
     */
    bool isReady() const
    {
        return _initialised;
    }

    // =========================================================================
    // Measurement — call update() at ~1 Hz
    // =========================================================================

    /**
     * @brief Poll the sensor for fresh air-quality and raw readings.
     *
     * Issues both Measure_air_quality (12 ms) and Measure_raw_signals
     * (25 ms) in sequence, then stores the combined result. The total
     * blocking time per call is ≤ 40 ms.
     *
     * Call this from a 1 Hz context — a FreeRTOS task, the app's
     * onRunning() with a millis() gate, etc. The SGP30's internal
     * baseline algorithm expects exactly 1 Hz; irregular intervals
     * degrade accuracy.
     *
     * @return true  – data was read successfully; call getData().
     *         false – an I²C error occurred; getData() returns stale data.
     */
    bool update();

    /**
     * @brief Return the most recently acquired sensor snapshot.
     *
     * Always safe to call; returns the last successfully read values
     * (or zeroed defaults if update() has never succeeded).
     */
    Data getData() const
    {
        return _data;
    }

    // =========================================================================
    // Baseline management
    // =========================================================================

    /**
     * @brief Read the current baseline values from the sensor.
     *
     * The baseline is only meaningful after ≥12 h of continuous
     * operation. Persist the result to NVS and restore it at the
     * next boot with setBaseline().
     *
     * @param[out] baseline  Populated with the current eCO2 and TVOC
     *                       baseline words.
     * @return ESP_OK on success, I²C error otherwise.
     */
    esp_err_t getBaseline(Baseline& baseline);

    /**
     * @brief Restore a previously saved baseline into the sensor.
     *
     * Dramatically shortens the warm-up period. Call this immediately
     * after begin() and before the first update(). Discard and do not
     * call this function if the saved baseline is older than 7 days,
     * as a stale baseline degrades accuracy worse than starting cold.
     *
     * @param baseline  Previously obtained via getBaseline().
     * @return ESP_OK on success, I²C error otherwise.
     */
    esp_err_t setBaseline(const Baseline& baseline);

    // =========================================================================
    // Humidity compensation
    // =========================================================================

    /**
     * @brief Update the on-chip humidity compensation value.
     *
     * Converts relative humidity + temperature to absolute humidity
     * using the Magnus approximation and writes it to the sensor.
     * The chip stores this value until the next call or a reset.
     *
     * Calling with (0.0f, 0.0f) disables compensation (sensor reverts
     * to its default assumption of 11.57 g/m³).
     *
     * @param rh_pct   Relative humidity in percent (0.0 – 100.0).
     * @param temp_c   Ambient temperature in Celsius.
     * @return ESP_OK on success, I²C error otherwise.
     */
    esp_err_t setHumidity(float rh_pct, float temp_c);

    // =========================================================================
    // Identification / diagnostics
    // =========================================================================

    /**
     * @brief Read the 48-bit hardware serial number.
     *
     * Useful for identifying sensors in multi-device systems or for
     * logging. Returns three 16-bit words, MSW first.
     *
     * @param[out] id  Populated with the serial words.
     * @return ESP_OK on success, I²C error otherwise.
     */
    esp_err_t getSerialId(SerialId& id);

    /**
     * @brief Run the on-chip self-test (Measure_test command).
     *
     * The test takes ≤ 220 ms. Do not use after Init_air_quality has
     * been issued (call softReset() first if needed). On success the
     * sensor returns the fixed pattern 0xD400.
     *
     * @return ESP_OK if the self-test passed.
     *         ESP_ERR_INVALID_RESPONSE if the sensor returned wrong data.
     *         I²C error on communication failure.
     */
    esp_err_t selfTest();

    /**
     * @brief Issue a General Call soft reset (address 0x00, data 0x06).
     *
     * Resets *all* devices on the bus that support General Call.
     * After reset, begin() must be called again.
     */
    esp_err_t softReset();

private:
    // ── I²C handles ──────────────────────────────────────────────────────────
    i2c_master_bus_handle_t _bus = nullptr;
    i2c_master_dev_handle_t _dev = nullptr;
    bool _initialised            = false;

    // ── Cached measurement ────────────────────────────────────────────────────
    Data _data             = {};
    uint32_t _update_count = 0;

    // ── Warm-up: sensor returns placeholder values for 15 s after init ────────
    static constexpr uint32_t WARMUP_UPDATES = 15;  // at 1 Hz → 15 s

    // ── Sensor constants (datasheet Table 8, Table 10, Table 12) ─────────────
    static constexpr uint8_t ADDR    = 0x58;
    static constexpr uint32_t SCL_HZ = 100000;  // safe for 400 kHz max

    // Commands (2-byte, MSB first)
    static constexpr uint8_t CMD_INIT_AIQ[2]   = {0x20, 0x03};  ///< Init_air_quality
    static constexpr uint8_t CMD_MEAS_AIQ[2]   = {0x20, 0x08};  ///< Measure_air_quality
    static constexpr uint8_t CMD_GET_BASE[2]   = {0x20, 0x15};  ///< Get_baseline
    static constexpr uint8_t CMD_SET_BASE[2]   = {0x20, 0x1E};  ///< Set_baseline
    static constexpr uint8_t CMD_SET_HUM[2]    = {0x20, 0x61};  ///< Set_humidity
    static constexpr uint8_t CMD_MEAS_RAW[2]   = {0x20, 0x50};  ///< Measure_raw_signals
    static constexpr uint8_t CMD_SELF_TEST[2]  = {0x20, 0x32};  ///< Measure_test
    static constexpr uint8_t CMD_GET_FEAT[2]   = {0x20, 0x2F};  ///< Get_feature_set_version
    static constexpr uint8_t CMD_GET_SERIAL[2] = {0x36, 0x82};  ///< Get_serial_id

    // General Call reset (separate I²C transaction to address 0x00)
    static constexpr uint8_t GENERAL_CALL_ADDR  = 0x00;
    static constexpr uint8_t GENERAL_CALL_RESET = 0x06;

    // ── Low-level helpers ─────────────────────────────────────────────────────

    /**
     * Transmit a 2-byte command, then wait delay_ms.
     * For commands that expect no response data.
     */
    esp_err_t _writeCmd(const uint8_t cmd[2], uint32_t delay_ms);

    /**
     * Transmit a 2-byte command + payload bytes (data words with CRC),
     * then wait delay_ms. For Set_baseline, Set_humidity.
     */
    esp_err_t _writeCmdWithData(const uint8_t cmd[2], const uint8_t* data, size_t data_len, uint32_t delay_ms);

    /**
     * Read num_words × 3 bytes (each word is 2 data bytes + 1 CRC byte).
     * Validates every CRC; returns ESP_ERR_INVALID_CRC on mismatch.
     * Decoded 16-bit words are written into out[].
     */
    esp_err_t _readWords(uint16_t* out, size_t num_words);

    /**
     * Issue a command, wait, then read and decode num_words response words.
     * Convenience wrapper for the read-response commands.
     */
    esp_err_t _cmdAndRead(const uint8_t cmd[2], uint32_t delay_ms, uint16_t* out, size_t num_words);

    /**
     * CRC-8 over exactly two data bytes.
     * Polynomial 0x31, init 0xFF, no reflection, no final XOR.
     * Verified: crc8({0xBE, 0xEF}) == 0x92  (datasheet Table 13).
     */
    static uint8_t _crc8(uint8_t a, uint8_t b);

    /**
     * Pack a 16-bit word into three bytes: [hi, lo, crc] at dst.
     */
    static void _packWord(uint8_t* dst, uint16_t word);
};
