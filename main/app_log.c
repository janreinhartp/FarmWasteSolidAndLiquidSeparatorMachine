#include "app_log.h"
#include <stdio.h>
#include "esp_spiffs.h"
#include "esp_log.h"

#define APP_LOG_TAG       "APP_LOG"
#define LOG_MOUNT_POINT   "/spiffs"
#define LOG_FILE_PATH     LOG_MOUNT_POINT "/machine.log"
#define LOG_FILE_OLD      LOG_MOUNT_POINT "/machine.log.old"
#define LOG_ROTATE_BYTES  (256 * 1024)   /* partition is 2 MB; leaves room for .old copy */

static vprintf_like_t s_orig_vprintf = NULL;
static FILE          *s_log_fp       = NULL;
static size_t          s_log_bytes   = 0;

/* Installed via esp_log_set_vprintf(): mirrors every log line to flash. */
static int log_vprintf(const char *fmt, va_list args)
{
    int ret = s_orig_vprintf(fmt, args);

    if (s_log_fp) {
        va_list args_copy;
        va_copy(args_copy, args);
        int written = vfprintf(s_log_fp, fmt, args_copy);
        va_end(args_copy);

        if (written > 0) {
            s_log_bytes += (size_t)written;
            fflush(s_log_fp);   /* durability: survive a crash right after this line */
            if (s_log_bytes >= LOG_ROTATE_BYTES) {
                fclose(s_log_fp);
                remove(LOG_FILE_OLD);
                rename(LOG_FILE_PATH, LOG_FILE_OLD);
                s_log_fp = fopen(LOG_FILE_PATH, "w");
                s_log_bytes = 0;
            }
        }
    }
    return ret;
}

/* Surface last boot's log now, since nobody may have been watching when it happened. */
static void replay_previous_log(void)
{
    FILE *fp = fopen(LOG_FILE_PATH, "r");
    if (!fp) return;

    printf("\n===== PREVIOUS BOOT LOG (%s) =====\n", LOG_FILE_PATH);
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        fwrite(buf, 1, n, stdout);
    }
    printf("===== END PREVIOUS BOOT LOG =====\n\n");
    fclose(fp);
}

esp_err_t app_log_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = LOG_MOUNT_POINT,
        .partition_label        = "storage",
        .max_files               = 3,
        .format_if_mount_failed  = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(APP_LOG_TAG, "SPIFFS mount failed: %s — flash logging disabled", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(conf.partition_label, &total, &used);
    ESP_LOGI(APP_LOG_TAG, "SPIFFS mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);

    replay_previous_log();

    s_log_fp = fopen(LOG_FILE_PATH, "w");   /* fresh file for this boot session */
    if (!s_log_fp) {
        ESP_LOGE(APP_LOG_TAG, "Could not open %s for writing", LOG_FILE_PATH);
        return ESP_FAIL;
    }
    s_log_bytes = 0;

    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
    return ESP_OK;
}
