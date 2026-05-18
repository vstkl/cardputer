/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "sgp30.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>
#include <cstring>

static const char* TAG = "SGP30";

// =============================================================================
// Lifecycle
// =============================================================================

esp_err_t SGP30::begin(i2c_master_bus_handle_t existing_bus)
{
    if (_initialised) {
        ESP_LOGW(TAG, "begin() called while already initialised; call end() first");
        return ESP_ERR_INVALID_STATE;
    }

    _bus = nullptr;  // we don't own this bus; end() must not delete it

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length     = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address      = ADDR;
    dev_cfg.scl_speed_hz        = SCL_HZ;

    esp_err_t ret = i2c_master_bus_add_device(existing_bus, &dev_cfg, &_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_master_probe(existing_bus, ADDR, 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SGP30 not found at 0x%02X: %s", ADDR, esp_err_to_name(ret));
        i2c_master_bus_rm_device(_dev);
        _dev = nullptr;
        return ret;
    }

    ret = _writeCmd(CMD_INIT_AIQ, 10);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init_air_quality failed: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(_dev);
        _dev = nullptr;
        return ret;
    }

    _initialised  = true;
    _update_count = 0;
    _data         = {};

    ESP_LOGI(TAG, "SGP30 ready on existing bus");
    return ESP_OK;
}

esp_err_t SGP30::begin(int sda_pin, int scl_pin, int i2c_port)
{
    if (_initialised) {
        ESP_LOGW(TAG, "begin() called while already initialised; call end() first");
        return ESP_ERR_INVALID_STATE;
    }

    // ── Create I²C master bus ─────────────────────────────────────────────────
    i2c_master_bus_config_t bus_cfg      = {};
    bus_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port                     = i2c_port;
    bus_cfg.sda_io_num                   = sda_pin;
    bus_cfg.scl_io_num                   = scl_pin;
    bus_cfg.glitch_ignore_cnt            = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // ── Add SGP30 device ──────────────────────────────────────────────────────
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length     = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address      = ADDR;
    dev_cfg.scl_speed_hz        = SCL_HZ;

    ret = i2c_master_bus_add_device(_bus, &dev_cfg, &_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        i2c_del_master_bus(_bus);
        _bus = nullptr;
        return ret;
    }

    // ── Probe: verify the sensor is on the bus ────────────────────────────────
    ret = i2c_master_probe(_bus, ADDR, 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SGP30 not found at 0x%02X on I2C port %d: %s", ADDR, i2c_port, esp_err_to_name(ret));
        end();
        return ret;
    }

    // ── Issue Init_air_quality (mandatory before Measure_air_quality) ─────────
    // Datasheet §6.3: no response, 10 ms max execution time.
    ret = _writeCmd(CMD_INIT_AIQ, 10);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init_air_quality failed: %s", esp_err_to_name(ret));
        end();
        return ret;
    }

    _initialised  = true;
    _update_count = 0;
    _data         = {};

    ESP_LOGI(TAG, "SGP30 ready on SDA=%d SCL=%d port=%d", sda_pin, scl_pin, i2c_port);
    return ESP_OK;
}

void SGP30::end()
{
    if (_dev) {
        i2c_master_bus_rm_device(_dev);
        _dev = nullptr;
    }
    if (_bus) {
        // Only delete the bus when we created it (begin(sda, scl, port) path).
        // begin(existing_bus) sets _bus to nullptr so we never delete a bus we don't own.
        i2c_del_master_bus(_bus);
        _bus = nullptr;
    }
    _initialised  = false;
    _update_count = 0;
    ESP_LOGI(TAG, "SGP30 released");
}

// =============================================================================
// Measurement
// =============================================================================

bool SGP30::update()
{
    if (!_initialised) return false;

    // ── Read Measure_air_quality (eCO2, TVOC) ────────────────────────────────
    // Datasheet §6.3: send command, wait ≤12 ms, read 6 bytes (2 words + CRC each).
    // Response order: CO2eq first, TVOC second.
    uint16_t aq[2] = {};
    esp_err_t ret  = _cmdAndRead(CMD_MEAS_AIQ, 12, aq, 2);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Measure_air_quality failed: %s", esp_err_to_name(ret));
        return false;
    }

    // ── Read Measure_raw_signals (H₂, Ethanol) ───────────────────────────────
    // Datasheet §6.3: send command, wait ≤25 ms, read 6 bytes.
    // Response order: H2_signal first, Ethanol_signal second.
    uint16_t raw[2] = {};
    ret             = _cmdAndRead(CMD_MEAS_RAW, 25, raw, 2);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Measure_raw_signals failed: %s", esp_err_to_name(ret));
        return false;
    }

    ++_update_count;

    // ── Commit snapshot ───────────────────────────────────────────────────────
    _data.eco2        = aq[0];
    _data.tvoc        = aq[1];
    _data.raw_h2      = raw[0];
    _data.raw_ethanol = raw[1];
    // Datasheet §6.3: "For the first 15s … fixed values of 400 ppm CO2eq and 0 ppb TVOC."
    _data.valid = (_update_count >= WARMUP_UPDATES);

    return true;
}

