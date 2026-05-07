#pragma once

/* Call once after ui_init() and app_machine_init() */
void app_process_init(void);

/* Called by emergency stop to abort the running process */
void app_process_stop(void);
