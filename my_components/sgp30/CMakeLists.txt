# components/sgp30/CMakeLists.txt
#
# SGP30 multi-pixel gas sensor driver.
# Requires the ESP-IDF 5.x new I²C master driver (driver/i2c_master.h).

idf_component_register(
    SRCS
        "sgp30.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        driver          # i2c_master, esp_err
        freertos        # vTaskDelay, pdMS_TO_TICKS
        esp_rom         # optional: esp_rom_delay_us if needed later
)