// =============================================================================
// Baseline management
// =============================================================================

esp_err_t SGP30::getBaseline(Baseline& baseline)
{
    if (!_initialised) return ESP_ERR_INVALID_STATE;
    // Datasheet §6.3: Get_baseline returns CO2eq word then TVOC word.
    uint16_t words[2] = {};
    esp_err_t ret     = _cmdAndRead(CMD_GET_BASE, 10, words, 2);
    if (ret == ESP_OK) {
        baseline.eco2 = words[0];
        baseline.tvoc = words[1];
        ESP_LOGI(TAG, "getBaseline: eco2=0x%04X  tvoc=0x%04X", baseline.eco2, baseline.tvoc);
    }
    return ret;
}

esp_err_t SGP30::setBaseline(const Baseline& baseline)
{
    if (!_initialised) return ESP_ERR_INVALID_STATE;
    // Datasheet §6.3: Set_baseline parameter order is *TVOC first, CO2eq second*
    // (opposite of Get_baseline). The driver handles this here transparently.
    uint8_t payload[6];
    _packWord(payload + 0, baseline.tvoc);  // word 0: TVOC
    _packWord(payload + 3, baseline.eco2);  // word 1: CO2eq

    esp_err_t ret = _writeCmdWithData(CMD_SET_BASE, payload, sizeof(payload), 10);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "setBaseline: eco2=0x%04X  tvoc=0x%04X", baseline.eco2, baseline.tvoc);
    }
    return ret;
}

// =============================================================================
// Humidity compensation
// =============================================================================

esp_err_t SGP30::setHumidity(float rh_pct, float temp_c)
{
    if (!_initialised) return ESP_ERR_INVALID_STATE;
    uint16_t encoded;

    if (rh_pct <= 0.0f || temp_c <= -273.15f) {
        // Disable compensation: send 0x0000
        // Datasheet §6.3: "Sending 0x0000 sets humidity to default (0x0B92 = 11.57 g/m³)."
        encoded = 0x0000;
    } else {
        // Magnus approximation for absolute humidity in g/m³:
        //   AH = 216.7 × (RH/100 × 6.112 × e^(17.62T / (243.12+T))) / (273.15+T)
        float ah =
            216.7f * (rh_pct / 100.0f) * 6.112f * expf(17.62f * temp_c / (243.12f + temp_c)) / (273.15f + temp_c);

        // Encode as unsigned 8.8 fixed-point (datasheet §6.3).
        // Valid sensor range: 0x0001 (1/256 g/m³) to 0xFFFF (≈255 g/m³).
        // Clamp to the recommended operating range (4–20 g/m³) to stay within spec.
        if (ah < (1.0f / 256.0f)) ah = 1.0f / 256.0f;
        if (ah > 255.996f) ah = 255.996f;
        encoded = (uint16_t)(ah * 256.0f);
    }

    uint8_t payload[3];
    _packWord(payload, encoded);

    esp_err_t ret = _writeCmdWithData(CMD_SET_HUM, payload, sizeof(payload), 10);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "setHumidity: rh=%.1f%%  t=%.1f°C  encoded=0x%04X", rh_pct, temp_c, encoded);
    }
    return ret;
}

// =============================================================================
// Identification / diagnostics
// =============================================================================

esp_err_t SGP30::getSerialId(SerialId& id)
{
    if (!_initialised) return ESP_ERR_INVALID_STATE;
    // Datasheet §6.5: 3 words (9 bytes incl. CRC), wait ≥0.5 ms.
    uint16_t words[3] = {};
    esp_err_t ret     = _cmdAndRead(CMD_GET_SERIAL, 1, words, 3);
    if (ret == ESP_OK) {
        id.word[0] = words[0];
        id.word[1] = words[1];
        id.word[2] = words[2];
        ESP_LOGI(TAG, "Serial ID: %04X-%04X-%04X", id.word[0], id.word[1], id.word[2]);
    }
    return ret;
}

esp_err_t SGP30::selfTest()
{
    if (!_initialised) return ESP_ERR_INVALID_STATE;
    // Datasheet §6.3: Measure_test — 220 ms max, returns 0xD400 on pass.
    // Do NOT call this after Init_air_quality has been issued.
    uint16_t result = 0;
    esp_err_t ret   = _cmdAndRead(CMD_SELF_TEST, 220, &result, 1);
    if (ret != ESP_OK) return ret;

    if (result != 0xD400) {
        ESP_LOGE(TAG, "Self-test FAILED: got 0x%04X, expected 0xD400", result);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "Self-test PASSED");
    return ESP_OK;
}

