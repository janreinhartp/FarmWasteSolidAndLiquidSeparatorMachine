#ifndef _APP_MACHINE_H_
#define _APP_MACHINE_H_

#include <stdbool.h>
#include <stdint.h>
#include "bsp_pcf8575.h"

/**
 * @brief  Called once after ui_init() to register Test Machine button
 *         callbacks and set initial button colours on both screens.
 */
void app_machine_init(void);

/**
 * @brief  Immediately de-energise all 8 relays and update the Run Auto
 *         indicator colours to grey.  Call this from the STOP handler.
 */
void app_machine_all_relays_off(void);

/**
 * @brief  Update a single Run Auto status-indicator colour.
 * @param  relay_num  One of the RELAY_xxx defines (0–7).
 * @param  on         true = green, false = grey.
 */
void app_machine_update_indicator(uint8_t relay_num, bool on);

/**
 * @brief  Start a 200 ms LVGL timer that reads all 7 sensor inputs and
 *         updates the checkbox widgets on both the TestMachine and RunAuto
 *         screens.  Must be called after ui_init().
 */
void app_machine_start_sensor_polling(void);

#endif /* _APP_MACHINE_H_ */
