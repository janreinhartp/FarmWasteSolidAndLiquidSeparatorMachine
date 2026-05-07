#include "app_process.h"
#include "app_machine.h"
#include "app_settings.h"
#include "bsp_pcf8575.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "ui/screens/ui_scrRunAuto.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

/* ---- State machine states ---- */
typedef enum {
    PROC_IDLE = 0,
    PROC_FILLING,      /* TOP_GATE + SUMP_PUMP on, wait for input-tank upper sensor   */
    PROC_PRESSING,     /* SCREW_PRESS on + periodic mixer, wait for input-tank lower   */
    PROC_DRYING,       /* HEATER on + periodic mixer, wait for drying-time timer       */
    PROC_DISCHARGING,  /* BOTTOM_GATE open 5 s, then back to IDLE                     */
} proc_state_t;

static volatile proc_state_t s_state          = PROC_IDLE;
static volatile bool          s_start_request = false;
static int64_t                s_state_enter_us = 0;

/* Mixer cycle (used in PRESSING and DRYING) */
static int64_t s_mixer_phase_start_us = 0;
static bool    s_mixer_running        = false; /* true = currently ON */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Set relay hardware and update the Run Auto indicator. */
static void set_relay(uint8_t relay_num, bool on)
{
    pcf8575_set_relay(relay_num, on);
    if (lvgl_port_lock(1000)) {
        app_machine_update_indicator(relay_num, on);
        lvgl_port_unlock();
    }
}

/* Update the status label on the Run Auto screen. */
static void set_status(const char *text)
{
    if (lvgl_port_lock(1000)) {
        if (uic_lblCurrentStatus) {
            lv_label_set_text(uic_lblCurrentStatus, text);
        }
        lvgl_port_unlock();
    }
}

/* Turn all relays off (hardware + indicators). */
static void all_off_with_ui(void)
{
    pcf8575_set_all_relays(0x00);
    if (lvgl_port_lock(1000)) {
        for (uint8_t i = 0; i < RELAY_COUNT; i++) {
            app_machine_update_indicator(i, false);
        }
        if (uic_lblCurrentStatus) {
            lv_label_set_text(uic_lblCurrentStatus, "IDLE");
        }
        lvgl_port_unlock();
    }
}

/* ------------------------------------------------------------------ */
/* State transitions                                                   */
/* ------------------------------------------------------------------ */

