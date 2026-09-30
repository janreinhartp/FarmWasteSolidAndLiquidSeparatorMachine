#pragma once

#include "esp_err.h"

/**
 * @brief  Mount the SPIFFS "storage" partition and redirect all ESP_LOG
 *         output to both the console and /spiffs/machine.log, so a crash
 *         or hang that nobody was watching live can still be inspected.
 *         On boot, any log left over from the previous run is replayed
 *         to the console before it is cleared for the new session.
 *         Call once, early in app_main() right after nvs_flash_init().
 */
esp_err_t app_log_init(void);
