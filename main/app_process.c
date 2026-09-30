#include "app_process.h"
#include "app_machine.h"
#include "app_settings.h"
#include "bsp_pcf8575.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "ui/screens/ui_scrRunAuto.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PROC_TAG "APP_PROCESS"

/* Safety ceiling on PRESSING — SCREW_PRESS/TOP_GATE must never run
   unattended forever if SENSOR_MIXER_UPPER fails to trigger (stuck float,
   jam, etc). Motors are not rated for indefinite continuous duty. */
#define PROC_PRESSING_MAX_US   ((int64_t)30 * 60 * 1000000LL)

/* ---- State machine states ---- */
typedef enum {
    PROC_IDLE = 0,
    PROC_FILLING,      /* SUMP_PUMP on, wait for input-tank upper sensor               */
    PROC_PRESSING,     /* Preheat + periodic mixer; press until mixer upper sensor     */
    PROC_DRYING,       /* HEATER on + periodic mixer, wait for drying-time timer       */
    PROC_DISCHARGING,  /* BOTTOM_GATE open 5 s, then back to IDLE                     */
} proc_state_t;

static volatile proc_state_t s_state          = PROC_IDLE;
static volatile bool          s_start_request = false;
static int64_t                s_state_enter_us  = 0;
static int64_t                s_process_start_us = 0;

/* Mixer cycle (used during PRESSING and DRYING) */
static int64_t s_mixer_phase_start_us = 0;
static int64_t s_mixer_run_start_us = 0;
static bool    s_mixer_running        = false; /* true = currently ON */
static bool    s_mixer_reverse        = false;

/* Pre-computed µs durations — updated at state entry, not every tick */
static int64_t s_mixer_interval_us = 0;
static int64_t s_mixer_run_us      = 0;
static int64_t s_mixer_direction_us = 0;
static int64_t s_drying_us         = 0;
static int64_t s_discharge_us      = 0;

/* Input tank refill timer (used in PRESSING — lower float sensor removed) */
static int64_t s_refill_delay_us      = 0;
static int64_t s_refill_wait_start_us = 0;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Set relay hardware and update the Run Auto indicator.
   Indicator is only updated to the requested state if the I2C write actually
   succeeded — otherwise the UI would show a relay as ON/green while the
   hardware silently failed to energise it (or failed to de-energise it). */
static bool set_relay(uint8_t relay_num, bool on)
{
    esp_err_t err = pcf8575_set_relay(relay_num, on);
    if (err != ESP_OK) {
        ESP_LOGE(PROC_TAG, "Relay %d write failed (%s) — indicator left unchanged",
                 relay_num, esp_err_to_name(err));
        return false;
    }
    if (lvgl_port_lock(1000)) {
        app_machine_update_indicator(relay_num, on);
        lvgl_port_unlock();
    }
    return true;
}

static bool set_mixer_outputs(mixer_direction_t direction)
{
    if (direction == MIXER_DIRECTION_OFF) {
        esp_err_t err = pcf8575_set_mixer_direction(MIXER_DIRECTION_OFF);
        bool master_off = set_relay(RELAY_MIXER, false);
        if (err != ESP_OK) {
            ESP_LOGE(PROC_TAG, "Mixer direction OFF write failed (%s)", esp_err_to_name(err));
        }
        return err == ESP_OK && master_off;
    }

    if (!set_relay(RELAY_MIXER, true)) return false;
    esp_err_t err = pcf8575_set_mixer_direction(direction);
    if (err != ESP_OK) {
        ESP_LOGE(PROC_TAG, "Mixer direction write failed (%s)", esp_err_to_name(err));
        set_relay(RELAY_MIXER, false);
        return false;
    }
    return true;
}

/* Update the elapsed-time label on the Run Auto screen (HH:MM:SS). */
static void update_time_display(void)
{
    if (!uic_lblCurrentStatus1) return;
    int64_t elapsed_s = (esp_timer_get_time() - s_process_start_us) / 1000000LL;
    int h = (int)(elapsed_s / 3600);
    int m = (int)((elapsed_s % 3600) / 60);
    int s = (int)(elapsed_s % 60);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    if (lvgl_port_lock(1000)) {
        lv_label_set_text(uic_lblCurrentStatus1, buf);
        lvgl_port_unlock();
    }
}

