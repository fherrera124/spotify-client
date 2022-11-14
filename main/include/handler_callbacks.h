#pragma once

#include "esp_http_client.h"
#include "jsmn.h"
#include "strlib.h"

typedef struct {
    char*    name_list;
    StrList* uris;
} Playlists_t;

void default_event_handler(char* buffer, esp_http_client_event_t* evt);
void playlists_handler(char* buffer, esp_http_client_event_t* evt);