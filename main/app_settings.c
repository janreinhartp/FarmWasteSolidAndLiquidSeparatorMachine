#include "app_settings.h"
#include "lvgl.h"
#include "ui/screens/ui_scrSettings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>

/* ---- NVS keys ---- */
#define NVS_NAMESPACE    "fw_settings"
#define NVS_KEY_MIX_INT  "mix_int"
#define NVS_KEY_MIX_RT   "mix_rt"
#define NVS_KEY_DRY_T    "dry_t"

/* ---- Defaults (stored as tenths of minutes) ---- */
#define DEFAULT_MIXER_INTERVAL   100   /* 10.0 min */
#define DEFAULT_MIXER_RUNTIME     10   /* 1.0  min */
#define DEFAULT_DRYING_TIME      1200  /* 120.0 min */

typedef enum {
    SETTING_MIXER_INTERVAL = 0,
    SETTING_MIXER_RUNTIME,
    SETTING_DRYING_TIME,
    SETTING_COUNT,
} setting_idx_t;

static const char * const s_names[SETTING_COUNT] = {
    "Mixer Interval",
    "Mixer Run Time",
    "Drying Time",
};

/* Values in tenths of minutes */
static uint16_t s_values[SETTING_COUNT] = {
    DEFAULT_MIXER_INTERVAL,
    DEFAULT_MIXER_RUNTIME,
    DEFAULT_DRYING_TIME,
};

static int s_current_idx = 0;
/* Step in tenths of minutes: 1=0.1min, 10=1min, 50=5min */
static int s_step = 10;

#define COLOR_ACTIVE    lv_color_hex(0x00AA00)
#define COLOR_INACTIVE  lv_color_hex(0xCB3C3C)

/* ------------------------------------------------------------------ */

static void update_display(void)
{
    if (!uic_lblSettingTitle || !uic_lblSettingValue) return;

    lv_label_set_text(uic_lblSettingTitle, s_names[s_current_idx]);

    char buf[32];
    uint16_t v = s_values[s_current_idx];
    if (v % 10 == 0) {
        snprintf(buf, sizeof(buf), "%u Min", (unsigned)(v / 10));
    } else {
        snprintf(buf, sizeof(buf), "%.1f Min", v / 10.0f);
    }
    lv_label_set_text(uic_lblSettingValue, buf);
}

static void highlight_multiplier(int step)
{
    lv_obj_set_style_bg_color(uic_btnMultipliesPointOne,
                              step == 1  ? COLOR_ACTIVE : COLOR_INACTIVE,
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(uic_btnMultiplierOne,
                              step == 10 ? COLOR_ACTIVE : COLOR_INACTIVE,
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(uic_btnMultiplierFive,
                              step == 50 ? COLOR_ACTIVE : COLOR_INACTIVE,
                              LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* ---- Event callbacks ---- */

static void plus_cb(lv_event_t *e)
{
    (void)e;
    uint32_t new_val = (uint32_t)s_values[s_current_idx] + (uint32_t)s_step;
    if (new_val > 65535) new_val = 65535;
    s_values[s_current_idx] = (uint16_t)new_val;
    update_display();
}

static void minus_cb(lv_event_t *e)
{
    (void)e;
    int new_val = (int)s_values[s_current_idx] - s_step;
    if (new_val < 1) new_val = 1;
    s_values[s_current_idx] = (uint16_t)new_val;
    update_display();
}

static void mult_point1_cb(lv_event_t *e)
{
    (void)e;
    s_step = 1;
    highlight_multiplier(s_step);
}

static void mult_one_cb(lv_event_t *e)
{
    (void)e;
    s_step = 10;
    highlight_multiplier(s_step);
}

static void mult_five_cb(lv_event_t *e)
{
    (void)e;
    s_step = 50;
    highlight_multiplier(s_step);
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    s_current_idx = (s_current_idx + SETTING_COUNT - 1) % SETTING_COUNT;
    update_display();
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    s_current_idx = (s_current_idx + 1) % SETTING_COUNT;
    update_display();
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, NVS_KEY_MIX_INT, s_values[SETTING_MIXER_INTERVAL]);
        nvs_set_u16(h, NVS_KEY_MIX_RT,  s_values[SETTING_MIXER_RUNTIME]);
        nvs_set_u16(h, NVS_KEY_DRY_T,   s_values[SETTING_DRYING_TIME]);
        nvs_commit(h);
        nvs_close(h);
    }
    /* Navigation is handled by the SquareLine-generated ui_event_btnSaveSetting */
}

static void screen_load_cb(lv_event_t *e)
{
    (void)e;
    /* Reset to first setting and default step on every screen entry */
    s_current_idx = 0;
    s_step = 10;
    highlight_multiplier(s_step);
    update_display();
}

/* ------------------------------------------------------------------ */

void app_settings_init(void)
{
    /* Load values from NVS; keep defaults when key is absent */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint16_t val;
        if (nvs_get_u16(h, NVS_KEY_MIX_INT, &val) == ESP_OK)
            s_values[SETTING_MIXER_INTERVAL] = val;
        if (nvs_get_u16(h, NVS_KEY_MIX_RT,  &val) == ESP_OK)
            s_values[SETTING_MIXER_RUNTIME]  = val;
        if (nvs_get_u16(h, NVS_KEY_DRY_T,   &val) == ESP_OK)
            s_values[SETTING_DRYING_TIME]    = val;
        nvs_close(h);
    }

    /* Register button callbacks */
    lv_obj_add_event_cb(uic_btnPlusSettings,       plus_cb,       LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(uic_btnMinusSettings,      minus_cb,      LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(uic_btnMultipliesPointOne, mult_point1_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(uic_btnMultiplierOne,      mult_one_cb,   LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(uic_btnMultiplierFive,     mult_five_cb,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(uic_btnPreviousSetting,    prev_cb,       LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(uic_btnNextSetting,        next_cb,       LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(uic_btnSaveSetting,        save_cb,       LV_EVENT_CLICKED, NULL);

    /* Refresh display whenever the settings screen is shown */
    lv_obj_add_event_cb(uic_srcSettings,           screen_load_cb, LV_EVENT_SCREEN_LOADED, NULL);

    /* Apply initial display state */
    highlight_multiplier(s_step);
    update_display();
}

float app_settings_get_mixer_interval_min(void)
{
    return s_values[SETTING_MIXER_INTERVAL] / 10.0f;
}

float app_settings_get_mixer_run_time_min(void)
{
    return s_values[SETTING_MIXER_RUNTIME] / 10.0f;
}

float app_settings_get_drying_time_min(void)
{
    return s_values[SETTING_DRYING_TIME] / 10.0f;
}
