#include "bsp_pcf8575.h"
#include "bsp_i2c.h"
#include "esp_log.h"

#define PCF8575_TAG    "PCF8575"
#define PCF8575_INFO(fmt, ...)  ESP_LOGI(PCF8575_TAG, fmt, ##__VA_ARGS__)
#define PCF8575_ERROR(fmt, ...) ESP_LOGE(PCF8575_TAG, fmt, ##__VA_ARGS__)

/*
 * Internal state: bit = 1 means pin is HIGH.
 * Relays (P0x, low byte):  1 = relay OFF,  0 = relay ON  (active LOW)
 * Sensors (P1x, high byte): must always be written 1 so they float for reading.
 * Initial value 0xFFFF → all relays OFF, all sensor pins pulled high.
 */
static uint16_t s_state = 0xFFFF;
static i2c_master_dev_handle_t s_dev = NULL;

/* Push the current s_state to the PCF8575 over I2C */
static esp_err_t pcf8575_flush(void)
{
    uint8_t buf[2] = {
        (uint8_t)(s_state & 0x00FF),         /* P00–P07 (relays) */
        (uint8_t)((s_state >> 8) & 0x00FF),  /* P10–P17 (sensors, kept HIGH) */
    };
    return i2c_write(s_dev, buf, sizeof(buf));
}

/* ------------------------------------------------------------------ */

esp_err_t pcf8575_init(void)
{
    s_dev = i2c_dev_register(PCF8575_I2C_ADDR);
    if (s_dev == NULL) {
        PCF8575_ERROR("Failed to register PCF8575 at I2C address 0x%02X", PCF8575_I2C_ADDR);
        return ESP_FAIL;
    }

    s_state = 0xFFFF; /* all relays OFF, sensor input pins float HIGH */
    esp_err_t err = pcf8575_flush();
    if (err != ESP_OK) {
        PCF8575_ERROR("Initial write failed: %s", esp_err_to_name(err));
        return err;
    }

    PCF8575_INFO("PCF8575 initialised at 0x%02X — all relays OFF", PCF8575_I2C_ADDR);
    return ESP_OK;
}

esp_err_t pcf8575_set_relay(uint8_t relay_num, bool on)
{
    if (relay_num >= RELAY_COUNT) return ESP_ERR_INVALID_ARG;
    if (on) {
        s_state &= ~(1U << relay_num);   /* clear bit → drive LOW → relay ON  */
    } else {
        s_state |=  (1U << relay_num);   /* set   bit → drive HIGH → relay OFF */
    }
    /* Ensure sensor input bits always stay HIGH */
    s_state |= 0xFF00;
    return pcf8575_flush();
}

esp_err_t pcf8575_set_all_relays(uint8_t relay_byte)
{
    /*
     * Caller convention: relay_byte bit=1 means relay ON, bit=0 means OFF.
     * Invert for active-LOW hardware, then merge with sensor bits kept HIGH.
     */
    s_state = 0xFF00 | ((uint8_t)(~relay_byte));
    return pcf8575_flush();
}

bool pcf8575_get_relay(uint8_t relay_num)
{
    if (relay_num >= RELAY_COUNT) return false;
    /* bit=0 in s_state means relay is ON (active LOW) */
    return !(s_state & (1U << relay_num));
}

esp_err_t pcf8575_read_inputs(uint16_t *state_out)
{
    if (state_out == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t buf[2] = {0, 0};
    esp_err_t err = i2c_read(s_dev, buf, sizeof(buf));
    if (err != ESP_OK) return err;
    *state_out = ((uint16_t)buf[1] << 8) | buf[0];
    return ESP_OK;
}

bool pcf8575_get_sensor(uint8_t sensor_bit)
{
    uint16_t raw = 0;
    if (pcf8575_read_inputs(&raw) != ESP_OK) return false;
    /* active LOW: bit=0 means sensor triggered → return true */
    return !(raw & (1U << sensor_bit));
}
