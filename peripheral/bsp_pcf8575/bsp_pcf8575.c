#include "bsp_pcf8575.h"
#include "bsp_i2c.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
/* Cached sensor read — updated once per tick via pcf8575_update_sensor_cache() */
static volatile uint16_t s_input_cache = 0xFFFF;

/* ---- Interrupt support ---- */
static SemaphoreHandle_t s_int_sem = NULL;

static void IRAM_ATTR pcf8575_isr_handler(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_int_sem, &woken);
    if (woken) portYIELD_FROM_ISR();
}

esp_err_t pcf8575_int_gpio_init(gpio_num_t gpio)
{
    s_int_sem = xSemaphoreCreateBinary();
    if (!s_int_sem) return ESP_ERR_NO_MEM;

    const gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,   /* INT is active-LOW, falls on any input change */
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) return err;

    gpio_install_isr_service(0);             /* safe to call even if already installed */
    err = gpio_isr_handler_add(gpio, pcf8575_isr_handler, NULL);
    if (err == ESP_OK) {
        PCF8575_INFO("INT GPIO%d configured (falling-edge ISR)", gpio);
    }
    return err;
}

SemaphoreHandle_t pcf8575_get_int_semaphore(void)
{
    return s_int_sem;
}

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

esp_err_t pcf8575_update_sensor_cache(void)
{
    uint16_t raw = 0;
    esp_err_t err = pcf8575_read_inputs(&raw);
    if (err == ESP_OK) {
        s_input_cache = raw;
    }
    return err;
}

bool pcf8575_get_sensor_cached(uint8_t sensor_bit)
{
    /* active LOW: bit=0 means sensor triggered → return true */
    return !(s_input_cache & (1U << sensor_bit));
}