static void enter_state(proc_state_t new_state)
{
    s_state          = new_state;
    s_state_enter_us = esp_timer_get_time();

    switch (new_state) {

    case PROC_IDLE:
        all_off_with_ui();
        break;

    case PROC_FILLING:
        set_relay(RELAY_SUMP_PUMP,  true);   /* pump sludge into input tank */
        set_status("FILLING INPUT TANK");
        break;

    case PROC_PRESSING:
        set_relay(RELAY_SUMP_PUMP,   false);  /* tank just filled; tick_input_tank takes over */
        set_relay(RELAY_TOP_GATE,    true);   /* open mixer upper gate for screw press output */
        set_relay(RELAY_SCREW_PRESS, true);
        s_mixer_phase_start_us = s_state_enter_us;
        s_mixer_running        = false;
        set_status("SCREW PRESSING");
        break;

    case PROC_DRYING:
        set_relay(RELAY_SCREW_PRESS, false);
        set_relay(RELAY_TOP_GATE,    false);  /* close mixer upper gate – mixer is full */
        set_relay(RELAY_SUMP_PUMP,   false);  /* stop input tank refill during drying */
        set_relay(RELAY_MIXER,       false);  /* reset; tick_mixer restarts cycle from scratch */
        set_relay(RELAY_HEATER,      true);
        s_mixer_phase_start_us = s_state_enter_us;
        s_mixer_running        = false;
        set_status("DRYING");
        break;

    case PROC_DISCHARGING:
        set_relay(RELAY_HEATER,      false);
        set_relay(RELAY_BOTTOM_GATE, true);
        set_relay(RELAY_MIXER,       true);   /* run mixer to sweep out dry solids */
        set_status("DISCHARGING");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Periodic mixer cycle (call each tick during PRESSING and DRYING)   */
/* ------------------------------------------------------------------ */

static void tick_mixer(void)
{
    int64_t now         = esp_timer_get_time();
    int64_t interval_us = (int64_t)(app_settings_get_mixer_interval_min() * 60.0f * 1e6f);
    int64_t run_us      = (int64_t)(app_settings_get_mixer_run_time_min()  * 60.0f * 1e6f);

    if (!s_mixer_running) {
        if ((now - s_mixer_phase_start_us) >= interval_us) {
            s_mixer_running        = true;
            s_mixer_phase_start_us = now;
            set_relay(RELAY_MIXER, true);
        }
    } else {
        if ((now - s_mixer_phase_start_us) >= run_us) {
            s_mixer_running        = false;
            s_mixer_phase_start_us = now;
            set_relay(RELAY_MIXER, false);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Input tank level control (call each tick during PRESSING)           */
/* Lower float triggered → refill from sump; upper float → stop.      */
/* ------------------------------------------------------------------ */

static void tick_input_tank(void)
{
    bool lower   = pcf8575_get_sensor(SENSOR_INPUT_TANK_LOWER);
    bool upper   = pcf8575_get_sensor(SENSOR_INPUT_TANK_UPPER);
    bool pump_on = pcf8575_get_relay(RELAY_SUMP_PUMP);

    if (!pump_on && lower) {
        /* Tank dropped to lower limit – refill from sump */
        set_relay(RELAY_SUMP_PUMP, true);
    } else if (pump_on && upper) {
        /* Tank is full – stop */
        set_relay(RELAY_SUMP_PUMP, false);
    }
}

/* ------------------------------------------------------------------ */
/* Liquid path (call each tick in any active state)                    */
/* Settling and filter pumps are controlled independently by their own */
/* tank level sensors.                                                 */
/* ------------------------------------------------------------------ */

static void tick_liquid_path(void)
{
    /* ---- Settling tank → settling pump → filter tank ---- */
    bool settling_upper   = pcf8575_get_sensor(SENSOR_SETTLING_UPPER);
    bool settling_lower   = pcf8575_get_sensor(SENSOR_SETTLING_LOWER);
    bool settling_pump_on = pcf8575_get_relay(RELAY_SETTLING_PUMP);

    if (!settling_pump_on && settling_upper) {
        /* Upper limit reached – pump liquid to filter tank */
        set_relay(RELAY_SETTLING_PUMP, true);
    } else if (settling_pump_on && settling_lower) {
        /* Drained to lower limit – stop pump */
        set_relay(RELAY_SETTLING_PUMP, false);
    }

    /* ---- Filter tank → filter pump → treated water output ---- */
    bool filter_upper   = pcf8575_get_sensor(SENSOR_FILTER_UPPER);
    bool filter_lower   = pcf8575_get_sensor(SENSOR_FILTER_LOWER);
    bool filter_pump_on = pcf8575_get_relay(RELAY_FILTER_PUMP);

    if (!filter_pump_on && filter_upper) {
        /* Filter tank full – pump treated water out */
        set_relay(RELAY_FILTER_PUMP, true);
    } else if (filter_pump_on && filter_lower) {
        /* Drained to lower limit – stop pump */
        set_relay(RELAY_FILTER_PUMP, false);
    }
}

/* ------------------------------------------------------------------ */
/* FreeRTOS process task                                               */
/* ------------------------------------------------------------------ */

static void process_task(void *arg)
{
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));

        if (s_state == PROC_IDLE) {
            if (s_start_request) {
                s_start_request = false;
                enter_state(PROC_FILLING);
            }
            continue;
        }

        /* Liquid path active in all non-idle states */
        tick_liquid_path();

        switch (s_state) {

        case PROC_FILLING:
            if (pcf8575_get_sensor(SENSOR_INPUT_TANK_UPPER)) {
                enter_state(PROC_PRESSING);
            }
            break;

        case PROC_PRESSING:
            tick_mixer();
            tick_input_tank();   /* maintain input tank level while screw press runs */
            if (pcf8575_get_sensor(SENSOR_MIXER_UPPER)) {
                /* Mixer is full of solids – stop pressing, begin drying */
                enter_state(PROC_DRYING);  /* enter_state resets relay and mixer state */
            }
            break;

        case PROC_DRYING: {
            tick_mixer();
            int64_t drying_us = (int64_t)(app_settings_get_drying_time_min() * 60.0f * 1e6f);
            if ((esp_timer_get_time() - s_state_enter_us) >= drying_us) {
                enter_state(PROC_DISCHARGING);
            }
            break;
        }

        case PROC_DISCHARGING:
            /* Keep discharge gate open for 5 seconds */
            if ((esp_timer_get_time() - s_state_enter_us) >= 5000000LL) {
                enter_state(PROC_IDLE);
            }
            break;

        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Screen-loaded callback: auto-start process on entering Run Auto     */
/* ------------------------------------------------------------------ */

static void runauto_screen_loaded_cb(lv_event_t *e)
{
    (void)e;
    if (s_state == PROC_IDLE) {
        s_start_request = true;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void app_process_init(void)
{
    lv_obj_add_event_cb(uic_ProcessFlowControl,
                        runauto_screen_loaded_cb,
                        LV_EVENT_SCREEN_LOADED, NULL);

    xTaskCreate(process_task, "proc_task", 8192, NULL, 5, NULL);
}

void app_process_stop(void)
{
    s_state         = PROC_IDLE;
    s_start_request = false;
    all_off_with_ui();
}
