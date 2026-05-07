#include "app_machine.h"
#include "bsp_pcf8575.h"
#include "lvgl.h"
#include "ui/screens/ui_scrTestMachine.h"
#include "ui/screens/ui_scrRunAuto.h"

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

/* Event callback shared by all 8 Test Machine relay buttons.
   relay_num is stored as user_data (cast to uintptr_t). */
static void test_relay_toggle_cb(lv_event_t *e)
{
    uint8_t relay_num = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *btn     = lv_event_get_target(e);

    bool new_state = !pcf8575_get_relay(relay_num);
    pcf8575_set_relay(relay_num, new_state);
    set_btn_color(btn, new_state);
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
}

void app_machine_update_indicator(uint8_t relay_num, bool on)
{
    lv_obj_t *indicators[RELAY_COUNT] = {
        uic_SumpPump,
        uic_ScrewPress,
        uic_TopGate,
        uic_Mixer,
        uic_Heater,
        uic_BottomGate,
        uic_SettlingTankPump,
        uic_FilterTankPump,
    };

    if (relay_num < RELAY_COUNT) {
        set_btn_color(indicators[relay_num], on);
    }
}

/* ------------------------------------------------------------------ */
/*  Sensor polling — runs on the LVGL task via lv_timer (no lock needed) */

static void sensor_poll_cb(lv_timer_t *t)
{
    (void)t;

    /* Sensor bit → RunAuto widget, TestMachine widget */
    static const struct {
        uint8_t   sensor_bit;
        lv_obj_t **runauto;
        lv_obj_t **testmachine;
    } map[] = {
        { SENSOR_INPUT_TANK_UPPER,  &uic_UpperLimitInputTank,    &uic_TestUpperLimitInputTank    },
        { SENSOR_INPUT_TANK_LOWER,  &uic_LowerLimitInputTank,    &uic_TestLowerLimitInputTank    },
        { SENSOR_MIXER_UPPER,       &uic_UpperLimitMixer,        &uic_TestUpperLimitMixer        },
        { SENSOR_SETTLING_UPPER,    &uic_UpperLimitSettlingTank, &uic_TestUpperLimitSettlingTank },
        { SENSOR_SETTLING_LOWER,    &uic_LowerLimitSettlingTank, &uic_TestLowerLimitSettlingTank },
        { SENSOR_FILTER_UPPER,      &uic_UpperLimitFilterTank,   &uic_TestUpperLimitFilterTank   },
        { SENSOR_FILTER_LOWER,      &uic_LowerLimitFilterTank,   &uic_TestLowerLimitFilterTank   },
    };

    /* One I2C transaction for all 7 sensor checks */
    pcf8575_update_sensor_cache();

    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++) {
        bool triggered = pcf8575_get_sensor_cached(map[i].sensor_bit);

        lv_obj_t *ra = *map[i].runauto;
        lv_obj_t *tm = *map[i].testmachine;

        if (triggered) {
            if (ra) lv_obj_add_state(ra, LV_STATE_CHECKED);
            if (tm) lv_obj_add_state(tm, LV_STATE_CHECKED);
        } else {
            if (ra) lv_obj_clear_state(ra, LV_STATE_CHECKED);
            if (tm) lv_obj_clear_state(tm, LV_STATE_CHECKED);
        }
    }
}

void app_machine_start_sensor_polling(void)
{
    lv_timer_create(sensor_poll_cb, 200, NULL);
}
