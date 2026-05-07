// main.c
#include "main.h"
#include "ui.h"
#include "nvs_flash.h"
#include "ui/screens/ui_scrSplashScreen.h"
#include "esp_lvgl_port.h"

/* LDO channel handle */
static esp_ldo_channel_handle_t ldo3 = NULL;
static esp_ldo_channel_handle_t ldo4 = NULL;

/* Forward declaration – defined after app_main */
static void splash_done_cb(lv_anim_t *a);
static void bar_anim_exec_cb(void *bar, int32_t val)
{
    lv_bar_set_value((lv_obj_t *)bar, val, LV_ANIM_OFF);
}

/**
 • @brief Initialization failure handler (repeatedly prints error information)

 */
static void init_fail_handler(const char *module_name, esp_err_t err) {
    while (1) {  // Infinite loop to help debug which module failed to initialize
        MAIN_ERROR("[%s] init failed: %s", module_name, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 • @brief System initialization (LDO + LCD + backlight + other hardware)

 */
static void system_init(void) {
    esp_err_t err = ESP_OK;

    // 1. Initialize LDO (required for screen)
    esp_ldo_channel_config_t ldo3_cof = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    err = esp_ldo_acquire_channel(&ldo3_cof, &ldo3);
    if (err != ESP_OK) init_fail_handler("ldo3", err);

    esp_ldo_channel_config_t ldo4_cof = {
        .chan_id = 4,
        .voltage_mv = 3300,
    };
    err = esp_ldo_acquire_channel(&ldo4_cof, &ldo4);
    if (err != ESP_OK) init_fail_handler("ldo4", err);
    MAIN_INFO("LDO3 and LDO4 init success");

    // 2. Initialize I2C (required for touch chip)
    MAIN_INFO("Initializing I2C...");
    err = i2c_init();
    if (err != ESP_OK) init_fail_handler("I2C", err);
    MAIN_INFO("I2C init success");

    // 3. Initialize touch panel (low-level driver)
    MAIN_INFO("Initializing touch panel...");
    err = touch_init();
    if (err != ESP_OK) init_fail_handler("Touch", err);
    MAIN_INFO("Touch panel init success");

    // 4. Initialize LCD hardware and LVGL (must initialize before turning on backlight)
    err = display_init();
    if (err != ESP_OK) init_fail_handler("LCD", err);
    MAIN_INFO("LCD init success");

    // 5. Turn on LCD backlight (brightness set to 100 = max)
    err = set_lcd_blight(100);
    if (err != ESP_OK) init_fail_handler("LCD Backlight", err);
    MAIN_INFO("LCD backlight opened (brightness: 100)");

    // 6. Initialize LED control GPIO (GPIO48)
    MAIN_INFO("Initializing GPIO48 for LED...");
    err = gpio_extra_init();
    if (err != ESP_OK) init_fail_handler("GPIO48", err);

    gpio_extra_set_level(false);  // Initially turn off LED
    MAIN_INFO("LED initialized to OFF state");

    // 7. Initialize PCF8575 relay/sensor expander
    MAIN_INFO("Initializing PCF8575 I/O expander...");
    err = pcf8575_init();
    if (err != ESP_OK) init_fail_handler("PCF8575", err);
    MAIN_INFO("PCF8575 init success");

    // 8. Initialize NVS flash (required for settings storage)
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) init_fail_handler("NVS", err);
    MAIN_INFO("NVS flash init success");
}

void app_main(void)
{
    MAIN_INFO("Starting LED control application...");

    // System initialization (including LDO, LCD, touch, LED and all hardware)
    system_init();
    MAIN_INFO("System initialized successfully");

    // Initialize UI components
    ui_init();
    MAIN_INFO("UI initialized successfully");

    /* All calls below touch LVGL internals — hold the lock for the entire
       init block so the LVGL port task cannot run concurrently. */
    lvgl_port_lock(portMAX_DELAY);

    // Wire relay buttons, indicator colours, and sensor poll timer
    app_machine_init();
    MAIN_INFO("Machine control layer initialized");

    // Initialize settings screen (loads NVS values, registers button callbacks)
    app_settings_init();
    MAIN_INFO("Settings screen initialized");

    // Register RunAuto screen callback (xTaskCreate inside is safe while holding the lock)
    app_process_init();
    MAIN_INFO("Process control task started");

    /* ---- Splash screen: animate loading bar 0 → 100 % then go to Main Menu ---- */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, uic_LoadingBar);
    lv_anim_set_exec_cb(&a, bar_anim_exec_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, 3000);           /* 3 seconds */
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, splash_done_cb);
    lv_anim_start(&a);

    lvgl_port_unlock();
}

/* Called by LVGL on the LVGL task when the bar animation finishes */
static void splash_done_cb(lv_anim_t *a)
{
    (void)a;
    _ui_screen_change(&ui_scrMainMenu, LV_SCR_LOAD_ANIM_FADE_OUT, 500, 0,
                      &ui_scrMainMenu_screen_init);
}