/* Update the mixer direction/phase label during PRESSING and DRYING. */
static void update_mixer_display(void)
{
    if (!uic_lblMixerStatus) return;
    char buf[20];
    if (s_state != PROC_PRESSING && s_state != PROC_DRYING) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        int64_t phase_s = (esp_timer_get_time() - s_mixer_phase_start_us) / 1000000LL;
        int m = (int)(phase_s / 60);
        int s = (int)(phase_s % 60);
        if (s_mixer_running) {
            snprintf(buf, sizeof(buf), "%s %02d:%02d", s_mixer_reverse ? "REV" : "FWD", m, s);
        } else {
            snprintf(buf, sizeof(buf), "WAIT %02d:%02d", m, s);
        }
    }
    if (lvgl_port_lock(1000)) {
        lv_label_set_text(uic_lblMixerStatus, buf);
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
    esp_err_t mixer_err = pcf8575_set_mixer_direction(MIXER_DIRECTION_OFF);
    if (mixer_err != ESP_OK) {
        ESP_LOGE(PROC_TAG, "Mixer direction OFF write failed (%s)", esp_err_to_name(mixer_err));
    }
    esp_err_t err = pcf8575_set_all_relays(0x00);
    if (err != ESP_OK) {
        ESP_LOGE(PROC_TAG, "all-relays-off write failed (%s)", esp_err_to_name(err));
    }
    if (lvgl_port_lock(1000)) {
        for (uint8_t i = 0; i < RELAY_COUNT; i++) {
            app_machine_update_indicator(i, false);
        }
        if (uic_lblCurrentStatus) {
            lv_label_set_text(uic_lblCurrentStatus, "IDLE");
        }
        if (uic_lblCurrentStatus1) {
            lv_label_set_text(uic_lblCurrentStatus1, "00:00:00");
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
        s_process_start_us = esp_timer_get_time();  /* record overall run start */
        set_relay(RELAY_SUMP_PUMP,  true);   /* pump sludge into input tank */
        set_status("FILLING INPUT TANK");
        break;

    case PROC_PRESSING:
        set_relay(RELAY_SUMP_PUMP,   false);  /* tank just filled; tick_input_tank takes over */
        set_relay(RELAY_TOP_GATE,    true);   /* open mixer upper gate to receive pressed solids */
        set_relay(RELAY_SCREW_PRESS, true);
        set_relay(RELAY_HEATER,      true);   /* begin preheating while solids are pressed */
        s_refill_delay_us      = (int64_t)(app_settings_get_refill_delay_min() * 60.0f * 1e6f);
        s_refill_wait_start_us = s_state_enter_us;  /* pump is off; start counting down to next refill */
        s_mixer_phase_start_us = s_state_enter_us;
        s_mixer_run_start_us   = 0;
        s_mixer_running        = false;
        s_mixer_reverse        = false;
        s_mixer_interval_us = (int64_t)(app_settings_get_mixer_interval_min() * 60.0f * 1e6f);
        s_mixer_run_us = (int64_t)(app_settings_get_mixer_run_time_min() * 60.0f * 1e6f);
        s_mixer_direction_us = (int64_t)(app_settings_get_mixer_direction_time_min() * 60.0f * 1e6f);
        set_status("PREHEAT + PRESSING");
        break;

    case PROC_DRYING:
        set_relay(RELAY_SCREW_PRESS, false);
        set_relay(RELAY_TOP_GATE,    false);  /* close mixer upper gate – mixer is full */
        set_relay(RELAY_SUMP_PUMP,   false);  /* stop input tank refill during drying */
        set_relay(RELAY_HEATER,      true);   /* continue preheating through drying */
        s_drying_us         = (int64_t)(app_settings_get_drying_time_min()     * 60.0f * 1e6f);
        set_status("DRYING");
        break;

    case PROC_DISCHARGING:
        set_relay(RELAY_HEATER,      false);
        set_relay(RELAY_BOTTOM_GATE, true);
        s_mixer_running = false;
        s_mixer_reverse = false;
        set_mixer_outputs(MIXER_DIRECTION_FORWARD);
        s_discharge_us = (int64_t)(app_settings_get_discharge_time_min() * 60.0f * 1e6f);
        set_status("DISCHARGING");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Periodic mixer cycle (call each tick during PRESSING and DRYING)   */
/* Mixer runs every mixer_interval for mixer_run_time, then pauses.   */
/* ------------------------------------------------------------------ */

static void tick_mixer(void)
{
    int64_t now = esp_timer_get_time();

    if (!s_mixer_running) {
        if ((now - s_mixer_phase_start_us) >= s_mixer_interval_us) {
            if (set_mixer_outputs(MIXER_DIRECTION_FORWARD)) {
                s_mixer_running = true;
                s_mixer_reverse = false;
                s_mixer_run_start_us = now;
                s_mixer_phase_start_us = now;
            }
        }
    } else {
        if ((now - s_mixer_run_start_us) >= s_mixer_run_us) {
            set_mixer_outputs(MIXER_DIRECTION_OFF);
            s_mixer_running        = false;
            s_mixer_phase_start_us = now;
        } else if ((now - s_mixer_phase_start_us) >= s_mixer_direction_us) {
            mixer_direction_t next_direction = s_mixer_reverse ?
                MIXER_DIRECTION_FORWARD : MIXER_DIRECTION_REVERSE;
            if (set_mixer_outputs(next_direction)) {
                s_mixer_reverse = !s_mixer_reverse;
                s_mixer_phase_start_us = now;
            } else {
                set_mixer_outputs(MIXER_DIRECTION_OFF);
                s_mixer_running = false;
                s_mixer_phase_start_us = now;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Input tank level control (call each tick during PRESSING)           */
/* Lower float sensor removed (fouled by sticky residual waste).       */
/* Upper float → stop pump, then wait Refill Delay before restarting.  */
/* ------------------------------------------------------------------ */

static void tick_input_tank(void)
{
    bool upper   = pcf8575_get_sensor_cached(SENSOR_INPUT_TANK_UPPER);
    bool pump_on = pcf8575_get_relay(RELAY_SUMP_PUMP);

    if (pump_on) {
        if (upper) {
            set_relay(RELAY_SUMP_PUMP, false);
            s_refill_wait_start_us = esp_timer_get_time();
        }
    } else {
        if ((esp_timer_get_time() - s_refill_wait_start_us) >= s_refill_delay_us) {
            set_relay(RELAY_SUMP_PUMP, true);
        }
    }
}

/* Settling and filter pump control has been moved to app_machine.c    */
/* (liquid_path_task) so the transfer pumps run independently of the   */
/* auto process state — always active while the machine is powered.    */

/* ------------------------------------------------------------------ */
/* FreeRTOS process task                                               */
/* ------------------------------------------------------------------ */

static void process_task(void *arg)
{
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));

        /* Single I2C read shared by all sensor checks this tick */
        pcf8575_update_sensor_cache();

        if (s_state == PROC_IDLE) {
            if (s_start_request) {
                s_start_request = false;
                enter_state(PROC_FILLING);
            }
            continue;
        }

        /* Update elapsed time display every tick (100 ms; label only redraws on change) */
        update_time_display();
        update_mixer_display();

        switch (s_state) {

        case PROC_FILLING:
            if (pcf8575_get_sensor_cached(SENSOR_INPUT_TANK_UPPER)) {
                enter_state(PROC_PRESSING);
            }
            break;

        case PROC_PRESSING:
            tick_mixer();         /* prevent solids from accumulating at the mixer inlet */
            tick_input_tank();   /* maintain input tank level while screw press runs */
            if (pcf8575_get_sensor_cached(SENSOR_MIXER_UPPER)) {
                /* Mixer is full of solids – close upper gate, begin drying */
                enter_state(PROC_DRYING);
            } else if ((esp_timer_get_time() - s_state_enter_us) >= PROC_PRESSING_MAX_US) {
                /* Safety timeout: SENSOR_MIXER_UPPER never triggered — do not let
                   the screw press run unattended indefinitely. */
                ESP_LOGE(PROC_TAG, "PRESSING exceeded safety limit without MIXER_UPPER — stopping");
                enter_state(PROC_IDLE);
            }
            break;

        case PROC_DRYING: {
            tick_mixer();
            if ((esp_timer_get_time() - s_state_enter_us) >= s_drying_us) {
                enter_state(PROC_DISCHARGING);
            }
            break;
        }

        case PROC_DISCHARGING:
            if ((esp_timer_get_time() - s_state_enter_us) >= s_discharge_us) {
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