esp_err_t SGP30::softReset()
{
    if (!_initialised) return ESP_ERR_INVALID_STATE;
    // Datasheet §6.4: General Call reset — write 0x06 to address 0x00.
    // This resets ALL devices on the bus that support General Call.
    // After reset, begin() must be called again before use.
    i2c_master_dev_handle_t gc_dev = nullptr;
    i2c_device_config_t gc_cfg     = {};
    gc_cfg.dev_addr_length         = I2C_ADDR_BIT_LEN_7;
    gc_cfg.device_address          = GENERAL_CALL_ADDR;
    gc_cfg.scl_speed_hz            = SCL_HZ;

    esp_err_t ret = i2c_master_bus_add_device(_bus, &gc_cfg, &gc_dev);
    if (ret != ESP_OK) return ret;

    uint8_t reset_byte = GENERAL_CALL_RESET;
    ret                = i2c_master_transmit(gc_dev, &reset_byte, 1, 50);
    i2c_master_bus_rm_device(gc_dev);

    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(1));  // datasheet §5.1: tSR ≤ 0.6 ms
        _initialised  = false;         // caller must call begin() again
        _update_count = 0;
        _data         = {};
        ESP_LOGI(TAG, "Soft reset issued");
    }
    return ret;
}

// =============================================================================
// Private helpers
// =============================================================================

esp_err_t SGP30::_writeCmd(const uint8_t cmd[2], uint32_t delay_ms)
{
    esp_err_t ret = i2c_master_transmit(_dev, cmd, 2, 50);
    if (ret == ESP_OK && delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    return ret;
}

esp_err_t SGP30::_writeCmdWithData(const uint8_t cmd[2], const uint8_t* data, size_t data_len, uint32_t delay_ms)
{
    // Build a single buffer: [cmd[0], cmd[1], data...]
    // Maximum payload: 2 (cmd) + 9 (3 words × 3 bytes) = 11 bytes.
    uint8_t buf[11];
    buf[0] = cmd[0];
    buf[1] = cmd[1];
    if (data_len > sizeof(buf) - 2) return ESP_ERR_INVALID_ARG;
    memcpy(buf + 2, data, data_len);

    esp_err_t ret = i2c_master_transmit(_dev, buf, 2 + data_len, 50);
    if (ret == ESP_OK && delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    return ret;
}

esp_err_t SGP30::_readWords(uint16_t* out, size_t num_words)
{
    // Each word = 2 data bytes + 1 CRC byte (datasheet §6.2).
    // Maximum words in any response: 3 (Serial ID).
    uint8_t buf[9];  // 3 words × 3 bytes
    if (num_words > 3) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = i2c_master_receive(_dev, buf, num_words * 3, 50);
    if (ret != ESP_OK) return ret;

    for (size_t i = 0; i < num_words; i++) {
        uint8_t hi  = buf[i * 3 + 0];
        uint8_t lo  = buf[i * 3 + 1];
        uint8_t crc = buf[i * 3 + 2];

        if (_crc8(hi, lo) != crc) {
            ESP_LOGE(TAG, "CRC mismatch on word %zu: got 0x%02X expected 0x%02X", i, crc, _crc8(hi, lo));
            return ESP_ERR_INVALID_CRC;
        }
        out[i] = ((uint16_t)hi << 8) | lo;
    }
    return ESP_OK;
}

esp_err_t SGP30::_cmdAndRead(const uint8_t cmd[2], uint32_t delay_ms, uint16_t* out, size_t num_words)
{
    // Send command, wait, then read response as separate I²C transactions.
    // The SGP30 requires a stop-condition + delay between command and read
    // (datasheet §6.2, Fig. 9) — do NOT use transmit_receive here.
    esp_err_t ret = _writeCmd(cmd, delay_ms);
    if (ret != ESP_OK) return ret;
    return _readWords(out, num_words);
}

uint8_t SGP30::_crc8(uint8_t a, uint8_t b)
{
    // CRC-8, polynomial 0x31, init 0xFF, no reflection, no final XOR.
    // Verified: _crc8(0xBE, 0xEF) == 0x92  (datasheet Table 13).
    uint8_t crc           = 0xFF;
    const uint8_t data[2] = {a, b};
    for (int i = 0; i < 2; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
        }
    }
    return crc;
}

void SGP30::_packWord(uint8_t* dst, uint16_t word)
{
    dst[0] = (word >> 8) & 0xFF;
    dst[1] = word & 0xFF;
    dst[2] = _crc8(dst[0], dst[1]);
}
