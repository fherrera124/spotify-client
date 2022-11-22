#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "esp_http_client.h"

/* Exported functions prototypes ---------------------------------------------*/
void default_event_handler(char* buffer, esp_http_client_event_t* evt);
void playlists_handler(char* buffer, esp_http_client_event_t* evt);

#ifdef __cplusplus
}
#endif