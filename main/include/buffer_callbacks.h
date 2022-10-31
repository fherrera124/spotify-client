#pragma once

#include "esp_http_client.h"
#include "jsmn.h"
#include "strlib.h"

typedef struct {
    char*    name_list;
    StrList* uris;
} Playlists_t;

void default_fun(char* buffer, esp_http_client_event_t* evt);
void get_playlists(char* buffer, esp_http_client_event_t* evt);