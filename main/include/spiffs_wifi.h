#pragma once

/* Includes ------------------------------------------------------------------*/
#include "esp_wifi_types.h"

/* Exported macro ------------------------------------------------------------*/
#define CONFIG_FOUND 0
#define CONFIG_NOT_FOUND -1

/* Exported functions prototypes ---------------------------------------------*/
int wifi_config_read(wifi_config_t* wifi_config);
int wifi_config_write(wifi_config_t* wifi_config);
int wifi_config_delete();