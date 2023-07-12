/* Includes ------------------------------------------------------------------*/
#include "spiffs_wifi.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"

#include <stdio.h>
#include <sys/unistd.h>

/* Private function prototypes -----------------------------------------------*/
int static inline spiffs_init();

/* Private variables ---------------------------------------------------------*/
static const char* TAG = "spiffs_wifi";

/* Exported functions --------------------------------------------------------*/
int wifi_config_read(wifi_config_t* wifi_config)
{
    ESP_LOGD(TAG, "Initializing SPIFFS for read");

    assert(spiffs_init() == ESP_OK);

    // Use POSIX and C standard library functions to work with files.
    // First create a file.
    ESP_LOGD(TAG, "Opening file");
    FILE* f = fopen("/spiffs/wifi_config.txt", "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        esp_vfs_spiffs_unregister(NULL);
        return CONFIG_NOT_FOUND;
    }
    fread(wifi_config, sizeof(wifi_config_t), 1, f);
    fclose(f);

    // All done, unmount partition and disable SPIFFS
    esp_vfs_spiffs_unregister(NULL);
    return CONFIG_FOUND;
}

int wifi_config_write(wifi_config_t* wifi_config)
{
    ESP_LOGD(TAG, "Initializing SPIFFS for write");

    assert(spiffs_init() == ESP_OK);

    ESP_LOGD(TAG, "Opening file");
    FILE* f = fopen("/spiffs/wifi_config.txt", "w");

    assert(f && "Failed to open/create file for writing");

    fwrite(wifi_config, sizeof(wifi_config_t), 1, f);
    fclose(f);

    esp_vfs_spiffs_unregister(NULL);
    return ESP_OK;
}

int wifi_config_delete()
{
    assert(spiffs_init() == ESP_OK);

    int res = unlink("/spiffs/wifi_config.txt");
    if (res == ESP_OK) {
        ESP_LOGD(TAG, "File deleted successfully");
    } else {
        ESP_LOGW(TAG, "Error: unable to delete the file");
    }
    esp_vfs_spiffs_unregister(NULL);
    return res;
}

int static inline spiffs_init()
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 1,
        .format_if_mount_failed = true
    };

    // Use settings defined above to initialize and mount SPIFFS filesystem.
    // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}