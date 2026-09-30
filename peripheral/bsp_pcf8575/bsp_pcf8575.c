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
static i2c_master_dev_handle_t s_mixer_dev = NULL;
static SemaphoreHandle_t s_mixer_mutex = NULL;
static mixer_direction_t s_mixer_direction = MIXER_DIRECTION_OFF;
/* Cached sensor read — updated once per tick via pcf8575_update_sensor_cache() */
static volatile uint16_t s_input_cache = 0xFFFF;
/* Guards s_state read-modify-write-flush: process_task, liquid_path_task and
   UI button callbacks all call pcf8575_set_relay() concurrently. */
static SemaphoreHandle_t s_state_mutex = NULL;

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

    /* ISR service is installed once in system_init() — just add the handler here */
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

/* ---- Relay state logging ---- */

static const char * const s_relay_names[RELAY_COUNT] = {
    "HEATER",        /* 0 – P00 */
    "TOP_GATE",      /* 1 – P01 */
    "BOTTOM_GATE",   /* 2 – P02 */
    "FILTER_PUMP",   /* 3 – P03 */
    "SETTLING_PUMP", /* 4 – P04 */
    "SUMP_PUMP",     /* 5 – P05 */
    "SCREW_PRESS",   /* 6 – P06 */
    "MIXER",         /* 7 – P07 */
};

/* Tracks the last relay byte that was logged — logs only on change. */
static uint8_t s_prev_relay_byte = 0xFF;   /* matches init state (all OFF) */

static void log_relay_states(uint8_t relay_byte)
{
    /* relay_byte is the raw PCF8575 P0x byte: bit=0 → relay ON, bit=1 → relay OFF */
    PCF8575_INFO("+---------------+-------+");
    PCF8575_INFO("| %-13s | State |", "Relay");
    PCF8575_INFO("+---------------+-------+");
    for (int i = 0; i < RELAY_COUNT; i++) {
        bool on = !(relay_byte & (1U << i));
        PCF8575_INFO("| %-13s |  %-3s  |", s_relay_names[i], on ? "ON" : "off");
    }
    PCF8575_INFO("+---------------+-------+");
}

/* ------------------------------------------------------------------ */

