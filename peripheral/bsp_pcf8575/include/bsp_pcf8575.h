#ifndef _BSP_PCF8575_H_
#define _BSP_PCF8575_H_

#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stdint.h>

/* PCF8575 I2C address (A0=A1=A2=GND) */
#define PCF8575_I2C_ADDR        0x20

/* ---- Relay output bit positions (P0x, low byte, active LOW) ---- */
/* Relay ON  = write 0 to the bit
   Relay OFF = write 1 to the bit                                   */
#define RELAY_HEATER            0   /* P00 */
#define RELAY_TOP_GATE          1   /* P01 */
#define RELAY_BOTTOM_GATE       2   /* P02 */
#define RELAY_FILTER_PUMP       3   /* P03 */
#define RELAY_SETTLING_PUMP     4   /* P04 */
#define RELAY_SUMP_PUMP         5   /* P05 */
#define RELAY_SCREW_PRESS       6   /* P06 */
#define RELAY_MIXER             7   /* P07 */
#define RELAY_COUNT             8

/* ---- Sensor input bit positions (P1x, high byte, active LOW) ---- */
/* Sensor TRIGGERED = pin reads 0
   Sensor IDLE      = pin reads 1                                    */
#define SENSOR_INPUT_TANK_LOWER     8   /* P10 */
#define SENSOR_INPUT_TANK_UPPER     9   /* P11 */
#define SENSOR_SETTLING_LOWER      10   /* P12 */
#define SENSOR_SETTLING_UPPER      11   /* P13 */
#define SENSOR_FILTER_LOWER        12   /* P14 */
#define SENSOR_FILTER_UPPER        13   /* P15 */
#define SENSOR_MIXER_UPPER         14   /* P16 */

/* ------------------------------------------------------------------ */

/**
 * @brief  Initialize PCF8575: register on I2C bus and set all relays OFF.
 * @return ESP_OK on success, ESP_FAIL if I2C device registration fails.
 */
esp_err_t pcf8575_init(void);

/**
 * @brief  Set a single relay ON or OFF.
 * @param  relay_num  One of the RELAY_xxx defines (0–7).
 * @param  on         true = relay energised (ON), false = relay off.
 */
esp_err_t pcf8575_set_relay(uint8_t relay_num, bool on);

/**
 * @brief  Set all 8 relays at once.
 * @param  relay_byte  Bitmask where bit=1 means relay ON, bit=0 means OFF.
 *                     This is caller-friendly (inverted internally for active-LOW).
 */
esp_err_t pcf8575_set_all_relays(uint8_t relay_byte);

/**
 * @brief  Return the current (cached) ON/OFF state of a relay.
 * @param  relay_num  One of the RELAY_xxx defines (0–7).
 * @return true if relay is currently ON.
 */
bool pcf8575_get_relay(uint8_t relay_num);

/**
 * @brief  Read the 16-bit pin state directly from the PCF8575.
 * @param  state_out  Receives raw 16-bit value (P0x in low byte, P1x in high byte).
 *                    For sensor input pins (active LOW): bit=0 means triggered.
 */
esp_err_t pcf8575_read_inputs(uint16_t *state_out);

/**
 * @brief  Convenience helper: return true if the given sensor is triggered.
 *         Performs a fresh I2C read each call — prefer pcf8575_get_sensor_cached()
 *         when querying multiple sensors in the same tick.
 * @param  sensor_bit  One of the SENSOR_xxx defines (8–14).
 */
bool pcf8575_get_sensor(uint8_t sensor_bit);

/**
 * @brief  Read all 16 input pins once and store in an internal cache.
 *         Call this once per task tick, then use pcf8575_get_sensor_cached()
 *         for all individual sensor checks in that same tick.
 * @return ESP_OK on success.
 */
esp_err_t pcf8575_update_sensor_cache(void);

/**
 * @brief  Return sensor state from the last pcf8575_update_sensor_cache() call.
 *         No I2C transaction — instant.
 * @param  sensor_bit  One of the SENSOR_xxx defines (8–14).
 * @return true if sensor is triggered (active LOW = reads 0).
 */
bool pcf8575_get_sensor_cached(uint8_t sensor_bit);

/* ------------------------------------------------------------------ */
/*  Interrupt support                                                  */

/**
 * @brief  Configure the PCF8575 ~INT GPIO and install the falling-edge ISR.
 *         The ISR gives the semaphore from pcf8575_get_int_semaphore() each
 *         time the INT pin falls (i.e. any input pin changes state).
 * @param  gpio  GPIO number wired to the PCF8575 ~INT pin (e.g. GPIO_NUM_3).
 * @return ESP_OK on success.
 */
esp_err_t pcf8575_int_gpio_init(gpio_num_t gpio);

/**
 * @brief  Return the binary semaphore that is given by the PCF8575 ISR.
 *         Call xSemaphoreTake() on this to block until a sensor input changes.
 *         Returns NULL if pcf8575_int_gpio_init() has not been called yet.
 */
SemaphoreHandle_t pcf8575_get_int_semaphore(void);

#endif /* _BSP_PCF8575_H_ */
