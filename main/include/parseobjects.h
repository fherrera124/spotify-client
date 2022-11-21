#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdbool.h>
#include <time.h>

#include "strlib.h"

/* Exported types ------------------------------------------------------------*/

typedef struct
{
    char* id;
    bool  is_active;
    char* name;
    char* type;
    char* volume_percent;
} Device;

typedef struct
{
    char*    name;
    StrList* artists;
    char*    album;
    time_t   duration_ms;
    time_t   progress_ms;
    bool     isPlaying : 1;
    Device*  device;
} TrackInfo;

typedef struct
{
    // char*  refreshToken;
    // char*  authToken;
    char*  access_token;
    time_t expiresIn;
} Tokens;

typedef struct {
    char*    items_string;
    StrList* values;
} u8g2_items_list_t;

/* Globally scoped variables declarations ------------------------------------*/
extern u8g2_items_list_t* PLAYLISTS;
extern u8g2_items_list_t* DEVICES;

/* Exported functions prototypes ---------------------------------------------*/
void      init_functions_cb(void);
void      parseTrackInfo(const char* js, TrackInfo* track);
void      parseTokens(const char* js, Tokens* tokens);
void      display_parse_playlist(const char* js, int output_len);
esp_err_t parse_available_devices(const char* js);

#ifdef __cplusplus
}
#endif