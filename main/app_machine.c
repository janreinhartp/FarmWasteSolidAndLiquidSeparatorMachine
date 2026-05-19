#include "app_machine.h"
#include "bsp_pcf8575.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui/screens/ui_scrTestMachine.h"
#include "ui/screens/ui_scrRunAuto.h"

#define APP_MACHINE_TAG     "APP_MACHINE"
#define PCF8575_INT_GPIO    GPIO_NUM_3          /* P4_IO3 wired to PCF8575 ~INT */
#define SENSOR_WATCHDOG_MS  2000                /* fallback read if INT edge missed */

/* ---- Colour palette ---- */
#define COLOR_RELAY_ON   lv_color_hex(0x00AA00)  /* Green  – relay energised  */
#define COLOR_RELAY_OFF  lv_color_hex(0x404040)  /* Grey   – relay off        */

/* ------------------------------------------------------------------ */

static void set_btn_color(lv_obj_t *btn, bool on)
{
    if (btn == NULL) return;
    lv_obj_set_style_bg_color(btn,
                              on ? COLOR_RELAY_ON : COLOR_RELAY_OFF,
                              LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* Human-readable names indexed by relay_num (RELAY_xxx order) */
static const char * const RELAY_NAMES[RELAY_COUNT] = {
    "HEATER",         /* 0 – P00 */
    "TOP_GATE",       /* 1 – P01 */
    "BOTTOM_GATE",    /* 2 – P02 */
    "FILTER_PUMP",    /* 3 – P03 */
    "SETTLING_PUMP",  /* 4 – P04 */
    "SUMP_PUMP",      /* 5 – P05 */
    "SCREW_PRESS",    /* 6 – P06 */
    "MIXER",          /* 7 – P07 */
};

/* Event callback shared by all 8 Test Machine relay buttons.
   relay_num is stored as user_data (cast to uintptr_t). */
static void test_relay_toggle_cb(lv_event_t *e)
{
    uint8_t relay_num = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *btn     = lv_event_get_target(e);

    bool new_state = !pcf8575_get_relay(relay_num);

    ESP_LOGI(APP_MACHINE_TAG,
             ">>> RELAY %-14s | bit %d | PCF8575 P0%d | Relay board IN%d | -> %s (pin driven %s)",
             RELAY_NAMES[relay_num],
             relay_num,
             relay_num,
             relay_num + 1,
             new_state ? "ON" : "OFF",
             new_state ? "LOW" : "HIGH");

    esp_err_t err = pcf8575_set_relay(relay_num, new_state);
    if (err != ESP_OK) {
        ESP_LOGE(APP_MACHINE_TAG,
                 "    I2C WRITE FAILED: %s", esp_err_to_name(err));
        return; /* Don't update colour — hardware state is unknown */
    }
    set_btn_color(btn, new_state);
}

/* ------------------------------------------------------------------ */

/* Called when the Test Machine screen is unloaded (navigating away).
   Turns off all relays so no load stays energised after leaving the screen. */
static void test_machine_unloaded_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(APP_MACHINE_TAG, "Test Machine screen unloaded — turning all relays OFF");
    app_machine_all_relays_off();
}

/* ------------------------------------------------------------------ */

void app_machine_init(void)
{
    /* ---- Test Machine screen ---- */
    /* Set all relay buttons to OFF colour */
    set_btn_color(uic_testSumpPump,           false);
    set_btn_color(uic_testScrewPress,         false);
    set_btn_color(uic_testTopGate,            false);
    set_btn_color(uic_testMixer,              false);
    set_btn_color(uic_testHeater,             false);
    set_btn_color(uic_testBottomGate,         false);
    set_btn_color(uic_testSettlingTankPump,   false);
    set_btn_color(uic_testFilterTankPump,     false);

    /* Register latching-toggle click events (relay number as user_data) */
    lv_obj_add_event_cb(uic_testSumpPump,
                        test_relay_toggle_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)RELAY_SUMP_PUMP);
    lv_obj_add_event_cb(uic_testScrewPress,
                        test_relay_toggle_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)RELAY_SCREW_PRESS);
    lv_obj_add_event_cb(uic_testTopGate,
                        test_relay_toggle_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)RELAY_TOP_GATE);
    lv_obj_add_event_cb(uic_testMixer,
                        test_relay_toggle_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)RELAY_MIXER);
    lv_obj_add_event_cb(uic_testHeater,
                        test_relay_toggle_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)RELAY_HEATER);
    lv_obj_add_event_cb(uic_testBottomGate,
                        test_relay_toggle_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)RELAY_BOTTOM_GATE);
    lv_obj_add_event_cb(uic_testSettlingTankPump,
                        test_relay_toggle_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)RELAY_SETTLING_PUMP);
    lv_obj_add_event_cb(uic_testFilterTankPump,
                        test_relay_toggle_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)RELAY_FILTER_PUMP);

    /* ---- Run Auto screen — relay buttons are STATUS INDICATORS only ---- */
    /* Remove clickable flag so accidental touches do nothing */
    lv_obj_remove_flag(uic_SumpPump,          LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(uic_ScrewPress,        LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(uic_TopGate,           LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(uic_Mixer,             LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(uic_Heater,            LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(uic_BottomGate,        LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(uic_SettlingTankPump,  LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(uic_FilterTankPump,    LV_OBJ_FLAG_CLICKABLE);

    /* ---- Clear default "Checkbox" label text on all sensor indicators ---- */
    /* RunAuto screen */
    lv_checkbox_set_text(uic_UpperLimitInputTank,    "");
    lv_checkbox_set_text(uic_LowerLimitInputTank,    "");
    lv_checkbox_set_text(uic_UpperLimitMixer,        "");
    lv_checkbox_set_text(uic_UpperLimitSettlingTank, "");
    lv_checkbox_set_text(uic_LowerLimitSettlingTank, "");
    lv_checkbox_set_text(uic_UpperLimitFilterTank,   "");
    lv_checkbox_set_text(uic_LowerLimitFilterTank,   "");
    /* TestMachine screen */
    lv_checkbox_set_text(uic_TestUpperLimitInputTank,    "");
    lv_checkbox_set_text(uic_TestLowerLimitInputTank,    "");
    lv_checkbox_set_text(uic_TestUpperLimitMixer,        "");
    lv_checkbox_set_text(uic_TestUpperLimitSettlingTank, "");
    lv_checkbox_set_text(uic_TestLowerLimitSettlingTank, "");
    lv_checkbox_set_text(uic_TestUpperLimitFilterTank,   "");
    lv_checkbox_set_text(uic_TestLowerLimitFilterTank,   "");

    /* Turn off all relays on screen unload so no load stays energised */
    lv_obj_add_event_cb(uic_scrTestMachine,
                        test_machine_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    /* Start all indicators grey */
    app_machine_all_relays_off();

    /* Start sensor polling timer (200 ms, runs on LVGL task) */
    app_machine_start_sensor_polling();
}

void app_machine_all_relays_off(void)
{
    /* Hardware: de-energise all 8 relays */
    pcf8575_set_all_relays(0x00);

    /* Update Run Auto indicator colours */
    set_btn_color(uic_SumpPump,          false);
    set_btn_color(uic_ScrewPress,        false);
    set_btn_color(uic_TopGate,           false);
    set_btn_color(uic_Mixer,             false);
    set_btn_color(uic_Heater,            false);
    set_btn_color(uic_BottomGate,        false);
    set_btn_color(uic_SettlingTankPump,  false);
    set_btn_color(uic_FilterTankPump,    false);

    /* Update Test Machine button colours */
    set_btn_color(uic_testSumpPump,          false);
    set_btn_color(uic_testScrewPress,        false);
    set_btn_color(uic_testTopGate,           false);
    set_btn_color(uic_testMixer,             false);
    set_btn_color(uic_testHeater,            false);
    set_btn_color(uic_testBottomGate,        false);
    set_btn_color(uic_testSettlingTankPump,  false);
    set_btn_color(uic_testFilterTankPump,    false);
}

void app_machine_update_indicator(uint8_t relay_num, bool on)
{
    lv_obj_t *indicators[RELAY_COUNT] = {
        uic_Heater,           /* 0 – RELAY_HEATER        – P00 */
        uic_TopGate,          /* 1 – RELAY_TOP_GATE      – P01 */
        uic_BottomGate,       /* 2 – RELAY_BOTTOM_GATE   – P02 */
        uic_FilterTankPump,   /* 3 – RELAY_FILTER_PUMP   – P03 */
        uic_SettlingTankPump, /* 4 – RELAY_SETTLING_PUMP – P04 */
        uic_SumpPump,         /* 5 – RELAY_SUMP_PUMP     – P05 */
        uic_ScrewPress,       /* 6 – RELAY_SCREW_PRESS   – P06 */
        uic_Mixer,            /* 7 – RELAY_MIXER         – P07 */
    };

    if (relay_num < RELAY_COUNT) {
        set_btn_color(indicators[relay_num], on);
    }
}

/* ------------------------------------------------------------------ */
/*  Sensor update — called from sensor_int_task with LVGL lock held   */

static const struct {
    uint8_t     sensor_bit;
    lv_obj_t  **runauto;
    lv_obj_t  **testmachine;
    const char *name;
} s_sensor_map[] = {
    { SENSOR_INPUT_TANK_UPPER,  &uic_UpperLimitInputTank,    &uic_TestUpperLimitInputTank,    "INPUT_TANK_UPPER"    },
    { SENSOR_INPUT_TANK_LOWER,  &uic_LowerLimitInputTank,    &uic_TestLowerLimitInputTank,    "INPUT_TANK_LOWER"    },
    { SENSOR_MIXER_UPPER,       &uic_UpperLimitMixer,        &uic_TestUpperLimitMixer,        "MIXER_UPPER"         },
    { SENSOR_SETTLING_UPPER,    &uic_UpperLimitSettlingTank, &uic_TestUpperLimitSettlingTank, "SETTLING_UPPER"      },
    { SENSOR_SETTLING_LOWER,    &uic_LowerLimitSettlingTank, &uic_TestLowerLimitSettlingTank, "SETTLING_LOWER"      },
    { SENSOR_FILTER_UPPER,      &uic_UpperLimitFilterTank,   &uic_TestUpperLimitFilterTank,   "FILTER_UPPER"        },
    { SENSOR_FILTER_LOWER,      &uic_LowerLimitFilterTank,   &uic_TestLowerLimitFilterTank,   "FILTER_LOWER"        },
};
#define SENSOR_MAP_LEN  (sizeof(s_sensor_map) / sizeof(s_sensor_map[0]))

static bool s_prev_sensor_state[SENSOR_MAP_LEN] = {0};

/* Must be called with the LVGL port lock already held. */
static void sensor_update_ui(void)
{
    for (int i = 0; i < (int)SENSOR_MAP_LEN; i++) {
        bool triggered = pcf8575_get_sensor_cached(s_sensor_map[i].sensor_bit);

        /* Log only on state change to avoid flooding the console */
        if (triggered != s_prev_sensor_state[i]) {
            ESP_LOGI(APP_MACHINE_TAG,
                     ">>> SENSOR %-20s | bit %2d | -> %s",
                     s_sensor_map[i].name,
                     s_sensor_map[i].sensor_bit,
                     triggered ? "TRIGGERED" : "CLEARED");
            s_prev_sensor_state[i] = triggered;
        }

        lv_obj_t *ra = *s_sensor_map[i].runauto;
        lv_obj_t *tm = *s_sensor_map[i].testmachine;

        if (triggered) {
            if (ra) lv_obj_add_state(ra, LV_STATE_CHECKED);
            if (tm) lv_obj_add_state(tm, LV_STATE_CHECKED);
        } else {
            if (ra) lv_obj_clear_state(ra, LV_STATE_CHECKED);
            if (tm) lv_obj_clear_state(tm, LV_STATE_CHECKED);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Interrupt-driven sensor task                                       */

static void sensor_int_task(void *arg)
{
    SemaphoreHandle_t sem = pcf8575_get_int_semaphore();

    for (;;) {
        /* Block until INT fires or the watchdog timeout expires */
        xSemaphoreTake(sem, pdMS_TO_TICKS(SENSOR_WATCHDOG_MS));

        /* I2C read — outside the LVGL lock (no LVGL calls here) */
        pcf8575_update_sensor_cache();

        /* Update LVGL widgets under lock */
        if (lvgl_port_lock(1000)) {
            sensor_update_ui();
            lvgl_port_unlock();
        }
    }
}

void app_machine_start_sensor_polling(void)
{
    /* Configure INT GPIO and attach ISR */
    esp_err_t err = pcf8575_int_gpio_init(PCF8575_INT_GPIO);
    if (err != ESP_OK) {
        ESP_LOGE(APP_MACHINE_TAG, "INT GPIO init failed: %s — sensor task will use watchdog only",
                 esp_err_to_name(err));
    }

    /* Create the sensor task (small stack, low priority) */
    xTaskCreate(sensor_int_task, "sensor_int", 3072, NULL, 3, NULL);
}