/* Push the current s_state to the PCF8575 over I2C, then verify the relay byte. */
static esp_err_t pcf8575_flush(void)
{
    uint8_t buf[2] = {
        (uint8_t)(s_state & 0x00FF),         /* P00–P07 (relays) */
        (uint8_t)((s_state >> 8) & 0x00FF),  /* P10–P17 (sensors, kept HIGH) */
    };

    esp_err_t err = i2c_write(s_dev, buf, sizeof(buf));
    if (err != ESP_OK) return err;

    /* ---- Readback verify (relay byte only — sensor byte reflects live inputs) ---- */
    uint16_t readback = 0;
    if (pcf8575_read_inputs(&readback) != ESP_OK) {
        /* Read failed but write succeeded; treat as a non-fatal warning. */
        PCF8575_ERROR("Relay readback read failed — skipping verify");
        goto flush_ok;
    }

    uint8_t written = (uint8_t)(s_state & 0x00FF);
    uint8_t actual  = (uint8_t)(readback & 0x00FF);
    if (actual == written) goto flush_ok;   /* verified OK */

    /* Mismatch — retry the write once */
    PCF8575_ERROR("Relay readback mismatch: wrote 0x%02X got 0x%02X — retrying",
                  written, actual);
    err = i2c_write(s_dev, buf, sizeof(buf));
    if (err != ESP_OK) return err;

    if (pcf8575_read_inputs(&readback) == ESP_OK) {
        actual = (uint8_t)(readback & 0x00FF);
        if (actual != written) {
            PCF8575_ERROR("Relay mismatch persists after retry: wrote 0x%02X got 0x%02X",
                          written, actual);
            return ESP_ERR_INVALID_RESPONSE;   /* triggers s_state rollback in caller */
        }
    }

    PCF8575_INFO("Relay readback OK after retry");

flush_ok:
    {
        uint8_t relay_byte = (uint8_t)(s_state & 0x00FF);
        if (relay_byte != s_prev_relay_byte) {
            s_prev_relay_byte = relay_byte;
            log_relay_states(relay_byte);
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

esp_err_t pcf8575_init(void)
{
    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
        if (s_state_mutex == NULL) return ESP_ERR_NO_MEM;
    }

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

esp_err_t pcf8575_mixer_init(void)
{
    if (s_mixer_mutex == NULL) {
        s_mixer_mutex = xSemaphoreCreateMutex();
        if (s_mixer_mutex == NULL) return ESP_ERR_NO_MEM;
    }

    s_mixer_dev = i2c_dev_register(PCF8575_MIXER_I2C_ADDR);
    if (s_mixer_dev == NULL) {
        PCF8575_ERROR("Failed to register mixer expander at I2C address 0x%02X",
                      PCF8575_MIXER_I2C_ADDR);
        return ESP_FAIL;
    }

    uint8_t outputs[2] = {0xFF, 0xFF};
    esp_err_t err = i2c_write(s_mixer_dev, outputs, sizeof(outputs));
    if (err != ESP_OK) {
        PCF8575_ERROR("Mixer expander initial write failed: %s", esp_err_to_name(err));
        return err;
    }
    s_mixer_direction = MIXER_DIRECTION_OFF;
    PCF8575_INFO("Mixer direction expander initialised at 0x%02X — outputs OFF",
                 PCF8575_MIXER_I2C_ADDR);
    return ESP_OK;
}

esp_err_t pcf8575_set_mixer_direction(mixer_direction_t direction)
{
    if (s_mixer_dev == NULL ||
        (direction != MIXER_DIRECTION_OFF &&
         direction != MIXER_DIRECTION_FORWARD &&
         direction != MIXER_DIRECTION_REVERSE)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mixer_mutex, portMAX_DELAY);
    uint8_t low_byte = 0xFF;
    if (direction == MIXER_DIRECTION_FORWARD) low_byte &= ~(1U << 0);
    if (direction == MIXER_DIRECTION_REVERSE) low_byte &= ~(1U << 1);
    uint8_t outputs[2] = {low_byte, 0xFF};
    esp_err_t err = i2c_write(s_mixer_dev, outputs, sizeof(outputs));
    if (err == ESP_OK) s_mixer_direction = direction;
    xSemaphoreGive(s_mixer_mutex);
    return err;
}

mixer_direction_t pcf8575_get_mixer_direction(void)
{
    if (s_mixer_mutex == NULL) return MIXER_DIRECTION_OFF;
    xSemaphoreTake(s_mixer_mutex, portMAX_DELAY);
    mixer_direction_t direction = s_mixer_direction;
    xSemaphoreGive(s_mixer_mutex);
    return direction;
}

esp_err_t pcf8575_set_relay(uint8_t relay_num, bool on)
{
    if (relay_num >= RELAY_COUNT) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    uint16_t prev = s_state;
    if (on) {
        s_state &= ~(1U << relay_num);   /* clear bit → drive LOW → relay ON  */
    } else {
        s_state |=  (1U << relay_num);   /* set   bit → drive HIGH → relay OFF */
    }
    /* Ensure sensor input bits always stay HIGH */
    s_state |= 0xFF00;
    esp_err_t err = pcf8575_flush();
    if (err != ESP_OK) s_state = prev;   /* restore shadow so SW matches HW */
    xSemaphoreGive(s_state_mutex);
    return err;
}

esp_err_t pcf8575_set_all_relays(uint8_t relay_byte)
{
    /*
     * Caller convention: relay_byte bit=1 means relay ON, bit=0 means OFF.
     * Invert for active-LOW hardware, then merge with sensor bits kept HIGH.
     */
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    uint16_t prev = s_state;
    s_state = 0xFF00 | ((uint8_t)(~relay_byte));
    esp_err_t err = pcf8575_flush();
    if (err != ESP_OK) s_state = prev;   /* restore shadow so SW matches HW */
    xSemaphoreGive(s_state_mutex);
    return err;
}

bool pcf8575_get_relay(uint8_t relay_num)
{
    if (relay_num >= RELAY_COUNT) return false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    /* bit=0 in s_state means relay is ON (active LOW) */
    bool on = !(s_state & (1U << relay_num));
    xSemaphoreGive(s_state_mutex);
    return on;
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
