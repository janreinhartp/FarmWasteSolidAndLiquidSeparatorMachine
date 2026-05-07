#pragma once

#include <stdint.h>

/* Call once after ui_init() */
void  app_settings_init(void);

/* Getters – return values in minutes (float) for use by the process task */
float app_settings_get_mixer_interval_min(void);
float app_settings_get_mixer_run_time_min(void);
float app_settings_get_drying_time_min(void);
