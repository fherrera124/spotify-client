#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "esp_http_client.h"

/* Exported functions prototypes ---------------------------------------------*/
void default_http_event_handler(char* http_buffer, esp_http_client_event_t* evt);
void playlists_handler(char* http_buffer, esp_http_client_event_t* evt);

#ifdef __cplusplus
}
#endif